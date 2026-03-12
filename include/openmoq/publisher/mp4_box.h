#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::publisher {

struct ByteSpan {
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct Mp4Box {
    std::string type;
    ByteSpan span;
    ByteSpan payload;
    std::vector<Mp4Box> children;
};

struct TrackDescription {
    std::string handler_type;
    std::string codec;
};

struct ParsedMp4 {
    std::vector<std::uint8_t> bytes;
    std::vector<Mp4Box> top_level_boxes;
    std::vector<TrackDescription> tracks;
};

ParsedMp4 parse_mp4_file(const std::string& path);
std::vector<Mp4Box> parse_mp4_boxes(std::span<const std::uint8_t> bytes);
std::vector<TrackDescription> extract_tracks(const std::vector<Mp4Box>& top_level_boxes,
                                             std::span<const std::uint8_t> bytes);

const Mp4Box* find_first_box(const std::vector<Mp4Box>& boxes, std::string_view type);
std::vector<const Mp4Box*> find_boxes(const std::vector<Mp4Box>& boxes, std::string_view type);
std::span<const std::uint8_t> slice_bytes(std::span<const std::uint8_t> bytes, const ByteSpan& span);

}  // namespace openmoq::publisher
