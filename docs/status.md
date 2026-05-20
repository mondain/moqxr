# Project Status and Roadmap

## Current Status

Current transport and packaging work is past the initial prototype stage. The publisher can generate publish plans, publish over picoquic-backed Raw QUIC and WebTransport paths, and exercise the supported MOQT draft profiles.

Draft status:

- draft 14 is the primary target
- draft 16 is maintained as a secondary compatibility profile
- draft 18 support is implemented for version selection, setup/request framing codec paths, and request-stream response correlation
- draft 18 interop hardening is still in progress

## Roadmap

1. Keep draft-14, draft-16, and draft-18 message/subgroup serde tests aligned with the current relay matrix so wire placement regressions are caught in CI.
2. Harden draft-18 request-stream lifecycle behavior, especially stream retention, close/reset behavior, and relay-specific response timing.
3. Continue Red5 relay and player interop validation, including catalog publication, downstream subscription discovery, and Red5 Pro playback behavior.
4. Expand CMAF packaging coverage for fragmented and progressive inputs, including Opus, AAC, H.264, HEVC, edit lists, and less common sample-table layouts.
5. Track CMSF catalog/schema changes and keep emitted catalog metadata, init segments, SAP timeline tracks, and per-track roles aligned with the current packaging draft.
6. Add DRM/CENC-aware packaging support: detect and preserve encrypted CMAF boxes such as `sinf`, `tenc`, `pssh`, `saiz`, `saio`, and `senc`, expose the needed catalog signaling, and validate encrypted sample forwarding without attempting decryption.
7. Create an M2TS packaging example based on `draft-gregoire-moq-msfts-00`, using the draft's `m2ts` packaging value to carry MPEG-2 Transport Stream or M2TS source packets directly over MOQT.
8. Keep Linux, macOS, and Windows CI/release builds green, including the psychedelic FFmpeg live-publisher example.
