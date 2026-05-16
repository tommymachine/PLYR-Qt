// AudioClock — implementation. Compiled only on macOS via CMake.
//
// What this does, end to end:
//
//   1. Walks the AudioComponent registry to find the AUHAL output unit Qt's
//      QAudioSink already created and started. There's exactly one such unit
//      alive in this process at the time `attach()` runs (QAudioSink::start
//      has run; no other code creates AUHAL outputs), so the first matching
//      component-instance is unambiguously Qt's.
//
//   2. Queries that unit for its currently bound output device, then sums
//      kAudioDevicePropertyLatency + kAudioDevicePropertySafetyOffset (in
//      frames) and kAudioStreamPropertyLatency on every output stream of
//      that device. Divided by sampleRate, this is the total speaker-emit
//      latency we have to subtract when projecting "audio that will be
//      heard at display time" back to a sample index.
//
//   3. Registers an AURenderNotify callback. The kAudioUnitRenderAction_PreRender
//      flag fires BEFORE Qt's own render callback pulls samples from the pipe;
//      that timestamp is what we want — it describes the buffer about to be
//      generated, not one already in flight. (Post-render shares the same
//      timestamp but runs after Qt has stuffed audio into AudioUnitRender's
//      buffer; the relative offset is irrelevant for our math but pre-render
//      gives us a hair more headroom on the read side.)
//
//   4. Publishes the captured (hostTime, sampleTime, sampleRate, latency) via
//      a seqlock so the audio-thread reader can grab a consistent snapshot
//      without locks or allocations.

#include "AudioClock_macOS.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/AudioHardware.h>
#include <Foundation/Foundation.h>
#include <mach/mach_time.h>

#include <atomic>
#include <vector>

namespace plyr::sync {

namespace {

// Cached mach_timebase ratio. Populated on first use; threadsafe via
// std::call_once / static-local.
double machRatio()
{
    static const double r = []() {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        // ratio of nanoseconds per host tick
        return double(info.numer) / double(info.denom);
    }();
    return r;
}

} // namespace

double AudioClock::hostToSeconds(uint64_t hostTime)
{
    // hostTime ticks are mach-continuous-time units. Multiply by numer/denom
    // → nanoseconds, then × 1e-9 → seconds. CACurrentMediaTime returns the
    // same base (seconds since boot), so display-link presentation timestamps
    // and our converted audio anchors share a single clock domain.
    return double(hostTime) * machRatio() * 1.0e-9;
}

double AudioClock::nowSeconds()
{
    // mach_continuous_time, not mach_absolute_time: it keeps counting across
    // sleep, which CACurrentMediaTime also does. Without this matched basis
    // the audio + display clocks would drift by however long the laptop slept.
    return hostToSeconds(mach_continuous_time());
}

// -------- PIMPL ----------------------------------------------------------

struct AudioClock::Impl {
    AudioUnit       m_auhal           = nullptr;
    AudioDeviceID   m_device          = kAudioObjectUnknown;
    double          m_outputLatencySec = 0.0;
    double          m_sampleRate       = 44100.0;
    bool            m_attached         = false;
    bool            m_isBluetooth      = false;

    // Seqlock state. The write side (RT callback) increments m_seq to odd,
    // writes the fields, increments again to even. Readers spin until they
    // observe the same even value before and after the field reads.
    //
    // m_published copies the four anchor fields. They're plain doubles +
    // uint64s; not std::atomic individually because the seqlock guards the
    // whole block. The release-on-end-of-write / acquire-on-start-of-read
    // fences come from m_seq.fetch_add with the appropriate memory order.
    std::atomic<uint64_t> m_seq{0};
    uint64_t              m_hostTime    = 0;
    double                m_sampleTime  = 0.0;
    double                m_sampleRate2 = 0.0;
    double                m_latency2    = 0.0;
    uint64_t              m_bufferIdx   = 0;  // monotonic per-buffer counter

