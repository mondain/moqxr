# OpenMOQ Publisher

`moqxr` is a C++20 contribution project for an OpenMOQ publisher targeting Linux and macOS.

The current codebase focuses on the media packaging side of a publisher:

- primary MOQT behavior modeled after `draft-ietf-moq-transport-14`
- secondary compatibility surface for `draft-ietf-moq-transport-16`
- MP4 ingest for AAC-LC or Opus audio and H.264 or H.265 video
- CMSF-oriented object planning for MOQT publication

It is buildable and testable today, but it is not yet a full interoperable MOQT publisher.

## Current capabilities

- Parses fragmented MP4 input with `ftyp` + `moov` + `moof`/`mdat`
- Remuxes non-fragmented MP4 input into synthesized fragmented media objects
- Extracts track metadata and RFC 6381 codec identifiers from MP4 sample tables
- Builds a publish plan consisting of initialization and media objects
- Emits planned objects to disk for inspection
- Keeps fragmented input on a zero-copy fast path where possible
- Isolates MOQT draft-version mapping from the media packaging code
- Builds a picoquic-backed QUIC transport path when local `picoquic` and `picotls` checkouts are available
- Publishes a draft-aware control stream plus per-object streams in the current session layer
- Supports a configurable published track namespace for relay and interop testing
- Supports optional paced publication using fragment media timestamps

## Current limitations

- External relay interoperability is still incomplete
- Progressive MP4 remux support is intentionally narrow
- Edit lists, richer interleaving cases, and broader timing edge cases are not fully handled yet
- The current remux path synthesizes fragments from `stbl` sample tables but does not attempt a full general-purpose MP4 muxer implementation
- Current external relay tests complete setup and namespace announcement for draft-14, but do not yet result in inbound subscriptions or a complete draft-16 session

## Design overview

### Fragmented MP4 fast path

For already fragmented input:

- `ftyp` + `moov` are reused as the initialization segment
- each `moof`/`mdat` pair is reused directly as a media object payload
- the pipeline preserves source-byte spans until optional file emission

### Progressive MP4 remux path

For non-fragmented input:

- the tool reads `stbl` tables such as `stsz`, `stsc`, `stco` or `co64`, `stts`, and optional `ctts` and `stss`
- it synthesizes a fragmented initialization segment by adding `mvex` and `trex`
- it builds synthetic `moof` + `mdat` payloads from the original sample data

This keeps the project aligned with CMAF-style publication while avoiding unnecessary redesign later when transport is added.

### Draft handling

- `draft-ietf-moq-transport-14` is the primary target
- `draft-ietf-moq-transport-16` is represented as a secondary compatibility profile
- draft-specific assumptions are documented in [docs/protocol-mapping.md](/media/mondain/terrorbyte/workspace/github/moqxr/docs/protocol-mapping.md)

## Repository layout

- `include/openmoq/publisher`: public headers
- `src`: library and CLI implementation
- `tests`: CTest-based unit coverage
- `docs`: protocol notes and design references
- `docs/transport-plan.md`: picoquic integration plan and implementation checklist
- `.github/workflows/ci.yml`: GitHub Actions build and test workflow for Linux and macOS

## Build

### Baseline build

This is the default path if you only want the packaging and session-layer code:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

### Build with local picoquic and picotls

If you have local checkouts at:

- `/media/mondain/terrorbyte/workspace/github/picoquic`
- `/media/mondain/terrorbyte/workspace/github/picotls`

then the project will automatically compile against them.

Required picotls setup:

```bash
git -C /media/mondain/terrorbyte/workspace/github/picotls submodule update --init --recursive
```

Then configure and build normally:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful CMake options:

- `-DOPENMOQ_ENABLE_PICOQUIC=ON|OFF`
- `-DOPENMOQ_PICOQUIC_SOURCE_DIR=/path/to/picoquic`
- `-DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=ON|OFF`

