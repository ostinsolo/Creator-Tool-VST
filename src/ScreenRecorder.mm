#include "ScreenRecorder.h"
#include "Logging.h"

#if JUCE_MAC
 #import <AVFoundation/AVFoundation.h>
 #import <AppKit/AppKit.h>
 #if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
  #import <ScreenCaptureKit/ScreenCaptureKit.h>
  #define HAVE_SCKIT 1
 #else
  #define HAVE_SCKIT 0
 #endif

@interface JUCECaptureDelegate : NSObject<AVCaptureFileOutputRecordingDelegate>
@property (nonatomic, copy) void (^didFinish)(NSURL* outputURL, NSError* error);
@end

@implementation JUCECaptureDelegate
- (void)fileOutput:(AVCaptureFileOutput*)output didStartRecordingToOutputFileAtURL:(NSURL*)fileURL fromConnections:(NSArray<AVCaptureConnection*>*)connections {
    juce::ignoreUnused(output, connections);
    LogMessage("AVF: didStartRecording -> " + juce::String([fileURL.path UTF8String]));
}
- (void)fileOutput:(AVCaptureFileOutput*)output didFinishRecordingToOutputFileAtURL:(NSURL*)outputFileURL fromConnections:(NSArray<AVCaptureConnection*>*)connections error:(NSError*)error {
    juce::ignoreUnused(output, connections);
    if (error) LogMessage("AVF: didFinishRecording (error) -> " + juce::String([[error localizedDescription] UTF8String]));
    else LogMessage("AVF: didFinishRecording -> " + juce::String([outputFileURL.path UTF8String]));
    if (self.didFinish) self.didFinish(outputFileURL, error);
}
@end

#if HAVE_SCKIT
@interface JUCEStreamOutputBridge : NSObject<SCStreamOutput>
@property (nonatomic, assign) void* cppImplPtr;
@end
@implementation JUCEStreamOutputBridge
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    juce::ignoreUnused(stream);
    if (type != SCStreamOutputTypeScreen) return;
    struct SCKImpl { BOOL (*cb)(void*, CMSampleBufferRef); void* selfPtr; };
    SCKImpl* shim = (SCKImpl*)_cppImplPtr; if (shim && shim->cb) shim->cb(shim->selfPtr, sampleBuffer);
}
@end
#endif

struct ScreenRecorder::Impl {
    std::atomic<bool> running { false };
    std::atomic<bool> combined { false };
    double combinedSampleRate { 0.0 };
    int combinedNumChannels { 0 };
    int64_t audioSamplesPushed { 0 };

    std::atomic<int> desiredWidth { 0 };
    std::atomic<int> desiredHeight { 0 };

    AVCaptureSession* session = nil;
    AVCaptureScreenInput* screenInput = nil;
    AVCaptureMovieFileOutput* movieOutput = nil;
    JUCECaptureDelegate* delegate = nil;

#if HAVE_SCKIT
    SCStream* scStream = nil;
    JUCEStreamOutputBridge* scOutput = nil;
    dispatch_queue_t scQueue = nullptr;
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* videoInput = nil;
    AVAssetWriterInputPixelBufferAdaptor* videoAdaptor = nil;
    AVAssetWriterInput* audioInput = nil;
    dispatch_queue_t writerQueue = nullptr;
    BOOL startedWriting = NO;
    CMTime baseVideoPTS { kCMTimeInvalid };

    // Audio ring buffer and drain thread (int16 interleaved)
    std::unique_ptr<juce::AbstractFifo> audioFifo;
    juce::HeapBlock<int16_t> audioRing;
    int audioRingCapacityFrames { 0 };

    struct AudioDrainThread : public juce::Thread {
        Impl& owner;
        explicit AudioDrainThread(Impl& o) : juce::Thread("A+V Audio Drain"), owner(o) {}
        void run() override {
            while (! threadShouldExit()) {
                owner.drainAudioOnce();
                wait(2);
            }
            owner.drainAudioOnce();
        }
    };
    std::unique_ptr<AudioDrainThread> audioDrainThread;

    void startAudioDrain() {
        if (audioDrainThread) return;
        audioDrainThread = std::make_unique<AudioDrainThread>(*this);
       #if JUCE_THREAD_PRIORITIES
        audioDrainThread->startThread(juce::Thread::Priority::backgroundPriority);
       #else
        audioDrainThread->startThread();
       #endif
    }

