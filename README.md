# OpenMOQ Publisher

`moqxr` is a C++20 contribution project for an OpenMOQ publisher targeting Linux and macOS.

The current codebase focuses on the media packaging side of a publisher:

- primary MOQT behavior modeled after `draft-ietf-moq-transport-14`
- secondary compatibility surface for `draft-ietf-moq-transport-16`
- MP4 ingest for AAC-LC or Opus audio and H.264 or H.265 video
- CMSF-oriented object planning for MOQT publication

It is buildable and testable today, but it is not yet a full end-to-end network publisher.

## Current capabilities

- Parses fragmented MP4 input with `ftyp` + `moov` + `moof`/`mdat`
- Remuxes non-fragmented MP4 input into synthesized fragmented media objects
- Extracts basic track metadata and codec identifiers from MP4 sample tables
- Builds a publish plan consisting of initialization and media objects
- Emits planned objects to disk for inspection
- Keeps fragmented input on a zero-copy fast path where possible
- Isolates MOQT draft-version mapping from the media packaging code

## Current limitations

- No QUIC or WebTransport transport session yet
- Progressive MP4 remux support is intentionally narrow
- Edit lists, richer interleaving cases, and broader timing edge cases are not fully handled yet
- The current remux path synthesizes fragments from `stbl` sample tables but does not attempt a full general-purpose MP4 muxer implementation

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
- `.github/workflows/ci.yml`: GitHub Actions build and test workflow for Linux and macOS

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

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

## Contributing

See [CONTRIBUTING.md](/media/mondain/terrorbyte/workspace/github/moqxr/CONTRIBUTING.md) for contribution expectations and development notes.

## Roadmap

1. Add a real MOQT transport publisher over QUIC or WebTransport.
2. Validate generated object layouts against interoperable OpenMOQ endpoints.
3. Expand progressive remux coverage for more sample-table layouts and edit-list cases.
4. Refine CMSF metadata generation as the packaging draft evolves.
