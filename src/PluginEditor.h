#pragma once
#include <JuceHeader.h>

class CreatorToolVSTAudioProcessor;

class CreatorToolVSTAudioProcessorEditor : public juce::AudioProcessorEditor,
                                           public juce::Button::Listener {
public:
    explicit CreatorToolVSTAudioProcessorEditor(CreatorToolVSTAudioProcessor&);
    ~CreatorToolVSTAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void buttonClicked(juce::Button* button) override;

private:
    CreatorToolVSTAudioProcessor& processor;

    juce::TextButton recordButton { "Record" };
    juce::TextButton stopButton { "Stop" };
    juce::TextButton chooseFolderButton { "Choose Folder" };
    juce::TextButton previewButton { "Preview Last" };

    juce::Label folderLabel;
    juce::Label statusLabel;

    void updateButtons();
    void updateFolderLabel();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CreatorToolVSTAudioProcessorEditor)
};
