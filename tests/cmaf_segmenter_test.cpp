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

std::vector<std::uint8_t> make_test_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    const auto stsd = make_full_box("stsd",
                                    {0, 0, 0, 1, 0, 0, 0, 16, 'a', 'v', 'c', '1', 0, 0, 0, 0});
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({hdlr, minf}));
    const auto trak = make_box("trak", mdia);
    const auto moov = make_box("moov", trak);
    const auto moof = make_box("moof", {'m', 'f', 'h', 'd'});
    const auto mdat = make_box("mdat", {1, 2, 3, 4, 5, 6});
    return concat({ftyp, moov, moof, mdat});
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

    const auto bytes = make_test_mp4();
    ParsedMp4 parsed{
        .bytes = bytes,
        .top_level_boxes = parse_mp4_boxes(bytes),
        .tracks = {},
    };
    parsed.tracks = extract_tracks(parsed.top_level_boxes, parsed.bytes);

    const auto segmented = segment_for_cmaf(parsed);
    const auto plan = build_publish_plan(segmented, DraftVersion::kDraft14);

    bool ok = true;
    ok &= expect(parsed.top_level_boxes.size() == 4, "expected 4 top-level boxes");
    ok &= expect(parsed.tracks.size() == 1, "expected one extracted track");
    ok &= expect(parsed.tracks.front().codec == "avc1", "expected avc1 codec");
    ok &= expect(segmented.fragments.size() == 1, "expected one media fragment");
    ok &= expect(plan.objects.size() == 2, "expected init plus one media object");
    ok &= expect(plan.objects.front().payload.size > 0, "expected non-empty init payload");
    ok &= expect(plan.objects.back().payload.size == segmented.fragments.front().moof.size +
                                                   segmented.fragments.front().mdat.size,
                 "expected combined moof+mdat media payload");

    return ok ? 0 : 1;
}
