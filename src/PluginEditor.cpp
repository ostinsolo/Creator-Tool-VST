#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "MuxUtils.h"
#include "Logging.h"
#include <chrono>
#include <ctime>

static juce::String makeTimestampedFilename(const juce::String& ext) {
    using namespace std::chrono;
    auto tp = system_clock::now();
    auto t  = system_clock::to_time_t(tp);
    std::tm tmval{};
   #if JUCE_WINDOWS
    localtime_s(&tmval, &t);
   #else
    localtime_r(&t, &tmval);
   #endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tmval);
    auto ms = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;
    char finalBuf[48];
    std::snprintf(finalBuf, sizeof(finalBuf), "%s_%03d.%s", buf, (int) ms.count(), ext.toRawUTF8());
    return juce::String(finalBuf);
}

CreatorToolVSTAudioProcessorEditor::CreatorToolVSTAudioProcessorEditor(CreatorToolVSTAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), processor(p)
{
    setSize(560, 450);

    addAndMakeVisible(recordButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(chooseFolderButton);
    addAndMakeVisible(previewButton);

    addAndMakeVisible(screenRecordButton);
    addAndMakeVisible(screenStopButton);

    addAndMakeVisible(bothRecordButton);
    addAndMakeVisible(bothStopButton);

    resolutionBox.addItem("Auto", 1);
    resolutionBox.addItem("1280 x 720", 2);
    resolutionBox.addItem("1920 x 1080", 3);
    resolutionBox.addItem("2560 x 1440", 4);
    resolutionBox.addItem("3840 x 2160", 5);
    resolutionBox.setSelectedId(1, juce::dontSendNotification);
    resolutionBox.addListener(this);
    addAndMakeVisible(resolutionBox);

    formatBox.addItem("MOV", 1);
    formatBox.addItem("MP4", 2);
    formatBox.setSelectedId(1, juce::dontSendNotification);
    formatBox.addListener(this);
    addAndMakeVisible(formatBox);

    addAndMakeVisible(folderLabel);
    addAndMakeVisible(statusLabel);

    addAndMakeVisible(video);

    recordButton.onClick = [this]() { buttonClicked(&recordButton); };
    stopButton.onClick = [this]() { buttonClicked(&stopButton); };
    chooseFolderButton.onClick = [this]() { buttonClicked(&chooseFolderButton); };
    previewButton.onClick = [this]() { buttonClicked(&previewButton); };

    screenRecordButton.onClick = [this]() { buttonClicked(&screenRecordButton); };
    screenStopButton.onClick = [this]() { buttonClicked(&screenStopButton); };

    bothRecordButton.onClick = [this]() { buttonClicked(&bothRecordButton); };
    bothStopButton.onClick = [this]() { buttonClicked(&bothStopButton); };

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

    auto buttonsRow = area.removeFromTop(36);
    recordButton.setBounds(buttonsRow.removeFromLeft(90).reduced(2));
    stopButton.setBounds(buttonsRow.removeFromLeft(90).reduced(2));
    previewButton.setBounds(buttonsRow.removeFromLeft(110).reduced(2));

    auto screenRow = area.removeFromTop(36);
    screenRecordButton.setBounds(screenRow.removeFromLeft(110).reduced(2));
    screenStopButton.setBounds(screenRow.removeFromLeft(110).reduced(2));

    auto bothRow = area.removeFromTop(36);
    bothRecordButton.setBounds(bothRow.removeFromLeft(110).reduced(2));
    bothStopButton.setBounds(bothRow.removeFromLeft(110).reduced(2));

    auto optsRow = area.removeFromTop(36);
    resolutionBox.setBounds(optsRow.removeFromLeft(180).reduced(2));
    formatBox.setBounds(optsRow.removeFromLeft(100).reduced(2));

    area.removeFromTop(6);

    chooseFolderButton.setBounds(area.removeFromTop(36).removeFromLeft(160).reduced(4));

    folderLabel.setBounds(area.removeFromTop(24));
    statusLabel.setBounds(area.removeFromTop(24));

    video.setBounds(area.removeFromTop(160));
}

void CreatorToolVSTAudioProcessorEditor::comboBoxChanged(juce::ComboBox* box) {
    if (box == &resolutionBox) {
        switch (resolutionBox.getSelectedId()) {
            case 1: processor.setCaptureResolution(0, 0); break; // auto
            case 2: processor.setCaptureResolution(1280, 720); break;
            case 3: processor.setCaptureResolution(1920, 1080); break;
            case 4: processor.setCaptureResolution(2560, 1440); break;
            case 5: processor.setCaptureResolution(3840, 2160); break;
            default: break;
        }
        LogMessage("UI: resolution changed -> id=" + juce::String(resolutionBox.getSelectedId()));
        return;
    }
    if (box == &formatBox) {
        LogMessage("UI: container changed -> " + formatBox.getText());
        return;
    }
}

void CreatorToolVSTAudioProcessorEditor::buttonClicked(juce::Button* button) {
    if (button == &chooseFolderButton) {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Choose destination folder",
            processor.getDestinationDirectory(),
            juce::String(), true);

        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this, chooser](const juce::FileChooser& fc) {
                juce::File result = fc.getResult();
                if (result.isDirectory()) {
                    processor.setDestinationDirectory(result);
                    updateFolderLabel();
                    LogMessage("UI: destination set -> " + result.getFullPathName());
                }
            });
        return;
    }

    if (button == &recordButton) {
        auto dir = processor.getDestinationDirectory();
        if (! dir.exists()) dir.createDirectory();
        auto target = dir.getChildFile("Recording-" + makeTimestampedFilename("wav"));
        if (processor.startRecordingToFile(target)) {
            statusLabel.setText("Recording audio…", juce::dontSendNotification);
            LogMessage("UI: audio record start -> " + target.getFileName());
        } else {
            statusLabel.setText("Failed to start audio recording", juce::dontSendNotification);
        }
        updateButtons();
        return;
    }

    if (button == &stopButton) {
        processor.stopRecording();
        statusLabel.setText("Audio stopped.", juce::dontSendNotification);
        LogMessage("UI: audio record stop");
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
            LogMessage("UI: screen record start -> " + target.getFileName());
            video.closeVideo();
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
        LogMessage("UI: screen record stop");
        updateButtons();
        return;
    }

    if (button == &bothRecordButton) {
       #if JUCE_MAC
        auto dir = processor.getDestinationDirectory();
        if (! dir.exists()) dir.createDirectory();
        const bool wantMp4 = (formatBox.getSelectedId() == 2);
        auto out = dir.getChildFile("AV-" + makeTimestampedFilename(wantMp4 ? "mp4" : "mov"));
        if (processor.startCombinedRecording(out)) {
            statusLabel.setText("Recording A+V…", juce::dontSendNotification);
            LogMessage("UI: A+V record start -> " + out.getFileName());
            video.closeVideo();
        } else {
            statusLabel.setText("Failed to start A+V", juce::dontSendNotification);
        }
       #else
        statusLabel.setText("A+V not supported on this platform", juce::dontSendNotification);
       #endif
        updateButtons();
        return;
    }

    if (button == &bothStopButton) {
       #if JUCE_MAC
        processor.stopCombinedRecording();
        statusLabel.setText("Stopped A+V.", juce::dontSendNotification);
        LogMessage("UI: A+V record stop");
       #else
        statusLabel.setText("A+V not supported on this platform", juce::dontSendNotification);
       #endif
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
    bothRecordButton.setEnabled(! isScreenRec);
    bothStopButton.setEnabled(isScreenRec);
    #else
    screenRecordButton.setEnabled(false);
    screenStopButton.setEnabled(false);
    bothRecordButton.setEnabled(false);
    bothStopButton.setEnabled(false);
    #endif

    previewButton.setEnabled(processor.getLastRecordedFile().existsAsFile());
}

void CreatorToolVSTAudioProcessorEditor::updateFolderLabel() {
    auto dir = processor.getDestinationDirectory();
    folderLabel.setText("Folder: " + dir.getFullPathName(), juce::dontSendNotification);
}
