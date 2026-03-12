#include "openmoq/publisher/cmsf_packager.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace openmoq::publisher {

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string object_filename(const CmsfObject& object) {
    if (object.kind == CmsfObjectKind::kInitialization && object.track_name == "catalog") {
        return "catalog.json";
    }

    return object.track_name + "_g" + std::to_string(object.group_id) + "_o" + std::to_string(object.object_id) +
           "_media.mp4";
}

void write_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> payload) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("failed to create " + path.string());
    }
    output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
}

std::string json_escape(std::string_view value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(ch)) << std::dec;
                } else {
                    out << ch;
                }
                break;
        }
    }
    return out.str();
}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const std::uint32_t b0 = bytes[offset];
        const std::uint32_t b1 = offset + 1 < bytes.size() ? bytes[offset + 1] : 0;
        const std::uint32_t b2 = offset + 2 < bytes.size() ? bytes[offset + 2] : 0;
        const std::uint32_t word = (b0 << 16U) | (b1 << 8U) | b2;
        encoded.push_back(kBase64Alphabet[(word >> 18U) & 0x3FU]);
        encoded.push_back(kBase64Alphabet[(word >> 12U) & 0x3FU]);
        encoded.push_back(offset + 1 < bytes.size() ? kBase64Alphabet[(word >> 6U) & 0x3FU] : '=');
        encoded.push_back(offset + 2 < bytes.size() ? kBase64Alphabet[word & 0x3FU] : '=');
    }
    return encoded;
}

std::string track_kind(std::string_view handler_type) {
    if (handler_type == "vide") {
        return "video";
    }
    if (handler_type == "soun") {
        return "audio";
    }
    return "data";
}

std::vector<std::uint8_t> build_catalog_payload(const SegmentedMp4& segmented_mp4) {
    const auto init_bytes = segmented_mp4.initialization_segment.owned_bytes.empty()
                                ? std::vector<std::uint8_t>{}
                                : segmented_mp4.initialization_segment.owned_bytes;
    if (init_bytes.empty()) {
        throw std::runtime_error("catalog generation requires owned initialization bytes");
    }

    const std::string init_data = base64_encode(init_bytes);
    std::ostringstream catalog;
    catalog << "{";
    catalog << "\"version\":1,";
    catalog << "\"format\":\"cmsf\",";
    catalog << "\"tracks\":[";
    for (std::size_t index = 0; index < segmented_mp4.tracks.size(); ++index) {
        const auto& track = segmented_mp4.tracks[index];
        if (index != 0) {
            catalog << ',';
        }
        catalog << '{'
                << "\"name\":\"" << json_escape(track.track_name) << "\","
                << "\"id\":" << track.track_id << ','
                << "\"kind\":\"" << track_kind(track.handler_type) << "\","
                << "\"handler\":\"" << json_escape(track.handler_type) << "\","
                << "\"codec\":\"" << json_escape(track.codec) << "\","
                << "\"packaging\":\"cmaf\","
                << "\"initData\":\"" << init_data << "\""
                << '}';
    }
    catalog << "]}";

    const std::string text = catalog.str();
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

}  // namespace

PublishPlan build_publish_plan(const SegmentedMp4& segmented_mp4, DraftVersion version) {
    std::vector<TrackDescription> tracks = segmented_mp4.tracks;
    tracks.insert(tracks.begin(), TrackDescription{
                                    .track_id = 0,
                                    .handler_type = "meta",
                                    .codec = "catalog",
                                    .track_name = "catalog",
                                });

    PublishPlan plan{
        .draft = draft_profile(version),
        .tracks = std::move(tracks),
        .objects = {},
    };

    const std::vector<std::uint8_t> catalog_payload = build_catalog_payload(segmented_mp4);
    plan.objects.push_back({
        .kind = CmsfObjectKind::kInitialization,
        .track_name = "catalog",
        .group_id = 0,
        .object_id = 0,
        .payload = {},
        .owned_payload = catalog_payload,
    });

    for (const auto& fragment : segmented_mp4.fragments) {
        plan.objects.push_back({
            .kind = CmsfObjectKind::kMedia,
            .track_name = fragment.track_name,
            .group_id = fragment.sequence,
            .object_id = 0,
            .payload = fragment.payload.span,
            .owned_payload = fragment.payload.owned_bytes,
        });
    }

    return plan;
}

std::string render_publish_plan(const PublishPlan& plan) {
    std::ostringstream stream;
    stream << "draft=" << to_string(plan.draft.version) << '\n';
    stream << "notes=" << plan.draft.notes << '\n';
    stream << "tracks=" << summarize_tracks(plan.tracks) << '\n';
    stream << "objects=" << plan.objects.size() << '\n';

    for (const auto& object : plan.objects) {
        stream << object.track_name << " group=" << object.group_id << " object=" << object.object_id
               << " bytes=" << (object.owned_payload.empty() ? object.payload.size : object.owned_payload.size())
               << " kind="
               << (object.kind == CmsfObjectKind::kInitialization ? "catalog" : "media") << '\n';
    }

    return stream.str();
}

PublishPlan materialize_publish_plan(const PublishPlan& plan, std::span<const std::uint8_t> bytes) {
    PublishPlan materialized = plan;

    for (auto& object : materialized.objects) {
        if (!object.owned_payload.empty() || object.payload.size == 0) {
            continue;
        }

        const auto payload = slice_bytes(bytes, object.payload);
        object.owned_payload.assign(payload.begin(), payload.end());
        object.payload = {};
    }

    return materialized;
}

void emit_plan_objects(const PublishPlan& plan,
                       std::span<const std::uint8_t> bytes,
                       const std::filesystem::path& output_dir) {
    std::filesystem::create_directories(output_dir);

    for (const auto& object : plan.objects) {
        if (object.owned_payload.empty()) {
            write_bytes(output_dir / object_filename(object), slice_bytes(bytes, object.payload));
        } else {
            write_bytes(output_dir / object_filename(object), object.owned_payload);
        }
    }

    std::ofstream manifest(output_dir / "publish-plan.txt");
    if (!manifest) {
        throw std::runtime_error("failed to create publish-plan.txt");
    }
    manifest << render_publish_plan(plan);
}

}  // namespace openmoq::publisher
