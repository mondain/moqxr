# FFmpeg Input Recipes

The publisher's preferred fast path is already fragmented MP4 input. You can generate that with `ffmpeg` by copying compatible AAC-LC, H.264, or HEVC streams and enabling CMAF-style fragmentation flags.

## Fragment an Existing MP4

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

## Re-Encode to Compatible H.264 or HEVC

If the source codecs are not already compatible, re-encode instead of copying.

H.264:

```bash
ffmpeg -i bbb_sunflower_2160p_60fps_normal.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -map_metadata -1 \
  -sn -dn \
  -c:v libx264 -preset medium -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 sunflower-frag.mp4
```

HEVC:

```bash
ffmpeg -i bbb_sunflower_2160p_60fps_normal.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -map_metadata -1 \
  -sn -dn \
  -c:v libx265 -preset medium -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 sunflower265-frag.mp4
```

## Practical Notes

- `-map 0:v -map 0:a` keeps only video and audio streams, excluding subtitle and other non-A/V tracks
- `-map 0:v:0 -map 0:a:0` uses the first audio stream if multiple exist
- `-sn -dn` explicitly disables subtitle and data or text streams
- `-map_metadata -1` drops container-level metadata from the output
- `+frag_keyframe` starts a new fragment on keyframes
- `+empty_moov` writes initialization metadata up front
- `+default_base_moof` and `+separate_moof` produce a layout that is easier for fragmented-MP4 pipelines to consume
- omit `+separate_moof` only if you are sure downstream tooling can parse interleaved multi-track fragments
- when audio appears in the catalog but no audio media objects are sent, regenerate with `+separate_moof`
- for HEVC, prefer streams that are already `hvc1`-compatible
- if a source is tagged `hev1` but keeps VPS/SPS/PPS only in the init segment, the publisher normalizes the advertised codec and emitted init segment to `hvc1`
- if HEVC samples include in-band parameter sets, the publisher preserves `hev1`
- if you start from a progressive MP4, this project can remux it internally, but pre-fragmented input is simpler and more efficient

## Live Fragmented MP4 Stdin Publishing

For live encoder pipelines, the publisher can consume fragmented MP4 directly from standard input.

This live path expects ffmpeg to emit track-separated fragments, where each `moof` + `mdat` pair belongs to a single media track. Use `+separate_moof` when generating the stream. Without `+separate_moof`, audio and video may be carried inside the same `moof`, which is not the intended input layout for the current live parser.

```bash
ffmpeg -stream_loop -1 -re -i bbb_sunflower_1080p_30fps_normal.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -c:v libx264 -preset medium -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 - | ./build/openmoq-publisher \
    --input - \
    --endpoint moqt://relay.example.com:443/moq \
    --namespace live/demo \
    --timeout 120
```
