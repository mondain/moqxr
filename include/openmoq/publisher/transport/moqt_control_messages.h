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

struct MaxRequestIdMessage {
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

struct PublishNamespaceOk {
    std::uint64_t request_id = 0;
};

struct RequestError {
    std::uint64_t request_id = 0;
    std::uint64_t error_code = 0;
    std::string reason;
};

struct SubscribeNamespaceMessage {
    std::uint64_t request_id = 0;
    std::vector<std::string> track_namespace_prefix;
};

struct SubscribeMessage {
    std::uint64_t request_id = 0;
    std::vector<std::string> track_namespace;
    std::string track_name;
    std::uint8_t subscriber_priority = 0;
    std::uint8_t group_order = 0;
    std::uint8_t forward = 0;
    std::uint64_t filter_type = 0;
    std::size_t start_group_id = 0;
    std::size_t start_object_id = 0;
    std::size_t end_group_id = 0;
};

struct SubscribeUpdateMessage {
    std::uint64_t request_id = 0;
    std::uint64_t subscription_request_id = 0;
    std::size_t start_group_id = 0;
    std::size_t start_object_id = 0;
    std::size_t end_group_plus_one = 0;
    std::uint8_t subscriber_priority = 0;
    std::uint8_t forward = 0;
};

struct PublishNamespaceError {
    std::uint64_t request_id = 0;
    std::uint64_t error_code = 0;
    std::string reason;
};

struct PublishOk {
    std::uint64_t request_id = 0;
    std::uint8_t forward = 0;
    std::uint8_t subscriber_priority = 0;
    std::uint8_t group_order = 0;
    std::uint64_t filter_type = 0;
};

struct PublishError {
    std::uint64_t request_id = 0;
    std::uint64_t error_code = 0;
    std::string reason;
};

std::vector<std::uint8_t> encode_varint(std::uint64_t value);
bool decode_varint(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value);

std::vector<std::uint8_t> encode_setup_message(const SetupMessage& message);
bool decode_server_setup_message(std::span<const std::uint8_t> bytes, ServerSetupMessage& message);
std::vector<std::uint8_t> encode_server_setup_message(const ServerSetupMessage& message);
bool decode_max_request_id_message(std::span<const std::uint8_t> bytes, MaxRequestIdMessage& message);
bool next_control_message(std::span<const std::uint8_t> bytes, std::size_t& message_size);
std::vector<std::uint8_t> encode_namespace_message(const NamespaceMessage& message);
std::vector<std::uint8_t> encode_request_ok_message(std::uint64_t request_id);
bool decode_request_ok(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishNamespaceOk& message);
bool decode_request_error(std::span<const std::uint8_t> bytes, DraftVersion draft, RequestError& message);
bool decode_subscribe_namespace_message(std::span<const std::uint8_t> bytes, SubscribeNamespaceMessage& message);
std::vector<std::uint8_t> encode_subscribe_namespace_ok_message(DraftVersion draft, std::uint64_t request_id);
bool decode_subscribe_message(std::span<const std::uint8_t> bytes, SubscribeMessage& message);
bool decode_subscribe_update_message(std::span<const std::uint8_t> bytes, SubscribeUpdateMessage& message);
std::vector<std::uint8_t> encode_subscribe_ok_message(DraftVersion draft,
                                                      std::uint64_t request_id,
                                                      std::uint64_t track_alias,
                                                      std::uint8_t subscriber_priority,
                                                      std::size_t largest_group_id,
                                                      std::size_t largest_object_id,
                                                      bool content_exists);
std::vector<std::uint8_t> encode_subscribe_error_message(std::uint64_t request_id,
                                                         std::uint64_t error_code,
                                                         std::string_view reason);
std::vector<std::uint8_t> encode_track_message(const TrackMessage& message);
std::vector<std::uint8_t> encode_publish_done_message(std::uint64_t request_id, std::uint64_t stream_count);
std::vector<std::uint8_t> encode_publish_namespace_done_message(const NamespaceMessage& message);
std::vector<std::uint8_t> encode_object_stream(DraftVersion draft,
                                               std::uint64_t track_alias,
                                               const CmsfObject& object,
                                               bool end_of_group,
                                               std::span<const std::uint8_t> payload);
bool decode_publish_namespace_ok(std::span<const std::uint8_t> bytes, PublishNamespaceOk& message);
bool decode_publish_namespace_error(std::span<const std::uint8_t> bytes, PublishNamespaceError& message);
bool decode_publish_ok(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishOk& message);
bool decode_publish_error(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishError& message);

}  // namespace openmoq::publisher::transport