## How To Test

### Packaging and session tests

For routine development, run the packaging and session-layer tests:

```bash
cmake -S . -B build-nosmoke -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build-nosmoke
ctest --test-dir build-nosmoke --output-on-failure
```

This covers:

- fragmented MP4 packaging
- progressive MP4 remux into CMAF-style objects
- MOQT setup encoding and decoding
- binary namespace announcement plus subscribe-serving or forward-publish control/object sequencing
- paced send scheduling against fragment media timestamps
- QUIC varint boundary coverage

### Picoquic loopback smoke test

When you want to exercise the live QUIC path locally, enable the smoke test target:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=ON
cmake --build build --target openmoq-publisher-picoquic-smoke-tests
./build/openmoq-publisher-picoquic-smoke-tests
```

For transport debugging, run it with tracing enabled:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher-picoquic-smoke-tests
```

### CLI dry-run testing

Use `--dump-plan` to inspect the generated publish plan without touching the network:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 14 --dump-plan
```

Use `--emit-dir` to inspect the emitted catalog and media objects on disk:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 14 --emit-dir out/
```

The output directory should currently contain:

- `catalog.json`
- one `*_init.mp4` file per media track
- one `*_media.mp4` file per emitted media object
- one `*_probe.mp4` file per emitted media object for direct `ffprobe` use
- `publish-plan.txt`

The current catalog format includes:

- `role` with values such as `video` and `audio`
- RFC 6381 `codec` strings such as `avc1.64000C` and `mp4a.40.2`
- `renderGroup` and `isLive`
- `width` and `height` for video tracks
- `sampleRate` and `channelCount` for audio tracks
- base64-encoded codec `initData` per track

### Relay interoperability test

To attempt a live publish against a relay:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://interop-relay.cloudflare.mediaoverquic.com:443/moq \
  --namespace interop \
  --forward 0 \
  --paced \
  --insecure
```

Current status as of March 13, 2026:

- QUIC handshake succeeds against `draft-14.cloudflare.mediaoverquic.com:443`, `interop-relay.cloudflare.mediaoverquic.com:443`, and `moq-relay.red5.net:8443`
- `CLIENT_SETUP` succeeds and the client prints the negotiated connection ID to stdout after setup
- `PUBLISH_NAMESPACE` is accepted with `PUBLISH_NAMESPACE_OK`
- with `--forward 0`, the current client waits for inbound `SUBSCRIBE`; relays may consume `SUBSCRIBE_NAMESPACE` themselves and only forward `SUBSCRIBE` to the publisher
- the Cloudflare endpoints accepted setup and namespace announce in testing, but did not issue subscriptions, so the publish attempt timed out waiting for control-stream data
- with `--forward 1`, `moq-relay.red5.net:8443` now progresses through `PUBLISH_OK` for the catalog and media tracks, after which the client begins sending object streams
- `fb.mvfst.net:9448` now accepts the draft-14 publish flow end-to-end after switching `PUBLISH`, `PUBLISH_OK`, and `PUBLISH_ERROR` control messages to `u16` outer lengths; the current draft-16 flow is still rejected with MOQT application error `3` (`PROTOCOL_VIOLATION`) immediately after setup
- `--paced` only affects media-object sends; it does not delay setup, namespace announce, or track publish requests

### Optional picoquic smoke test

There is an in-repo loopback smoke test target for the picoquic transport path, but it is disabled by default:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=ON
cmake --build build --target openmoq-publisher-picoquic-smoke-tests
ctest --test-dir build --output-on-failure
```

Current status:

- the smoke test passes when run in an environment that allows real UDP sockets
- restricted sandboxes can still fail early during socket setup
- keep this option off for routine packaging-only development, and run the smoke binary directly when validating transport changes

## Usage

Inspect the publish plan for an already fragmented MP4:

```bash
./build/openmoq-publisher --input sample-fragmented.mp4 --draft 14 --dump-plan
```

