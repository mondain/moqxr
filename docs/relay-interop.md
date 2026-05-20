# Relay Interoperability

## Basic Relay Publish

To attempt a live publish against a relay:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace interop \
  --forward 0 \
  --timeout 10 \
  --paced
```

Publish the same stream with SAP timeline tracks included:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace interop \
  --forward 0 \
  --timeout 10 \
  --paced \
  --sap
```

## SNI Override

If you need to connect to a relay by IP while still presenting the relay hostname in TLS SNI:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint 203.0.113.10:443 \
  --sni relay.example.com \
  --namespace interop \
  --forward 0 \
  --timeout 10
```

Use `--insecure` only when intentionally testing a relay with an untrusted or self-signed certificate. Public relays should be exercised with normal TLS verification so certificate and SNI regressions are visible.

## Verified-TLS WebTransport Examples

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input tmp-relay-test.mp4 \
  --transport webtransport \
  --endpoint https://<moqx-relay-host>:4433/moq-relay \
  --namespace live/paul1 \
  --forward 0 \
  --timeout 10 \
  --paced \
  --draft 16
```

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input tmp-relay-test.mp4 \
  --transport webtransport \
  --endpoint https://moq-relay.red5.net:4433/moq \
  --namespace live/paul1 \
  --forward 0 \
  --timeout 10 \
  --paced \
  --draft 16
```

`moq-relay.red5.net:4433` currently accepts WebTransport on `/moq`; `/moq-relay` returns HTTP `404` during CONNECT. The moqx relay examples use a placeholder hostname because those relay hostnames are not public yet; moqx uses `/moq-relay`.

## Trace CSV

If you want a per-object CSV trace for pacing and enqueue correlation, set `OPENMOQ_PICOQUIC_TRACE_CSV` alongside `OPENMOQ_PICOQUIC_TRACE`:

```bash
OPENMOQ_PICOQUIC_TRACE=1 \
OPENMOQ_PICOQUIC_TRACE_CSV=/tmp/openmoq-publisher-trace.csv \
./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace interop \
  --forward 0 \
  --timeout 10 \
  --paced
```

Rows include `pacing_before`, `pacing_after`, `enqueue`, and `served`/`sent` events for media objects.

## Behavior Notes

- `--forward 0` waits for inbound `SUBSCRIBE` requests before sending matching media objects
- with `--forward 0`, subscribers are still expected to request tracks explicitly
- by default, subscribers should subscribe to `catalog` if they need track discovery
- `--publish-catalog` keeps `--forward 0` for media tracks but proactively publishes the `catalog` track through the normal `PUBLISH` / `PUBLISH_OK` path
- `--sap` adds per-track `*_sap` event timeline tracks and metadata objects
- media packaging defaults to lower-latency split MOQT objects per group when chunk/sample boundaries are available
- `--coalesce-cmaf-chunks` disables that split and falls back to one media object per group
- when multiple tracks are subscribed, matching objects are served in publish-plan order so time-aligned audio/video stay interleaved
- `--forward 1` proactively publishes tracks and objects after namespace setup completes
- `--timeout <seconds>` controls how long the publisher waits for inbound `SUBSCRIBE` requests; the default is 30 seconds
- `--sni <value>` overrides the TLS SNI sent to the relay, useful when `--endpoint` uses a raw IP address
- WebTransport still sends HTTP authority from the configured endpoint host
- `--paced` applies pacing only to media-object sends; setup and publish control messages are sent immediately
