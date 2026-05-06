#include "openmoq/publisher/transport/moqt_session.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <vector>

namespace openmoq::publisher::transport {

namespace {

bool trace_enabled() {
    static const bool enabled = std::getenv("OPENMOQ_PICOQUIC_TRACE") != nullptr;
    return enabled;
}

const char* trace_csv_path() {
    static const char* path = std::getenv("OPENMOQ_PICOQUIC_TRACE_CSV");
    return path;
}

bool trace_csv_enabled() {
    const char* path = trace_csv_path();
    return trace_enabled() && path != nullptr && path[0] != '\0';
}

std::chrono::steady_clock::time_point trace_epoch() {
    static const auto epoch = std::chrono::steady_clock::now();
    return epoch;
}

long long trace_elapsed_ms(std::chrono::steady_clock::time_point time_point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time_point - trace_epoch()).count();
}

std::uint64_t next_send_seq() {
    static std::uint64_t counter = 0;
    return ++counter;
}

struct TraceCsvSink {
    std::mutex mutex;
    std::ofstream stream;
    bool header_written = false;
};

TraceCsvSink& trace_csv_sink() {
    static TraceCsvSink sink;
    return sink;
}

void trace_csv_write_row(std::initializer_list<std::string_view> fields) {
    if (!trace_csv_enabled()) {
        return;
    }

    TraceCsvSink& sink = trace_csv_sink();
    std::lock_guard<std::mutex> lock(sink.mutex);
    if (!sink.stream.is_open()) {
        sink.stream.open(trace_csv_path(), std::ios::out | std::ios::app);
        if (!sink.stream.is_open()) {
            return;
        }
    }
    if (!sink.header_written) {
        sink.stream
            << "event,send_seq,now_ms,target_ms,delta_ms,track,group_id,subgroup_id,object_id,media_time_us,"
               "stream_id,opened,payload_bytes,wire_bytes,fin\n";
        sink.header_written = true;
    }

    bool first = true;
    for (const auto field : fields) {
        if (!first) {
            sink.stream << ',';
        }
        first = false;
        sink.stream << field;
    }
    sink.stream << '\n';
    sink.stream.flush();
}

bool is_idle_subscribe_exit(std::string_view message) {
    return message == "timed out waiting for stream data" ||
           message == "no queued read for stream" ||
           message == "webtransport connection closed";
}

std::string hex_dump(std::span<const std::uint8_t> bytes);

TransportStatus try_read_wt_session_stream(PublisherTransport& transport,
                                           bool& saw_bytes,
                                           bool& fin) {
    std::vector<std::uint8_t> session_chunk;
    bool session_fin = false;
    const TransportStatus session_status =
        transport.read_stream(0, session_chunk, session_fin, std::chrono::milliseconds(0));
    if (!session_status.ok) {
        return session_status;
    }
    saw_bytes = !session_chunk.empty();
    if (trace_enabled()) {
        std::cerr << "[moqt-session] setup fallback read stream=0 bytes=" << session_chunk.size()
                  << " fin=" << (session_fin ? 1 : 0)
                  << " bytes=[" << hex_dump(session_chunk) << "]" << std::endl;
    }
    fin = session_fin;
    return TransportStatus::success();
}

const char* control_message_type_name(std::uint64_t message_type,
                                      openmoq::publisher::DraftVersion draft) {
    switch (message_type) {
        case 0x02:
            return "SUBSCRIBE_UPDATE";
        case 0x03:
            return "SUBSCRIBE";
        case 0x04:
            return "SUBSCRIBE_OK";
        case 0x05:
            return draft == openmoq::publisher::DraftVersion::kDraft16 ? "REQUEST_ERROR" : "SUBSCRIBE_ERROR";
        case 0x06:
            return "PUBLISH_NAMESPACE";
        case 0x07:
            return "PUBLISH_NAMESPACE_OK";
        case 0x08:
            return "PUBLISH_NAMESPACE_ERROR";
        case 0x09:
            return "PUBLISH_NAMESPACE_DONE";
        case 0x0b:
            return "PUBLISH_DONE";
        case 0x11:
            return "SUBSCRIBE_NAMESPACE";
        case 0x12:
            return "SUBSCRIBE_NAMESPACE_OK";
        case 0x15:
            return "MAX_REQUEST_ID";
        case 0x1d:
            return "PUBLISH";
        case 0x1e:
            return "PUBLISH_OK";
        case 0x1f:
            return "PUBLISH_ERROR";
        case 0x20:
            return "CLIENT_SETUP";
        case 0x21:
            return "SERVER_SETUP";
        case 0x0a:
            return "UNSUBSCRIBE";
        case 0x10:
            return "GOAWAY";
        case 0x13:
            return "SUBSCRIBE_NAMESPACE_ERROR";
        case 0x14:
            return "SUBSCRIBE_DONE";
        case 0x16:
            return "FETCH";
        case 0x17:
            return "FETCH_OK";
        case 0x18:
            return "FETCH_CANCEL";
        case 0x1a:
            return "REQUESTS_BLOCKED";
        case 0x1b:
            return "UNSUBSCRIBE_NAMESPACE";
        default:
            return "UNKNOWN";
    }
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

template <typename MapLike>
std::string format_request_id_keys(const MapLike& values) {
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto& [request_id, ignored] : values) {
        static_cast<void>(ignored);
        if (!first) {
            out << ',';
        }
        out << request_id;
        first = false;
    }
    out << ']';
    return out.str();
}

std::string format_request_id_keys(const std::set<std::uint64_t>& values) {
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto request_id : values) {
        if (!first) {
            out << ',';
        }
        out << request_id;
        first = false;
    }
    out << ']';
    return out.str();
}

