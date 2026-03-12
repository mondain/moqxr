#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace openmoq::publisher::transport {

namespace {

constexpr std::uint64_t kClientSetupType = 0x20;
constexpr std::uint64_t kServerSetupType = 0x21;
constexpr std::uint64_t kPublishNamespaceType = 0x06;
constexpr std::uint64_t kPublishNamespaceDoneType = 0x09;
constexpr std::uint64_t kPublishDoneType = 0x0b;
constexpr std::uint64_t kPublishType = 0x1d;
constexpr std::uint64_t kSubgroupHeaderType = 0x1c;
constexpr std::uint64_t kSetupParamPath = 0x1;
constexpr std::uint64_t kSetupParamMaxRequestId = 0x2;
constexpr std::uint64_t kDraft14SetupParamAuthority = 0x3;
constexpr std::uint64_t kDraft16SetupParamAuthority = 0x5;
constexpr std::uint64_t kDraft14Version = 0xff00000eULL;
constexpr std::uint64_t kDraft16Version = 0xff000010ULL;
constexpr std::uint64_t kMaxVarintValue = 4611686018427387903ULL;
constexpr std::uint64_t kPublishStatusTrackEnded = 0x2;
constexpr std::uint8_t kGroupOrderAscending = 0x1;
constexpr std::uint8_t kForwardPreference = 0x1;
constexpr std::uint8_t kContentExistsTrue = 0x1;
constexpr std::uint8_t kPublisherPriority = 0x80;

std::vector<std::uint8_t> to_bytes(std::string_view value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

void append_uint16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
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

void append_location(std::vector<std::uint8_t>& out, std::size_t group_id, std::size_t object_id) {
    append_varint(out, group_id);
    append_varint(out, object_id);
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
    append_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
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

std::uint64_t authority_parameter_type(DraftVersion draft) {
    return draft == DraftVersion::kDraft16 ? kDraft16SetupParamAuthority : kDraft14SetupParamAuthority;
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

std::vector<std::uint8_t> encode_setup_message(const SetupMessage& message) {
    std::vector<std::uint8_t> payload;

    if (message.draft == DraftVersion::kDraft14) {
        append_varint(payload, 1);
        append_varint(payload, draft_version_number(message.draft));
    }

    append_varint(payload, 3);
    const std::vector<std::uint8_t> authority = to_bytes(message.authority);
    const std::vector<std::uint8_t> path = to_bytes(message.path);
    std::vector<std::uint8_t> max_request_id;
    append_varint(max_request_id, message.max_request_id);
    append_parameter(payload, authority_parameter_type(message.draft), authority);
    append_parameter(payload, kSetupParamPath, path);
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
        std::uint64_t parameter_length = 0;
        if (!decode_varint_impl(payload_bytes, offset, parameter_type) ||
            !decode_varint_impl(payload_bytes, offset, parameter_length) ||
            offset + parameter_length > payload_end) {
            return false;
        }

        if (parameter_type == kSetupParamMaxRequestId) {
            std::size_t parameter_offset = 0;
            std::uint64_t value = 0;
            if (!decode_varint_impl(payload_bytes.subspan(offset, parameter_length), parameter_offset, value) ||
                parameter_offset != parameter_length) {
                return false;
            }
            message.max_request_id = value;
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
    append_varint(payload, 0);

    std::vector<std::uint8_t> message_bytes;
    append_varint(message_bytes, kPublishNamespaceDoneType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_object_stream(std::uint64_t track_alias,
                                               const CmsfObject& object,
                                               std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> stream_bytes;
    append_varint(stream_bytes, kSubgroupHeaderType);
    append_varint(stream_bytes, track_alias);
    append_varint(stream_bytes, object.group_id);
    append_varint(stream_bytes, object.object_id);
    stream_bytes.push_back(kPublisherPriority);
    append_varint(stream_bytes, 0);
    append_varint(stream_bytes, payload.size());
    stream_bytes.insert(stream_bytes.end(), payload.begin(), payload.end());
    return stream_bytes;
}

}  // namespace openmoq::publisher::transport
