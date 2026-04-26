# Transport Plan

This document turns the current transport work into an implementation checklist for `moqxr`.

## Goal

Add transport modes that let the existing publisher pipeline send MOQT control and media objects over either:

- direct QUIC with raw MOQT ALPN
- WebTransport over HTTP/3, with MOQT draft negotiation happening inside the WebTransport session

## Scope boundaries

The current repository already handles:

- MP4 parsing
- fragmented MP4 fast-path packaging
- progressive MP4 remux into synthetic fragments
- draft-aware publish planning

The transport work should add:

- QUIC connection management
- HTTP/3 and WebTransport session establishment
- MOQT session setup and control streams
- media object publication over QUIC streams
- transport-level backpressure and error handling

The transport work should not initially add:

- broad muxing or repackaging logic
- advanced QUIC features such as migration or multipath

## Architecture target

The intended layering is:

1. Packaging layer
   Uses `ParsedMp4`, `SegmentedMp4`, and `PublishPlan`.
2. Session layer
   Converts a `PublishPlan` into MOQT control and object publication actions.
3. Transport layer
   Provides connection, stream, and write primitives.
4. Transport adapters
   Implement the transport layer on top of:
   - picoquic raw QUIC callbacks and sockets
   - picoquic `h3zero` + WebTransport helpers

This separation is important because MOQT draft churn should stay isolated to the session layer, while raw QUIC and WebTransport remain byte-stream transports.

## Deliverables

### Phase 1: transport seam

- [x] Add a transport interface for connect, open stream, write, close
- [x] Add a session façade that consumes `PublishPlan`
- [x] Add a picoquic client stub that implements the transport interface
- [x] Keep current CLI and packaging flow unchanged
- [x] Add explicit transport selection in CLI/config
- [x] Add a WebTransport client scaffold behind `PublisherTransport`

### Phase 2: raw QUIC connection establishment

- [x] Add endpoint configuration: host, port, ALPN
- [x] Add TLS configuration hooks: cert, key, CA, insecure-dev toggle
- [x] Establish a client QUIC connection with picoquic
- [x] Report handshake success and failures cleanly

### Phase 3: WebTransport connection establishment

- [x] Require explicit transport path for WebTransport endpoints
- [x] Establish client HTTP/3 connection with ALPN `h3` by default
- [x] Bring up a WebTransport CONNECT session using picoquic `h3zero` helpers
- [x] Map WebTransport stream open/read/write operations onto `PublisherTransport`
- [x] Report WebTransport session establishment failures cleanly

### Phase 4: MOQT control plane

- [x] Open a control stream after handshake
- [x] Implement setup and session negotiation scaffolding
- [x] Implement namespace or publish announcement flow
- [x] Represent draft-14 and draft-16 control-plane differences behind one abstraction
- [x] Confirm the same `MoqtSession` flow works unchanged on top of WebTransport streams

### Phase 5: object publication

- [x] Publish initialization object first
- [x] Publish media objects according to `PublishPlan`
- [x] Decide and document one stream mapping policy
- [x] Use picoquic's callback-driven write path for WebTransport application streams
- [ ] Handle transport write backpressure beyond the current callback-driven queue

### Phase 6: observability and testing

- [x] Add detailed trace logs for handshake, stream lifecycle, and object publication
- [x] Add unit tests for session-to-transport mapping
- [x] Add loopback integration tests for transport
- [ ] Add interoperability tests against a raw OpenMOQ endpoint
- [x] Add manual interoperability coverage against WebTransport-capable endpoints

## Stream mapping

Current policy:

- one bidirectional control stream per session
- one unidirectional stream per published object

This remains intentionally conservative. It keeps object boundaries explicit and makes current draft-specific control and subscriber-serving logic easier to validate. Stream reuse is still a possible future optimization once backpressure handling is in place.

## Interface responsibilities

### `PublisherTransport`

Owns:

- connection establishment
- stream creation
- byte writes
- close
- connection state

