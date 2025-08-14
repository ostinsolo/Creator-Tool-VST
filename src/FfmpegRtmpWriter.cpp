#include "FfmpegRtmpWriter.h"
#include "Logging.h"
#include <mutex>
#include <cstdarg>
#include <atomic>
#include <cmath>

#if HAVE_FFMPEG
extern "C" {
 #include <libavformat/avformat.h>
 #include <libavutil/avutil.h>
 #include <libavcodec/avcodec.h>
 #include <libavutil/opt.h>
}

static inline juce::String ff_err2str(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return juce::String(buf);
}

static void ff_log_cb(void* ptr, int level, const char* fmt, va_list vl) {
    juce::ignoreUnused(ptr);
    if (level > AV_LOG_DEBUG) return; // capture DEBUG and below
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, vl);
    juce::String s(msg);
    s = s.trim();
    if (s.isNotEmpty()) LogMessage("FFMPEG-LOG: " + s);
}
#endif

static std::mutex g_ffmpegWriteMutex;

static juce::String derive_tcurl(const juce::String& rtmpUrl) {
    auto idx = rtmpUrl.indexOfIgnoreCase("/rtmp/");
    if (idx >= 0) return rtmpUrl.substring(0, idx + 5);
    int lastSlash = rtmpUrl.lastIndexOfChar('/');
    if (lastSlash > 0) return rtmpUrl.substring(0, lastSlash);
    return rtmpUrl;
}

static juce::String normalize_fb_url(const juce::String& in) {
    juce::String u = in;
    if (u.containsIgnoreCase("facebook.com") && u.containsIgnoreCase("live-api-s.facebook.com")) {
        u = u.replace("live-api-s.facebook.com", "rtmp-api.facebook.com");
    }
    return u;
}

struct FfmpegRtmpWriter::Impl {
#if HAVE_FFMPEG
    AVFormatContext* fmt = nullptr;
    AVStream* vstream = nullptr;
    AVStream* astream = nullptr;
    juce::String url;
    bool headerWritten { false };
    bool haveVideoConfig { false };
    bool haveAudioConfig { false };
    // Cached extradata for reconnects
    juce::MemoryBlock vExtra;
    juce::MemoryBlock aExtra;
    int videoWidth { 1920 };
    int videoHeight { 1080 };
    int audioSampleRate { 48000 };
    int fps { 30 };

    // Connection options
    AVDictionary* ioOpts { nullptr }; // rtmp_live, rw_timeout, etc.
    AVDictionary* muxerOpts { nullptr }; // flvflags, etc.

    std::atomic<bool> isOpen { false };

    void build_io_options() {
        if (ioOpts) { av_dict_free(&ioOpts); ioOpts = nullptr; }
        av_dict_set(&ioOpts, "rtmp_live", "live", 0);
        av_dict_set(&ioOpts, "rtmp_buffer", "3000", 0);
        av_dict_set(&ioOpts, "rtmp_flashver", "FMLE/3.0 (compatible; FMSc/1.0)", 0);
        av_dict_set(&ioOpts, "rtmp_pageurl", "https://www.facebook.com/live/producer", 0);
        juce::String tcurl = derive_tcurl(url);
        av_dict_set(&ioOpts, "rtmp_tcurl", tcurl.toRawUTF8(), 0);
        av_dict_set(&ioOpts, "rw_timeout", "20000000", 0);
        av_dict_set(&ioOpts, "stimeout", "20000000", 0);
        av_dict_set(&ioOpts, "reconnect", "1", 0);
        av_dict_set(&ioOpts, "reconnect_streamed", "1", 0);
        av_dict_set(&ioOpts, "reconnect_on_network_error", "1", 0);
        av_dict_set(&ioOpts, "reconnect_delay_max", "16", 0);
        // Prefer TLS 1.2 for stability
        av_dict_set(&ioOpts, "tls_min_protocol", "TLSv1.2", 0);
        av_dict_set(&ioOpts, "tls_max_protocol", "TLSv1.2", 0);
    }

    void build_muxer_options() {
        if (muxerOpts) { av_dict_free(&muxerOpts); muxerOpts = nullptr; }
        av_dict_set(&muxerOpts, "flvflags", "no_duration_filesize", 0);
        av_dict_set(&muxerOpts, "flush_packets", "1", 0);
    }

