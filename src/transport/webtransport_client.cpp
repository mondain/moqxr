#include "openmoq/publisher/transport/webtransport_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#ifdef OPENMOQ_HAS_PICOQUIC
#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_utils.h>
#include <picosocks.h>
#include <h3zero.h>
#include <h3zero_common.h>
#include <pico_webtransport.h>
#endif

namespace openmoq::publisher::transport {

struct WebTransportClient::Impl {
    struct PendingWrite {
        std::uint64_t stream_id = 0;
        std::vector<std::uint8_t> bytes;
        bool fin = false;
    };

    struct PendingOpen {
        StreamDirection direction = StreamDirection::kBidirectional;
        std::uint64_t assigned_stream_id = 0;
        bool completed = false;
        bool failed = false;
        std::string error;
    };

    struct ReceivedStreamData {
        std::vector<std::uint8_t> bytes;
        bool fin = false;
    };

    std::mutex mutex;
    std::condition_variable condition;
    std::deque<PendingOpen*> pending_opens;
    std::deque<PendingWrite> pending_writes;
    std::map<std::uint64_t, ReceivedStreamData> received_streams;
    std::set<std::uint64_t> application_streams;
    bool connected = false;
    bool failed = false;
    bool disconnected = false;
    bool close_requested = false;
    bool loop_ready = false;
    bool loop_exited = false;
    std::string last_error;
    std::thread packet_loop_thread;

#ifdef OPENMOQ_HAS_PICOQUIC
    picoquic_quic_t* quic = nullptr;
    picoquic_cnx_t* cnx = nullptr;
    sockaddr_storage server_address{};
    h3zero_callback_ctx_t* h3_ctx = nullptr;
    h3zero_stream_ctx_t* control_stream_ctx = nullptr;
    int packet_loop_return_code = 0;
#endif
};

#ifdef OPENMOQ_HAS_PICOQUIC
namespace {

std::mutex g_connection_impl_mutex;
std::map<picoquic_cnx_t*, WebTransportClient::Impl*> g_connection_impls;

void register_connection_impl(picoquic_cnx_t* cnx, WebTransportClient::Impl* impl) {
    if (cnx == nullptr || impl == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_connection_impl_mutex);
    g_connection_impls[cnx] = impl;
}

void unregister_connection_impl(picoquic_cnx_t* cnx) {
    if (cnx == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_connection_impl_mutex);
    g_connection_impls.erase(cnx);
}

bool control_stream_connect_established(const WebTransportClient::Impl& impl) {
    const auto* stream_ctx = impl.control_stream_ctx;
    if (stream_ctx == nullptr) {
        return false;
    }

    const int status = stream_ctx->ps.stream_state.header.status;
    return stream_ctx->is_upgraded && status >= 200 && status < 300;
}

int webtransport_callback(picoquic_cnx_t* cnx,
                          uint8_t* bytes,
                          size_t length,
                          picohttp_call_back_event_t event,
                          h3zero_stream_ctx_t* stream_ctx,
                          void* path_app_ctx);

h3zero_stream_ctx_t* find_local_stream_context(WebTransportClient::Impl& impl,
                                               std::uint64_t stream_id) {
    if (impl.cnx == nullptr || impl.h3_ctx == nullptr || impl.control_stream_ctx == nullptr) {
        return nullptr;
    }

    return h3zero_find_stream(impl.h3_ctx, stream_id);
}

int apply_pending_writes(WebTransportClient::Impl& impl) {
    if (impl.cnx == nullptr) {
        return 0;
    }

    std::deque<WebTransportClient::Impl::PendingOpen*> opens;
    std::deque<WebTransportClient::Impl::PendingWrite> writes;
    bool close_requested = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        opens.swap(impl.pending_opens);
        writes.swap(impl.pending_writes);
        close_requested = impl.close_requested;
    }

    for (auto* open : opens) {
        if (open == nullptr) {
            continue;
        }
        h3zero_stream_ctx_t* stream_ctx =
            picowt_create_local_stream(impl.cnx,
                                       open->direction == StreamDirection::kBidirectional ? 1 : 0,
                                       impl.h3_ctx,
                                       impl.control_stream_ctx->stream_id);
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (stream_ctx == nullptr) {
            open->failed = true;
            open->error = "failed to create local webtransport stream";
            open->completed = true;
            impl.condition.notify_all();
            continue;
        }
        // For client-created WebTransport application streams, incoming peer bytes on
        // the same QUIC stream must be delivered directly to the application callback.
        stream_ctx->is_open = 0;
        stream_ctx->is_h3 = 0;
        stream_ctx->path_callback = webtransport_callback;
        stream_ctx->path_callback_ctx = &impl;
        open->assigned_stream_id = stream_ctx->stream_id;
        open->completed = true;
        impl.application_streams.insert(stream_ctx->stream_id);
        impl.condition.notify_all();
    }

