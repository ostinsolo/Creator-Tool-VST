cmake_minimum_required(VERSION 3.21)
project(CreatorToolVST VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Require a local JUCE checkout
set(JUCE_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE")
if(NOT EXISTS "${JUCE_LOCAL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at external/JUCE. Please init/update the submodule: git submodule update --init --recursive")
endif()
message(STATUS "Using local JUCE from external/JUCE")
add_subdirectory(${JUCE_LOCAL_DIR} ${CMAKE_BINARY_DIR}/juce)

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

# Add Objective-C++ files for macOS screen recorder and mux
if(APPLE)
    list(APPEND PLUGIN_SOURCES src/ScreenRecorder.mm)
endif()

target_sources(CreatorToolVST PRIVATE ${PLUGIN_SOURCES})

if(APPLE)
    # Explicitly add MuxUtils.mm to avoid glob issues
    target_sources(CreatorToolVST PRIVATE src/MuxUtils.mm)
endif()

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
    juce::juce_video
    juce::juce_dsp
)

if(APPLE)
    # Ensure Objective-C++ with ARC for the ObjC++ sources
    set_source_files_properties(src/ScreenRecorder.mm PROPERTIES LANGUAGE OBJCXX)
    set_source_files_properties(src/ScreenRecorder.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    set_source_files_properties(src/MuxUtils.mm PROPERTIES LANGUAGE OBJCXX)
    set_source_files_properties(src/MuxUtils.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    # Link required frameworks; ScreenCaptureKit provided since macOS 12.3
    target_link_libraries(CreatorToolVST PRIVATE "-framework AVFoundation" "-framework AppKit" "-framework CoreMedia" "-framework AVKit" "-framework ScreenCaptureKit")
endif()
