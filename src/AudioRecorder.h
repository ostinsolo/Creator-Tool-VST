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
    // Writer to disk (JUCE-provided background writer)
    juce::TimeSliceThread writerThread { "Audio Recorder Writer Thread" };
    juce::CriticalSection writerLock;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    std::unique_ptr<juce::FileOutputStream> fileStream;

    // Lock-free ring buffer between audio thread and drain thread
    std::unique_ptr<juce::AbstractFifo> fifo;
    juce::AudioBuffer<float> fifoBuffer;
    int fifoCapacity = 0;
    int fifoNumChannels = 0;

    // Drain thread moves from ring buffer -> ThreadedWriter (non-audio thread)
    struct DrainThread : public juce::Thread {
        AudioRecorder& owner;
        explicit DrainThread(AudioRecorder& o) : juce::Thread("Audio Recorder FIFO Drain"), owner(o) {}
        void run() override;
    };
    std::unique_ptr<DrainThread> drainThread;

    std::atomic<bool> isRecordingAtomic { false };
    std::atomic<int> droppedSamples { 0 };
    double currentSampleRate { 44100.0 };

    void startDrainThread();
    void stopDrainThread();
    void drainOnce();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioRecorder)
};
