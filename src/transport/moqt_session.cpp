#include "openmoq/publisher/transport/moqt_session.h"

#include <string>
#include <vector>

namespace openmoq::publisher::transport {

namespace {

std::vector<std::uint8_t> to_bytes(std::string_view value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

}  // namespace

MoqtSession::MoqtSession(PublisherTransport& transport) : transport_(transport) {}

TransportStatus MoqtSession::connect(const EndpointConfig& endpoint, const TlsConfig& tls) {
    TransportStatus status = transport_.configure(endpoint, tls);
    if (!status.ok) {
        return status;
    }

    status = transport_.connect();
    if (!status.ok) {
        return status;
    }

    return ensure_control_stream();
}

TransportStatus MoqtSession::publish(const openmoq::publisher::PublishPlan& plan) {
    if (transport_.state() != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

    TransportStatus status = ensure_control_stream();
    if (!status.ok) {
        return status;
    }

    status = write_text_frame(control_stream_id_,
                              std::string("SETUP ") + openmoq::publisher::to_string(plan.draft.version) + "\n",
                              false);
    if (!status.ok) {
        return status;
    }

    status = write_text_frame(control_stream_id_,
                              std::string("ANNOUNCE init objects=") + std::to_string(plan.objects.size()) + "\n",
                              false);
    if (!status.ok) {
        return status;
    }

    for (const auto& object : plan.objects) {
        std::uint64_t stream_id = 0;
        status = transport_.open_stream(StreamDirection::kUnidirectional, stream_id);
        if (!status.ok) {
            return status;
        }

        const std::string header =
            "OBJECT " + object.track_name + " " + std::to_string(object.group_id) + " " +
            std::to_string(object.object_id) + "\n";
        status = write_text_frame(stream_id, header, object.owned_payload.empty() && object.payload.size == 0);
        if (!status.ok) {
            return status;
        }

        if (object.owned_payload.empty() && object.payload.size != 0) {
            return TransportStatus::failure("publish plan must be materialized before session publication");
        }

        if (!object.owned_payload.empty()) {
            status = transport_.write_stream(stream_id, object.owned_payload, true);
            if (!status.ok) {
                return status;
            }
        }
    }

    return write_text_frame(control_stream_id_, "ANNOUNCE_DONE\n", false);
}

TransportStatus MoqtSession::close(std::uint64_t application_error_code) {
    control_stream_open_ = false;
    control_stream_id_ = 0;
    return transport_.close(application_error_code);
}

TransportStatus MoqtSession::ensure_control_stream() {
    if (control_stream_open_) {
        return TransportStatus::success();
    }

    TransportStatus status =
        transport_.open_stream(StreamDirection::kBidirectional, control_stream_id_);
    if (!status.ok) {
        return status;
    }

    control_stream_open_ = true;
    return TransportStatus::success();
}

TransportStatus MoqtSession::write_text_frame(std::uint64_t stream_id, std::string_view frame, bool fin) {
    const std::vector<std::uint8_t> bytes = to_bytes(frame);
    return transport_.write_stream(stream_id, bytes, fin);
}

}  // namespace openmoq::publisher::transport
