#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>

class ScreenRecorder {
public:
    ScreenRecorder();
    ~ScreenRecorder();

    // Legacy video-only start (fallback path)
    bool startRecording(const juce::File& outputFile);

    // Combined audio+video single-writer start (preferred)
    bool startCombined(const juce::File& outputFile, double sampleRate, int numChannels);

    // Live streaming: start capture without writing to file (ScreenCaptureKit only)
    bool startStreamOnly();

    void stop();
    bool isRecording() const;

    // Feed audio from processBlock when combined is active
    void pushAudio(const juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate, int numChannels);

    // Set frame callback for live streaming (called on SCK sample handler queue)
    void setFrameCallback(std::function<void(void* cvPixelBufferRef, int64_t ptsMs)> cb);

    // Set desired capture resolution (width x height)
    void setCaptureResolution(int width, int height);

    juce::File getLastRecordedFile() const { return lastRecordedFile; }

private:
    juce::File lastRecordedFile;

    struct Impl;
    std::unique_ptr<Impl> impl;
};
