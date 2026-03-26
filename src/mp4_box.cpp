#include "openmoq/publisher/mp4_box.h"

#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
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

std::uint16_t read_be16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1]);
}

std::string hex_byte(std::uint8_t value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(value);
    return out.str();
}

std::size_t find_child_box_offset(const Mp4Box& sample_entry,
                                  std::span<const std::uint8_t> bytes,
                                  std::size_t child_offset,
                                  std::string_view type) {
    for (std::size_t scan_offset = child_offset; scan_offset + 8 <= sample_entry.span.size; ++scan_offset) {
        const std::size_t cursor = sample_entry.span.offset + scan_offset;
        const std::uint32_t box_size = read_be32(bytes, cursor);
        if (box_size < 8 || cursor + box_size > sample_entry.span.offset + sample_entry.span.size) {
            continue;
        }
        if (std::string_view(reinterpret_cast<const char*>(bytes.data() + cursor + 4), 4) == type) {
            return cursor;
        }
    }
    return 0;
}

bool decode_descriptor_length(std::span<const std::uint8_t> bytes,
                              std::size_t offset,
                              std::size_t limit,
                              std::size_t& length,
                              std::size_t& bytes_consumed) {
    length = 0;
    bytes_consumed = 0;
    while (offset + bytes_consumed < limit && bytes_consumed < 4) {
        const std::uint8_t value = bytes[offset + bytes_consumed];
        length = (length << 7U) | static_cast<std::size_t>(value & 0x7FU);
        ++bytes_consumed;
        if ((value & 0x80U) == 0) {
            return true;
        }
    }
    return false;
}

std::string avc_codec_string(const Mp4Box& sample_entry, std::span<const std::uint8_t> bytes) {
    const std::size_t avcc_offset = find_child_box_offset(sample_entry, bytes, 8 + 70, "avcC");
    if (avcc_offset == 0 || avcc_offset + 12 > bytes.size()) {
        return "avc1";
    }

    const std::uint8_t profile = bytes[avcc_offset + 9];
    const std::uint8_t compatibility = bytes[avcc_offset + 10];
    const std::uint8_t level = bytes[avcc_offset + 11];
    return sample_entry.type + "." + hex_byte(profile) + hex_byte(compatibility) + hex_byte(level);
}

std::string hevc_codec_string(const Mp4Box& sample_entry, std::span<const std::uint8_t> bytes) {
    const std::size_t hvcc_offset = find_child_box_offset(sample_entry, bytes, 8 + 70, "hvcC");
    if (hvcc_offset == 0 || hvcc_offset + 21 > bytes.size()) {
        return sample_entry.type;
    }

    const std::uint8_t profile_byte = bytes[hvcc_offset + 9];
    const char profile_space = (profile_byte >> 6U) == 1 ? 'A' : (profile_byte >> 6U) == 2 ? 'B' : (profile_byte >> 6U) == 3 ? 'C' : '\0';
    const std::uint8_t profile_idc = profile_byte & 0x1FU;
    const std::uint32_t compatibility_flags = read_be32(bytes, hvcc_offset + 10);
    const std::uint64_t constraint_indicator =
        (static_cast<std::uint64_t>(bytes[hvcc_offset + 14]) << 40U) |
        (static_cast<std::uint64_t>(bytes[hvcc_offset + 15]) << 32U) |
        (static_cast<std::uint64_t>(bytes[hvcc_offset + 16]) << 24U) |
        (static_cast<std::uint64_t>(bytes[hvcc_offset + 17]) << 16U) |
        (static_cast<std::uint64_t>(bytes[hvcc_offset + 18]) << 8U) |
        static_cast<std::uint64_t>(bytes[hvcc_offset + 19]);
    const std::uint8_t level_idc = bytes[hvcc_offset + 20];

    std::ostringstream out;
    out << sample_entry.type << '.';
    if (profile_space != '\0') {
        out << profile_space;
    }
    out << static_cast<unsigned int>(profile_idc) << '.'
        << std::uppercase << std::hex << compatibility_flags << '.'
        << ((bytes[hvcc_offset + 13] & 0x20U) != 0 ? 'H' : 'L') << static_cast<unsigned int>(level_idc);
    if (constraint_indicator != 0) {
        out << '.';
        for (int shift = 40; shift >= 0; shift -= 8) {
            const auto component = static_cast<std::uint8_t>((constraint_indicator >> shift) & 0xFFU);
            if (component == 0 && shift != 0) {
                continue;
            }
            out << hex_byte(component);
        }
    }
    return out.str();
}

