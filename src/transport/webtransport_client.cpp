#include "openmoq/publisher/transport/webtransport_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
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
    };

    struct OutgoingStreamData {
        std::vector<std::uint8_t> bytes;
        std::size_t offset = 0;
        bool fin_requested = false;
    };

    struct ReceivedStreamData {
        std::vector<std::uint8_t> bytes;
        bool fin = false;
    };

    std::mutex mutex;
    std::condition_variable condition;
    std::deque<PendingWrite> pending_writes;
    std::map<std::uint64_t, OutgoingStreamData> outgoing_streams;
    std::map<std::uint64_t, ReceivedStreamData> received_streams;
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

bool trace_enabled() {
    static const bool enabled = std::getenv("OPENMOQ_PICOQUIC_TRACE") != nullptr;
    return enabled;
}

std::string hex_dump(std::span<const std::uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index != 0) {
            out.push_back(' ');
        }
        const std::uint8_t value = bytes[index];
        out.push_back(kHex[(value >> 4) & 0x0f]);
        out.push_back(kHex[value & 0x0f]);
    }
    return out;
}

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

    std::deque<WebTransportClient::Impl::PendingWrite> writes;
    bool close_requested = false;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        writes.swap(impl.pending_writes);
        close_requested = impl.close_requested;
    }

    for (auto& write : writes) {
        if (trace_enabled()) {
            std::cerr << "[webtransport] apply write stream=" << write.stream_id
                      << std::endl;
        }
        h3zero_stream_ctx_t* stream_ctx = find_local_stream_context(impl, write.stream_id);
        if (stream_ctx == nullptr) {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.failed = true;
            impl.last_error = "failed to find webtransport stream context";
            impl.condition.notify_all();
            return -1;
        }

        if (picoquic_mark_active_stream(impl.cnx, write.stream_id, 1, stream_ctx) != 0) {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.failed = true;
            impl.last_error = "failed to activate webtransport stream";
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

int provide_pending_stream_data(WebTransportClient::Impl& impl,
                                uint8_t* context,
                                size_t space,
                                h3zero_stream_ctx_t* stream_ctx) {
    if (stream_ctx == nullptr) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(impl.mutex);
    auto it = impl.outgoing_streams.find(stream_ctx->stream_id);
    if (it == impl.outgoing_streams.end()) {
        (void)picoquic_provide_stream_data_buffer(context, 0, 0, 0);
        return 0;
    }

    auto& outgoing = it->second;
    const std::size_t remaining = outgoing.bytes.size() - outgoing.offset;
    const std::size_t to_send = std::min(space, remaining);
    const bool send_fin = outgoing.fin_requested && to_send == remaining;
    const bool still_active = to_send < remaining ? 1 : 0;

    uint8_t* buffer = picoquic_provide_stream_data_buffer(context, to_send, send_fin ? 1 : 0, still_active ? 1 : 0);
    if (buffer == nullptr) {
        impl.failed = true;
        impl.last_error = "failed to obtain stream send buffer";
        impl.condition.notify_all();
        return -1;
    }

    if (to_send != 0) {
        std::memcpy(buffer, outgoing.bytes.data() + static_cast<std::ptrdiff_t>(outgoing.offset), to_send);
        outgoing.offset += to_send;
    }

    if (to_send == remaining) {
        if (send_fin || !outgoing.fin_requested) {
            impl.outgoing_streams.erase(it);
        } else {
            outgoing.bytes.clear();
            outgoing.offset = 0;
        }
    }

    if (trace_enabled()) {
        std::cerr << "[webtransport] provide_data stream=" << stream_ctx->stream_id
                  << " bytes=" << to_send
                  << " fin=" << (send_fin ? 1 : 0)
                  << " still_active=" << (still_active ? 1 : 0) << std::endl;
    }
    return 0;
}

int webtransport_callback(picoquic_cnx_t* cnx,
                          uint8_t* bytes,
                          size_t length,
                          picohttp_call_back_event_t event,
                          h3zero_stream_ctx_t* stream_ctx,
                          void* path_app_ctx) {
    auto* impl = static_cast<WebTransportClient::Impl*>(path_app_ctx);
    if (impl == nullptr) {
        return -1;
    }

    if (trace_enabled()) {
        std::cerr << "[webtransport] callback event=" << static_cast<int>(event)
                  << " stream=" << (stream_ctx != nullptr ? std::to_string(stream_ctx->stream_id) : "<none>")
                  << " length=" << length;
        if (event == picohttp_callback_connect_refused && stream_ctx != nullptr) {
            std::cerr << " status=" << stream_ctx->ps.stream_state.header.status;
        }
        if (stream_ctx != nullptr) {
            std::cerr << " is_h3=" << (stream_ctx->is_h3 ? 1 : 0)
                      << " is_open=" << (stream_ctx->is_open ? 1 : 0)
                      << " upgraded=" << (stream_ctx->is_upgraded ? 1 : 0)
                      << " wt=" << (stream_ctx->ps.stream_state.is_web_transport ? 1 : 0)
                      << " control_stream_id=" << stream_ctx->ps.stream_state.control_stream_id
                      << " stream_type=" << stream_ctx->ps.stream_state.stream_type;
        }
        if (cnx != nullptr) {
            std::cerr << " cnx_state=" << picoquic_get_cnx_state(cnx);
        }
        std::cerr << std::endl;
    }

    switch (event) {
        case picohttp_callback_get:
        case picohttp_callback_post:
        case picohttp_callback_connect:
        case picohttp_callback_post_datagram:
        case picohttp_callback_provide_datagram:
        case picohttp_callback_free:
        case picohttp_callback_connecting:
            return 0;
        case picohttp_callback_provide_data:
            return provide_pending_stream_data(*impl, bytes, length, stream_ctx);
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
            if (trace_enabled() && impl->control_stream_ctx != nullptr) {
                std::cerr << "[webtransport] CONNECT accepted control_stream="
                          << impl->control_stream_ctx->stream_id;
                if (stream_ctx != nullptr && stream_ctx->ps.stream_state.header.wt_protocol != nullptr) {
                    std::cerr << " wt_protocol=" << stream_ctx->ps.stream_state.header.wt_protocol;
                }
                std::cerr << std::endl;
            }
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
            if (trace_enabled()) {
                std::cerr << "[webtransport] queued stream data stream=" << stream_ctx->stream_id
                          << " total_bytes=" << received.bytes.size()
                          << " fin=" << (event == picohttp_callback_post_fin ? 1 : 0)
                          << " bytes=["
                          << hex_dump(std::span<const std::uint8_t>(received.bytes.data(), received.bytes.size()))
                          << "]" << std::endl;
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
            if (impl->cnx != nullptr && picoquic_get_cnx_state(impl->cnx) == picoquic_state_disconnected) {
                if (trace_enabled()) {
                    std::cerr << "[webtransport] disconnected"
                              << " local_error=0x" << std::hex << picoquic_get_local_error(impl->cnx)
                              << " remote_error=0x" << picoquic_get_remote_error(impl->cnx)
                              << " application_error=0x" << picoquic_get_application_error(impl->cnx)
                              << std::dec << std::endl;
                }
                impl->disconnected = true;
                impl->condition.notify_all();
                return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
            }
            return 0;
        }
        case picoquic_packet_loop_after_receive:
        case picoquic_packet_loop_port_update:
            return 0;
        default:
            return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
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
    impl_->outgoing_streams.clear();
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

    std::string authority = endpoint_->host + ":" + std::to_string(endpoint_->port);
    if (trace_enabled()) {
        std::cerr << "[webtransport] connect host=" << endpoint_->host
                  << " port=" << endpoint_->port
                  << " authority=" << authority
                  << " path=" << endpoint_->path
                  << " alpn=" << endpoint_->alpn
                  << " wt_protocols="
                  << (endpoint_->application_protocol.empty() ? "<none>" : endpoint_->application_protocol)
                  << " sni=" << (sni == nullptr ? "<none>" : sni) << std::endl;
    }
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

    if (picoquic_start_client_cnx(impl_->cnx) != 0) {
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
        if (!impl->connected && !impl->failed && !impl->disconnected) {
            impl->failed = true;
            impl->last_error = "webtransport packet loop exited before CONNECT completed";
        }
        impl->condition.notify_all();
    });

    std::unique_lock<std::mutex> lock(impl_->mutex);
    const bool completed = impl_->condition.wait_for(lock, std::chrono::seconds(5), [&] {
        return (impl_->loop_ready && (impl_->connected || impl_->failed || impl_->disconnected)) ||
               impl_->failed || impl_->disconnected || impl_->loop_exited;
    });
    if (!completed) {
        impl_->failed = true;
        impl_->last_error = "timed out waiting for webtransport CONNECT";
    }

    if (impl_->connected) {
        if (impl_->control_stream_ctx != nullptr) {
            next_bidirectional_stream_id_ = impl_->control_stream_ctx->stream_id + 4;
        } else {
            next_bidirectional_stream_id_ = 4;
        }
        next_unidirectional_stream_id_ = 2;
        state_ = ConnectionState::kConnected;
        return TransportStatus::success();
    }

    state_ = ConnectionState::kFailed;
    return TransportStatus::failure(impl_->last_error.empty() ? "webtransport connect failed" : impl_->last_error);
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

    h3zero_stream_ctx_t* stream_ctx =
        picowt_create_local_stream(impl_->cnx,
                                   direction == StreamDirection::kBidirectional ? 1 : 0,
                                   impl_->h3_ctx,
                                   impl_->control_stream_ctx->stream_id);
    if (stream_ctx == nullptr) {
        return TransportStatus::failure("failed to create local webtransport stream");
    }
    stream_ctx->path_callback = webtransport_callback;
    stream_ctx->path_callback_ctx = impl_.get();
    stream_id = stream_ctx->stream_id;
    if (trace_enabled()) {
        std::cerr << "[webtransport] open_stream direction="
                  << (direction == StreamDirection::kBidirectional ? "bidi" : "uni")
                  << " stream=" << stream_id << std::endl;
    }
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
        auto& outgoing = impl_->outgoing_streams[stream_id];
        outgoing.bytes.insert(outgoing.bytes.end(), bytes.begin(), bytes.end());
        outgoing.fin_requested = outgoing.fin_requested || fin;
        impl_->pending_writes.push_back({.stream_id = stream_id});
    }
    if (trace_enabled()) {
        std::cerr << "[webtransport] queue write stream=" << stream_id
                  << " bytes=" << bytes.size()
                  << " fin=" << (fin ? 1 : 0) << std::endl;
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
    if (trace_enabled()) {
        std::cerr << "[webtransport] read_stream wait stream=" << stream_id
                  << " timeout_ms=" << timeout.count() << std::endl;
    }
    const bool ready = impl_->condition.wait_for(lock, timeout, [&] {
        return impl_->received_streams.contains(stream_id) || impl_->failed || impl_->disconnected;
    });
    if (!ready) {
        if (trace_enabled()) {
            std::cerr << "[webtransport] read_stream timeout stream=" << stream_id << std::endl;
        }
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
    if (trace_enabled()) {
        std::cerr << "[webtransport] read_stream got stream=" << stream_id
                  << " bytes=" << bytes.size()
                  << " fin=" << (fin ? 1 : 0) << std::endl;
    }
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
