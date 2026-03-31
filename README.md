# OpenMOQ Publisher

`moqxr` is a C++20 OpenMOQ publisher contribution project for Linux and macOS.

It packages MP4 input into CMSF-style publishable objects, supports MOQT draft-specific framing for drafts 14 and 16, and can either inspect the generated publish plan locally or publish it over a picoquic-backed transport when local `picoquic` and `picotls` checkouts are available.

## Overview

- Parses fragmented MP4 input with `ftyp` + `moov` + `moof`/`mdat`
- Remuxes progressive MP4 input into synthesized fragmented media objects
- Extracts track metadata and RFC 6381 codec identifiers from MP4 sample tables
- Preserves HEVC track signaling and normalizes `hev1` to `hvc1` when samples do not carry in-band parameter sets
- Builds a publish plan with catalog, SAP event timeline, and media objects
- Emits generated objects and catalog metadata to disk for inspection
- Supports a configurable track namespace, optional paced publication, and draft-aware MOQT control/object encoding
- Can optionally add per-track SAP event timeline metadata with a CLI flag
- Includes packaging, CLI, and MOQT session tests through CTest

## Design overview

### Fragmented MP4 fast path

For already fragmented input:

- `ftyp` + `moov` are reused as the initialization segment
- by default, each fragmented input is split into lower-latency MOQT media objects within the same group when per-sample boundaries can be derived
- `--coalesce-cmaf-chunks` keeps the older one-object-per-fragment behavior
- the pipeline preserves source-byte spans until optional file emission

### Progressive MP4 remux path

For non-fragmented input:

- the tool reads `stbl` tables such as `stsz`, `stsc`, `stco` or `co64`, `stts`, and optional `ctts` and `stss`
- it synthesizes a fragmented initialization segment by adding `mvex` and `trex`
- it builds synthetic `moof` + `mdat` payloads from the original sample data
- by default, progressive remux output is split into multiple MOQT objects per group for lower latency

This keeps the project aligned with CMAF-style publication while reusing the same publish-plan model for local inspection and transport-driven publication.

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
- `.github/workflows/release.yml`: GitHub Actions release-build workflow that uploads Linux and macOS archives

## Release builds

For users who just want a prebuilt binary, GitHub Actions publishes release archives for Linux and macOS:

- pushing a `v*` tag builds release artifacts and attaches them to the matching GitHub Release
- running the `Release Builds` workflow manually uploads the same archives as workflow artifacts
- manual runs can also publish a GitHub Release when you provide a `release_tag` such as `v0.1.0`
- both CI and release workflows check out `private-octopus/picoquic` plus `private-octopus/picotls`, so published binaries include the picoquic transport path instead of falling back to a local-inspection-only build

## Build

### Baseline build

This is the default path for local development:

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

## Quick Start

If you already have a sample MP4 and just want to see what the publisher does, these are the most useful first commands:

Inspect the publish plan with the default settings:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Inspect the same input with SAP event timeline metadata enabled:

```bash
./build/openmoq-publisher --input sample.mp4 --sap --dump-plan
```

Emit the catalog and media objects to disk:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Stream the input over stdin instead of reading it from a file path:

```bash
cat sample.mp4 | ./build/openmoq-publisher --input - --dump-plan
```

## How To Test

### Packaging and session tests

For routine development, run the packaging, CLI, and transport tests from the default build directory:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

This covers:

- fragmented MP4 packaging
- progressive MP4 remux into CMAF-style objects
- CLI option parsing and validation
- MOQT setup encoding and decoding
- binary namespace announcement plus subscribe-serving or forward-publish control/object sequencing
- multitrack subscribe serving in publish-plan/media-time order rather than draining one track at a time
- paced send scheduling against fragment media timestamps
- QUIC varint boundary coverage

Publish-plan numbering note:

- `group_id` is allocated per track, not across all tracks, so interleaved audio and video fragments can both use `0, 1, 2, ...`
- by default, `object_id` advances within a group when CMAF content is split into multiple MOQT objects for lower latency
- `--coalesce-cmaf-chunks` forces `object_id = 0` for the current one-object-per-group fallback
- SAP event timeline tracks are disabled by default; add `--sap` when you want catalog and metadata objects for `*_sap` tracks

If you want the packaging and transport tests without the CLI target, use the secondary build tree:

```bash
cmake -S . -B build-nosmoke -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build-nosmoke
ctest --test-dir build-nosmoke --output-on-failure
```

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

Input source note:

- `--input <path>` reads an MP4 from a regular file
- `--input -` reads the MP4 byte stream from standard input, which allows `cat`, `ffmpeg`, or other producer pipelines to feed the publisher directly
- fragmented and progressive MP4 inputs are both supported through either source type
- SAP event timeline tracks are disabled by default; add `--sap` to include `*_sap` metadata tracks and objects in the publish plan

Use `--dump-plan` to inspect the generated publish plan without touching the network:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 14 --dump-plan
```

Include SAP event timeline output explicitly:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 14 --sap --dump-plan
```

The difference is:

