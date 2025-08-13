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
    fileStream = file.createOutputStream();
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

    // Allocate FIFO with 2 seconds of audio as headroom (minimum 32768 samples)
    fifoNumChannels = juce::jmax(1, numChannels);
    const int targetSamples = juce::jmax(32768, (int) (2.0 * sampleRate));
    fifoCapacity = targetSamples;
    fifo.reset(new juce::AbstractFifo(fifoCapacity));
    fifoBuffer.setSize(fifoNumChannels, fifoCapacity);
    fifoBuffer.clear();
    droppedSamples.store(0);

    startDrainThread();
    isRecordingAtomic.store(true);
    return true;
}

void AudioRecorder::stop() {
    stopDrainThread();
    const juce::ScopedLock sl(writerLock);
    threadedWriter.reset();
    fileStream.reset();
    fifoBuffer.setSize(0, 0);
    fifoCapacity = 0;
    fifoNumChannels = 0;
    fifo.reset();
    isRecordingAtomic.store(false);
}

void AudioRecorder::pushBuffer(const juce::AudioBuffer<float>& buffer, int numSamples) {
    auto* w = threadedWriter.get();
    if (w == nullptr || ! isRecordingAtomic.load() || numSamples <= 0 || fifo == nullptr)
        return;

    // Lock-free write into ring buffer; drop if full
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo->prepareToWrite(numSamples, start1, size1, start2, size2);

    if (size1 + size2 < numSamples) {
        droppedSamples.fetch_add(numSamples);
        return;
    }

    const int channelsToCopy = juce::jmin(buffer.getNumChannels(), fifoNumChannels);

    if (size1 > 0) {
        for (int ch = 0; ch < channelsToCopy; ++ch)
            fifoBuffer.copyFrom(ch, start1, buffer.getReadPointer(ch), size1);
        for (int ch = channelsToCopy; ch < fifoNumChannels; ++ch)
            fifoBuffer.clear(ch, start1, size1);
    }
    if (size2 > 0) {
        for (int ch = 0; ch < channelsToCopy; ++ch)
            fifoBuffer.copyFrom(ch, start2, buffer.getReadPointer(ch) + size1, size2);
        for (int ch = channelsToCopy; ch < fifoNumChannels; ++ch)
            fifoBuffer.clear(ch, start2, size2);
    }

    fifo->finishedWrite(size1 + size2);
}

void AudioRecorder::startDrainThread() {
    if (drainThread) return;
    drainThread = std::make_unique<DrainThread>(*this);
   #if JUCE_THREAD_PRIORITIES
    drainThread->startThread(juce::Thread::Priority::backgroundPriority);
   #else
    drainThread->startThread();
   #endif
}

void AudioRecorder::stopDrainThread() {
    if (drainThread) {
        drainThread->stopThread(2000);
        drainThread.reset();
    }
}

void AudioRecorder::DrainThread::run() {
    while (! threadShouldExit()) {
        owner.drainOnce();
        wait(2);
    }
    owner.drainOnce();
}

void AudioRecorder::drainOnce() {
    auto* w = threadedWriter.get();
    if (w == nullptr || fifo == nullptr) return;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo->prepareToRead(fifoCapacity, start1, size1, start2, size2);

    if (size1 + size2 == 0) {
        fifo->finishedRead(0);
        return;
    }

    if (size1 > 0) {
        std::vector<const float*> channelPtrs;
        channelPtrs.reserve((size_t) fifoNumChannels);
        for (int ch = 0; ch < fifoNumChannels; ++ch)
            channelPtrs.push_back(fifoBuffer.getReadPointer(ch, start1));
        w->write(channelPtrs.data(), size1);
    }

    if (size2 > 0) {
        std::vector<const float*> channelPtrs;
        channelPtrs.reserve((size_t) fifoNumChannels);
        for (int ch = 0; ch < fifoNumChannels; ++ch)
            channelPtrs.push_back(fifoBuffer.getReadPointer(ch, start2));
        w->write(channelPtrs.data(), size2);
    }

    fifo->finishedRead(size1 + size2);
}
