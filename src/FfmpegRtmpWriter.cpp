#include "FfmpegRtmpWriter.h"
#include "Logging.h"
#include <mutex>

#if HAVE_FFMPEG
extern "C" {
 #include <libavformat/avformat.h>
 #include <libavutil/avutil.h>
 #include <libavcodec/avcodec.h>
}

static inline juce::String ff_err2str(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return juce::String(buf);
}
#endif

static std::mutex g_ffmpegWriteMutex;

struct FfmpegRtmpWriter::Impl {
#if HAVE_FFMPEG
    AVFormatContext* fmt = nullptr;
    AVStream* vstream = nullptr;
    AVStream* astream = nullptr;
    juce::String url;
    bool headerWritten { false };
    bool haveVideoConfig { false };
    bool haveAudioConfig { false };
#endif
};

FfmpegRtmpWriter::FfmpegRtmpWriter() : impl(std::make_unique<Impl>()) {}
FfmpegRtmpWriter::~FfmpegRtmpWriter() { close(); }

#if HAVE_FFMPEG
static inline void ff_try_write_header_internal(AVFormatContext* fmt, bool haveVideoConfig, bool& headerWritten) {
    if (!fmt || headerWritten) return;
    if (!haveVideoConfig) return;
    if (avformat_write_header(fmt, nullptr) < 0) {
        LogMessage("FFMPEG: write_header failed");
        return;
    }
    headerWritten = true;
    LogMessage("FFMPEG: write_header OK");
}
#endif

bool FfmpegRtmpWriter::open(const juce::String& url, const StreamingConfig& cfg) {
#if HAVE_FFMPEG
    LogMessage("FFMPEG: open -> " + url);
    avformat_network_init();

    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "flv", url.toRawUTF8()) < 0 || fmt == nullptr) {
        LogMessage("FFMPEG: avformat_alloc_output_context2 failed");
        return false;
    }

    // Create video stream (H.264)
    AVStream* v = avformat_new_stream(fmt, nullptr);
    if (!v) { LogMessage("FFMPEG: new video stream failed"); avformat_free_context(fmt); return false; }
    v->id = 0;
    v->time_base = AVRational{ 1, 1000 }; // ms
    v->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    v->codecpar->codec_id = AV_CODEC_ID_H264;
    v->codecpar->width = cfg.videoWidth;
    v->codecpar->height = cfg.videoHeight;
    // We'll provide avcC in extradata; payloads remain length-prefixed (AVCC) as produced by VT

    // Create audio stream (AAC)
    AVStream* a = avformat_new_stream(fmt, nullptr);
    if (!a) { LogMessage("FFMPEG: new audio stream failed"); avformat_free_context(fmt); return false; }
    a->id = 1;
    a->time_base = AVRational{ 1, 1000 }; // ms
    a->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    a->codecpar->codec_id = AV_CODEC_ID_AAC;
    a->codecpar->sample_rate = cfg.audioSampleRate;

    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open2(&fmt->pb, url.toRawUTF8(), AVIO_FLAG_WRITE, nullptr, nullptr) < 0) {
            LogMessage("FFMPEG: avio_open2 failed");
            avformat_free_context(fmt);
            return false;
        }
    }

    // Defer write_header until we have at least H.264 SPS/PPS (and AAC ASC if available)
    impl->fmt = fmt;
    impl->vstream = v;
    impl->astream = a;
    impl->url = url;
    impl->headerWritten = false;
    impl->haveVideoConfig = false;
    impl->haveAudioConfig = false;
    return true;
#else
    juce::ignoreUnused(url, cfg);
    LogMessage("FFMPEG: not available (HAVE_FFMPEG off)");
    return false;
#endif
}

