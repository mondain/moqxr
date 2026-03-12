#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace openmoq::publisher::transport {

enum class StreamDirection {
    kBidirectional,
    kUnidirectional,
};

enum class ConnectionState {
    kIdle,
    kConnecting,
    kConnected,
    kClosed,
    kFailed,
};

struct EndpointConfig {
    std::string host;
    std::uint16_t port = 0;
    std::string alpn = "moqt";
};

struct TlsConfig {
    std::string certificate_path;
    std::string private_key_path;
    std::string ca_path;
    bool insecure_skip_verify = false;
};

struct TransportStatus {
    bool ok = true;
    std::string message;

    static TransportStatus success();
    static TransportStatus failure(std::string_view error_message);
};

class PublisherTransport {
public:
    virtual ~PublisherTransport() = default;

    virtual TransportStatus configure(const EndpointConfig& endpoint, const TlsConfig& tls) = 0;
    virtual TransportStatus connect() = 0;
    virtual ConnectionState state() const = 0;
    virtual TransportStatus open_stream(StreamDirection direction, std::uint64_t& stream_id) = 0;
    virtual TransportStatus write_stream(std::uint64_t stream_id,
                                         std::span<const std::uint8_t> bytes,
                                         bool fin) = 0;
    virtual TransportStatus close(std::uint64_t application_error_code) = 0;
};

}  // namespace openmoq::publisher::transport
