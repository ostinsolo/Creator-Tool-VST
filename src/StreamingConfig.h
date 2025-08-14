#pragma once
#include <juce_core/juce_core.h>

struct StreamingConfig {
    // Single primary RTMP URL (backward-compatible)
    juce::String rtmpUrl;            // e.g. rtmps://live-api.facebook.com:443/rtmp/<stream-key>

    // Multistream: optional list of endpoints (if non-empty, use these instead of rtmpUrl)
    struct RtmpEndpoint { juce::String url; bool enabled { true }; };
    juce::Array<RtmpEndpoint> endpoints; // connect to all enabled endpoints

    // Optional: use a local relay (e.g., SRS/nginx-rtmp) and publish only once
    bool useLocalRelay { false };
    juce::String relayUrl;           // e.g. rtmp://127.0.0.1/live/stream

    int videoWidth { 1920 };
    int videoHeight { 1080 };
    int fps { 30 };
    int videoBitrateKbps { 6000 };   // 6 Mbps default
    int keyframeIntervalSec { 2 };   // GOP 2s
    bool constantBitrate { true };
    bool useHardwareEncoder { true };

    int audioSampleRate { 48000 };
    int audioChannels { 2 };
    int audioBitrateKbps { 160 };    // 160 kbps
};