bool FfmpegRtmpWriter::setVideoConfig(const void* data, size_t size) {
#if HAVE_FFMPEG
    if (!impl->fmt || !impl->vstream) return false;
    auto* par = impl->vstream->codecpar;
    par->extradata = (uint8_t*)av_malloc((int)size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!par->extradata) return false;
    memcpy(par->extradata, data, size);
    memset(par->extradata + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    par->extradata_size = (int) size;
    impl->haveVideoConfig = true;
    LogMessage("FFMPEG: video extradata set (SPS/PPS) size=" + juce::String((int)size));
    ff_try_write_header_internal(impl->fmt, impl->haveVideoConfig, impl->headerWritten);
    return true;
#else
    juce::ignoreUnused(data, size);
    return false;
#endif
}

bool FfmpegRtmpWriter::setAudioConfig(const void* data, size_t size) {
#if HAVE_FFMPEG
    if (!impl->fmt || !impl->astream) return false;
    auto* par = impl->astream->codecpar;
    par->extradata = (uint8_t*)av_malloc((int)size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!par->extradata) return false;
    memcpy(par->extradata, data, size);
    memset(par->extradata + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    par->extradata_size = (int) size;
    impl->haveAudioConfig = true;
    LogMessage("FFMPEG: audio extradata set (ASC) size=" + juce::String((int)size));
    ff_try_write_header_internal(impl->fmt, impl->haveVideoConfig, impl->headerWritten);
    return true;
#else
    juce::ignoreUnused(data, size);
    return false;
#endif
}

bool FfmpegRtmpWriter::writeVideoFrame(const void* data, size_t size, int64_t ptsMs, bool keyframe) {
#if HAVE_FFMPEG
    if (!impl->fmt || !impl->vstream) return false;
    ff_try_write_header_internal(impl->fmt, impl->haveVideoConfig, impl->headerWritten);
    if (!impl->headerWritten) return false;
    std::lock_guard<std::mutex> lk(g_ffmpegWriteMutex);
    AVPacket pkt{};
    av_init_packet(&pkt);
    pkt.data = (uint8_t*) data; // assumed valid until write
    pkt.size = (int) size;
    pkt.stream_index = impl->vstream->index;
    pkt.pts = pkt.dts = ptsMs; // time_base 1/1000
    if (keyframe) pkt.flags |= AV_PKT_FLAG_KEY;
    int ret = av_interleaved_write_frame(impl->fmt, &pkt);
    if (ret < 0) {
        LogMessage("FFMPEG: write video frame failed -> " + ff_err2str(ret));
        return false;
    }
    return true;
#else
    juce::ignoreUnused(data, size, ptsMs, keyframe);
    return false;
#endif
}

bool FfmpegRtmpWriter::writeAudioFrame(const void* data, size_t size, int64_t ptsMs) {
#if HAVE_FFMPEG
    if (!impl->fmt || !impl->astream) return false;
    ff_try_write_header_internal(impl->fmt, impl->haveVideoConfig, impl->headerWritten);
    if (!impl->headerWritten) return false;
    std::lock_guard<std::mutex> lk(g_ffmpegWriteMutex);
    AVPacket pkt{};
    av_init_packet(&pkt);
    pkt.data = (uint8_t*) data;
    pkt.size = (int) size;
    pkt.stream_index = impl->astream->index;
    pkt.pts = pkt.dts = ptsMs;
    int ret = av_interleaved_write_frame(impl->fmt, &pkt);
    if (ret < 0) {
        LogMessage("FFMPEG: write audio frame failed -> " + ff_err2str(ret));
        return false;
    }
    return true;
#else
    juce::ignoreUnused(data, size, ptsMs);
    return false;
#endif
}

void FfmpegRtmpWriter::close() {
#if HAVE_FFMPEG
    if (!impl->fmt) return;
    LogMessage("FFMPEG: close -> " + impl->url);
    if (impl->headerWritten)
        av_write_trailer(impl->fmt);
    if (impl->fmt->pb) avio_closep(&impl->fmt->pb);
    avformat_free_context(impl->fmt);
    impl->fmt = nullptr;
    impl->vstream = nullptr;
    impl->astream = nullptr;
    impl->url = {};
    impl->headerWritten = false;
    impl->haveVideoConfig = false;
    impl->haveAudioConfig = false;
#endif
}
