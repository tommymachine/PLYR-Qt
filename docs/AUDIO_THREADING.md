# Qt 6 / Core Audio threading on macOS — research report

Prepared during the investigation of "audio skips when the Open Folder dialog
opens." See the chat transcript for the full report; this file records the
conclusions and recommended mitigations.

## TL;DR

Counter-intuitive finding: **`PcmPipe::readData`, the EQ, and the FFT tap all
run on the main application thread, NOT on the Core Audio real-time thread.**

Qt 6's modern `QDarwinAudioSink` architecture is:

```
Core Audio HAL IOProc thread  (THREAD_TIME_CONSTRAINT_POLICY)
  └── QPlatformAudioSinkStream::process()      [QT_MM_NONBLOCKING]
        pops PCM from an internal ring buffer, no user code here

Application thread (main thread for us)
  └── pullFromQIODeviceImpl()
        device.read(...)  →  PcmPipe::readData
                               → eq_process (in place)
                               → emit samplesServed  →  FftProcessor (Direct)
```

The RT thread never calls our code. Main thread refills Qt's internal 250 ms
ring buffer on demand. When main thread stalls for >250 ms (e.g. cold-opening
NSOpenPanel on macOS), the ring buffer drains, the RT thread outputs silence,
we hear a skip.

## Why NSOpenPanel glitches audio on a cold open

First invocation of `QML FolderDialog.open()` on macOS synchronously:
- dyld-loads additional AppKit/QuickLook/CoreServices code paths
- Establishes XPC connection to `com.apple.appkit.xpc.openAndSavePanelService`
  (if the service isn't already running, we pay process-launch cost)
- Launch Services + IconServicesAgent queries for file-type icons
- Potentially Launch-Services trust/certificate callbacks

Cumulative: 100–500 ms of main-thread stall is typical for a cold open.
Subsequent opens are cached and much faster.

## Recommended fix sequence (cheapest first)

1. **Pre-warm NSOpenPanel at startup** — a single `[NSOpenPanel openPanel]` call
   during app init, discarded immediately. Forces XPC + dyld warmup before any
   user-visible dialog open. Single-line fix in a `.mm` file. Expected to
   eliminate the glitch entirely.

2. **Increase QAudioSink buffer to 500 ms** via `setBufferSize()` BEFORE
   `start()`. Note: `setBufferSize` measures bytes, not frames or µs. Trade-off
   is play/pause latency, but 500 ms is fine for a music player.

3. **Move the audio chain to a dedicated QThread.** If (1)+(2) aren't enough,
   this is the architecturally clean answer — a thread whose only job is to
   refill Qt's ring buffer, never blocked by main-thread UI work.

## What NOT to do

- Don't `pthread_setschedparam` / `thread_policy_set` / `os_workgroup_join` our
  own threads. Our code isn't on the RT thread; Qt handles that correctly.
- Don't remove QMutex from PcmPipe. The contention is app-thread-only and
  cheap. If we move to (3), could switch to lock-free SPSC for cleanliness.
- Don't rely on Qt::DirectConnection crossing a thread boundary we haven't
  explicitly designed for.

## Unrelated hygiene note

`QByteArray::append` in `PcmPipe::append` causes many reallocations while
40–110 MB FLACs are being buffered. Call `m_buf.reserve(expectedBytes)` once
the decoder reports duration. Not the cause of the skip; just good practice.

## Key sources

- [Qt source: qdarwinaudiosink.mm (modernized)](https://code.qt.io/cgit/qt/qtmultimedia.git/plain/src/multimedia/darwin/qdarwinaudiosink.mm?id=ece4344dd8237ddeb7f5ba17f00d38b1bc83d4ff)
- [Apple WWDC20 session 10224: Meet Audio Workgroups](https://developer.apple.com/videos/play/wwdc2020/10224/)
- [Apple QA1467: CoreAudio Overload Warnings](https://developer.apple.com/library/archive/qa/qa1467/_index.html)
- [Ross Bencina: Real-time audio programming 101](http://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing)
- [A Tasty Pixel: Four common mistakes in audio development](https://atastypixel.com/four-common-mistakes-in-audio-development/)
- [Mike Ash: Why CoreAudio is Hard](https://www.mikeash.com/pyblog/why-coreaudio-is-hard.html)
- [Apple Developer Forums: openAndSavePanelService cold-start issues](https://developer.apple.com/forums/thread/742941)
- [Qt docs: QAudioSink](https://doc.qt.io/qt-6/qaudiosink.html)
