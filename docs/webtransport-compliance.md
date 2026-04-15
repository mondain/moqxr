# WebTransport Compliance Notes

This note is the guardrail for MoQ over WebTransport work in this tree.
It is intentionally limited to the draft variants we support today:

- MoQ Transport draft 14
- MoQ Transport draft 16
- WebTransport over HTTP/3 draft 14

## Normative Model

### 1. Transport split

There are two distinct connection models:

- Native QUIC MoQ
- MoQ over WebTransport over HTTP/3

They are not interchangeable at session setup time.

For WebTransport:

- QUIC ALPN is `h3`
- session establishment uses HTTPS extended CONNECT
- the WebTransport resource is identified by `authority` and `path`
- MoQ draft/version negotiation happens after CONNECT

For native QUIC:

- QUIC ALPN identifies the MoQ draft directly
- `authority` and `path` are carried in the MoQ SETUP parameters

### 2. WebTransport CONNECT requirements

For WebTransport over HTTP/3:

- `:scheme` must be `https`
- `:protocol` must be `webtransport`
- the URI authority and path identify the session
- the connection requires the H3/WebTransport settings and transport parameters

In this codebase, picoquic's `picowt_prepare_client_cnx()` already configures the required transport parameters and H3 callback path.

### 3. WT protocol negotiation

MoQ-over-WebTransport draft selection is not QUIC ALPN.

It is carried in:

- `WT-Available-Protocols`
- `WT-Protocol`

These are Structured Fields strings on the CONNECT exchange.

Current offers in this repo:

- draft 14: offer no WebTransport subprotocol
- draft 16: offer `"moqt-16"` only

### 4. MoQ SETUP parameters over WebTransport

For WebTransport:

- `AUTHORITY` must not appear in MoQ SETUP
- `PATH` must not appear in MoQ SETUP

Those parameters are only for native QUIC mode.

### 5. WebTransport application stream format

WebTransport bidi application streams are not raw application bytes on the wire.

Each application bidi stream begins with:

- signal value `0x41`
- Session ID varint
- application payload

The application should only see the stream body after the stack strips the WT preamble.

The CONNECT stream is different:

- it is the HTTP/3 stream that established the WT session
- it can carry capsule traffic
- it is not the MoQ control stream

## What picoquic already does for us

The current picoquic WebTransport helpers cover a large part of the protocol surface:

- `picowt_prepare_client_cnx()` sets the H3 callback and WT transport parameters
- `h3zero` emits H3 settings including CONNECT protocol, H3 datagram, and WT session settings
- `picowt_set_control_stream()` creates the CONNECT stream context
- `picowt_connect()` formats the CONNECT request
- `picowt_create_local_stream()` creates WT app streams and writes the required preamble

That means this client should not manually reproduce those pieces unless there is a verified picoquic bug.

## Current code status

Areas that are aligned with the drafts:

- WT mode uses QUIC ALPN `h3` by default
- CONNECT uses the endpoint `authority` and `path`
- WT protocol offer is sent separately from QUIC ALPN
- MoQ `CLIENT_SETUP` omits `AUTHORITY` and `PATH` in WT mode
- local WT app streams are created through `picowt_create_local_stream()`
- WebTransport app-stream writes use picoquic's callback-driven provide-data path instead of direct stream injection

Areas that should be treated as suspect until proven:

- anything that interprets bytes arriving on the CONNECT stream as MoQ data
- any logic that tries to recreate WT app-stream context or preamble outside picoquic helpers
- any assumptions that the first MoQ response can arrive on the CONNECT stream

## Debugging rules

Before changing wire behavior, verify each of these:

1. CONNECT succeeded and the WebTransport protocol negotiation matches the intended draft:
   draft-14 offers no `WT-Protocol`, while draft-16 offers `moqt-16`.
2. The first client MoQ bytes are sent only on a WT application stream, not the CONNECT stream.
3. The WT application stream gets exactly one WT preamble.
4. Incoming CONNECT-stream bytes are logged as WT control/capsule traffic, not parsed as MoQ.
5. Incoming application-stream bytes are delivered without the WT preamble before MoQ parsing.
6. Draft-14 and draft-16 are tested separately.

## Current leading hypothesis

The remaining interoperability issue is most likely below the MoQ `CLIENT_SETUP` payload itself.

The strongest remaining risk area is WebTransport stream handling in our client:

- stream context ownership
- receive path separation between CONNECT-stream traffic and WT app-stream traffic
- possible double-management of local stream context around writes

The current evidence does not support parsing CONNECT-stream bytes as `SERVER_SETUP`.

## Current interoperability state

Observed behavior as of April 6, 2026:

- `draft-14.cloudflare.mediaoverquic.com:443/moq`
  - CONNECT succeeds
  - `SERVER_SETUP` arrives on the first WT bidi application stream
  - `PUBLISH_NAMESPACE_OK` arrives
  - the relay may remain idle afterward until a downstream subscriber appears
- `us-ord-1.moqx.akaleapi.net:4433/moq-relay`
  - draft 16 CONNECT succeeds
  - `SERVER_SETUP` and `PUBLISH_NAMESPACE_OK` arrive on the WT control stream as expected
  - the relay may remain idle afterward until a downstream subscriber appears
- `fb.mvfst.net:9448`
  - the tested resource paths still return HTTP `404` during CONNECT
  - that is currently treated as a server resource-path issue, not a post-CONNECT MoQ framing issue

## Idle subscriber behavior

For `--forward 0`, once `PUBLISH_NAMESPACE_OK` has been received:

- the publisher waits up to `--timeout` seconds for downstream `SUBSCRIBE`
- if no `SUBSCRIBE` arrives before that timeout, the session is treated as idle rather than failed
- the publisher emits `PUBLISH_NAMESPACE_DONE` and exits successfully

This behavior is intentional. It keeps interoperability probes from being reported as publish failures when the relay accepted the namespace but no subscriber appeared during the configured wait window.
