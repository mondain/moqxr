# OpenMOQ Publisher

Greenfield C++20 contribution project for an OpenMOQ publisher that targets Linux and macOS.

Current focus:

- Primary MOQT behavior modeled after `draft-ietf-moq-transport-14`
- Secondary compatibility surface for `draft-ietf-moq-transport-16`
- MP4 ingest for AAC-LC or Opus audio and H.264 or H.265 video
- CMSF-oriented packaging with a zero-copy bias from ISO BMFF boxes into initialization/media objects

## Status

This repository currently provides a buildable publisher core and CLI for:

- Parsing fragmented MP4 input (`ftyp` + `moov` + `moof`/`mdat`)
- Detecting common sample-entry codecs from `stsd`
- Building a CMSF-style object plan without remuxing media payloads
- Emitting initialization and media objects to disk for inspection
- Capturing draft-14 vs draft-16 mapping decisions in one place

It does **not** yet include a QUIC/WebTransport session implementation. The transport boundary is isolated so a future OpenMOQ transport implementation can publish the generated object stream without reworking the packaging pipeline.

## Design Notes

### Input assumptions

- The fast path assumes **fragmented MP4** input.
- For fragmented input, `moof`/`mdat` pairs are reused directly as media fragments.
- `ftyp` + `moov` are reused directly as the initialization segment.
- Non-fragmented MP4 is rejected for now because generating compliant CMAF fragments would require a full remux path.

### Packaging path

The project avoids unnecessary copies:

- Source bytes are loaded once.
- Parsed boxes are represented as spans into the original buffer.
- Initialization and media objects reference those spans until optional emission to files.

### Project layout

- `include/openmoq/publisher`: public library headers
- `src`: library and CLI implementation
- `tests`: lightweight CTest-based coverage
- `docs/protocol-mapping.md`: draft-14/draft-16 and CMAF packaging notes

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Usage

```bash
./build/openmoq-publisher --input sample-fragmented.mp4 --draft 14 --dump-plan
./build/openmoq-publisher --input sample-fragmented.mp4 --draft 16 --emit-dir out/
```

## Roadmap

1. Add a real MOQT transport publisher over QUIC/WebTransport.
2. Expand CMSF metadata generation once the draft stabilizes further.
3. Add optional remux support for non-fragmented MP4 sources.
4. Validate object layout against interoperable OpenMOQ endpoints.
