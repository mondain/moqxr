# Design: Draft-18 Request Stream ID Retention and Cancellation

**Date:** 2026-05-13
**Branch:** draft-18
**Scope:** `PublisherTransport`, `WebTransportClient`, `MoqtSession`, affected free functions, session tests

---

## Problem

`send_request_stream_and_wait` opens a QUIC bidirectional stream for each draft-18 request (PUBLISH_NAMESPACE, PUBLISH track), completes the initial handshake, and returns — discarding the stream ID as a local variable. Per draft-18 §3.3.2 and §6.2, these streams are long-lived: a PUBLISH_NAMESPACE stream persists for the lifetime of the namespace subscription, and PUBLISH-track streams persist for the subscription lifetime. Without retaining the stream ID, the session cannot cancel or reset these streams on shutdown or error.

---

## Design

### 1. Transport Interface — `reset_stream`

Add one method to `PublisherTransport`:

```cpp
virtual TransportStatus reset_stream(std::uint64_t stream_id,
                                     std::uint64_t error_code) = 0;
```

`WebTransportClient` implements this using picoquic's `picoquic_reset_stream`. The mock transport in tests gets a recording no-op. Error code `0x0` (INTERNAL_ERROR, draft-18 §3.3.3) is used for shutdown-driven resets; callers may pass a more specific code if available.

### 2. `send_request_stream_and_wait` — Out-Parameter

Extend the signature with an optional out-parameter:

```cpp
TransportStatus send_request_stream_and_wait(
    PublisherTransport& transport,
    openmoq::publisher::DraftVersion draft,
    std::span<const std::uint8_t> request_bytes,
    bool expect_publish_ack,
    PublishOk* publish_ok = nullptr,
    std::uint64_t* out_stream_id = nullptr);
```

The function sets `*out_stream_id = request_stream_id` on every success path. On all failure paths, `out_stream_id` is left untouched (callers initialize retained IDs to `0`; zero means "stream not established").

All existing callers compile unchanged because the parameter defaults to `nullptr`.

### 3. Session State

Add to `MoqtSession` private members:

```cpp
std::uint64_t namespace_stream_id_ = 0;
std::map<std::uint64_t, std::uint64_t> publish_stream_id_by_request_id_;
```

- `namespace_stream_id_`: the single PUBLISH_NAMESPACE bidi stream (shared by `publish()` and `publish_live()` paths; at most one is active at a time).
- `publish_stream_id_by_request_id_`: maps each `request_id` to its PUBLISH-track bidi stream ID. Keyed by `request_id` because that is already the natural key for track state throughout the session.

### 4. Call Site Updates

| Location | Request type | Change |
|---|---|---|
| `MoqtSession::publish()` line ~2294 | PUBLISH_NAMESPACE | pass `&namespace_stream_id_` |
| `MoqtSession::publish_live()` line ~2455 | PUBLISH_NAMESPACE | pass `&namespace_stream_id_` |
| `forward_published_tracks()` line ~1847 | PUBLISH track | capture stream ID into `publish_stream_id_by_request_id_` |
| `publish_selected_tracks()` line ~2051 | PUBLISH track | same |

`forward_published_tracks` and `publish_selected_tracks` are free functions. Each receives a new `std::map<std::uint64_t, std::uint64_t>& publish_stream_ids` reference parameter, which they populate alongside the existing `publish_ok_by_request_id` map. Callers pass `publish_stream_id_by_request_id_` from the session.

### 5. Session Close and Cleanup

`MoqtSession::close()` resets all retained stream IDs before closing the connection:

```cpp
TransportStatus MoqtSession::close(std::uint64_t application_error_code) {
    if (namespace_stream_id_ != 0) {
        transport_.reset_stream(namespace_stream_id_, 0x0);
        namespace_stream_id_ = 0;
    }
    for (auto& [request_id, stream_id] : publish_stream_id_by_request_id_) {
        transport_.reset_stream(stream_id, 0x0);
    }
    publish_stream_id_by_request_id_.clear();
    return transport_.close(application_error_code);
}
```

Reset errors are intentionally ignored — if the transport is already gone, stream resets are best-effort. Clearing the IDs makes double-close safe.

### 6. Tests

Add to `tests/moqt_session_test.cpp`:

1. **Namespace stream retained**: After a successful draft-18 `publish()` or `publish_live()` setup, verify `namespace_stream_id_` is non-zero.
2. **Close resets streams**: After setup, calling `close()` triggers `reset_stream` on the mock transport for the namespace stream ID and for each publish-track stream ID in the map. Verify via a recording mock that captures all `reset_stream` calls.

---

## Out of Scope

- Selective mid-session namespace cancellation (withdraw a specific namespace without closing the session) — that requires a separate API surface and is not needed now.
- FIN-based graceful half-close on write side — a follow-on; the current approach uses RESET_STREAM for all cleanup.
- Draft-14/16 paths — these use the control stream, not per-request bidi streams; no change needed.
