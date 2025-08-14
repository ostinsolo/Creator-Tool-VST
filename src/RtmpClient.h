#pragma once
#include <juce_core/juce_core.h>
#include "StreamingConfig.h"

namespace streaming {

class RtmpClient {
public:
    RtmpClient() = default;
    ~RtmpClient() = default;

    bool connect(const StreamingConfig& cfg); // rtmpUrl
    bool sendChunk(const void* data, size_t size);
    void close();
};

class RtmpMultiPublisher {
public:
    bool connectAll(const StreamingConfig& cfg); // uses endpoints or relay
    bool sendChunkAll(const void* data, size_t size); // fan-out
    void closeAll();
private:
    struct Conn { std::unique_ptr<RtmpClient> client; bool ok { false }; };
    juce::Array<Conn> conns;
};

} // namespace streaming
