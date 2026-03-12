#pragma once

#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher::transport {

class MoqtSession {
public:
    explicit MoqtSession(PublisherTransport& transport);

    TransportStatus connect(const EndpointConfig& endpoint, const TlsConfig& tls);
    TransportStatus publish(const openmoq::publisher::PublishPlan& plan);
    TransportStatus close(std::uint64_t application_error_code = 0);

private:
    TransportStatus ensure_control_stream();
    TransportStatus write_text_frame(std::uint64_t stream_id, std::string_view frame, bool fin);

    PublisherTransport& transport_;
    std::uint64_t control_stream_id_ = 0;
    bool control_stream_open_ = false;
};

}  // namespace openmoq::publisher::transport