Does not own:

- MOQT message structure
- media packaging
- object scheduling policy

### `MoqtSession`

Owns:

- setup and control-plane sequencing
- draft-specific control behavior
- translation from `PublishPlan` to transport actions

Does not own:

- raw QUIC callbacks
- MP4 parsing
- object bytes

### `PicoquicClient`

Owns:

- picoquic configuration and lifecycle
- callback bridging
- stream IDs and write execution
- error translation into transport-level status objects

### `WebTransportClient`

Owns:

- HTTP/3 session establishment over picoquic
- WebTransport CONNECT setup using `h3zero` and picoquic WebTransport helpers
- mapping WebTransport streams to transport stream operations
- error translation into transport-level status objects

Does not own:

- MOQT draft semantics
- MOQT control messages
- media packaging

## Suggested implementation order

1. Land transport selection and factory scaffolding
2. Keep raw QUIC defaults stable while separating transport-specific ALPN behavior
3. Implement a connect-only WebTransport client using picoquic `h3zero`
4. Add WebTransport stream mapping
5. Reuse `MoqtSession` on top of the new transport
6. Add interoperability coverage before optimizing backpressure or stream reuse

## Current repository status

- The transport seam and session façade are implemented.
- CLI flags for endpoint, transport, ALPN, and TLS-related parameters are present.
- The build integrates local picoquic, picohttp, and picotls source checkouts directly.
- Raw QUIC and WebTransport publisher transports both build and pass local transport tests.
- The session layer uses a dedicated control-message encoder that keeps draft-14 and draft-16 naming differences out of the transport adapter.
- Subscriber-driven serving is implemented for `--forward 0`, including multitrack publication in publish-plan/media-time order rather than draining one subscribed track completely before the next.
- Incremental downstream `SUBSCRIBE` handling lets later-arriving tracks join future object servicing without restarting the session or losing interleaving for remaining media objects.
- WebTransport mode is implemented on picoquic `h3zero` helpers and uses callback-driven WT app-stream writes instead of direct stream pushes.
- Current live interoperability results:
  - `<moqx-la-relay-host>:4433/moq-relay` and `<moqx-ord-relay-host>:4433/moq-relay` complete verified-TLS draft-16 WebTransport setup and reach `PUBLISH_NAMESPACE_OK`
  - `moq-relay.red5.net:4433/moq` completes verified-TLS draft-16 WebTransport setup, reaches `PUBLISH_NAMESPACE_OK`, accepts a downstream `SUBSCRIBE` for `catalog`, and serves the catalog object
  - `moq-relay.red5.net:4433/moq-relay` rejects WebTransport CONNECT with HTTP `404`; use `/moq` for that relay
  - historical endpoints such as `draft-14.cloudflare.mediaoverquic.com` and `fb.mvfst.net:9448` are retained as reference points, but are not the primary draft-16 validation targets
- After `PUBLISH_NAMESPACE_OK`, an idle `--forward 0` publisher now exits cleanly after the subscriber timeout, emits `PUBLISH_NAMESPACE_DONE`, and does not treat the absence of downstream `SUBSCRIBE` as a publish failure.

## Key risks

- draft-14 vs draft-16 control differences leaking into transport code
- draft-16 message parameter rules drifting from the spec; `SUBSCRIBE_NAMESPACE` and `PUBLISH_OK` have dedicated regression coverage now
- coupling raw QUIC or WebTransport callback state too tightly to publish scheduling
- incomplete backpressure behavior once object volume or pacing pressure increases
- treating H3 ALPN and MOQT draft signaling as the same layer in WebTransport mode

## Mitigations

- keep draft-specific message encoding in session code
- keep transport status and callbacks generic
- keep the current one-object-per-stream policy until backpressure and reuse semantics are explicit
- add integration tests before optimizing stream reuse or pacing
- keep WebTransport session establishment concerns inside `WebTransportClient`, not `MoqtSession`
