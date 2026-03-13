#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"
#include "openmoq/publisher/transport/moqt_session.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

using openmoq::publisher::ByteSpan;
using openmoq::publisher::CmsfObject;
using openmoq::publisher::CmsfObjectKind;
using openmoq::publisher::DraftVersion;
using openmoq::publisher::PublishPlan;
using openmoq::publisher::TrackDescription;
using openmoq::publisher::materialize_publish_plan;
using openmoq::publisher::transport::ConnectionState;
using openmoq::publisher::transport::ServerSetupMessage;
using openmoq::publisher::transport::SubscribeMessage;
using openmoq::publisher::transport::SubscribeNamespaceMessage;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::MoqtSession;
using openmoq::publisher::transport::PublisherTransport;
using openmoq::publisher::transport::StreamDirection;
using openmoq::publisher::transport::TlsConfig;
using openmoq::publisher::transport::TransportStatus;
using openmoq::publisher::transport::decode_varint;
using openmoq::publisher::transport::encode_setup_message;
using openmoq::publisher::transport::encode_server_setup_message;
using openmoq::publisher::transport::encode_varint;

struct MockTransport final : PublisherTransport {
    struct WriteEvent {
        std::uint64_t stream_id = 0;
        std::vector<std::uint8_t> bytes;
        bool fin = false;
    };

    TransportStatus configure(const EndpointConfig& endpoint, const TlsConfig& tls) override {
        endpoint_ = endpoint;
        tls_ = tls;
        return TransportStatus::success();
    }

    TransportStatus connect() override {
        state_ = ConnectionState::kConnected;
        return TransportStatus::success();
    }

    ConnectionState state() const override {
        return state_;
    }

    TransportStatus open_stream(StreamDirection direction, std::uint64_t& stream_id) override {
        if (state_ != ConnectionState::kConnected) {
            return TransportStatus::failure("not connected");
        }

        if (direction == StreamDirection::kBidirectional) {
            stream_id = next_bidi_;
            next_bidi_ += 4;
        } else {
            stream_id = next_uni_;
            next_uni_ += 4;
        }
        return TransportStatus::success();
    }

    TransportStatus write_stream(std::uint64_t stream_id,
                                 std::span<const std::uint8_t> bytes,
                                 bool fin) override {
        writes.push_back({
            .stream_id = stream_id,
            .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
            .fin = fin,
        });
        return TransportStatus::success();
    }

    TransportStatus read_stream(std::uint64_t stream_id,
                                std::vector<std::uint8_t>& bytes,
                                bool& fin,
                                std::chrono::milliseconds timeout) override {
        static_cast<void>(timeout);
        const auto it = reads.find(stream_id);
        if (it == reads.end()) {
            return TransportStatus::failure("no queued read for stream");
        }

        if (it->second.empty()) {
            reads.erase(it);
            return TransportStatus::failure("queued stream had no chunks");
        }

        bytes = it->second.front();
        it->second.erase(it->second.begin());
        fin = it->second.empty();
        if (fin) {
            reads.erase(it);
        }
        return TransportStatus::success();
    }

    TransportStatus close(std::uint64_t application_error_code) override {
        last_close_code = application_error_code;
        state_ = ConnectionState::kClosed;
        return TransportStatus::success();
    }

    std::string connection_id() const override {
        return "mock-connection-id";
    }

    EndpointConfig endpoint_;
    TlsConfig tls_;
    ConnectionState state_ = ConnectionState::kIdle;
    std::uint64_t next_bidi_ = 0;
    std::uint64_t next_uni_ = 2;
    std::uint64_t last_close_code = 0;
    std::vector<WriteEvent> writes;
    std::map<std::uint64_t, std::vector<std::vector<std::uint8_t>>> reads;
};

void append_be16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

