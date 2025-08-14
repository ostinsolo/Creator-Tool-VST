cmake_minimum_required(VERSION 3.21)
project(CreatorToolVST VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer interprocedural optimization for Release if available
cmake_policy(SET CMP0069 NEW)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

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
    # Resolve CoreVideo path explicitly for reliable linking
    find_library(COREVIDEO_FRAMEWORK CoreVideo)
    list(APPEND PLUGIN_SOURCES src/ScreenRecorder.mm)
endif()

target_sources(CreatorToolVST PRIVATE ${PLUGIN_SOURCES})
target_sources(CreatorToolVST PRIVATE
    src/StreamingConfig.h
    src/FlvMuxer.h
    src/RtmpClient.h
    src/LiveStreamer.h
    src/LiveStreamer.mm
    src/FfmpegRtmpWriter.h
)

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

# Platform-specific compile/link options
if(MSVC)
    # Windows optimization flags
    target_compile_options(CreatorToolVST PRIVATE
        $<$<CONFIG:Release>:/O2 /GL /Gw /Gy>
        $<$<CONFIG:Debug>:/Zi /Od>
    )
    target_link_options(CreatorToolVST PRIVATE
        $<$<CONFIG:Release>:/LTCG>
    )
else()
    # macOS/Linux
    target_compile_options(CreatorToolVST PRIVATE
        $<$<CONFIG:Release>:-O3 -fvisibility=hidden -fvisibility-inlines-hidden>
        $<$<CONFIG:Debug>:-O0 -g>
    )
    # Hide symbols by default; JUCE exports plugin entry points
    set_target_properties(CreatorToolVST PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endif()

if(APPLE)
    # Ensure Objective-C++ with ARC for the ObjC++ sources
    set_source_files_properties(src/ScreenRecorder.mm PROPERTIES LANGUAGE OBJCXX)
    set_source_files_properties(src/ScreenRecorder.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    set_source_files_properties(src/MuxUtils.mm PROPERTIES LANGUAGE OBJCXX)
    set_source_files_properties(src/MuxUtils.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    # Link required frameworks; ScreenCaptureKit provided since macOS 12.3
    target_link_libraries(CreatorToolVST PRIVATE "-framework AVFoundation" "-framework AppKit" "-framework CoreMedia" "-framework AVKit" "-framework ScreenCaptureKit" "-framework VideoToolbox" "-framework AudioToolbox" ${COREVIDEO_FRAMEWORK})

    # Optional FFmpeg (Pro streaming). Set HAVE_FFMPEG if found.
    find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h 
        PATHS /usr/local/include /opt/homebrew/include ${CMAKE_SOURCE_DIR}/external/ffmpeg/include)
    find_library(AVFORMAT_LIBRARY avformat 
        PATHS /usr/local/lib /opt/homebrew/lib ${CMAKE_SOURCE_DIR}/external/ffmpeg/lib)
    find_library(AVUTIL_LIBRARY avutil 
        PATHS /usr/local/lib /opt/homebrew/lib ${CMAKE_SOURCE_DIR}/external/ffmpeg/lib)
    find_library(AVCODEC_LIBRARY avcodec 
        PATHS /usr/local/lib /opt/homebrew/lib ${CMAKE_SOURCE_DIR}/external/ffmpeg/lib)
    if (FFMPEG_INCLUDE_DIR AND AVFORMAT_LIBRARY AND AVUTIL_LIBRARY AND AVCODEC_LIBRARY)
        target_compile_definitions(CreatorToolVST PRIVATE HAVE_FFMPEG=1)
        target_include_directories(CreatorToolVST PRIVATE ${FFMPEG_INCLUDE_DIR})
        target_link_libraries(CreatorToolVST PRIVATE ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY} ${AVCODEC_LIBRARY})
        message(STATUS "FFmpeg found; enabling Pro RTMP writer")

        # Standalone CLI test app that reuses capture/streaming
        add_executable(StreamerTest
            src/LiveStreamer.h
            src/LiveStreamer.mm
            src/FfmpegRtmpWriter.h
            src/FfmpegRtmpWriter.cpp
            src/ScreenRecorder.h
            src/ScreenRecorder.mm
            src/Logging.h
            tools/StreamerTest.cpp
        )
        target_compile_definitions(StreamerTest PRIVATE HAVE_FFMPEG=1)
        target_include_directories(StreamerTest PRIVATE ${FFMPEG_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/modules ${CMAKE_CURRENT_BINARY_DIR}/Release/include)
        set_source_files_properties(src/ScreenRecorder.mm PROPERTIES LANGUAGE OBJCXX)
        set_source_files_properties(src/ScreenRecorder.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(StreamerTest PRIVATE ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY} ${AVCODEC_LIBRARY})
        target_link_libraries(StreamerTest PRIVATE "-framework AVFoundation" "-framework AppKit" "-framework CoreMedia" "-framework AVKit" "-framework ScreenCaptureKit" "-framework VideoToolbox" "-framework AudioToolbox" ${COREVIDEO_FRAMEWORK} juce::juce_core juce::juce_events)
        target_compile_options(StreamerTest PRIVATE $<$<CONFIG:Release>:-O3> $<$<CONFIG:Debug>:-O0 -g>)
    else()
        message(STATUS "FFmpeg not found; StreamerTest will not be built")
    endif()
endif()
