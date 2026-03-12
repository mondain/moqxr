#include "openmoq/publisher/cmaf_segmenter.h"

#include <sstream>
#include <stdexcept>

namespace openmoq::publisher {

SegmentedMp4 segment_for_cmaf(const ParsedMp4& parsed_mp4) {
    const Mp4Box* ftyp = find_first_box(parsed_mp4.top_level_boxes, "ftyp");
    const Mp4Box* moov = find_first_box(parsed_mp4.top_level_boxes, "moov");

    if (ftyp == nullptr || moov == nullptr) {
        throw std::runtime_error("input must contain ftyp and moov boxes");
    }

    SegmentedMp4 segmented{
        .initialization_segment =
            {.offset = ftyp->span.offset, .size = moov->span.offset + moov->span.size - ftyp->span.offset},
        .fragments = {},
        .tracks = parsed_mp4.tracks,
    };

    std::vector<const Mp4Box*> moofs = find_boxes(parsed_mp4.top_level_boxes, "moof");
    std::vector<const Mp4Box*> mdats = find_boxes(parsed_mp4.top_level_boxes, "mdat");

    if (moofs.empty() || mdats.empty() || moofs.size() != mdats.size()) {
        throw std::runtime_error(
            "only fragmented MP4 with paired moof/mdat boxes is supported in the zero-copy path");
    }

    for (std::size_t index = 0; index < moofs.size(); ++index) {
        if (moofs[index]->span.offset > mdats[index]->span.offset) {
            throw std::runtime_error("expected moof to precede matching mdat");
        }

        segmented.fragments.push_back({
            .sequence = index,
            .moof = moofs[index]->span,
            .mdat = mdats[index]->span,
        });
    }

    return segmented;
}

std::string summarize_tracks(const std::vector<TrackDescription>& tracks) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (index != 0) {
            stream << ", ";
        }
        stream << tracks[index].handler_type << ":" << tracks[index].codec;
    }
    return stream.str();
}

}  // namespace openmoq::publisher
