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

    // Audio conversion
    AVAudioConverter* converter { nil };
    AVAudioFormat* inFmt { nil };
    AVAudioFormat* outFmt { nil }; // AAC
    juce::int64 audioPtsMs{0};
    dispatch_queue_t aacQueue { nullptr };
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
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
        int64_t ms = (int64_t) llround(CMTimeGetSeconds(pts) * 1000.0);

        // Extract SPS/PPS once from format desc then send as extradata to RTMP
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
                    avcc.writeByte(1); // configurationVersion
                    avcc.writeByte(spsPtr[1]); // profile
                    avcc.writeByte(spsPtr[2]); // compatibility
                    avcc.writeByte(spsPtr[3]); // level
                    avcc.writeByte(0xFC | 3); // lengthSizeMinusOne (4 bytes)
                    avcc.writeByte(0xE0 | 1); // numOfSPS = 1
                    avcc.writeShortBigEndian((short)spsSize);
                    avcc.write(spsPtr, spsSize);
                    avcc.writeByte(1); // numOfPPS = 1
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

        // Send AVCC payload directly (length-prefixed NALs) to FLV muxer
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sampleBuffer);
        if (!bb) return;
        size_t totalLen = 0; char* dataPtr = nullptr;
        OSStatus st = CMBlockBufferGetDataPointer(bb, 0, nullptr, &totalLen, &dataPtr);
        if (st != noErr || totalLen == 0 || dataPtr == nullptr) return;

        self->rtmp.writeVideoFrame(dataPtr, totalLen, ms, keyframe);
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
        int32_t bps = cfg.videoBitrateKbps * 1000;
        CFNumberRef br = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bps);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_AverageBitRate, br); vtRelease(br);
        int32_t keyint = cfg.keyframeIntervalSec * fps;
        CFNumberRef gop = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &keyint);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_MaxKeyFrameInterval, gop); vtRelease(gop);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
        // Profile/Level
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_4_1);
        // Constrain data rate window
        CFNumberRef maxr = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bps);
        VTSessionSetProperty(vt, kVTCompressionPropertyKey_DataRateLimits, (CFTypeRef)@[(__bridge NSNumber*)maxr, @(1)]);
        vtRelease(maxr);

        st = VTCompressionSessionPrepareToEncodeFrames(vt);
        if (st != noErr) { LogMessage("VT: prepare failed"); return false; }
        vtReady.store(true);
        LogMessage("VT: ready");
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
        // Try to get AudioSpecificConfig (magic cookie) from output format
        NSData* cookie = [outFmt magicCookie];
        if (cookie && cookie.length > 0) {
            rtmp.setAudioConfig(cookie.bytes, (size_t) cookie.length);
            LogMessage("AAC: sent magic cookie ASC size=" + juce::String((int)cookie.length));
        } else {
            // Build minimal ASC for AAC-LC (object type 2)
            auto sr = (int) cfg.audioSampleRate;
            int sfi = 4; // default 44100
            const int srTable[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
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
#endif
    return true;
}

void LiveStreamer::stop() {
#if JUCE_MAC
    impl->active.store(false);
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
    // Copy to an AVAudioPCMBuffer and encode on background queue to avoid audio thread work
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
    FfmpegRtmpWriter* writer = &impl->rtmp;
    std::atomic<bool>* activePtr = &impl->active;
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
        if (outBuf.byteLength > 0) writer->writeAudioFrame(outBuf.data, outBuf.byteLength, ptsMs);
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
