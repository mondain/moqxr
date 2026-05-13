# Draft-18 Implementation Plan

This plan tracks adding `draft-ietf-moq-transport-18` support to `moqxr`.

Reference reviewed: <https://datatracker.ietf.org/doc/html/draft-ietf-moq-transport-18> (12 May 2026).

## Goals

1. Add draft-18 as a selectable profile in CLI and API.
2. Preserve existing draft-14 and draft-16 behavior.
3. Incrementally introduce draft-18 wire semantics with tests at each phase.

## Current Status

- Completed:
  - Phase A: version plumbing (`kDraft18`, CLI/API selection, ALPN defaults).
  - Phase B: draft-18 codec support in control messages:
    - `SETUP` (`0x2F00`) handling.
    - setup options parsing/encoding for `PATH` and `AUTHORITY`.
    - draft-18 `REQUEST_OK` / `REQUEST_ERROR` parsing support.
    - dedicated control-message tests.
  - Phase C (partial): request-stream semantics in session layer:
    - per-request bidirectional stream request/response flow for namespace/publish paths.
    - strict response correlation on request streams, including:
      - unexpected response type rejection
      - GOAWAY rejection on request stream
      - duplicate terminal response rejection
    - legacy (draft-14/16) shared-control-stream publish response ID validation hardening.
- Remaining:
  - Complete Phase C across all request categories used by this publisher.
  - Phase D data-stream/object framing alignment and additional draft-18 conformance checks.
  - Phase E interop and production hardening.

## Phase A: Version Plumbing (Low Risk)

Files:

- `include/openmoq/publisher/moq_draft.h`
- `src/moq_draft.cpp`
- `src/cli_options.cpp`
- `src/publisher_api.cpp`
- `tests/cli_options_test.cpp`

Deliverables:

1. `kDraft18` enum value and string conversion.
2. ALPN default selection for raw QUIC (`moqt-18`).
3. CLI accepts `--draft 18`.
4. API maps WebTransport protocol offer for draft-18.
5. Basic tests for draft parsing.

## Phase B: Control Message Codec for Draft-18

Primary files:

- `include/openmoq/publisher/transport/moqt_control_messages.h`
- `src/transport/moqt_control_messages.cpp`

Work:

1. Add draft-18 message constants:
   - `SETUP` message type `0x2F00`.
2. Implement draft-18 `SETUP` option KVP handling:
   - `PATH (0x01)`
   - `AUTHORITY (0x05)`
   - Option parsing model that ignores unknown setup options.
3. Add draft-18 `REQUEST_OK (0x07)` and `REQUEST_ERROR (0x05)` codec support.
4. Introduce tests for message length/parsing and parameter handling.

## Phase C: Session Request-Stream Semantics

Primary file:

- `src/transport/moqt_session.cpp`

Work:

1. Add draft-18 request stream lifecycle:
   - one bidirectional stream per request type (`PUBLISH`, `SUBSCRIBE_NAMESPACE`, etc.).
2. Read responses (`REQUEST_OK` / `REQUEST_ERROR`) on matching request stream.
3. Keep existing control stream behavior for draft-14/16.
4. Add per-request state bookkeeping and robust stream-close handling.

Status:

- Implemented for publish namespace and publish-track handshake paths.
- Includes strict request-stream response validation and duplicate-response rejection.
- Remaining stream choreography work is focused on full request coverage and expanded state tracking.

## Phase D: Data Stream/Object Framing Alignment

Primary files:

- `src/transport/moqt_control_messages.cpp`
- `src/transport/moqt_session.cpp`

Work:

1. Validate/align subgroup header flags for draft-18 (`0b0XX1XXXX` family).
2. Add explicit handling for `FIRST_OBJECT` semantics.
3. Confirm object ID delta handling and end-of-group behavior.
4. Add regression tests with malformed inputs and expected protocol errors.

## Phase E: Interop + Hardening

Work:

1. Add an interop checklist document for at least one draft-18 relay target.
2. Capture failure taxonomy and map to existing `TransportStatus` messages.
3. Confirm no regressions in:
   - draft-14 test flows
   - draft-16 test flows
   - WebTransport connect flows

## Test Strategy

1. Unit tests for codec and message validation in `moqt_control_messages`.
2. Session-level tests in `tests/moqt_session_test.cpp` for request-stream choreography.
3. End-to-end sanity using existing build + ctest workflow.

## Risks

1. Architectural mismatch: current request ID flow vs draft-18 per-request streams.
2. Parameter parsing strictness causing interop breakage if not version-gated.
3. Unintended regressions to draft-14/draft-16 behavior.

## Mitigations

1. Strict version gates around all draft-18-only behavior.
2. Land in small commits with tests per step.
3. Preserve legacy paths untouched where practical.
