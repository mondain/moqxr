#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace openmoq::publisher::transport {

namespace {

constexpr std::uint64_t kClientSetupType = 0x20;
constexpr std::uint64_t kServerSetupType = 0x21;
constexpr std::uint64_t kSubscribeUpdateType = 0x02;
constexpr std::uint64_t kSubscribeType = 0x03;
constexpr std::uint64_t kSubscribeOkType = 0x04;
constexpr std::uint64_t kSubscribeErrorType = 0x05;
constexpr std::uint64_t kRequestErrorType = 0x05;
constexpr std::uint64_t kPublishNamespaceType = 0x06;
constexpr std::uint64_t kPublishNamespaceOkType = 0x07;
constexpr std::uint64_t kRequestOkType = 0x07;
constexpr std::uint64_t kPublishNamespaceErrorType = 0x08;
constexpr std::uint64_t kPublishNamespaceDoneType = 0x09;
constexpr std::uint64_t kPublishDoneType = 0x0b;
constexpr std::uint64_t kSubscribeNamespaceType = 0x11;
constexpr std::uint64_t kSubscribeNamespaceOkType = 0x12;
constexpr std::uint64_t kMaxRequestIdType = 0x15;
constexpr std::uint64_t kPublishType = 0x1d;
constexpr std::uint64_t kPublishOkType = 0x1e;
constexpr std::uint64_t kPublishErrorType = 0x1f;
constexpr std::uint64_t kSubgroupHeaderType = 0x14;
constexpr std::uint64_t kSetupParamPath = 0x1;
constexpr std::uint64_t kSetupParamMaxRequestId = 0x2;
constexpr std::uint64_t kSetupParamAuthority = 0x5;
constexpr std::uint64_t kDraft14Version = 0xff00000eULL;
constexpr std::uint64_t kDraft16Version = 0xff000010ULL;
constexpr std::uint64_t kMaxVarintValue = 4611686018427387903ULL;
constexpr std::uint64_t kPublishStatusTrackEnded = 0x2;
constexpr std::uint64_t kSubscribeErrorTrackDoesNotExist = 0x2;
constexpr std::uint8_t kGroupOrderAscending = 0x1;
constexpr std::uint8_t kForwardPreference = 0x1;
constexpr std::uint8_t kContentExistsTrue = 0x1;
constexpr std::uint8_t kPublisherPriority = 0x00;

bool decode_varint_impl(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value);

std::vector<std::uint8_t> to_bytes(std::string_view value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

void append_uint16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

bool parse_uint16_length_message(std::span<const std::uint8_t> bytes,
                                 std::uint64_t expected_type,
                                 std::size_t& payload_offset,
                                 std::size_t& payload_length) {
    std::size_t offset = 0;
    std::uint64_t type = 0;
    if (!decode_varint_impl(bytes, offset, type) || type != expected_type || offset + 2 > bytes.size()) {
        return false;
    }
    payload_length =
        (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
    offset += 2;
    if (offset + payload_length != bytes.size()) {
        return false;
    }
    payload_offset = offset;
    return true;
}

bool parse_varint_length_message(std::span<const std::uint8_t> bytes,
                                 std::uint64_t expected_type,
                                 std::size_t& payload_offset,
                                 std::size_t& payload_length) {
    std::size_t offset = 0;
    std::uint64_t type = 0;
    std::uint64_t length = 0;
    if (!decode_varint_impl(bytes, offset, type) || type != expected_type || !decode_varint_impl(bytes, offset, length)) {
        return false;
    }
    if (offset + length != bytes.size()) {
        return false;
    }
    payload_offset = offset;
    payload_length = static_cast<std::size_t>(length);
    return true;
}

bool decode_reason_phrase(std::span<const std::uint8_t> bytes, std::size_t& offset, std::string& reason) {
    std::uint64_t length = 0;
    if (!decode_varint_impl(bytes, offset, length) || offset + length > bytes.size()) {
        return false;
    }
    reason.assign(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(length));
    offset += static_cast<std::size_t>(length);
    return true;
}

bool append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    if (value > kMaxVarintValue) {
        return false;
    }

    if (value <= 63) {
        out.push_back(static_cast<std::uint8_t>(value));
    } else if (value <= 16383) {
        out.push_back(static_cast<std::uint8_t>(0x40 | ((value >> 8) & 0x3f)));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else if (value <= 1073741823ULL) {
        out.push_back(static_cast<std::uint8_t>(0x80 | ((value >> 24) & 0x3f)));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else {
        out.push_back(static_cast<std::uint8_t>(0xc0 | ((value >> 56) & 0x3f)));
        out.push_back(static_cast<std::uint8_t>((value >> 48) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 40) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 32) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    }

    return true;
}

void append_string(std::vector<std::uint8_t>& out, std::string_view value) {
    append_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void append_track_namespace(std::vector<std::uint8_t>& out, std::string_view track_namespace) {
    append_varint(out, 1);
    append_string(out, track_namespace);
}

void append_track_namespace(std::vector<std::uint8_t>& out, const std::vector<std::string>& track_namespace) {
    append_varint(out, track_namespace.size());
    for (const auto& part : track_namespace) {
        append_string(out, part);
    }
}

void append_location(std::vector<std::uint8_t>& out, std::size_t group_id, std::size_t object_id) {
    append_varint(out, group_id);
    append_varint(out, object_id);
}

bool decode_track_namespace(std::span<const std::uint8_t> bytes,
                            std::size_t& offset,
                            std::vector<std::string>& track_namespace) {
    std::uint64_t entry_count = 0;
    if (!decode_varint_impl(bytes, offset, entry_count)) {
        return false;
    }
    track_namespace.clear();
    track_namespace.reserve(static_cast<std::size_t>(entry_count));
    for (std::uint64_t index = 0; index < entry_count; ++index) {
        std::uint64_t length = 0;
        if (!decode_varint_impl(bytes, offset, length) || offset + length > bytes.size()) {
            return false;
        }
        track_namespace.emplace_back(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(length));
        offset += static_cast<std::size_t>(length);
    }
    return true;
}

bool decode_varint_impl(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value) {
    if (offset >= bytes.size()) {
        return false;
    }

    const std::uint8_t first = bytes[offset];
    const std::size_t length = 1ULL << (first >> 6);
    if (offset + length > bytes.size()) {
        return false;
    }

    value = first & 0x3f;
    for (std::size_t index = 1; index < length; ++index) {
        value = (value << 8) | bytes[offset + index];
    }
    offset += length;
    return true;
}

void append_parameter(std::vector<std::uint8_t>& out,
                      std::uint64_t type,
                      std::span<const std::uint8_t> value) {
    append_varint(out, type);
    if ((type & 0x1ULL) == 0) {
        out.insert(out.end(), value.begin(), value.end());
    } else {
        append_varint(out, value.size());
        out.insert(out.end(), value.begin(), value.end());
    }
}

std::uint64_t draft_version_number(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::kDraft14:
            return kDraft14Version;
        case DraftVersion::kDraft16:
            return kDraft16Version;
    }

    return kDraft16Version;
}

}  // namespace

std::vector<std::uint8_t> encode_varint(std::uint64_t value) {
    std::vector<std::uint8_t> bytes;
    if (!append_varint(bytes, value)) {
        return {};
    }
    return bytes;
}

bool decode_varint(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value) {
    return decode_varint_impl(bytes, offset, value);
}

bool next_control_message(std::span<const std::uint8_t> bytes, std::size_t& message_size) {
    std::size_t offset = 0;
    std::uint64_t type = 0;
    if (!decode_varint_impl(bytes, offset, type)) {
        return false;
    }

    switch (type) {
        case kClientSetupType:
        case kServerSetupType:
        case kSubscribeUpdateType:
        case kSubscribeType:
        case kSubscribeOkType:
        case kSubscribeErrorType:
        case kPublishNamespaceType:
        case kPublishNamespaceOkType:
        case kPublishNamespaceErrorType:
        case kPublishNamespaceDoneType:
        case kPublishDoneType: {
            if (offset + 2 > bytes.size()) {
                return false;
            }
            const std::size_t payload_length =
                (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
            message_size = offset + 2 + payload_length;
            return bytes.size() >= message_size;
        }
        case kSubscribeNamespaceType:
        case kSubscribeNamespaceOkType:
        case kPublishType:
        case kPublishOkType:
        case kPublishErrorType: {
            if (type == kPublishType || type == kPublishOkType || type == kPublishErrorType) {
                if (offset + 2 > bytes.size()) {
                    return false;
                }
                const std::size_t payload_length =
                    (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
                message_size = offset + 2 + payload_length;
                return bytes.size() >= message_size;
            }
            std::uint64_t payload_length = 0;
            if (!decode_varint_impl(bytes, offset, payload_length)) {
                return false;
            }
            message_size = offset + static_cast<std::size_t>(payload_length);
            return bytes.size() >= message_size;
        }
        case kMaxRequestIdType: {
            if (offset + 2 > bytes.size()) {
                return false;
            }
            const std::size_t payload_length =
                (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
            message_size = offset + 2 + payload_length;
            return bytes.size() >= message_size;
        }
        default:
            return false;
    }
}

std::vector<std::uint8_t> encode_setup_message(const SetupMessage& message) {
    std::vector<std::uint8_t> payload;

    if (message.draft == DraftVersion::kDraft14) {
        append_varint(payload, 1);
        append_varint(payload, draft_version_number(message.draft));
    }

    const std::vector<std::uint8_t> authority = to_bytes(message.authority);
    const std::vector<std::uint8_t> path = to_bytes(message.path);
    append_varint(payload, 3);
    append_parameter(payload, kSetupParamAuthority, authority);
    append_parameter(payload, kSetupParamPath, path);
    std::vector<std::uint8_t> max_request_id;
    append_varint(max_request_id, message.max_request_id);
    append_parameter(payload, kSetupParamMaxRequestId, max_request_id);

    std::vector<std::uint8_t> message_bytes;
    append_varint(message_bytes, kClientSetupType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

bool decode_server_setup_message(std::span<const std::uint8_t> bytes, ServerSetupMessage& message) {
    std::size_t offset = 0;
    std::uint64_t message_type = 0;
    if (!decode_varint_impl(bytes, offset, message_type) || message_type != kServerSetupType) {
        return false;
    }

    if (offset + 2 > bytes.size()) {
        return false;
    }
    const std::size_t payload_length =
        (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
    offset += 2;
    if (offset + payload_length > bytes.size()) {
        return false;
    }

    const std::size_t payload_end = offset + payload_length;
    const auto payload_bytes = bytes.subspan(0, payload_end);
    if (payload_end == offset) {
        return false;
    }

    const std::uint8_t first_payload_byte = bytes[offset];
    if ((first_payload_byte & 0xc0) == 0xc0) {
        std::uint64_t selected_version = 0;
        if (!decode_varint_impl(payload_bytes, offset, selected_version) || selected_version != kDraft14Version) {
            return false;
        }
        message.draft = DraftVersion::kDraft14;
    } else {
        message.draft = DraftVersion::kDraft16;
    }

    std::uint64_t parameter_count = 0;
    if (!decode_varint_impl(payload_bytes, offset, parameter_count)) {
        return false;
    }

    for (std::uint64_t parameter_index = 0; parameter_index < parameter_count; ++parameter_index) {
        std::uint64_t parameter_type = 0;
        if (!decode_varint_impl(payload_bytes, offset, parameter_type)) {
            return false;
        }

        if ((parameter_type & 0x1ULL) == 0) {
            std::uint64_t value = 0;
            if (!decode_varint_impl(payload_bytes, offset, value)) {
                return false;
            }
            if (parameter_type == kSetupParamMaxRequestId) {
                message.max_request_id = value;
            }
            continue;
        }

        std::uint64_t parameter_length = 0;
        if (!decode_varint_impl(payload_bytes, offset, parameter_length) || offset + parameter_length > payload_end) {
            return false;
        }
        offset += parameter_length;
    }

    return offset == payload_end;
}

std::vector<std::uint8_t> encode_server_setup_message(const ServerSetupMessage& message) {
    std::vector<std::uint8_t> payload;
    if (message.draft == DraftVersion::kDraft14) {
        append_varint(payload, draft_version_number(message.draft));
    }

    append_varint(payload, 1);
    std::vector<std::uint8_t> max_request_id;
    append_varint(max_request_id, message.max_request_id);
    append_parameter(payload, kSetupParamMaxRequestId, max_request_id);

    std::vector<std::uint8_t> message_bytes;
    append_varint(message_bytes, kServerSetupType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

bool decode_max_request_id_message(std::span<const std::uint8_t> bytes, MaxRequestIdMessage& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, kMaxRequestIdType, payload_offset, payload_length)) {
        return false;
    }
    std::size_t offset = payload_offset;
    return decode_varint_impl(bytes, offset, message.max_request_id) && offset == payload_offset + payload_length;
}

std::vector<std::uint8_t> encode_namespace_message(const NamespaceMessage& message) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, message.request_id);
    append_track_namespace(payload, message.track_namespace);
    append_varint(payload, 0);

    std::vector<std::uint8_t> message_bytes;
    append_varint(message_bytes, kPublishNamespaceType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_request_ok_message(std::uint64_t request_id) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, request_id);
    append_varint(payload, 0);

    std::vector<std::uint8_t> message_bytes;
    append_varint(message_bytes, kRequestOkType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

bool decode_request_ok(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishNamespaceOk& message) {
    if (draft == DraftVersion::kDraft14) {
        return decode_publish_namespace_ok(bytes, message);
    }

    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, kRequestOkType, payload_offset, payload_length)) {
        return false;
    }
    std::size_t offset = payload_offset;
    std::uint64_t parameter_count = 0;
    return decode_varint_impl(bytes, offset, message.request_id) &&
           decode_varint_impl(bytes, offset, parameter_count) && parameter_count == 0 &&
           offset == payload_offset + payload_length;
}

bool decode_request_error(std::span<const std::uint8_t> bytes, DraftVersion draft, RequestError& message) {
    if (draft == DraftVersion::kDraft14) {
        PublishNamespaceError namespace_error;
        if (!decode_publish_namespace_error(bytes, namespace_error)) {
            return false;
        }
        message.request_id = namespace_error.request_id;
        message.error_code = namespace_error.error_code;
        message.reason = std::move(namespace_error.reason);
        return true;
    }

    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, kRequestErrorType, payload_offset, payload_length)) {
        return false;
    }
    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    std::uint64_t parameter_count = 0;
    return decode_varint_impl(bytes, offset, message.request_id) &&
           decode_varint_impl(bytes, offset, message.error_code) &&
           decode_reason_phrase(bytes.subspan(0, payload_end), offset, message.reason) &&
           decode_varint_impl(bytes, offset, parameter_count) && parameter_count == 0 && offset == payload_end;
}

bool decode_subscribe_namespace_message(std::span<const std::uint8_t> bytes, SubscribeNamespaceMessage& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_varint_length_message(bytes, kSubscribeNamespaceType, payload_offset, payload_length)) {
        return false;
    }
    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    std::uint64_t parameters = 0;
    return decode_varint_impl(bytes, offset, message.request_id) &&
           decode_track_namespace(bytes.subspan(0, payload_end), offset, message.track_namespace_prefix) &&
           decode_varint_impl(bytes, offset, parameters) && parameters == 0 && offset == payload_end;
}

std::vector<std::uint8_t> encode_subscribe_namespace_ok_message(DraftVersion draft, std::uint64_t request_id) {
    if (draft == DraftVersion::kDraft16) {
        return encode_request_ok_message(request_id);
    }

    std::vector<std::uint8_t> payload;
    append_varint(payload, request_id);

    std::vector<std::uint8_t> message_bytes;
    append_varint(message_bytes, kSubscribeNamespaceOkType);
    append_varint(message_bytes, payload.size());
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

bool decode_subscribe_message(std::span<const std::uint8_t> bytes, SubscribeMessage& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, kSubscribeType, payload_offset, payload_length)) {
        return false;
    }

    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    std::uint64_t track_name_length = 0;
    std::uint64_t parameter_count = 0;
    if (!decode_varint_impl(bytes, offset, message.request_id) ||
        !decode_track_namespace(bytes.subspan(0, payload_end), offset, message.track_namespace) ||
        !decode_varint_impl(bytes, offset, track_name_length) ||
        offset + track_name_length > payload_end) {
        return false;
    }

    message.track_name.assign(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(track_name_length));
    offset += static_cast<std::size_t>(track_name_length);

    if (offset + 3 > payload_end) {
        return false;
    }
    message.subscriber_priority = bytes[offset++];
    message.group_order = bytes[offset++];
    message.forward = bytes[offset++];
    if (message.group_order > 2 || message.forward > 1) {
        return false;
    }

    if (!decode_varint_impl(bytes, offset, message.filter_type)) {
        return false;
    }

    if (message.filter_type < 0x01 || message.filter_type > 0x04) {
        return false;
    }

    if (message.filter_type == 0x03 || message.filter_type == 0x04) {
        std::uint64_t group_id = 0;
        std::uint64_t object_id = 0;
        if (!decode_varint_impl(bytes, offset, group_id) || !decode_varint_impl(bytes, offset, object_id)) {
            return false;
        }
        message.start_group_id = static_cast<std::size_t>(group_id);
        message.start_object_id = static_cast<std::size_t>(object_id);
        if (message.filter_type == 0x04) {
            std::uint64_t end_group_id = 0;
            if (!decode_varint_impl(bytes, offset, end_group_id)) {
                return false;
            }
            message.end_group_id = static_cast<std::size_t>(end_group_id);
        }
    } else {
        message.start_group_id = 0;
        message.start_object_id = 0;
        message.end_group_id = 0;
    }

    return decode_varint_impl(bytes, offset, parameter_count) && parameter_count == 0 && offset == payload_end;
}

bool decode_subscribe_update_message(std::span<const std::uint8_t> bytes, SubscribeUpdateMessage& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, kSubscribeUpdateType, payload_offset, payload_length)) {
        return false;
    }

    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    std::uint64_t start_group_id = 0;
    std::uint64_t start_object_id = 0;
    std::uint64_t end_group_plus_one = 0;
    std::uint64_t parameter_count = 0;
    if (!decode_varint_impl(bytes, offset, message.request_id) ||
        !decode_varint_impl(bytes, offset, message.subscription_request_id) ||
        !decode_varint_impl(bytes, offset, start_group_id) ||
        !decode_varint_impl(bytes, offset, start_object_id) ||
        !decode_varint_impl(bytes, offset, end_group_plus_one) ||
        offset + 2 > payload_end) {
        return false;
    }

    message.start_group_id = static_cast<std::size_t>(start_group_id);
    message.start_object_id = static_cast<std::size_t>(start_object_id);
    message.end_group_plus_one = static_cast<std::size_t>(end_group_plus_one);
    message.subscriber_priority = bytes[offset++];
    message.forward = bytes[offset++];
    if (message.forward > 1) {
        return false;
    }

    return decode_varint_impl(bytes, offset, parameter_count) && parameter_count == 0 && offset == payload_end;
}

std::vector<std::uint8_t> encode_subscribe_ok_message(DraftVersion draft,
                                                      std::uint64_t request_id,
                                                      std::uint64_t track_alias,
                                                      std::uint8_t subscriber_priority,
                                                      std::size_t largest_group_id,
                                                      std::size_t largest_object_id,
                                                      bool content_exists) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, request_id);
    if (draft == DraftVersion::kDraft14) {
        append_varint(payload, 0);
        payload.push_back(subscriber_priority);
        payload.push_back(kGroupOrderAscending);
        payload.push_back(content_exists ? kContentExistsTrue : 0);
        if (content_exists) {
            append_location(payload, largest_group_id, largest_object_id);
        }
        append_varint(payload, 0);
        append_varint(payload, track_alias);
    } else {
        append_varint(payload, track_alias);
        append_varint(payload, 0);
    }

    std::vector<std::uint8_t> message_bytes;
    append_varint(message_bytes, kSubscribeOkType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_subscribe_error_message(std::uint64_t request_id,
                                                         std::uint64_t error_code,
                                                         std::string_view reason) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, request_id);
    append_varint(payload, error_code);
    append_string(payload, reason);
    append_varint(payload, 0);

    std::vector<std::uint8_t> message_bytes;
    append_varint(message_bytes, kSubscribeErrorType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_track_message(const TrackMessage& message) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, message.request_id);
    append_track_namespace(payload, message.track_namespace);
    append_string(payload, message.track_name);
    append_varint(payload, message.track_alias);

    if (message.draft == DraftVersion::kDraft14) {
        payload.push_back(kGroupOrderAscending);
        payload.push_back(message.content_exists ? kContentExistsTrue : 0);
        if (message.content_exists) {
            append_location(payload, message.largest_group_id, message.largest_object_id);
        }
        payload.push_back(kForwardPreference);
    }

    append_varint(payload, 0);
    if (message.draft == DraftVersion::kDraft16) {
        // No Track Extensions are needed for the current draft-16 publish path.
    }

    std::vector<std::uint8_t> message_bytes;
    append_varint(message_bytes, kPublishType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_publish_done_message(std::uint64_t request_id, std::uint64_t stream_count) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, request_id);
    append_varint(payload, kPublishStatusTrackEnded);
    append_varint(payload, stream_count);
    append_varint(payload, 0);

    std::vector<std::uint8_t> message_bytes;
    append_varint(message_bytes, kPublishDoneType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_publish_namespace_done_message(const NamespaceMessage& message) {
    std::vector<std::uint8_t> payload;
    if (message.draft == DraftVersion::kDraft14) {
        append_track_namespace(payload, message.track_namespace);
    } else {
        append_varint(payload, message.request_id);
    }

    std::vector<std::uint8_t> message_bytes;
    append_varint(message_bytes, kPublishNamespaceDoneType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_object_stream(DraftVersion draft,
                                               std::uint64_t track_alias,
                                               const CmsfObject& object,
                                               std::span<const std::uint8_t> payload) {
    static_cast<void>(draft);

    if (draft == DraftVersion::kDraft14) {
        std::vector<std::uint8_t> stream_bytes;
        append_varint(stream_bytes, kSubgroupHeaderType);
        append_varint(stream_bytes, track_alias);
        append_varint(stream_bytes, object.group_id);
        append_varint(stream_bytes, 0);
        stream_bytes.push_back(kPublisherPriority);
        append_varint(stream_bytes, object.object_id);
        append_varint(stream_bytes, payload.size());
        stream_bytes.insert(stream_bytes.end(), payload.begin(), payload.end());
        return stream_bytes;
    }

    std::vector<std::uint8_t> stream_bytes;
    append_varint(stream_bytes, kSubgroupHeaderType);
    append_varint(stream_bytes, track_alias);
    append_varint(stream_bytes, object.group_id);
    append_varint(stream_bytes, 0);
    stream_bytes.push_back(kPublisherPriority);
    append_varint(stream_bytes, object.object_id);
    append_varint(stream_bytes, payload.size());
    stream_bytes.insert(stream_bytes.end(), payload.begin(), payload.end());
    return stream_bytes;
}

bool decode_publish_namespace_ok(std::span<const std::uint8_t> bytes, PublishNamespaceOk& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, kPublishNamespaceOkType, payload_offset, payload_length)) {
        return false;
    }
    std::size_t offset = payload_offset;
    return decode_varint_impl(bytes, offset, message.request_id) && offset == payload_offset + payload_length;
}

bool decode_publish_namespace_error(std::span<const std::uint8_t> bytes, PublishNamespaceError& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, kPublishNamespaceErrorType, payload_offset, payload_length)) {
        return false;
    }
    std::size_t offset = payload_offset;
    return decode_varint_impl(bytes, offset, message.request_id) && decode_varint_impl(bytes, offset, message.error_code) &&
           decode_reason_phrase(bytes.subspan(0, payload_offset + payload_length), offset, message.reason) &&
           offset == payload_offset + payload_length;
}

