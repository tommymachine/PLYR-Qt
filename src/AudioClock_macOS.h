// AudioClock — Mac-only CoreAudio output-clock shim.
//
// Qt's CoreAudio backend (QCoreAudioSinkStream) registers an AURenderCallback
// against the AUHAL output unit but discards the AudioTimeStamp argument. No
// consumer of QAudioSink (pull-mode QIODevice or push-mode AudioCallback) ever
// sees mHostTime / mSampleTime. We sidestep Qt by attaching an additive
// `AudioUnitAddRenderNotify` to the same AUHAL — additive in the literal sense
// that Apple's API does not replace Qt's render callback, so Qt's audio flow
// is undisturbed.
//
// On every render cycle we capture the timestamp + sample count, then publish
// it through a seqlock so any reader thread can grab the latest atomic
// snapshot without locking. The audio-thread lookahead tap reads these to
// compute the exact playhead-in-samples at any wall-clock moment.
//
// The header is pure C++ (atomics + std), buildable on any platform; CMake
// only compiles the matching `.mm` on `APPLE AND NOT IOS`.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace plyr::sync {

// Snapshot of the audio output clock at the moment of the latest
// AURenderCallback that fired. All fields are valid only if version > 0.
//
// Threading: written by the AURenderNotify callback on CoreAudio's RT thread.
// Read by any other thread via AudioClock::load(), which uses a seqlock — no
// locks, no allocations on either side.
struct AudioAnchor {
    uint64_t version          = 0;    // monotonically increasing per buffer
    uint64_t hostTime         = 0;    // mach_continuous_time units of first
                                      // sample in the buffer
    double   sampleTime       = 0.0;  // running sample count at hostTime
    double   sampleRate       = 0.0;  // hardware sample rate in Hz
    double   outputLatencySec = 0.0;  // device + safety + stream latency
};

class AudioClock {
public:
    AudioClock();
    ~AudioClock();
    AudioClock(const AudioClock&)            = delete;
    AudioClock& operator=(const AudioClock&) = delete;

    // Attach to the process's AUHAL output unit and start capturing
    // timestamps. Idempotent. Returns true on success.
    bool attach();

    // Detach (called from the destructor; exposed for tests / replug).
    void detach();

    // Read the latest anchor. std::nullopt until the first render-notify
    // callback has fired.
    std::optional<AudioAnchor> load() const;

    // Did the underlying output device report a Bluetooth transport at
    // attach() time? Used to widen the user-facing calibration range.
    bool isBluetooth() const;

    // Convert mach_continuous_time host-time units to seconds. We use
    // mach_continuous_time (not mach_absolute_time) so the clock keeps
    // ticking across sleep/wake transitions, matching CACurrentMediaTime
    // and therefore the display link's targetPresentationTimestamp.
    static double hostToSeconds(uint64_t hostTime);

    // Wall-clock "now" in the same seconds-since-boot base as
    // hostToSeconds() and CACurrentMediaTime(). Safe from any thread.
    static double nowSeconds();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace plyr::sync