    void stopAudioDrain() {
        if (audioDrainThread) {
            audioDrainThread->stopThread(2000);
            audioDrainThread.reset();
        }
    }

    void drainAudioOnce() {
        if (audioInput == nil || writer == nil || !startedWriting || !combined.load() || audioFifo == nullptr) return;
        const int channels = combinedNumChannels;
        if (channels <= 0) return;
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        audioFifo->prepareToRead(audioRingCapacityFrames, start1, size1, start2, size2);
        const auto framesAvail = size1 + size2;
        if (framesAvail == 0) { audioFifo->finishedRead(0); return; }

        auto appendChunk = [&](int start, int count) {
            if (count <= 0) return;
            const size_t frameBytes = sizeof(int16_t) * (size_t)channels;
            const void* basePtr = audioRing.getData() + (size_t)start * (size_t)channels;

            CMTime audioOffset = CMTIME_IS_VALID(baseVideoPTS) ? baseVideoPTS : kCMTimeZero;
            CMTime ptsFromStart = CMTimeMake((int64_t)audioSamplesPushed, (int32_t)combinedSampleRate);
            CMTime pts = CMTIME_IS_VALID(audioOffset) ? CMTimeAdd(audioOffset, ptsFromStart) : ptsFromStart;
            audioSamplesPushed += count;

            AudioStreamBasicDescription asbd = {0};
            asbd.mSampleRate = combinedSampleRate;
            asbd.mFormatID = kAudioFormatLinearPCM;
            asbd.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
            asbd.mBitsPerChannel = 16;
            asbd.mChannelsPerFrame = (UInt32) channels;
            asbd.mBytesPerFrame = (UInt32)frameBytes;
            asbd.mFramesPerPacket = 1;
            asbd.mBytesPerPacket = asbd.mBytesPerFrame * asbd.mFramesPerPacket;

            CMAudioFormatDescriptionRef formatDesc = nullptr;
            CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd, 0, nullptr, 0, nullptr, nullptr, &formatDesc);

            CMBlockBufferRef blockBuf = nullptr;
            CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, (void*)basePtr, (size_t)count * asbd.mBytesPerFrame, kCFAllocatorNull, nullptr, 0, (size_t)count * asbd.mBytesPerFrame, 0, &blockBuf);

            CMSampleBufferRef sampleBuf = nullptr;
            CMSampleTimingInfo timing = { .duration = CMTimeMake(1, (int32_t)combinedSampleRate), .presentationTimeStamp = pts, .decodeTimeStamp = kCMTimeInvalid };
            CMSampleBufferCreate(kCFAllocatorDefault, blockBuf, true, nullptr, nullptr, formatDesc, (CMItemCount)count, 1, &timing, 0, nullptr, &sampleBuf);

            CFRelease(formatDesc);
            CFRelease(blockBuf);

            if (sampleBuf) {
                if ([audioInput isReadyForMoreMediaData]) {
                    [audioInput appendSampleBuffer:sampleBuf];
                }
                CFRelease(sampleBuf);
            }
        };

        appendChunk(start1, size1);
        appendChunk(start2, size2);
        audioFifo->finishedRead(framesAvail);
    }

    static BOOL handleSample(void* selfPtr, CMSampleBufferRef sbuf) {
        Impl* self = (Impl*) selfPtr;
        if (!self->running.load() || self->writer == nil) return NO;
        CVImageBufferRef pix = CMSampleBufferGetImageBuffer(sbuf);
        if (pix == nullptr) return NO;
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sbuf);
        if (!CMTIME_IS_VALID(self->baseVideoPTS)) self->baseVideoPTS = pts;
        if (!self->startedWriting) {
            [self->writer startWriting];
            [self->writer startSessionAtSourceTime:pts];
            self->startedWriting = YES;
            LogMessage("SCK: started writing at pts=" + juce::String(CMTimeGetSeconds(pts)));
        }
        if (![self->videoInput isReadyForMoreMediaData]) return NO;
        return [self->videoAdaptor appendPixelBuffer:pix withPresentationTime:pts];
    }
