#include "PluginEditor.h"
#include "PluginProcessor.h"

static juce::String makeTimestampedFilename(const juce::String& ext) {
    auto now = juce::Time::getCurrentTime();
    return now.formatted("yyyy-MM-dd_HH-mm-ss") + "." + ext;
}

CreatorToolVSTAudioProcessorEditor::CreatorToolVSTAudioProcessorEditor(CreatorToolVSTAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), processor(p)
{
    setSize(560, 240);

    addAndMakeVisible(recordButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(chooseFolderButton);
    addAndMakeVisible(previewButton);

    addAndMakeVisible(screenRecordButton);
    addAndMakeVisible(screenStopButton);

    addAndMakeVisible(folderLabel);
    addAndMakeVisible(statusLabel);

    recordButton.onClick = [this]() { buttonClicked(&recordButton); };
    stopButton.onClick = [this]() { buttonClicked(&stopButton); };
    chooseFolderButton.onClick = [this]() { buttonClicked(&chooseFolderButton); };
    previewButton.onClick = [this]() { buttonClicked(&previewButton); };

    screenRecordButton.onClick = [this]() { buttonClicked(&screenRecordButton); };
    screenStopButton.onClick = [this]() { buttonClicked(&screenStopButton); };

    folderLabel.setJustificationType(juce::Justification::centred);
    statusLabel.setJustificationType(juce::Justification::centred);

    updateButtons();
    updateFolderLabel();
}

CreatorToolVSTAudioProcessorEditor::~CreatorToolVSTAudioProcessorEditor() = default;

void CreatorToolVSTAudioProcessorEditor::paint(juce::Graphics& g) {
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
    g.setColour(juce::Colours::white);
    g.setFont(16.0f);
    g.drawFittedText("Creator Tool VST — Audio+Screen Recorder", getLocalBounds().removeFromTop(24), juce::Justification::centred, 1);
}

void CreatorToolVSTAudioProcessorEditor::resized() {
    auto area = getLocalBounds().reduced(12);
    area.removeFromTop(28);

    auto buttonsRow = area.removeFromTop(40);
    recordButton.setBounds(buttonsRow.removeFromLeft(100).reduced(4));
    stopButton.setBounds(buttonsRow.removeFromLeft(100).reduced(4));
    previewButton.setBounds(buttonsRow.removeFromLeft(120).reduced(4));

    auto screenRow = area.removeFromTop(40);
    screenRecordButton.setBounds(screenRow.removeFromLeft(120).reduced(4));
    screenStopButton.setBounds(screenRow.removeFromLeft(120).reduced(4));

    area.removeFromTop(10);

    chooseFolderButton.setBounds(area.removeFromTop(36).removeFromLeft(160).reduced(4));

    folderLabel.setBounds(area.removeFromTop(32));
    statusLabel.setBounds(area.removeFromTop(32));
}

void CreatorToolVSTAudioProcessorEditor::buttonClicked(juce::Button* button) {
    if (button == &chooseFolderButton) {
        juce::FileChooser chooser("Choose destination folder", processor.getDestinationDirectory(), juce::String(), true);
        if (chooser.browseForDirectory()) {
            processor.setDestinationDirectory(chooser.getResult());
            updateFolderLabel();
        }
        return;
    }

    if (button == &recordButton) {
        auto dir = processor.getDestinationDirectory();
        if (! dir.exists()) dir.createDirectory();
        auto target = dir.getChildFile("Recording-" + makeTimestampedFilename("wav"));
        if (processor.startRecordingToFile(target)) {
            statusLabel.setText("Recording audio…", juce::dontSendNotification);
        } else {
            statusLabel.setText("Failed to start audio recording", juce::dontSendNotification);
        }
        updateButtons();
        return;
    }

    if (button == &stopButton) {
        processor.stopRecording();
        statusLabel.setText("Audio stopped.", juce::dontSendNotification);
        updateButtons();
        return;
    }

    if (button == &previewButton) {
        auto last = processor.getLastRecordedFile();
        if (last.existsAsFile()) juce::URL(last).launchInDefaultBrowser();
        else statusLabel.setText("No recording to preview", juce::dontSendNotification);
        return;
    }

    if (button == &screenRecordButton) {
       #if JUCE_MAC
        auto dir = processor.getDestinationDirectory();
        if (! dir.exists()) dir.createDirectory();
        auto target = dir.getChildFile("Screen-" + makeTimestampedFilename("mov"));
        if (processor.startScreenRecording(target)) {
            statusLabel.setText("Screen recording…", juce::dontSendNotification);
        } else {
            statusLabel.setText("Failed to start screen recording", juce::dontSendNotification);
        }
       #else
        statusLabel.setText("Screen recording not supported on this platform", juce::dontSendNotification);
       #endif
        updateButtons();
        return;
    }

    if (button == &screenStopButton) {
        processor.stopScreenRecording();
        statusLabel.setText("Screen stopped.", juce::dontSendNotification);
        updateButtons();
        return;
    }
}

void CreatorToolVSTAudioProcessorEditor::updateButtons() {
    const bool isRec = processor.isRecording();
    recordButton.setEnabled(! isRec);
    stopButton.setEnabled(isRec);

    #if JUCE_MAC
    const bool isScreenRec = processor.isScreenRecording();
    screenRecordButton.setEnabled(! isScreenRec);
    screenStopButton.setEnabled(isScreenRec);
    #else
    screenRecordButton.setEnabled(false);
    screenStopButton.setEnabled(false);
    #endif

    previewButton.setEnabled(processor.getLastRecordedFile().existsAsFile());
}

void CreatorToolVSTAudioProcessorEditor::updateFolderLabel() {
    auto dir = processor.getDestinationDirectory();
    folderLabel.setText("Folder: " + dir.getFullPathName(), juce::dontSendNotification);
}