void trace_control_message(std::span<const std::uint8_t> message_bytes, openmoq::publisher::DraftVersion draft) {
    if (!trace_enabled()) {
        return;
    }

    std::size_t offset = 0;
    std::uint64_t message_type = 0;
    if (!decode_varint(message_bytes, offset, message_type)) {
        std::cerr << "[moqt-session] control message parse error now_ms="
                  << trace_elapsed_ms(std::chrono::steady_clock::now())
                  << " bytes=[" << hex_dump(message_bytes) << "]" << std::endl;
        return;
    }

    std::cerr << "[moqt-session] control message now_ms="
              << trace_elapsed_ms(std::chrono::steady_clock::now())
              << " type=0x" << std::hex << message_type << std::dec << " "
              << control_message_type_name(message_type, draft);

    if (message_type == 0x07) {
        PublishNamespaceOk message;
        if (decode_request_ok(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id;
        }
    } else if (message_type == 0x08) {
        PublishNamespaceError message;
        if (decode_publish_namespace_error(message_bytes, message)) {
            std::cerr << " request_id=" << message.request_id << " error_code=" << message.error_code
                      << " reason=" << message.reason;
        }
    } else if (message_type == 0x05 && draft == openmoq::publisher::DraftVersion::kDraft16) {
        RequestError message;
        if (decode_request_error(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id << " error_code=" << message.error_code
                      << " reason=" << message.reason;
        }
    } else if (message_type == 0x02) {
        SubscribeUpdateMessage message;
        if (decode_subscribe_update_message(message_bytes, message)) {
            std::cerr << " request_id=" << message.request_id
                      << " subscription_request_id=" << message.subscription_request_id
                      << " start_group=" << message.start_group_id
                      << " start_object=" << message.start_object_id
                      << " end_group_plus_one=" << message.end_group_plus_one
                      << " forward=" << static_cast<unsigned int>(message.forward);
        }
    } else if (message_type == 0x11) {
        SubscribeNamespaceMessage message;
        if (decode_subscribe_namespace_message(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id;
            if (!message.track_namespace_prefix.empty()) {
                std::cerr << " prefix=" << message.track_namespace_prefix.front();
            } else {
                std::cerr << " prefix=<empty>";
            }
        }
    } else if (message_type == 0x03) {
        SubscribeMessage message;
        if (decode_subscribe_message(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id << " track=" << message.track_name
                      << " forward=" << static_cast<unsigned int>(message.forward)
                      << " filter_type=" << message.filter_type;
        }
    } else if (message_type == 0x1e) {
        PublishOk message;
        if (decode_publish_ok(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id
                      << " forward=" << static_cast<unsigned int>(message.forward)
                      << " subscriber_priority=" << static_cast<unsigned int>(message.subscriber_priority)
                      << " group_order=" << static_cast<unsigned int>(message.group_order)
                      << " filter_type=" << message.filter_type;
        }
    } else if (message_type == 0x1f) {
        PublishError message;
        if (decode_publish_error(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id << " error_code=" << message.error_code
                      << " reason=" << message.reason;
        }
    }

    std::cerr << " bytes=[" << hex_dump(message_bytes) << "]" << std::endl;
}

std::span<const std::uint8_t> object_payload(const openmoq::publisher::CmsfObject& object) {
    if (!object.owned_payload.empty()) {
        return object.owned_payload;
    }
    return {};
}

std::size_t object_payload_size(const openmoq::publisher::CmsfObject& object) {
    if (!object.owned_payload.empty()) {
        return object.owned_payload.size();
    }
    return object.payload.size;
}

void trace_csv_write_media_timing(std::string_view event,
                                  std::uint64_t send_seq,
                                  long long now_ms,
                                  long long target_ms,
                                  long long delta_ms,
                                  const openmoq::publisher::CmsfObject& object) {
    if (!trace_csv_enabled() || object.kind != openmoq::publisher::CmsfObjectKind::kMedia) {
        return;
    }

    trace_csv_write_row({
        std::string(event),
        std::to_string(send_seq),
        std::to_string(now_ms),
        std::to_string(target_ms),
        std::to_string(delta_ms),
        object.track_name,
        std::to_string(object.group_id),
        std::to_string(object.subgroup_id),
        std::to_string(object.object_id),
        std::to_string(object.media_time_us),
        "",
        "",
        std::to_string(object_payload_size(object)),
        "",
        "",
    });
}

void trace_csv_write_enqueue(std::uint64_t send_seq,
                             long long now_ms,
                             const openmoq::publisher::CmsfObject& object,
                             std::uint64_t stream_id,
                             bool opened_stream,
                             std::size_t payload_bytes,
                             std::size_t wire_bytes,
                             bool fin) {
    if (!trace_csv_enabled() || object.kind != openmoq::publisher::CmsfObjectKind::kMedia) {
        return;
    }

    trace_csv_write_row({
        "enqueue",
        std::to_string(send_seq),
        std::to_string(now_ms),
        "",
        "",
        object.track_name,
        std::to_string(object.group_id),
        std::to_string(object.subgroup_id),
        std::to_string(object.object_id),
        std::to_string(object.media_time_us),
        std::to_string(stream_id),
        opened_stream ? "1" : "0",
        std::to_string(payload_bytes),
        std::to_string(wire_bytes),
        fin ? "1" : "0",
    });
}

void trace_csv_write_served(std::string_view event,
                            std::uint64_t send_seq,
                            long long now_ms,
                            const openmoq::publisher::CmsfObject& object) {
    if (!trace_csv_enabled() || object.kind != openmoq::publisher::CmsfObjectKind::kMedia) {
        return;
    }

    trace_csv_write_row({
        std::string(event),
        std::to_string(send_seq),
        std::to_string(now_ms),
        "",
        "",
        object.track_name,
        std::to_string(object.group_id),
        std::to_string(object.subgroup_id),
        std::to_string(object.object_id),
        std::to_string(object.media_time_us),
        "",
        "",
        std::to_string(object_payload_size(object)),
        "",
        "",
    });
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

void trace_pacing_decision(std::string_view phase,
                           std::uint64_t send_seq,
                           std::chrono::steady_clock::time_point pacing_start,
                           std::uint64_t first_media_time_us,
                           const openmoq::publisher::CmsfObject& object,
                           bool paced) {
    if (!trace_enabled() || object.kind != openmoq::publisher::CmsfObjectKind::kMedia) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t target_offset_us =
        object.media_time_us < first_media_time_us ? 0 : (object.media_time_us - first_media_time_us);
    const auto target_time = pacing_start + std::chrono::microseconds(target_offset_us);
    const auto now_ms = trace_elapsed_ms(now);
    const auto target_ms = trace_elapsed_ms(target_time);
    const auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - target_time).count();

    std::cerr << "[moqt-session] pacing " << phase
              << " send_seq=" << send_seq
              << " now_ms=" << now_ms
              << " target_ms=" << target_ms
              << " delta_ms=" << delta_ms
              << " paced=" << (paced ? 1 : 0)
              << " track=" << object.track_name
              << " group=" << object.group_id
              << " object=" << object.object_id
              << " media_time_us=" << object.media_time_us
              << std::endl;
    trace_csv_write_media_timing(phase == "before" ? "pacing_before" : "pacing_after",
                                 send_seq,
                                 now_ms,
                                 target_ms,
                                 delta_ms,
                                 object);
}

struct TrackLoopInfo {
    std::size_t group_span = 0;
    std::size_t first_loop_object_index = 0;
    bool has_loopable_objects = false;
};

struct LoopState {
    bool enabled = false;
    std::uint64_t cycle_duration_us = 0;
    std::map<std::string, TrackLoopInfo> tracks;
};

struct PublishedTrack {
    std::string name;
    std::uint64_t alias = 0;
    std::size_t largest_group_id = 0;
    std::size_t largest_object_id = 0;
    bool content_exists = false;
};

class SubgroupSenderState;

struct ActiveSubscription {
    SubscribeMessage subscribe;
    PublishedTrack track;
    std::shared_ptr<SubgroupSenderState> sender;
    std::size_t loop_cycle = 0;
    std::size_t next_object_index = 0;
    bool completed = false;
};

struct DormantPublishedTrack {
    SubscribeMessage subscribe;
    PublishedTrack track;
};

std::string format_alias_mapping(const std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias) {
    if (request_id_by_track_alias == nullptr || request_id_by_track_alias->empty()) {
        return "[]";
    }

    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto& [alias, request_id] : *request_id_by_track_alias) {
        if (!first) {
            out << ',';
        }
        out << alias << "->" << request_id;
        first = false;
    }
    out << ']';
    return out.str();
}

void trace_subscribe_update_state(std::string_view reason,
                                  const SubscribeUpdateMessage& subscribe_update,
                                  std::uint64_t original_request_id,
                                  std::uint64_t remapped_request_id,
                                  const std::map<std::uint64_t, SubscribeMessage>& pending_subscriptions,
                                  const std::map<std::uint64_t, ActiveSubscription>& active_subscriptions,
                                  const std::map<std::uint64_t, DormantPublishedTrack>* dormant_published_tracks,
                                  const std::set<std::uint64_t>& completed_request_ids,
                                  const std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias) {
    if (!trace_enabled()) {
        return;
    }

    std::cerr << "[moqt-session] SUBSCRIBE_UPDATE reject reason=" << reason
              << " request_id=" << subscribe_update.request_id
              << " subscription_request_id=" << original_request_id
              << " remapped_request_id=" << remapped_request_id
              << " start_group=" << subscribe_update.start_group_id
              << " start_object=" << subscribe_update.start_object_id
              << " end_group_plus_one=" << subscribe_update.end_group_plus_one
              << " forward=" << static_cast<unsigned int>(subscribe_update.forward)
              << " subscriber_priority=" << static_cast<unsigned int>(subscribe_update.subscriber_priority)
              << " pending=" << format_request_id_keys(pending_subscriptions)
              << " active=" << format_request_id_keys(active_subscriptions)
              << " dormant="
              << (dormant_published_tracks != nullptr ? format_request_id_keys(*dormant_published_tracks) : "[]")
              << " completed=" << format_request_id_keys(completed_request_ids)
              << " alias_map=" << format_alias_mapping(request_id_by_track_alias)
              << std::endl;
}

LoopState build_loop_state(const openmoq::publisher::PublishPlan& plan, bool loop_enabled) {
    LoopState state;
    state.enabled = loop_enabled;
    if (!loop_enabled) {
        return state;
    }

    for (std::size_t index = 0; index < plan.objects.size(); ++index) {
        const auto& object = plan.objects[index];
        auto& track = state.tracks[object.track_name];
        track.group_span = std::max(track.group_span, object.group_id + 1);
        if (object.kind == openmoq::publisher::CmsfObjectKind::kInitialization) {
            continue;
        }
        if (!track.has_loopable_objects) {
            track.first_loop_object_index = index;
            track.has_loopable_objects = true;
        }
        if (object.kind == openmoq::publisher::CmsfObjectKind::kMedia) {
            state.cycle_duration_us = std::max(state.cycle_duration_us, object.media_time_us + object.media_duration_us);
        }
    }

    return state;
}

const TrackLoopInfo* find_track_loop_info(const LoopState& loop_state, std::string_view track_name) {
    const auto it = loop_state.tracks.find(std::string(track_name));
    return it == loop_state.tracks.end() ? nullptr : &it->second;
}

bool track_can_loop(const LoopState& loop_state, std::string_view track_name) {
    const TrackLoopInfo* info = find_track_loop_info(loop_state, track_name);
    return info != nullptr && info->has_loopable_objects;
}

openmoq::publisher::CmsfObject make_looped_object(const openmoq::publisher::CmsfObject& object,
                                                  const LoopState& loop_state,
                                                  std::size_t loop_cycle) {
    if (!loop_state.enabled || loop_cycle == 0) {
        return object;
    }

    const TrackLoopInfo* info = find_track_loop_info(loop_state, object.track_name);
    if (info == nullptr || !info->has_loopable_objects || object.kind == openmoq::publisher::CmsfObjectKind::kInitialization) {
        return object;
    }

    openmoq::publisher::CmsfObject adjusted = object;
    adjusted.group_id += info->group_span * loop_cycle;
    adjusted.media_time_us += loop_state.cycle_duration_us * loop_cycle;
    return adjusted;
}

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
                                                 std::size_t expected_publish_responses,
                                                 std::vector<std::uint8_t>& pending_control_bytes,
                                                 std::map<std::uint64_t, PublishOk>* publish_ok_by_request_id = nullptr) {
    std::vector<std::uint8_t> buffer = std::move(pending_control_bytes);
    std::vector<std::uint8_t> deferred_messages;
    pending_control_bytes.clear();
    std::size_t namespace_responses = 0;
    std::size_t publish_responses = 0;
    bool fin = false;

    while (namespace_responses < expected_namespace_responses || publish_responses < expected_publish_responses) {
        std::size_t consumed = 0;
        std::size_t message_size = 0;
        while (next_control_message(std::span<const std::uint8_t>(buffer).subspan(consumed), draft, message_size)) {
            const std::vector<std::uint8_t> message_bytes(buffer.begin() + static_cast<std::ptrdiff_t>(consumed),
                                                          buffer.begin() + static_cast<std::ptrdiff_t>(consumed + message_size));
            std::size_t offset = 0;
            std::uint64_t message_type = 0;
            if (!decode_varint(message_bytes, offset, message_type)) {
                return TransportStatus::failure("failed to parse control response type");
            }
            trace_control_message(message_bytes, draft);

            bool handled = false;
            if (message_type == 0x07 && namespace_responses < expected_namespace_responses) {
                PublishNamespaceOk message;
                if (!decode_request_ok(message_bytes, draft, message)) {
                    return TransportStatus::failure(draft == openmoq::publisher::DraftVersion::kDraft16
                                                        ? "received invalid REQUEST_OK"
                                                        : "received invalid PUBLISH_NAMESPACE_OK");
                }
                ++namespace_responses;
                handled = true;
            } else if (namespace_responses < expected_namespace_responses &&
                       draft == openmoq::publisher::DraftVersion::kDraft14 &&
                       message_type == 0x08) {
                RequestError message;
                if (!decode_request_error(message_bytes, draft, message)) {
                    return TransportStatus::failure("received invalid PUBLISH_NAMESPACE_ERROR");
                }
                return TransportStatus::failure("peer rejected namespace publish: " + message.reason);
            } else if (draft == openmoq::publisher::DraftVersion::kDraft16 &&
                       message_type == 0x05) {
                RequestError message;
                if (!decode_request_error(message_bytes, draft, message)) {
                    return TransportStatus::failure("received invalid REQUEST_ERROR");
                }
                if (message.request_id == 0 && namespace_responses < expected_namespace_responses) {
                    return TransportStatus::failure("peer rejected namespace publish: " + message.reason);
                }
                if (message.request_id != 0 && publish_responses < expected_publish_responses) {
                    return TransportStatus::failure("peer rejected track publish: " + message.reason);
                }
            } else if (message_type == 0x1e && publish_responses < expected_publish_responses) {
                PublishOk message;
                if (!decode_publish_ok(message_bytes, draft, message)) {
                    return TransportStatus::failure("received invalid PUBLISH_OK");
                }
                if (publish_ok_by_request_id != nullptr) {
                    publish_ok_by_request_id->insert_or_assign(message.request_id, message);
                }
                ++publish_responses;
                handled = true;
            } else if (message_type == 0x1f && publish_responses < expected_publish_responses) {
                PublishError message;
                if (!decode_publish_error(message_bytes, draft, message)) {
                    return TransportStatus::failure("received invalid PUBLISH_ERROR");
                }
                return TransportStatus::failure("peer rejected track publish: " + message.reason);
            }
            if (!handled) {
                deferred_messages.insert(deferred_messages.end(), message_bytes.begin(), message_bytes.end());
            }
            consumed += message_size;
        }

        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(consumed));

        if (namespace_responses >= expected_namespace_responses && publish_responses >= expected_publish_responses) {
            break;
        }
        if (fin) {
            break;
        }

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
    }

    if (namespace_responses < expected_namespace_responses || publish_responses < expected_publish_responses) {
        if (trace_enabled() && (!buffer.empty() || !deferred_messages.empty())) {
            if (!deferred_messages.empty()) {
                std::cerr << "[moqt-session] deferred control bytes=[" << hex_dump(deferred_messages) << "]" << std::endl;
            }
            if (!buffer.empty()) {
                std::cerr << "[moqt-session] unparsed control bytes=[" << hex_dump(buffer) << "]" << std::endl;
            }
        }
        return TransportStatus::failure("timed out waiting for publish acknowledgements");
    }

    deferred_messages.insert(deferred_messages.end(), buffer.begin(), buffer.end());
    pending_control_bytes = std::move(deferred_messages);
    return TransportStatus::success();
}

bool namespace_matches(std::span<const std::string> track_namespace, std::string_view expected) {
    std::size_t component_count = 0;
    std::size_t start = 0;
    for (; start <= expected.size(); ++component_count) {
        const std::size_t slash = expected.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? expected.size() : slash;
        if (component_count >= track_namespace.size() || track_namespace[component_count] != expected.substr(start, end - start)) {
            return false;
        }
        if (slash == std::string_view::npos) {
            return component_count + 1 == track_namespace.size();
        }
        start = slash + 1;
    }
    return false;
}

bool namespace_prefix_matches(std::span<const std::string> track_namespace_prefix, std::string_view expected) {
    if (track_namespace_prefix.empty()) {
        return true;
    }

    std::size_t component_index = 0;
    std::size_t start = 0;
    while (start <= expected.size()) {
        const std::size_t slash = expected.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? expected.size() : slash;
        if (component_index >= track_namespace_prefix.size() ||
            track_namespace_prefix[component_index] != expected.substr(start, end - start)) {
            return false;
        }
        ++component_index;
        if (slash == std::string_view::npos) {
            return component_index == track_namespace_prefix.size();
        }
        start = slash + 1;
    }
    return false;
}

std::vector<std::string> split_track_namespace_components(std::string_view ns) {
    std::vector<std::string> components;
    std::size_t start = 0;
    while (start <= ns.size()) {
        const std::size_t slash = ns.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? ns.size() : slash;
        components.emplace_back(ns.substr(start, end - start));
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return components;
}

bool object_matches_filter(const openmoq::publisher::CmsfObject& object, const SubscribeMessage& subscribe) {
    switch (subscribe.filter_type) {
        case 0x01:
        case 0x02:
            return true;
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

bool apply_subscribe_update(SubscribeMessage& subscribe, const SubscribeUpdateMessage& update) {
    const bool start_increases = update.start_group_id > subscribe.start_group_id ||
                                 (update.start_group_id == subscribe.start_group_id &&
                                  update.start_object_id >= subscribe.start_object_id);
    if (!start_increases) {
        return false;
    }

    const std::size_t updated_end_group_id =
        update.end_group_plus_one == 0 ? 0 : (update.end_group_plus_one - 1);

    if (subscribe.filter_type == 0x04) {
        if (update.end_group_plus_one == 0 || updated_end_group_id > subscribe.end_group_id) {
            return false;
        }
    }

    subscribe.start_group_id = update.start_group_id;
    subscribe.start_object_id = update.start_object_id;
    subscribe.subscriber_priority = update.subscriber_priority;
    subscribe.forward = update.forward;

    if (update.end_group_plus_one != 0) {
        subscribe.filter_type = 0x04;
        subscribe.end_group_id = updated_end_group_id;
    } else if (subscribe.filter_type != 0x04) {
        subscribe.end_group_id = 0;
    }

    return true;
}

bool decode_legacy_subscribe_update_message(std::span<const std::uint8_t> bytes,
                                            const std::map<std::string, PublishedTrack>& tracks_by_name,
                                            const std::map<std::uint64_t, SubscribeMessage>& pending_subscriptions,
                                            const std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias,
                                            SubscribeUpdateMessage& message) {
    std::size_t offset = 0;
    std::uint64_t message_type = 0;
    if (!decode_varint(bytes, offset, message_type) || message_type != 0x02 || offset + 2 > bytes.size()) {
        return false;
    }

    const std::size_t payload_length =
        (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
    offset += 2;
    if (offset + payload_length != bytes.size() || payload_length != 7) {
        return false;
    }

    std::size_t payload_offset = offset;
    std::uint64_t track_alias = 0;
    std::uint64_t start_group_id = 0;
    std::uint64_t start_object_id = 0;
    if (!decode_varint(bytes, payload_offset, track_alias) ||
        !decode_varint(bytes, payload_offset, start_group_id) ||
        !decode_varint(bytes, payload_offset, start_object_id) ||
        payload_offset + 4 != bytes.size()) {
        return false;
    }

    const std::uint8_t subscriber_priority = bytes[payload_offset++];
    const std::uint8_t group_order = bytes[payload_offset++];
    const std::uint8_t parameter_or_filter = bytes[payload_offset++];
    const std::uint8_t forward = bytes[payload_offset++];
    if (group_order > 2 || forward > 1) {
        return false;
    }
    static_cast<void>(parameter_or_filter);

    for (const auto& [request_id, subscribe] : pending_subscriptions) {
        const auto track_it = tracks_by_name.find(subscribe.track_name);
        if (track_it == tracks_by_name.end() || track_it->second.alias != track_alias) {
            continue;
        }

        message.request_id = 0;
        message.subscription_request_id = request_id;
        message.start_group_id = static_cast<std::size_t>(start_group_id);
        message.start_object_id = static_cast<std::size_t>(start_object_id);
        message.end_group_plus_one = subscribe.filter_type == 0x04 ? (subscribe.end_group_id + 1) : 0;
        message.subscriber_priority = subscriber_priority;
        message.forward = forward;
        return true;
    }

    if (request_id_by_track_alias != nullptr) {
        const auto request_id_it = request_id_by_track_alias->find(track_alias);
        if (request_id_it != request_id_by_track_alias->end()) {
            message.request_id = 0;
            message.subscription_request_id = request_id_it->second;
            message.start_group_id = static_cast<std::size_t>(start_group_id);
            message.start_object_id = static_cast<std::size_t>(start_object_id);
            message.end_group_plus_one = 0;
            message.subscriber_priority = subscriber_priority;
            message.forward = forward;
            return true;
        }
    }

    return false;
}

bool find_next_matching_object_index(const openmoq::publisher::PublishPlan& plan,
                                     const SubscribeMessage& subscribe,
                                     std::size_t start_index,
                                     std::size_t& object_index) {
    for (std::size_t index = start_index; index < plan.objects.size(); ++index) {
        const auto& object = plan.objects[index];
        if (object.track_name == subscribe.track_name && object_matches_filter(object, subscribe)) {
            object_index = index;
            return true;
        }
    }
    return false;
}

bool advance_subscription_to_next_loop_object(const openmoq::publisher::PublishPlan& plan,
                                              const LoopState& loop_state,
                                              ActiveSubscription& active) {
    if (!loop_state.enabled || !track_can_loop(loop_state, active.track.name)) {
        return false;
    }

    const TrackLoopInfo* info = find_track_loop_info(loop_state, active.track.name);
    if (info == nullptr || !info->has_loopable_objects) {
        return false;
    }

    std::size_t next_object_index = 0;
    if (!find_next_matching_object_index(plan, active.subscribe, info->first_loop_object_index, next_object_index)) {
        return false;
    }

    ++active.loop_cycle;
    active.next_object_index = next_object_index;
    active.completed = false;
    return true;
}

bool is_final_object_in_group(const openmoq::publisher::PublishPlan& plan, std::size_t object_index) {
    const auto& object = plan.objects.at(object_index);
    for (std::size_t index = object_index + 1; index < plan.objects.size(); ++index) {
        const auto& candidate = plan.objects[index];
        if (candidate.track_name == object.track_name && candidate.group_id == object.group_id &&
            candidate.object_id > object.object_id) {
            return false;
        }
    }
    return true;
}

bool is_final_object_in_subgroup(const openmoq::publisher::PublishPlan& plan, std::size_t object_index) {
    const auto& object = plan.objects.at(object_index);
    for (std::size_t index = object_index + 1; index < plan.objects.size(); ++index) {
        const auto& candidate = plan.objects[index];
        if (candidate.track_name == object.track_name && candidate.group_id == object.group_id &&
            candidate.subgroup_id == object.subgroup_id && candidate.object_id > object.object_id) {
            return false;
        }
    }
    return true;
}

bool subgroup_contains_group_largest(const openmoq::publisher::PublishPlan& plan, std::size_t object_index) {
    // The subgroup "contains the group's largest object" iff no later plan
    // object in the same track+group has a larger object_id outside this
    // subgroup. For the current baseline (every object is in subgroup 0) this
    // is equivalent to is_final_object_in_group; the helper is defined
    // explicitly so future multi-subgroup packagers don't mis-set the
    // END_OF_GROUP bit.
    const auto& object = plan.objects.at(object_index);
    for (std::size_t index = 0; index < plan.objects.size(); ++index) {
        const auto& candidate = plan.objects[index];
        if (candidate.track_name != object.track_name || candidate.group_id != object.group_id) {
            continue;
        }
        if (candidate.object_id > object.object_id && candidate.subgroup_id != object.subgroup_id) {
            return false;
        }
    }
    return true;
}

// Tracks per-subgroup open QUIC streams so that the same subgroup's objects
// are appended onto a single data stream rather than splitting across streams
// (draft-16 §2.2 / §10.4.2). Callers instantiate one of these per "sending
// context" -- per-subscription for serve_subscriptions, per-track for the
// publish paths -- and delegate the stream lifecycle to it.
class SubgroupSenderState {
public:
    // Write one object. If no stream is currently open for this object's
    // (group_id, subgroup_id), opens a uni stream, emits its SUBGROUP_HEADER
    // (with END_OF_GROUP set when the subgroup owns the group's largest
    // object), then appends the object. If is_final_in_subgroup is true, the
    // write is FIN'd and the stream is released.
    TransportStatus serve(PublisherTransport& transport,
                          openmoq::publisher::DraftVersion draft,
                          std::uint64_t track_alias,
                          std::uint64_t send_seq,
                          const openmoq::publisher::CmsfObject& object,
                          bool subgroup_contains_group_largest,
                          bool is_final_in_subgroup,
                          std::span<const std::uint8_t> payload) {
        const Key key{static_cast<std::uint64_t>(object.group_id), object.subgroup_id};
        auto it = streams_.find(key);
        std::optional<std::uint64_t> previous_object_id;
        std::uint64_t stream_id = 0;

        std::vector<std::uint8_t> wire_bytes;
        const bool opened_stream = it == streams_.end();
        if (it == streams_.end()) {
            TransportStatus status = transport.open_stream(StreamDirection::kUnidirectional, stream_id);
            if (!status.ok) {
                return status;
            }
            wire_bytes = encode_subgroup_header(
                draft, track_alias, static_cast<std::uint64_t>(object.group_id),
                object.subgroup_id, subgroup_contains_group_largest);
            ++stream_count_;
            it = streams_.emplace(key, OpenStream{stream_id, 0}).first;
        } else {
            stream_id = it->second.stream_id;
            previous_object_id = it->second.last_object_id;
        }

        std::vector<std::uint8_t> object_bytes =
            encode_subgroup_object(previous_object_id, object.object_id, payload);
        wire_bytes.insert(wire_bytes.end(), object_bytes.begin(), object_bytes.end());

        if (trace_enabled()) {
            const auto now_ms = trace_elapsed_ms(std::chrono::steady_clock::now());
            std::cerr << "[moqt-session] enqueue object stream=" << stream_id
                      << " send_seq=" << send_seq
                      << " now_ms=" << now_ms
                      << " opened=" << (opened_stream ? 1 : 0)
                      << " track=" << object.track_name
                      << " group=" << object.group_id
                      << " subgroup=" << object.subgroup_id
                      << " object=" << object.object_id
                      << " payload_bytes=" << payload.size()
                      << " wire_bytes=" << wire_bytes.size()
                      << " fin=" << (is_final_in_subgroup ? 1 : 0)
                      << std::endl;
            trace_csv_write_enqueue(send_seq,
                                    now_ms,
                                    object,
                                    stream_id,
                                    opened_stream,
                                    payload.size(),
                                    wire_bytes.size(),
                                    is_final_in_subgroup);
        }

        const TransportStatus status = transport.write_stream(stream_id, wire_bytes, is_final_in_subgroup);
        if (!status.ok) {
            return status;
        }

        if (is_final_in_subgroup) {
            streams_.erase(it);
        } else {
            it->second.last_object_id = object.object_id;
        }
        return TransportStatus::success();
    }

    std::uint64_t stream_count() const { return stream_count_; }

private:
    struct Key {
        std::uint64_t group_id;
        std::uint64_t subgroup_id;
        friend bool operator<(const Key& a, const Key& b) {
            return std::tie(a.group_id, a.subgroup_id) < std::tie(b.group_id, b.subgroup_id);
        }
    };
    struct OpenStream {
        std::uint64_t stream_id;
        std::uint64_t last_object_id;
    };
    std::map<Key, OpenStream> streams_;
    std::uint64_t stream_count_ = 0;
};

TransportStatus finalize_subscription(PublisherTransport& transport,
                                      std::uint64_t control_stream_id,
                                      std::uint64_t request_id,
                                      std::uint64_t stream_count,
                                      std::set<std::uint64_t>& completed_request_ids) {
    const TransportStatus write_status = transport.write_stream(
        control_stream_id, encode_publish_done_message(request_id, stream_count), false);
    if (!write_status.ok) {
        return write_status;
    }
    completed_request_ids.insert(request_id);
    return TransportStatus::success();
}

TransportStatus serve_subscriptions(PublisherTransport& transport,
                                    std::uint64_t control_stream_id,
                                    const openmoq::publisher::PublishPlan& plan,
                                    const LoopState& loop_state,
                                    const std::map<std::string, PublishedTrack>& tracks_by_name,
                                    openmoq::publisher::DraftVersion draft,
                                    std::string_view track_namespace,
                                    bool paced,
                                    std::chrono::milliseconds subscriber_timeout,
                                    std::vector<std::uint8_t>& pending_control_bytes,
                                    bool send_namespace_done = true,
                                    std::map<std::uint64_t, DormantPublishedTrack>* dormant_published_tracks = nullptr,
                                    const std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias = nullptr) {
    std::vector<std::uint8_t> buffer = std::move(pending_control_bytes);
    pending_control_bytes.clear();
    std::set<std::uint64_t> completed_request_ids;
    std::map<std::uint64_t, SubscribeMessage> pending_subscriptions;
    std::deque<std::uint64_t> pending_subscription_order;
    std::map<std::uint64_t, ActiveSubscription> active_subscriptions;
    bool fin = false;
    bool served_any_subscription = false;
    std::uint64_t first_media_time_us = 0;
    bool first_media_time_set = false;
    const auto pacing_start = std::chrono::steady_clock::now();
    NamespaceMessage namespace_message{
        .draft = draft,
        .track_namespace = std::string(track_namespace),
        .request_id = 0,
    };

    while (true) {
        std::size_t message_size = 0;
        while (next_control_message(buffer, draft, message_size)) {
            const std::vector<std::uint8_t> message_bytes(buffer.begin(), buffer.begin() + message_size);
            std::size_t offset = 0;
            std::uint64_t message_type = 0;
            if (!decode_varint(message_bytes, offset, message_type)) {
                return TransportStatus::failure("failed to parse control request type");
            }
            trace_control_message(message_bytes, draft);

            // Discard parsed control messages we don't act on (for example,
            // acknowledged responses, FETCH, GOAWAY, or UNSUBSCRIBE) so they
            // do not block the control-stream buffer. This only applies to
            // messages that next_control_message() can frame successfully;
            // log them for visibility only when tracing is explicitly enabled.
            const bool is_handled_type =
                message_type == 0x02 ||  // SUBSCRIBE_UPDATE
                message_type == 0x11 ||  // SUBSCRIBE_NAMESPACE
                message_type == 0x03;    // SUBSCRIBE
            if (!is_handled_type) {
                if (trace_enabled()) {
                    std::cerr << "[moqt-session] skipping unhandled control message type=0x"
                              << std::hex << message_type << std::dec
                              << " (" << control_message_type_name(message_type, draft) << ")"
                              << " size=" << message_size << '\n';
                }
                buffer.erase(buffer.begin(), buffer.begin() + message_size);
                continue;
            }

            if (message_type == 0x02) {
                SubscribeUpdateMessage subscribe_update;
                if (!decode_subscribe_update_message(message_bytes, subscribe_update) &&
                    !decode_legacy_subscribe_update_message(message_bytes,
                                                            tracks_by_name,
                                                            pending_subscriptions,
                                                            request_id_by_track_alias,
                                                            subscribe_update)) {
                    return TransportStatus::failure("received invalid SUBSCRIBE_UPDATE");
                }
                const auto original_request_id = subscribe_update.subscription_request_id;
                auto remapped_request_id = subscribe_update.subscription_request_id;
                if (request_id_by_track_alias != nullptr && !pending_subscriptions.contains(remapped_request_id) &&
                    !active_subscriptions.contains(remapped_request_id) &&
                    (dormant_published_tracks == nullptr || !dormant_published_tracks->contains(remapped_request_id))) {
                    const auto alias_it = request_id_by_track_alias->find(remapped_request_id);
                    if (alias_it != request_id_by_track_alias->end()) {
                        remapped_request_id = alias_it->second;
                        subscribe_update.subscription_request_id = remapped_request_id;
                    }
                }
                if (completed_request_ids.contains(remapped_request_id)) {
                    trace_subscribe_update_state("request already completed",
                                                 subscribe_update,
                                                 original_request_id,
                                                 remapped_request_id,
                                                 pending_subscriptions,
                                                 active_subscriptions,
                                                 dormant_published_tracks,
                                                 completed_request_ids,
                                                 request_id_by_track_alias);
                    return TransportStatus::failure("received invalid SUBSCRIBE_UPDATE state transition");
                }

                auto pending_it = pending_subscriptions.find(remapped_request_id);
                if (pending_it != pending_subscriptions.end()) {
                    if (!apply_subscribe_update(pending_it->second, subscribe_update)) {
                        trace_subscribe_update_state("pending update not monotonic",
                                                     subscribe_update,
                                                     original_request_id,
                                                     remapped_request_id,
                                                     pending_subscriptions,
                                                     active_subscriptions,
                                                     dormant_published_tracks,
                                                     completed_request_ids,
                                                     request_id_by_track_alias);
                        return TransportStatus::failure("received invalid SUBSCRIBE_UPDATE state transition");
                    }
                    buffer.erase(buffer.begin(), buffer.begin() + message_size);
                    continue;
                }

                auto active_it = active_subscriptions.find(remapped_request_id);
                if (active_it != active_subscriptions.end()) {
                    if (!apply_subscribe_update(active_it->second.subscribe, subscribe_update)) {
                        trace_subscribe_update_state("active update not monotonic",
                                                     subscribe_update,
                                                     original_request_id,
                                                     remapped_request_id,
                                                     pending_subscriptions,
                                                     active_subscriptions,
                                                     dormant_published_tracks,
                                                     completed_request_ids,
                                                     request_id_by_track_alias);
                        return TransportStatus::failure("received invalid SUBSCRIBE_UPDATE state transition");
                    }

                    std::size_t next_object_index = 0;
                    if (find_next_matching_object_index(plan,
                                                        active_it->second.subscribe,
                                                        active_it->second.next_object_index,
                                                        next_object_index)) {
                        active_it->second.next_object_index = next_object_index;
                        active_it->second.completed = false;
                    } else {
                        active_it->second.next_object_index = plan.objects.size();
                        active_it->second.completed =
                            !advance_subscription_to_next_loop_object(plan, loop_state, active_it->second);
                    }
                    buffer.erase(buffer.begin(), buffer.begin() + message_size);
                    continue;
                }

                if (dormant_published_tracks != nullptr) {
                    auto dormant_it = dormant_published_tracks->find(remapped_request_id);
                    if (dormant_it != dormant_published_tracks->end()) {
                        ActiveSubscription active{
                            .subscribe = dormant_it->second.subscribe,
                            .track = dormant_it->second.track,
                            .sender = std::make_shared<SubgroupSenderState>(),
                            .loop_cycle = 0,
                            .next_object_index = 0,
                            .completed = false,
                        };
                        if (!apply_subscribe_update(active.subscribe, subscribe_update)) {
                            trace_subscribe_update_state("dormant update not monotonic",
                                                         subscribe_update,
                                                         original_request_id,
                                                         remapped_request_id,
                                                         pending_subscriptions,
                                                         active_subscriptions,
                                                         dormant_published_tracks,
                                                         completed_request_ids,
                                                         request_id_by_track_alias);
                            return TransportStatus::failure("received invalid SUBSCRIBE_UPDATE state transition");
                        }

                        std::size_t next_object_index = 0;
                        if (find_next_matching_object_index(plan, active.subscribe, 0, next_object_index)) {
                            active.next_object_index = next_object_index;
                            active_subscriptions.insert_or_assign(remapped_request_id, std::move(active));
                        } else {
                            if (advance_subscription_to_next_loop_object(plan, loop_state, active)) {
                                active_subscriptions.insert_or_assign(remapped_request_id, std::move(active));
                            } else {
                                const TransportStatus finalize_status = finalize_subscription(
                                    transport, control_stream_id, remapped_request_id, 0, completed_request_ids);
                                if (!finalize_status.ok) {
                                    return finalize_status;
                                }
                                served_any_subscription = true;
                            }
                        }
                        dormant_published_tracks->erase(dormant_it);
                        buffer.erase(buffer.begin(), buffer.begin() + message_size);
                        continue;
                    }
                }

                trace_subscribe_update_state("unknown subscription_request_id",
                                             subscribe_update,
                                             original_request_id,
                                             remapped_request_id,
                                             pending_subscriptions,
                                             active_subscriptions,
                                             dormant_published_tracks,
                                             completed_request_ids,
                                             request_id_by_track_alias);
                return TransportStatus::failure("received invalid SUBSCRIBE_UPDATE state transition");
            }

            if (message_type == 0x11) {
                SubscribeNamespaceMessage subscribe_namespace;
                if (!decode_subscribe_namespace_message(message_bytes, draft, subscribe_namespace)) {
                    return TransportStatus::failure("received invalid SUBSCRIBE_NAMESPACE");
                }
                if (!namespace_prefix_matches(subscribe_namespace.track_namespace_prefix, track_namespace)) {
                    return TransportStatus::failure("peer requested unsupported namespace prefix");
                }
                const TransportStatus write_status =
                    transport.write_stream(control_stream_id,
                                           encode_subscribe_namespace_ok_message(draft, subscribe_namespace.request_id),
                                           false);
                if (!write_status.ok) {
                    return write_status;
                }
                buffer.erase(buffer.begin(), buffer.begin() + message_size);
                continue;
            }

            if (message_type == 0x03) {
                SubscribeMessage subscribe;
                if (!decode_subscribe_message(message_bytes, draft, subscribe)) {
                    return TransportStatus::failure("received invalid SUBSCRIBE");
                }
                if (!namespace_matches(subscribe.track_namespace, track_namespace)) {
                    return TransportStatus::failure("peer requested unsupported track namespace");
                }
                pending_subscriptions.insert_or_assign(subscribe.request_id, subscribe);
                pending_subscription_order.push_back(subscribe.request_id);
                buffer.erase(buffer.begin(), buffer.begin() + message_size);
                continue;
            }
        }

        if (!fin && !pending_subscription_order.empty()) {
            std::vector<std::uint8_t> chunk;
            bool immediate_fin = false;
            const TransportStatus read_status =
                transport.read_stream(control_stream_id, chunk, immediate_fin, std::chrono::milliseconds(0));
            if (read_status.ok) {
                if (trace_enabled()) {
                    std::cerr << "[moqt-session] control chunk now_ms="
                              << trace_elapsed_ms(std::chrono::steady_clock::now())
                              << " fin=" << (immediate_fin ? 1 : 0) << " bytes=["
                              << hex_dump(chunk) << "]" << std::endl;
                }
                buffer.insert(buffer.end(), chunk.begin(), chunk.end());
                fin = immediate_fin;
                continue;
            }
            if (read_status.message != "timed out waiting for stream data") {
                return read_status;
            }
        }

        while (!pending_subscription_order.empty()) {
            const std::uint64_t request_id = pending_subscription_order.front();
            pending_subscription_order.pop_front();
            auto pending_it = pending_subscriptions.find(request_id);
            if (pending_it == pending_subscriptions.end()) {
                continue;
            }

            const SubscribeMessage subscribe = pending_it->second;
            pending_subscriptions.erase(pending_it);

            const auto track_it = tracks_by_name.find(subscribe.track_name);
            if (track_it == tracks_by_name.end()) {
                const TransportStatus write_status =
                    transport.write_stream(control_stream_id,
                                           encode_request_error_message(
                                               draft, subscribe.request_id, 0x2, 0, "track does not exist"),
                                           false);
                if (!write_status.ok) {
                    return write_status;
                }
                continue;
            }

            const TransportStatus write_status =
                transport.write_stream(control_stream_id,
                                       encode_subscribe_ok_message(draft,
                                                                   subscribe.request_id,
                                                                   track_it->second.alias,
                                                                   0,
                                                                   0,
                                                                   false),
                                       false);
            if (!write_status.ok) {
                return write_status;
            }
            std::cerr << "[moqt-session] accepted subscribe track=" << subscribe.track_name
                      << " request_id=" << subscribe.request_id << '\n';

            ActiveSubscription active{
                .subscribe = subscribe,
                .track = track_it->second,
                .sender = std::make_shared<SubgroupSenderState>(),
                .loop_cycle = 0,
                .next_object_index = 0,
                .completed = false,
            };

            std::size_t next_object_index = 0;
            if (find_next_matching_object_index(plan, active.subscribe, 0, next_object_index)) {
                active.next_object_index = next_object_index;
                active_subscriptions.insert_or_assign(request_id, std::move(active));
                continue;
            }

            const TransportStatus finalize_status =
                finalize_subscription(transport, control_stream_id, request_id, 0, completed_request_ids);
            if (!finalize_status.ok) {
                return finalize_status;
            }
            served_any_subscription = true;
        }

        if (!active_subscriptions.empty()) {
            std::vector<std::uint8_t> chunk;
            bool immediate_fin = false;
            const TransportStatus read_status =
                transport.read_stream(control_stream_id, chunk, immediate_fin, std::chrono::milliseconds(0));
            if (read_status.ok) {
                if (trace_enabled()) {
                    std::cerr << "[moqt-session] control chunk now_ms="
                              << trace_elapsed_ms(std::chrono::steady_clock::now())
                              << " fin=" << (immediate_fin ? 1 : 0) << " bytes=["
                              << hex_dump(chunk) << "]" << std::endl;
                }
                buffer.insert(buffer.end(), chunk.begin(), chunk.end());
                fin = immediate_fin;
                continue;
            }
            if (read_status.message != "timed out waiting for stream data") {
                return read_status;
            }

            // Pick the (loop_cycle, media_time_us) of the earliest pending
            // object across all active subscriptions. Scheduling by media time
            // — rather than plan-array index — keeps tracks properly
            // interleaved when the plan lays tracks out track-by-track (e.g.
            // a single giant video group followed by a single giant audio
            // group). Ordering by plan index in that layout would send every
            // video object before any audio object, starving the audio track.
            std::size_t next_plan_index = plan.objects.size();
            std::size_t next_loop_cycle = 0;
            std::uint64_t next_media_time_us = 0;
            bool have_candidate = false;
            for (const auto& [request_id, active] : active_subscriptions) {
                static_cast<void>(request_id);
                if (active.completed) {
                    continue;
                }
                if (active.next_object_index >= plan.objects.size()) {
                    continue;
                }
                const std::uint64_t candidate_time_us = plan.objects[active.next_object_index].media_time_us;
                const bool is_earlier = !have_candidate ||
                                        active.loop_cycle < next_loop_cycle ||
                                        (active.loop_cycle == next_loop_cycle &&
                                         (candidate_time_us < next_media_time_us ||
                                          (candidate_time_us == next_media_time_us &&
                                           active.next_object_index < next_plan_index)));
                if (is_earlier) {
                    next_plan_index = active.next_object_index;
                    next_loop_cycle = active.loop_cycle;
                    next_media_time_us = candidate_time_us;
                    have_candidate = true;
                }
            }

            if (next_plan_index < plan.objects.size()) {
                const auto& source_object = plan.objects[next_plan_index];
                const openmoq::publisher::CmsfObject object =
                    make_looped_object(source_object, loop_state, next_loop_cycle);
                const auto payload = object_payload(source_object);
                if (payload.empty()) {
                    return TransportStatus::failure("transport publish requires materialized object payloads");
                }

                if (object.kind == openmoq::publisher::CmsfObjectKind::kMedia && !first_media_time_set) {
                    first_media_time_us = object.media_time_us;
                    first_media_time_set = true;
                }
                const std::uint64_t send_seq = object.kind == openmoq::publisher::CmsfObjectKind::kMedia ? next_send_seq() : 0;
                trace_pacing_decision("before", send_seq, pacing_start, first_media_time_us, object, paced);
                pace_until(pacing_start, first_media_time_us, object, paced);
                trace_pacing_decision("after", send_seq, pacing_start, first_media_time_us, object, paced);

                for (auto& [request_id, active] : active_subscriptions) {
                    if (active.loop_cycle != next_loop_cycle || active.next_object_index != next_plan_index) {
                        continue;
                    }

                    const TransportStatus write_status = active.sender->serve(
                        transport,
                        draft,
                        active.track.alias,
                        send_seq,
                        object,
                        subgroup_contains_group_largest(plan, next_plan_index),
                        is_final_object_in_subgroup(plan, next_plan_index),
                        payload);
                    if (!write_status.ok) {
                        return write_status;
                    }
                    std::cerr << "[moqt-session] served object send_seq=" << send_seq
                              << " now_ms=" << trace_elapsed_ms(std::chrono::steady_clock::now())
                              << " track=" << object.track_name
                              << " group=" << object.group_id << " object=" << object.object_id
                              << " bytes=" << object_payload_size(source_object) << '\n';
                    trace_csv_write_served("served",
                                           send_seq,
                                           trace_elapsed_ms(std::chrono::steady_clock::now()),
                                           object);

                    std::size_t upcoming_object_index = 0;
                    if (find_next_matching_object_index(plan,
                                                        active.subscribe,
                                                        next_plan_index + 1,
                                                        upcoming_object_index)) {
                        active.next_object_index = upcoming_object_index;
                    } else {
                        static_cast<void>(request_id);
                        active.next_object_index = plan.objects.size();
                        active.completed = !advance_subscription_to_next_loop_object(plan, loop_state, active);
                    }
                }

                served_any_subscription = true;
                continue;
            }

            std::vector<std::uint64_t> completed_request_ids_to_finalize;
            for (const auto& [request_id, active] : active_subscriptions) {
                if (active.completed) {
                    completed_request_ids_to_finalize.push_back(request_id);
                }
            }
            for (const auto request_id : completed_request_ids_to_finalize) {
                const auto active_it = active_subscriptions.find(request_id);
                if (active_it == active_subscriptions.end()) {
                    continue;
                }
                const TransportStatus finalize_status =
                    finalize_subscription(transport,
                                          control_stream_id,
                                          request_id,
                                          active_it->second.sender->stream_count(),
                                          completed_request_ids);
                if (!finalize_status.ok) {
                    return finalize_status;
                }
                active_subscriptions.erase(active_it);
            }
            if (!completed_request_ids_to_finalize.empty()) {
                served_any_subscription = true;
                continue;
            }
        }

        if (fin) {
            break;
        }

        std::vector<std::uint8_t> chunk;
        const TransportStatus read_status = transport.read_stream(control_stream_id, chunk, fin, subscriber_timeout);
        if (!read_status.ok) {
            if (!served_any_subscription && is_idle_subscribe_exit(read_status.message)) {
                break;
            }
            if (read_status.message == "timed out waiting for stream data" ||
                read_status.message == "no queued read for stream") {
                break;
            }
            return read_status;
        }

        if (trace_enabled()) {
            std::cerr << "[moqt-session] control chunk now_ms="
                      << trace_elapsed_ms(std::chrono::steady_clock::now())
                      << " fin=" << (fin ? 1 : 0) << " bytes=[" << hex_dump(chunk)
                      << "]" << std::endl;
        }
        buffer.insert(buffer.end(), chunk.begin(), chunk.end());
    }

    if (!served_any_subscription) {
        std::cerr << "[moqt-session] no downstream SUBSCRIBE before timeout; closing idle publish session" << '\n';
        pending_control_bytes = std::move(buffer);
        if (!send_namespace_done) {
            return TransportStatus::success();
        }
        return transport.write_stream(control_stream_id, encode_publish_namespace_done_message(namespace_message), false);
    }

    pending_control_bytes = std::move(buffer);
    if (!send_namespace_done) {
        return TransportStatus::success();
    }
    return transport.write_stream(control_stream_id, encode_publish_namespace_done_message(namespace_message), false);
}

TransportStatus forward_published_tracks(PublisherTransport& transport,
                                         std::uint64_t control_stream_id,
                                         const openmoq::publisher::PublishPlan& plan,
                                         const LoopState& loop_state,
                                         std::span<const PublishedTrack> tracks,
                                         std::uint64_t peer_max_request_id,
                                         std::string_view track_namespace,
                                         bool paced,
                                         std::chrono::milliseconds subscriber_timeout,
                                         std::vector<std::uint8_t>& pending_control_bytes) {
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
        std::cerr << "[moqt-session] publish track namespace=" << track_namespace << " track=" << track.name
                  << " request_id=" << next_request_id << " alias=" << track.alias << '\n';
        next_request_id += 2;
    }

    std::map<std::uint64_t, PublishOk> publish_ok_by_request_id;
    TransportStatus status = collect_control_acknowledgements(transport,
                                                              control_stream_id,
                                                              plan.draft.version,
                                                              0,
                                                              tracks.size(),
                                                              pending_control_bytes,
                                                              &publish_ok_by_request_id);
    if (!status.ok) {
        return status;
    }

    std::map<std::string, SubgroupSenderState> sender_by_track;
    std::map<std::string, PublishedTrack> downgraded_tracks_by_name;
    const auto pacing_start = std::chrono::steady_clock::now();
    std::uint64_t first_media_time_us = 0;
    bool first_media_time_set = false;
    std::size_t loop_cycle = 0;
    std::size_t cycle_start_index = 0;

    while (true) {
        for (std::size_t object_index = cycle_start_index; object_index < plan.objects.size(); ++object_index) {
            const auto& source_object = plan.objects[object_index];
            const auto track_it = tracks_by_name.find(source_object.track_name);
            if (track_it == tracks_by_name.end()) {
                continue;
            }
            const auto publish_ok_it = publish_ok_by_request_id.find(request_id_by_track.at(source_object.track_name));
            if (publish_ok_it == publish_ok_by_request_id.end()) {
                return TransportStatus::failure("missing PUBLISH_OK for published track");
            }
            if (publish_ok_it->second.forward == 0) {
                std::cerr << "[moqt-session] publish accepted without forwarding track=" << source_object.track_name
                          << " request_id=" << request_id_by_track.at(source_object.track_name) << '\n';
                downgraded_tracks_by_name.emplace(track_it->first, track_it->second);
                continue;
            }

            const openmoq::publisher::CmsfObject object =
                make_looped_object(source_object, loop_state, loop_cycle);
            const auto payload = object_payload(source_object);
            if (payload.empty()) {
                return TransportStatus::failure("transport publish requires materialized object payloads");
            }
            if (object.kind == openmoq::publisher::CmsfObjectKind::kMedia && !first_media_time_set) {
                first_media_time_us = object.media_time_us;
                first_media_time_set = true;
            }
            const std::uint64_t send_seq = object.kind == openmoq::publisher::CmsfObjectKind::kMedia ? next_send_seq() : 0;
            trace_pacing_decision("before", send_seq, pacing_start, first_media_time_us, object, paced);
            pace_until(pacing_start, first_media_time_us, object, paced);
            trace_pacing_decision("after", send_seq, pacing_start, first_media_time_us, object, paced);

            status = sender_by_track[source_object.track_name].serve(
                transport,
                plan.draft.version,
                track_it->second.alias,
                send_seq,
                object,
                subgroup_contains_group_largest(plan, object_index),
                is_final_object_in_subgroup(plan, object_index),
                payload);
            if (!status.ok) {
                return status;
            }
            std::cerr << "[moqt-session] sent object send_seq=" << send_seq
                      << " now_ms=" << trace_elapsed_ms(std::chrono::steady_clock::now())
                      << " track=" << object.track_name << " group=" << object.group_id
                      << " object=" << object.object_id << " bytes=" << object_payload_size(source_object)
                      << " kind="
                      << (object.kind == openmoq::publisher::CmsfObjectKind::kInitialization ? "catalog" : "media")
                      << '\n';
            trace_csv_write_served("sent", send_seq, trace_elapsed_ms(std::chrono::steady_clock::now()), object);
        }

        if (!loop_state.enabled) {
            break;
        }

        cycle_start_index = plan.objects.size();
        for (const auto& [track_name, info] : loop_state.tracks) {
            static_cast<void>(track_name);
            if (info.has_loopable_objects) {
                cycle_start_index = std::min(cycle_start_index, info.first_loop_object_index);
            }
        }
        if (cycle_start_index >= plan.objects.size()) {
            break;
        }
        ++loop_cycle;
    }

    for (const auto& track : tracks) {
        const auto publish_ok_it = publish_ok_by_request_id.find(request_id_by_track.at(track.name));
        if (publish_ok_it == publish_ok_by_request_id.end()) {
            return TransportStatus::failure("missing PUBLISH_OK for published track");
        }
        if (publish_ok_it->second.forward == 0) {
            downgraded_tracks_by_name.emplace(track.name, track);
            continue;
        }
        if (loop_state.enabled && track_can_loop(loop_state, track.name)) {
            continue;
        }
        status = transport.write_stream(control_stream_id,
                                        encode_publish_done_message(request_id_by_track.at(track.name),
                                                                    sender_by_track[track.name].stream_count()),
                                        false);
        if (!status.ok) {
            return status;
        }
    }

    if (!downgraded_tracks_by_name.empty()) {
        std::cerr << "[moqt-session] waiting for downstream SUBSCRIBE on "
                  << downgraded_tracks_by_name.size() << " track(s) after forward=0 reply" << '\n';
        status = serve_subscriptions(transport,
                                     control_stream_id,
                                     plan,
                                     loop_state,
                                     downgraded_tracks_by_name,
                                     plan.draft.version,
                                     track_namespace,
                                     paced,
                                     subscriber_timeout,
                                     pending_control_bytes,
                                     false);
        if (!status.ok) {
            return status;
        }
    }

    if (loop_state.enabled) {
        return TransportStatus::success();
    }

    return transport.write_stream(control_stream_id,
                                  encode_publish_namespace_done_message({
                                      .draft = plan.draft.version,
                                      .track_namespace = std::string(track_namespace),
                                      .request_id = 0,
                                  }),
                                  false);
}

TransportStatus publish_selected_tracks(PublisherTransport& transport,
                                        std::uint64_t control_stream_id,
                                        const openmoq::publisher::PublishPlan& plan,
                                        const LoopState& loop_state,
                                        std::span<const PublishedTrack> tracks,
                                        std::uint64_t peer_max_request_id,
                                        std::string_view track_namespace,
                                        bool paced,
                                        std::vector<std::uint8_t>& pending_control_bytes,
                                        std::map<std::uint64_t, DormantPublishedTrack>* dormant_published_tracks = nullptr,
                                        std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias = nullptr) {
    if (tracks.empty()) {
        return TransportStatus::success();
    }

    std::map<std::string, std::uint64_t> request_id_by_track;
    std::map<std::string, PublishedTrack> tracks_by_name;
    std::uint64_t next_request_id = 2;

    for (const auto& track : tracks) {
        if (next_request_id > peer_max_request_id) {
            return TransportStatus::failure("peer max_request_id is too small for publish requests");
        }

        request_id_by_track.emplace(track.name, next_request_id);
        tracks_by_name.emplace(track.name, track);
        const TransportStatus status =
            transport.write_stream(control_stream_id,
                                   encode_track_message({
                                       .draft = plan.draft.version,
                                       .track_name = track.name,
                                       .track_namespace = std::string(track_namespace),
                                       .request_id = next_request_id,
                                       .track_alias = track.alias,
                                       .largest_group_id = track.largest_group_id,
                                       .largest_object_id = track.largest_object_id,
                                       .content_exists = track.content_exists,
                                   }),
                                   false);
        if (!status.ok) {
            return status;
        }
        std::cerr << "[moqt-session] publish selected track namespace=" << track_namespace
                  << " track=" << track.name << " request_id=" << next_request_id
                  << " alias=" << track.alias << '\n';
        next_request_id += 2;
    }

    std::map<std::uint64_t, PublishOk> publish_ok_by_request_id;
    TransportStatus status = collect_control_acknowledgements(transport,
                                                              control_stream_id,
                                                              plan.draft.version,
                                                              0,
                                                              tracks.size(),
                                                              pending_control_bytes,
                                                              &publish_ok_by_request_id);
    if (!status.ok) {
        return status;
    }

    const auto pacing_start = std::chrono::steady_clock::now();
    std::uint64_t first_media_time_us = 0;
    bool first_media_time_set = false;
    std::map<std::string, SubgroupSenderState> sender_by_track;

    std::size_t loop_cycle = 0;
    std::size_t cycle_start_index = 0;
    while (true) {
        for (std::size_t object_index = cycle_start_index; object_index < plan.objects.size(); ++object_index) {
            const auto& source_object = plan.objects[object_index];
            const auto track_it = tracks_by_name.find(source_object.track_name);
            if (track_it == tracks_by_name.end()) {
                continue;
            }

            const auto publish_ok_it = publish_ok_by_request_id.find(request_id_by_track.at(source_object.track_name));
            if (publish_ok_it == publish_ok_by_request_id.end()) {
                return TransportStatus::failure("missing PUBLISH_OK for published track");
            }
            if (publish_ok_it->second.forward == 0) {
                if (dormant_published_tracks != nullptr) {
                    dormant_published_tracks->insert_or_assign(
                        request_id_by_track.at(source_object.track_name),
                        DormantPublishedTrack{
                            .subscribe =
                                SubscribeMessage{
                                    .request_id = request_id_by_track.at(source_object.track_name),
                                    .track_namespace = split_track_namespace_components(track_namespace),
                                    .track_name = source_object.track_name,
                                    .subscriber_priority = publish_ok_it->second.subscriber_priority,
                                    .group_order = publish_ok_it->second.group_order,
                                    .forward = publish_ok_it->second.forward,
                                    .filter_type = publish_ok_it->second.filter_type == 0 ? 0x03 : publish_ok_it->second.filter_type,
                                    .start_group_id = 0,
                                    .start_object_id = 0,
                                    .end_group_id = 0,
                                },
                            .track = track_it->second,
                        });
                }
                if (request_id_by_track_alias != nullptr) {
                    request_id_by_track_alias->insert_or_assign(track_it->second.alias,
                                                                request_id_by_track.at(source_object.track_name));
                }
                continue;
            }

            const openmoq::publisher::CmsfObject object =
                make_looped_object(source_object, loop_state, loop_cycle);
            const auto payload = object_payload(source_object);
            if (payload.empty()) {
                return TransportStatus::failure("transport publish requires materialized object payloads");
            }
            if (object.kind == openmoq::publisher::CmsfObjectKind::kMedia && !first_media_time_set) {
                first_media_time_us = object.media_time_us;
                first_media_time_set = true;
            }
            const std::uint64_t send_seq = object.kind == openmoq::publisher::CmsfObjectKind::kMedia ? next_send_seq() : 0;
            trace_pacing_decision("before", send_seq, pacing_start, first_media_time_us, object, paced);
            pace_until(pacing_start, first_media_time_us, object, paced);
            trace_pacing_decision("after", send_seq, pacing_start, first_media_time_us, object, paced);

            status = sender_by_track[source_object.track_name].serve(
                transport,
                plan.draft.version,
                track_it->second.alias,
                send_seq,
                object,
                subgroup_contains_group_largest(plan, object_index),
                is_final_object_in_subgroup(plan, object_index),
                payload);
            if (!status.ok) {
                return status;
            }
            std::cerr << "[moqt-session] sent selected object send_seq=" << send_seq
                      << " now_ms=" << trace_elapsed_ms(std::chrono::steady_clock::now())
                      << " track=" << object.track_name
                      << " group=" << object.group_id
                      << " object=" << object.object_id
                      << " bytes=" << object_payload_size(source_object)
                      << " kind="
                      << (object.kind == openmoq::publisher::CmsfObjectKind::kInitialization ? "catalog" : "media")
                      << '\n';
            trace_csv_write_served("sent_selected",
                                   send_seq,
                                   trace_elapsed_ms(std::chrono::steady_clock::now()),
                                   object);
        }

        if (!loop_state.enabled) {
            break;
        }

        cycle_start_index = plan.objects.size();
        for (const auto& track : tracks) {
            const TrackLoopInfo* info = find_track_loop_info(loop_state, track.name);
            if (info != nullptr && info->has_loopable_objects) {
                cycle_start_index = std::min(cycle_start_index, info->first_loop_object_index);
            }
        }
        if (cycle_start_index >= plan.objects.size()) {
            break;
        }
        ++loop_cycle;
    }

    for (const auto& track : tracks) {
        const auto publish_ok_it = publish_ok_by_request_id.find(request_id_by_track.at(track.name));
        if (publish_ok_it == publish_ok_by_request_id.end()) {
            return TransportStatus::failure("missing PUBLISH_OK for published track");
        }
        if (publish_ok_it->second.forward == 0) {
            std::cerr << "[moqt-session] selected publish accepted without forwarding track=" << track.name
                      << " request_id=" << request_id_by_track.at(track.name) << '\n';
            continue;
        }
        if (loop_state.enabled && track_can_loop(loop_state, track.name)) {
            continue;
        }
        status = transport.write_stream(control_stream_id,
                                        encode_publish_done_message(request_id_by_track.at(track.name),
                                                                    sender_by_track[track.name].stream_count()),
                                        false);
        if (!status.ok) {
            return status;
        }
    }

    return TransportStatus::success();
}

}  // namespace

MoqtSession::MoqtSession(PublisherTransport& transport,
                         std::string track_namespace,
                         bool auto_forward,
                         bool publish_catalog,
                         bool paced,
                         std::chrono::seconds subscriber_timeout)
    : MoqtSession(transport,
                  std::move(track_namespace),
                  auto_forward,
                  publish_catalog,
                  paced,
                  false,
                  subscriber_timeout) {}

MoqtSession::MoqtSession(PublisherTransport& transport,
                         std::string track_namespace,
                         bool auto_forward,
                         bool publish_catalog,
                         bool paced,
                         bool loop,
                         std::chrono::seconds subscriber_timeout)
    : transport_(transport),
      track_namespace_(std::move(track_namespace)),
      auto_forward_(auto_forward),
      publish_catalog_(publish_catalog),
      paced_(paced),
      loop_(loop),
      subscriber_timeout_(subscriber_timeout) {}

TransportStatus MoqtSession::connect(const EndpointConfig& endpoint, const TlsConfig& tls) {
    endpoint_ = endpoint;
    setup_complete_ = false;
    peer_max_request_id_ = 0;
    pending_control_bytes_.clear();
    TransportStatus status = transport_.configure(endpoint, tls);
    if (!status.ok) {
        return status;
    }

    status = transport_.connect();
    if (!status.ok) {
        return status;
    }

    if (trace_enabled()) {
        std::cerr << "[moqt-session] transport connected id=" << transport_.connection_id()
                  << " transport="
                  << (endpoint.transport == openmoq::publisher::transport::TransportKind::kWebTransport ? "webtransport"
                                                                                                         : "raw")
                  << std::endl;
    }

    return ensure_control_stream();
}

TransportStatus MoqtSession::publish(const openmoq::publisher::PublishPlan& plan) {
    if (transport_.state() != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

    TransportStatus status = ensure_setup(plan.draft.version);
    if (!status.ok) {
        if (trace_enabled()) {
            std::cerr << "[moqt-session] setup failed error=" << status.message << std::endl;
        }
        return status;
    }
    std::cout << "connection_id=" << transport_.connection_id() << '\n' << std::flush;

    const std::vector<PublishedTrack> tracks = build_published_tracks(plan);
    const LoopState loop_state = build_loop_state(plan, loop_);

    NamespaceMessage namespace_message{
        .draft = plan.draft.version,
        .track_namespace = track_namespace_,
        .request_id = 0,
    };
    status = write_frame(control_stream_id_, encode_namespace_message(namespace_message), false);
    if (!status.ok) {
        return status;
    }

    status = collect_control_acknowledgements(
        transport_, control_stream_id_, plan.draft.version, 1, 0, pending_control_bytes_);
    if (!status.ok) {
        return status;
    }
    std::cerr << "[moqt-session] namespace published namespace=" << track_namespace_ << " tracks=" << tracks.size()
              << " mode=" << (auto_forward_ ? "forward" : "await-subscribe") << '\n';

    std::map<std::string, PublishedTrack> tracks_by_name;
    for (auto track : tracks) {
        tracks_by_name.emplace(track.name, track);
    }

    if (auto_forward_) {
        return forward_published_tracks(
            transport_,
            control_stream_id_,
            plan,
            loop_state,
            tracks,
            peer_max_request_id_,
            track_namespace_,
            paced_,
            subscriber_timeout_,
            pending_control_bytes_);
    }

    if (publish_catalog_) {
        std::map<std::uint64_t, DormantPublishedTrack> dormant_published_tracks;
        std::map<std::uint64_t, std::uint64_t> request_id_by_track_alias;
        std::vector<PublishedTrack> selected_tracks;
        for (const auto& track : tracks) {
            if (track.name == "catalog") {
                selected_tracks.push_back(track);
            }
        }
        if (!selected_tracks.empty()) {
            status = publish_selected_tracks(transport_,
                                             control_stream_id_,
                                             plan,
                                             loop_state,
                                             selected_tracks,
                                             peer_max_request_id_,
                                             track_namespace_,
                                             paced_,
                                             pending_control_bytes_,
                                             &dormant_published_tracks,
                                             &request_id_by_track_alias);
            if (!status.ok) {
                return status;
            }

            return serve_subscriptions(transport_,
                                       control_stream_id_,
                                       plan,
                                       loop_state,
                                       tracks_by_name,
                                       plan.draft.version,
                                       track_namespace_,
                                       paced_,
                                       subscriber_timeout_,
                                       pending_control_bytes_,
                                       true,
                                       &dormant_published_tracks,
                                       &request_id_by_track_alias);
        }
    }

    std::cerr << "[moqt-session] awaiting SUBSCRIBE for tracks:";
    for (const auto& track : tracks) {
        std::cerr << ' ' << track.name;
    }
    std::cerr << '\n';

    return serve_subscriptions(transport_,
                               control_stream_id_,
                               plan,
                               loop_state,
                               tracks_by_name,
                               plan.draft.version,
                               track_namespace_,
                               paced_,
                               subscriber_timeout_,
                               pending_control_bytes_);
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
    const std::uint64_t max_request_id =
        endpoint_->transport == openmoq::publisher::transport::TransportKind::kWebTransport ? 128 : 100;
    const std::vector<std::uint8_t> setup_bytes = encode_setup_message({
        .draft = draft,
        .transport = endpoint_->transport,
        .authority = authority,
        .path = endpoint_->path,
        .max_request_id = max_request_id,
    });
    status = write_frame(control_stream_id_, setup_bytes, false);
    if (!status.ok) {
        return status;
    }
    if (trace_enabled()) {
        std::cerr << "[moqt-session] sent CLIENT_SETUP stream=" << control_stream_id_
                  << " draft=" << openmoq::publisher::to_string(draft)
                  << " transport="
                  << (endpoint_->transport == openmoq::publisher::transport::TransportKind::kWebTransport ? "webtransport"
                                                                                                            : "raw")
                  << " bytes=[" << hex_dump(setup_bytes) << "]"
                  << std::endl;
    }

    std::vector<std::uint8_t> response = std::move(pending_control_bytes_);
    pending_control_bytes_.clear();
    std::vector<std::uint8_t> chunk;
    bool fin = false;
    bool saw_server_setup = false;
    std::size_t consumed = 0;

    while (true) {
        std::size_t message_size = 0;
        if (!next_control_message(std::span<const std::uint8_t>(response).subspan(consumed), draft, message_size)) {
            if (fin) {
                break;
            }
            if (endpoint_->transport == openmoq::publisher::transport::TransportKind::kWebTransport) {
                bool saw_session_bytes = false;
                bool session_fin = false;
                const TransportStatus session_status = try_read_wt_session_stream(transport_, saw_session_bytes, session_fin);
                if (session_status.ok) {
                    if (session_fin) {
                        return TransportStatus::failure("webtransport session control stream closed during setup");
                    }
                    if (saw_session_bytes) {
                        continue;
                    }
                    continue;
                }
            }
            chunk.clear();
            status = transport_.read_stream(control_stream_id_, chunk, fin, std::chrono::seconds(5));
            if (!status.ok) {
                if (endpoint_->transport == openmoq::publisher::transport::TransportKind::kWebTransport) {
                    bool saw_session_bytes = false;
                    bool session_fin = false;
                    const TransportStatus session_status = try_read_wt_session_stream(transport_, saw_session_bytes, session_fin);
                    if (session_status.ok) {
                        if (session_fin) {
                            return TransportStatus::failure("webtransport session control stream closed during setup");
                        }
                        if (saw_session_bytes) {
                            continue;
                        }
                        continue;
                    }
                }
                if (trace_enabled()) {
                    std::cerr << "[moqt-session] setup read failed stream=" << control_stream_id_
                              << " error=" << status.message << std::endl;
                }
                return status;
            }
            if (trace_enabled()) {
                std::cerr << "[moqt-session] setup read now_ms="
                          << trace_elapsed_ms(std::chrono::steady_clock::now())
                          << " stream=" << control_stream_id_
                          << " bytes=" << chunk.size()
                          << " fin=" << (fin ? 1 : 0)
                          << " buffered=" << response.size() + chunk.size() << std::endl;
            }
            response.insert(response.end(), chunk.begin(), chunk.end());
            continue;
        }

        const std::vector<std::uint8_t> message_bytes(response.begin() + static_cast<std::ptrdiff_t>(consumed),
                                                      response.begin() + static_cast<std::ptrdiff_t>(consumed + message_size));
        std::size_t offset = 0;
        std::uint64_t message_type = 0;
        if (!decode_varint(message_bytes, offset, message_type)) {
            return TransportStatus::failure("failed to parse setup response type");
        }

        if (!saw_server_setup) {
            ServerSetupMessage server_setup;
            if (!decode_server_setup_message(message_bytes, server_setup)) {
                if (trace_enabled()) {
                    std::cerr << "[moqt-session] invalid SERVER_SETUP bytes=[" << hex_dump(message_bytes) << "]"
                              << std::endl;
                }
                return TransportStatus::failure("received invalid SERVER_SETUP message");
            }
            saw_server_setup = true;
            if (draft == openmoq::publisher::DraftVersion::kDraft14) {
                peer_max_request_id_ = server_setup.max_request_id;
            }
        } else if (draft == openmoq::publisher::DraftVersion::kDraft16 && message_type == 0x15) {
            MaxRequestIdMessage max_request_id;
            if (!decode_max_request_id_message(message_bytes, max_request_id)) {
                return TransportStatus::failure("received invalid MAX_REQUEST_ID message");
            }
            peer_max_request_id_ = max_request_id.max_request_id;
        } else {
            break;
        }

        consumed += message_size;
        if (saw_server_setup &&
            (draft == openmoq::publisher::DraftVersion::kDraft14 || !auto_forward_ || peer_max_request_id_ != 0)) {
            break;
        }
    }

    if (!saw_server_setup) {
        return TransportStatus::failure("received incomplete SERVER_SETUP message");
    }

    pending_control_bytes_.assign(response.begin() + static_cast<std::ptrdiff_t>(consumed), response.end());
    setup_complete_ = true;
    return TransportStatus::success();
}

TransportStatus MoqtSession::write_frame(std::uint64_t stream_id,
                                         std::span<const std::uint8_t> frame,
                                         bool fin) {
    return transport_.write_stream(stream_id, frame, fin);
}

}  // namespace openmoq::publisher::transport