std::vector<std::uint8_t> encode_publish_namespace_ok_message(DraftVersion draft, std::uint64_t request_id) {
    std::vector<std::uint8_t> payload = encode_varint(request_id);
    if (draft == DraftVersion::kDraft16) {
        const std::vector<std::uint8_t> parameter_count = encode_varint(0);
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
    }
    std::vector<std::uint8_t> message = encode_varint(0x07);
    append_be16(message, static_cast<std::uint16_t>(payload.size()));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_subscribe_namespace_message(std::uint64_t request_id, std::string_view track_namespace) {
    std::vector<std::uint8_t> payload = encode_varint(request_id);
    const std::vector<std::uint8_t> tuple_len = encode_varint(1);
    const std::vector<std::uint8_t> component_len = encode_varint(track_namespace.size());
    payload.insert(payload.end(), tuple_len.begin(), tuple_len.end());
    payload.insert(payload.end(), component_len.begin(), component_len.end());
    payload.insert(payload.end(), track_namespace.begin(), track_namespace.end());
    const std::vector<std::uint8_t> parameter_count = encode_varint(0);
    payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());

    std::vector<std::uint8_t> message = encode_varint(0x11);
    const std::vector<std::uint8_t> length = encode_varint(payload.size());
    message.insert(message.end(), length.begin(), length.end());
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_subscribe_message(std::uint64_t request_id,
                                                   std::string_view track_namespace,
                                                   std::string_view track_name,
                                                   std::uint8_t forward) {
    std::vector<std::uint8_t> payload = encode_varint(request_id);
    const std::vector<std::uint8_t> tuple_len = encode_varint(1);
    const std::vector<std::uint8_t> component_len = encode_varint(track_namespace.size());
    payload.insert(payload.end(), tuple_len.begin(), tuple_len.end());
    payload.insert(payload.end(), component_len.begin(), component_len.end());
    payload.insert(payload.end(), track_namespace.begin(), track_namespace.end());
    const std::vector<std::uint8_t> track_name_length = encode_varint(track_name.size());
    payload.insert(payload.end(), track_name_length.begin(), track_name_length.end());
    payload.insert(payload.end(), track_name.begin(), track_name.end());
    payload.push_back(0x80);
    payload.push_back(0x01);
    payload.push_back(forward);
    const std::vector<std::uint8_t> filter_type = encode_varint(0x03);
    const std::vector<std::uint8_t> start_group = encode_varint(0);
    const std::vector<std::uint8_t> start_object = encode_varint(0);
    const std::vector<std::uint8_t> parameter_count = encode_varint(0);
    payload.insert(payload.end(), filter_type.begin(), filter_type.end());
    payload.insert(payload.end(), start_group.begin(), start_group.end());
    payload.insert(payload.end(), start_object.begin(), start_object.end());
    payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());

    std::vector<std::uint8_t> message = encode_varint(0x03);
    append_be16(message, static_cast<std::uint16_t>(payload.size()));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_publish_ok_message(DraftVersion draft, std::uint64_t request_id) {
    std::vector<std::uint8_t> payload = encode_varint(request_id);
    if (draft == DraftVersion::kDraft14) {
        payload.push_back(0x01);
        payload.push_back(0x80);
        payload.push_back(0x01);
        const std::vector<std::uint8_t> filter_type = encode_varint(0);
        const std::vector<std::uint8_t> parameter_count = encode_varint(0);
        payload.insert(payload.end(), filter_type.begin(), filter_type.end());
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
    } else {
        const std::vector<std::uint8_t> parameter_count = encode_varint(0);
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
    }

    std::vector<std::uint8_t> message = encode_varint(0x1e);
    append_be16(message, static_cast<std::uint16_t>(payload.size()));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

void queue_subscribe_requests(MockTransport& transport,
                              DraftVersion draft,
                              std::string_view track_namespace,
                              std::initializer_list<std::pair<std::uint64_t, std::string>> requests,
                              bool include_subscribe_namespace = false,
                              std::uint8_t forward = 0) {
    transport.reads[0].push_back(encode_publish_namespace_ok_message(draft, 0));
    if (include_subscribe_namespace) {
        transport.reads[0].push_back(encode_subscribe_namespace_message(1, track_namespace));
    }
    for (const auto& [request_id, track_name] : requests) {
        transport.reads[0].push_back(encode_subscribe_message(request_id, track_namespace, track_name, forward));
    }
}

void queue_publish_ok_responses(MockTransport& transport,
                                DraftVersion draft,
                                std::initializer_list<std::uint64_t> request_ids) {
    transport.reads[0].push_back(encode_publish_namespace_ok_message(draft, 0));
    for (const auto request_id : request_ids) {
        transport.reads[0].push_back(encode_publish_ok_message(draft, request_id));
    }
}

bool bytes_equal(const std::vector<std::uint8_t>& bytes, std::initializer_list<std::uint8_t> expected) {
    return std::vector<std::uint8_t>(expected) == bytes;
}

std::string hex_dump(const std::vector<std::uint8_t>& bytes) {
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

std::optional<std::uint64_t> message_type(const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 0;
    std::uint64_t type = 0;
    if (!decode_varint(bytes, offset, type)) {
        return std::nullopt;
    }
    return type;
}

bool decode_setup_fields(const std::vector<std::uint8_t>& bytes,
                         DraftVersion expected_draft,
                         std::string& authority,
                         std::string& path,
                         std::uint64_t& max_request_id) {
    authority.clear();
    path.clear();
    max_request_id = 0;

    std::size_t offset = 0;
    std::uint64_t message_type = 0;
    if (!decode_varint(bytes, offset, message_type) || message_type != 0x20) {
        return false;
    }
    if (offset + 2 > bytes.size()) {
        return false;
    }

    const std::size_t payload_length =
        (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
    offset += 2;
    if (offset + payload_length != bytes.size()) {
        return false;
    }

    if (expected_draft == DraftVersion::kDraft14) {
        std::uint64_t version_count = 0;
        std::uint64_t version = 0;
        if (!decode_varint(bytes, offset, version_count) || version_count != 1 ||
            !decode_varint(bytes, offset, version) || version != 0xff00000eULL) {
            return false;
        }
    }

    std::uint64_t parameter_count = 0;
    if (!decode_varint(bytes, offset, parameter_count) || parameter_count != 3) {
        return false;
    }

    for (std::uint64_t index = 0; index < parameter_count; ++index) {
        std::uint64_t parameter_type = 0;
        if (!decode_varint(bytes, offset, parameter_type)) {
            return false;
        }

        if ((parameter_type & 0x1ULL) == 0) {
            if (parameter_type != 0x02) {
                return false;
            }
            std::size_t parameter_offset = offset;
            if (!decode_varint(bytes, parameter_offset, max_request_id)) {
                return false;
            }
            offset = parameter_offset;
            continue;
        }

        std::uint64_t parameter_length = 0;
        if (!decode_varint(bytes, offset, parameter_length) || offset + parameter_length > bytes.size()) {
            return false;
        }

        const auto parameter_bytes = std::span<const std::uint8_t>(bytes).subspan(offset, parameter_length);
        if (parameter_type == 0x05ULL) {
            authority.assign(parameter_bytes.begin(), parameter_bytes.end());
        } else if (parameter_type == 0x01) {
            path.assign(parameter_bytes.begin(), parameter_bytes.end());
        }
        offset += parameter_length;
    }

    return offset == bytes.size();
}

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

PublishPlan make_span_backed_plan() {
    return {
        .draft = openmoq::publisher::draft_profile(DraftVersion::kDraft14),
        .tracks = {TrackDescription{.track_id = 0, .handler_type = "meta", .codec = "catalog", .sample_entry_type = "catalog", .track_name = "catalog"},
                   TrackDescription{.track_id = 1, .handler_type = "vide", .codec = "avc1.64000C", .sample_entry_type = "avc1", .track_name = "vide_1"}},
        .objects = {
            CmsfObject{
                .kind = CmsfObjectKind::kInitialization,
                .track_name = "catalog",
                .group_id = 0,
                .object_id = 0,
                .payload = ByteSpan{.offset = 0, .size = 4},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "vide_1",
                .group_id = 1,
                .object_id = 0,
                .payload = ByteSpan{.offset = 4, .size = 3},
            },
        },
    };
}

PublishPlan make_span_backed_plan(DraftVersion draft) {
    PublishPlan plan = make_span_backed_plan();
    plan.draft = openmoq::publisher::draft_profile(draft);
    return plan;
}

}  // namespace

int main() {
    bool ok = true;
    constexpr std::string_view kTestTrackNamespace = "interop";
    const EndpointConfig endpoint{
        .host = "example.com",
        .port = 4433,
        .alpn = "moq-00",
    };
    const TlsConfig tls{
        .certificate_path = {},
        .private_key_path = {},
        .ca_path = {},
        .insecure_skip_verify = true,
    };
    const std::vector<std::uint8_t> source_bytes = {'I', 'N', 'I', 'T', 'M', 'S', 'G'};
    auto status = TransportStatus::success();
    std::string authority;
    std::string path;
    std::uint64_t max_request_id = 1;

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        queue_subscribe_requests(transport, DraftVersion::kDraft14, kTestTrackNamespace, {{2, "catalog"}, {4, "vide_1"}});
        MoqtSession session(transport, std::string(kTestTrackNamespace), false);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected session connect to succeed");

        const PublishPlan materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes);

        status = session.publish(materialized);
        ok &= expect(status.ok, "expected publish to succeed with relay subscribe flow");
        ok &= expect(transport.writes.size() == 9,
                     "expected setup, namespace, two subscribe_ok, two object streams, two publish_done, namespace_done");
        ok &= expect(!transport.writes[0].bytes.empty() && transport.writes[0].bytes.front() == 0x20,
                     "expected binary CLIENT_SETUP message type");
        authority.clear();
        path.clear();
        max_request_id = 1;
        ok &= expect(decode_setup_fields(transport.writes[0].bytes, DraftVersion::kDraft14, authority, path, max_request_id),
                     "expected draft-14 CLIENT_SETUP to decode");
        ok &= expect(authority == "example.com:4433", "expected draft-14 CLIENT_SETUP authority");
        ok &= expect(path == "/", "expected draft-14 CLIENT_SETUP path");
        ok &= expect(max_request_id == 0, "expected draft-14 CLIENT_SETUP max_request_id");
        ok &= expect(message_type(transport.writes[1].bytes) == 0x06, "expected PUBLISH_NAMESPACE");
        ok &= expect(transport.writes[1].bytes == std::vector<std::uint8_t>({0x06, 0x00, 0x0b, 0x00, 0x01, 0x07, 0x69, 0x6e,
                                                                             0x74, 0x65, 0x72, 0x6f, 0x70, 0x00}),
                     "expected namespace write to use the configured track namespace");
        ok &= expect(message_type(transport.writes[2].bytes) == 0x04, "expected first SUBSCRIBE_OK");
        ok &= expect(transport.writes[3].stream_id == 2, "expected first object stream to be unidirectional stream 2");
        ok &= expect(!transport.writes[3].bytes.empty() && transport.writes[3].bytes.front() == 0x14,
                     "expected first object stream to use a subgroup header with explicit subgroup ID");
        ok &= expect(transport.writes[3].fin, "expected first object stream write to set FIN");
        ok &= expect(message_type(transport.writes[4].bytes) == 0x0b, "expected first PUBLISH_DONE");
        ok &= expect(message_type(transport.writes[5].bytes) == 0x04, "expected second SUBSCRIBE_OK");
        ok &= expect(transport.writes[6].stream_id == 6, "expected second object stream to be unidirectional stream 6");
        ok &= expect(!transport.writes[6].bytes.empty() && transport.writes[6].bytes.front() == 0x14,
                     "expected second object stream to use a subgroup header with explicit subgroup ID");
        ok &= expect(transport.writes[6].fin, "expected second object stream write to set FIN");
        ok &= expect(message_type(transport.writes[7].bytes) == 0x0b, "expected second PUBLISH_DONE");
        ok &= expect(message_type(transport.writes[8].bytes) == 0x09, "expected PUBLISH_NAMESPACE_DONE");
        ok &= expect(transport.writes[8].bytes == std::vector<std::uint8_t>({0x09, 0x00, 0x09, 0x01, 0x07, 0x69, 0x6e,
                                                                             0x74, 0x65, 0x72, 0x6f, 0x70}),
                     "expected draft-14 PUBLISH_NAMESPACE_DONE to contain the configured track namespace");
    }

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        queue_publish_ok_responses(transport, DraftVersion::kDraft14, {2, 4});
        MoqtSession session(transport, std::string(kTestTrackNamespace), true);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected auto-forward session connect to succeed");

        const PublishPlan materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes);
        status = session.publish(materialized);
        ok &= expect(status.ok, "expected publish to succeed with auto-forward flow");
        ok &= expect(transport.writes.size() == 9,
                     "expected setup, namespace, two publish requests, two object streams, two publish_done, namespace_done");
        if (transport.writes.size() >= 9) {
            ok &= expect(message_type(transport.writes[1].bytes) == 0x06,
                         "expected PUBLISH_NAMESPACE before auto-forward track publish");
            ok &= expect(message_type(transport.writes[2].bytes) == 0x1d, "expected first PUBLISH");
            ok &= expect(message_type(transport.writes[3].bytes) == 0x1d, "expected second PUBLISH");
            ok &= expect(transport.writes[4].stream_id == 2, "expected first auto-forward object stream on stream 2");
            ok &= expect(transport.writes[5].stream_id == 6, "expected second auto-forward object stream on stream 6");
            ok &= expect(message_type(transport.writes[6].bytes) == 0x0b, "expected first auto-forward PUBLISH_DONE");
            ok &= expect(message_type(transport.writes[7].bytes) == 0x0b, "expected second auto-forward PUBLISH_DONE");
            ok &= expect(message_type(transport.writes[8].bytes) == 0x09,
                         "expected auto-forward PUBLISH_NAMESPACE_DONE");
        }
    }

    MockTransport failing_transport;
    MoqtSession failing_session(failing_transport, std::string(kTestTrackNamespace));
    status = failing_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected second session connect to succeed");

    status = failing_session.publish(make_span_backed_plan(DraftVersion::kDraft14));
    ok &= expect(!status.ok, "expected span-backed publish to fail before full transport integration");

    MockTransport draft16_transport;
    draft16_transport.reads[0].push_back(encode_server_setup_message({
        .draft = DraftVersion::kDraft16,
        .max_request_id = 8,
    }));
    queue_subscribe_requests(draft16_transport, DraftVersion::kDraft16, kTestTrackNamespace, {{2, "catalog"}, {4, "vide_1"}});
    MoqtSession draft16_session(draft16_transport, std::string(kTestTrackNamespace));
    status = draft16_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected draft-16 session connect to succeed");

    const PublishPlan draft16_materialized =
        materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft16), source_bytes);
    status = draft16_session.publish(draft16_materialized);
    ok &= expect(status.ok, "expected draft-16 publish to succeed");
    ok &= expect(draft16_transport.writes.size() == 9, "expected draft-16 relay subscribe control/object sequence");
    ok &= expect(!draft16_transport.writes[0].bytes.empty() && draft16_transport.writes[0].bytes.front() == 0x20,
                 "expected draft-16 binary CLIENT_SETUP message type");
    authority.clear();
    path.clear();
    max_request_id = 1;
    ok &= expect(
        decode_setup_fields(draft16_transport.writes[0].bytes, DraftVersion::kDraft16, authority, path, max_request_id),
        "expected draft-16 CLIENT_SETUP to decode");
    ok &= expect(authority == "example.com:4433", "expected draft-16 CLIENT_SETUP authority");
    ok &= expect(path == "/", "expected draft-16 CLIENT_SETUP path");
    ok &= expect(max_request_id == 0, "expected draft-16 CLIENT_SETUP max_request_id");
    ok &= expect(message_type(draft16_transport.writes[1].bytes) == 0x06, "expected draft-16 PUBLISH_NAMESPACE");
    ok &= expect(draft16_transport.writes[1].bytes == std::vector<std::uint8_t>({0x06, 0x00, 0x0b, 0x00, 0x01, 0x07,
                                                                                  0x69, 0x6e, 0x74, 0x65, 0x72, 0x6f,
                                                                                  0x70, 0x00}),
                 "expected draft-16 namespace write to use the configured track namespace");
    ok &= expect(message_type(draft16_transport.writes[2].bytes) == 0x04, "expected first draft-16 SUBSCRIBE_OK");
    ok &= expect(draft16_transport.writes[3].stream_id == 2, "expected first draft-16 object stream");
    ok &= expect(message_type(draft16_transport.writes[4].bytes) == 0x0b, "expected first draft-16 PUBLISH_DONE");
    ok &= expect(message_type(draft16_transport.writes[5].bytes) == 0x04, "expected second draft-16 SUBSCRIBE_OK");
    ok &= expect(draft16_transport.writes[6].stream_id == 6, "expected second draft-16 object stream");
    ok &= expect(message_type(draft16_transport.writes[7].bytes) == 0x0b, "expected second draft-16 PUBLISH_DONE");
    ok &= expect(message_type(draft16_transport.writes[8].bytes) == 0x09,
                 "expected draft-16 PUBLISH_NAMESPACE_DONE");
    ok &= expect(draft16_transport.writes[8].bytes == std::vector<std::uint8_t>({0x09, 0x00, 0x01, 0x00}),
                 "expected draft-16 PUBLISH_NAMESPACE_DONE to contain only the request ID");

    const std::vector<std::uint8_t> split_server_setup = encode_server_setup_message({
        .draft = DraftVersion::kDraft14,
        .max_request_id = 23,
    });
    MockTransport segmented_transport;
    segmented_transport.reads[0].push_back(
        std::vector<std::uint8_t>(split_server_setup.begin(), split_server_setup.begin() + 3));
    segmented_transport.reads[0].push_back(
        std::vector<std::uint8_t>(split_server_setup.begin() + 3, split_server_setup.end()));
    queue_subscribe_requests(segmented_transport, DraftVersion::kDraft14, kTestTrackNamespace, {{2, "catalog"}, {4, "vide_1"}});
    MoqtSession segmented_session(segmented_transport, std::string(kTestTrackNamespace));
    status = segmented_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected segmented setup connect to succeed");
    status = segmented_session.publish(materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes));
    ok &= expect(status.ok, "expected segmented SERVER_SETUP path to publish successfully");

    MockTransport extra_server_setup_param_transport;
    extra_server_setup_param_transport.reads[0].push_back(
        std::vector<std::uint8_t>({0x21, 0x00, 0x0f, 0xc0, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x0e, 0x02,
                                   0x02, 0x40, 0x64, 0x04, 0x44, 0x00}));
    queue_subscribe_requests(extra_server_setup_param_transport,
                             DraftVersion::kDraft14,
                             kTestTrackNamespace,
                             {{2, "catalog"}, {4, "vide_1"}});
    MoqtSession extra_server_setup_param_session(extra_server_setup_param_transport, std::string(kTestTrackNamespace));
    status = extra_server_setup_param_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected SERVER_SETUP with extra even-numbered parameter to connect successfully");
    status = extra_server_setup_param_session.publish(
        materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes));
    ok &= expect(status.ok, "expected publish to succeed after SERVER_SETUP with extra even-numbered parameter");

    ok &= expect(bytes_equal(encode_varint(0), {0x00}), "expected single-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(63), {0x3f}), "expected max one-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(64), {0x40, 0x40}), "expected two-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(16383), {0x7f, 0xff}), "expected max two-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(16384), {0x80, 0x00, 0x40, 0x00}), "expected four-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(1073741824ULL), {0xc0, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00}),
                 "expected eight-byte varint encoding");
    ok &= expect(encode_varint(4611686018427387904ULL).empty(), "expected oversized varint encoding to fail");

    const auto draft14_setup = encode_setup_message({
        .draft = DraftVersion::kDraft14,
        .authority = "example.com:4433",
        .path = "/moq",
        .max_request_id = 0,
    });
    const auto draft16_setup = encode_setup_message({
        .draft = DraftVersion::kDraft16,
        .authority = "example.com:4433",
        .path = "/moq",
        .max_request_id = 0,
    });
    if (!bytes_equal(draft14_setup,
                     {0x20, 0x00, 0x24, 0x01, 0xc0, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x0e, 0x03, 0x05, 0x10,
                      0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d, 0x3a, 0x34, 0x34, 0x33,
                      0x33, 0x01, 0x04, 0x2f, 0x6d, 0x6f, 0x71, 0x02, 0x00})) {
        std::cerr << "draft14 actual: " << hex_dump(draft14_setup) << '\n';
        ok = false;
    }
    if (!bytes_equal(draft16_setup,
                     {0x20, 0x00, 0x1b, 0x03, 0x05, 0x10, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63,
                      0x6f, 0x6d, 0x3a, 0x34, 0x34, 0x33, 0x33, 0x01, 0x04, 0x2f, 0x6d, 0x6f, 0x71, 0x02, 0x00})) {
        std::cerr << "draft16 actual: " << hex_dump(draft16_setup) << '\n';
        ok = false;
    }

    MockTransport close_transport;
    MoqtSession close_session(close_transport, std::string(kTestTrackNamespace));
    status = close_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected close session connect to succeed");
    status = close_session.close(7);
    ok &= expect(status.ok, "expected close to succeed");
    ok &= expect(close_transport.last_close_code == 7, "expected close code to propagate");

    return ok ? 0 : 1;
}
