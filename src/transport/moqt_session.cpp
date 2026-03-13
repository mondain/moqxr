#include "openmoq/publisher/transport/moqt_session.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <thread>
#include <map>
#include <iostream>
#include <sstream>
#include <set>
#include <vector>

namespace openmoq::publisher::transport {

namespace {

bool control_message_complete(std::span<const std::uint8_t> bytes, std::size_t& message_size) {
    return next_control_message(bytes, message_size);
}

bool trace_enabled() {
    static const bool enabled = std::getenv("OPENMOQ_PICOQUIC_TRACE") != nullptr;
    return enabled;
}

std::string hex_dump(std::span<const std::uint8_t> bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index != 0) {
            out << ' ';
        }
        out << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return out.str();
}

std::span<const std::uint8_t> object_payload(const openmoq::publisher::CmsfObject& object) {
    if (!object.owned_payload.empty()) {
        return object.owned_payload;
    }
    return {};
}

void pace_until(std::chrono::steady_clock::time_point start_time,
                std::uint64_t first_media_time_us,
                const openmoq::publisher::CmsfObject& object,
                bool paced) {
    if (!paced || object.kind != openmoq::publisher::CmsfObjectKind::kMedia) {
        return;
    }
    if (object.media_time_us < first_media_time_us) {
        return;
    }
    std::this_thread::sleep_until(start_time + std::chrono::microseconds(object.media_time_us - first_media_time_us));
}

struct PublishedTrack {
    std::string name;
    std::uint64_t alias = 0;
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

TransportStatus collect_control_acknowledgements(PublisherTransport& transport,
                                                 std::uint64_t control_stream_id,
                                                 openmoq::publisher::DraftVersion draft,
                                                 std::size_t expected_namespace_responses,
                                                 std::size_t expected_publish_responses) {
    std::vector<std::uint8_t> buffer;
    std::size_t namespace_responses = 0;
    std::size_t publish_responses = 0;
    bool fin = false;

    while ((namespace_responses < expected_namespace_responses || publish_responses < expected_publish_responses) && !fin) {
        std::vector<std::uint8_t> chunk;
        const TransportStatus status = transport.read_stream(control_stream_id, chunk, fin, std::chrono::seconds(2));
        if (!status.ok) {
            return status;
        }
        if (trace_enabled()) {
            std::cerr << "[moqt-session] control chunk fin=" << (fin ? 1 : 0) << " bytes=[" << hex_dump(chunk)
                      << "]" << std::endl;
        }
        buffer.insert(buffer.end(), chunk.begin(), chunk.end());

        std::size_t message_size = 0;
        while (next_control_message(buffer, message_size)) {
            const std::vector<std::uint8_t> message_bytes(buffer.begin(), buffer.begin() + message_size);
            std::size_t offset = 0;
            std::uint64_t message_type = 0;
            if (!decode_varint(message_bytes, offset, message_type)) {
                return TransportStatus::failure("failed to parse control response type");
            }
            if (trace_enabled()) {
                std::cerr << "[moqt-session] control message type=0x" << std::hex << message_type << std::dec
                          << " bytes=[" << hex_dump(message_bytes) << "]" << std::endl;
            }

            if (message_type == 0x07) {
                PublishNamespaceOk message;
                if (!decode_publish_namespace_ok(message_bytes, message)) {
                    return TransportStatus::failure("received invalid PUBLISH_NAMESPACE_OK");
                }
                ++namespace_responses;
            } else if (message_type == 0x08) {
                PublishNamespaceError message;
                if (!decode_publish_namespace_error(message_bytes, message)) {
                    return TransportStatus::failure("received invalid PUBLISH_NAMESPACE_ERROR");
                }
                return TransportStatus::failure("peer rejected namespace publish: " + message.reason);
            } else if (message_type == 0x1e) {
                PublishOk message;
                if (!decode_publish_ok(message_bytes, draft, message)) {
                    return TransportStatus::failure("received invalid PUBLISH_OK");
                }
                ++publish_responses;
            } else if (message_type == 0x1f) {
                PublishError message;
                if (!decode_publish_error(message_bytes, message)) {
                    return TransportStatus::failure("received invalid PUBLISH_ERROR");
                }
                return TransportStatus::failure("peer rejected track publish: " + message.reason);
            }

            buffer.erase(buffer.begin(), buffer.begin() + message_size);
        }
    }

    if (namespace_responses < expected_namespace_responses || publish_responses < expected_publish_responses) {
        if (trace_enabled() && !buffer.empty()) {
            std::cerr << "[moqt-session] unparsed control bytes=[" << hex_dump(buffer) << "]" << std::endl;
        }
        return TransportStatus::failure("timed out waiting for publish acknowledgements");
    }

    return TransportStatus::success();
}

bool namespace_matches(std::span<const std::string> track_namespace, std::string_view expected) {
    return track_namespace.size() == 1 && track_namespace.front() == expected;
}

bool namespace_prefix_matches(std::span<const std::string> track_namespace_prefix, std::string_view expected) {
    return track_namespace_prefix.empty() ||
           (track_namespace_prefix.size() == 1 && track_namespace_prefix.front() == expected);
}

bool object_matches_filter(const openmoq::publisher::CmsfObject& object, const SubscribeMessage& subscribe) {
    switch (subscribe.filter_type) {
        case 0x03:
            return object.group_id > subscribe.start_group_id ||
                   (object.group_id == subscribe.start_group_id && object.object_id >= subscribe.start_object_id);
        case 0x04:
            return (object.group_id > subscribe.start_group_id ||
                    (object.group_id == subscribe.start_group_id && object.object_id >= subscribe.start_object_id)) &&
                   object.group_id <= subscribe.end_group_id;
        default:
            return true;
    }
}

TransportStatus serve_subscriptions(PublisherTransport& transport,
                                    std::uint64_t control_stream_id,
                                    const openmoq::publisher::PublishPlan& plan,
                                    const std::map<std::string, PublishedTrack>& tracks_by_name,
                                    openmoq::publisher::DraftVersion draft,
                                    std::string_view track_namespace,
                                    bool paced) {
    std::vector<std::uint8_t> buffer;
    std::set<std::uint64_t> completed_request_ids;
    bool fin = false;
    bool served_any_subscription = false;
    NamespaceMessage namespace_message{
        .draft = draft,
        .track_namespace = std::string(track_namespace),
        .request_id = 0,
    };

    while (!fin) {
        std::vector<std::uint8_t> chunk;
        const TransportStatus read_status = transport.read_stream(control_stream_id, chunk, fin, std::chrono::seconds(3));
        if (!read_status.ok) {
            if (served_any_subscription &&
                (read_status.message == "timed out waiting for stream data" ||
                 read_status.message == "no queued read for stream")) {
                break;
            }
            return read_status;
        }

        if (trace_enabled()) {
            std::cerr << "[moqt-session] control chunk fin=" << (fin ? 1 : 0) << " bytes=[" << hex_dump(chunk)
                      << "]" << std::endl;
        }
        buffer.insert(buffer.end(), chunk.begin(), chunk.end());

        std::size_t message_size = 0;
        while (next_control_message(buffer, message_size)) {
            const std::vector<std::uint8_t> message_bytes(buffer.begin(), buffer.begin() + message_size);
            std::size_t offset = 0;
            std::uint64_t message_type = 0;
            if (!decode_varint(message_bytes, offset, message_type)) {
                return TransportStatus::failure("failed to parse control request type");
            }

            if (trace_enabled()) {
                std::cerr << "[moqt-session] control message type=0x" << std::hex << message_type << std::dec
                          << " bytes=[" << hex_dump(message_bytes) << "]" << std::endl;
            }

            if (message_type == 0x12 || message_type == 0x07 || message_type == 0x1e) {
                buffer.erase(buffer.begin(), buffer.begin() + message_size);
                continue;
            }

            if (message_type == 0x11) {
                SubscribeNamespaceMessage subscribe_namespace;
                if (!decode_subscribe_namespace_message(message_bytes, subscribe_namespace)) {
                    return TransportStatus::failure("received invalid SUBSCRIBE_NAMESPACE");
                }
                if (!namespace_prefix_matches(subscribe_namespace.track_namespace_prefix, track_namespace)) {
                    return TransportStatus::failure("peer requested unsupported namespace prefix");
                }
                const TransportStatus write_status =
                    transport.write_stream(control_stream_id,
                                           encode_subscribe_namespace_ok_message(subscribe_namespace.request_id),
                                           false);
                if (!write_status.ok) {
                    return write_status;
                }
                buffer.erase(buffer.begin(), buffer.begin() + message_size);
                continue;
            }

            if (message_type == 0x03) {
                SubscribeMessage subscribe;
                if (!decode_subscribe_message(message_bytes, subscribe)) {
                    return TransportStatus::failure("received invalid SUBSCRIBE");
                }
                if (!namespace_matches(subscribe.track_namespace, track_namespace)) {
                    return TransportStatus::failure("peer requested unsupported track namespace");
                }

                const auto track_it = tracks_by_name.find(subscribe.track_name);
                if (track_it == tracks_by_name.end()) {
                    const TransportStatus write_status =
                        transport.write_stream(control_stream_id,
                                               encode_subscribe_error_message(subscribe.request_id,
                                                                              0x2,
                                                                              "track does not exist"),
                                               false);
                    if (!write_status.ok) {
                        return write_status;
                    }
                    buffer.erase(buffer.begin(), buffer.begin() + message_size);
                    continue;
                }

                TransportStatus write_status =
                    transport.write_stream(control_stream_id,
                                           encode_subscribe_ok_message(subscribe.request_id,
                                                                       track_it->second.alias,
                                                                       subscribe.subscriber_priority,
                                                                       0,
                                                                       0,
                                                                       false),
                                           false);
                if (!write_status.ok) {
                    return write_status;
                }

                std::uint64_t stream_count = 0;
                std::uint64_t first_media_time_us = 0;
                bool first_media_time_set = false;
                const auto pacing_start = std::chrono::steady_clock::now();
                if (subscribe.forward != 0) {
                    for (const auto& object : plan.objects) {
                        if (object.track_name != subscribe.track_name || !object_matches_filter(object, subscribe)) {
                            continue;
                        }
                        const auto payload = object_payload(object);
                        if (payload.empty()) {
                            return TransportStatus::failure("transport publish requires materialized object payloads");
                        }
                        if (object.kind == openmoq::publisher::CmsfObjectKind::kMedia && !first_media_time_set) {
                            first_media_time_us = object.media_time_us;
                            first_media_time_set = true;
                        }
                        pace_until(pacing_start, first_media_time_us, object, paced);
                        std::uint64_t stream_id = 0;
                        write_status = transport.open_stream(StreamDirection::kUnidirectional, stream_id);
                        if (!write_status.ok) {
                            return write_status;
                        }
                        write_status = transport.write_stream(stream_id,
                                                              encode_object_stream(track_it->second.alias, object, payload),
                                                              true);
                        if (!write_status.ok) {
                            return write_status;
                        }
                        ++stream_count;
                    }
                }

                write_status = transport.write_stream(control_stream_id,
                                                      encode_publish_done_message(subscribe.request_id, stream_count),
                                                      false);
                if (!write_status.ok) {
                    return write_status;
                }

                completed_request_ids.insert(subscribe.request_id);
                served_any_subscription = true;
                buffer.erase(buffer.begin(), buffer.begin() + message_size);
                continue;
            }

            return TransportStatus::failure("received unsupported control request");
        }
    }

    if (!served_any_subscription) {
        return TransportStatus::failure("timed out waiting for subscribe request");
    }

    return transport.write_stream(control_stream_id, encode_publish_namespace_done_message(namespace_message), false);
}

TransportStatus forward_published_tracks(PublisherTransport& transport,
                                         std::uint64_t control_stream_id,
                                         const openmoq::publisher::PublishPlan& plan,
                                         std::span<const PublishedTrack> tracks,
                                         std::uint64_t peer_max_request_id,
                                         std::string_view track_namespace,
                                         bool paced) {
    std::map<std::string, std::uint64_t> request_id_by_track;
    std::map<std::string, PublishedTrack> tracks_by_name;
    std::uint64_t next_request_id = 2;

    for (const auto& track : tracks) {
        if (next_request_id > peer_max_request_id) {
            return TransportStatus::failure("peer max_request_id is too small for publish requests");
        }

        request_id_by_track.emplace(track.name, next_request_id);
        tracks_by_name.emplace(track.name, track);

        const TrackMessage track_message{
            .draft = plan.draft.version,
            .track_name = track.name,
            .track_namespace = std::string(track_namespace),
            .request_id = next_request_id,
            .track_alias = track.alias,
            .largest_group_id = track.largest_group_id,
            .largest_object_id = track.largest_object_id,
            .content_exists = track.content_exists,
        };
        const TransportStatus status =
            transport.write_stream(control_stream_id, encode_track_message(track_message), false);
        if (!status.ok) {
            return status;
        }
        next_request_id += 2;
    }

    TransportStatus status =
        collect_control_acknowledgements(transport, control_stream_id, plan.draft.version, 0, tracks.size());
    if (!status.ok) {
        return status;
    }

    std::map<std::string, std::uint64_t> stream_count_by_track;
    const auto pacing_start = std::chrono::steady_clock::now();
    std::uint64_t first_media_time_us = 0;
    bool first_media_time_set = false;
    for (const auto& object : plan.objects) {
        const auto track_it = tracks_by_name.find(object.track_name);
        if (track_it == tracks_by_name.end()) {
            continue;
        }
        const auto payload = object_payload(object);
        if (payload.empty()) {
            return TransportStatus::failure("transport publish requires materialized object payloads");
        }
        if (object.kind == openmoq::publisher::CmsfObjectKind::kMedia && !first_media_time_set) {
            first_media_time_us = object.media_time_us;
            first_media_time_set = true;
        }
        pace_until(pacing_start, first_media_time_us, object, paced);

        std::uint64_t stream_id = 0;
        status = transport.open_stream(StreamDirection::kUnidirectional, stream_id);
        if (!status.ok) {
            return status;
        }
        status = transport.write_stream(stream_id, encode_object_stream(track_it->second.alias, object, payload), true);
        if (!status.ok) {
            return status;
        }
        ++stream_count_by_track[object.track_name];
    }

    for (const auto& track : tracks) {
        status = transport.write_stream(control_stream_id,
                                        encode_publish_done_message(request_id_by_track.at(track.name),
                                                                    stream_count_by_track[track.name]),
                                        false);
        if (!status.ok) {
            return status;
        }
    }

    return transport.write_stream(control_stream_id,
                                  encode_publish_namespace_done_message({
                                      .draft = plan.draft.version,
                                      .track_namespace = std::string(track_namespace),
                                      .request_id = 0,
                                  }),
                                  false);
}

}  // namespace

MoqtSession::MoqtSession(PublisherTransport& transport, std::string track_namespace, bool auto_forward, bool paced)
    : transport_(transport), track_namespace_(std::move(track_namespace)), auto_forward_(auto_forward), paced_(paced) {}

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
    std::cout << "connection_id=" << transport_.connection_id() << '\n';

    const std::vector<PublishedTrack> tracks = build_published_tracks(plan);

    NamespaceMessage namespace_message{
        .draft = plan.draft.version,
        .track_namespace = track_namespace_,
        .request_id = 0,
    };
    status = write_frame(control_stream_id_, encode_namespace_message(namespace_message), false);
    if (!status.ok) {
        return status;
    }

    status = collect_control_acknowledgements(transport_, control_stream_id_, plan.draft.version, 1, 0);
    if (!status.ok) {
        return status;
    }

    std::map<std::string, PublishedTrack> tracks_by_name;
    for (auto track : tracks) {
        tracks_by_name.emplace(track.name, track);
    }

    if (auto_forward_) {
        return forward_published_tracks(
            transport_, control_stream_id_, plan, tracks, peer_max_request_id_, track_namespace_, paced_);
    }

    return serve_subscriptions(
        transport_, control_stream_id_, plan, tracks_by_name, plan.draft.version, track_namespace_, paced_);
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
        if (trace_enabled()) {
            std::cerr << "[moqt-session] invalid SERVER_SETUP bytes=[" << hex_dump(response) << "]" << std::endl;
        }
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
