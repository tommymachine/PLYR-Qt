cmake_minimum_required(VERSION 3.21)
project(plyr_qt VERSION 0.1 LANGUAGES CXX C)

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

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Network
    Qml
    Quick
    QuickControls2
    Multimedia
    ShaderTools
)

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

qt_add_executable(plyr_qt
    src/main.cpp
)

target_include_directories(plyr_qt PRIVATE src)

qt_add_qml_module(plyr_qt
    URI         PLYR
    VERSION     1.0
    QML_FILES
        qml/Main.qml
        qml/MainMobile.qml
        qml/PLYRSeekSlider.qml
        qml/MarqueeText.qml
        qml/EqPanel.qml
        qml/EqBandSlider.qml
        qml/EqPresetChip.qml
        qml/EqCurveOverlay.qml
        qml/FolderPicker.qml
        qml/CdDiscCanvas.qml
        qml/RipView.qml
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
        src/MusicBlocker.h
        src/CdShield.h
        src/Ripper.cpp
        src/Ripper.h
)

# macOS-only platform sources for plyr_qt. AppKit + DiskArbitration get
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
    target_sources(plyr_qt PRIVATE
        src/macos_prewarm.mm
        src/MusicBlocker_macOS.mm
        src/CdShield_macOS.cpp
    )
    target_link_libraries(plyr_qt PRIVATE
        "-framework AppKit"
        "-framework DiskArbitration"
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

    # MusicBrainz lookup demo: given a disc folder, reconstruct the TOC,
    # query MB for the disc ID, print metadata, optionally re-encode
    # track 1 with derived Vorbis tags and read them back to verify.
    qt_add_executable(mbquery_cli
        src/mbquery_main.cpp
        src/MusicBrainz.cpp
        src/MusicBrainz.h
        src/FlacDecode.cpp
        src/FlacDecode.h
        src/FlacEncode.cpp
        src/FlacEncode.h
    )
    target_include_directories(mbquery_cli PRIVATE src)
    target_link_libraries(mbquery_cli PRIVATE
        Qt6::Core
        Qt6::Network
        arverify
        FLAC
    )
    set_target_properties(mbquery_cli PROPERTIES MACOSX_BUNDLE FALSE)

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
endif()

target_link_libraries(plyr_qt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Multimedia
    kissfft
)

# Compile the GLSL visualizer shaders to .qsb (Qt Shader Baker) files.
# qsb emits multi-target bytecode so the same shaders run on Metal (macOS,
# iOS), Vulkan (Linux, Android), D3D11/12 (Windows), and OpenGL. ShaderEffect
# in Main.qml loads them by URL: qrc:/shaders/viz.{vert,frag}.qsb
qt_add_shaders(plyr_qt "viz_shaders"
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
qt_add_shaders(plyr_qt "cd_disc_shaders"
    PRECOMPILE
    BASE    shaders
    PREFIX  "/shaders"
    FILES
        shaders/cd_disc.vert
        shaders/cd_disc.frag
)

set_target_properties(plyr_qt PROPERTIES
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
    target_sources(plyr_qt PRIVATE "${_CONCERTO_ICNS}")
endif()

# iOS-specific bundle/signing metadata. Code signing is left to Xcode's
# "Sign to Run Locally" (Simulator) or the user's free Apple ID cert
# (device), which Xcode sets up through its Signing & Capabilities tab.
if(IOS)
    set_target_properties(plyr_qt PROPERTIES
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
        target_sources(plyr_qt PRIVATE "${_IOS_XCASSETS}")
        set_source_files_properties("${_IOS_XCASSETS}"
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    endif()

    # Qt 6.11's iOS FFmpeg multimedia backend references symbols from the
    # bundled FFmpeg xcframeworks but does NOT auto-link them — we have
    # to pull them in ourselves, or the link fails with
    # "Undefined symbols: _sws_freeContext, ...".
    set(_QT_IOS_FFMPEG_DIR "$ENV{HOME}/Qt/6.11.0/ios/lib/ffmpeg")
    foreach(_lib avcodec avformat avutil swresample swscale)
        target_link_libraries(plyr_qt PRIVATE
            "${_QT_IOS_FFMPEG_DIR}/lib${_lib}.xcframework/ios-arm64/lib${_lib}.framework/lib${_lib}"
        )
    endforeach()
endif()
