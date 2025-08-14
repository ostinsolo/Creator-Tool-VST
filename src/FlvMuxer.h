#pragma once
#include <juce_core/juce_core.h>
#include "StreamingConfig.h"

namespace streaming {

struct EncodedAudioFrame {
    juce::MemoryBlock data; // AAC raw or ADTS-less payload
    juce::int64 timestampMs { 0 };
    bool isConfig { false }; // true if this carries AudioSpecificConfig
};

struct EncodedVideoFrame {
    juce::MemoryBlock data; // H.264 Annex B (with SPS/PPS for IDR)
    juce::int64 timestampMs { 0 };
    bool isKeyframe { false };
};

class FlvMuxer {
public:
    FlvMuxer() = default;
    ~FlvMuxer() = default;

    // Initialize FLV header and write AAC/H.264 codec configs
    void start(const StreamingConfig& cfg);

    // Mux frames into internal buffer; returns a chunk ready to send
    bool pushAudio(const EncodedAudioFrame& aacFrame, juce::MemoryBlock& outChunk);
    bool pushVideo(const EncodedVideoFrame& h264Frame, juce::MemoryBlock& outChunk);

    void reset();
};

} // namespace streaming
