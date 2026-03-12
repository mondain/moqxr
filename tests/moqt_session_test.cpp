#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"
#include "openmoq/publisher/transport/moqt_session.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
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
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::MoqtSession;
using openmoq::publisher::transport::PublisherTransport;
using openmoq::publisher::transport::StreamDirection;
using openmoq::publisher::transport::TlsConfig;
using openmoq::publisher::transport::TransportStatus;
using openmoq::publisher::transport::decode_varint;
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

    EndpointConfig endpoint_;
    TlsConfig tls_;
    ConnectionState state_ = ConnectionState::kIdle;
    std::uint64_t next_bidi_ = 0;
    std::uint64_t next_uni_ = 2;
    std::uint64_t last_close_code = 0;
    std::vector<WriteEvent> writes;
    std::map<std::uint64_t, std::vector<std::vector<std::uint8_t>>> reads;
};

bool bytes_equal(const std::vector<std::uint8_t>& bytes, std::initializer_list<std::uint8_t> expected) {
    return std::vector<std::uint8_t>(expected) == bytes;
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
        std::uint64_t parameter_length = 0;
        if (!decode_varint(bytes, offset, parameter_type) || !decode_varint(bytes, offset, parameter_length) ||
            offset + parameter_length > bytes.size()) {
            return false;
        }

        const auto parameter_bytes = std::span<const std::uint8_t>(bytes).subspan(offset, parameter_length);
        if (parameter_type == (expected_draft == DraftVersion::kDraft16 ? 0x05ULL : 0x03ULL)) {
            authority.assign(parameter_bytes.begin(), parameter_bytes.end());
        } else if (parameter_type == 0x01) {
            path.assign(parameter_bytes.begin(), parameter_bytes.end());
        } else if (parameter_type == 0x02) {
            std::size_t parameter_offset = 0;
            if (!decode_varint(parameter_bytes, parameter_offset, max_request_id) ||
                parameter_offset != parameter_bytes.size()) {
                return false;
            }
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
        .tracks = {TrackDescription{.track_id = 0, .handler_type = "meta", .codec = "catalog", .track_name = "catalog"},
                   TrackDescription{.track_id = 1, .handler_type = "vide", .codec = "avc1", .track_name = "vide_1"}},
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

    MockTransport transport;
    transport.reads[0].push_back(encode_server_setup_message({
        .draft = DraftVersion::kDraft14,
        .max_request_id = 8,
    }));
    MoqtSession session(transport);

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

    auto status = session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected session connect to succeed");

    const std::vector<std::uint8_t> source_bytes = {'I', 'N', 'I', 'T', 'M', 'S', 'G'};
    const PublishPlan materialized = materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes);

    status = session.publish(materialized);
    ok &= expect(status.ok, "expected publish to succeed with binary MOQT publish messages");
    ok &= expect(transport.writes.size() == 9,
                 "expected setup, namespace, two track publishes, two object streams, two publish_done, namespace_done");
    ok &= expect(!transport.writes[0].bytes.empty() && transport.writes[0].bytes.front() == 0x20,
                 "expected binary CLIENT_SETUP message type");
    std::string authority;
    std::string path;
    std::uint64_t max_request_id = 1;
    ok &= expect(decode_setup_fields(transport.writes[0].bytes, DraftVersion::kDraft14, authority, path, max_request_id),
                 "expected draft-14 CLIENT_SETUP to decode");
    ok &= expect(authority == "example.com:4433", "expected draft-14 CLIENT_SETUP authority");
    ok &= expect(path == "/", "expected draft-14 CLIENT_SETUP path");
    ok &= expect(max_request_id == 0, "expected draft-14 CLIENT_SETUP max_request_id");
    ok &= expect(message_type(transport.writes[1].bytes) == 0x06, "expected PUBLISH_NAMESPACE");
    ok &= expect(message_type(transport.writes[2].bytes) == 0x1d, "expected first PUBLISH");
    ok &= expect(message_type(transport.writes[3].bytes) == 0x1d, "expected second PUBLISH");
    ok &= expect(transport.writes[4].stream_id == 2, "expected first object stream to be unidirectional stream 2");
    ok &= expect(transport.writes[4].fin, "expected first object stream write to set FIN");
    ok &= expect(transport.writes[5].stream_id == 6, "expected second object stream to be unidirectional stream 6");
    ok &= expect(transport.writes[5].fin, "expected second object stream write to set FIN");
    ok &= expect(message_type(transport.writes[6].bytes) == 0x0b, "expected first PUBLISH_DONE");
    ok &= expect(message_type(transport.writes[7].bytes) == 0x0b, "expected second PUBLISH_DONE");
    ok &= expect(message_type(transport.writes[8].bytes) == 0x09, "expected PUBLISH_NAMESPACE_DONE");

    MockTransport failing_transport;
    MoqtSession failing_session(failing_transport);
    status = failing_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected second session connect to succeed");

    status = failing_session.publish(make_span_backed_plan(DraftVersion::kDraft14));
    ok &= expect(!status.ok, "expected span-backed publish to fail before full transport integration");

    MockTransport draft16_transport;
    draft16_transport.reads[0].push_back(encode_server_setup_message({
        .draft = DraftVersion::kDraft16,
        .max_request_id = 8,
    }));
    MoqtSession draft16_session(draft16_transport);
    status = draft16_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected draft-16 session connect to succeed");

    const PublishPlan draft16_materialized =
        materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft16), source_bytes);
    status = draft16_session.publish(draft16_materialized);
    ok &= expect(status.ok, "expected draft-16 publish to succeed");
    ok &= expect(draft16_transport.writes.size() == 9, "expected draft-16 publish control/object sequence");
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
    ok &= expect(message_type(draft16_transport.writes[2].bytes) == 0x1d, "expected first draft-16 PUBLISH");
    ok &= expect(message_type(draft16_transport.writes[3].bytes) == 0x1d, "expected second draft-16 PUBLISH");
    ok &= expect(message_type(draft16_transport.writes[6].bytes) == 0x0b, "expected first draft-16 PUBLISH_DONE");
    ok &= expect(message_type(draft16_transport.writes[7].bytes) == 0x0b, "expected second draft-16 PUBLISH_DONE");
    ok &= expect(message_type(draft16_transport.writes[8].bytes) == 0x09,
                 "expected draft-16 PUBLISH_NAMESPACE_DONE");

    const std::vector<std::uint8_t> split_server_setup = encode_server_setup_message({
        .draft = DraftVersion::kDraft14,
        .max_request_id = 23,
    });
    MockTransport segmented_transport;
    segmented_transport.reads[0].push_back(
        std::vector<std::uint8_t>(split_server_setup.begin(), split_server_setup.begin() + 3));
    segmented_transport.reads[0].push_back(
        std::vector<std::uint8_t>(split_server_setup.begin() + 3, split_server_setup.end()));
    MoqtSession segmented_session(segmented_transport);
    status = segmented_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected segmented setup connect to succeed");
    status = segmented_session.publish(materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes));
    ok &= expect(status.ok, "expected segmented SERVER_SETUP path to publish successfully");

    ok &= expect(bytes_equal(encode_varint(0), {0x00}), "expected single-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(63), {0x3f}), "expected max one-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(64), {0x40, 0x40}), "expected two-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(16383), {0x7f, 0xff}), "expected max two-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(16384), {0x80, 0x00, 0x40, 0x00}), "expected four-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(1073741824ULL), {0xc0, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00}),
                 "expected eight-byte varint encoding");
    ok &= expect(encode_varint(4611686018427387904ULL).empty(), "expected oversized varint encoding to fail");

    status = session.close(7);
    ok &= expect(status.ok, "expected close to succeed");
    ok &= expect(transport.last_close_code == 7, "expected close code to propagate");

    return ok ? 0 : 1;
}
