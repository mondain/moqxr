#include "openmoq/publisher/moq_draft.h"

#include <stdexcept>

namespace openmoq::publisher {

DraftProfile draft_profile(DraftVersion version) {
    switch (version) {
        case DraftVersion::kDraft14:
            return {
                .version = version,
                .subscribe_namespace_label = "Track Namespace",
                .track_alias_label = "Track Alias",
                .object_status_label = "Object Status",
                .notes = "Primary profile. Use this for baseline publisher interoperability.",
            };
        case DraftVersion::kDraft16:
            return {
                .version = version,
                .subscribe_namespace_label = "Track Namespace",
                .track_alias_label = "Track Alias",
                .object_status_label = "Object Status",
                .notes = "Secondary profile. Verify control message details against transport integration.",
            };
    }

    throw std::runtime_error("unreachable draft version");
}

std::string to_string(DraftVersion version) {
    switch (version) {
        case DraftVersion::kDraft14:
            return "draft-14";
        case DraftVersion::kDraft16:
            return "draft-16";
    }

    throw std::runtime_error("unreachable draft version");
}

}  // namespace openmoq::publisher
