# Picoquic Transport Plan

This document turns the current picoquic integration idea into an implementation checklist for `moqxr`.

## Goal

Add a native QUIC transport layer using picoquic so the existing publisher pipeline can send MOQT control and media objects over a real connection without rewriting the packaging code.

## Scope boundaries

The current repository already handles:

- MP4 parsing
- fragmented MP4 fast-path packaging
- progressive MP4 remux into synthetic fragments
- draft-aware publish planning

The picoquic work should add:

- QUIC connection management
- MOQT session setup and control streams
- media object publication over QUIC streams
- transport-level backpressure and error handling

The picoquic work should not initially add:

- HTTP/3
- WebTransport
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
4. Picoquic adapter
   Implements the transport layer on top of picoquic callbacks and sockets.

This separation is important because MOQT draft churn should be isolated to the session layer, while picoquic remains a byte transport.

## Deliverables

### Phase 1: transport seam

- [x] Add a transport interface for connect, open stream, write, close
- [x] Add a session façade that consumes `PublishPlan`
- [x] Add a picoquic client stub that implements the transport interface
- [x] Keep current CLI and packaging flow unchanged

### Phase 2: connection establishment

- [x] Add endpoint configuration: host, port, ALPN
- [x] Add TLS configuration hooks: cert, key, CA, insecure-dev toggle
- [x] Establish a client QUIC connection with picoquic
- [x] Report handshake success and failures cleanly

### Phase 3: MOQT control plane

- [ ] Open a control stream after handshake
- [ ] Implement setup and session negotiation
- [ ] Implement namespace or publish announcement flow
- [ ] Represent draft-14 and draft-16 control-plane differences behind one abstraction

### Phase 4: object publication

- [ ] Publish initialization object first
- [ ] Publish media objects according to `PublishPlan`
- [ ] Decide and document one stream mapping policy
- [ ] Handle transport write backpressure

### Phase 5: observability and testing

- [ ] Add structured logs for handshake, stream lifecycle, and object publication
- [ ] Add unit tests for session-to-transport mapping
- [ ] Add loopback integration tests for transport
- [ ] Add interoperability tests against an OpenMOQ-capable endpoint

## Proposed stream mapping

Initial recommendation:

- one bidirectional control stream per session
- one unidirectional stream per published object in the first implementation

That policy is not necessarily optimal long-term, but it is easier to reason about and validate while the control-plane logic is still moving.

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

## Suggested implementation order

1. Land interface-only scaffolding
2. Add endpoint and TLS config types
3. Implement a connect-only picoquic client
4. Add control stream bring-up
5. Add init object publication
6. Add full object publication from `PublishPlan`
7. Add retry, backpressure, and metrics

## Current repository status

- The transport seam and session façade are implemented.
- CLI flags for endpoint, ALPN, and TLS-related parameters are present.
- The build can integrate local picoquic and picotls source checkouts directly.
- The current workspace now compiles picoquic and picotls successfully.
- A loopback smoke test was added for live handshake validation, but it is not enabled by default because the current handshake attempt still times out.

## Key risks

- draft-14 vs draft-16 control differences leaking into transport code
- coupling picoquic callback state too tightly to publish scheduling
- under-specifying how objects map to streams early on

## Mitigations

- keep draft-specific message encoding in session code
- keep transport status and callbacks generic
- start with a conservative one-object-per-stream policy
- add integration tests before optimizing stream reuse or pacing
