#include "ScreenRecorder.h"

#if JUCE_MAC
 #import <AVFoundation/AVFoundation.h>
 #import <AppKit/AppKit.h>

struct ScreenRecorder::Impl {
    AVCaptureSession* session = nil;
    AVCaptureScreenInput* screenInput = nil;
    AVCaptureMovieFileOutput* movieOutput = nil;
    bool running = false;

    ~Impl() {
        stop();
    }

    bool start(const juce::File& outFile) {
        if (running) return false;
        session = [[AVCaptureSession alloc] init];
        screenInput = [[AVCaptureScreenInput alloc] initWithDisplayID:CGMainDisplayID()];
        if (![session canAddInput:screenInput]) return false;
        [session addInput:screenInput];

        movieOutput = [[AVCaptureMovieFileOutput alloc] init];
        if (![session canAddOutput:movieOutput]) return false;
        [session addOutput:movieOutput];

        [session startRunning];
        NSURL* url = [NSURL fileURLWithPath: juce::String(outFile.getFullPathName()).toNSString()];
        [movieOutput startRecordingToOutputFileURL:url recordingDelegate:nil];
        running = true;
        return true;
    }

    void stop() {
        if (!running) return;
        [movieOutput stopRecording];
        [session stopRunning];
        running = false;
    }

    bool isRunning() const { return running; }
};

ScreenRecorder::ScreenRecorder() : impl(std::make_unique<Impl>()) {}
ScreenRecorder::~ScreenRecorder() { stop(); }

bool ScreenRecorder::startRecording(const juce::File& outputFile) {
    lastRecordedFile = outputFile;
    return impl->start(outputFile);
}

void ScreenRecorder::stop() {
    if (impl) impl->stop();
}

bool ScreenRecorder::isRecording() const {
    return impl && impl->isRunning();
}

#else

struct ScreenRecorder::Impl {
    bool start(const juce::File&) { return false; }
    void stop() {}
    bool isRunning() const { return false; }
};

ScreenRecorder::ScreenRecorder() : impl(std::make_unique<Impl>()) {}
ScreenRecorder::~ScreenRecorder() { stop(); }

bool ScreenRecorder::startRecording(const juce::File&) { return false; }
void ScreenRecorder::stop() {}
bool ScreenRecorder::isRecording() const { return false; }

#endif
