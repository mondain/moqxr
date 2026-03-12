#include "openmoq/publisher/mp4_box.h"

#include <array>
#include <fstream>
#include <stdexcept>

namespace openmoq::publisher {

namespace {

constexpr std::array<const char*, 11> kContainerBoxes = {
    "moov", "trak", "mdia", "minf", "stbl", "moof", "traf", "mvex", "edts", "dinf", "meta"};

bool is_container_type(std::string_view type) {
    for (const char* candidate : kContainerBoxes) {
        if (type == candidate) {
            return true;
        }
    }
    return false;
}

std::uint32_t read_be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t read_be64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | bytes[offset + i];
    }
    return value;
}

std::vector<Mp4Box> parse_box_range(std::span<const std::uint8_t> bytes,
                                    std::size_t begin,
                                    std::size_t end) {
    std::vector<Mp4Box> boxes;
    std::size_t cursor = begin;

    while (cursor + 8 <= end) {
        const std::uint32_t small_size = read_be32(bytes, cursor);
        const std::string type(reinterpret_cast<const char*>(bytes.data() + cursor + 4), 4);

        std::size_t header_size = 8;
        std::uint64_t box_size = small_size;

        if (small_size == 1) {
            if (cursor + 16 > end) {
                throw std::runtime_error("truncated extended-size MP4 box");
            }
            header_size = 16;
            box_size = read_be64(bytes, cursor + 8);
        } else if (small_size == 0) {
            box_size = end - cursor;
        }

        if (box_size < header_size || cursor + box_size > end) {
            throw std::runtime_error("invalid MP4 box size for box type " + type);
        }

        Mp4Box box{
            .type = type,
            .span = {.offset = cursor, .size = static_cast<std::size_t>(box_size)},
            .payload = {.offset = cursor + header_size, .size = static_cast<std::size_t>(box_size) - header_size},
            .children = {},
        };

        if (is_container_type(type)) {
            box.children = parse_box_range(bytes, box.payload.offset, box.payload.offset + box.payload.size);
        }

        boxes.push_back(std::move(box));
        cursor += static_cast<std::size_t>(box_size);
    }

    return boxes;
}

const Mp4Box* find_child(const Mp4Box& box, std::string_view type) {
    for (const auto& child : box.children) {
        if (child.type == type) {
            return &child;
        }
    }
    return nullptr;
}

std::string codec_from_stsd(const Mp4Box& stsd, std::span<const std::uint8_t> bytes) {
    if (stsd.payload.size < 16) {
        return "unknown";
    }

    const std::size_t sample_entry_offset = stsd.payload.offset + 8;
    if (sample_entry_offset + 8 > bytes.size()) {
        return "unknown";
    }

    return std::string(reinterpret_cast<const char*>(bytes.data() + sample_entry_offset + 4), 4);
}

}  // namespace

ParsedMp4 parse_mp4_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open MP4 file: " + path);
    }

    input.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(input.tellg());
    input.seekg(0, std::ios::beg);

    ParsedMp4 parsed;
    parsed.bytes.resize(size);
    input.read(reinterpret_cast<char*>(parsed.bytes.data()), static_cast<std::streamsize>(size));

    if (!input) {
        throw std::runtime_error("failed to read MP4 file: " + path);
    }

    parsed.top_level_boxes = parse_mp4_boxes(parsed.bytes);
    parsed.tracks = extract_tracks(parsed.top_level_boxes, parsed.bytes);
    return parsed;
}

std::vector<Mp4Box> parse_mp4_boxes(std::span<const std::uint8_t> bytes) {
    return parse_box_range(bytes, 0, bytes.size());
}

std::vector<TrackDescription> extract_tracks(const std::vector<Mp4Box>& top_level_boxes,
                                             std::span<const std::uint8_t> bytes) {
    std::vector<TrackDescription> tracks;
    const Mp4Box* moov = find_first_box(top_level_boxes, "moov");
    if (moov == nullptr) {
        return tracks;
    }

    for (const auto& trak : moov->children) {
        if (trak.type != "trak") {
            continue;
        }

        const Mp4Box* mdia = find_child(trak, "mdia");
        if (mdia == nullptr) {
            continue;
        }

        const Mp4Box* hdlr = find_child(*mdia, "hdlr");
        const Mp4Box* minf = find_child(*mdia, "minf");
        if (hdlr == nullptr || minf == nullptr) {
            continue;
        }

        const Mp4Box* stbl = find_child(*minf, "stbl");
        const Mp4Box* stsd = stbl == nullptr ? nullptr : find_child(*stbl, "stsd");
        if (stsd == nullptr || hdlr->payload.size < 12) {
            continue;
        }

        const std::size_t handler_offset = hdlr->payload.offset + 8;
        const std::string handler_type(reinterpret_cast<const char*>(bytes.data() + handler_offset), 4);
        const std::string codec = codec_from_stsd(*stsd, bytes);

        tracks.push_back({
            .handler_type = handler_type,
            .codec = codec,
        });
    }

    return tracks;
}

const Mp4Box* find_first_box(const std::vector<Mp4Box>& boxes, std::string_view type) {
    for (const auto& box : boxes) {
        if (box.type == type) {
            return &box;
        }
    }
    return nullptr;
}

std::vector<const Mp4Box*> find_boxes(const std::vector<Mp4Box>& boxes, std::string_view type) {
    std::vector<const Mp4Box*> matches;
    for (const auto& box : boxes) {
        if (box.type == type) {
            matches.push_back(&box);
        }
    }
    return matches;
}

std::span<const std::uint8_t> slice_bytes(std::span<const std::uint8_t> bytes, const ByteSpan& span) {
    return bytes.subspan(span.offset, span.size);
}

}  // namespace openmoq::publisher
