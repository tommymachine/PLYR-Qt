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
        qml/PLYRSeekSlider.qml
        qml/MarqueeText.qml
    SOURCES
        src/FlacTags.cpp
        src/FlacTags.h
        src/Track.h
        src/PlaylistModel.cpp
        src/PlaylistModel.h
        src/FftProcessor.cpp
        src/FftProcessor.h
)

# Standalone CLI for testing the FLAC parser in isolation.
qt_add_executable(flactest
    src/flactest_main.cpp
    src/FlacTags.cpp
    src/FlacTags.h
)
target_link_libraries(flactest PRIVATE Qt6::Core)
set_target_properties(flactest PROPERTIES MACOSX_BUNDLE FALSE)

target_link_libraries(plyr_qt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Multimedia
    kissfft
)

set_target_properties(plyr_qt PROPERTIES
    MACOSX_BUNDLE                TRUE
    MACOSX_BUNDLE_BUNDLE_NAME    "PLYR Qt"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.thompson.plyr-qt"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
)
