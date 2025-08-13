#include "PluginProcessor.h"
#include "PluginEditor.h"

CreatorToolVSTAudioProcessor::CreatorToolVSTAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    destinationDirectory = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
        .getChildFile("CreatorTool Recordings");
}

CreatorToolVSTAudioProcessor::~CreatorToolVSTAudioProcessor() = default;

void CreatorToolVSTAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/) {
    currentSampleRate = sampleRate;
    audioRecorder.prepare(sampleRate);
}

void CreatorToolVSTAudioProcessor::releaseResources() {
    audioRecorder.stop();
}

bool CreatorToolVSTAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto& mainIn  = layouts.getMainInputChannelSet();
    const auto& mainOut = layouts.getMainOutputChannelSet();

    if (mainIn.isDisabled() || mainOut.isDisabled())
        return false;

    if (mainIn.size() != mainOut.size())
        return false;

    if (mainIn.size() != 1 && mainIn.size() != 2)
        return false;

    return true;
}

void CreatorToolVSTAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) {
    juce::ScopedNoDenormals noDenormals;

    // Pass-through
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    if (audioRecorder.isRecording())
        audioRecorder.pushBuffer(buffer, buffer.getNumSamples());
}

juce::AudioProcessorEditor* CreatorToolVSTAudioProcessor::createEditor() {
    return new CreatorToolVSTAudioProcessorEditor(*this);
}

bool CreatorToolVSTAudioProcessor::hasEditor() const { return true; }

void CreatorToolVSTAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    juce::ValueTree state("state");
    state.setProperty("destination", destinationDirectory.getFullPathName(), nullptr);
    state.setProperty("lastFile", lastRecordedFile.getFullPathName(), nullptr);
    juce::MemoryOutputStream mos(destData, false);
    state.writeToStream(mos);
}

void CreatorToolVSTAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto state = juce::ValueTree::readFromData(data, (size_t) sizeInBytes);
    if (state.isValid()) {
        auto dest = juce::File(state.getProperty("destination").toString());
        if (dest.exists() || dest.getParentDirectory().exists())
            destinationDirectory = dest;

        auto last = juce::File(state.getProperty("lastFile").toString());
        if (last.existsAsFile())
            lastRecordedFile = last;
    }
}

bool CreatorToolVSTAudioProcessor::startRecordingToFile(const juce::File& file) {
    if (currentSampleRate <= 0)
        return false;

    bool ok = audioRecorder.startRecording(file, getTotalNumInputChannels(), currentSampleRate);
    if (ok)
        lastRecordedFile = file;
    return ok;
}

void CreatorToolVSTAudioProcessor::stopRecording() {
    audioRecorder.stop();
}

void CreatorToolVSTAudioProcessor::setDestinationDirectory(const juce::File& dir) {
    destinationDirectory = dir;
}
