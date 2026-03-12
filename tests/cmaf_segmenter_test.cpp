#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/mp4_box.h"

#include <cstdint>
#include <iostream>
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
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    const auto stsd = make_full_box("stsd",
                                    {0, 0, 0, 1, 0, 0, 0, 16, 'a', 'v', 'c', '1', 0, 0, 0, 0});
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
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 232, 0, 0, 7, 208, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    const auto stsd = make_full_box("stsd",
                                    {0, 0, 0, 1, 0, 0, 0, 16, 'a', 'v', 'c', '1', 0, 0, 0, 0});
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

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
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

    return ok ? 0 : 1;
}
