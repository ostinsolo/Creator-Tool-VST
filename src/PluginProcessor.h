#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_core/juce_core.h>
#include "AudioRecorder.h"
#include "ScreenRecorder.h"
#include "StreamingConfig.h"
#include "LiveStreamer.h"

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

    // Audio-only recording
    bool startRecordingToFile(const juce::File& file);
    void stopRecording();
    bool isRecording() const { return audioRecorder.isRecording(); }

    // Video-only (legacy) and combined A+V
    bool startScreenRecording(const juce::File& file) { return screenRecorder.startRecording(file); }
    bool startCombinedRecording(const juce::File& file) { return screenRecorder.startCombined(file, currentSampleRate, getTotalNumInputChannels()); }
    void stopScreenRecording() { screenRecorder.stop(); }
    void stopCombinedRecording() { screenRecorder.stop(); }
    bool isScreenRecording() const { return screenRecorder.isRecording(); }

    // Live streaming
    bool startLiveStreaming(const StreamingConfig& cfg);
    void stopLiveStreaming();
    bool isLiveStreaming() const { return liveActive; }

    // Capture options
    void setCaptureResolution(int width, int height) { screenRecorder.setCaptureResolution(width, height); }

    void setDestinationDirectory(const juce::File& dir);
    juce::File getDestinationDirectory() const { return destinationDirectory; }
    juce::File getLastRecordedFile() const { return lastRecordedFile; }

private:
    AudioRecorder audioRecorder;
    ScreenRecorder screenRecorder;
    std::unique_ptr<streaming::LiveStreamer> liveStreamer;
    StreamingConfig liveCfg;
    bool liveActive { false };

    juce::File destinationDirectory;
    juce::File lastRecordedFile;
    double currentSampleRate { 44100.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CreatorToolVSTAudioProcessor)
};
