#pragma once

#include <string>
#include <vector>

#include "openmoq/publisher/mp4_box.h"

namespace openmoq::publisher {

struct MediaFragment {
    std::size_t sequence = 0;
    ByteSpan moof;
    ByteSpan mdat;
};

struct SegmentedMp4 {
    ByteSpan initialization_segment;
    std::vector<MediaFragment> fragments;
    std::vector<TrackDescription> tracks;
};

SegmentedMp4 segment_for_cmaf(const ParsedMp4& parsed_mp4);
std::string summarize_tracks(const std::vector<TrackDescription>& tracks);

}  // namespace openmoq::publisher
