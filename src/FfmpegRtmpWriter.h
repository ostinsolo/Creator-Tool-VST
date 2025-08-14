#pragma once

#include <juce_core/juce_core.h>
#include "StreamingConfig.h"
#include "Logging.h"

// Define HAVE_FFMPEG at build time if libavformat/libavutil/libavcodec are available

class FfmpegRtmpWriter {
public:
    FfmpegRtmpWriter();
    ~FfmpegRtmpWriter();

    // url: rtmp/rtmps URL; cfg provides timing/bitrate (we pass-through streams)
    bool open(const juce::String& url, const StreamingConfig& cfg);

    // Provide codec config (SPS/PPS for H.264; AudioSpecificConfig for AAC)
    bool setVideoConfig(const void* data, size_t size);
    bool setAudioConfig(const void* data, size_t size);

    // Push encoded frames (timestamps in ms). Data: Annex B H.264 for video, raw AAC without ADTS for audio
    bool writeVideoFrame(const void* data, size_t size, int64_t ptsMs, bool keyframe);
    bool writeAudioFrame(const void* data, size_t size, int64_t ptsMs);

    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
