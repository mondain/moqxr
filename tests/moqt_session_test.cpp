#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/transport/moqt_session.h"

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using openmoq::publisher::ByteSpan;
using openmoq::publisher::CmsfObject;
using openmoq::publisher::CmsfObjectKind;
using openmoq::publisher::DraftProfile;
using openmoq::publisher::DraftVersion;
using openmoq::publisher::PublishPlan;
using openmoq::publisher::TrackDescription;
using openmoq::publisher::materialize_publish_plan;
using openmoq::publisher::to_string;
using openmoq::publisher::transport::ConnectionState;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::MoqtSession;
using openmoq::publisher::transport::PublisherTransport;
using openmoq::publisher::transport::StreamDirection;
using openmoq::publisher::transport::TlsConfig;
using openmoq::publisher::transport::TransportStatus;

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
};

std::string to_text(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
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
        .tracks = {TrackDescription{.track_id = 1, .handler_type = "vide", .codec = "avc1", .track_name = "vide_1"}},
        .objects = {
            CmsfObject{
                .kind = CmsfObjectKind::kInitialization,
                .track_name = "init",
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

}  // namespace

int main() {
    bool ok = true;

    MockTransport transport;
    MoqtSession session(transport);

    const EndpointConfig endpoint{
        .host = "example.com",
        .port = 4433,
        .alpn = "moqt",
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
    const PublishPlan materialized = materialize_publish_plan(make_span_backed_plan(), source_bytes);

    status = session.publish(materialized);
    ok &= expect(status.ok, "expected publish to succeed with materialized plan");
    ok &= expect(transport.writes.size() == 7, "expected control frames plus object headers and payloads");
    ok &= expect(to_text(transport.writes[0].bytes) == "SETUP " + to_string(DraftVersion::kDraft14) + "\n",
                 "expected setup frame on control stream");
    ok &= expect(to_text(transport.writes[1].bytes) == "ANNOUNCE init objects=2\n",
                 "expected announce frame on control stream");
    ok &= expect(to_text(transport.writes[2].bytes) == "OBJECT init 0 0\n",
                 "expected init object header");
    ok &= expect(to_text(transport.writes[3].bytes) == "INIT", "expected init payload");
    ok &= expect(to_text(transport.writes[4].bytes) == "OBJECT vide_1 1 0\n",
                 "expected media object header");
    ok &= expect(to_text(transport.writes[5].bytes) == "MSG", "expected media payload");
    ok &= expect(to_text(transport.writes[6].bytes) == "ANNOUNCE_DONE\n",
                 "expected final announce frame");

    MockTransport failing_transport;
    MoqtSession failing_session(failing_transport);
    status = failing_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected second session connect to succeed");

    status = failing_session.publish(make_span_backed_plan());
    ok &= expect(!status.ok, "expected span-backed publish to fail before full transport integration");

    status = session.close(7);
    ok &= expect(status.ok, "expected close to succeed");
    ok &= expect(transport.last_close_code == 7, "expected close code to propagate");

    return ok ? 0 : 1;
}
