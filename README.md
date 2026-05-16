# Concerto

Cross-platform port of [PLYR](https://github.com/tommymachine/PLYR), written
in C++/QML with Qt 6. Targets macOS, Windows, Linux, iOS, and Android from a
single codebase.

## Stack

- **UI**: QML + Qt Quick
- **Audio engine**: FFmpeg (libavformat/libavcodec) for decode + PortAudio or
  Qt Multimedia for output; gapless-capable scheduler in C++
- **FLAC metadata**: in-house VORBIS_COMMENT / STREAMINFO parser (port of the
  Swift original)
- **FFT**: KissFFT (permissive license)
- **MilkDrop preset runtime**: projectm-eval (MIT) — a clean-room
  reimplementation of NullSoft's NS-EEL2 expression compiler. Used by
  Layer 3b's `MilkdropRuntime` / `MilkdropView` to evaluate per-frame /
  per-vertex preset scripts. Vendored at `third_party/projectm-eval/`,
  LICENSE.md retained alongside the source. Note: this is the MIT
  expression-only library — we deliberately do NOT vendor `projectm`
  itself (the surrounding LGPL renderer); our renderer is original.
- **Visualizer**: Qt RHI — shaders authored in GLSL, baked to `.qsb`,
  rendered via `QQuickItem` + `QSGMaterialShader`. Qt translates at runtime
  to Metal (Apple), Vulkan (Android/Linux/Windows), or D3D12 (Windows).
- **Build system**: CMake

## Building (macOS)

```sh
cmake -B build -G Ninja
cmake --build build
open ./build/Concerto.app
```

Requires:
- Qt 6 (`brew install qt`)
- CMake (`brew install cmake`)
- Ninja (`brew install ninja`) — optional, faster incremental builds

## Licensing

LGPL v3 (Qt) dynamic linking. See the LICENSE file for Concerto's own
license and the `THIRDPARTY` notes for Qt / FFmpeg / KissFFT attribution.
