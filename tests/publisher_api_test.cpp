#include "openmoq/publisher/publisher_api.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using openmoq::publisher::DraftVersion;
using openmoq::publisher::PreparedPublish;
using openmoq::publisher::PublishPlan;
using openmoq::publisher::Publisher;
using openmoq::publisher::PublisherConfig;
using openmoq::publisher::draft_profile;
using openmoq::publisher::transport::ConnectionState;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::PublisherTransport;
using openmoq::publisher::transport::StreamDirection;
using openmoq::publisher::transport::TlsConfig;
using openmoq::publisher::transport::TransportKind;
using openmoq::publisher::transport::TransportStatus;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

struct MockTransport final : PublisherTransport {
    struct State {
        EndpointConfig configured_endpoint;
        TlsConfig configured_tls;
        bool configure_called = false;
    };

    explicit MockTransport(std::shared_ptr<State> state) : shared_state(std::move(state)) {}

    std::shared_ptr<State> shared_state;
    std::string connect_error = "mock connect failure";

    TransportStatus configure(const EndpointConfig& endpoint, const TlsConfig& tls) override {
        shared_state->configured_endpoint = endpoint;
        shared_state->configured_tls = tls;
        shared_state->configure_called = true;
        return TransportStatus::success();
    }

    TransportStatus connect() override {
        return TransportStatus::failure(connect_error);
    }

    ConnectionState state() const override {
        return ConnectionState::kIdle;
    }

    TransportStatus open_stream(StreamDirection, std::uint64_t&) override {
        return TransportStatus::failure("not implemented");
    }

    TransportStatus accept_stream(StreamDirection, std::uint64_t&, std::chrono::milliseconds) override {
        return TransportStatus::failure("not implemented");
    }

    TransportStatus write_stream(std::uint64_t, std::span<const std::uint8_t>, bool) override {
        return TransportStatus::failure("not implemented");
    }

    TransportStatus read_stream(std::uint64_t, std::vector<std::uint8_t>&, bool&, std::chrono::milliseconds) override {
        return TransportStatus::failure("not implemented");
    }

    TransportStatus reset_stream(std::uint64_t, std::uint64_t) override {
        return TransportStatus::failure("not implemented");
    }

    std::string connection_id() const override {
        return "mock";
    }

    TransportStatus close(std::uint64_t) override {
        return TransportStatus::success();
    }
};

}  // namespace

int main() {
    bool ok = true;

    {
        PublisherConfig config;
        config.draft_version = DraftVersion::kDraft16;
        config.track_namespace = "app";

        const auto state = std::make_shared<MockTransport::State>();
        Publisher publisher(
            config,
            [state](TransportKind kind) -> std::unique_ptr<PublisherTransport> {
                if (kind != TransportKind::kRawQuic) {
                    return nullptr;
                }
                return std::make_unique<MockTransport>(state);
            });

        PreparedPublish prepared;
        prepared.plan = PublishPlan{.draft = draft_profile(DraftVersion::kDraft16)};

        EndpointConfig endpoint;
        endpoint.transport = TransportKind::kRawQuic;
        endpoint.host = "relay.example.com";
        endpoint.port = 443;

        const TransportStatus status = publisher.publish(prepared, endpoint);
        ok &= expect(!status.ok, "expected mock connect failure to propagate");
        ok &= expect(status.message == "transport connect failed: mock connect failure",
                     "expected connect failure message to be wrapped");
        ok &= expect(state->configure_called,
                     "expected transport configure to be invoked");
        ok &= expect(state->configured_endpoint.alpn == "moqt-16",
                     "expected default ALPN for draft-16 raw transport");
        ok &= expect(state->configured_endpoint.application_protocol == "moqt-16",
                     "expected application protocol to follow draft default for raw transport");
    }

    {
        PublisherConfig config;
        config.draft_version = DraftVersion::kDraft14;

        const auto state = std::make_shared<MockTransport::State>();
        Publisher publisher(
            config,
            [state](TransportKind kind) -> std::unique_ptr<PublisherTransport> {
                if (kind != TransportKind::kWebTransport) {
                    return nullptr;
                }
                return std::make_unique<MockTransport>(state);
            });

        PreparedPublish prepared;
        prepared.plan = PublishPlan{.draft = draft_profile(DraftVersion::kDraft14)};

        EndpointConfig endpoint;
        endpoint.transport = TransportKind::kWebTransport;
        endpoint.host = "relay.example.com";
        endpoint.port = 443;
        endpoint.path = "/moq";
        endpoint.path_explicit = true;

        const TransportStatus status = publisher.publish(prepared, endpoint);
        ok &= expect(!status.ok, "expected mock connect failure to propagate for webtransport");
        ok &= expect(state->configured_endpoint.alpn == "h3",
                     "expected default ALPN h3 for webtransport");
        ok &= expect(state->configured_endpoint.application_protocol.empty(),
                     "expected draft-14 webtransport to offer empty application protocol");
    }

    return ok ? 0 : 1;
}