- default output includes the `catalog` object plus media objects
- `--sap` additionally creates `*_sap` metadata tracks and objects
- this affects both `--dump-plan` output and emitted files under `--emit-dir`

Use stdin when the source is already being produced by another command:

```bash
cat sample.mp4 | ./build/openmoq-publisher --input - --draft 14 --dump-plan
```

For example, you can inspect an ffmpeg-produced fragmented stream without writing an intermediate file:

```bash
ffmpeg -i input.mp4 \
  -map 0:v -map 0:a \
  -c:v copy \
  -c:a copy \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 - | ./build/openmoq-publisher --input - --draft 14 --dump-plan
```

Use `--emit-dir` to inspect the emitted catalog and media objects on disk:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 14 --emit-dir out/
```

Emit the same plan with SAP metadata enabled:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 14 --sap --emit-dir out/
```

The output directory should contain:

- `catalog.json`
- one `*_init.mp4` file per media track
- one `*_media.mp4` file per emitted media object
- one `*_probe.mp4` file per emitted media object for direct `ffprobe` use
- `publish-plan.txt`

When `--sap` is enabled, the output directory also contains:

- one `*_sap_g*_o*.json` file per emitted SAP event timeline object

The catalog format includes:

- `role` with values such as `video` and `audio`
- RFC 6381 `codec` strings such as `avc1.64000C`, `mp4a.40.2`, and HEVC values like `hvc1.1.6.L90.B0`
- `renderGroup` and `isLive`
- SAP event timeline tracks with `packaging: "eventtimeline"` and `eventType: "org.ietf.moq.cmsf.sap"`
- `width` and `height` for video tracks
- `sampleRate` and `channelCount` for audio tracks
- base64-encoded per-track CMAF initialization segment (`ftyp` + `moov`) in `initData`

HEVC-specific behavior:

- compact HEVC RFC 6381 codec strings are derived from the track `hvcC` box
- `hev1` tracks that do not carry in-band VPS, SPS, or PPS NAL units are normalized to `hvc1`
- when in-band HEVC parameter sets are present, the publisher preserves `hev1`
- emitted initialization segments are rewritten to match the normalized sample entry type, so catalog metadata and init segments stay aligned

When `--sap` is enabled, `catalog.json` also includes:

- one `*_sap` track per media track
- `packaging: "eventtimeline"`
- `eventType: "org.ietf.moq.cmsf.sap"`
- `depends` pointing back to the corresponding media track

### Relay interoperability test

To attempt a live publish against a relay:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://interop-relay.cloudflare.mediaoverquic.com:443/moq \
  --namespace interop \
  --forward 0 \
  --timeout 10 \
  --paced \
  --insecure
```

Publish the same stream with SAP timeline tracks included:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://interop-relay.cloudflare.mediaoverquic.com:443/moq \
  --namespace interop \
  --forward 0 \
  --timeout 10 \
  --paced \
  --sap \
  --insecure
```

If you need to connect to a relay by IP while still presenting the relay hostname in TLS SNI:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint 203.0.113.10:443 \
  --sni relay.example.com \
  --namespace interop \
  --forward 0 \
  --timeout 10 \
  --insecure
```

Behavior notes:

- `--forward 0` waits for inbound `SUBSCRIBE` requests before sending matching media objects
- with `--forward 0`, subscribers are still expected to request tracks explicitly; by default that includes subscribing to `catalog` if they need track discovery
- `--publish-catalog` keeps `--forward 0` for media tracks but proactively publishes the `catalog` track through the normal `PUBLISH` / `PUBLISH_OK` path so downstream consumers can discover available tracks without first subscribing to `catalog`
- `--sap` adds per-track `*_sap` event timeline tracks and metadata objects; by default those tracks are not created
- media packaging defaults to lower-latency split MOQT objects per group when chunk/sample boundaries are available
- `--coalesce-cmaf-chunks` disables that split and falls back to one media object per group
- when multiple tracks are subscribed, matching objects are served in publish-plan order so time-aligned audio/video stay interleaved instead of draining one track before the next
- `--forward 1` proactively publishes tracks and objects after namespace setup completes
- `--timeout <seconds>` controls how long the publisher waits for inbound `SUBSCRIBE` requests; the default is 3 seconds
- `--sni <value>` overrides the TLS SNI sent to the relay, which is useful when `--endpoint` uses a raw IP address
- `--paced` applies pacing only to media-object sends; setup and publish control messages are sent immediately

### Optional picoquic smoke test

There is an in-repo loopback smoke test target for the picoquic transport path, but it is disabled by default:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=ON
cmake --build build --target openmoq-publisher-picoquic-smoke-tests
ctest --test-dir build --output-on-failure
```

The smoke test requires an environment that permits real UDP sockets. Keep it disabled for routine packaging or session work, and run the smoke binary directly when validating transport changes.

## Usage

Inspect the publish plan for an already fragmented MP4:

```bash
./build/openmoq-publisher --input sample-fragmented.mp4 --draft 14 --dump-plan
```

Inspect the same fragmented MP4 with SAP metadata enabled:

