#pragma once

#include <string>
#include <vector>

#include "openmoq/publisher/mp4_box.h"

namespace openmoq::publisher {

struct PayloadBuffer {
    ByteSpan span;
    std::vector<std::uint8_t> owned_bytes;
};

struct MediaFragment {
    std::size_t sequence = 0;
    std::string track_name;
    PayloadBuffer payload;
};

struct SegmentedMp4 {
    PayloadBuffer initialization_segment;
    std::vector<MediaFragment> fragments;
    std::vector<TrackDescription> tracks;
};

SegmentedMp4 segment_for_cmaf(const ParsedMp4& parsed_mp4);
std::string summarize_tracks(const std::vector<TrackDescription>& tracks);
std::size_t payload_size(const PayloadBuffer& payload);

}  // namespace openmoq::publisher
