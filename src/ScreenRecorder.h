#pragma once
#include <juce_core/juce_core.h>

class ScreenRecorder {
public:
    ScreenRecorder();
    ~ScreenRecorder();

    bool startRecording(const juce::File& outputFile);
    void stop();
    bool isRecording() const;

    juce::File getLastRecordedFile() const { return lastRecordedFile; }

private:
    juce::File lastRecordedFile;

    struct Impl;
    std::unique_ptr<Impl> impl;
};
