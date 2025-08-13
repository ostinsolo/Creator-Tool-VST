cmake_minimum_required(VERSION 3.21)
project(CreatorToolVST VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 7.0.11
)
FetchContent_MakeAvailable(juce)

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

file(GLOB_RECURSE PLUGIN_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
)

target_sources(CreatorToolVST PRIVATE ${PLUGIN_SOURCES})

# Definitions and JUCE modules

target_compile_definitions(CreatorToolVST PRIVATE
    JUCE_WEB_BROWSER=1
    JUCE_USE_FREETYPE=1
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
    # Frameworks potentially useful for preview/launching external players
    target_link_libraries(CreatorToolVST PRIVATE "-framework AVFoundation" "-framework CoreMedia")
endif()
