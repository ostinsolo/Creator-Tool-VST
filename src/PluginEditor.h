#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_video/juce_video.h>
#include <juce_core/juce_core.h>

class CreatorToolVSTAudioProcessor;

class CreatorToolVSTAudioProcessorEditor : public juce::AudioProcessorEditor,
                                           public juce::Button::Listener,
                                           public juce::ComboBox::Listener {
public:
    explicit CreatorToolVSTAudioProcessorEditor(CreatorToolVSTAudioProcessor&);
    ~CreatorToolVSTAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void buttonClicked(juce::Button* button) override;
    void comboBoxChanged(juce::ComboBox* box) override;

private:
    CreatorToolVSTAudioProcessor& processor;

    juce::TextButton recordButton { "Record" };
    juce::TextButton stopButton { "Stop" };
    juce::TextButton chooseFolderButton { "Choose Folder" };
    juce::TextButton previewButton { "Preview Last" };

    juce::TextButton screenRecordButton { "Screen Rec" };
    juce::TextButton screenStopButton { "Screen Stop" };

    juce::TextButton bothRecordButton { "Record A+V" };
    juce::TextButton bothStopButton { "Stop A+V" };

    juce::ComboBox resolutionBox;
    juce::ComboBox formatBox; // MOV/MP4

    // Live streaming controls
    juce::TextEditor rtmpUrlEdit;
    juce::TextButton goLiveButton { "Go Live" };
    juce::TextButton stopLiveButton { "Stop Live" };

    juce::Label folderLabel;
    juce::Label statusLabel;

    juce::VideoComponent video { true };

    void updateButtons();
    void updateFolderLabel();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CreatorToolVSTAudioProcessorEditor)
};
