#include "../src/LiveStreamer.h"
#include "../src/ScreenRecorder.h"
#include "../src/StreamingConfig.h"
#include "../src/Logging.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <CoreVideo/CoreVideo.h>

using namespace streaming;

static void printUsage() {
    juce::String msg = "Usage: StreamerTest [--url <rtmp(s)_url>] [--profile <name>] [--preset <name>] [--seconds <N>] [--synthetic]\n"
                       "Presets: youtube_720p30, youtube_1080p30, facebook_720p30, facebook_1080p30, facebook_1080p60\n";
    LogMessage(msg);
}

static CVPixelBufferRef makeBGRAFrame(int width, int height, uint8_t r, uint8_t g, uint8_t b) {
    CVPixelBufferRef pixel = nullptr;
    const OSType fmt = kCVPixelFormatType_32BGRA;
    CVReturn rc = CVPixelBufferCreate(kCFAllocatorDefault, width, height, fmt, nullptr, &pixel);
    if (rc != kCVReturnSuccess || pixel == nullptr) return nullptr;
    CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
    uint8_t* base = (uint8_t*) CVPixelBufferGetBaseAddress(pixel);
    const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixel);
    for (int y = 0; y < height; ++y) {
        uint8_t* row = base + y * bytesPerRow;
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 0xFF;
        }
    }
    CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
    return pixel;
}

static juce::File keysConfigFile() {
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("Creator Tool");
    if (!dir.exists()) dir.createDirectory();
    return dir.getChildFile("stream_keys.json");
}

static juce::String urlFromProfile(const juce::String& profileName) {
    juce::File f = keysConfigFile();
    if (!f.existsAsFile()) {
        // Create default template with YouTube and Facebook
        juce::DynamicObject* yt = new juce::DynamicObject();
        yt->setProperty("name", "youtube");
        yt->setProperty("urlPrefix", "rtmp://a.rtmp.youtube.com/live2/");
        yt->setProperty("streamKey", "yped-764s-v435-151x-d89q");
        yt->setProperty("fullUrl", ""); // will be constructed at load

        juce::DynamicObject* fb = new juce::DynamicObject();
        fb->setProperty("name", "facebook");
        fb->setProperty("fullUrl", ""); // paste full secure_stream_url here

        juce::Array<juce::var> profiles;
        profiles.add(juce::var(yt));
        profiles.add(juce::var(fb));

        juce::DynamicObject* root = new juce::DynamicObject();
        root->setProperty("profiles", profiles);
        juce::var json(root);
        f.replaceWithText(juce::JSON::toString(json, true));
        LogMessage("CLI: wrote default key store -> " + f.getFullPathName());
    }

    juce::String text = f.loadFileAsString();
    juce::var v = juce::JSON::parse(text);
    if (!v.isObject()) return {};
    auto* obj = v.getDynamicObject();
    juce::var profiles = obj->getProperty("profiles");
    if (!profiles.isArray()) return {};
    auto* arr = profiles.getArray();
    for (auto& item : *arr) {
        if (!item.isObject()) continue;
        auto* po = item.getDynamicObject();
        juce::String name = po->getProperty("name").toString();
        if (!name.equalsIgnoreCase(profileName)) continue;
        juce::String full = po->getProperty("fullUrl").toString();
        if (full.isNotEmpty()) return full;
        juce::String prefix = po->getProperty("urlPrefix").toString();
        juce::String key = po->getProperty("streamKey").toString();
        if (prefix.isNotEmpty() && key.isNotEmpty()) return prefix + key;
        return {};
    }
    return {};
}

