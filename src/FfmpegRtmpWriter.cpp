#include "FfmpegRtmpWriter.h"
#include "Logging.h"
#include <mutex>
#include <cstdarg>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <deque>
#include <vector>
#include <condition_variable>
#include <thread>

#if JUCE_MAC || defined(__APPLE__) || defined(__unix__)
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#endif

#if HAVE_FFMPEG
extern "C" {
 #include <libavformat/avformat.h>
 #include <libavutil/avutil.h>
 #include <libavcodec/avcodec.h>
 #include <libavutil/opt.h>
}

// Forward declaration so member methods can call it
static inline void ff_try_write_header_internal(AVFormatContext* fmt, bool haveVideoConfig, bool& headerWritten, AVDictionary** muxerOpts);

static inline juce::String ff_err2str(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return juce::String(buf);
}

static void ff_log_cb(void* ptr, int level, const char* fmt, va_list vl) {
    juce::ignoreUnused(ptr);
    if (level > AV_LOG_TRACE) return; // capture TRACE and below
    char msg[1024];
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

// NOTE: do not normalize hostnames; use the exact ingest URL provided by the service
// static juce::String normalize_fb_url(const juce::String& in) { return in; }

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
    int videoBitrateKbps { 2500 };
    int audioBitrateKbps { 128 };

    // Connection options
    AVDictionary* ioOpts { nullptr }; // rtmp_live, rw_timeout, etc.
    AVDictionary* muxerOpts { nullptr }; // flvflags, etc.

    std::atomic<bool> isOpen { false };
    struct QueuedPacket { bool isVideo { true }; bool keyframe { false }; std::vector<uint8_t> bytes; int64_t ptsMs { 0 }; int durationMs { 0 }; };
    std::deque<QueuedPacket> egressQueue; std::mutex egressMutex; std::condition_variable egressCv; std::thread egressThread; std::atomic<bool> egressRunning { false }; std::chrono::steady_clock::time_point wallStart; bool egressBaseAligned { false }; std::atomic<int64_t> lastVideoSentRelMs { 0 }; double tokensBytes { 0.0 }; double bucketCapacityBytes { 0.0 }; double fillRateBytesPerSec { 0.0 }; std::chrono::steady_clock::time_point lastTokenUpdate;

    // Reconnect/backoff state
    std::atomic<int> reconnectAttempts { 0 };
    std::chrono::steady_clock::time_point lastReconnectAt { std::chrono::steady_clock::now() - std::chrono::seconds(60) };
    std::chrono::steady_clock::time_point openedAt { std::chrono::steady_clock::now() };

    void build_io_options() {
        if (ioOpts) { av_dict_free(&ioOpts); ioOpts = nullptr; }
        av_dict_set(&ioOpts, "rtmp_live", "live", 0);
        av_dict_set(&ioOpts, "rtmp_buffer", "3000", 0);
        av_dict_set(&ioOpts, "rtmp_flashver", "FMLE/3.0 (compatible; FMSc/1.0)", 0);
        // Avoid extra hints like pageurl; keep minimal
        juce::String tcurl = derive_tcurl(url);
        av_dict_set(&ioOpts, "rtmp_tcurl", tcurl.toRawUTF8(), 0);
        av_dict_set(&ioOpts, "rw_timeout", "20000000", 0);
        av_dict_set(&ioOpts, "stimeout", "20000000", 0);
        av_dict_set(&ioOpts, "reconnect", "1", 0);
        av_dict_set(&ioOpts, "reconnect_streamed", "1", 0);
        av_dict_set(&ioOpts, "reconnect_on_network_error", "1", 0);
        av_dict_set(&ioOpts, "reconnect_delay_max", "16", 0);
        // Let TLS version negotiate automatically (YouTube prefers TLS1.3). Avoid forcing min/max.
        // Force IPv4 for stability on some networks
        av_dict_set(&ioOpts, "rtmp_dns_cache_clear", "1", 0);
        av_dict_set(&ioOpts, "dns_cache_timeout", "0", 0);
        av_dict_set(&ioOpts, "listen_timeout", "0", 0);
        av_dict_set(&ioOpts, "protocol_whitelist", "file,crypto,tcp,tls,rtmp,rtmps", 0);
        av_dict_set(&ioOpts, "rtmp_frame_type_id", "2", 0);
        // Prefer IPv4
        av_dict_set(&ioOpts, "dns_resolve_ipv4_only", "1", 0);
        // Ensure correct SNI for TLS by explicitly setting server name from URL host
        juce::String beforeHost, afterHost;
        juce::String hostOnly = extract_host(url, beforeHost, afterHost);
        if (hostOnly.isNotEmpty()) {
            av_dict_set(&ioOpts, "tls_server_name", hostOnly.toRawUTF8(), 0);
        }
    }

    void build_muxer_options() {
        if (muxerOpts) { av_dict_free(&muxerOpts); muxerOpts = nullptr; }
        av_dict_set(&muxerOpts, "flvflags", "no_duration_filesize", 0);
        // Avoid immediate flush that can bunch N packets; let interleaver pace
        av_dict_set(&muxerOpts, "flush_packets", "0", 0);
    }

    static juce::String extract_host(const juce::String& fullUrl, juce::String& beforeHost, juce::String& afterHost) {
        // crude parse: scheme://host[:port]/rest
        int schemeEnd = fullUrl.indexOf("//");
        if (schemeEnd < 0) { beforeHost = {}; afterHost = {}; return {}; }
        int hostStart = schemeEnd + 2;
        int slash = fullUrl.indexOfChar(hostStart, '/');
        juce::String hostPort = slash > 0 ? fullUrl.substring(hostStart, slash) : fullUrl.substring(hostStart);
        beforeHost = fullUrl.substring(0, hostStart);
        afterHost = slash > 0 ? fullUrl.substring(slash) : juce::String();
        int colon = hostPort.indexOfChar(':');
        return colon > 0 ? hostPort.substring(0, colon) : hostPort;
    }

    static juce::String resolve_ipv4(const juce::String& host) {
#if JUCE_MAC || defined(__APPLE__) || defined(__unix__)
        struct addrinfo hints{}; memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET; // IPv4 only
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        int rc = getaddrinfo(host.toRawUTF8(), nullptr, &hints, &res);
        if (rc != 0 || res == nullptr) return {};
        char buf[INET_ADDRSTRLEN] = {0};
        void* addrPtr = &((struct sockaddr_in*)res->ai_addr)->sin_addr;
        const char* s = inet_ntop(AF_INET, addrPtr, buf, sizeof(buf));
        juce::String out = s ? juce::String(s) : juce::String();
        freeaddrinfo(res);
        return out;
#else
        juce::ignoreUnused(host);
        return {};
#endif
    }

    static juce::String rewrite_url_ipv4_if_possible(const juce::String& inputUrl) {
        juce::String before, after;
        juce::String host = extract_host(inputUrl, before, after);
        if (host.isEmpty()) return inputUrl;
        juce::String ipv4 = resolve_ipv4(host);
        if (ipv4.isEmpty()) return inputUrl;
        // Preserve port if present inside 'after' (it's part of before+hostPort actually); since we replace only host token, keep before/after unchanged
        return before + ipv4 + after;
    }

    bool can_attempt_reconnect_now() {
        using namespace std::chrono;
        auto now = steady_clock::now();
        // Exponential backoff: 2,4,8,16 up to 30s
        int attempt = reconnectAttempts.load();
        int backoffSec = std::min(30, std::max(2, 1 << std::min(attempt, 4))); // 2,4,8,16,16,16...
        auto due = lastReconnectAt + seconds(backoffSec);
        return now >= due;
    }

    void note_reconnect_attempt(bool success) {
        lastReconnectAt = std::chrono::steady_clock::now();
        if (success) reconnectAttempts.store(0); else reconnectAttempts.fetch_add(1);
    }

    bool reopen() {
        std::lock_guard<std::mutex> lk(g_ffmpegWriteMutex);
        // Preview guard: if repeated failures occur shortly after opening, avoid
        // hammering the server while user hasn't clicked "Go Live" yet.
        using namespace std::chrono;
        auto sinceOpen = steady_clock::now() - openedAt;
        if (sinceOpen < seconds(20) && reconnectAttempts.load() >= 2) {
            LogMessage("FFMPEG: preview guard active (too many early failures) — not reconnecting yet");
            return false;
        }
        if (!can_attempt_reconnect_now()) {
            LogMessage("FFMPEG: backoff active, skipping reconnect");
            return false;
        }
        if (fmt) {
            if (headerWritten) av_write_trailer(fmt);
            if (fmt->pb) avio_closep(&fmt->pb);
            avformat_free_context(fmt);
            fmt = nullptr; vstream = nullptr; astream = nullptr; headerWritten = false;
        }
        AVFormatContext* newfmt = nullptr;
        juce::String trimmed = url.trim();
        // Keep original hostname to preserve RTMP vhost/TLS SNI; do not rewrite to IPv4.
        if (avformat_alloc_output_context2(&newfmt, nullptr, "flv", trimmed.toRawUTF8()) < 0 || newfmt == nullptr) {
            LogMessage("FFMPEG: reconnect alloc failed");
            isOpen.store(false);
            note_reconnect_attempt(false);
            return false;
        }
        // Tighten interleave queue threshold to 0ms (no backlog bursts)
        av_opt_set_int(newfmt, "max_interleave_delta", 0, 0);

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
                if (ret < 0) { avformat_free_context(newfmt); isOpen.store(false); note_reconnect_attempt(false); return false; }
            }
        }
        if (haveVideoConfig) {
            if (avformat_write_header(newfmt, &muxerOpts) < 0) {
                LogMessage("FFMPEG: reconnect write_header failed");
                if (newfmt->pb) avio_closep(&newfmt->pb);
                avformat_free_context(newfmt);
                isOpen.store(false);
                note_reconnect_attempt(false);
                return false;
            }
            headerWritten = true;
        } else {
            headerWritten = false;
        }
        fmt = newfmt; vstream = v; astream = a;
        isOpen.store(true);
        LogMessage("FFMPEG: reconnected");
        note_reconnect_attempt(true);
        return true;
    }

    void startEgressIfNeeded() {
        if (egressRunning.load()) return;
        egressRunning.store(true);
        wallStart = std::chrono::steady_clock::now();
        egressBaseAligned = false;
        tokensBytes = 0.0;
        bucketCapacityBytes = std::max(1024.0, (double)(videoBitrateKbps + audioBitrateKbps) * 1000.0 / 8.0);
        fillRateBytesPerSec = (double)(videoBitrateKbps + audioBitrateKbps) * 1000.0 / 8.0;
        lastTokenUpdate = std::chrono::steady_clock::now();
        egressThread = std::thread([this]{ egressLoop(); });
    }

    void stopEgress() {
        if (!egressRunning.load()) return;
        egressRunning.store(false);
        egressCv.notify_all();
        if (egressThread.joinable()) egressThread.join();
        std::lock_guard<std::mutex> lk(egressMutex);
        egressQueue.clear();
        egressBaseAligned = false;
    }

    void egressLoop() {
        while (egressRunning.load()) {
            QueuedPacket pkt;
            bool hasPkt = false;
            {
                std::unique_lock<std::mutex> lk(egressMutex);
                egressCv.wait_for(lk, std::chrono::milliseconds(5), [&]{ return !egressQueue.empty() || !egressRunning.load(); });
                if (!egressRunning.load()) break;
                if (egressQueue.empty()) continue;
                if (!egressBaseAligned) {
                    wallStart = std::chrono::steady_clock::now() - std::chrono::milliseconds(egressQueue.front().ptsMs);
                    egressBaseAligned = true;
                }
                // Drop late non-keyframes if backlog too large
                if (egressQueue.front().isVideo && !egressQueue.front().keyframe) {
                    int64_t headPts = egressQueue.front().ptsMs;
                    int64_t lag = headPts - lastVideoSentRelMs.load();
                    if (lag > 1000) { egressQueue.pop_front(); continue; }
                }
                // Wait until due
                auto now = std::chrono::steady_clock::now();
                int64_t elapsedMs = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(now - wallStart).count();
                if (egressQueue.front().ptsMs > elapsedMs) {
                    int64_t waitMs = std::min<int64_t>(egressQueue.front().ptsMs - elapsedMs, 5);
                    lk.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds((int)waitMs));
                    continue;
                }
                pkt = std::move(egressQueue.front());
                egressQueue.pop_front();
                hasPkt = true;
            }
            if (!hasPkt) continue;

            // Token bucket pacing
            auto now2 = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now2 - lastTokenUpdate).count();
            lastTokenUpdate = now2;
            tokensBytes = std::min(bucketCapacityBytes, tokensBytes + dt * fillRateBytesPerSec);
            size_t pktSize = pkt.bytes.size();
            if ((double)pktSize > tokensBytes) {
                double need = ((double)pktSize - tokensBytes) / fillRateBytesPerSec;
                if (need > 0.0) std::this_thread::sleep_for(std::chrono::milliseconds((int) std::ceil(need * 1000.0)));
                now2 = std::chrono::steady_clock::now();
                dt = std::chrono::duration<double>(now2 - lastTokenUpdate).count();
                lastTokenUpdate = now2;
                tokensBytes = std::min(bucketCapacityBytes, tokensBytes + dt * fillRateBytesPerSec);
            }
            if ((double)pktSize <= tokensBytes) tokensBytes -= (double)pktSize;

            // Send via FFmpeg
            std::lock_guard<std::mutex> lk(g_ffmpegWriteMutex);
            if (!isOpen.load()) continue;
            ff_try_write_header_internal(fmt, haveVideoConfig, headerWritten, &muxerOpts);
            if (!headerWritten) continue;
            AVPacket avpkt{}; av_init_packet(&avpkt);
            avpkt.data = pkt.bytes.data(); avpkt.size = (int) pkt.bytes.size();
            avpkt.stream_index = pkt.isVideo ? vstream->index : astream->index;
            avpkt.pts = avpkt.dts = pkt.ptsMs;
            if (pkt.isVideo && pkt.keyframe) avpkt.flags |= AV_PKT_FLAG_KEY;
            avpkt.duration = pkt.durationMs;
            int ret = av_interleaved_write_frame(fmt, &avpkt);
            if (ret >= 0 && pkt.isVideo) lastVideoSentRelMs.store(pkt.ptsMs);
        }
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
    // Enable deep TLS logging (GnuTLS)
    setenv("GNUTLS_DEBUG_LEVEL", "6", 1);

    juce::String inputUrl = url.isNotEmpty() ? url.trim() : cfg.rtmpUrl.trim();
    if (inputUrl.isEmpty()) { LogMessage("FFMPEG: open failed (empty URL)"); return false; }
    // Force IPv4 if possible to avoid AAAA-only issues
    juce::String finalUrl = inputUrl;
    LogMessage("FFMPEG: open -> " + finalUrl);
    avformat_network_init();
    av_log_set_level(AV_LOG_TRACE);
    av_log_set_callback(ff_log_cb);

    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "flv", finalUrl.toRawUTF8()) < 0 || fmt == nullptr) {
        LogMessage("FFMPEG: avformat_alloc_output_context2 failed");
        return false;
    }

    // Tighten interleave queue threshold to 0ms (no backlog bursts)
    av_opt_set_int(fmt, "max_interleave_delta", 0, 0);

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
    impl->url = finalUrl;
    impl->videoWidth = cfg.videoWidth;
    impl->videoHeight = cfg.videoHeight;
    impl->audioSampleRate = cfg.audioSampleRate;
    impl->fps = cfg.fps;
    impl->videoBitrateKbps = cfg.videoBitrateKbps;
    impl->audioBitrateKbps = cfg.audioBitrateKbps;
    impl->openedAt = std::chrono::steady_clock::now();
    impl->reconnectAttempts.store(0);
    impl->build_io_options();
    impl->build_muxer_options();

    int ret = 0;
    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&fmt->pb, finalUrl.toRawUTF8(), AVIO_FLAG_WRITE, nullptr, &impl->ioOpts);
        if (ret < 0) {
            LogMessage("FFMPEG: avio_open2 failed -> " + ff_err2str(ret));
            AVDictionary* tls = nullptr; av_dict_set(&tls, "tls_verify", "0", 0);
            ret = avio_open2(&fmt->pb, finalUrl.toRawUTF8(), AVIO_FLAG_WRITE, nullptr, &tls);
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
    impl->startEgressIfNeeded();
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
    impl->startEgressIfNeeded();
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
    if (!impl->isOpen.load() || !impl->fmt || !impl->vstream) return false;
    // Avoid pre-header backlog: drop frames until header is written
    if (!impl->headerWritten) return true;
    impl->startEgressIfNeeded();
    int frameDurMs = (impl->fps > 0) ? (int) std::lround(1000.0 / (double) impl->fps) : 33;
    Impl::QueuedPacket qp;
    qp.isVideo = true; qp.keyframe = keyframe; qp.ptsMs = ptsMs; qp.durationMs = frameDurMs; qp.bytes.assign((const uint8_t*)data, (const uint8_t*)data + size);
    {
        std::lock_guard<std::mutex> lk(impl->egressMutex);
        impl->egressQueue.emplace_back(std::move(qp));
    }
    impl->egressCv.notify_one();
    return true;
#else
    juce::ignoreUnused(data, size, ptsMs, keyframe);
    return false;
#endif
}

bool FfmpegRtmpWriter::writeAudioFrame(const void* data, size_t size, int64_t ptsMs) {
#if HAVE_FFMPEG
    if (!impl->isOpen.load() || !impl->fmt || !impl->astream) return false;
    // Avoid pre-header backlog and ensure audio after first video
    if (!impl->headerWritten) return true;
    impl->startEgressIfNeeded();
    int aacFrameDurMs = (int) std::lround(1024.0 * 1000.0 / (double) impl->audioSampleRate);
    Impl::QueuedPacket qp;
    qp.isVideo = false; qp.keyframe = false; qp.ptsMs = ptsMs; qp.durationMs = aacFrameDurMs; qp.bytes.assign((const uint8_t*)data, (const uint8_t*)data + size);
    {
        std::lock_guard<std::mutex> lk(impl->egressMutex);
        impl->egressQueue.emplace_back(std::move(qp));
    }
    impl->egressCv.notify_one();
    return true;
#else
    juce::ignoreUnused(data, size, ptsMs);
    return false;
#endif
}

void FfmpegRtmpWriter::close() {
#if HAVE_FFMPEG
    impl->stopEgress();
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
