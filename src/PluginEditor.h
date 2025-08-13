#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_core/juce_core.h>

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

    juce::TextButton screenRecordButton { "Screen Rec" };
    juce::TextButton screenStopButton { "Screen Stop" };

    juce::Label folderLabel;
    juce::Label statusLabel;

    void updateButtons();
    void updateFolderLabel();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CreatorToolVSTAudioProcessorEditor)
};
