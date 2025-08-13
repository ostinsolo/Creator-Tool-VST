#pragma once
#include <JuceHeader.h>
#include "AudioRecorder.h"

class CreatorToolVSTAudioProcessor : public juce::AudioProcessor {
public:
    CreatorToolVSTAudioProcessor();
    ~CreatorToolVSTAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override { return "Creator Tool VST"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Recording controls
    bool startRecordingToFile(const juce::File& file);
    void stopRecording();
    bool isRecording() const { return audioRecorder.isRecording(); }

    void setDestinationDirectory(const juce::File& dir);
    juce::File getDestinationDirectory() const { return destinationDirectory; }
    juce::File getLastRecordedFile() const { return lastRecordedFile; }

private:
    AudioRecorder audioRecorder;
    juce::File destinationDirectory;
    juce::File lastRecordedFile;
    double currentSampleRate { 44100.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CreatorToolVSTAudioProcessor)
};