```bash
./build/openmoq-publisher --input sample-fragmented.mp4 --draft 14 --sap --dump-plan
```

Emit object payloads for a progressive MP4 after remux:

```bash
./build/openmoq-publisher --input sample-progressive.mp4 --draft 14 --emit-dir out/
```

Emit a progressive MP4 after remux with SAP metadata enabled:

```bash
./build/openmoq-publisher --input sample-progressive.mp4 --draft 14 --sap --emit-dir out/
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
  --timeout 3 \
  --paced \
  --insecure
```

If you want subscribers or downstream tools to see SAP event timeline tracks, add `--sap`:

```bash
./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint localhost:4433 \
  --namespace media \
  --forward 0 \
  --timeout 3 \
  --paced \
  --sap \
  --insecure
```

The same CLI accepts stdin for transport publishing as well:

```bash
cat sample.mp4 | ./build/openmoq-publisher \
  --input - \
  --endpoint localhost:4433 \
  --namespace media \
  --forward 0 \
  --timeout 3 \
  --paced \
  --insecure
```

Chunk/object mapping:

- default behavior is lower-latency split publication, which emits multiple MOQT objects in the same group when CMAF chunk/sample boundaries are available
- `--coalesce-cmaf-chunks` restores one media object per group

SAP metadata:

- by default the catalog only includes media tracks plus the top-level `catalog` object
- `--sap` adds per-track `*_sap` event timeline entries to the catalog and emits matching metadata objects

ALPN selection:

- draft-14 defaults to `moq-00`
- draft-16 defaults to `moqt-16`
- `--alpn` overrides either default when you need to target a specific relay

Catalog note:

- `catalog.json` uses the CMSF-style `role` field such as `video` and `audio`
- with `--sap`, `catalog.json` also advertises per-track SAP event timeline tracks using CMSF `eventtimeline` metadata
- `publish-plan.txt` and `--dump-plan` still print an internal debug `kind=` label for object type (`catalog`, `metadata`, or `media`); that debug label is not part of the catalog spec

## Creating Fragmented MP4 with FFmpeg

The publisher’s preferred fast path is already fragmented MP4 input. You can generate that with `ffmpeg` by copying compatible AAC-LC, H.264, or HEVC streams and enabling CMAF-style fragmentation flags:

```bash
ffmpeg -i input.mp4 \
  -map 0:v -map 0:a \
  -map_metadata -1 \
  -sn -dn \
  -c:v copy \
  -c:a copy \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 fragmented.mp4
```

If the source codecs are not already compatible, re-encode instead of copying. For example (`h264` and `hevc`):

```bash
ffmpeg -i bbb_sunflower_2160p_60fps_normal.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -map_metadata -1 \
  -sn -dn \
  -c:v libx264 -preset medium -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -movflags +frag_keyframe+empty_moov+default_base_moof \
  -f mp4 sunflower-frag.mp4

ffmpeg -i bbb_sunflower_2160p_60fps_normal.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -map_metadata -1 \
  -sn -dn \
  -c:v libx265 -preset medium -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -movflags +frag_keyframe+empty_moov+default_base_moof \
  -f mp4 sunflower265-frag.mp4
```

Practical notes:

- `-map 0:v -map 0:a` keeps only video and audio streams, excluding subtitle and other non-A/V tracks
- `-map 0:v:0 -map 0:a:0` uses first audio stream if multiple exist
- `-sn -dn` explicitly disables subtitle and data or text streams
- `-map_metadata -1` drops container-level metadata from the output
- `+frag_keyframe` starts a new fragment on keyframes
- `+empty_moov` writes initialization metadata up front
- `+default_base_moof` and `+separate_moof` produce a layout that is easier for fragmented-MP4 pipelines to consume
- for HEVC, prefer streams that are already `hvc1`-compatible; if a source is tagged `hev1` but keeps VPS/SPS/PPS only in the init segment, the publisher will normalize the advertised codec and emitted init segment to `hvc1`
- if HEVC samples include in-band parameter sets, the publisher preserves `hev1` because rewriting those samples would be incorrect
- if you start from a progressive MP4, this project can remux it internally, but pre-fragmented input is still the simpler and more efficient path

## CI

GitHub Actions is configured to build and test the project on:

- `ubuntu-latest`
- `macos-latest`

The workflow runs the same CMake configure, build, and CTest steps on both platforms.

## Transport Notes

The repository includes a transport abstraction and a picoquic-backed client wrapper.

- if local picoquic and picotls source trees are available, CMake can compile the real picoquic transport path into this project
- if those dependencies are not available, the project still builds and tests normally, and the transport layer falls back cleanly
- the session layer uses a draft-aware control-message module instead of ad hoc string formatting
- the optional loopback smoke test is the intended local validation path for real QUIC transport changes

## Roadmap

1. Add a real MOQT transport publisher over QUIC or WebTransport.
2. Validate generated object layouts against interoperable OpenMOQ endpoints.
3. Expand progressive remux coverage for more sample-table layouts and edit-list cases.
4. Refine CMSF metadata generation as the packaging draft evolves.
