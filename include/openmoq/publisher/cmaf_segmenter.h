#pragma once

#include <string>
#include <vector>

#include "openmoq/publisher/mp4_box.h"

namespace openmoq::publisher {

enum class CmafObjectMode {
    kSplit,
    kCoalesced,
};

struct PayloadBuffer {
    ByteSpan span;
    std::vector<std::uint8_t> owned_bytes;
};

struct MediaFragment {
    std::size_t group_id = 0;
    std::size_t object_id = 0;
    std::string track_name;
    std::uint64_t start_time_us = 0;
    std::uint64_t duration_us = 0;
    std::uint64_t earliest_presentation_time_us = 0;
    std::uint8_t sap_type = 0;
    PayloadBuffer payload;
};

struct SegmentedMp4 {
    PayloadBuffer initialization_segment;
    std::vector<MediaFragment> fragments;
    std::vector<TrackDescription> tracks;
};

SegmentedMp4 segment_for_cmaf(const ParsedMp4& parsed_mp4, CmafObjectMode object_mode = CmafObjectMode::kSplit);
std::string summarize_tracks(const std::vector<TrackDescription>& tracks);
std::size_t payload_size(const PayloadBuffer& payload);

}  // namespace openmoq::publisher