    // ---------- AUHAL discovery ----------
    //
    // AudioComponentFindNext / AudioComponent is descriptor-level: it
    // returns component definitions, not live instances. There is no public
    // API to enumerate or grab live AudioUnit instances created by other
    // code in the same process — Qt's AUHAL is opaque to us.
    //
    // The canonical solution for observing the output clock without
    // touching another framework's AU pipeline is to stand up our OWN
    // AUHAL bound to the SAME output device. Since both units sit atop
    // the same CoreAudio device, they share its host-time clock exactly,
    // and our render-notify callback receives the same mHostTime /
    // mSampleTime timeline Qt's AU is firing against. We provide a
    // silent render callback so the device doesn't actually mix anything
    // additional onto the bus.
    static AudioUnit findOutputAUHAL()
    {
        AudioComponentDescription desc{};
        desc.componentType         = kAudioUnitType_Output;
        desc.componentSubType      = kAudioUnitSubType_HALOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) return nullptr;
        AudioUnit au = nullptr;
        OSStatus err = AudioComponentInstanceNew(comp, &au);
        if (err != noErr || !au) return nullptr;
        return au;
    }

    // Bind the AU to the system default output device, then start it. We
    // don't need its samples (NULL render callback returns silence into
    // a buffer we never read), only its timestamps.
    bool bindAndStart()
    {
        // Find default output device.
        AudioObjectPropertyAddress addr{
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioDeviceID dev = kAudioObjectUnknown;
        UInt32 sz = sizeof(dev);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                       0, nullptr, &sz, &dev) != noErr) {
            return false;
        }
        m_device = dev;
        if (AudioUnitSetProperty(
                m_auhal, kAudioOutputUnitProperty_CurrentDevice,
                kAudioUnitScope_Global, 0, &dev, sizeof(dev)) != noErr) {
            return false;
        }

        // Match its nominal sample rate so the AU's timeline aligns with
        // the device's.
        Float64 sr = 0.0;
        sz = sizeof(sr);
        AudioObjectPropertyAddress srAddr{
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(dev, &srAddr, 0, nullptr, &sz, &sr) == noErr
            && sr > 0.0) {
            m_sampleRate = sr;
        }

        // Provide a silent render callback so AudioUnitInitialize succeeds.
        // The clock observer doesn't care about the actual samples — only
        // the timestamps that arrive on the render-notify hook below.
        AURenderCallbackStruct cb{};
        cb.inputProc       = &Impl::silentRender;
        cb.inputProcRefCon = this;
        if (AudioUnitSetProperty(
                m_auhal, kAudioUnitProperty_SetRenderCallback,
                kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr) {
            return false;
        }

        if (AudioUnitInitialize(m_auhal) != noErr) return false;
        if (AudioOutputUnitStart(m_auhal) != noErr) {
            AudioUnitUninitialize(m_auhal);
            return false;
        }
        return true;
    }

    // Silent render — fill with zeros. Allocation-free; no logging.
    static OSStatus silentRender(void*                       inRefCon,
                                 AudioUnitRenderActionFlags* ioActionFlags,
                                 const AudioTimeStamp*       /*inTimeStamp*/,
                                 UInt32                      /*inBusNumber*/,
                                 UInt32                      inNumberFrames,
                                 AudioBufferList*            ioData)
    {
        (void)inRefCon;
        if (ioData) {
            for (UInt32 i = 0; i < ioData->mNumberBuffers; ++i) {
                if (ioData->mBuffers[i].mData) {
                    memset(ioData->mBuffers[i].mData, 0,
                           ioData->mBuffers[i].mDataByteSize);
                }
            }
        }
        if (ioActionFlags) {
            *ioActionFlags |= kAudioUnitRenderAction_OutputIsSilence;
        }
        // We use the silent buffer as a "tick source" — but we still need
        // a host time anchor that matches the real Qt-driven AU. Since the
        // two AUs are bound to the SAME output device, their CoreAudio
        // host clocks are identical, so the render-notify callback we
        // attach below reads the correct shared timeline.
        (void)inNumberFrames;
        return noErr;
    }