int main(int argc, char** argv) {
    juce::ignoreUnused(argc, argv);

    juce::String url;
    int runSeconds = 900; // default 15 minutes
    bool useSynthetic = false;
    juce::String profile;
    juce::String preset;
    int overrideVideoKbps = -1;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            url = argv[++i];
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            runSeconds = juce::String(argv[++i]).getIntValue();
        } else if (std::strcmp(argv[i], "--synthetic") == 0) {
            useSynthetic = true;
        } else if (std::strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            profile = argv[++i];
        } else if (std::strcmp(argv[i], "--preset") == 0 && i + 1 < argc) {
            preset = argv[++i];
        } else if (std::strcmp(argv[i], "--videoKbps") == 0 && i + 1 < argc) {
            overrideVideoKbps = juce::String(argv[++i]).getIntValue();
        }
    }

    if (url.isEmpty() && profile.isNotEmpty()) url = urlFromProfile(profile);
    if (url.isEmpty()) LogMessage("CLI: no URL provided; use --url or --profile to select a saved key");

    LogMessage("CLI: StreamerTest starting");

    StreamingConfig cfg;
    cfg.fps = 30;
    cfg.videoWidth = 1280;
    cfg.videoHeight = 720;
    cfg.videoBitrateKbps = 2500;
    cfg.keyframeIntervalSec = 2;
    cfg.audioSampleRate = 48000;
    cfg.audioChannels = 2;
    cfg.audioBitrateKbps = 128;
    cfg.rtmpUrl = url;

    if (preset.isNotEmpty()) {
        auto p = preset.toLowerCase();
        if (p == "youtube_720p30") {
            cfg.videoWidth = 1280; cfg.videoHeight = 720; cfg.fps = 30; cfg.videoBitrateKbps = 2500;
            cfg.audioSampleRate = 48000; cfg.audioBitrateKbps = 128; cfg.keyframeIntervalSec = 2;
        } else if (p == "youtube_1080p30") {
            cfg.videoWidth = 1920; cfg.videoHeight = 1080; cfg.fps = 30; cfg.videoBitrateKbps = 6000;
            cfg.audioSampleRate = 48000; cfg.audioBitrateKbps = 128; cfg.keyframeIntervalSec = 2;
        } else if (p == "facebook_720p30") {
            cfg.videoWidth = 1280; cfg.videoHeight = 720; cfg.fps = 30; cfg.videoBitrateKbps = 2500;
            cfg.audioSampleRate = 48000; cfg.audioBitrateKbps = 128; cfg.keyframeIntervalSec = 2;
        } else if (p == "facebook_1080p30") {
            cfg.videoWidth = 1920; cfg.videoHeight = 1080; cfg.fps = 30; cfg.videoBitrateKbps = 6000;
            cfg.audioSampleRate = 48000; cfg.audioBitrateKbps = 128; cfg.keyframeIntervalSec = 2;
        } else if (p == "facebook_1080p60") {
            cfg.videoWidth = 1920; cfg.videoHeight = 1080; cfg.fps = 60; cfg.videoBitrateKbps = 9000;
            cfg.audioSampleRate = 48000; cfg.audioBitrateKbps = 160; cfg.keyframeIntervalSec = 2;
        }
    }

    if (overrideVideoKbps > 0) {
        cfg.videoBitrateKbps = overrideVideoKbps;
    }

    LiveStreamer streamer;
    if (!streamer.start(cfg)) {
        LogMessage("CLI: streamer.start failed");
        return 1;
    }

    std::atomic<bool> running{true};

    // Audio thread: 1 kHz sine into AAC path
    const double freq = 1000.0;
    const int block = 512;
    juce::AudioBuffer<float> tone(2, block);
    double phase = 0.0;
    const double inc = 2.0 * juce::MathConstants<double>::pi * freq / (double) cfg.audioSampleRate;

    auto audioThread = std::thread([&]{
        while (running.load()) {
            for (int i = 0; i < block; ++i) {
                float s = (float) std::sin(phase);
                phase += inc; if (phase > 2.0 * juce::MathConstants<double>::pi) phase -= 2.0 * juce::MathConstants<double>::pi;
                for (int c = 0; c < tone.getNumChannels(); ++c) tone.setSample(c, i, s * 0.1f);
            }
            streamer.pushAudioPCM(tone, block, (double) cfg.audioSampleRate, tone.getNumChannels());
            std::this_thread::sleep_for(std::chrono::milliseconds((int) llround(1000.0 * (double) block / (double) cfg.audioSampleRate)));
        }
    });

    std::unique_ptr<ScreenRecorder> cap;
    std::unique_ptr<std::thread> videoThread;

    if (useSynthetic) {
        // Generate BGRA frames @ cfg.fps with color cycling
        videoThread = std::make_unique<std::thread>([&]{
            const int64_t frameDurationMs = (int64_t) llround(1000.0 / (double) cfg.fps);
            int64_t ptsMs = 0;
            int t = 0;
            while (running.load()) {
                uint8_t r = (uint8_t) ((std::sin(t * 0.05) * 0.5 + 0.5) * 255.0);
                uint8_t g = (uint8_t) ((std::sin(t * 0.07 + 2.0) * 0.5 + 0.5) * 255.0);
                uint8_t b = (uint8_t) ((std::sin(t * 0.09 + 4.0) * 0.5 + 0.5) * 255.0);
                CVPixelBufferRef pb = makeBGRAFrame(cfg.videoWidth, cfg.videoHeight, r, g, b);
                if (pb != nullptr) {
                    streamer.pushPixelBuffer(pb, ptsMs);
                    CVBufferRelease(pb);
                }
                ++t;
                ptsMs += frameDurationMs;
                std::this_thread::sleep_for(std::chrono::milliseconds((long) frameDurationMs));
            }
        });
    } else {
        cap = std::make_unique<ScreenRecorder>();
        cap->setFrameCallback([&](void* pixelBuffer, int64_t ptsMs){
            streamer.pushPixelBuffer(pixelBuffer, ptsMs);
        });
        cap->setCaptureResolution(cfg.videoWidth, cfg.videoHeight);
        if (!cap->startStreamOnly()) {
            LogMessage("CLI: startStreamOnly failed");
            running.store(false);
            audioThread.join();
            return 1;
        }
    }

    if (runSeconds <= 0) {
        LogMessage("CLI: streaming... press Ctrl+C to stop");
        while (running.load()) std::this_thread::sleep_for(std::chrono::seconds(10));
    } else {
        LogMessage("CLI: streaming for " + juce::String(runSeconds) + " seconds...");
        std::this_thread::sleep_for(std::chrono::seconds(runSeconds));
    }

    running.store(false);
    audioThread.join();
    if (videoThread) videoThread->join();
    if (cap) cap->stop();
    streamer.stop();
    LogMessage("CLI: done");
    return 0;
}
