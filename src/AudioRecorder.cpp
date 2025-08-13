#include "AudioRecorder.h"

AudioRecorder::AudioRecorder() {
    writerThread.startThread();
}

AudioRecorder::~AudioRecorder() {
    stop();
    writerThread.stopThread(2000);
}

void AudioRecorder::prepare(double sampleRate) {
    currentSampleRate = sampleRate;
}

bool AudioRecorder::startRecording(const juce::File& file, int numChannels, double sampleRate) {
    stop();

    auto parentDir = file.getParentDirectory();
    if (! parentDir.exists())
        parentDir.createDirectory();

    file.deleteFile();
    fileStream.reset(file.createOutputStream());
    if (fileStream == nullptr)
        return false;

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(fileStream.get(), sampleRate, (unsigned int) numChannels, 24, {}, 0));

    if (writer.get() == nullptr)
        return false;

    fileStream.release(); // writer now owns the stream

    const juce::ScopedLock sl(writerLock);
    threadedWriter.reset(new juce::AudioFormatWriter::ThreadedWriter(writer.release(), writerThread, 32768));
    isRecordingAtomic.store(true);
    return true;
}

void AudioRecorder::stop() {
    const juce::ScopedLock sl(writerLock);
    threadedWriter.reset();
    fileStream.reset();
    isRecordingAtomic.store(false);
}

void AudioRecorder::pushBuffer(const juce::AudioBuffer<float>& buffer, int numSamples) {
    auto* writer = threadedWriter.get();
    if (writer == nullptr || numSamples <= 0)
        return;

    const int numChannels = buffer.getNumChannels();
    std::vector<const float*> channelData;
    channelData.reserve((size_t) numChannels);
    for (int ch = 0; ch < numChannels; ++ch)
        channelData.push_back(buffer.getReadPointer(ch));

    writer->write(channelData.data(), numSamples);
}