    bool reopen() {
        std::lock_guard<std::mutex> lk(g_ffmpegWriteMutex);
        if (fmt) {
            if (headerWritten) av_write_trailer(fmt);
            if (fmt->pb) avio_closep(&fmt->pb);
            avformat_free_context(fmt);
            fmt = nullptr; vstream = nullptr; astream = nullptr; headerWritten = false;
        }
        AVFormatContext* newfmt = nullptr;
        juce::String trimmed = url.trim();
        if (avformat_alloc_output_context2(&newfmt, nullptr, "flv", trimmed.toRawUTF8()) < 0 || newfmt == nullptr) {
            LogMessage("FFMPEG: reconnect alloc failed");
            isOpen.store(false);
            return false;
        }
        // Reduce interleave queue threshold to 1s
        av_opt_set_int(newfmt, "max_interleave_delta", 1000000, 0);

        AVStream* v = avformat_new_stream(newfmt, nullptr);
        if (!v) { avformat_free_context(newfmt); LogMessage("FFMPEG: reconnect new video stream failed"); isOpen.store(false); return false; }
        v->id = 0; v->time_base = AVRational{1, 1000}; v->codecpar->codec_type = AVMEDIA_TYPE_VIDEO; v->codecpar->codec_id = AV_CODEC_ID_H264; v->codecpar->width = videoWidth; v->codecpar->height = videoHeight;
        if (vExtra.getSize() > 0) {
            v->codecpar->extradata = (uint8_t*) av_malloc((int)vExtra.getSize() + AV_INPUT_BUFFER_PADDING_SIZE);
            if (v->codecpar->extradata) {
                memcpy(v->codecpar->extradata, vExtra.getData(), vExtra.getSize());
                memset(v->codecpar->extradata + vExtra.getSize(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
                v->codecpar->extradata_size = (int) vExtra.getSize();
            }
            haveVideoConfig = true;
        }
        AVStream* a = avformat_new_stream(newfmt, nullptr);
        if (!a) { avformat_free_context(newfmt); LogMessage("FFMPEG: reconnect new audio stream failed"); isOpen.store(false); return false; }
        a->id = 1; a->time_base = AVRational{1, 1000}; a->codecpar->codec_type = AVMEDIA_TYPE_AUDIO; a->codecpar->codec_id = AV_CODEC_ID_AAC; a->codecpar->sample_rate = audioSampleRate;
        if (aExtra.getSize() > 0) {
            a->codecpar->extradata = (uint8_t*) av_malloc((int)aExtra.getSize() + AV_INPUT_BUFFER_PADDING_SIZE);
            if (a->codecpar->extradata) {
                memcpy(a->codecpar->extradata, aExtra.getData(), aExtra.getSize());
                memset(a->codecpar->extradata + aExtra.getSize(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
                a->codecpar->extradata_size = (int) aExtra.getSize();
            }
            haveAudioConfig = true;
        }
        build_io_options();
        build_muxer_options();
        int ret = 0;
        if (!(newfmt->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open2(&newfmt->pb, trimmed.toRawUTF8(), AVIO_FLAG_WRITE, nullptr, &ioOpts);
            if (ret < 0) {
                LogMessage("FFMPEG: reconnect avio_open2 failed -> " + ff_err2str(ret));
                AVDictionary* tls = nullptr; av_dict_set(&tls, "tls_verify", "0", 0);
                ret = avio_open2(&newfmt->pb, trimmed.toRawUTF8(), AVIO_FLAG_WRITE, nullptr, &tls);
                av_dict_free(&tls);
                if (ret < 0) { avformat_free_context(newfmt); isOpen.store(false); return false; }
            }
        }
        if (haveVideoConfig) {
            if (avformat_write_header(newfmt, &muxerOpts) < 0) {
                LogMessage("FFMPEG: reconnect write_header failed");
                if (newfmt->pb) avio_closep(&newfmt->pb);
                avformat_free_context(newfmt);
                isOpen.store(false);
                return false;
            }
            headerWritten = true;
        } else {
            headerWritten = false;
        }
        fmt = newfmt; vstream = v; astream = a;
        isOpen.store(true);
        LogMessage("FFMPEG: reconnected");
        return true;
    }
#endif
};

FfmpegRtmpWriter::FfmpegRtmpWriter() : impl(std::make_unique<Impl>()) {}
FfmpegRtmpWriter::~FfmpegRtmpWriter() { close(); }

#if HAVE_FFMPEG
static inline void ff_try_write_header_internal(AVFormatContext* fmt, bool haveVideoConfig, bool& headerWritten, AVDictionary** muxerOpts) {
    if (!fmt || headerWritten) return;
    if (!haveVideoConfig) return;
    if (avformat_write_header(fmt, muxerOpts) < 0) {
        LogMessage("FFMPEG: write_header failed");
        return;
    }
    headerWritten = true;
    LogMessage("FFMPEG: write_header OK");
}
#endif

bool FfmpegRtmpWriter::open(const juce::String& url, const StreamingConfig& cfg) {
#if HAVE_FFMPEG
    juce::String normalized = normalize_fb_url(url.trim());
    LogMessage("FFMPEG: open -> " + normalized);
    avformat_network_init();
    av_log_set_level(AV_LOG_DEBUG); // more verbose
    av_log_set_callback(ff_log_cb);

    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "flv", normalized.toRawUTF8()) < 0 || fmt == nullptr) {
        LogMessage("FFMPEG: avformat_alloc_output_context2 failed");
        return false;
    }

    // Reduce interleave queue threshold to 1s
    av_opt_set_int(fmt, "max_interleave_delta", 1000000, 0);

    AVStream* v = avformat_new_stream(fmt, nullptr);
    if (!v) { LogMessage("FFMPEG: new video stream failed"); avformat_free_context(fmt); return false; }
    v->id = 0;
    v->time_base = AVRational{ 1, 1000 };
    v->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    v->codecpar->codec_id = AV_CODEC_ID_H264;
    v->codecpar->width = cfg.videoWidth;
    v->codecpar->height = cfg.videoHeight;

    AVStream* a = avformat_new_stream(fmt, nullptr);
    if (!a) { LogMessage("FFMPEG: new audio stream failed"); avformat_free_context(fmt); return false; }
    a->id = 1;
    a->time_base = AVRational{ 1, 1000 };
    a->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    a->codecpar->codec_id = AV_CODEC_ID_AAC;
    a->codecpar->sample_rate = cfg.audioSampleRate;

    // Build and keep options for reconnects
    impl->url = normalized;
    impl->videoWidth = cfg.videoWidth;
    impl->videoHeight = cfg.videoHeight;
    impl->audioSampleRate = cfg.audioSampleRate;
    impl->fps = cfg.fps;
    impl->build_io_options();
    impl->build_muxer_options();

    int ret = 0;
    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&fmt->pb, normalized.toRawUTF8(), AVIO_FLAG_WRITE, nullptr, &impl->ioOpts);
        if (ret < 0) {
            LogMessage("FFMPEG: avio_open2 failed -> " + ff_err2str(ret));
            AVDictionary* tls = nullptr; av_dict_set(&tls, "tls_verify", "0", 0);
            ret = avio_open2(&fmt->pb, normalized.toRawUTF8(), AVIO_FLAG_WRITE, nullptr, &tls);
            av_dict_free(&tls);
            if (ret < 0) { avformat_free_context(fmt); return false; }
        }
    }

    impl->fmt = fmt;
    impl->vstream = v;
    impl->astream = a;
    impl->headerWritten = false;
    impl->haveVideoConfig = false;
    impl->haveAudioConfig = false;
    impl->isOpen.store(true);
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
    impl->vExtra.setSize(size, false);
    memcpy(impl->vExtra.getData(), data, size);
    LogMessage("FFMPEG: video extradata set (SPS/PPS) size=" + juce::String((int)size));
    ff_try_write_header_internal(impl->fmt, impl->haveVideoConfig, impl->headerWritten, &impl->muxerOpts);
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
    impl->aExtra.setSize(size, false);
    memcpy(impl->aExtra.getData(), data, size);
    LogMessage("FFMPEG: audio extradata set (ASC) size=" + juce::String((int)size));
    ff_try_write_header_internal(impl->fmt, impl->haveVideoConfig, impl->headerWritten, &impl->muxerOpts);
    return true;
#else
    juce::ignoreUnused(data, size);
    return false;
#endif
}

static inline bool is_network_broken(int err) {
#if HAVE_FFMPEG
    return err == AVERROR(EPIPE) || err == AVERROR_EOF || err == AVERROR(ECONNRESET) || err == AVERROR(ETIMEDOUT) || err == AVERROR(EIO);
#else
    return false;
#endif
}

bool FfmpegRtmpWriter::writeVideoFrame(const void* data, size_t size, int64_t ptsMs, bool keyframe) {
#if HAVE_FFMPEG
    if (!impl->isOpen.load()) return false;
    if (!impl->fmt || !impl->vstream) return false;
    ff_try_write_header_internal(impl->fmt, impl->haveVideoConfig, impl->headerWritten, &impl->muxerOpts);
    if (!impl->headerWritten) return false;

    int ret = 0;
    {
        std::lock_guard<std::mutex> lk(g_ffmpegWriteMutex);
        if (!impl->isOpen.load()) return false;
        AVPacket pkt{}; av_init_packet(&pkt);
        pkt.data = (uint8_t*) data; pkt.size = (int) size; pkt.stream_index = impl->vstream->index; pkt.pts = pkt.dts = ptsMs; if (keyframe) pkt.flags |= AV_PKT_FLAG_KEY;
        // Set duration based on fps in ms
        int frameDurMs = (impl->fps > 0) ? (int) std::lround(1000.0 / (double) impl->fps) : 33;
        pkt.duration = frameDurMs;
        ret = av_interleaved_write_frame(impl->fmt, &pkt);
    }

    if (ret < 0) {
        juce::String reason = ff_err2str(ret);
        LogMessage("FFMPEG: write video frame failed -> " + reason);
        if (is_network_broken(ret)) {
            LogMessage("FFMPEG: attempting reconnect (video) reason=" + reason);
            if (impl->reopen()) {
                ff_try_write_header_internal(impl->fmt, impl->haveVideoConfig, impl->headerWritten, &impl->muxerOpts);
                if (impl->headerWritten) {
                    std::lock_guard<std::mutex> lk2(g_ffmpegWriteMutex);
                    AVPacket pkt{}; av_init_packet(&pkt);
                    pkt.data = (uint8_t*) data; pkt.size = (int) size; pkt.stream_index = impl->vstream->index; pkt.pts = pkt.dts = ptsMs; if (keyframe) pkt.flags |= AV_PKT_FLAG_KEY;
                    int frameDurMs = (impl->fps > 0) ? (int) std::lround(1000.0 / (double) impl->fps) : 33;
                    pkt.duration = frameDurMs;
                    ret = av_interleaved_write_frame(impl->fmt, &pkt);
                    if (ret >= 0) return true;
                    LogMessage("FFMPEG: retry video failed -> " + ff_err2str(ret));
                }
            }
        }
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
    if (!impl->isOpen.load()) return false;
    if (!impl->fmt || !impl->astream) return false;
    ff_try_write_header_internal(impl->fmt, impl->haveVideoConfig, impl->headerWritten, &impl->muxerOpts);
    if (!impl->headerWritten) return false;

    int ret = 0;
    {
        std::lock_guard<std::mutex> lk(g_ffmpegWriteMutex);
        if (!impl->isOpen.load()) return false;
        AVPacket pkt{}; av_init_packet(&pkt);
        pkt.data = (uint8_t*) data; pkt.size = (int) size; pkt.stream_index = impl->astream->index; pkt.pts = pkt.dts = ptsMs;
        // AAC frame duration ~ 1024 samples
        int aacFrameDurMs = (int) std::lround(1024.0 * 1000.0 / (double) impl->audioSampleRate);
        pkt.duration = aacFrameDurMs;
        ret = av_interleaved_write_frame(impl->fmt, &pkt);
    }

    if (ret < 0) {
        juce::String reason = ff_err2str(ret);
        LogMessage("FFMPEG: write audio frame failed -> " + reason);
        if (is_network_broken(ret)) {
            LogMessage("FFMPEG: attempting reconnect (audio) reason=" + reason);
            if (impl->reopen()) {
                ff_try_write_header_internal(impl->fmt, impl->haveVideoConfig, impl->headerWritten, &impl->muxerOpts);
                if (impl->headerWritten) {
                    std::lock_guard<std::mutex> lk2(g_ffmpegWriteMutex);
                    AVPacket pkt{}; av_init_packet(&pkt);
                    pkt.data = (uint8_t*) data; pkt.size = (int) size; pkt.stream_index = impl->astream->index; pkt.pts = pkt.dts = ptsMs;
                    int aacFrameDurMs = (int) std::lround(1024.0 * 1000.0 / (double) impl->audioSampleRate);
                    pkt.duration = aacFrameDurMs;
                    ret = av_interleaved_write_frame(impl->fmt, &pkt);
                    if (ret >= 0) return true;
                    LogMessage("FFMPEG: retry audio failed -> " + ff_err2str(ret));
                }
            }
        }
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
    std::lock_guard<std::mutex> lk(g_ffmpegWriteMutex);
    if (!impl->fmt) { impl->isOpen.store(false); return; }
    LogMessage("FFMPEG: close -> " + impl->url);
    impl->isOpen.store(false);
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
    if (impl->ioOpts) { av_dict_free(&impl->ioOpts); impl->ioOpts = nullptr; }
    if (impl->muxerOpts) { av_dict_free(&impl->muxerOpts); impl->muxerOpts = nullptr; }
#endif
}