    for (auto& write : writes) {
        h3zero_stream_ctx_t* stream_ctx = find_local_stream_context(impl, write.stream_id);
        if (stream_ctx == nullptr) {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.failed = true;
            impl.last_error = "failed to find webtransport stream context";
            impl.condition.notify_all();
            return -1;
        }

        if (picoquic_add_to_stream_with_ctx(
                impl.cnx, write.stream_id, write.bytes.data(), write.bytes.size(), write.fin ? 1 : 0, stream_ctx) != 0) {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.failed = true;
            impl.last_error = "failed to queue webtransport stream data";
            impl.condition.notify_all();
            return -1;
        }
    }

    if (close_requested && impl.control_stream_ctx != nullptr) {
        if (picowt_send_close_session_message(impl.cnx, impl.control_stream_ctx, 0, "closed") != 0) {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.failed = true;
            impl.last_error = "failed to close webtransport session";
            impl.condition.notify_all();
            return -1;
        }
    }

    return 0;
}

int webtransport_callback(picoquic_cnx_t* cnx,
                          uint8_t* bytes,
                          size_t length,
                          picohttp_call_back_event_t event,
                          h3zero_stream_ctx_t* stream_ctx,
                          void* path_app_ctx) {
    static_cast<void>(cnx);
    auto* impl = static_cast<WebTransportClient::Impl*>(path_app_ctx);
    if (impl == nullptr) {
        return -1;
    }

    switch (event) {
        case picohttp_callback_get:
        case picohttp_callback_post:
        case picohttp_callback_connect:
        case picohttp_callback_post_datagram:
        case picohttp_callback_provide_datagram:
        case picohttp_callback_free:
        case picohttp_callback_connecting:
        case picohttp_callback_provide_data:
            return 0;
        case picohttp_callback_connect_refused: {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->failed = true;
            impl->last_error = "webtransport CONNECT refused with status=" +
                               std::to_string(stream_ctx != nullptr ? stream_ctx->ps.stream_state.header.status : 0);
            impl->condition.notify_all();
            return 0;
        }
        case picohttp_callback_connect_accepted: {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->connected = true;
            impl->condition.notify_all();
            return 0;
        }
        case picohttp_callback_post_data:
        case picohttp_callback_post_fin: {
            if (stream_ctx == nullptr) {
                return 0;
            }
            std::lock_guard<std::mutex> lock(impl->mutex);
            auto& received = impl->received_streams[stream_ctx->stream_id];
            if (bytes != nullptr && length != 0) {
                received.bytes.insert(received.bytes.end(), bytes, bytes + length);
            }
            if (event == picohttp_callback_post_fin) {
                received.fin = true;
                if (impl->control_stream_ctx != nullptr &&
                    stream_ctx->stream_id == impl->control_stream_ctx->stream_id) {
                    impl->disconnected = true;
                }
            }
            impl->condition.notify_all();
            return 0;
        }
        case picohttp_callback_reset:
        case picohttp_callback_stop_sending: {
            if (stream_ctx != nullptr) {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->received_streams[stream_ctx->stream_id].fin = true;
                impl->condition.notify_all();
            }
            return 0;
        }
        case picohttp_callback_deregister: {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (!impl->connected && control_stream_connect_established(*impl)) {
                impl->connected = true;
            }
            impl->disconnected = true;
            impl->condition.notify_all();
            return 0;
        }
        default:
            return 0;
    }
}

int loop_callback(picoquic_quic_t* quic,
                  picoquic_packet_loop_cb_enum cb_mode,
                  void* callback_ctx,
                  void* callback_arg) {
    static_cast<void>(quic);
    static_cast<void>(callback_arg);

    auto* impl = static_cast<WebTransportClient::Impl*>(callback_ctx);
    if (impl == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    switch (cb_mode) {
        case picoquic_packet_loop_ready: {
            auto* options = static_cast<picoquic_packet_loop_options_t*>(callback_arg);
            if (options != nullptr) {
                options->do_time_check = 1;
            }
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->loop_ready = true;
            impl->condition.notify_all();
            return 0;
        }
        case picoquic_packet_loop_wake_up:
            return apply_pending_writes(*impl);
        case picoquic_packet_loop_time_check: {
            auto* time_check = static_cast<packet_loop_time_check_arg_t*>(callback_arg);
            if (time_check != nullptr && time_check->delta_t > 10000) {
                time_check->delta_t = 10000;
            }
            return apply_pending_writes(*impl);
        }
        case picoquic_packet_loop_after_send: {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (!impl->connected && control_stream_connect_established(*impl)) {
                impl->connected = true;
                impl->condition.notify_all();
            }
            if (impl->cnx != nullptr && picoquic_get_cnx_state(impl->cnx) == picoquic_state_disconnected) {
                impl->disconnected = true;
                impl->condition.notify_all();
                return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
            }
            return 0;
        }
        case picoquic_packet_loop_after_receive:
            return 0;
        case picoquic_packet_loop_port_update:
            return 0;
        default:
            return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
}

int webtransport_connection_callback(picoquic_cnx_t* cnx,
                                     uint64_t stream_id,
                                     uint8_t* bytes,
                                     size_t length,
                                     picoquic_call_back_event_t fin_or_event,
                                     void* callback_ctx,
                                     void* v_stream_ctx) {
    return h3zero_callback(cnx, stream_id, bytes, length, fin_or_event, callback_ctx, v_stream_ctx);
}

}  // namespace
#endif

WebTransportClient::WebTransportClient() : impl_(std::make_unique<Impl>()) {}

WebTransportClient::~WebTransportClient() {
    const auto status = close(0);
    static_cast<void>(status);
}

TransportStatus WebTransportClient::configure(const EndpointConfig& endpoint, const TlsConfig& tls) {
    endpoint_ = endpoint;
    tls_ = tls;
    state_ = ConnectionState::kIdle;
    next_bidirectional_stream_id_ = 0;
    next_unidirectional_stream_id_ = 2;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->connected = false;
    impl_->failed = false;
    impl_->disconnected = false;
    impl_->close_requested = false;
    impl_->loop_ready = false;
    impl_->loop_exited = false;
    impl_->last_error.clear();
    impl_->pending_writes.clear();
    impl_->received_streams.clear();
    return TransportStatus::success();
}

TransportStatus WebTransportClient::connect() {
    if (!endpoint_.has_value()) {
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("webtransport transport is not configured");
    }

#ifndef OPENMOQ_HAS_PICOQUIC
    state_ = ConnectionState::kFailed;
    return TransportStatus::failure("picoquic support is not enabled in this build");
#else
    if (endpoint_->path.empty()) {
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("webtransport endpoint path must not be empty");
    }

    state_ = ConnectionState::kConnecting;

    int is_name = 0;
    if (picoquic_get_server_address(endpoint_->host.c_str(), endpoint_->port, &impl_->server_address, &is_name) != 0) {
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to resolve webtransport server address");
    }

    const char* sni = nullptr;
    if (!endpoint_->sni.empty()) {
        sni = endpoint_->sni.c_str();
    } else if (is_name) {
        sni = endpoint_->host.c_str();
    }

    const uint64_t current_time = picoquic_current_time();
    impl_->quic = picoquic_create(1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                  current_time, nullptr, nullptr, nullptr, 0);
    if (impl_->quic == nullptr) {
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to create picoquic context for webtransport");
    }

    if (tls_.has_value() && tls_->insecure_skip_verify) {
        picoquic_set_null_verifier(impl_->quic);
    }

    impl_->cnx = picoquic_create_cnx(impl_->quic, picoquic_null_connection_id, picoquic_null_connection_id,
                                     reinterpret_cast<struct sockaddr*>(&impl_->server_address), current_time, 0,
                                     sni, endpoint_->alpn.c_str(), 1);
    if (impl_->cnx == nullptr) {
        picoquic_free(impl_->quic);
        impl_->quic = nullptr;
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to create webtransport connection");
    }

    if (picowt_prepare_client_cnx(impl_->quic,
                                  reinterpret_cast<struct sockaddr*>(&impl_->server_address),
                                  &impl_->cnx,
                                  &impl_->h3_ctx,
                                  &impl_->control_stream_ctx,
                                  current_time,
                                  sni) != 0) {
        picoquic_free(impl_->quic);
        impl_->cnx = nullptr;
        impl_->quic = nullptr;
        impl_->h3_ctx = nullptr;
        impl_->control_stream_ctx = nullptr;
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to prepare webtransport client context");
    }

    // When dialing an IP literal with SNI override, WebTransport still needs a
    // hostname-based :authority so relays can route the request correctly.
    const std::string_view authority_host =
        (!is_name && !endpoint_->sni.empty()) ? std::string_view(endpoint_->sni) : std::string_view(endpoint_->host);
    std::string authority = std::string(authority_host) + ":" + std::to_string(endpoint_->port);
    if (picowt_connect(impl_->cnx,
                       impl_->h3_ctx,
                       impl_->control_stream_ctx,
                       authority.c_str(),
                       endpoint_->path.c_str(),
                       webtransport_callback,
                       impl_.get(),
                       endpoint_->application_protocol.empty() ? nullptr : endpoint_->application_protocol.c_str()) != 0) {
        picoquic_free(impl_->quic);
        impl_->cnx = nullptr;
        impl_->quic = nullptr;
        impl_->h3_ctx = nullptr;
        impl_->control_stream_ctx = nullptr;
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to queue webtransport CONNECT request");
    }

    register_connection_impl(impl_->cnx, impl_.get());
    picoquic_set_callback(impl_->cnx, webtransport_connection_callback, impl_->h3_ctx);

    if (picoquic_start_client_cnx(impl_->cnx) != 0) {
        unregister_connection_impl(impl_->cnx);
        picoquic_free(impl_->quic);
        impl_->cnx = nullptr;
        impl_->quic = nullptr;
        impl_->h3_ctx = nullptr;
        impl_->control_stream_ctx = nullptr;
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to start webtransport client connection");
    }

    impl_->packet_loop_thread = std::thread([impl = impl_.get()] {
        impl->packet_loop_return_code =
            picoquic_packet_loop(impl->quic, 0, impl->server_address.ss_family, 0, 0, 1, loop_callback, impl);
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->loop_exited = true;
        if (!impl->loop_ready && !impl->failed && !impl->disconnected) {
            impl->failed = true;
            impl->last_error = "webtransport packet loop exited before becoming ready";
        }
        impl->condition.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        const bool loop_ready = impl_->condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return impl_->loop_ready || impl_->failed || impl_->disconnected || impl_->loop_exited;
        });
        if (!loop_ready) {
            impl_->failed = true;
            impl_->last_error = "timed out waiting for webtransport packet loop";
        }
        if (impl_->failed || impl_->disconnected || impl_->loop_exited) {
            state_ = ConnectionState::kFailed;
            return TransportStatus::failure(impl_->last_error.empty() ? "webtransport startup failed" : impl_->last_error);
        }
        const bool session_ready = impl_->condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return impl_->connected || impl_->failed || impl_->disconnected || impl_->loop_exited;
        });
        if (!session_ready) {
            impl_->failed = true;
            impl_->last_error = "timed out waiting for webtransport CONNECT acceptance";
        }
        if (impl_->failed || impl_->disconnected || impl_->loop_exited || !impl_->connected) {
            state_ = ConnectionState::kFailed;
            return TransportStatus::failure(impl_->last_error.empty() ? "webtransport startup failed" : impl_->last_error);
        }
    }

    if (impl_->control_stream_ctx != nullptr) {
        next_bidirectional_stream_id_ = impl_->control_stream_ctx->stream_id + 4;
    } else {
        next_bidirectional_stream_id_ = 4;
    }
    next_unidirectional_stream_id_ = 2;
    state_ = ConnectionState::kConnected;
    return TransportStatus::success();

