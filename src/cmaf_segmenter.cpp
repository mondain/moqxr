#include "openmoq/publisher/cmaf_segmenter.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace openmoq::publisher {

namespace {

struct ChunkMapEntry {
    std::uint32_t first_chunk = 0;
    std::uint32_t samples_per_chunk = 0;
};

struct TrackRemuxInfo {
    TrackDescription description;
    std::uint32_t timescale = 0;
    std::vector<std::uint64_t> sample_offsets;
    std::vector<std::uint32_t> sample_sizes;
    std::vector<std::uint32_t> sample_durations;
    std::vector<std::int32_t> composition_offsets;
    std::vector<bool> sync_samples;
};

std::uint32_t read_be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t read_be64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_ascii(std::vector<std::uint8_t>& out, std::string_view value) {
    out.insert(out.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> make_box(std::string_view type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    append_be32(out, static_cast<std::uint32_t>(8 + payload.size()));
    append_ascii(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> make_full_box(std::string_view type,
                                        std::uint8_t version,
                                        std::uint32_t flags,
                                        const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> full_payload;
    full_payload.push_back(version);
    full_payload.push_back(static_cast<std::uint8_t>((flags >> 16U) & 0xFFU));
    full_payload.push_back(static_cast<std::uint8_t>((flags >> 8U) & 0xFFU));
    full_payload.push_back(static_cast<std::uint8_t>(flags & 0xFFU));
    full_payload.insert(full_payload.end(), payload.begin(), payload.end());
    return make_box(type, full_payload);
}

std::vector<std::uint8_t> concat_boxes(const std::vector<std::vector<std::uint8_t>>& boxes) {
    std::vector<std::uint8_t> out;
    for (const auto& box : boxes) {
        out.insert(out.end(), box.begin(), box.end());
    }
    return out;
}

const Mp4Box* require_child(const Mp4Box& box, std::string_view type) {
    const Mp4Box* child = find_child_box(box, type);
    if (child == nullptr) {
        throw std::runtime_error("missing required child box: " + std::string(type));
    }
    return child;
}

std::vector<std::uint32_t> parse_sample_sizes(const Mp4Box& stsz, std::span<const std::uint8_t> bytes) {
    const std::size_t table_offset = stsz.payload.offset + 4;
    const std::uint32_t sample_size = read_be32(bytes, table_offset);
    const std::uint32_t sample_count = read_be32(bytes, table_offset + 4);

    std::vector<std::uint32_t> sizes;
    sizes.reserve(sample_count);

    if (sample_size != 0) {
        sizes.assign(sample_count, sample_size);
        return sizes;
    }

    std::size_t cursor = table_offset + 8;
    for (std::uint32_t index = 0; index < sample_count; ++index) {
        sizes.push_back(read_be32(bytes, cursor));
        cursor += 4;
    }

    return sizes;
}

std::vector<std::uint64_t> parse_chunk_offsets(const Mp4Box& stbl, std::span<const std::uint8_t> bytes) {
    if (const Mp4Box* stco = find_child_box(stbl, "stco")) {
        const std::size_t table_offset = stco->payload.offset + 4;
        const std::uint32_t count = read_be32(bytes, table_offset);
        std::vector<std::uint64_t> offsets;
        offsets.reserve(count);
        std::size_t cursor = table_offset + 4;
        for (std::uint32_t index = 0; index < count; ++index) {
            offsets.push_back(read_be32(bytes, cursor));
            cursor += 4;
        }
        return offsets;
    }

    if (const Mp4Box* co64 = find_child_box(stbl, "co64")) {
        const std::size_t table_offset = co64->payload.offset + 4;
        const std::uint32_t count = read_be32(bytes, table_offset);
        std::vector<std::uint64_t> offsets;
        offsets.reserve(count);
        std::size_t cursor = table_offset + 4;
        for (std::uint32_t index = 0; index < count; ++index) {
            offsets.push_back(read_be64(bytes, cursor));
            cursor += 8;
        }
        return offsets;
    }

    throw std::runtime_error("missing stco/co64 box for progressive MP4 remux");
}

std::vector<ChunkMapEntry> parse_chunk_map(const Mp4Box& stsc, std::span<const std::uint8_t> bytes) {
    const std::size_t table_offset = stsc.payload.offset + 4;
    const std::uint32_t count = read_be32(bytes, table_offset);
    std::vector<ChunkMapEntry> entries;
    entries.reserve(count);
    std::size_t cursor = table_offset + 4;
    for (std::uint32_t index = 0; index < count; ++index) {
        entries.push_back({
            .first_chunk = read_be32(bytes, cursor),
            .samples_per_chunk = read_be32(bytes, cursor + 4),
        });
        cursor += 12;
    }
    return entries;
}

std::vector<std::uint32_t> parse_timing_table(const Mp4Box& stts, std::span<const std::uint8_t> bytes) {
    const std::size_t table_offset = stts.payload.offset + 4;
    const std::uint32_t count = read_be32(bytes, table_offset);
    std::vector<std::uint32_t> durations;
    std::size_t cursor = table_offset + 4;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t run_count = read_be32(bytes, cursor);
        const std::uint32_t delta = read_be32(bytes, cursor + 4);
        durations.insert(durations.end(), run_count, delta);
        cursor += 8;
    }
    return durations;
}

std::vector<std::int32_t> parse_composition_offsets(const Mp4Box* ctts,
                                                    std::size_t sample_count,
                                                    std::span<const std::uint8_t> bytes) {
    std::vector<std::int32_t> offsets(sample_count, 0);
    if (ctts == nullptr) {
        return offsets;
    }

    const std::uint8_t version = bytes[ctts->payload.offset];
    const std::size_t table_offset = ctts->payload.offset + 4;
    const std::uint32_t count = read_be32(bytes, table_offset);
    std::size_t cursor = table_offset + 4;
    std::size_t sample_index = 0;

    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t run_count = read_be32(bytes, cursor);
        const std::uint32_t raw_value = read_be32(bytes, cursor + 4);
        const std::int32_t value =
            version == 1 ? static_cast<std::int32_t>(raw_value) : static_cast<std::int32_t>(raw_value);
        for (std::uint32_t run = 0; run < run_count && sample_index < offsets.size(); ++run) {
            offsets[sample_index++] = value;
        }
        cursor += 8;
    }

    return offsets;
}

std::vector<bool> parse_sync_samples(const Mp4Box* stss,
                                     std::size_t sample_count,
                                     std::string_view handler_type,
                                     std::span<const std::uint8_t> bytes) {
    std::vector<bool> sync_samples(sample_count, handler_type != "vide");
    if (stss == nullptr) {
        return sync_samples;
    }

    std::fill(sync_samples.begin(), sync_samples.end(), false);
    const std::size_t table_offset = stss->payload.offset + 4;
    const std::uint32_t count = read_be32(bytes, table_offset);
    std::size_t cursor = table_offset + 4;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t sample_number = read_be32(bytes, cursor);
        if (sample_number > 0 && sample_number <= sync_samples.size()) {
            sync_samples[sample_number - 1] = true;
        }
        cursor += 4;
    }
    return sync_samples;
}

std::vector<std::uint64_t> derive_sample_offsets(const std::vector<std::uint64_t>& chunk_offsets,
                                                 const std::vector<ChunkMapEntry>& chunk_map,
                                                 const std::vector<std::uint32_t>& sample_sizes) {
    std::vector<std::uint64_t> sample_offsets;
    sample_offsets.reserve(sample_sizes.size());

    std::size_t sample_index = 0;
    std::size_t map_index = 0;

    for (std::size_t chunk_index = 0; chunk_index < chunk_offsets.size(); ++chunk_index) {
        while (map_index + 1 < chunk_map.size() && chunk_map[map_index + 1].first_chunk <= chunk_index + 1) {
            ++map_index;
        }

        std::uint64_t current_offset = chunk_offsets[chunk_index];
        for (std::uint32_t in_chunk = 0; in_chunk < chunk_map[map_index].samples_per_chunk; ++in_chunk) {
            if (sample_index >= sample_sizes.size()) {
                break;
            }
            sample_offsets.push_back(current_offset);
            current_offset += sample_sizes[sample_index++];
        }
    }

    if (sample_offsets.size() != sample_sizes.size()) {
        throw std::runtime_error("sample table mapping does not cover all samples");
    }

    return sample_offsets;
}

std::uint32_t parse_timescale(const Mp4Box& mdhd, std::span<const std::uint8_t> bytes) {
    const std::uint8_t version = bytes[mdhd.payload.offset];
    const std::size_t offset = mdhd.payload.offset + (version == 1 ? 20 : 12);
    return read_be32(bytes, offset);
}

TrackRemuxInfo parse_track_remux_info(const Mp4Box& trak,
                                     const TrackDescription& track,
                                     std::span<const std::uint8_t> bytes) {
    const Mp4Box* mdia = require_child(trak, "mdia");
    const Mp4Box* mdhd = require_child(*mdia, "mdhd");
    const Mp4Box* minf = require_child(*mdia, "minf");
    const Mp4Box* stbl = require_child(*minf, "stbl");
    const Mp4Box* stsz = require_child(*stbl, "stsz");
    const Mp4Box* stsc = require_child(*stbl, "stsc");
    const Mp4Box* stts = require_child(*stbl, "stts");

    const std::vector<std::uint32_t> sample_sizes = parse_sample_sizes(*stsz, bytes);
    const std::vector<std::uint32_t> sample_durations = parse_timing_table(*stts, bytes);
    if (sample_sizes.size() != sample_durations.size()) {
        throw std::runtime_error("stsz and stts tables disagree on sample count");
    }

    return {
        .description = track,
        .timescale = parse_timescale(*mdhd, bytes),
        .sample_offsets = derive_sample_offsets(parse_chunk_offsets(*stbl, bytes),
                                               parse_chunk_map(*stsc, bytes),
                                               sample_sizes),
        .sample_sizes = sample_sizes,
        .sample_durations = sample_durations,
        .composition_offsets = parse_composition_offsets(find_child_box(*stbl, "ctts"), sample_sizes.size(), bytes),
        .sync_samples = parse_sync_samples(find_child_box(*stbl, "stss"), sample_sizes.size(), track.handler_type, bytes),
    };
}

std::vector<std::uint8_t> build_mvex_box(const std::vector<TrackDescription>& tracks) {
    std::vector<std::vector<std::uint8_t>> trex_boxes;
    trex_boxes.reserve(tracks.size());
    for (const auto& track : tracks) {
        std::vector<std::uint8_t> trex_payload;
        append_be32(trex_payload, track.track_id);
        append_be32(trex_payload, 1);
        append_be32(trex_payload, 0);
        append_be32(trex_payload, 0);
        append_be32(trex_payload, 0);
        trex_boxes.push_back(make_full_box("trex", 0, 0, trex_payload));
    }
    return make_box("mvex", concat_boxes(trex_boxes));
}

std::vector<std::uint8_t> build_fragmented_init_segment(const Mp4Box& ftyp,
                                                        const Mp4Box& moov,
                                                        const std::vector<TrackDescription>& tracks,
                                                        std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> init(slice_bytes(bytes, ftyp.span).begin(), slice_bytes(bytes, ftyp.span).end());
    std::vector<std::uint8_t> moov_bytes(slice_bytes(bytes, moov.span).begin(), slice_bytes(bytes, moov.span).end());

    if (find_child_box(moov, "mvex") == nullptr) {
        const std::vector<std::uint8_t> mvex = build_mvex_box(tracks);
        moov_bytes.insert(moov_bytes.end(), mvex.begin(), mvex.end());
        const std::uint32_t new_size = static_cast<std::uint32_t>(moov_bytes.size());
        moov_bytes[0] = static_cast<std::uint8_t>((new_size >> 24U) & 0xFFU);
        moov_bytes[1] = static_cast<std::uint8_t>((new_size >> 16U) & 0xFFU);
        moov_bytes[2] = static_cast<std::uint8_t>((new_size >> 8U) & 0xFFU);
        moov_bytes[3] = static_cast<std::uint8_t>(new_size & 0xFFU);
    }

    init.insert(init.end(), moov_bytes.begin(), moov_bytes.end());
    return init;
}

std::uint32_t sample_flags_for(const TrackRemuxInfo& track, std::size_t sample_index) {
    if (track.description.handler_type == "vide") {
        return track.sync_samples[sample_index] ? 0x02000000U : 0x01010000U;
    }
    return 0x02000000U;
}

std::vector<std::uint8_t> build_trun_box(const TrackRemuxInfo& track, std::uint32_t data_offset) {
    std::vector<std::uint8_t> trun_payload;
    append_be32(trun_payload, static_cast<std::uint32_t>(track.sample_sizes.size()));
    append_be32(trun_payload, data_offset);
    for (std::size_t index = 0; index < track.sample_sizes.size(); ++index) {
        append_be32(trun_payload, track.sample_durations[index]);
        append_be32(trun_payload, track.sample_sizes[index]);
        append_be32(trun_payload, sample_flags_for(track, index));
        append_be32(trun_payload, static_cast<std::uint32_t>(track.composition_offsets[index]));
    }
    return make_full_box("trun", 1, 0x000F01, trun_payload);
}

std::vector<std::uint8_t> build_remuxed_fragment(const TrackRemuxInfo& track,
                                                 std::size_t sequence,
                                                 std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> mdat_payload;
    mdat_payload.reserve(std::accumulate(track.sample_sizes.begin(), track.sample_sizes.end(), std::size_t{0}));
    for (std::size_t index = 0; index < track.sample_sizes.size(); ++index) {
        const ByteSpan sample_span{.offset = static_cast<std::size_t>(track.sample_offsets[index]),
                                   .size = track.sample_sizes[index]};
        const auto sample_bytes = slice_bytes(bytes, sample_span);
        mdat_payload.insert(mdat_payload.end(), sample_bytes.begin(), sample_bytes.end());
    }

    std::vector<std::uint8_t> mfhd_payload;
    append_be32(mfhd_payload, static_cast<std::uint32_t>(sequence + 1));
    const std::vector<std::uint8_t> mfhd = make_full_box("mfhd", 0, 0, mfhd_payload);

    std::vector<std::uint8_t> tfhd_payload;
    append_be32(tfhd_payload, track.description.track_id);
    const std::vector<std::uint8_t> tfhd = make_full_box("tfhd", 0, 0x020000, tfhd_payload);

    std::vector<std::uint8_t> tfdt_payload;
    append_be32(tfdt_payload, 0);
    const std::vector<std::uint8_t> tfdt = make_full_box("tfdt", 0, 0, tfdt_payload);

    const std::vector<std::uint8_t> placeholder_trun = build_trun_box(track, 0);
    const std::vector<std::uint8_t> placeholder_traf = make_box("traf", concat_boxes({tfhd, tfdt, placeholder_trun}));
    const std::vector<std::uint8_t> placeholder_moof = make_box("moof", concat_boxes({mfhd, placeholder_traf}));
    const std::uint32_t data_offset = static_cast<std::uint32_t>(placeholder_moof.size() + 8);

    const std::vector<std::uint8_t> trun = build_trun_box(track, data_offset);
    const std::vector<std::uint8_t> traf = make_box("traf", concat_boxes({tfhd, tfdt, trun}));
    const std::vector<std::uint8_t> moof = make_box("moof", concat_boxes({mfhd, traf}));
    const std::vector<std::uint8_t> mdat = make_box("mdat", mdat_payload);

    return concat_boxes({moof, mdat});
}

}  // namespace

SegmentedMp4 segment_for_cmaf(const ParsedMp4& parsed_mp4) {
    const Mp4Box* ftyp = find_first_box(parsed_mp4.top_level_boxes, "ftyp");
    const Mp4Box* moov = find_first_box(parsed_mp4.top_level_boxes, "moov");
    if (ftyp == nullptr || moov == nullptr) {
        throw std::runtime_error("input must contain ftyp and moov boxes");
    }

    SegmentedMp4 segmented{
        .initialization_segment = {.span = {.offset = ftyp->span.offset,
                                            .size = moov->span.offset + moov->span.size - ftyp->span.offset},
                                   .owned_bytes = {}},
        .fragments = {},
        .tracks = parsed_mp4.tracks,
    };

    const std::vector<const Mp4Box*> moofs = find_boxes(parsed_mp4.top_level_boxes, "moof");
    const std::vector<const Mp4Box*> mdats = find_boxes(parsed_mp4.top_level_boxes, "mdat");

    if (!moofs.empty()) {
        if (moofs.size() != mdats.size()) {
            throw std::runtime_error("fragmented MP4 must contain matched moof/mdat pairs");
        }

        for (std::size_t index = 0; index < moofs.size(); ++index) {
            if (moofs[index]->span.offset > mdats[index]->span.offset) {
                throw std::runtime_error("expected moof to precede matching mdat");
            }

            segmented.fragments.push_back({
                .sequence = index,
                .track_name = "media",
                .payload = {.span = {.offset = moofs[index]->span.offset,
                                     .size = moofs[index]->span.size + mdats[index]->span.size},
                            .owned_bytes = {}},
            });
        }

        return segmented;
    }

    segmented.initialization_segment.owned_bytes =
        build_fragmented_init_segment(*ftyp, *moov, parsed_mp4.tracks, parsed_mp4.bytes);
    segmented.initialization_segment.span = {};

    std::size_t track_index = 0;
    for (const auto& child : moov->children) {
        if (child.type != "trak") {
            continue;
        }
        if (track_index >= parsed_mp4.tracks.size()) {
            break;
        }

        const TrackRemuxInfo track_info = parse_track_remux_info(child, parsed_mp4.tracks[track_index], parsed_mp4.bytes);
        segmented.fragments.push_back({
            .sequence = track_index,
            .track_name = track_info.description.track_name,
            .payload = {.span = {}, .owned_bytes = build_remuxed_fragment(track_info, track_index, parsed_mp4.bytes)},
        });
        ++track_index;
    }

    if (segmented.fragments.empty()) {
        throw std::runtime_error("no tracks available for progressive MP4 remux");
    }

    return segmented;
}

std::string summarize_tracks(const std::vector<TrackDescription>& tracks) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (index != 0) {
            stream << ", ";
        }
        stream << tracks[index].track_name << "(" << tracks[index].handler_type << ":" << tracks[index].codec << ")";
    }
    return stream.str();
}

std::size_t payload_size(const PayloadBuffer& payload) {
    return payload.owned_bytes.empty() ? payload.span.size : payload.owned_bytes.size();
}

}  // namespace openmoq::publisher
