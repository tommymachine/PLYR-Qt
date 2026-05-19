cmake_minimum_required(VERSION 3.21)
# OBJC + OBJCXX are needed for the macOS .mm files (audio/display clock
# shims, NSOpenPanel prewarm, MusicBlocker observer). On non-Apple builds
# CMake silently ignores OBJC* — they're harmless to list everywhere.
project(concerto VERSION 0.1 LANGUAGES CXX C OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Default to Release for single-config generators if no build type was set.
# Multi-config generators (Xcode / VS) ignore CMAKE_BUILD_TYPE and pick the
# config at build time, so we only set it when it's currently empty.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

# Point CMake at Homebrew's Qt if it's not already in the prefix path.
if(NOT DEFINED CMAKE_PREFIX_PATH AND APPLE)
    if(EXISTS "/opt/homebrew/opt/qt")
        list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt")
    elseif(EXISTS "/usr/local/opt/qt")
        list(APPEND CMAKE_PREFIX_PATH "/usr/local/opt/qt")
    endif()
endif()

# QQuickRhiItem landed in Qt 6.7; bump the floor for the scope visualiser.
find_package(Qt6 6.7 REQUIRED COMPONENTS
    Core
    Gui
    GuiPrivate    # QRhi / QShader for the analytic-line-integral scope.
    Network
    Qml
    Quick
    QuickControls2
    Multimedia
    ShaderTools
    Sql           # SQLite-backed library DB (concerto::library::LibraryDatabase).
)

# TagLib (Homebrew, LGPL-2.1) — multi-format tag I/O for the folder-time
# library pipeline. **Dynamic link only.** The LGPL-2.1 closed-source
# pattern is the same shape we already use for libFLAC: ship libtag.dylib
# in Contents/Frameworks/, @rpath resolution lets the user substitute a
# rebuilt copy. Never statically link TagLib.
#
# CMake config landed in TagLib 1.12+; Homebrew installs taglib-config.cmake
# under <prefix>/lib/cmake/taglib/ which find_package picks up directly.
# We export TAGLIB_FOUND so optional builds (e.g. the iOS configuration
# that doesn't yet pull this in) can skip the library targets.
find_package(taglib CONFIG REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

add_library(kissfft STATIC
    third_party/kissfft/kiss_fft.c
    third_party/kissfft/kiss_fftr.c
)
target_include_directories(kissfft PUBLIC third_party/kissfft)
set_target_properties(kissfft PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)

# Ripser (Ulrich Bauer, MIT) -- single-source-file C++ implementation
# of Vietoris-Rips persistent homology with the cohomology + clearing
# optimization. Vendored fresh from https://github.com/Ripser/ripser at
# third_party/ripser/. Three compile-time switches turn the CLI source
# into a linkable library:
#   * RIPSER_NO_MAIN          -- fence main() + file I/O drivers.
#   * RIPSER_USE_OUTPUT_HOOK  -- replace stdout barcode prints with
#                                calls to extern ripser_emit_pair /
#                                ripser_begin_dim (defined in
#                                src/PersistentHomologyAnalyzer.cpp).
#   * RIPSER_CONCERTO_TRAMPOLINE -- expose a plain extern-"C" entry
#                                point `phRunRipser(distances, n,
#                                threshold, dimMax)` so callers don't
#                                need to #include the 1300-line
#                                ripser.cpp directly.
# See third_party/ripser/CONCERTO.patch for the patch summary and
# rationale. License hygiene: third_party/ripser/COPYING.txt is
# preserved at the source-tree level.
add_library(ripser STATIC third_party/ripser/ripser.cpp)
target_compile_definitions(ripser PUBLIC
    RIPSER_NO_MAIN
    RIPSER_USE_OUTPUT_HOOK
    RIPSER_CONCERTO_TRAMPOLINE
)
target_include_directories(ripser PUBLIC third_party/ripser)
target_compile_options(ripser PRIVATE -O3)
# Ripser uses some signed/unsigned compares and unused private members
# that don't matter to its correctness; silence those for the vendored
# subtree so our build doesn't warn-spew. The trampoline + analyzer
# code that drives it is held to the project's default warning level.
target_compile_options(ripser PRIVATE
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-unused-private-field
)
set_target_properties(ripser PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)

# projectm-eval — the MIT-licensed reimplementation of NullSoft's
# NS-EEL2 expression compiler, used by Layer 3b's MilkdropRuntime.
# Vendored at third_party/projectm-eval/ (cloned from
# https://github.com/projectM-visualizer/projectm-eval). Builds a single
# static library `projectM::Eval` (~80 KB, no Qt dependency). The
# Compiler.c / Scanner.c files are prebuilt from .y / .l so we don't
# require a modern Bison on the build machine. Suppress Qt's AUTOMOC /
# AUTORCC / AUTOUIC for the subtree — pure C, no Qt.
set(_saved_automoc_pme ${CMAKE_AUTOMOC})
set(_saved_autorcc_pme ${CMAKE_AUTORCC})
set(_saved_autouic_pme ${CMAKE_AUTOUIC})
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)
add_subdirectory(third_party/projectm-eval EXCLUDE_FROM_ALL)
set(CMAKE_AUTOMOC ${_saved_automoc_pme})
set(CMAKE_AUTORCC ${_saved_autorcc_pme})
set(CMAKE_AUTOUIC ${_saved_autouic_pme})

# AccurateRip / CD-rip verification core. Pure computation — TOC
# reconstruction, disc-id math, checksums (AR v1/v2 + CTDB CRC32),
# response parsing. Links QtCore (SHA-1 for the MusicBrainz disc id,
# QXmlStreamReader for CTDB) and zlib (hardware-accelerated CRC32 — the
# CTDB offset scan is CRC32-bound). No UI, no networking.
find_package(ZLIB REQUIRED)
add_library(arverify STATIC
    src/ArVerify.cpp
    src/ArVerify.h
)
target_include_directories(arverify PUBLIC src)
target_link_libraries(arverify PRIVATE Qt6::Core PUBLIC ZLIB::ZLIB)
set_target_properties(arverify PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)

qt_add_executable(concerto
    src/main.cpp
)

target_include_directories(concerto PRIVATE src)

qt_add_qml_module(concerto
    URI         PLYR
    VERSION     1.0
    QML_FILES
        qml/Main.qml
        qml/MainMobile.qml
        qml/PLYRSeekSlider.qml
        qml/MarqueeText.qml
        qml/PlayingBars.qml
        # qml/SyncTuner.qml   # Re-enable alongside the SyncTuner{} block in Main.qml if A/V sync drifts.
        qml/EqPanel.qml
        qml/EqBandSlider.qml
        qml/EqPresetChip.qml
        qml/EqCurveOverlay.qml
        qml/FolderPicker.qml
        qml/CdDiscCanvas.qml
        qml/RipView.qml
        qml/AudioFeaturesHud.qml
        qml/ScopeView.qml
        qml/SpectrumView.qml
        qml/CqtSpectrogramView.qml
        qml/TonnetzOverlay.qml
        qml/TonalTorusView.qml
        qml/MfccTrajectoryView.qml
        qml/HilbertRosetteView.qml
        qml/ShaderToyPane.qml
        qml/MilkdropView.qml
        qml/PdeView.qml
        qml/SsmScrubber.qml
        qml/PersistenceBarcodeView.qml
    RESOURCES
        resources/images/concerto-logo.png
    SOURCES
        src/FlacTags.cpp
        src/FlacTags.h
        src/Track.h
        src/PlaylistModel.cpp
        src/PlaylistModel.h
        src/FftProcessor.cpp
        src/FftProcessor.h
        src/VisualizerRegistry.cpp
        src/VisualizerRegistry.h
        src/AudioFeatures.cpp
        src/AudioFeatures.h
        src/SpectrumTexture.cpp
        src/SpectrumTexture.h
        src/PcmPipe.cpp
        src/PcmPipe.h
        src/AudioEngine.cpp
        src/AudioEngine.h
        src/AudioWorker.cpp
        src/AudioWorker.h
        src/eq_engine.c
        src/eq_engine.h
        src/eq_presets.c
        src/eq_presets.h
        src/EqController.cpp
        src/EqController.h
        src/SystemPaths.cpp
        src/SystemPaths.h
        src/macos_prewarm.h
        src/macos_titlebar.h
        src/MusicBlocker.h
        src/CdDevice.h
        src/CdShield.h
        src/DriveOffsetDb.cpp
        src/DriveOffsetDb.h
        src/FlacEncode.cpp
        src/FlacEncode.h
        src/MusicBrainz.cpp
        src/MusicBrainz.h
        src/MetadataModel.cpp
        src/MetadataModel.h
        src/MetadataScoring.cpp
        src/MetadataScoring.h
        src/MetadataCache.cpp
        src/MetadataCache.h
        src/MetadataResolver.cpp
        src/MetadataResolver.h
        src/CdTextReader.cpp
        src/CdTextReader.h
        src/AcoustIdProvider.cpp
        src/AcoustIdProvider.h
        src/PendingSubmissions.cpp
        src/PendingSubmissions.h
        src/Ripper.cpp
        src/Ripper.h
        src/RipBatchStore.cpp
        src/RipBatchStore.h
        src/RipWorker.cpp
        src/RipWorker.h
        src/ScopeRenderer.cpp
        src/ScopeRenderer.h
        src/SpectrumAnalyzer.cpp
        src/SpectrumAnalyzer.h
        src/CqtAnalyzer.cpp
        src/CqtAnalyzer.h
        src/CqtSpectrogram.cpp
        src/CqtSpectrogram.h
        src/ChromaAnalyzer.cpp
        src/ChromaAnalyzer.h
        src/TonnetzView.cpp
        src/TonnetzView.h
        src/KeyEstimator.cpp
        src/KeyEstimator.h
        src/MfccAnalyzer.cpp
        src/MfccAnalyzer.h
        src/MfccTrajectory.cpp
        src/MfccTrajectory.h
        src/PersistentHomologyAnalyzer.cpp
        src/PersistentHomologyAnalyzer.h
        src/PersistenceBarcode.cpp
        src/PersistenceBarcode.h
        src/HilbertAnalyzer.cpp
        src/HilbertAnalyzer.h
        src/HilbertRosette.cpp
        src/HilbertRosette.h
        src/MilkdropRuntime.cpp
        src/MilkdropRuntime.h
        src/MilkdropView.cpp
        src/MilkdropView.h
        src/PdeView.cpp
        src/PdeView.h
        src/SsmGenerator.cpp
        src/SsmGenerator.h
        src/SsmLoader.cpp
        src/SsmLoader.h
        src/SsmTexture.cpp
        src/SsmTexture.h
        src/FlacDecode.cpp
        src/FlacDecode.h
        # Folder-time library metadata pipeline (Steps 1-3 of
        # docs/LIBRARY_METADATA_PLAN.md). SQLite store, multiformat
        # tag I/O, content-stable hashing, and the FolderIdentifier
        # strategy chain (Stages Z + A + B; C / D stubbed).
        src/LibraryDatabase.cpp
        src/LibraryDatabase.h
        src/AudioTagIo.cpp
        src/AudioTagIo.h
        src/AudioFrameHash.cpp
        src/AudioFrameHash.h
        src/FolderIdentifier.cpp
        src/FolderIdentifier.h
)

# Embed the bundled AccurateRip drive-offset table into concerto so the
# in-app rip can look up the connected drive's offset at rip time — same
# resource cdrip_cli already uses for the CLI rip flow.
qt_add_resources(concerto "concerto_drive_offsets"
    PREFIX "/"
    FILES resources/drive_offsets.json
)

# macOS-only platform sources for concerto. AppKit + DiskArbitration get
# linked together because the rip-session components depend on both:
#   * macos_prewarm.mm    — NSOpenPanel prewarm (AppKit; iOS uses
#                           UIDocumentPickerViewController, so excluded).
#   * MusicBlocker_macOS.mm — NSWorkspace observer that force-terminates
#                           Music.app / iTunes if they try to fresh-launch
#                           during a rip (AppKit). Safety net behind
#                           CdShield.
#   * CdShield_macOS.cpp  — Disk Arbitration session that claims any CD
#                           media on appearance, so Music never sees the
#                           disc event in the first place. Plain C++ —
#                           DA is a C API, no Obj-C needed.
if(APPLE AND NOT IOS)
    target_sources(concerto PRIVATE
        src/macos_prewarm.mm
        src/macos_titlebar.mm
        src/MusicBlocker_macOS.mm
        src/CdShield_macOS.cpp
        src/CdDevice_macOS.cpp
        # A/V-sync clock shims. AudioClock attaches a render-notify
        # callback to a probe AUHAL bound to the same default output
        # device Qt's QAudioSink uses, capturing per-buffer host-time +
        # sample-count anchors. DisplayClock attaches a
        # CAMetalDisplayLink (macOS 14+) to QQuickWindow's CAMetalLayer,
        # surfacing targetPresentationTimestamp per refresh. Both publish
        # via lock-free seqlocks so the audio thread's lookahead tap can
        # solve for the exact playhead sample at the next scanout instant.
        src/AudioClock_macOS.h
        src/AudioClock_macOS.mm
        src/DisplayClock_macOS.h
        src/DisplayClock_macOS.mm
    )
    set_source_files_properties(
        src/AudioClock_macOS.mm
        src/DisplayClock_macOS.mm
        PROPERTIES LANGUAGE OBJCXX
    )
    target_link_libraries(concerto PRIVATE
        "-framework AppKit"
        "-framework DiskArbitration"
        "-framework IOKit"
        "-framework CoreFoundation"
        # CoreAudio anchor (AUHAL + AudioUnit render-notify) and the
        # display anchor (CAMetalDisplayLink / CVDisplayLink fallback).
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework AudioUnit"
        "-framework CoreVideo"
        "-framework QuartzCore"
        "-framework Metal"
    )
endif()

# Standalone CLI for testing the FLAC parser in isolation — desktop only.
# iOS doesn't run CLI tools, so the target is skipped there.
if(NOT IOS AND NOT ANDROID)
    qt_add_executable(flactest
        src/flactest_main.cpp
        src/FlacTags.cpp
        src/FlacTags.h
    )
    target_link_libraries(flactest PRIVATE Qt6::Core)
    set_target_properties(flactest PROPERTIES MACOSX_BUNDLE FALSE)
endif()

# Vendored libFLAC (xiph/flac 1.4.3, pruned to the C library + its
# direct deps — no C++ wrapper, CLIs, tests, docs, examples). Provides
# the static `FLAC` target. Matches the third_party/kissfft precedent and
# removes the runtime dependency on a Homebrew-installed libFLAC.dylib.
# EXCLUDE_FROM_ALL keeps the upstream microbench/etc. targets out of the
# default build — they only build if something we link to depends on them.
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(WITH_OGG OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# Suppress Qt's AUTOMOC/AUTORCC/AUTOUIC for the vendored subtree — none of
# those C sources are Qt-aware, and the inherited setting otherwise
# generates empty mocs_compilation.cpp objects that ranlib warns on.
set(_saved_automoc ${CMAKE_AUTOMOC})
set(_saved_autorcc ${CMAKE_AUTORCC})
set(_saved_autouic ${CMAKE_AUTOUIC})
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)
add_subdirectory(third_party/flac EXCLUDE_FROM_ALL)
set(CMAKE_AUTOMOC ${_saved_automoc})
set(CMAKE_AUTORCC ${_saved_autorcc})
set(CMAKE_AUTOUIC ${_saved_autouic})

# Standalone CLI: verify ripped FLAC discs against AccurateRip. Desktop only
# — it needs a filesystem of .flac files and network access.
if(NOT IOS AND NOT ANDROID)
    qt_add_executable(arverify_cli
        src/arverify_main.cpp
        src/FlacDecode.cpp
        src/FlacDecode.h
    )
    target_include_directories(arverify_cli PRIVATE src)
    target_link_libraries(arverify_cli PRIVATE
        Qt6::Core
        Qt6::Network
        arverify
        FLAC
    )
    set_target_properties(arverify_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # Round-trip test for the FLAC encoder: decode an input FLAC,
    # re-encode it, decode the result, and byte-compare PCM. Catches
    # any silent corruption in the encode path.
    qt_add_executable(flacrt_cli
        src/flacrt_main.cpp
        src/FlacDecode.cpp
        src/FlacDecode.h
        src/FlacEncode.cpp
        src/FlacEncode.h
    )
    target_include_directories(flacrt_cli PRIVATE src)
    target_link_libraries(flacrt_cli PRIVATE Qt6::Core FLAC)
    set_target_properties(flacrt_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # Sinusoid-verification harness for CqtAnalyzer. Feeds known-frequency
    # tones through the analyzer and reports peak bin offsets. Used to
    # validate kernel construction + octave alignment at development time;
    # not bundled into the GUI app.
    qt_add_executable(cqtverify_cli
        src/cqtverify_main.cpp
        src/CqtAnalyzer.cpp
        src/CqtAnalyzer.h
        src/AudioFeatures.cpp
        src/AudioFeatures.h
    )
    target_include_directories(cqtverify_cli PRIVATE src)
    # Multimedia is dragged in via AudioFeatures.cpp's QAudioFormat include
    # path; with no audio device opened the link is dependency-only.
    target_link_libraries(cqtverify_cli PRIVATE
        Qt6::Core
        Qt6::Qml
        Qt6::Multimedia
        kissfft
    )
    set_target_properties(cqtverify_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # Chromagram + Tonnetz correctness harness (Layer 2a). Synthesizes
    # known triads and confirms the chromagram peaks and the brightest
    # Tonnetz triad land where they should. Pulls Qt6::Quick because
    # TonnetzView is a QQuickPaintedItem -- we never realize a window,
    # but the type registration + QQuickPaintedItem base ctor want the
    # module linked.
    qt_add_executable(chromaverify_cli
        src/chromaverify_main.cpp
        src/CqtAnalyzer.cpp
        src/CqtAnalyzer.h
        src/AudioFeatures.cpp
        src/AudioFeatures.h
        src/ChromaAnalyzer.cpp
        src/ChromaAnalyzer.h
        src/TonnetzView.cpp
        src/TonnetzView.h
    )
    target_include_directories(chromaverify_cli PRIVATE src)
    target_link_libraries(chromaverify_cli PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::Multimedia
        kissfft
    )
    set_target_properties(chromaverify_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # Layer 2c correctness harness. Drives an ABA test signal
    # (200+800 Hz sines / white noise / sines) through MfccAnalyzer
    # and MfccTrajectory, then prints:
    #   * Jacobi eigendecomposition unit-test results (3x3 diag, 3x3
    #     symmetric, 13x13 identity).
    #   * Centroid + std per section, distances between centroids.
    #   * Top-3 PCA eigenvalues and the lambda_1 / lambda_3 ratio.
    # MfccTrajectory.cpp must be in the source list -- the harness
    # calls into jacobiEigen<N>() (template defined there) and uses
    # MfccTrajectory::debugSnapshot. We pull Qt6::Quick because
    # MfccTrajectory is a QQuickRhiItem -- registering the type needs
    # the module linked, even though we never instantiate a window.
    qt_add_executable(mfccverify_cli
        src/mfccverify_main.cpp
        src/MfccAnalyzer.cpp
        src/MfccAnalyzer.h
        src/MfccTrajectory.cpp
        src/MfccTrajectory.h
        src/AudioFeatures.cpp
        src/AudioFeatures.h
    )
    target_include_directories(mfccverify_cli PRIVATE src)
    target_link_libraries(mfccverify_cli PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Qml
        Qt6::Quick
        Qt6::Multimedia
        kissfft
    )
    set_target_properties(mfccverify_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # Layer 4a correctness harness. Drives PersistentHomologyAnalyzer's
    # synchronous computeFromPointCloudSync() with a small library of
    # synthetic point clouds (circles of varying radius, a noisy
    # Lissajous figure, a trefoil knot in R^3, a 4-point square) and
    # asserts the persistent-homology output matches textbook Betti
    # numbers. Also prints per-N wall-clock so the layer report can
    # quote performance numbers. Headless -- never opens a window;
    # Quick is linked only because the analyzer's QML_ELEMENT
    # registration needs the module.
    qt_add_executable(phverify_cli
        src/phverify_main.cpp
        src/PersistentHomologyAnalyzer.cpp
        src/PersistentHomologyAnalyzer.h
        src/MfccAnalyzer.cpp
        src/MfccAnalyzer.h
        src/AudioFeatures.cpp
        src/AudioFeatures.h
    )
    target_include_directories(phverify_cli PRIVATE src)
    target_link_libraries(phverify_cli PRIVATE
        Qt6::Core
        Qt6::Qml
        Qt6::Quick
        Qt6::Multimedia
        ripser
        kissfft
    )
    set_target_properties(phverify_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # Layer 4c correctness harness. Synthesizes pure-sinusoid / white-
    # noise / silence test signals, drives them through HilbertAnalyzer,
    # and reports per-band envelopes. The 440 Hz sine should excite the
    # nearest-by-log-Hz band(s) and leave the rest near zero; white noise
    # should produce a roughly-equal envelope across all bands; silence
    # should give all-zero. Also unit-tests the FIR Hilbert kernel
    # against the analytic ideal at a sweep of frequencies.
    qt_add_executable(hilbertverify_cli
        src/hilbertverify_main.cpp
        src/HilbertAnalyzer.cpp
        src/HilbertAnalyzer.h
        src/AudioFeatures.cpp
        src/AudioFeatures.h
    )
    target_include_directories(hilbertverify_cli PRIVATE src)
    target_link_libraries(hilbertverify_cli PRIVATE
        Qt6::Core
        Qt6::Qml
        Qt6::Multimedia
        kissfft
    )
    set_target_properties(hilbertverify_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # Layer 3b correctness harness. Loads a few minimal .milk presets,
    # drives audio scalars by hand, and asserts the runtime's q-register
    # passing + per-vertex displacement work as documented. Headless;
    # never opens a window or RHI context. Used to keep the parser +
    # variable-binding glue from silently regressing.
    qt_add_executable(milkverify_cli
        src/milkverify_main.cpp
        src/MilkdropRuntime.cpp
        src/MilkdropRuntime.h
    )
    target_include_directories(milkverify_cli PRIVATE src)
    target_link_libraries(milkverify_cli PRIVATE
        Qt6::Core
        projectM::Eval
    )
    set_target_properties(milkverify_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # Layer 4d correctness harness. Runs a CPU reference Gray-Scott
    # solver with the same parameters the shader uses, asserts the
    # variance + spot count after 1000 steps. Pure-arithmetic CLI; no
    # Qt window or RHI context.
    qt_add_executable(pdeverify_cli
        src/pdeverify_main.cpp
    )
    target_link_libraries(pdeverify_cli PRIVATE Qt6::Core)
    set_target_properties(pdeverify_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # Layer 2b correctness harness. Synthesizes three known triads
    # (C major / A minor / G major) and confirms KeyEstimator's
    # published keyName, confidence, and torus position. Same sources
    # graph as chromaverify_cli + KeyEstimator.cpp/.h; no Quick widget
    # needed -- KeyEstimator is plain QObject.
    qt_add_executable(keyverify_cli
        src/keyverify_main.cpp
        src/CqtAnalyzer.cpp
        src/CqtAnalyzer.h
        src/AudioFeatures.cpp
        src/AudioFeatures.h
        src/ChromaAnalyzer.cpp
        src/ChromaAnalyzer.h
        src/KeyEstimator.cpp
        src/KeyEstimator.h
    )
    target_include_directories(keyverify_cli PRIVATE src)
    target_link_libraries(keyverify_cli PRIVATE
        Qt6::Core
        Qt6::Qml
        Qt6::Multimedia
        kissfft
    )
    set_target_properties(keyverify_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # MusicBrainz lookup demo: given a disc folder, reconstruct the TOC,
    # query MB for the disc ID, print metadata, optionally re-encode
    # track 1 with derived Vorbis tags and read them back to verify.
    qt_add_executable(mbquery_cli
        src/mbquery_main.cpp
        src/MusicBrainz.cpp
        src/MusicBrainz.h
        src/MetadataModel.cpp
        src/MetadataModel.h
        src/MetadataScoring.cpp
        src/MetadataScoring.h
        src/MetadataCache.cpp
        src/MetadataCache.h
        src/MetadataResolver.cpp
        src/MetadataResolver.h
        src/CdTextReader.cpp
        src/CdTextReader.h
        src/AcoustIdProvider.cpp
        src/AcoustIdProvider.h
        src/PendingSubmissions.cpp
        src/PendingSubmissions.h
        src/FlacDecode.cpp
        src/FlacDecode.h
        src/FlacEncode.cpp
        src/FlacEncode.h
        src/FlacTags.cpp
        src/FlacTags.h
        # SystemPaths gives the CLI access to the same appDataDir() +
        # migrateAppData() helpers the GUI app uses, so cache hits land
        # in the same on-disk location.
        src/SystemPaths.cpp
        src/SystemPaths.h
    )
    target_include_directories(mbquery_cli PRIVATE src)
    target_link_libraries(mbquery_cli PRIVATE
        Qt6::Core
        Qt6::Network
        arverify
        FLAC
    )
    set_target_properties(mbquery_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # Folder-time library pipeline test harness. Mirrors mbquery_cli's
    # shape: a single QCoreApplication-backed CLI that exercises the
    # LibraryDatabase + AudioTagIo + AudioFrameHash + FolderIdentifier
    # stack against a folder of audio files. Used to validate Steps 1-3
    # of docs/LIBRARY_METADATA_PLAN.md before the QML library view lands.
    qt_add_executable(library_cli
        src/library_cli_main.cpp
        src/LibraryDatabase.cpp
        src/LibraryDatabase.h
        src/AudioTagIo.cpp
        src/AudioTagIo.h
        src/AudioFrameHash.cpp
        src/AudioFrameHash.h
        src/FolderIdentifier.cpp
        src/FolderIdentifier.h
        # Reuse the existing pipeline pieces verbatim — same shapes the
        # rip flow uses.
        src/MusicBrainz.cpp
        src/MusicBrainz.h
        src/MetadataModel.cpp
        src/MetadataModel.h
        src/MetadataScoring.cpp
        src/MetadataScoring.h
        src/MetadataCache.cpp
        src/MetadataCache.h
        src/SystemPaths.cpp
        src/SystemPaths.h
        # FlacTags is pulled in only for buildVorbisTags() / debugDumpTrack
        # symbol resolution where AudioTagIo's CONCERTO_* writeback later
        # delegates. Cheap to compile, keeps the link surface aligned with
        # mbquery_cli.
        src/FlacTags.cpp
        src/FlacTags.h
        src/FlacEncode.cpp
        src/FlacEncode.h
    )
    target_include_directories(library_cli PRIVATE src)
    target_link_libraries(library_cli PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Sql
        Qt6::Multimedia    # QAudioDecoder for AudioFrameHash
        TagLib::tag
        FLAC
    )
    set_target_properties(library_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # Native CD-DA reader. Cross-platform shape lives in CdDevice.h; the
    # macOS implementation uses IOKit's BSD-client ioctls (DKIOCCDREAD &
    # friends) — public, entitlement-free, App-Store-eligible. Linux and
    # Windows sources get added behind their own `if(...)` guards when
    # those backends land.
    set(CDRIP_SOURCES
        src/cdrip_main.cpp
        src/CdDevice.h
        src/CdShield.h
        src/DriveOffsetDb.h
        src/DriveOffsetDb.cpp
        src/FlacEncode.cpp
        src/FlacEncode.h
        src/MusicBrainz.cpp
        src/MusicBrainz.h
        src/MetadataModel.cpp
        src/MetadataModel.h
        src/MetadataScoring.cpp
        src/MetadataScoring.h
    )
    if(APPLE)
        list(APPEND CDRIP_SOURCES
            src/CdDevice_macOS.cpp
            src/CdShield_macOS.cpp
        )
    endif()
    qt_add_executable(cdrip_cli ${CDRIP_SOURCES})
    target_include_directories(cdrip_cli PRIVATE src)
    # arverify provides the disc-ID math; FLAC is the vendored libFLAC
    # encoder; Qt6::Network is the transport for MusicBrainz lookups.
    target_link_libraries(cdrip_cli PRIVATE
        Qt6::Core Qt6::Network arverify FLAC)
    # Bundle the AccurateRip drive-offsets table (~4800 drives, parsed
    # from accuraterip.com/driveoffsets.htm via scripts/import_drive_offsets.py).
    # Accessed at runtime as `:/drive_offsets.json` by DriveOffsetDb.cpp.
    qt_add_resources(cdrip_cli "cdrip_resources"
        PREFIX "/"
        FILES resources/drive_offsets.json
    )
    if(APPLE)
        target_link_libraries(cdrip_cli PRIVATE
            "-framework IOKit"
            "-framework DiskArbitration"
            "-framework CoreFoundation"
        )
    endif()
    set_target_properties(cdrip_cli PROPERTIES MACOSX_BUNDLE FALSE)

    # Layer 4b self-similarity-matrix generator. Standalone CLI: decode a
    # FLAC, extract MFCCs at a 0.5 s hop (2 s for tracks >10 min), build
    # the T x T cosine-similarity matrix, write {flac}.ssm. Also supports
    # --selftest for the in-memory ABA structure unit test (no FLAC
    # needed). Eventually RipWorker hooks this same code path post-encode
    # so every ripped track lands next to its SSM sidecar.
    qt_add_executable(ssmgen_cli
        src/ssmgen_main.cpp
        src/SsmGenerator.cpp
        src/SsmGenerator.h
        src/FlacDecode.cpp
        src/FlacDecode.h
    )
    target_include_directories(ssmgen_cli PRIVATE src)
    target_link_libraries(ssmgen_cli PRIVATE Qt6::Core FLAC kissfft)
    set_target_properties(ssmgen_cli PROPERTIES MACOSX_BUNDLE FALSE)
endif()

target_link_libraries(concerto PRIVATE
    Qt6::Core
    Qt6::Gui
    # GuiPrivate gives ScopeRenderer access to QRhi/QShader — the
    # analytic-line-integral scope (Layer 1a) drives Qt's RHI directly
    # via QQuickRhiItem, which requires the private QRhi headers.
    Qt6::GuiPrivate
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Multimedia
    # SQLite-backed library DB (concerto::library::LibraryDatabase). Qt6
    # bundles the SQLite driver as a plugin; no separate sqlite dep needed.
    Qt6::Sql
    # TagLib (LGPL-2.1, dynamic-link only — see find_package comment near
    # the top of this file). Powers AudioTagIo's multiformat read/write.
    TagLib::tag
    arverify
    FLAC
    kissfft
    # projectm-eval (MIT) — the NS-EEL2 reimplementation that powers
    # Layer 3b's MilkDrop expression runtime. Tiny static lib, no Qt.
    projectM::Eval
    # ripser (MIT) — Vietoris-Rips persistent-homology engine for
    # Layer 4a's H0/H1 barcode visualizer. Single-file C++17 lib;
    # see third_party/ripser/CONCERTO.patch for the minimal patch we
    # apply to turn its CLI source into a linkable library.
    ripser
)

# Compile the GLSL visualizer shaders to .qsb (Qt Shader Baker) files.
# qsb emits multi-target bytecode so the same shaders run on Metal (macOS,
# iOS), Vulkan (Linux, Android), D3D11/12 (Windows), and OpenGL. ShaderEffect
# in Main.qml loads them by URL: qrc:/shaders/viz.{vert,frag}.qsb
qt_add_shaders(concerto "viz_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/viz.vert
        shaders/viz.frag
)

# CD disc surface — silver + diffraction-grating iridescence + an
# orbiting specular highlight. Backs CdDiscCanvas's bottom layer; the
# Canvas2D overlay on top still draws track boundary circles, sweep
# line, hub trim, etc.
qt_add_shaders(concerto "cd_disc_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/cd_disc.vert
        shaders/cd_disc.frag
)

# Oscilloscope / vectorscope shaders — Layer 1a of the visualizer
# stack. The segment shader implements m1el's woscope closed-form
# Gaussian-beam line integral (one quad per inter-sample segment, with
# beam-velocity modulation baked into the analytic 1/L factor). The
# decay shader is reused for both phosphor fade and the final composite
# pass — same math (tint × scalar × sample), different uniforms.
qt_add_shaders(concerto "scope_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/scope_segment.vert
        shaders/scope_segment.frag
        shaders/scope_decay.vert
        shaders/scope_decay.frag
)

# Spectrum analyzer shaders — Layer 1b. The fragment stage samples three
# 1×N R8 textures (live curve, peak-hold, infinite-peak) emitted by
# SpectrumAnalyzer, composites a viridis-tinted fill + peak traces, and
# draws a subtle bottom-to-top brightness ramp. All log-x remapping and
# +N dB/oct tilting happens CPU-side in SpectrumAnalyzer::runPipeline.
qt_add_shaders(concerto "spectrum_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/spectrum.vert
        shaders/spectrum.frag
)

# Constant-Q spectrogram waterfall shaders -- Layer 1c. The fragment
# stage samples a 2D R8 ring texture (columns x bins) with wraparound
# along the time axis, applies a perceptual gamma, then routes through
# either the analytic magma colormap (mastering default) or a two-
# colour gradient. All octave-recursive CQT math lives in CqtAnalyzer
# and CqtSpectrogram on the CPU side.
qt_add_shaders(concerto "cqt_spectrogram_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/cqt_spectrogram.vert
        shaders/cqt_spectrogram.frag
)

# Janata-style tonal-torus shaders -- Layer 2b. Raymarched SDF of a
# torus with a glowing point at the current Krumhansl-Kessler key
# estimate's toroidal centroid. All audio analysis happens on the CPU
# in KeyEstimator; the fragment shader is a pure-rendering pass driven
# by a handful of vec4 uniforms.
qt_add_shaders(concerto "tonal_torus_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/tonal_torus.vert
        shaders/tonal_torus.frag
)

# 3D MFCC trajectory shaders -- Layer 2c. Each segment of the 600-row
# trajectory becomes a screen-space-extruded quad: the vertex shader
# projects both 3D endpoints to NDC, expands a lineWidth-pixel-thick
# quad in the screen perpendicular; the fragment shader applies AA on
# the lateral coordinate, an age-based color + alpha gradient
# (tailColor -> headColor), and clip-z fog. All MFCC + PCA math lives
# CPU-side in MfccAnalyzer / MfccTrajectory.
qt_add_shaders(concerto "mfcc_trajectory_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/mfcc_trajectory.vert
        shaders/mfcc_trajectory.frag
)

# Hilbert-pair rosette shaders -- Layer 4c. Each of 8 log-spaced
# subbands gets a Gaussian dot whose radius = envelope, angle = base +
# instantaneous phase of the band's analytic signal. Two pipelines:
#   * hilbert_decay.{vert,frag} -- full-screen quad sampling the prior
#     accumulation buffer with intensity multiplier exp(-dt/tau). Reused
#     for the final composite pass (decay = 1, tint = white).
#   * hilbert_dot.{vert,frag}   -- per-band quad, fragment evaluates
#     exp(-d^2 / (2*sigma^2)) and looks up the per-band hue from a UBO
#     color array, output as premultiplied-alpha RGBA for additive
#     accumulation.
# All Hilbert + bandpass math lives CPU-side in HilbertAnalyzer.
qt_add_shaders(concerto "hilbert_rosette_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/hilbert_decay.vert
        shaders/hilbert_decay.frag
        shaders/hilbert_dot.vert
        shaders/hilbert_dot.frag
)

# ShaderToy-style audio shader pane -- Layer 3a. A library of bundled
# fragment shaders, each consuming the same 512x2 spectrum/waveform
# texture exposed by SpectrumTexture plus a shared set of UBO audio
# scalars (bass/mid/treb/rms/centroid/flux). Driven from QML by
# ShaderToyPane.qml, which swaps fragmentShader / vertexShader URLs
# to cycle. Every shader uses an identical UBO layout so cycling has
# no flicker (Qt doesn't have to relayout uniforms between draws).
#
# Adding a new shader: drop shaders/st_NAME.{vert,frag} next to the
# others, list both here, and add a { name, vert, frag, desc } entry
# in qml/ShaderToyPane.qml's shaderLibrary array. Rebuild and it's
# live (compile-time, not runtime — users can author new shaders but
# each requires a rebuild).
qt_add_shaders(concerto "shadertoy_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/st_polar_spectrum.vert
        shaders/st_polar_spectrum.frag
        shaders/st_domain_warp.vert
        shaders/st_domain_warp.frag
        shaders/st_spectrum_tunnel.vert
        shaders/st_spectrum_tunnel.frag
        shaders/st_waveform_heightmap.vert
        shaders/st_waveform_heightmap.frag
        shaders/st_reaction_diffusion.vert
        shaders/st_reaction_diffusion.frag
)

# Milkdrop warp-mesh visualizer shaders -- Layer 3b. Two passes:
#   * milkdrop_warp.{vert,frag} -- samples the previous-frame texture
#     at the 32x24 grid's per-vertex UV (computed by MilkdropRuntime),
#     applies `decay`. Output: a ping-pong RGBA8 texture.
#   * milkdrop_composite.{vert,frag} -- blits the warp output onto the
#     QQuickRhiItem's visible target with optional gamma correction.
# Both share their UBO layouts with the matching C struct in
# src/MilkdropView.cpp; std140 packing must match.
qt_add_shaders(concerto "milkdrop_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/milkdrop_warp.vert
        shaders/milkdrop_warp.frag
        shaders/milkdrop_composite.vert
        shaders/milkdrop_composite.frag
)

# Audio-modulated PDE visualizer shaders -- Layer 4d. Two modes share
# the same fullscreen-quad vertex stage:
#   * pde_grayscott.frag         -- Gray-Scott reaction-diffusion solver
#                                   (reads prev RGBA16F state, writes new).
#   * pde_grayscott_display.frag -- maps (u, v) state to visible colours.
#   * pde_chladni.frag           -- stateless vibrating-plate eigenmodes.
# UBO layouts are duplicated in src/PdeView.cpp; std140 packing must match.
qt_add_shaders(concerto "pde_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/pde_quad.vert
        shaders/pde_grayscott.frag
        shaders/pde_grayscott_display.frag
        shaders/pde_chladni.frag
)

# Self-similarity-matrix scrubber shaders -- Layer 4b. Both pipelines
# share the same R8 sidecar texture exposed by SsmTexture, the same
# magma colormap, and the same UBO layout (qt_Matrix, qt_Opacity,
# playbackFrac, tintColor). ssm_2d renders a full T x T heatmap with a
# playback crosshair; ssm_inline renders one row at v=playbackFrac as a
# horizontal strip for use behind the seek bar.
qt_add_shaders(concerto "ssm_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/ssm_2d.vert
        shaders/ssm_2d.frag
        shaders/ssm_inline.vert
        shaders/ssm_inline.frag
)

# Bundled MilkDrop preset library -- text files in the .milk format,
# parsed at runtime by MilkdropRuntime. PREFIX "/" + relative path
# means qml/C++ accesses them as `qrc:/presets/01_simple_zoom.milk`.
qt_add_resources(concerto "milkdrop_presets"
    PREFIX "/"
    FILES
        presets/01_simple_zoom.milk
        presets/02_swirl.milk
        presets/03_kaleidoscope.milk
)

set_target_properties(concerto PROPERTIES
    OUTPUT_NAME                  "Concerto"
    MACOSX_BUNDLE                TRUE
    MACOSX_BUNDLE_BUNDLE_NAME    "Concerto"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.thompson.concerto"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
    MACOSX_BUNDLE_ICON_FILE      "Concerto.icns"
)

# Bundle the Concerto icon into Contents/Resources so Info.plist can
# resolve the CFBundleIconFile set above.
set(_CONCERTO_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/Concerto.icns")
if(EXISTS "${_CONCERTO_ICNS}")
    set_source_files_properties("${_CONCERTO_ICNS}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(concerto PRIVATE "${_CONCERTO_ICNS}")
endif()

# iOS-specific bundle/signing metadata. Code signing is left to Xcode's
# "Sign to Run Locally" (Simulator) or the user's free Apple ID cert
# (device), which Xcode sets up through its Signing & Capabilities tab.
if(IOS)
    set_target_properties(concerto PROPERTIES
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.thompson.concerto"
        # Qt 6.11 iOS prebuilts target iOS 17.0 — must match or ld warns.
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "17.0"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"  # 1 = iPhone, 2 = iPad
        XCODE_ATTRIBUTE_INFOPLIST_KEY_CFBundleDisplayName "Concerto"
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    )

    # Bundle the AppIcon asset catalog. Xcode compiles .xcassets and
    # auto-generates the full suite of iPhone/iPad icon sizes from the
    # single 1024×1024 source; iOS rounds the corners at runtime.
    set(_IOS_XCASSETS
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/AppIcon.xcassets")
    if(EXISTS "${_IOS_XCASSETS}")
        target_sources(concerto PRIVATE "${_IOS_XCASSETS}")
        set_source_files_properties("${_IOS_XCASSETS}"
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    endif()

    # Qt 6.11's iOS FFmpeg multimedia backend references symbols from the
    # bundled FFmpeg xcframeworks but does NOT auto-link them — we have
    # to pull them in ourselves, or the link fails with
    # "Undefined symbols: _sws_freeContext, ...".
    set(_QT_IOS_FFMPEG_DIR "$ENV{HOME}/Qt/6.11.0/ios/lib/ffmpeg")
    foreach(_lib avcodec avformat avutil swresample swscale)
        target_link_libraries(concerto PRIVATE
            "${_QT_IOS_FFMPEG_DIR}/lib${_lib}.xcframework/ios-arm64/lib${_lib}.framework/lib${_lib}"
        )
    endforeach()
endif()