#endif
}

ConnectionState WebTransportClient::state() const {
    return state_;
}

TransportStatus WebTransportClient::open_stream(StreamDirection direction, std::uint64_t& stream_id) {
    if (state_ != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

#ifndef OPENMOQ_HAS_PICOQUIC
    if (direction == StreamDirection::kBidirectional) {
        stream_id = next_bidirectional_stream_id_;
        next_bidirectional_stream_id_ += 4;
    } else {
        stream_id = next_unidirectional_stream_id_;
        next_unidirectional_stream_id_ += 4;
    }
#else
    if (impl_ == nullptr || impl_->cnx == nullptr || impl_->h3_ctx == nullptr || impl_->control_stream_ctx == nullptr) {
        return TransportStatus::failure("webtransport stream context is not initialized");
    }
    Impl::PendingOpen pending_open;
    pending_open.direction = direction;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->pending_opens.push_back(&pending_open);
    }
    if (impl_->cnx != nullptr && impl_->quic != nullptr) {
        picoquic_set_app_wake_time(impl_->cnx, picoquic_get_quic_time(impl_->quic));
    }
    impl_->condition.notify_all();

    std::unique_lock<std::mutex> lock(impl_->mutex);
    const bool completed = impl_->condition.wait_for(lock, std::chrono::seconds(5), [&] {
        return pending_open.completed || impl_->failed || impl_->disconnected;
    });
    if (!completed) {
        return TransportStatus::failure("timed out creating local webtransport stream");
    }
    if (pending_open.failed) {
        return TransportStatus::failure(pending_open.error.empty() ? "failed to create local webtransport stream"
                                                                  : pending_open.error);
    }
    if (impl_->failed) {
        return TransportStatus::failure(impl_->last_error.empty() ? "webtransport connection failed" : impl_->last_error);
    }
    if (impl_->disconnected) {
        return TransportStatus::failure("webtransport connection closed");
    }
    stream_id = pending_open.assigned_stream_id;
#endif
    return TransportStatus::success();
}

