cmake_minimum_required(VERSION 3.21)
project(CreatorToolVST VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer a local JUCE checkout if available
set(JUCE_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE")
if(EXISTS "${JUCE_LOCAL_DIR}/CMakeLists.txt")
    message(STATUS "Using local JUCE from external/JUCE")
    add_subdirectory(${JUCE_LOCAL_DIR} ${CMAKE_BINARY_DIR}/juce)
else()
    include(FetchContent)
    FetchContent_Declare(
        juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.8
    )
    FetchContent_MakeAvailable(juce)
endif()

juce_add_plugin(CreatorToolVST
    COMPANY_NAME "CreatorTool"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE CrTl
    PLUGIN_CODE CTVS
    FORMATS VST3
    PRODUCT_NAME "Creator Tool VST"
)

# Generate JuceHeader.h for this target (harmless to keep even without including it)
juce_generate_juce_header(CreatorToolVST)

file(GLOB_RECURSE PLUGIN_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
)

# Add Objective-C++ file for macOS screen recorder
if(APPLE)
    list(APPEND PLUGIN_SOURCES src/ScreenRecorder.mm)
endif()

target_sources(CreatorToolVST PRIVATE ${PLUGIN_SOURCES})

# Definitions and JUCE modules

target_compile_definitions(CreatorToolVST PRIVATE
    JUCE_WEB_BROWSER=1
    JUCE_USE_FREETYPE=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(CreatorToolVST PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_dsp
)

if(APPLE)
    # Ensure Objective-C++ with ARC for the screen recorder
    set_source_files_properties(src/ScreenRecorder.mm PROPERTIES LANGUAGE OBJCXX)
    set_source_files_properties(src/ScreenRecorder.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(CreatorToolVST PRIVATE "-framework AVFoundation" "-framework AppKit" "-framework CoreMedia")
endif()
