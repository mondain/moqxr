#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/moq_draft.h"

namespace openmoq::publisher {

enum class CmsfObjectKind {
    kInitialization,
    kMedia,
};

struct CmsfObject {
    CmsfObjectKind kind = CmsfObjectKind::kInitialization;
    std::string track_name;
    std::size_t group_id = 0;
    std::size_t object_id = 0;
    ByteSpan payload;
};

struct PublishPlan {
    DraftProfile draft;
    std::vector<TrackDescription> tracks;
    std::vector<CmsfObject> objects;
};

PublishPlan build_publish_plan(const SegmentedMp4& segmented_mp4, DraftVersion version);
std::string render_publish_plan(const PublishPlan& plan);
void emit_plan_objects(const PublishPlan& plan,
                       std::span<const std::uint8_t> bytes,
                       const std::filesystem::path& output_dir);

}  // namespace openmoq::publisher