TransportStatus WebTransportClient::write_stream(std::uint64_t stream_id,
                                                 std::span<const std::uint8_t> bytes,
                                                 bool fin) {
    if (state_ != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

#ifndef OPENMOQ_HAS_PICOQUIC
    static_cast<void>(stream_id);
    static_cast<void>(bytes);
    static_cast<void>(fin);
    return TransportStatus::failure("picoquic support is not enabled in this build");
#else
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->pending_writes.push_back({
            .stream_id = stream_id,
            .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
            .fin = fin,
        });
    }

    if (impl_->cnx != nullptr && impl_->quic != nullptr) {
        picoquic_set_app_wake_time(impl_->cnx, picoquic_get_quic_time(impl_->quic));
    }
    impl_->condition.notify_all();
    return TransportStatus::success();
#endif
}

TransportStatus WebTransportClient::read_stream(std::uint64_t stream_id,
                                                std::vector<std::uint8_t>& bytes,
                                                bool& fin,
                                                std::chrono::milliseconds timeout) {
    if (state_ != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

#ifndef OPENMOQ_HAS_PICOQUIC
    static_cast<void>(stream_id);
    static_cast<void>(bytes);
    static_cast<void>(fin);
    static_cast<void>(timeout);
    return TransportStatus::failure("picoquic support is not enabled in this build");
#else
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const bool ready = impl_->condition.wait_for(lock, timeout, [&] {
        return impl_->received_streams.contains(stream_id) || impl_->failed || impl_->disconnected;
    });
    if (!ready) {
        return TransportStatus::failure("timed out waiting for stream data");
    }

    if (impl_->failed) {
        return TransportStatus::failure(impl_->last_error.empty() ? "webtransport connection failed" : impl_->last_error);
    }
    if (impl_->disconnected && !impl_->received_streams.contains(stream_id)) {
        return TransportStatus::failure("webtransport connection closed");
    }

    auto it = impl_->received_streams.find(stream_id);
    if (it == impl_->received_streams.end()) {
        return TransportStatus::failure("no queued read for stream");
    }

    bytes = std::move(it->second.bytes);
    fin = it->second.fin;
    impl_->received_streams.erase(it);
    return TransportStatus::success();
#endif
}

std::string WebTransportClient::connection_id() const {
#ifdef OPENMOQ_HAS_PICOQUIC
    if (impl_ != nullptr && impl_->cnx != nullptr) {
        return std::string("wt-") + std::to_string(reinterpret_cast<std::uintptr_t>(impl_->cnx));
    }
#endif
    return {};
}

TransportStatus WebTransportClient::close(std::uint64_t application_error_code) {
    static_cast<void>(application_error_code);

    if (impl_ == nullptr) {
        state_ = ConnectionState::kClosed;
        return TransportStatus::success();
    }

#ifdef OPENMOQ_HAS_PICOQUIC
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->close_requested = true;
    }
    if (impl_->cnx != nullptr && impl_->quic != nullptr) {
        picoquic_set_app_wake_time(impl_->cnx, picoquic_get_quic_time(impl_->quic));
    }
    impl_->condition.notify_all();

    if (impl_->packet_loop_thread.joinable()) {
        impl_->packet_loop_thread.join();
    }

    unregister_connection_impl(impl_->cnx);

    if (impl_->h3_ctx != nullptr && impl_->control_stream_ctx != nullptr && impl_->cnx != nullptr) {
        picowt_deregister(impl_->cnx, impl_->h3_ctx, impl_->control_stream_ctx);
        impl_->control_stream_ctx = nullptr;
    }
    if (impl_->quic != nullptr) {
        picoquic_free(impl_->quic);
        impl_->quic = nullptr;
        impl_->cnx = nullptr;
        impl_->h3_ctx = nullptr;
    }
#endif

    state_ = ConnectionState::kClosed;
    return TransportStatus::success();
}

}  // namespace openmoq::publisher::transport