#endif

    ~Impl() { stop(); }

    static bool canUseScreenCaptureKit() {
   #if HAVE_SCKIT
        if (@available(macOS 12.3, *)) return true; else return false;
   #else
        return false;
   #endif
    }

    static SCDisplay* pickMainDisplay() {
   #if HAVE_SCKIT
        __block SCDisplay* display = nil;
        dispatch_semaphore_t sema = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *error) {
            if (error == nil && content.displays.count > 0) display = content.displays.firstObject;
            dispatch_semaphore_signal(sema);
        }];
        dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
        return display;
   #else
        return nil;
   #endif
    }

    bool setupWriter(const juce::File& outFile, int width, int height, double sampleRate, int numChannels) {
    #if HAVE_SCKIT
        NSError* err = nil;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String: outFile.getFullPathName().toRawUTF8()]];
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
        LogMessage("SCK: creating AVAssetWriter -> " + juce::String(outFile.getFullPathName()));
        writer = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeQuickTimeMovie error:&err];
        if (err) { LogMessage("SCK: AVAssetWriter init error -> " + juce::String([[err localizedDescription] UTF8String])); return false; }

        NSDictionary* vidSettings = @{ AVVideoCodecKey: AVVideoCodecTypeH264,
                                        AVVideoWidthKey: @(width),
                                        AVVideoHeightKey: @(height) };
        videoInput = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo outputSettings:vidSettings];
        videoInput.expectsMediaDataInRealTime = YES;
        videoAdaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:videoInput sourcePixelBufferAttributes:nil];
        if ([writer canAddInput:videoInput]) [writer addInput:videoInput]; else { LogMessage("SCK: cannot add video input"); return false; }

        if (sampleRate > 0 && numChannels > 0) {
            NSDictionary* audSettings = @{ AVFormatIDKey: @(kAudioFormatLinearPCM),
                                            AVSampleRateKey: @(sampleRate),
                                            AVNumberOfChannelsKey: @(numChannels),
                                            AVLinearPCMBitDepthKey: @16,
                                            AVLinearPCMIsFloatKey: @NO,
                                            AVLinearPCMIsBigEndianKey: @NO,
                                            AVLinearPCMIsNonInterleaved: @NO };
            audioInput = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeAudio outputSettings:audSettings];
            audioInput.expectsMediaDataInRealTime = YES;
            if ([writer canAddInput:audioInput]) [writer addInput:audioInput]; else { LogMessage("SCK: cannot add audio input"); }
        }
        writerQueue = dispatch_queue_create("writer.queue", DISPATCH_QUEUE_SERIAL);
        return true;
    #else
        juce::ignoreUnused(outFile, width, height, sampleRate, numChannels);
        return false;
    #endif
    }

    bool startSCCombined(const juce::File& outFile, double sampleRate, int numChannels) {
   #if HAVE_SCKIT
        if (!canUseScreenCaptureKit()) return false;
        SCDisplay* display = pickMainDisplay();
        if (display == nil) { LogMessage("SCK: no display found"); return false; }
        NSError* err = nil;
        SCStreamConfiguration* cfg = [SCStreamConfiguration new];
        cfg.showsCursor = YES;
        cfg.queueDepth = 8;
        CGSize size = display.frame.size;
        if (desiredWidth.load() > 0 && desiredHeight.load() > 0) {
            size.width = desiredWidth.load();
            size.height = desiredHeight.load();
        }
        if (size.width <= 0 || size.height <= 0) { size.width = 1280; size.height = 720; }
        cfg.width = size.width; cfg.height = size.height;
        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];

        scStream = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:nil];
        scQueue = dispatch_queue_create("scstream.queue", DISPATCH_QUEUE_SERIAL);
        scOutput = [JUCEStreamOutputBridge new];
        struct Shim { BOOL (*cb)(void*, CMSampleBufferRef); void* selfPtr; };
        Shim* shim = (Shim*)malloc(sizeof(Shim)); shim->cb = &Impl::handleSample; shim->selfPtr = this; scOutput.cppImplPtr = shim;
        [scStream addStreamOutput:scOutput type:SCStreamOutputTypeScreen sampleHandlerQueue:scQueue error:&err];
        if (err) { LogMessage("SCK: addStreamOutput error -> " + juce::String([[err localizedDescription] UTF8String])); return false; }
        LogMessage("SCK: addStreamOutput OK");
        if (!setupWriter(outFile, (int)cfg.width, (int)cfg.height, sampleRate, numChannels)) return false;
        LogMessage("SCK: writer configured (audio=" + juce::String(sampleRate > 0.0 && numChannels > 0 ? "yes" : "no") + ")");
        combined.store(sampleRate > 0.0 && numChannels > 0);
        combinedSampleRate = sampleRate;
        combinedNumChannels = (numChannels > 0 ? numChannels : 2);
        audioSamplesPushed = 0;
        baseVideoPTS = kCMTimeInvalid;

        // Init audio ring if combined
        if (combined.load()) {
            const int targetFrames = juce::jmax(32768, (int) (2.0 * sampleRate));
            audioRingCapacityFrames = targetFrames;
            audioFifo.reset(new juce::AbstractFifo(audioRingCapacityFrames));
            audioRing.allocate((size_t)audioRingCapacityFrames * (size_t)combinedNumChannels, true);
            startAudioDrain();
        }

        running.store(true);
        LogMessage("SCK: startCapture requested -> " + juce::String(outFile.getFileName()));
        [scStream startCaptureWithCompletionHandler:^(NSError * _Nullable error) {
            if (error != nil) { LogMessage("SCK: startCapture error -> " + juce::String([[error localizedDescription] UTF8String])); running.store(false); }
            else { LogMessage("SCK: startCapture completed"); }
        }];
        return true;
   #else
        juce::ignoreUnused(outFile, sampleRate, numChannels);
        return false;
   #endif
    }

    bool startSCVideoOnly(const juce::File& outFile) {
   #if HAVE_SCKIT
        if (!canUseScreenCaptureKit()) return false;
        return startSCCombined(outFile, 0.0, 0);
   #else
        juce::ignoreUnused(outFile);
        return false;
   #endif
    }

    bool startAVFMovie(const juce::File& outFile) {
        LogMessage("AVF: startAVFMovie -> " + juce::String(outFile.getFullPathName()));
        session = [[AVCaptureSession alloc] init];
        screenInput = [[AVCaptureScreenInput alloc] initWithDisplayID:CGMainDisplayID()];
        if (![session canAddInput:screenInput]) { LogMessage("AVF: cannot add input"); return false; }
        [session addInput:screenInput];
        movieOutput = [[AVCaptureMovieFileOutput alloc] init];
        if (![session canAddOutput:movieOutput]) { LogMessage("AVF: cannot add output"); return false; }
        [session addOutput:movieOutput];
        delegate = [[JUCECaptureDelegate alloc] init];
        std::atomic<bool>* runningPtr = &running;
        delegate.didFinish = ^(NSURL* url, NSError* error) { juce::ignoreUnused(url, error); runningPtr->store(false); };
        [session startRunning];
        NSString* nsPath = [NSString stringWithUTF8String: outFile.getFullPathName().toRawUTF8()];
        NSURL* url = [NSURL fileURLWithPath: nsPath];
        [movieOutput startRecordingToOutputFileURL:url recordingDelegate:delegate];
        running.store(true);
        return true;
    }

    bool start(const juce::File& outFile) {
        if (running.load()) { LogMessage("start: already running"); return false; }
        if (canUseScreenCaptureKit()) {
            if (startSCVideoOnly(outFile)) return true;
        }
        return startAVFMovie(outFile);
    }

    bool startCombined(const juce::File& outFile, double sampleRate, int numChannels) {
        if (running.load()) { LogMessage("startCombined: already running"); return false; }
        if (canUseScreenCaptureKit()) {
            return startSCCombined(outFile, sampleRate, numChannels);
        }
        LogMessage("startCombined: SCK not available");
        return false;
    }

    void pushAudio(const juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate, int numChannels) {
    #if HAVE_SCKIT
        juce::ignoreUnused(sampleRate, numChannels);
        if (!combined.load() || writer == nil || audioInput == nil || audioFifo == nullptr) return;
        if (numSamples <= 0) return;
        const int channels = combinedNumChannels;
        const int chAvail = juce::jmin(buffer.getNumChannels(), channels);
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        audioFifo->prepareToWrite(numSamples, start1, size1, start2, size2);
        if (size1 + size2 < numSamples) {
            // drop
            return;
        }
        auto writeChunk = [&](int start, int count, int offsetInBuffer) {
            if (count <= 0) return;
            int16_t* dest = audioRing.getData() + ((size_t)start * (size_t)channels);
            for (int i = 0; i < count; ++i) {
                for (int c = 0; c < channels; ++c) {
                    float f = (c < chAvail) ? buffer.getReadPointer(c)[i + offsetInBuffer] : 0.0f;
                    f = juce::jlimit(-1.0f, 1.0f, f);
                    dest[i * channels + c] = (int16_t) juce::roundToInt(f * 32767.0f);
                }
            }
        };
        writeChunk(start1, size1, 0);
        writeChunk(start2, size2, size1);
        audioFifo->finishedWrite(size1 + size2);
    #else
        juce::ignoreUnused(buffer, numSamples, sampleRate, numChannels);
    #endif
    }

    void stop() {
        LogMessage("stop requested");
        if (!running.load()) {
            cleanupAVF(); cleanupSCK();
            return;
        }
        running.store(false);
        if (movieOutput != nil) [movieOutput stopRecording];
        if (session != nil) [session stopRunning];
        cleanupAVF();
#if HAVE_SCKIT
        if (scStream != nil) [scStream stopCaptureWithCompletionHandler:^(NSError * _Nullable error){ if (error) LogMessage("SCK: stopCapture error -> " + juce::String([[error localizedDescription] UTF8String])); }];
        if (videoInput != nil) [videoInput markAsFinished];
        if (audioInput != nil) [audioInput markAsFinished];
        if (writer != nil) [writer finishWritingWithCompletionHandler:^{ LogMessage("SCK: writer finished"); }];
        stopAudioDrain();
        cleanupSCK();
#endif
    }

    void cleanupAVF() {
        movieOutput = nil; screenInput = nil; session = nil; delegate = nil;
    }