    // The render-notify callback: this fires twice per buffer (pre and
    // post). We act only on pre-render — the timestamp is the same as
    // post-render but pre-render runs before Qt's own callback chain has
    // consumed any of the buffer, giving the reader the most headroom.
    static OSStatus renderNotifyCb(void*                       inRefCon,
                                   AudioUnitRenderActionFlags* ioActionFlags,
                                   const AudioTimeStamp*       inTimeStamp,
                                   UInt32                      /*inBusNumber*/,
                                   UInt32                      /*inNumberFrames*/,
                                   AudioBufferList*            /*ioData*/)
    {
        if (!inRefCon || !ioActionFlags || !inTimeStamp) return noErr;
        if (!(*ioActionFlags & kAudioUnitRenderAction_PreRender)) return noErr;

        // Validate that the timestamp has both fields we need. Without
        // either we'd corrupt the anchor.
        const bool haveHost   = (inTimeStamp->mFlags & kAudioTimeStampHostTimeValid)   != 0;
        const bool haveSample = (inTimeStamp->mFlags & kAudioTimeStampSampleTimeValid) != 0;
        if (!haveHost || !haveSample) return noErr;

        auto* self = static_cast<Impl*>(inRefCon);

        // Seqlock write: bump to odd, write fields, bump to even. Memory
        // order is release on the second bump and acquire on the first so
        // a reader cannot see partially written fields.
        const uint64_t v0 = self->m_seq.load(std::memory_order_relaxed);
        self->m_seq.store(v0 + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        self->m_hostTime    = inTimeStamp->mHostTime;
        self->m_sampleTime  = inTimeStamp->mSampleTime;
        self->m_sampleRate2 = self->m_sampleRate;
        self->m_latency2    = self->m_outputLatencySec;
        self->m_bufferIdx  += 1;

        std::atomic_thread_fence(std::memory_order_release);
        self->m_seq.store(v0 + 2, std::memory_order_release);
        return noErr;
    }

    // Sum the per-stream + per-device latency for the output device. All
    // values are in frames at the device's nominal rate, so we convert
    // to seconds once at the end. Re-run this on device-changed events.
    void recomputeLatency()
    {
        if (m_device == kAudioObjectUnknown) {
            m_outputLatencySec = 0.0;
            return;
        }

        UInt32 deviceFrames = 0;
        UInt32 safetyFrames = 0;
        UInt32 sz = sizeof(UInt32);

        AudioObjectPropertyAddress addrLat{
            kAudioDevicePropertyLatency,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(m_device, &addrLat, 0, nullptr,
                                   &sz, &deviceFrames);

        sz = sizeof(UInt32);
        AudioObjectPropertyAddress addrSafety{
            kAudioDevicePropertySafetyOffset,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(m_device, &addrSafety, 0, nullptr,
                                   &sz, &safetyFrames);

        // Enumerate output streams; sum kAudioStreamPropertyLatency over
        // each. Typically just one stream on stereo output.
        UInt32 streamFramesTotal = 0;
        AudioObjectPropertyAddress addrStreams{
            kAudioDevicePropertyStreams,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 streamsBytes = 0;
        if (AudioObjectGetPropertyDataSize(m_device, &addrStreams, 0,
                                           nullptr, &streamsBytes) == noErr) {
            const int nStreams = int(streamsBytes / sizeof(AudioStreamID));
            std::vector<AudioStreamID> ids;
            ids.resize(size_t(nStreams));
            if (nStreams > 0 &&
                AudioObjectGetPropertyData(m_device, &addrStreams, 0,
                                           nullptr, &streamsBytes,
                                           ids.data()) == noErr) {
                for (AudioStreamID sid : ids) {
                    AudioObjectPropertyAddress addrSL{
                        kAudioStreamPropertyLatency,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    UInt32 sl = 0;
                    UInt32 sl_sz = sizeof(sl);
                    if (AudioObjectGetPropertyData(sid, &addrSL, 0, nullptr,
                                                   &sl_sz, &sl) == noErr) {
                        streamFramesTotal += sl;
                    }
                }
            }
        }

        const UInt32 totalFrames = deviceFrames + safetyFrames + streamFramesTotal;
        const double sr = (m_sampleRate > 0.0) ? m_sampleRate : 44100.0;
        m_outputLatencySec = double(totalFrames) / sr;

        // Surface what we found, once per attach, for diagnostics.
        NSLog(@"[AudioClock] latency: device=%u safety=%u stream=%u total=%u frames (%.2f ms) @ %.0f Hz",
              (unsigned)deviceFrames, (unsigned)safetyFrames,
              (unsigned)streamFramesTotal, (unsigned)totalFrames,
              m_outputLatencySec * 1000.0, m_sampleRate);
    }

    void detect_bluetooth()
    {
        if (m_device == kAudioObjectUnknown) {
            m_isBluetooth = false;
            return;
        }
        UInt32 transport = 0;
        UInt32 sz = sizeof(transport);
        AudioObjectPropertyAddress addr{
            kAudioDevicePropertyTransportType,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(m_device, &addr, 0, nullptr,
                                       &sz, &transport) == noErr) {
            m_isBluetooth = (transport == kAudioDeviceTransportTypeBluetooth);
        }

        // Best-effort device name for the diagnostic log.
        CFStringRef name = nullptr;
        sz = sizeof(name);
        AudioObjectPropertyAddress nameAddr{
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(m_device, &nameAddr, 0, nullptr,
                                       &sz, &name) == noErr && name) {
            NSLog(@"[AudioClock] output device: %@ (transport=%c%c%c%c %s)",
                  (__bridge NSString*)name,
                  (transport >> 24) & 0xff, (transport >> 16) & 0xff,
                  (transport >>  8) & 0xff, transport & 0xff,
                  m_isBluetooth ? "BLUETOOTH" : "");
            CFRelease(name);
        }
    }
};

AudioClock::AudioClock()  : m_impl(std::make_unique<Impl>()) {}
AudioClock::~AudioClock() { detach(); }

bool AudioClock::attach()
{
    if (m_impl->m_attached) return true;

    m_impl->m_auhal = Impl::findOutputAUHAL();
    if (!m_impl->m_auhal) {
        NSLog(@"[AudioClock] failed to find AUHAL output unit");
        return false;
    }

    if (!m_impl->bindAndStart()) {
        NSLog(@"[AudioClock] failed to bind/start probe AUHAL");
        AudioComponentInstanceDispose(m_impl->m_auhal);
        m_impl->m_auhal = nullptr;
        return false;
    }

    m_impl->detect_bluetooth();
    m_impl->recomputeLatency();

    OSStatus err = AudioUnitAddRenderNotify(
        m_impl->m_auhal, &Impl::renderNotifyCb, m_impl.get());
    if (err != noErr) {
        NSLog(@"[AudioClock] AudioUnitAddRenderNotify failed: %d", int(err));
        AudioOutputUnitStop(m_impl->m_auhal);
        AudioUnitUninitialize(m_impl->m_auhal);
        AudioComponentInstanceDispose(m_impl->m_auhal);
        m_impl->m_auhal = nullptr;
        return false;
    }

    m_impl->m_attached = true;
    NSLog(@"[AudioClock] attached: AU=%p device=%u sr=%.0f Hz latency=%.2f ms",
          (void*)m_impl->m_auhal, (unsigned)m_impl->m_device,
          m_impl->m_sampleRate, m_impl->m_outputLatencySec * 1000.0);
    return true;
}

void AudioClock::detach()
{
    if (!m_impl || !m_impl->m_attached) return;
    if (m_impl->m_auhal) {
        AudioUnitRemoveRenderNotify(m_impl->m_auhal,
                                    &Impl::renderNotifyCb, m_impl.get());
        AudioOutputUnitStop(m_impl->m_auhal);
        AudioUnitUninitialize(m_impl->m_auhal);
        AudioComponentInstanceDispose(m_impl->m_auhal);
        m_impl->m_auhal = nullptr;
    }
    m_impl->m_attached = false;
}

std::optional<AudioAnchor> AudioClock::load() const
{
    if (!m_impl || !m_impl->m_attached) return std::nullopt;

    // Seqlock read: spin until we observe an even sequence number that
    // bracketed the field reads consistently. Bound the spin (the writer
    // runs at audio-buffer cadence, ~10 ms; one retry is more than enough)
    // so we never busy-wait forever.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t v0 = m_impl->m_seq.load(std::memory_order_acquire);
        if (v0 == 0) return std::nullopt;     // never written
        if (v0 & 1u) {                         // currently being written
            // tiny pause; CPU will reorder around it
            continue;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        AudioAnchor a;
        a.version          = m_impl->m_bufferIdx;
        a.hostTime         = m_impl->m_hostTime;
        a.sampleTime       = m_impl->m_sampleTime;
        a.sampleRate       = m_impl->m_sampleRate2;
        a.outputLatencySec = m_impl->m_latency2;
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t v1 = m_impl->m_seq.load(std::memory_order_acquire);
        if (v1 == v0) return a;
    }
    return std::nullopt;
}

bool AudioClock::isBluetooth() const
{
    return m_impl ? m_impl->m_isBluetooth : false;
}

} // namespace plyr::sync
