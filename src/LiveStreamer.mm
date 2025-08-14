#include "LiveStreamer.h"
#include "FfmpegRtmpWriter.h"
#include "Logging.h"

#if JUCE_MAC
 #import <VideoToolbox/VideoToolbox.h>
 #import <AudioToolbox/AudioToolbox.h>
 #import <AVFoundation/AVFoundation.h>
 #import <CoreMedia/CoreMedia.h>
#endif

using namespace streaming;

namespace {
static void vtRelease(CFTypeRef obj) { if (obj) CFRelease(obj); }
}

struct streaming::LiveStreamer::Impl {
    StreamingConfig cfg;
    FfmpegRtmpWriter rtmp;

#if JUCE_MAC
    VTCompressionSessionRef vt{nullptr};
    std::atomic<bool> vtReady{false};
    std::atomic<bool> active{false};
    bool sentFirstVideo { false };
    juce::HeapBlock<uint8_t> spspps;
    size_t spsppsSize{0};
    std::atomic<juce::int64> lastVideoSentRelMs { 0 };

    // Audio conversion
    AVAudioConverter* converter { nil };
    AVAudioFormat* inFmt { nil };
    AVAudioFormat* outFmt { nil }; // AAC
    juce::int64 audioPtsMs{0};
    dispatch_queue_t aacQueue { nullptr };

    // Common PTS base (ms) set on first video frame
    std::atomic<bool> ptsBaseSet { false };
    std::atomic<juce::int64> basePtsMs { 0 };
    // Pacing: send encoded frames at real-time rate
    struct PendingFrame {
        juce::HeapBlock<uint8_t> bytes;
        size_t length { 0 };
        juce::int64 ptsMs { 0 };
        bool keyframe { false };
    };
    std::deque<PendingFrame> pendingFrames;
    std::mutex pendingMutex;
    std::atomic<bool> pacingStarted { false };
    std::chrono::steady_clock::time_point wallStart;
    dispatch_source_t pacerTimer { nullptr };

    // Audio pacing
    struct PendingAudio {
        juce::HeapBlock<uint8_t> bytes;
        size_t length { 0 };
        juce::int64 ptsMs { 0 };
    };
    std::deque<PendingAudio> pendingAudio;
    std::mutex audioMutex;
    std::atomic<bool> audioPacingStarted { false };
    dispatch_source_t audioTimer { nullptr };

    void startAudioPacingIfNeeded() {
        if (audioPacingStarted.load()) return;
        audioTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
        if (!audioTimer) return;
        // Fire every ~21ms for 48kHz AAC (1024 samples ≈ 21.33ms)
        dispatch_source_set_timer(audioTimer, dispatch_time(DISPATCH_TIME_NOW, 0), (uint64_t)(21 * NSEC_PER_MSEC), (uint64_t)(1 * NSEC_PER_MSEC));
        dispatch_source_set_event_handler(audioTimer, ^{
            if (!ptsBaseSet.load()) return;
            auto now = std::chrono::steady_clock::now();
            juce::int64 elapsedMs = (juce::int64) std::chrono::duration_cast<std::chrono::milliseconds>(now - wallStart).count();
            int sentThisTick = 0;
            while (sentThisTick < 2) { // allow up to 2 AAC packets per tick to catch up slightly
                PendingAudio pa;
                {
                    std::lock_guard<std::mutex> lk(audioMutex);
                    if (pendingAudio.empty()) break;
                    if (pendingAudio.front().ptsMs > elapsedMs) break; // not yet due
                    pa = std::move(pendingAudio.front());
                    pendingAudio.pop_front();
                }
                rtmp.writeAudioFrame(pa.bytes.getData(), pa.length, pa.ptsMs);
                ++sentThisTick;
            }
        });
        dispatch_resume(audioTimer);
        audioPacingStarted.store(true);
    }
#endif

