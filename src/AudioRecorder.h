#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

class AudioRecorder {
public:
    AudioRecorder();
    ~AudioRecorder();

    void prepare(double sampleRate);
    bool startRecording(const juce::File& file, int numChannels, double sampleRate);
    void stop();
    bool isRecording() const { return isRecordingAtomic.load(); }

    void pushBuffer(const juce::AudioBuffer<float>& buffer, int numSamples);

private:
    juce::TimeSliceThread writerThread { "Audio Recorder Writer Thread" };
    juce::CriticalSection writerLock;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    std::unique_ptr<juce::FileOutputStream> fileStream;
    std::atomic<bool> isRecordingAtomic { false };
    double currentSampleRate { 44100.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioRecorder)
};
