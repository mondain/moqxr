#include "openmoq/publisher/transport/moqt_session.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace openmoq::publisher::transport {

namespace {

bool control_message_complete(std::span<const std::uint8_t> bytes, std::size_t& message_size) {
    std::size_t offset = 0;
    std::uint64_t message_type = 0;
    if (!decode_varint(bytes, offset, message_type)) {
        return false;
    }
    static_cast<void>(message_type);

    if (offset + 2 > bytes.size()) {
        return false;
    }

    const std::size_t payload_length =
        (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
    message_size = offset + 2 + payload_length;
    return bytes.size() >= message_size;
}

std::span<const std::uint8_t> object_payload(const openmoq::publisher::CmsfObject& object) {
    if (!object.owned_payload.empty()) {
        return object.owned_payload;
    }
    return {};
}

struct PublishedTrack {
    std::string name;
    std::uint64_t alias = 0;
    std::uint64_t request_id = 0;
    std::size_t largest_group_id = 0;
    std::size_t largest_object_id = 0;
    bool content_exists = false;
};

std::vector<PublishedTrack> build_published_tracks(const openmoq::publisher::PublishPlan& plan) {
    std::map<std::string, PublishedTrack> tracks;
    std::uint64_t next_alias = 0;
    for (const auto& object : plan.objects) {
        auto [it, inserted] = tracks.emplace(object.track_name,
                                             PublishedTrack{
                                                 .name = object.track_name,
                                                 .alias = next_alias,
                                             });
        if (inserted) {
            ++next_alias;
        }
        it->second.content_exists = true;
        if (object.group_id > it->second.largest_group_id ||
            (object.group_id == it->second.largest_group_id && object.object_id > it->second.largest_object_id)) {
            it->second.largest_group_id = object.group_id;
            it->second.largest_object_id = object.object_id;
        }
    }

    std::vector<PublishedTrack> ordered_tracks;
    ordered_tracks.reserve(tracks.size());
    for (auto& [name, track] : tracks) {
        static_cast<void>(name);
        ordered_tracks.push_back(track);
    }
    return ordered_tracks;
}

}  // namespace

MoqtSession::MoqtSession(PublisherTransport& transport) : transport_(transport) {}

TransportStatus MoqtSession::connect(const EndpointConfig& endpoint, const TlsConfig& tls) {
    endpoint_ = endpoint;
    setup_complete_ = false;
    peer_max_request_id_ = 0;
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

    TransportStatus status = ensure_setup(plan.draft.version);
    if (!status.ok) {
        return status;
    }

    const std::vector<PublishedTrack> tracks = build_published_tracks(plan);
    const std::uint64_t namespace_request_id = 0;
    const std::uint64_t largest_request_id = tracks.empty() ? namespace_request_id : namespace_request_id + tracks.size();
    if (largest_request_id > peer_max_request_id_) {
        return TransportStatus::failure("peer SERVER_SETUP max_request_id is too small for this publish");
    }

    NamespaceMessage namespace_message{
        .draft = plan.draft.version,
        .track_namespace = std::string(kDefaultTrackNamespace),
        .request_id = namespace_request_id,
    };
    status = write_frame(control_stream_id_, encode_namespace_message(namespace_message), false);
    if (!status.ok) {
        return status;
    }

    std::map<std::string, PublishedTrack> tracks_by_name;
    std::uint64_t next_request_id = 1;
    for (auto track : tracks) {
        track.request_id = next_request_id++;
        tracks_by_name.emplace(track.name, track);

        status = write_frame(control_stream_id_,
                             encode_track_message({
                                 .draft = plan.draft.version,
                                 .track_name = track.name,
                                 .track_namespace = std::string(kDefaultTrackNamespace),
                                 .request_id = track.request_id,
                                 .track_alias = track.alias,
                                 .largest_group_id = track.largest_group_id,
                                 .largest_object_id = track.largest_object_id,
                                 .content_exists = track.content_exists,
                             }),
                             false);
        if (!status.ok) {
            return status;
        }
    }

    std::map<std::string, std::uint64_t> stream_count_by_track;
    for (const auto& object : plan.objects) {
        const auto track_it = tracks_by_name.find(object.track_name);
        if (track_it == tracks_by_name.end()) {
            return TransportStatus::failure("object track alias mapping is missing");
        }

        const auto payload = object_payload(object);
        if (payload.empty()) {
            return TransportStatus::failure("transport publish requires materialized object payloads");
        }

        std::uint64_t stream_id = 0;
        status = transport_.open_stream(StreamDirection::kUnidirectional, stream_id);
        if (!status.ok) {
            return status;
        }

        status = write_frame(stream_id, encode_object_stream(track_it->second.alias, object, payload), true);
        if (!status.ok) {
            return status;
        }

        ++stream_count_by_track[object.track_name];
    }

    for (const auto& [track_name, track] : tracks_by_name) {
        static_cast<void>(track_name);
        status = write_frame(control_stream_id_,
                             encode_publish_done_message(track.request_id, stream_count_by_track[track.name]),
                             false);
        if (!status.ok) {
            return status;
        }
    }

    status = write_frame(control_stream_id_, encode_publish_namespace_done_message(namespace_message), false);
    if (!status.ok) {
        return status;
    }

    return TransportStatus::success();
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

TransportStatus MoqtSession::ensure_setup(openmoq::publisher::DraftVersion draft) {
    if (setup_complete_) {
        return TransportStatus::success();
    }

    if (!endpoint_.has_value()) {
        return TransportStatus::failure("session endpoint is not configured");
    }

    TransportStatus status = ensure_control_stream();
    if (!status.ok) {
        return status;
    }

    std::string authority = endpoint_->host + ":" + std::to_string(endpoint_->port);
    status = write_frame(control_stream_id_,
                         encode_setup_message({
                             .draft = draft,
                             .authority = authority,
                             .path = endpoint_->path,
                             .max_request_id = 0,
                         }),
                         false);
    if (!status.ok) {
        return status;
    }

    std::vector<std::uint8_t> response;
    std::vector<std::uint8_t> chunk;
    bool fin = false;
    std::size_t response_size = 0;
    do {
        chunk.clear();
        status = transport_.read_stream(control_stream_id_, chunk, fin, std::chrono::seconds(5));
        if (!status.ok) {
            return status;
        }
        response.insert(response.end(), chunk.begin(), chunk.end());
    } while (!control_message_complete(response, response_size) && !fin);

    if (!control_message_complete(response, response_size)) {
        return TransportStatus::failure("received incomplete SERVER_SETUP message");
    }
    response.resize(response_size);

    ServerSetupMessage server_setup;
    if (!decode_server_setup_message(response, server_setup)) {
        return TransportStatus::failure("received invalid SERVER_SETUP message");
    }

    peer_max_request_id_ = server_setup.max_request_id;
    setup_complete_ = true;
    return TransportStatus::success();
}

TransportStatus MoqtSession::write_frame(std::uint64_t stream_id,
                                         std::span<const std::uint8_t> frame,
                                         bool fin) {
    return transport_.write_stream(stream_id, frame, fin);
}

}  // namespace openmoq::publisher::transport