    bool openRtmp() {
        if (!rtmp.open(cfg.relayUrl.isNotEmpty() && cfg.useLocalRelay ? cfg.relayUrl : cfg.rtmpUrl, cfg)) return false;
        return true;
    }

#if JUCE_MAC
    static void vtOutputCallback(void* outputCallbackRefCon, void* sourceFrameRefCon, OSStatus status, VTEncodeInfoFlags infoFlags, CMSampleBufferRef sampleBuffer) {
        juce::ignoreUnused(sourceFrameRefCon, infoFlags);
        auto* self = static_cast<Impl*>(outputCallbackRefCon);
        if (status != noErr || !sampleBuffer) return;
        bool keyframe = false;
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
        if (attachments && CFArrayGetCount(attachments) > 0) {
            CFDictionaryRef att = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
            keyframe = !CFDictionaryContainsKey(att, kCMSampleAttachmentKey_NotSync);
        }
        // We will generate a CFR timeline; ignore capture PTS

        // Extract SPS/PPS once
        if (self->spsppsSize == 0) {
            CMFormatDescriptionRef fmtDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
            if (fmtDesc) {
                const uint8_t* spsPtr = nullptr; size_t spsSize = 0;
                const uint8_t* ppsPtr = nullptr; size_t ppsSize = 0;
                size_t count = 0; int nalHdrLen = 0;
                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmtDesc, 0, &spsPtr, &spsSize, &count, &nalHdrLen) == noErr &&
                    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmtDesc, 1, &ppsPtr, &ppsSize, &count, &nalHdrLen) == noErr &&
                    spsPtr && ppsPtr && spsSize > 3) {
                    juce::MemoryOutputStream avcc(256);
                    avcc.writeByte(1);
                    avcc.writeByte(spsPtr[1]);
                    avcc.writeByte(spsPtr[2]);
                    avcc.writeByte(spsPtr[3]);
                    avcc.writeByte(0xFC | 3);
                    avcc.writeByte(0xE0 | 1);
                    avcc.writeShortBigEndian((short)spsSize);
                    avcc.write(spsPtr, spsSize);
                    avcc.writeByte(1);
                    avcc.writeShortBigEndian((short)ppsSize);
                    avcc.write(ppsPtr, ppsSize);
                    self->spsppsSize = (size_t)avcc.getDataSize();
                    self->spspps.allocate(self->spsppsSize, true);
                    memcpy(self->spspps.getData(), avcc.getData(), self->spsppsSize);
                    self->rtmp.setVideoConfig(self->spspps.getData(), self->spsppsSize);
                    LogMessage("VT: SPS/PPS extracted and set (avcC) size=" + juce::String((int)self->spsppsSize));
                }
            }
        }

        // Base PTS and CFR counter
        if (!self->ptsBaseSet.load()) {
            self->ptsBaseSet.store(true);
            self->wallStart = std::chrono::steady_clock::now();
            self->lastVideoSentRelMs.store(0);
        }
        const int frameMs = (self->cfg.fps > 0 ? (int) llround(1000.0 / (double) self->cfg.fps) : 33);
        int64_t relMs = self->lastVideoSentRelMs.load() + frameMs;

        // Emit frame (enqueue for paced sending)
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sampleBuffer);
        if (!bb) return;
        size_t totalLen = 0; char* dataPtr = nullptr;
        OSStatus st = CMBlockBufferGetDataPointer(bb, 0, nullptr, &totalLen, &dataPtr);
        if (st != noErr || totalLen == 0 || dataPtr == nullptr) return;

        // If backlog is large, drop non-keyframes to avoid bursts
        juce::int64 lastSent = self->lastVideoSentRelMs.load();
        if (!keyframe && (relMs - lastSent) > 1000) {
            LogMessage("VT: dropping non-keyframe due to backlog relMs=" + juce::String((int)relMs) + " lastSent=" + juce::String((int)lastSent));
            return;
        }

        // Copy frame bytes as the CMBlockBuffer will not remain valid after callback
        PendingFrame pf;
        pf.bytes.allocate(totalLen, true);
        memcpy(pf.bytes.getData(), dataPtr, totalLen);
        pf.length = totalLen;
        pf.ptsMs = relMs;
        pf.keyframe = keyframe;
        {
            std::lock_guard<std::mutex> lk(self->pendingMutex);
            self->pendingFrames.emplace_back(std::move(pf));
        }

        // Start pacing on first video if not started
        if (!self->pacingStarted.load()) {
            self->wallStart = std::chrono::steady_clock::now();
            self->pacerTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
            if (self->pacerTimer) {
                // Pace according to cfg.fps (one frame period)
                int32_t frameMs = (self->cfg.fps > 0 ? (int32_t) llround(1000.0 / (double) self->cfg.fps) : 33);
                dispatch_source_set_timer(self->pacerTimer, dispatch_time(DISPATCH_TIME_NOW, 0), (uint64_t)(frameMs * NSEC_PER_MSEC), (uint64_t)(1 * NSEC_PER_MSEC));
                dispatch_source_set_event_handler(self->pacerTimer, ^{
                    auto now = std::chrono::steady_clock::now();
                    juce::int64 elapsedMs = (juce::int64) std::chrono::duration_cast<std::chrono::milliseconds>(now - self->wallStart).count();
                    int sentThisTick = 0;
                    while (sentThisTick < 1) {
                        PendingFrame next;
                        {
                            std::lock_guard<std::mutex> lk(self->pendingMutex);
                            if (self->pendingFrames.empty()) break;
                            // Only send when its PTS is due
                            if (self->pendingFrames.front().ptsMs > elapsedMs) break;
                            next = std::move(self->pendingFrames.front());
                            self->pendingFrames.pop_front();
                        }
                        // Send
                        if (self->rtmp.writeVideoFrame(next.bytes.getData(), next.length, next.ptsMs, next.keyframe)) {
                            self->lastVideoSentRelMs.store(next.ptsMs);
                        }
                        ++sentThisTick;
                    }
                });
                dispatch_resume(self->pacerTimer);
                self->pacingStarted.store(true);
            }
        }
    }

    bool initVideoEncoder() {
        OSStatus st = VTCompressionSessionCreate(kCFAllocatorDefault,
                                                 cfg.videoWidth,
                                                 cfg.videoHeight,
                                                 kCMVideoCodecType_H264,
                                                 nullptr, nullptr, nullptr,
                                                 vtOutputCallback,
                                                 this,
                                                 &vt);
        if (st != noErr || !vt) { LogMessage("VT: create session failed"); return false; }
        int32_t fps = cfg.fps;
        CFNumberRef fpsNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &fps);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_ExpectedFrameRate, fpsNum); vtRelease(fpsNum);
        // Bitrate ramp: start lower to stabilize ingest, then raise to target
        int32_t bpsTarget = cfg.videoBitrateKbps * 1000;
        int32_t bpsInitial = (int32_t) std::lround((double) bpsTarget * 0.6);
        CFNumberRef br = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bpsInitial);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_AverageBitRate, br); vtRelease(br);
        // Short initial GOP (1s) for faster detection, will switch later
        int32_t keyintInitial = fps;
        CFNumberRef gopInit = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &keyintInitial);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_MaxKeyFrameInterval, gopInit); vtRelease(gopInit);
        // Real-time low-latency dials
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
        int32_t maxDelay = 1; CFNumberRef md = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &maxDelay);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_MaxFrameDelayCount, md); vtRelease(md);
        // Profile/Level (Facebook 1080p60 needs 4.2; otherwise 4.1)
        CFStringRef level = (cfg.fps >= 60 && cfg.videoHeight >= 1080)
            ? kVTProfileLevel_H264_High_4_2
            : kVTProfileLevel_H264_High_4_1;
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_ProfileLevel, level);
        // Constrain data rate window (per-second). This property expects BYTES/sec and a window in seconds.
        NSNumber* bytesPerSecInit = @(bpsInitial / 8);
        NSNumber* windowSec = @(1);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_DataRateLimits, (__bridge CFArrayRef)@[bytesPerSecInit, windowSec]);

        st = VTCompressionSessionPrepareToEncodeFrames(vt);
        if (st != noErr) { LogMessage("VT: prepare failed"); return false; }
        vtReady.store(true);
        LogMessage("VT: ready");

        // After 2s, switch to configured GOP (2s by default)
        int32_t keyintFinal = cfg.keyframeIntervalSec * fps;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2 * NSEC_PER_SEC)), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            if (!vt) return;
            CFNumberRef gopFinal = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &keyintFinal);
            VTSessionSetProperty(vt, kVTCompressionPropertyKey_MaxKeyFrameInterval, gopFinal); vtRelease(gopFinal);
        });

        // After 5s, raise to target bitrate and update data rate window
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5 * NSEC_PER_SEC)), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            if (!vt) return;
            CFNumberRef brFinal = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bpsTarget);
            VTSessionSetProperty(vt, kVTCompressionPropertyKey_AverageBitRate, brFinal); vtRelease(brFinal);
            NSNumber* bytesPerSecFinal = @(bpsTarget / 8);
            NSNumber* windowSecFinal = @(1);
            VTSessionSetProperty(vt, kVTCompressionPropertyKey_DataRateLimits, (__bridge CFArrayRef)@[bytesPerSecFinal, windowSecFinal]);
        });
        return true;
    }

    bool initAudioConverter() {
        // Input: float32 non-interleaved
        inFmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:cfg.audioSampleRate channels:cfg.audioChannels];
        // Output: AAC LC
        NSDictionary* settings = @{ AVFormatIDKey: @(kAudioFormatMPEG4AAC),
                                     AVSampleRateKey: @(cfg.audioSampleRate),
                                     AVNumberOfChannelsKey: @(cfg.audioChannels),
                                     AVEncoderBitRateKey: @(cfg.audioBitrateKbps * 1000) };
        outFmt = [[AVAudioFormat alloc] initWithSettings:settings];
        converter = [[AVAudioConverter alloc] initFromFormat:inFmt toFormat:outFmt];
        if (!converter) { LogMessage("AAC: converter create failed"); return false; }
        NSData* cookie = [outFmt magicCookie];
        if (cookie && cookie.length > 0) {
            rtmp.setAudioConfig(cookie.bytes, (size_t) cookie.length);
            LogMessage("AAC: sent magic cookie ASC size=" + juce::String((int)cookie.length));
        } else {
            auto sr = (int) cfg.audioSampleRate;
            int sfi = 4; const int srTable[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
            for (int i = 0; i < (int)(sizeof(srTable)/sizeof(srTable[0])); ++i) { if (srTable[i] == sr) { sfi = i; break; } }
            int ch = juce::jlimit(1, 2, (int) cfg.audioChannels);
            uint8_t asc[2];
            asc[0] = (uint8_t)((2 << 3) | ((sfi & 0x0F) >> 1));
            asc[1] = (uint8_t)(((sfi & 0x01) << 7) | ((ch & 0x0F) << 3));
            rtmp.setAudioConfig(asc, sizeof(asc));
            LogMessage("AAC: built minimal ASC (sr=" + juce::String(sr) + ", ch=" + juce::String(ch) + ")");
        }
        LogMessage("AAC: converter ready");
        return true;
    }