std::string mpeg4_audio_codec_string(const Mp4Box& sample_entry, std::span<const std::uint8_t> bytes) {
    const std::size_t esds_offset = find_child_box_offset(sample_entry, bytes, 8 + 28, "esds");
    if (esds_offset == 0 || esds_offset + 16 > bytes.size()) {
        return "mp4a.40.2";
    }

    std::uint8_t audio_object_type = 2;
    for (std::size_t cursor = esds_offset + 12; cursor + 2 <= sample_entry.span.offset + sample_entry.span.size; ++cursor) {
        if (bytes[cursor] != 0x05) {
            continue;
        }
        std::size_t length = 0;
        std::size_t length_bytes = 0;
        if (!decode_descriptor_length(bytes, cursor + 1, sample_entry.span.offset + sample_entry.span.size, length, length_bytes) ||
            length == 0) {
            continue;
        }
        const std::size_t config_offset = cursor + 1 + length_bytes;
        if (config_offset + length > sample_entry.span.offset + sample_entry.span.size) {
            continue;
        }
        const std::uint8_t config = bytes[config_offset];
        audio_object_type = static_cast<std::uint8_t>((config >> 3U) & 0x1FU);
        if (audio_object_type == 31 && length >= 2) {
            audio_object_type =
                static_cast<std::uint8_t>(32 + ((config & 0x07U) << 3U) + ((bytes[config_offset + 1] >> 5U) & 0x07U));
        }
        break;
    }

    std::ostringstream out;
    out << "mp4a.40." << static_cast<unsigned int>(audio_object_type);
    return out.str();
}

std::string codec_string_from_sample_entry(const Mp4Box& sample_entry, std::span<const std::uint8_t> bytes) {
    if (sample_entry.type == "avc1" || sample_entry.type == "avc3") {
        return avc_codec_string(sample_entry, bytes);
    }
    if (sample_entry.type == "hvc1" || sample_entry.type == "hev1") {
        return hevc_codec_string(sample_entry, bytes);
    }
    if (sample_entry.type == "mp4a") {
        return mpeg4_audio_codec_string(sample_entry, bytes);
    }
    if (sample_entry.type == "Opus" || sample_entry.type == "opus") {
        return "opus";
    }
    return sample_entry.type;
}

double frame_rate_from_stts(const Mp4Box* stts, std::uint32_t timescale, std::span<const std::uint8_t> bytes) {
    if (stts == nullptr || timescale == 0 || stts->payload.size < 8) {
        return 0.0;
    }

    const std::size_t table_offset = stts->payload.offset + 4;
    const std::uint32_t entry_count = read_be32(bytes, table_offset);
    std::size_t cursor = table_offset + 4;
    std::uint64_t sample_count = 0;
    std::uint64_t duration_sum = 0;
    for (std::uint32_t index = 0; index < entry_count && cursor + 8 <= bytes.size(); ++index) {
        const std::uint32_t run_count = read_be32(bytes, cursor);
        const std::uint32_t delta = read_be32(bytes, cursor + 4);
        sample_count += run_count;
        duration_sum += static_cast<std::uint64_t>(run_count) * delta;
        cursor += 8;
    }

    if (sample_count == 0 || duration_sum == 0) {
        return 0.0;
    }
    return static_cast<double>(sample_count) * static_cast<double>(timescale) / static_cast<double>(duration_sum);
}

}  // namespace

ParsedMp4 parse_mp4_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open MP4 file: " + path);
    }

    return parse_mp4_stream(input, path);
}

