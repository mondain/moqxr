#include "openmoq/publisher/cmsf_packager.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace openmoq::publisher {

namespace {

std::string object_filename(const CmsfObject& object) {
    const char* kind = object.kind == CmsfObjectKind::kInitialization ? "init" : "media";
    return object.track_name + "_g" + std::to_string(object.group_id) + "_o" + std::to_string(object.object_id) +
           "_" + kind + ".mp4";
}

void write_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> payload) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("failed to create " + path.string());
    }
    output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
}

}  // namespace

PublishPlan build_publish_plan(const SegmentedMp4& segmented_mp4, DraftVersion version) {
    PublishPlan plan{
        .draft = draft_profile(version),
        .tracks = segmented_mp4.tracks,
        .objects = {},
    };

    plan.objects.push_back({
        .kind = CmsfObjectKind::kInitialization,
        .track_name = "init",
        .group_id = 0,
        .object_id = 0,
        .payload = segmented_mp4.initialization_segment,
    });

    for (const auto& fragment : segmented_mp4.fragments) {
        const std::size_t media_size = fragment.moof.size + fragment.mdat.size;
        plan.objects.push_back({
            .kind = CmsfObjectKind::kMedia,
            .track_name = "media",
            .group_id = fragment.sequence,
            .object_id = 0,
            .payload = {.offset = fragment.moof.offset, .size = media_size},
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
               << " bytes=" << object.payload.size << " kind="
               << (object.kind == CmsfObjectKind::kInitialization ? "init" : "media") << '\n';
    }

    return stream.str();
}

void emit_plan_objects(const PublishPlan& plan,
                       std::span<const std::uint8_t> bytes,
                       const std::filesystem::path& output_dir) {
    std::filesystem::create_directories(output_dir);

    for (const auto& object : plan.objects) {
        write_bytes(output_dir / object_filename(object), slice_bytes(bytes, object.payload));
    }

    std::ofstream manifest(output_dir / "publish-plan.txt");
    if (!manifest) {
        throw std::runtime_error("failed to create publish-plan.txt");
    }
    manifest << render_publish_plan(plan);
}

}  // namespace openmoq::publisher