#endif
};

LiveStreamer::LiveStreamer() : impl(new Impl()) {}
LiveStreamer::~LiveStreamer() { stop(); }

bool LiveStreamer::start(const StreamingConfig& cfg) {
    impl->cfg = cfg;
    if (!impl->openRtmp()) return false;
#if JUCE_MAC
    if (!impl->initVideoEncoder()) return false;
    if (!impl->initAudioConverter()) return false;
    impl->active.store(true);
    impl->sentFirstVideo = false;
    impl->aacQueue = dispatch_queue_create("live.aac.queue", DISPATCH_QUEUE_SERIAL);
    impl->ptsBaseSet.store(false);
    impl->basePtsMs.store(0);
    impl->audioPtsMs = 0;
        impl->startAudioPacingIfNeeded();
#endif
    return true;
}

void LiveStreamer::stop() {
#if JUCE_MAC
    impl->active.store(false);
    if (impl->pacerTimer) { dispatch_source_cancel(impl->pacerTimer); impl->pacerTimer = nullptr; }
    if (impl->audioTimer) { dispatch_source_cancel(impl->audioTimer); impl->audioTimer = nullptr; }
    {
        std::lock_guard<std::mutex> lk(impl->pendingMutex);
        impl->pendingFrames.clear();
    }
    {
        std::lock_guard<std::mutex> lk2(impl->audioMutex);
        impl->pendingAudio.clear();
    }
    if (impl->vt) { VTCompressionSessionInvalidate(impl->vt); CFRelease(impl->vt); impl->vt = nullptr; }
    impl->converter = nil; impl->inFmt = nil; impl->outFmt = nil;
    impl->aacQueue = nullptr;
#endif
    impl->rtmp.close();
}