ParsedMp4 parse_mp4_stream(std::istream& input, std::string_view source_name) {
    ParsedMp4 parsed;

    input.seekg(0, std::ios::end);
    if (input.good()) {
        const auto end = input.tellg();
        if (end >= 0) {
            parsed.bytes.resize(static_cast<std::size_t>(end));
            input.seekg(0, std::ios::beg);
            input.read(reinterpret_cast<char*>(parsed.bytes.data()), static_cast<std::streamsize>(parsed.bytes.size()));
            if (!input) {
                throw std::runtime_error("failed to read MP4 input: " + std::string(source_name));
            }
        }
    }

    if (parsed.bytes.empty()) {
        input.clear();
        input.seekg(0, std::ios::beg);

        constexpr std::size_t kChunkSize = 16 * 1024;
        std::array<char, kChunkSize> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = input.gcount();
            if (bytes_read > 0) {
                parsed.bytes.insert(parsed.bytes.end(),
                                    reinterpret_cast<const std::uint8_t*>(buffer.data()),
                                    reinterpret_cast<const std::uint8_t*>(buffer.data()) + bytes_read);
            }
        }
        if (!input.eof()) {
            throw std::runtime_error("failed to read MP4 input: " + std::string(source_name));
        }
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
        const Mp4Box* mdhd = find_child(*mdia, "mdhd");
        const Mp4Box* minf = find_child(*mdia, "minf");
        const Mp4Box* tkhd = find_child(trak, "tkhd");
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
        const std::string sample_entry_type = codec_from_stsd(*stsd, bytes);
        const Mp4Box sample_entry{
            .type = sample_entry_type,
            .span = {.offset = stsd->payload.offset + 8, .size = read_be32(bytes, stsd->payload.offset + 8)},
            .payload = {.offset = stsd->payload.offset + 16, .size = read_be32(bytes, stsd->payload.offset + 8) - 8},
            .children = {},
        };
        const std::string codec = codec_string_from_sample_entry(sample_entry, bytes);
        std::uint32_t track_id = 0;
        std::uint32_t timescale = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t channel_count = 0;
        std::uint32_t sample_rate = 0;
        double frame_rate = 0.0;
        if (tkhd != nullptr && tkhd->payload.size >= 20) {
            const std::uint8_t version = bytes[tkhd->payload.offset];
            const std::size_t track_id_offset = tkhd->payload.offset + (version == 1 ? 20 : 12);
            if (track_id_offset + 4 <= bytes.size()) {
                track_id = read_be32(bytes, track_id_offset);
            }
        }
        if (mdhd != nullptr && mdhd->payload.size >= 20) {
            const std::uint8_t version = bytes[mdhd->payload.offset];
            const std::size_t timescale_offset = mdhd->payload.offset + (version == 1 ? 20 : 12);
            if (timescale_offset + 4 <= bytes.size()) {
                timescale = read_be32(bytes, timescale_offset);
            }
        }
        if ((sample_entry_type == "avc1" || sample_entry_type == "avc3" || sample_entry_type == "hvc1" ||
             sample_entry_type == "hev1") &&
            sample_entry.payload.offset + 28 <= bytes.size()) {
            width = read_be16(bytes, sample_entry.payload.offset + 24);
            height = read_be16(bytes, sample_entry.payload.offset + 26);
            frame_rate = frame_rate_from_stts(find_child(*stbl, "stts"), timescale, bytes);
        }
        if ((sample_entry_type == "mp4a" || sample_entry_type == "Opus" || sample_entry_type == "opus") &&
            sample_entry.payload.offset + 28 <= bytes.size()) {
            channel_count = read_be16(bytes, sample_entry.payload.offset + 16);
            sample_rate = read_be16(bytes, sample_entry.payload.offset + 24);
        }

        tracks.push_back({
            .track_id = track_id,
            .handler_type = handler_type,
            .codec = codec,
            .sample_entry_type = sample_entry_type,
            .track_name = handler_type + "_" + std::to_string(tracks.size() + 1),
            .packaging = "cmaf",
            .event_type = {},
            .mime_type = {},
            .depends = {},
            .timescale = timescale,
            .width = width,
            .height = height,
            .channel_count = channel_count,
            .sample_rate = sample_rate,
            .frame_rate = frame_rate,
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

const Mp4Box* find_child_box(const Mp4Box& box, std::string_view type) {
    return find_child(box, type);
}

std::span<const std::uint8_t> slice_bytes(std::span<const std::uint8_t> bytes, const ByteSpan& span) {
    return bytes.subspan(span.offset, span.size);
}

}  // namespace openmoq::publisher
