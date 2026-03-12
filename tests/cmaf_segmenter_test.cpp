#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/mp4_box.h"

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_ascii(std::vector<std::uint8_t>& out, const std::string& value) {
    out.insert(out.end(), value.begin(), value.end());
}

void patch_be32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
    out[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    out[offset + 2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    out[offset + 3] = static_cast<std::uint8_t>(value & 0xFFU);
}

std::vector<std::uint8_t> make_box(const std::string& type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    append_be32(out, static_cast<std::uint32_t>(8 + payload.size()));
    append_ascii(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> make_full_box(const std::string& type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out(4, 0);
    out.insert(out.end(), payload.begin(), payload.end());
    return make_box(type, out);
}

std::vector<std::uint8_t> concat(std::initializer_list<std::vector<std::uint8_t>> boxes) {
    std::vector<std::uint8_t> out;
    for (const auto& box : boxes) {
        out.insert(out.end(), box.begin(), box.end());
    }
    return out;
}

std::vector<std::uint8_t> make_fragmented_test_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    const auto sample_entry = make_box("avc1",
                                       concat({std::vector<std::uint8_t>(70, 0),
                                               make_box("avcC", {1, 100, 0, 12, 0xff})}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);
    const auto moof = make_box("moof", {'m', 'f', 'h', 'd'});
    const auto mdat = make_box("mdat", {1, 2, 3, 4, 5, 6});
    return concat({ftyp, moov, moof, mdat});
}

std::vector<std::uint8_t> make_progressive_test_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 232, 0, 0, 7, 208, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    const auto sample_entry = make_box("avc1",
                                       concat({std::vector<std::uint8_t>(70, 0),
                                               make_box("avcC", {1, 100, 0, 12, 0xff})}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stts = make_full_box("stts", {0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 3, 232});
    const auto stsc = make_full_box("stsc", {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1});
    const auto stsz = make_full_box("stsz", {0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0, 4});
    auto stco = make_full_box("stco", {0, 0, 0, 1, 0, 0, 0, 0});
    const auto stss = make_full_box("stss", {0, 0, 0, 1, 0, 0, 0, 1});
    const auto stbl = make_box("stbl", concat({stsd, stts, stsc, stsz, stco, stss}));
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);
    const auto mdat = make_box("mdat", {1, 2, 3, 4, 5, 6, 7, 8});

    std::vector<std::uint8_t> file = concat({ftyp, moov, mdat});
    const std::uint32_t mdat_payload_offset = static_cast<std::uint32_t>(ftyp.size() + moov.size() + 8);
    const std::size_t stco_payload_offset =
        ftyp.size() + 8 + tkhd.size() + 8 + mdhd.size() + hdlr.size() + 8 + 8 + stsd.size() + stts.size() +
        stsc.size() + stsz.size() + 16;
    patch_be32(file, stco_payload_offset, mdat_payload_offset);
    return file;
}

std::vector<std::uint8_t> make_multitrack_init_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});

    const auto video_tkhd = make_full_box("tkhd",
                                          {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto video_hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    const auto video_sample_entry = make_box("avc1",
                                             concat({std::vector<std::uint8_t>(70, 0),
                                                     make_box("avcC", {1, 100, 0, 12, 0xff})}));
    const auto video_stsd = make_full_box("stsd",
                                          concat({std::vector<std::uint8_t>{0, 0, 0, 1}, video_sample_entry}));
    const auto video_stbl = make_box("stbl", video_stsd);
    const auto video_minf = make_box("minf", video_stbl);
    const auto video_mdia = make_box("mdia", concat({video_hdlr, video_minf}));
    const auto video_trak = make_box("trak", concat({video_tkhd, video_mdia}));

    const auto audio_tkhd = make_full_box("tkhd",
                                          {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0});
    const auto audio_hdlr = make_full_box("hdlr", {0, 0, 0, 0, 's', 'o', 'u', 'n', 0, 0, 0, 0});
    const auto audio_sample_entry = make_box("mp4a",
                                             concat({std::vector<std::uint8_t>(28, 0),
                                                     make_box("esds", {0, 0, 0, 0, 0x03, 0x19, 0x00, 0x02})}));
    const auto audio_stsd = make_full_box("stsd",
                                          concat({std::vector<std::uint8_t>{0, 0, 0, 1}, audio_sample_entry}));
    const auto audio_stbl = make_box("stbl", audio_stsd);
    const auto audio_minf = make_box("minf", audio_stbl);
    const auto audio_mdia = make_box("mdia", concat({audio_hdlr, audio_minf}));
    const auto audio_trak = make_box("trak", concat({audio_tkhd, audio_mdia}));

    const auto trex_video =
        make_full_box("trex", {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto trex_audio =
        make_full_box("trex", {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto mvex = make_box("mvex", concat({trex_video, trex_audio}));
    const auto moov = make_box("moov", concat({video_trak, audio_trak, mvex}));
    return concat({ftyp, moov});
}

std::size_t find_after(std::string_view haystack, std::string_view needle, std::size_t start = 0) {
    const std::size_t pos = haystack.find(needle, start);
    return pos == std::string_view::npos ? pos : pos + needle.size();
}

std::string catalog_init_data(std::string_view catalog, std::string_view track_name) {
    const std::string name_key = std::string("\"name\":\"") + std::string(track_name) + "\"";
    const std::size_t name_pos = catalog.find(name_key);
    if (name_pos == std::string_view::npos) {
        return {};
    }
    const std::size_t init_pos = find_after(catalog, "\"initData\":\"", name_pos);
    if (init_pos == std::string_view::npos) {
        return {};
    }
    const std::size_t end_pos = catalog.find('"', init_pos);
    if (end_pos == std::string_view::npos) {
        return {};
    }
    return std::string(catalog.substr(init_pos, end_pos - init_pos));
}

std::vector<std::uint8_t> base64_decode(std::string_view text) {
    std::map<char, std::uint8_t> table;
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        table.emplace(alphabet[index], static_cast<std::uint8_t>(index));
    }

    std::vector<std::uint8_t> decoded;
    for (std::size_t index = 0; index < text.size(); index += 4) {
        const std::uint32_t a = table.at(text[index]);
        const std::uint32_t b = table.at(text[index + 1]);
        const std::uint32_t c = text[index + 2] == '=' ? 0 : table.at(text[index + 2]);
        const std::uint32_t d = text[index + 3] == '=' ? 0 : table.at(text[index + 3]);
        const std::uint32_t word = (a << 18U) | (b << 12U) | (c << 6U) | d;
        decoded.push_back(static_cast<std::uint8_t>((word >> 16U) & 0xFFU));
        if (text[index + 2] != '=') {
            decoded.push_back(static_cast<std::uint8_t>((word >> 8U) & 0xFFU));
        }
        if (text[index + 3] != '=') {
            decoded.push_back(static_cast<std::uint8_t>(word & 0xFFU));
        }
    }
    return decoded;
}

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool bytes_equal(const std::vector<std::uint8_t>& bytes, std::initializer_list<std::uint8_t> expected) {
    return std::vector<std::uint8_t>(expected) == bytes;
}

}  // namespace

int main() {
    using namespace openmoq::publisher;

    bool ok = true;

    const auto fragmented_bytes = make_fragmented_test_mp4();
    ParsedMp4 fragmented{
        .bytes = fragmented_bytes,
        .top_level_boxes = parse_mp4_boxes(fragmented_bytes),
        .tracks = {},
    };
    fragmented.tracks = extract_tracks(fragmented.top_level_boxes, fragmented.bytes);

    const auto segmented = segment_for_cmaf(fragmented);
    const auto plan = build_publish_plan(segmented, DraftVersion::kDraft14);

    ok &= expect(fragmented.top_level_boxes.size() == 4, "expected 4 top-level boxes");
    ok &= expect(fragmented.tracks.size() == 1, "expected one extracted fragmented track");
    ok &= expect(fragmented.tracks.front().codec == "avc1", "expected avc1 codec");
    ok &= expect(segmented.fragments.size() == 1, "expected one fragmented media fragment");
    ok &= expect(plan.objects.size() == 2, "expected catalog plus one fragmented media object");
    ok &= expect(plan.objects.front().track_name == "catalog", "expected catalog object first");
    ok &= expect(payload_size(segmented.initialization_segment) > 0, "expected fragmented init payload");
    ok &= expect(payload_size(segmented.fragments.front().payload) > 0, "expected fragmented media payload");

    const auto progressive_bytes = make_progressive_test_mp4();
    ParsedMp4 progressive{
        .bytes = progressive_bytes,
        .top_level_boxes = parse_mp4_boxes(progressive_bytes),
        .tracks = {},
    };
    progressive.tracks = extract_tracks(progressive.top_level_boxes, progressive.bytes);

    const auto remuxed = segment_for_cmaf(progressive);
    const auto remuxed_plan = build_publish_plan(remuxed, DraftVersion::kDraft14);

    ok &= expect(progressive.tracks.size() == 1, "expected one progressive track");
    ok &= expect(!remuxed.initialization_segment.owned_bytes.empty(), "expected synthesized init segment");
    ok &= expect(remuxed.fragments.size() == 1, "expected one remuxed fragment");
    ok &= expect(!remuxed.fragments.front().payload.owned_bytes.empty(), "expected synthesized media fragment");
    ok &= expect(remuxed_plan.objects.size() == 2, "expected catalog and media objects for remuxed file");
    ok &= expect(remuxed_plan.objects.front().track_name == "catalog", "expected remuxed catalog object first");
    ok &= expect(remuxed_plan.objects[1].track_name == "vide_1", "expected remuxed track naming");
    ok &= expect(remuxed_plan.track_initializations.size() == 1, "expected one remuxed track init payload");
    ok &= expect(remuxed_plan.track_initializations.front().track_name == "vide_1",
                 "expected remuxed init payload to follow the media track name");
    ok &= expect(!remuxed_plan.track_initializations.front().codec_payload.empty(),
                 "expected remuxed codec init payload");
    ok &= expect(!remuxed_plan.track_initializations.front().init_segment.empty(),
                 "expected remuxed standalone init segment");

    const auto multitrack_init_bytes = make_multitrack_init_mp4();
    const SegmentedMp4 multitrack_segmented{
        .initialization_segment = {.span = {}, .owned_bytes = multitrack_init_bytes},
        .fragments = {},
        .tracks = {
            TrackDescription{.track_id = 1, .handler_type = "vide", .codec = "avc1", .track_name = "vide_1"},
            TrackDescription{.track_id = 2, .handler_type = "soun", .codec = "mp4a", .track_name = "soun_2"},
        },
    };
    const auto multitrack_plan = build_publish_plan(multitrack_segmented, DraftVersion::kDraft14);
    const std::string catalog_text(multitrack_plan.objects.front().owned_payload.begin(),
                                   multitrack_plan.objects.front().owned_payload.end());
    const std::string video_init_data = catalog_init_data(catalog_text, "vide_1");
    const std::string audio_init_data = catalog_init_data(catalog_text, "soun_2");
    ok &= expect(!video_init_data.empty(), "expected video initData in catalog");
    ok &= expect(!audio_init_data.empty(), "expected audio initData in catalog");
    ok &= expect(video_init_data != audio_init_data, "expected per-track initData entries to differ");
    ok &= expect(multitrack_plan.track_initializations.size() == 2, "expected per-track init payloads in plan");

    const auto video_init_bytes = base64_decode(video_init_data);
    const auto audio_init_bytes = base64_decode(audio_init_data);
    ok &= expect(multitrack_plan.track_initializations[0].codec_payload == video_init_bytes,
                 "expected emitted video codec init payload to match catalog initData");
    ok &= expect(multitrack_plan.track_initializations[1].codec_payload == audio_init_bytes,
                 "expected emitted audio codec init payload to match catalog initData");
    const auto video_init_boxes = parse_mp4_boxes(multitrack_plan.track_initializations[0].init_segment);
    const auto audio_init_boxes = parse_mp4_boxes(multitrack_plan.track_initializations[1].init_segment);
    const auto video_init_tracks = extract_tracks(video_init_boxes, multitrack_plan.track_initializations[0].init_segment);
    const auto audio_init_tracks = extract_tracks(audio_init_boxes, multitrack_plan.track_initializations[1].init_segment);
    ok &= expect(video_init_tracks.size() == 1, "expected one track in emitted video init segment");
    ok &= expect(audio_init_tracks.size() == 1, "expected one track in emitted audio init segment");
    ok &= expect(video_init_tracks.front().codec == "avc1", "expected avc1-only emitted video init segment");
    ok &= expect(audio_init_tracks.front().codec == "mp4a", "expected mp4a-only emitted audio init segment");
    ok &= expect(bytes_equal(video_init_bytes, {0x00, 0x00, 0x00, 0x0d, 0x61, 0x76, 0x63, 0x43, 0x01, 0x64, 0x00, 0x0c, 0xff}),
                 "expected video initData to contain only the avcC box");
    ok &= expect(bytes_equal(audio_init_bytes, {0x00, 0x00, 0x00, 0x10, 0x65, 0x73, 0x64, 0x73, 0x00, 0x00, 0x00, 0x00, 0x03, 0x19, 0x00, 0x02}),
                 "expected audio initData to contain only the esds box");

    return ok ? 0 : 1;
}