void LiveStreamer::pushAudioPCM(const juce::AudioBuffer<float>& buffer, int numSamples, double sampleRate, int numChannels) {
    juce::ignoreUnused(sampleRate, numChannels);
#if JUCE_MAC
    if (!impl->active.load() || !impl->converter || !impl->inFmt || !impl->outFmt || impl->aacQueue == nullptr) return;
    if (!impl->ptsBaseSet.load()) return;
    AVAudioPCMBuffer* inBuf = [[AVAudioPCMBuffer alloc] initWithPCMFormat:impl->inFmt frameCapacity:(AVAudioFrameCount) numSamples];
    inBuf.frameLength = (AVAudioFrameCount) numSamples;
    const int ch = juce::jmin(buffer.getNumChannels(), (int) impl->inFmt.channelCount);
    for (int c = 0; c < ch; ++c) {
        memcpy(inBuf.floatChannelData[c], buffer.getReadPointer(c), sizeof(float) * (size_t) numSamples);
    }
    juce::int64 ptsMs = impl->audioPtsMs;
    impl->audioPtsMs += (int64_t) llround(1000.0 * (double) numSamples / (double) impl->cfg.audioSampleRate);
    AVAudioFormat* outFmt = impl->outFmt;
    AVAudioConverter* conv = impl->converter;
    std::atomic<bool>* activePtr = &impl->active;
    std::deque<Impl::PendingAudio>* pendingAudio = &impl->pendingAudio;
    std::mutex* audioMutex = &impl->audioMutex;
    dispatch_async(impl->aacQueue, ^{
        if (!conv) return;
        AVAudioCompressedBuffer* outBuf = [[AVAudioCompressedBuffer alloc] initWithFormat:outFmt packetCapacity:512 maximumPacketSize:2048];
        NSError* err = nil;
        AVAudioConverterOutputStatus st = [conv convertToBuffer:outBuf error:&err withInputFromBlock:^AVAudioBuffer * _Nullable(AVAudioPacketCount inNumberOfPackets, AVAudioConverterInputStatus * _Nonnull outStatus) {
            juce::ignoreUnused(inNumberOfPackets);
            if (inBuf.frameLength > 0) { *outStatus = AVAudioConverterInputStatus_HaveData; return (AVAudioBuffer*) inBuf; }
            *outStatus = AVAudioConverterInputStatus_EndOfStream; return (AVAudioBuffer*) nil;
        }];
        if (st == AVAudioConverterOutputStatus_Error || err) { LogMessage("AAC: convert failed"); return; }
        if (!activePtr->load()) return;
        if (outBuf.byteLength > 0) {
            AVAudioPacketCount pc = outBuf.packetCount;
            const AudioStreamPacketDescription* pds = outBuf.packetDescriptions;
            const int64_t packetDurMs = (int64_t) llround(1024.0 * 1000.0 / (double) impl->cfg.audioSampleRate);
            uint8_t* base = (uint8_t*) outBuf.data;
            for (AVAudioPacketCount i = 0; i < pc; ++i) {
                size_t offs = (size_t) pds[i].mStartOffset;
                size_t sz   = (size_t) pds[i].mDataByteSize;
                Impl::PendingAudio pa;
                pa.bytes.allocate(sz, true);
                memcpy(pa.bytes.getData(), base + offs, sz);
                pa.length = sz;
                pa.ptsMs = ptsMs + (int64_t) i * packetDurMs;
                std::lock_guard<std::mutex> lk(*audioMutex);
                pendingAudio->emplace_back(std::move(pa));
            }
        }
    });
#endif
}

void LiveStreamer::pushPixelBuffer(void* cvPixelBufferRef, int64_t ptsMs) {
#if JUCE_MAC
    if (!impl->vt || !impl->vtReady.load() || !impl->active.load()) return;
    CVImageBufferRef pix = (CVImageBufferRef) cvPixelBufferRef;
    CMTime pts = CMTimeMake(ptsMs, 1000);
    VTEncodeInfoFlags flags = 0;
    CFDictionaryRef opts = nullptr;
    if (!impl->sentFirstVideo) {
        const void* keys[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
        const void* vals[] = { kCFBooleanTrue };
        opts = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    OSStatus st = VTCompressionSessionEncodeFrame(impl->vt, pix, pts, kCMTimeInvalid, opts, nullptr, &flags);
    if (opts) CFRelease(opts);
    if (st != noErr) { LogMessage("VT: encode frame failed" ); }
    impl->sentFirstVideo = true;
#else
    juce::ignoreUnused(cvPixelBufferRef, ptsMs);
#endif
}

// (removed static duplicate: audio pacing implemented as Impl::startAudioPacingIfNeeded)
