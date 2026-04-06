#pragma once

#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/transport/publisher_transport.h"

#include <optional>
#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace openmoq::publisher::transport {

class MoqtSession {
public:
    explicit MoqtSession(PublisherTransport& transport,
                         std::string track_namespace,
                         bool auto_forward,
                         bool publish_catalog,
                         bool paced,
                         std::chrono::seconds subscriber_timeout);

    explicit MoqtSession(PublisherTransport& transport,
                         std::string track_namespace = "media",
                         bool auto_forward = false,
                         bool publish_catalog = false,
                         bool paced = false,
                         bool loop = false,
                         std::chrono::seconds subscriber_timeout = std::chrono::seconds(3));

    TransportStatus connect(const EndpointConfig& endpoint, const TlsConfig& tls);
    TransportStatus publish(const openmoq::publisher::PublishPlan& plan);
    TransportStatus close(std::uint64_t application_error_code = 0);

private:
    TransportStatus ensure_setup(openmoq::publisher::DraftVersion draft);
    TransportStatus ensure_control_stream();
    TransportStatus write_frame(std::uint64_t stream_id, std::span<const std::uint8_t> frame, bool fin);

    PublisherTransport& transport_;
    std::string track_namespace_;
    bool auto_forward_ = false;
    bool publish_catalog_ = false;
    bool paced_ = false;
    bool loop_ = false;
    std::chrono::seconds subscriber_timeout_ = std::chrono::seconds(3);
    std::optional<EndpointConfig> endpoint_;
    std::uint64_t control_stream_id_ = 0;
    std::uint64_t peer_max_request_id_ = 0;
    std::vector<std::uint8_t> pending_control_bytes_;
    bool control_stream_open_ = false;
    bool setup_complete_ = false;
};

}  // namespace openmoq::publisher::transport
