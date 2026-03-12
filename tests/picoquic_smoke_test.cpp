#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/transport/moqt_session.h"
#include "openmoq/publisher/transport/picoquic_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <picoquic.h>
#include <picoquic_packet_loop.h>

namespace {

using openmoq::publisher::ByteSpan;
using openmoq::publisher::CmsfObject;
using openmoq::publisher::CmsfObjectKind;
using openmoq::publisher::DraftVersion;
using openmoq::publisher::PublishPlan;
using openmoq::publisher::TrackDescription;
using openmoq::publisher::draft_profile;
using openmoq::publisher::materialize_publish_plan;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::MoqtSession;
using openmoq::publisher::transport::PicoquicClient;
using openmoq::publisher::transport::TlsConfig;

constexpr const char* kPicoquicSourceDir = "/media/mondain/terrorbyte/workspace/github/picoquic";

struct SmokeServer {
    picoquic_quic_t* quic = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    uint16_t port = 0;
    std::size_t bytes_received = 0;
    bool loop_ready = false;
    bool stop_requested = false;
    bool loop_exited = false;
    int loop_return_code = 0;
};

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

PublishPlan make_span_backed_plan() {
    return {
        .draft = draft_profile(DraftVersion::kDraft14),
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

int smoke_server_callback(picoquic_cnx_t* cnx,
                          uint64_t stream_id,
                          uint8_t* bytes,
                          size_t length,
                          picoquic_call_back_event_t event,
                          void* callback_ctx,
                          void* stream_ctx) {
    static_cast<void>(cnx);
    static_cast<void>(stream_id);
    static_cast<void>(stream_ctx);

    auto* server = static_cast<SmokeServer*>(callback_ctx);
    if (server == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    switch (event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin: {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->bytes_received += length;
            server->condition.notify_all();
            return 0;
        }
        case picoquic_callback_close:
        case picoquic_callback_application_close:
        case picoquic_callback_stateless_reset:
            return 0;
        default:
            static_cast<void>(bytes);
            return 0;
    }
}

int smoke_server_loop_callback(picoquic_quic_t* quic,
                               picoquic_packet_loop_cb_enum cb_mode,
                               void* callback_ctx,
                               void* callback_arg) {
    static_cast<void>(quic);

    auto* server = static_cast<SmokeServer*>(callback_ctx);
    if (server == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    switch (cb_mode) {
        case picoquic_packet_loop_ready: {
            auto* options = static_cast<picoquic_packet_loop_options_t*>(callback_arg);
            if (options != nullptr) {
                options->do_time_check = 1;
            }
            std::lock_guard<std::mutex> lock(server->mutex);
            server->loop_ready = true;
            server->condition.notify_all();
            return 0;
        }
        case picoquic_packet_loop_after_receive:
        case picoquic_packet_loop_after_send:
            return 0;
        case picoquic_packet_loop_port_update:
            if (callback_arg != nullptr) {
                auto* addr = static_cast<sockaddr*>(callback_arg);
                std::lock_guard<std::mutex> lock(server->mutex);
                if (addr->sa_family == AF_INET) {
                    server->port = ntohs(reinterpret_cast<sockaddr_in*>(addr)->sin_port);
                } else if (addr->sa_family == AF_INET6) {
                    server->port = ntohs(reinterpret_cast<sockaddr_in6*>(addr)->sin6_port);
                }
                server->condition.notify_all();
            }
            return 0;
        case picoquic_packet_loop_time_check: {
            auto* time_check = static_cast<packet_loop_time_check_arg_t*>(callback_arg);
            std::lock_guard<std::mutex> lock(server->mutex);
            if (time_check != nullptr && time_check->delta_t > 10000) {
                time_check->delta_t = 10000;
            }
            return server->stop_requested ? PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP : 0;
        }
        default:
            return 0;
    }
}

bool start_server(SmokeServer& server) {
    const std::string cert_path = std::string(kPicoquicSourceDir) + "/certs/cert.pem";
    const std::string key_path = std::string(kPicoquicSourceDir) + "/certs/key.pem";

    server.quic = picoquic_create(8, cert_path.c_str(), key_path.c_str(), nullptr, "moqt",
                                  smoke_server_callback, &server, nullptr, nullptr, nullptr,
                                  picoquic_current_time(), nullptr, nullptr, nullptr, 0);
    if (server.quic == nullptr) {
        return false;
    }

    picoquic_set_cookie_mode(server.quic, 2);
    server.thread = std::thread([&server] {
        const int ret =
            picoquic_packet_loop(server.quic, server.port, AF_INET, 0, 0, 0, smoke_server_loop_callback, &server);
        std::lock_guard<std::mutex> lock(server.mutex);
        server.loop_return_code = ret;
        server.loop_exited = true;
        server.condition.notify_all();
    });

    std::unique_lock<std::mutex> lock(server.mutex);
    const bool started = server.condition.wait_for(lock, std::chrono::seconds(5), [&] {
        return (server.loop_ready && server.port != 0) || server.loop_exited;
    });
    if (!started) {
        std::cerr << "server loop did not report ready within timeout\n";
    } else if (server.loop_exited) {
        std::cerr << "server loop exited early, return_code=" << server.loop_return_code << '\n';
    }
    return started && server.loop_ready && server.port != 0 && !server.loop_exited;
}

void stop_server(SmokeServer& server) {
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        server.stop_requested = true;
    }

    if (server.thread.joinable()) {
        server.thread.join();
    }

    if (server.quic != nullptr) {
        picoquic_free(server.quic);
        server.quic = nullptr;
    }
}

}  // namespace

int main() {
    bool ok = true;

    SmokeServer server;
    ok &= expect(start_server(server), "expected picoquic smoke server to start");
    if (!ok) {
        stop_server(server);
        return 1;
    }

    const EndpointConfig endpoint{
        .host = "127.0.0.1",
        .port = server.port,
        .alpn = "moqt",
    };
    const TlsConfig tls{
        .certificate_path = {},
        .private_key_path = {},
        .ca_path = {},
        .insecure_skip_verify = true,
    };

    PicoquicClient transport;
    MoqtSession session(transport);

    auto status = session.connect(endpoint, tls);
    if (!status.ok) {
        std::cerr << "connect error: " << status.message << '\n';
    }
    ok &= expect(status.ok,
                 status.ok ? "expected picoquic client handshake to succeed"
                           : "expected picoquic client handshake to succeed: " + status.message);

    const std::vector<std::uint8_t> source_bytes = {'I', 'N', 'I', 'T', 'M', 'S', 'G'};
    const PublishPlan materialized = materialize_publish_plan(make_span_backed_plan(), source_bytes);

    status = session.publish(materialized);
    if (!status.ok) {
        std::cerr << "publish error: " << status.message << '\n';
    }
    ok &= expect(status.ok,
                 status.ok ? "expected session publish over picoquic to succeed"
                           : "expected session publish over picoquic to succeed: " + status.message);

    if (status.ok) {
        std::unique_lock<std::mutex> lock(server.mutex);
        ok &= expect(server.condition.wait_for(lock, std::chrono::seconds(5), [&] {
                         return server.bytes_received >= 64;
                     }),
                     "expected server to receive published bytes");
    }

    status = session.close(0);
    ok &= expect(status.ok, "expected picoquic session close to succeed");

    stop_server(server);
    return ok ? 0 : 1;
}
