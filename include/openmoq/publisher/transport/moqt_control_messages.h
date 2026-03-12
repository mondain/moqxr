#pragma once

#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/moq_draft.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::publisher::transport {

struct SetupMessage {
    DraftVersion draft = DraftVersion::kDraft14;
    std::string authority;
    std::string path = "/";
    std::uint64_t max_request_id = 0;
};

struct ServerSetupMessage {
    DraftVersion draft = DraftVersion::kDraft14;
    std::uint64_t max_request_id = 0;
};

struct NamespaceMessage {
    DraftVersion draft = DraftVersion::kDraft14;
    std::string track_namespace = "media";
    std::uint64_t request_id = 0;
};

struct TrackMessage {
    DraftVersion draft = DraftVersion::kDraft14;
    std::string track_name;
    std::string track_namespace = "media";
    std::uint64_t request_id = 0;
    std::uint64_t track_alias = 0;
    std::size_t largest_group_id = 0;
    std::size_t largest_object_id = 0;
    bool content_exists = false;
};

std::vector<std::uint8_t> encode_varint(std::uint64_t value);
bool decode_varint(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value);

std::vector<std::uint8_t> encode_setup_message(const SetupMessage& message);
bool decode_server_setup_message(std::span<const std::uint8_t> bytes, ServerSetupMessage& message);
std::vector<std::uint8_t> encode_server_setup_message(const ServerSetupMessage& message);
std::vector<std::uint8_t> encode_namespace_message(const NamespaceMessage& message);
std::vector<std::uint8_t> encode_track_message(const TrackMessage& message);
std::vector<std::uint8_t> encode_publish_done_message(std::uint64_t request_id, std::uint64_t stream_count);
std::vector<std::uint8_t> encode_publish_namespace_done_message(const NamespaceMessage& message);
std::vector<std::uint8_t> encode_object_stream(std::uint64_t track_alias,
                                               const CmsfObject& object,
                                               std::span<const std::uint8_t> payload);

}  // namespace openmoq::publisher::transport
