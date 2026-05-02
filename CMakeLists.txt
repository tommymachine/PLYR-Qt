cmake_minimum_required(VERSION 3.21)
project(plyr_qt VERSION 0.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

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
)

# macOS-only NSOpenPanel prewarm. Compile the Objective-C++ source and
# link AppKit. iOS uses UIDocumentPickerViewController, not NSOpenPanel,
# so this file is excluded there.
if(APPLE AND NOT IOS)
    target_sources(plyr_qt PRIVATE src/macos_prewarm.mm)
    target_link_libraries(plyr_qt PRIVATE "-framework AppKit")
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
