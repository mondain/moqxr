# Protocol Mapping Notes

This project keeps `draft-ietf-moq-transport-14` as the primary publisher profile and treats `draft-ietf-moq-transport-16` as a secondary compatibility target.

## Draft-14 primary assumptions

- Namespace subscription responses are modeled as the dedicated `SUBSCRIBE_NAMESPACE_OK` and `SUBSCRIBE_NAMESPACE_ERROR` flow.
- Namespace overlap handling is documented against `NAMESPACE_PREFIX_OVERLAP`.
- Publisher-side namespace acceptance is modeled with draft-14 style `PUBLISH_NAMESPACE_OK` and `PUBLISH_NAMESPACE_ERROR`.
- Draft-14 control messages use a `u16` outer `Length` field, including `PUBLISH`, `PUBLISH_OK`, and `PUBLISH_ERROR`; only inner fields explicitly marked `(i)` remain QUIC varints.

## Draft-16 secondary assumptions

- Some request/response handling moved to the generic `REQUEST_OK` and `REQUEST_ERROR` flow.
- `SUBSCRIBE_NAMESPACE` carries `Subscribe Options`, which affects whether namespace advertisements, publish advertisements, or both are requested.
- Namespace overlap handling is expressed through the generic request error path rather than the older dedicated response message family.

## CMAF packaging assumptions

- Initialization data is represented either as a binary payload or as a dedicated MOQT track with one group and one object.
- Media objects follow the fast path of `styp? + moof + mdat`, with `moof`/`mdat` reused directly from fragmented MP4 input.
- The current implementation uses one media object per fragment/group, which aligns with the fragment-to-group mapping in the current MOQT CMAF packaging draft.

## Implementation consequence

The code in this repository intentionally separates:

- MP4/CMAF packaging
- draft-version control-plane mapping
- future transport publication

That separation should make it practical to contribute the packaging path first and wire in a concrete OpenMOQ transport session afterwards.
