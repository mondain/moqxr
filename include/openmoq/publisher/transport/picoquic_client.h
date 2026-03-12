#pragma once

#include <memory>
#include <optional>

#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher::transport {

class PicoquicClient final : public PublisherTransport {
public:
    PicoquicClient();
    ~PicoquicClient() override;

    TransportStatus configure(const EndpointConfig& endpoint, const TlsConfig& tls) override;
    TransportStatus connect() override;
    ConnectionState state() const override;
    TransportStatus open_stream(StreamDirection direction, std::uint64_t& stream_id) override;
    TransportStatus write_stream(std::uint64_t stream_id,
                                 std::span<const std::uint8_t> bytes,
                                 bool fin) override;
    TransportStatus close(std::uint64_t application_error_code) override;

private:
public:
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::optional<EndpointConfig> endpoint_;
    std::optional<TlsConfig> tls_;
    ConnectionState state_ = ConnectionState::kIdle;
    std::uint64_t next_bidirectional_stream_id_ = 0;
    std::uint64_t next_unidirectional_stream_id_ = 2;
};

}  // namespace openmoq::publisher::transport