#if HAVE_SCKIT
    void cleanupSCK() {
        scOutput = nil; scStream = nil; videoAdaptor = nil; videoInput = nil; audioInput = nil; writer = nil; startedWriting = NO; baseVideoPTS = kCMTimeInvalid; combined.store(false); audioSamplesPushed = 0; combinedSampleRate = 0.0; combinedNumChannels = 0; audioFifo.reset(); audioRingCapacityFrames = 0; audioRing.free(); if (scQueue) { scQueue = nullptr; } if (writerQueue) { writerQueue = nullptr; }
    }
#endif

    bool isRunning() const { return running.load(); }
};

ScreenRecorder::ScreenRecorder() : impl(std::make_unique<Impl>()) {}
ScreenRecorder::~ScreenRecorder() { stop(); }

bool ScreenRecorder::startRecording(const juce::File& outputFile) {
    LogMessage("ScreenRecorder::startRecording -> " + outputFile.getFullPathName());
    lastRecordedFile = outputFile;
    return impl->start(outputFile);
}

bool ScreenRecorder::startCombined(const juce::File& outputFile, double sampleRate, int numChannels) {
    LogMessage("ScreenRecorder::startCombined -> " + outputFile.getFullPathName());
    lastRecordedFile = outputFile;
    return impl->startCombined(outputFile, sampleRate, numChannels);
}