Emit object payloads for a progressive MP4 after remux:

```bash
./build/openmoq-publisher --input sample-progressive.mp4 --draft 14 --emit-dir out/
```

Try the draft-16 compatibility profile:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 16 --dump-plan
```

Transport-oriented CLI flags are also present now:

```bash
./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint localhost:4433 \
  --namespace media \
  --forward 0 \
  --paced \
  --insecure
```

ALPN selection:

- draft-14 defaults to `moq-00`
- draft-16 defaults to `moqt-16`
- `--alpn` overrides either default when you need to target a specific relay

Current status:

- the packaging pipeline is fully usable today
- the session layer now emits typed control messages for setup, namespace publication, and subscription servicing
- `--endpoint` now enters the real picoquic-backed transport path when the project is built with local picoquic and picotls support
- the local picoquic loopback handshake works, including object publication over QUIC streams
- `--namespace` lets you choose the advertised track namespace during transport tests
- `--forward 0|1` selects whether the publisher waits for `SUBSCRIBE` (`0`) or immediately sends `PUBLISH` requests and forwards objects after namespace announce (`1`)
- ALPN is selected from the requested draft unless `--alpn` explicitly overrides it
- `--paced` delays media-object sends to match fragment media timestamps instead of sending the whole file as fast as possible; it only has an effect once object transmission begins
- after setup completes, the CLI prints `connection_id=<hex>` to stdout
- interoperability against external relays is partially working at draft-14 setup and namespace announcement, but not yet at end-to-end subscription delivery and not yet complete for draft-16 setup

Catalog note:

- `catalog.json` uses the CMSF-style `role` field such as `video` and `audio`
- `publish-plan.txt` and `--dump-plan` still print an internal debug `kind=` label for object type (`catalog` vs `media`); that debug label is not part of the catalog spec

## Creating Fragmented MP4 with FFmpeg

The publisher’s preferred fast path is already fragmented MP4 input. You can generate that with `ffmpeg` by copying compatible AAC-LC or H.264 streams and enabling CMAF-style fragmentation flags:

```bash
ffmpeg -i input.mp4 \
  -c:v copy \
  -c:a copy \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 fragmented.mp4
```

If the source codecs are not already compatible, re-encode instead of copying. For example:

```bash
ffmpeg -i input.mov \
  -c:v libx264 -preset medium -g 48 -keyint_min 48 \
  -c:a aac -profile:a aac_low -b:a 128k \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 fragmented.mp4
```

Practical notes:

- `+frag_keyframe` starts a new fragment on keyframes
- `+empty_moov` writes initialization metadata up front
- `+default_base_moof` and `+separate_moof` produce a layout that is easier for fragmented-MP4 pipelines to consume
- if you start from a progressive MP4, this project can remux it internally, but pre-fragmented input is still the simpler and more efficient path

## CI

GitHub Actions is configured to build and test the project on:

- `ubuntu-latest`
- `macos-latest`

The workflow currently runs the same CMake configure, build, and CTest steps on both platforms.

## Picoquic status

The repository now includes a transport abstraction and a picoquic-backed client wrapper.

- if local picoquic and picotls source trees are available, CMake can compile the real picoquic transport path into this project
- if those dependencies are not available, the project still builds and tests normally, and the transport layer falls back cleanly
- in this workspace, picoquic and picotls compile successfully as subprojects
- the loopback smoke test now completes successfully when run outside restricted sandboxes
- the current session layer uses a draft-aware control-message module instead of ad hoc string formatting
- the next remaining transport step is external interoperability, not local handshake bring-up

## Roadmap

1. Add a real MOQT transport publisher over QUIC or WebTransport.
2. Validate generated object layouts against interoperable OpenMOQ endpoints.
3. Expand progressive remux coverage for more sample-table layouts and edit-list cases.
4. Refine CMSF metadata generation as the packaging draft evolves.
