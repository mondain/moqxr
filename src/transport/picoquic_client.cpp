#include "openmoq/publisher/transport/picoquic_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef OPENMOQ_HAS_PICOQUIC
#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_utils.h>
#include <picosocks.h>
#endif

namespace openmoq::publisher::transport {

struct PicoquicClient::Impl {
    struct PendingWrite {
        std::uint64_t stream_id = 0;
        std::vector<std::uint8_t> bytes;
        bool fin = false;
    };

    struct ReceivedStreamData {
        std::vector<std::uint8_t> bytes;
        bool fin = false;
    };

    std::mutex mutex;
    std::condition_variable condition;
    std::deque<PendingWrite> pending_writes;
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
    int packet_loop_return_code = 0;
#endif
};

#ifdef OPENMOQ_HAS_PICOQUIC
namespace {

bool trace_enabled() {
    static const bool enabled = std::getenv("OPENMOQ_PICOQUIC_TRACE") != nullptr;
    return enabled;
}

void trace(const std::string& message) {
    if (trace_enabled()) {
        std::cerr << "[picoquic-client] " << message << std::endl;
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

int apply_pending_operations(PicoquicClient::Impl& impl) {
    if (impl.cnx == nullptr) {
        return 0;
    }

    std::deque<PicoquicClient::Impl::PendingWrite> writes;
    bool close_requested = false;

    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        writes.swap(impl.pending_writes);
        close_requested = impl.close_requested;
    }

    for (auto& write : writes) {
        trace("stream " + std::to_string(write.stream_id) + " bytes=[" + hex_dump(write.bytes) + "]");
        const int ret = picoquic_add_to_stream(
            impl.cnx, write.stream_id, write.bytes.data(), write.bytes.size(), write.fin ? 1 : 0);
        if (ret != 0) {
            trace("add_to_stream failed for stream " + std::to_string(write.stream_id));
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.failed = true;
            impl.last_error = "picoquic_add_to_stream failed";
            impl.condition.notify_all();
            return ret;
        }
    }

    if (close_requested) {
        trace("close requested");
        const int ret = picoquic_close(impl.cnx, 0);
        if (ret != 0) {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.failed = true;
            impl.last_error = "picoquic_close failed";
            impl.condition.notify_all();
            return ret;
        }
    }

    return 0;
}

int client_callback(picoquic_cnx_t* cnx,
                    uint64_t stream_id,
                    uint8_t* bytes,
                    size_t length,
                    picoquic_call_back_event_t event,
                    void* callback_ctx,
                    void* stream_ctx) {
    static_cast<void>(cnx);
    static_cast<void>(stream_ctx);

    auto* impl = static_cast<PicoquicClient::Impl*>(callback_ctx);
    if (impl == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    switch (event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin: {
            std::lock_guard<std::mutex> lock(impl->mutex);
            auto& received = impl->received_streams[stream_id];
            if (bytes != nullptr && length != 0) {
                received.bytes.insert(received.bytes.end(), bytes, bytes + length);
            }
            if (event == picoquic_callback_stream_fin) {
                received.fin = true;
            }
            impl->condition.notify_all();
            return 0;
        }
        case picoquic_callback_almost_ready:
        case picoquic_callback_ready: {
            trace(event == picoquic_callback_ready ? "callback ready" : "callback almost_ready");
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->connected = true;
            impl->condition.notify_all();
            return 0;
        }
        case picoquic_callback_close:
        case picoquic_callback_application_close:
        case picoquic_callback_stateless_reset: {
            const std::uint64_t local_error = picoquic_get_local_error(cnx);
            const std::uint64_t remote_error = picoquic_get_remote_error(cnx);
            const std::uint64_t application_error = picoquic_get_application_error(cnx);
            std::uint64_t local_reason = 0;
            std::uint64_t remote_reason = 0;
            std::uint64_t local_application_reason = 0;
            std::uint64_t remote_application_reason = 0;
            picoquic_get_close_reasons(
                cnx, &local_reason, &remote_reason, &local_application_reason, &remote_application_reason);
            trace(std::string("callback connection closed event=") +
                  (event == picoquic_callback_application_close
                       ? "application_close"
                       : (event == picoquic_callback_stateless_reset ? "stateless_reset" : "close")) +
                  " local_error=" + std::to_string(local_error) +
                  " remote_error=" + std::to_string(remote_error) +
                  " application_error=" + std::to_string(application_error) +
                  " local_reason=" + std::to_string(local_reason) +
                  " remote_reason=" + std::to_string(remote_reason) +
                  " local_application_reason=" + std::to_string(local_application_reason) +
                  " remote_application_reason=" + std::to_string(remote_application_reason));
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->disconnected = true;
            if (!impl->connected) {
                impl->failed = true;
                impl->last_error = "connection closed before reaching ready state";
            } else if (impl->last_error.empty() &&
                       (remote_error != 0 || remote_reason != 0 || remote_application_reason != 0 ||
                        application_error != 0)) {
                impl->last_error = "peer closed connection with remote_error=" + std::to_string(remote_error) +
                                   ", remote_reason=" + std::to_string(remote_reason) +
                                   ", remote_application_reason=" + std::to_string(remote_application_reason) +
                                   ", application_error=" + std::to_string(application_error);
            }
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

    auto* impl = static_cast<PicoquicClient::Impl*>(callback_ctx);
    if (impl == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    switch (cb_mode) {
        case picoquic_packet_loop_ready: {
            trace("packet loop ready");
            auto* options = static_cast<picoquic_packet_loop_options_t*>(callback_arg);
            if (options != nullptr) {
                options->do_time_check = 1;
            }
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->loop_ready = true;
            impl->condition.notify_all();
            return 0;
        }
        case picoquic_packet_loop_after_receive:
            if (callback_arg != nullptr) {
                trace("packet loop after receive count=" +
                      std::to_string(*static_cast<size_t*>(callback_arg)));
            } else {
                trace("packet loop after receive");
            }
            return 0;
        case picoquic_packet_loop_wake_up:
            trace("packet loop wake up");
            return apply_pending_operations(*impl);
        case picoquic_packet_loop_time_check: {
            trace("packet loop time check");
            auto* time_check = static_cast<packet_loop_time_check_arg_t*>(callback_arg);
            if (time_check != nullptr && time_check->delta_t > 10000) {
                time_check->delta_t = 10000;
            }
            return apply_pending_operations(*impl);
        }
        case picoquic_packet_loop_after_send: {
            if (callback_arg != nullptr) {
                trace("packet loop after send count=" +
                      std::to_string(*static_cast<size_t*>(callback_arg)));
            } else {
                trace("packet loop after send");
            }
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (impl->disconnected) {
                return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
            }
            return 0;
        }
        case picoquic_packet_loop_port_update:
            return 0;
        default:
            return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
}

}  // namespace
#endif

PicoquicClient::PicoquicClient() : impl_(std::make_unique<Impl>()) {}

PicoquicClient::~PicoquicClient() {
    const auto status = close(0);
    static_cast<void>(status);
}

TransportStatus PicoquicClient::configure(const EndpointConfig& endpoint, const TlsConfig& tls) {
    endpoint_ = endpoint;
    tls_ = tls;
    state_ = ConnectionState::kIdle;
    next_bidirectional_stream_id_ = 0;
    next_unidirectional_stream_id_ = 2;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->connected = false;
        impl_->failed = false;
        impl_->disconnected = false;
        impl_->close_requested = false;
        impl_->loop_ready = false;
        impl_->loop_exited = false;
        impl_->last_error.clear();
        impl_->pending_writes.clear();
    }

    return TransportStatus::success();
}

TransportStatus PicoquicClient::connect() {
    if (!endpoint_.has_value()) {
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("picoquic transport is not configured");
    }

#ifndef OPENMOQ_HAS_PICOQUIC
    state_ = ConnectionState::kFailed;
    return TransportStatus::failure("picoquic support is not enabled in this build");
#else
    state_ = ConnectionState::kConnecting;

    int is_name = 0;
    if (picoquic_get_server_address(endpoint_->host.c_str(), endpoint_->port, &impl_->server_address, &is_name) != 0) {
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to resolve picoquic server address");
    }

    const char* sni = is_name ? endpoint_->host.c_str() : "localhost";
    const char* alpn = endpoint_->alpn.c_str();
    const uint64_t current_time = picoquic_current_time();
    trace("connect start host=" + endpoint_->host + " port=" + std::to_string(endpoint_->port) +
          " sni=" + sni + " alpn=" + alpn);

    impl_->quic =
        picoquic_create(1, nullptr, nullptr, nullptr, alpn, nullptr, nullptr, nullptr, nullptr, nullptr,
                        current_time, nullptr, nullptr, nullptr, 0);
    if (impl_->quic == nullptr) {
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to create picoquic context");
    }

    if (tls_.has_value() && tls_->insecure_skip_verify) {
        picoquic_set_null_verifier(impl_->quic);
    }

    impl_->cnx = picoquic_create_cnx(impl_->quic, picoquic_null_connection_id, picoquic_null_connection_id,
                                     reinterpret_cast<struct sockaddr*>(&impl_->server_address), current_time, 0,
                                     sni, alpn, 1);
    if (impl_->cnx == nullptr) {
        picoquic_free(impl_->quic);
        impl_->quic = nullptr;
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to create picoquic connection");
    }

    picoquic_set_callback(impl_->cnx, client_callback, impl_.get());

    if (picoquic_start_client_cnx(impl_->cnx) != 0) {
        picoquic_free(impl_->quic);
        impl_->cnx = nullptr;
        impl_->quic = nullptr;
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to start picoquic client connection");
    }

    impl_->packet_loop_thread = std::thread([impl = impl_.get()] {
        trace("client packet loop thread start");
        // Disable UDP GSO in the default socket loop. Some Linux driver paths can
        // report a successful send while dropping coalesced UDP packets, which
        // looks exactly like a handshake black hole in local loopback tests.
        impl->packet_loop_return_code =
            picoquic_packet_loop(impl->quic, 0, impl->server_address.ss_family, 0, 0, 1, loop_callback, impl);
        trace("client packet loop thread exit rc=" + std::to_string(impl->packet_loop_return_code));
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->loop_exited = true;
        if (!impl->connected && !impl->failed && !impl->disconnected) {
            impl->failed = true;
            impl->last_error = "picoquic packet loop exited before handshake completed";
        }
        impl->condition.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        const bool completed = impl_->condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return (impl_->loop_ready && (impl_->connected || impl_->failed || impl_->disconnected)) ||
                   impl_->failed || impl_->disconnected || impl_->loop_exited;
        });

        if (!completed) {
            trace("handshake wait timed out");
            impl_->failed = true;
            impl_->last_error = "timed out waiting for picoquic handshake";
        }

        if (impl_->connected) {
            state_ = ConnectionState::kConnected;
            return TransportStatus::success();
        }

        state_ = ConnectionState::kFailed;
        return TransportStatus::failure(impl_->last_error.empty() ? "picoquic handshake failed" : impl_->last_error);
    }
#endif
}

ConnectionState PicoquicClient::state() const {
    return state_;
}

TransportStatus PicoquicClient::open_stream(StreamDirection direction, std::uint64_t& stream_id) {
    if (state_ != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

    if (direction == StreamDirection::kBidirectional) {
        stream_id = next_bidirectional_stream_id_;
        next_bidirectional_stream_id_ += 4;
    } else {
        stream_id = next_unidirectional_stream_id_;
        next_unidirectional_stream_id_ += 4;
    }

    return TransportStatus::success();
}

TransportStatus PicoquicClient::write_stream(std::uint64_t stream_id,
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
        trace("queue write stream=" + std::to_string(stream_id) + " bytes=" + std::to_string(bytes.size()) +
              " fin=" + std::to_string(fin ? 1 : 0));
        impl_->pending_writes.push_back({
            .stream_id = stream_id,
            .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
            .fin = fin,
        });
    }

    return TransportStatus::success();
#endif
}

TransportStatus PicoquicClient::read_stream(std::uint64_t stream_id,
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
        const auto it = impl_->received_streams.find(stream_id);
        return it != impl_->received_streams.end() || impl_->failed || impl_->disconnected;
    });

    if (!ready) {
        return TransportStatus::failure("timed out waiting for stream data");
    }

    const auto it = impl_->received_streams.find(stream_id);
    if (it == impl_->received_streams.end()) {
        return TransportStatus::failure(impl_->last_error.empty() ? "stream closed before data arrived"
                                                                  : impl_->last_error);
    }

    bytes = std::move(it->second.bytes);
    fin = it->second.fin;
    impl_->received_streams.erase(it);
    return TransportStatus::success();
#endif
}

std::string PicoquicClient::connection_id() const {
#ifndef OPENMOQ_HAS_PICOQUIC
    return {};
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_ == nullptr || impl_->cnx == nullptr) {
        return {};
    }

    char buffer[513] = {};
    const picoquic_connection_id_t cid = picoquic_get_logging_cnxid(impl_->cnx);
    if (picoquic_print_connection_id_hexa(buffer, sizeof(buffer), &cid) != 0) {
        return {};
    }
    return buffer;
#endif
}

TransportStatus PicoquicClient::close(std::uint64_t application_error_code) {
    static_cast<void>(application_error_code);

#ifdef OPENMOQ_HAS_PICOQUIC
    if (impl_) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->close_requested = true;
        }
        trace("joining client packet loop thread");
        if (impl_->packet_loop_thread.joinable()) {
            impl_->packet_loop_thread.join();
        }
    }

    if (impl_ && impl_->quic != nullptr) {
        picoquic_free(impl_->quic);
        impl_->quic = nullptr;
        impl_->cnx = nullptr;
    }
#endif

    state_ = ConnectionState::kClosed;
    return TransportStatus::success();
}

}  // namespace openmoq::publisher::transport