void ScreenRecorder::pushAudio(const juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate, int numChannels) {
    impl->pushAudio(buffer, numSamples, sampleRate, numChannels);
}

void ScreenRecorder::setCaptureResolution(int width, int height) {
    impl->desiredWidth.store(width);
    impl->desiredHeight.store(height);
}

void ScreenRecorder::stop() {
    LogMessage("ScreenRecorder::stop");
    if (impl) impl->stop();
}

bool ScreenRecorder::isRecording() const {
    return impl && impl->isRunning();
}

#else

struct ScreenRecorder::Impl {
    bool start(const juce::File&) { return false; }
    bool startCombined(const juce::File&, double, int) { return false; }
    void pushAudio(const juce::AudioBuffer<float>&, int, double, int) {}
    void stop() {}
    bool isRunning() const { return false; }
};

ScreenRecorder::ScreenRecorder() : impl(std::make_unique<Impl>()) {}
ScreenRecorder::~ScreenRecorder() { stop(); }

bool ScreenRecorder::startRecording(const juce::File&) { return false; }
bool ScreenRecorder::startCombined(const juce::File&, double, int) { return false; }
void ScreenRecorder::pushAudio(const juce::AudioBuffer<float>&, int, double, int) {}
void ScreenRecorder::stop() {}
bool ScreenRecorder::isRecording() const { return false; }

#endif
