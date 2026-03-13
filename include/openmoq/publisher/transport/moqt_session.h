#pragma once

#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/transport/publisher_transport.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace openmoq::publisher::transport {

class MoqtSession {
public:
    explicit MoqtSession(PublisherTransport& transport,
                         std::string track_namespace = "media",
                         bool auto_forward = false,
                         bool paced = false);

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
    bool paced_ = false;
    std::optional<EndpointConfig> endpoint_;
    std::uint64_t control_stream_id_ = 0;
    std::uint64_t peer_max_request_id_ = 0;
    bool control_stream_open_ = false;
    bool setup_complete_ = false;
};

}  // namespace openmoq::publisher::transport
