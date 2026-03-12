# Contributing

## Scope

This repository currently focuses on a C++20 publisher pipeline for OpenMOQ:

- fragmented MP4 ingest
- CMSF-oriented object planning
- draft-14 primary MOQT behavior
- draft-16 secondary compatibility mapping

Transport integration is intentionally separated from packaging. Contributions should preserve that boundary unless the change explicitly introduces a transport layer.

## Development

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Contribution Guidelines

- Keep changes C++20-compatible on Linux and macOS.
- Prefer incremental changes with tests for parser, segmenter, or packaging behavior.
- Avoid unnecessary media copies in the MP4 to CMSF path.
- Document draft-specific MOQT behavior in [`docs/protocol-mapping.md`](/media/mondain/terrorbyte/workspace/github/moqxr/docs/protocol-mapping.md) when protocol assumptions change.
- If a feature depends on unstable draft behavior, isolate it behind a clearly named abstraction.

## Pull Requests

- Explain the user-visible or interoperability impact.
- Note whether the change affects draft-14, draft-16, or both.
- Include a minimal reproduction or test case when fixing parser or packaging bugs.

