#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace openmoq::publisher {

struct LiveTrack {
    std::string track_name;
};

struct LiveObject {
    std::string track_name;
    std::size_t group_id = 0;
    std::uint64_t subgroup_id = 0;
    std::size_t object_id = 0;
    std::uint64_t media_time_us = 0;
    std::uint64_t media_duration_us = 0;
    std::vector<std::uint8_t> payload;
    bool subgroup_contains_group_largest = true;
    bool final_in_subgroup = true;
};

struct LiveObjectSource {
    std::vector<LiveTrack> tracks;
    std::function<std::optional<LiveObject>()> next_object;
};

}  // namespace openmoq::publisher
