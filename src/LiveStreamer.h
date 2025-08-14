#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "StreamingConfig.h"
#include "FlvMuxer.h"
#include "RtmpClient.h"

namespace streaming {

class LiveStreamer {
public:
    LiveStreamer();
    ~LiveStreamer();

    bool start(const StreamingConfig& cfg);
    void stop();

    // Audio: push PCM from the audio thread (non-blocking)
    void pushAudioPCM(const juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate, int numChannels);

    // Video frame bridge: from ScreenRecorder (CVPixelBufferRef + ms pts)
    void pushPixelBuffer(void* cvPixelBufferRef, int64_t ptsMs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace streaming