bool decode_publish_ok(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishOk& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, kPublishOkType, payload_offset, payload_length)) {
        return false;
    }
    if (payload_offset + payload_length > bytes.size()) {
        return false;
    }
    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    if (!decode_varint_impl(bytes, offset, message.request_id)) {
        return false;
    }

    if (draft == DraftVersion::kDraft14) {
        std::uint64_t parameter_count = 0;
        if (offset + 3 > payload_end) {
            return false;
        }
        message.forward = bytes[offset++];
        message.subscriber_priority = bytes[offset++];
        message.group_order = bytes[offset++];
        if (!decode_varint_impl(bytes, offset, message.filter_type)) {
            return false;
        }

        if (message.filter_type == 0x03 || message.filter_type == 0x04) {
            std::uint64_t start_group_id = 0;
            std::uint64_t start_object_id = 0;
            if (!decode_varint_impl(bytes, offset, start_group_id) || !decode_varint_impl(bytes, offset, start_object_id)) {
                return false;
            }
            if (message.filter_type == 0x04) {
                std::uint64_t end_group_id = 0;
                if (!decode_varint_impl(bytes, offset, end_group_id)) {
                    return false;
                }
            }
        }

        if (!decode_varint_impl(bytes, offset, parameter_count)) {
            return false;
        }
        for (std::uint64_t index = 0; index < parameter_count; ++index) {
            std::uint64_t parameter_type = 0;
            std::uint64_t parameter_length = 0;
            if (!decode_varint_impl(bytes, offset, parameter_type) ||
                !decode_varint_impl(bytes, offset, parameter_length) ||
                offset + parameter_length > payload_end) {
                return false;
            }
            offset += static_cast<std::size_t>(parameter_length);
        }
        return offset == payload_end;
    }

    std::uint64_t parameter_count = 0;
    if (!decode_varint_impl(bytes, offset, parameter_count)) {
        return false;
    }
    for (std::uint64_t index = 0; index < parameter_count; ++index) {
        std::uint64_t parameter_type = 0;
        std::uint64_t parameter_length = 0;
        if (!decode_varint_impl(bytes, offset, parameter_type) ||
            !decode_varint_impl(bytes, offset, parameter_length) ||
            offset + parameter_length > payload_end) {
            return false;
        }
        offset += static_cast<std::size_t>(parameter_length);
    }
    return offset == payload_end;
}

bool decode_publish_error(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishError& message) {
    static_cast<void>(draft);
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, kPublishErrorType, payload_offset, payload_length)) {
        return false;
    }
    if (payload_offset + payload_length > bytes.size()) {
        return false;
    }
    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    return decode_varint_impl(bytes, offset, message.request_id) && decode_varint_impl(bytes, offset, message.error_code) &&
           decode_reason_phrase(bytes.subspan(0, payload_end), offset, message.reason) && offset == payload_end;
}

}  // namespace openmoq::publisher::transport
