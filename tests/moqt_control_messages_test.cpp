#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openmoq::publisher::DraftVersion;
using openmoq::publisher::transport::MaxRequestIdMessage;
using openmoq::publisher::transport::NamespaceMessage;
using openmoq::publisher::transport::PublishError;
using openmoq::publisher::transport::PublishNamespaceOk;
using openmoq::publisher::transport::PublishOk;
using openmoq::publisher::transport::RequestError;
using openmoq::publisher::transport::ServerSetupMessage;
using openmoq::publisher::transport::SetupMessage;
using openmoq::publisher::transport::SubscribeMessage;
using openmoq::publisher::transport::SubscribeNamespaceMessage;
using openmoq::publisher::transport::SubscribeTracksMessage;
using openmoq::publisher::transport::SubscribeUpdateMessage;
using openmoq::publisher::transport::TrackMessage;
using openmoq::publisher::transport::TransportKind;
using openmoq::publisher::transport::decode_max_request_id_message;
using openmoq::publisher::transport::decode_publish_error;
using openmoq::publisher::transport::decode_publish_ok;
using openmoq::publisher::transport::decode_request_error;
using openmoq::publisher::transport::decode_request_ok;
using openmoq::publisher::transport::decode_server_setup_message;
using openmoq::publisher::transport::decode_subscribe_message;
using openmoq::publisher::transport::decode_subscribe_namespace_message;
using openmoq::publisher::transport::decode_subscribe_tracks_message;
using openmoq::publisher::transport::decode_subscribe_update_message;
using openmoq::publisher::transport::decode_varint;
using openmoq::publisher::transport::encode_namespace_message;
using openmoq::publisher::transport::encode_publish_done_message;
using openmoq::publisher::transport::encode_publish_namespace_done_message;
using openmoq::publisher::transport::encode_request_error_message;
using openmoq::publisher::transport::encode_request_ok_message;
using openmoq::publisher::transport::encode_server_setup_message;
using openmoq::publisher::transport::encode_setup_message;
using openmoq::publisher::transport::encode_subgroup_header;
using openmoq::publisher::transport::encode_subgroup_object;
using openmoq::publisher::transport::encode_subscribe_error_message;
using openmoq::publisher::transport::encode_subscribe_namespace_ok_message;
using openmoq::publisher::transport::encode_subscribe_ok_message;
using openmoq::publisher::transport::encode_track_message;

constexpr std::uint64_t kDraft14Version = 0xff00000eULL;
constexpr std::uint64_t kDraft16Version = 0xff000010ULL;
constexpr std::uint64_t kDraft18Version = 0xff000012ULL;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
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
}

void append_string(std::vector<std::uint8_t>& out, std::string_view value) {
    append_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void append_track_namespace(std::vector<std::uint8_t>& out, const std::vector<std::string_view>& tuple) {
    append_varint(out, tuple.size());
    for (std::string_view part : tuple) {
        append_string(out, part);
    }
}

void append_uint16_length_message(std::vector<std::uint8_t>& out,
                                  std::uint64_t type,
                                  const std::vector<std::uint8_t>& payload) {
    append_varint(out, type);
    out.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
    out.insert(out.end(), payload.begin(), payload.end());
}

bool read_varint(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value) {
    return decode_varint(bytes, offset, value);
}

bool read_string(std::span<const std::uint8_t> bytes, std::size_t& offset, std::size_t end, std::string& value) {
    std::uint64_t length = 0;
    if (!read_varint(bytes, offset, length) || offset + length > end) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(length));
    offset += static_cast<std::size_t>(length);
    return true;
}

bool read_track_namespace(std::span<const std::uint8_t> bytes,
                          std::size_t& offset,
                          std::size_t end,
                          std::vector<std::string>& tuple) {
    std::uint64_t count = 0;
    if (!read_varint(bytes, offset, count)) {
        return false;
    }
    tuple.clear();
    for (std::uint64_t i = 0; i < count; ++i) {
        std::string part;
        if (!read_string(bytes, offset, end, part)) {
            return false;
        }
        tuple.push_back(part);
    }
    return true;
}

struct Uint16Frame {
    std::uint64_t type = 0;
    std::size_t payload_offset = 0;
    std::size_t payload_end = 0;
};

bool read_uint16_frame(std::span<const std::uint8_t> bytes, Uint16Frame& frame) {
    std::size_t offset = 0;
    if (!read_varint(bytes, offset, frame.type) || offset + 2 > bytes.size()) {
        return false;
    }
    const std::size_t payload_length =
        (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
    offset += 2;
    if (offset + payload_length != bytes.size()) {
        return false;
    }
    frame.payload_offset = offset;
    frame.payload_end = offset + payload_length;
    return true;
}

bool expect_uint16_frame(std::span<const std::uint8_t> bytes,
                         std::uint64_t expected_type,
                         Uint16Frame& frame,
                         const std::string& label) {
    return expect(read_uint16_frame(bytes, frame), label + " frame parses") &&
           expect(frame.type == expected_type, label + " type");
}

std::uint64_t draft_version_number(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::kDraft14:
            return kDraft14Version;
        case DraftVersion::kDraft16:
            return kDraft16Version;
        case DraftVersion::kDraft18:
            return kDraft18Version;
    }
    return 0;
}

std::string draft_label(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::kDraft14:
            return "draft-14";
        case DraftVersion::kDraft16:
            return "draft-16";
        case DraftVersion::kDraft18:
            return "draft-18";
    }
    return "unknown";
}

std::vector<std::uint8_t> build_subscribe_message(DraftVersion draft) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, 77);
    append_track_namespace(payload, {"live", "alpha"});
    append_string(payload, "catalog");

    if (draft == DraftVersion::kDraft14) {
        payload.push_back(128);  // subscriber priority
        payload.push_back(1);    // ascending group order
        payload.push_back(1);    // forward
        append_varint(payload, 3);
        append_varint(payload, 9);
        append_varint(payload, 4);
        append_varint(payload, 0);
    } else {
        append_varint(payload, 4);   // parameter count
        append_varint(payload, 16);  // FORWARD, delta from 0x00
        append_varint(payload, 1);
        append_varint(payload, 16);  // SUBSCRIBER_PRIORITY, delta from 0x10 to 0x20
        append_varint(payload, 127);
        append_varint(payload, 1);   // SUBSCRIPTION_FILTER, delta from 0x20 to 0x21
        std::vector<std::uint8_t> filter;
        append_varint(filter, 3);
        append_varint(filter, 9);
        append_varint(filter, 4);
        append_varint(payload, filter.size());
        payload.insert(payload.end(), filter.begin(), filter.end());
        append_varint(payload, 1);   // GROUP_ORDER, delta from 0x21 to 0x22
        append_varint(payload, 1);
    }

    std::vector<std::uint8_t> bytes;
    append_uint16_length_message(bytes, 0x03, payload);
    return bytes;
}

std::vector<std::uint8_t> build_subscribe_namespace_message(DraftVersion draft) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, 91);
    append_track_namespace(payload, {"live", "alpha"});
    if (draft == DraftVersion::kDraft16) {
        append_varint(payload, 1);  // subscribe options
    }
    append_varint(payload, 0);  // parameters

    std::vector<std::uint8_t> bytes;
    append_varint(bytes, 0x11);
    if (draft == DraftVersion::kDraft16) {
        bytes.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
    } else {
        append_varint(bytes, payload.size());
    }
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> build_subscribe_tracks_message() {
    std::vector<std::uint8_t> payload;
    append_varint(payload, 93);
    append_track_namespace(payload, {"live", "alpha"});
    append_varint(payload, 1);     // one delta-encoded parameter
    append_varint(payload, 0x10);  // FORWARD
    append_varint(payload, 0);

    std::vector<std::uint8_t> bytes;
    append_uint16_length_message(bytes, 0x51, payload);
    return bytes;
}

std::vector<std::uint8_t> build_subscribe_update_message() {
    std::vector<std::uint8_t> payload;
    append_varint(payload, 101);
    append_varint(payload, 77);
    append_varint(payload, 12);
    append_varint(payload, 5);
    append_varint(payload, 20);
    payload.push_back(200);
    payload.push_back(1);
    append_varint(payload, 0);

    std::vector<std::uint8_t> bytes;
    append_uint16_length_message(bytes, 0x02, payload);
    return bytes;
}

std::vector<std::uint8_t> build_publish_ok_message(DraftVersion draft) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, 55);
    if (draft == DraftVersion::kDraft14) {
        payload.push_back(1);    // forward
        payload.push_back(128);  // subscriber priority
        payload.push_back(1);    // group order
        append_varint(payload, 3);
        append_varint(payload, 12);
        append_varint(payload, 5);
        append_varint(payload, 0);
    } else {
        append_varint(payload, 4);   // parameter count
        append_varint(payload, 16);  // FORWARD
        append_varint(payload, 1);
        append_varint(payload, 16);  // SUBSCRIBER_PRIORITY
        append_varint(payload, 127);
        append_varint(payload, 1);   // SUBSCRIPTION_FILTER
        std::vector<std::uint8_t> filter;
        append_varint(filter, 3);
        append_varint(filter, 12);
        append_varint(filter, 5);
        append_varint(payload, filter.size());
        payload.insert(payload.end(), filter.begin(), filter.end());
        append_varint(payload, 1);   // GROUP_ORDER
        append_varint(payload, 1);
    }

    std::vector<std::uint8_t> bytes;
    append_uint16_length_message(bytes, 0x1e, payload);
    return bytes;
}

std::vector<std::uint8_t> build_publish_error_message() {
    std::vector<std::uint8_t> payload;
    append_varint(payload, 55);
    append_varint(payload, 2);
    append_string(payload, "nope");

    std::vector<std::uint8_t> bytes;
    append_uint16_length_message(bytes, 0x1f, payload);
    return bytes;
}

bool test_setup_serdes_for_all_drafts() {
    bool ok = true;
    for (DraftVersion draft : {DraftVersion::kDraft14, DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        const std::string label = draft_label(draft) + " setup";
        const SetupMessage setup{
            .draft = draft,
            .transport = TransportKind::kRawQuic,
            .authority = "relay.example.com:4433",
            .path = "/moq/live",
            .max_request_id = 32,
        };
        const std::vector<std::uint8_t> bytes = encode_setup_message(setup);
        Uint16Frame frame;
        ok &= expect_uint16_frame(bytes, draft == DraftVersion::kDraft18 ? 0x2f00 : 0x20, frame, label);

        std::size_t offset = frame.payload_offset;
        if (draft == DraftVersion::kDraft18) {
            bool saw_path = false;
            bool saw_authority = false;
            while (offset < frame.payload_end) {
                std::uint64_t option_type = 0;
                std::uint64_t option_length = 0;
                ok &= expect(read_varint(bytes, offset, option_type), label + " option type");
                ok &= expect(read_varint(bytes, offset, option_length), label + " option length");
                ok &= expect(offset + option_length <= frame.payload_end, label + " option length fits");
                saw_path = saw_path || option_type == 0x01;
                saw_authority = saw_authority || option_type == 0x05;
                offset += static_cast<std::size_t>(option_length);
            }
            ok &= expect(saw_path, label + " path option placement");
            ok &= expect(saw_authority, label + " authority option placement");
        } else {
            if (draft == DraftVersion::kDraft14) {
                std::uint64_t version_count = 0;
                std::uint64_t version = 0;
                ok &= expect(read_varint(bytes, offset, version_count) && version_count == 1, label + " version count");
                ok &= expect(read_varint(bytes, offset, version) && version == draft_version_number(draft), label + " version");
            }
            std::uint64_t parameter_count = 0;
            ok &= expect(read_varint(bytes, offset, parameter_count) && parameter_count == 3, label + " parameter count");
            ok &= expect(offset == frame.payload_end || parameter_count == 3, label + " payload remains valid");
        }

        const std::vector<std::uint8_t> server_bytes = encode_server_setup_message({.draft = draft, .max_request_id = 32});
        ServerSetupMessage server;
        ok &= expect(decode_server_setup_message(server_bytes, server), label + " server setup decode");
        ok &= expect(server.draft == draft, label + " server setup draft");
        if (draft != DraftVersion::kDraft18) {
            ok &= expect(server.max_request_id == 32, label + " server setup max request id");
        }
    }
    return ok;
}

bool test_publisher_control_message_encoders_for_all_drafts() {
    bool ok = true;
    for (DraftVersion draft : {DraftVersion::kDraft14, DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        const std::string label = draft_label(draft);

        const NamespaceMessage namespace_message{.draft = draft, .track_namespace = "live/alpha", .request_id = 42};
        Uint16Frame frame;
        std::size_t offset = 0;
        std::vector<std::string> tuple;
        ok &= expect_uint16_frame(encode_namespace_message(namespace_message), 0x06, frame, label + " publish namespace");
        offset = frame.payload_offset;
        std::uint64_t request_id = 0;
        std::uint64_t parameter_count = 99;
        ok &= expect(read_varint(encode_namespace_message(namespace_message), offset, request_id) && request_id == 42,
                     label + " publish namespace request id");
        const auto namespace_bytes = encode_namespace_message(namespace_message);
        offset = frame.payload_offset;
        ok &= expect(read_varint(namespace_bytes, offset, request_id) && request_id == 42,
                     label + " publish namespace request id stable");
        ok &= expect(read_track_namespace(namespace_bytes, offset, frame.payload_end, tuple) &&
                         tuple == std::vector<std::string>({"live", "alpha"}),
                     label + " publish namespace tuple placement");
        ok &= expect(read_varint(namespace_bytes, offset, parameter_count) && parameter_count == 0 && offset == frame.payload_end,
                     label + " publish namespace params");

        const TrackMessage track{
            .draft = draft,
            .track_name = "catalog",
            .track_namespace = "live/alpha",
            .request_id = 44,
            .track_alias = 7,
            .largest_group_id = 3,
            .largest_object_id = 5,
            .content_exists = true,
        };
        const auto track_bytes = encode_track_message(track);
        ok &= expect_uint16_frame(track_bytes, 0x1d, frame, label + " publish track");
        offset = frame.payload_offset;
        std::string track_name;
        std::uint64_t alias = 0;
        ok &= expect(read_varint(track_bytes, offset, request_id) && request_id == 44, label + " track request id");
        ok &= expect(read_track_namespace(track_bytes, offset, frame.payload_end, tuple) &&
                         tuple == std::vector<std::string>({"live", "alpha"}),
                     label + " track namespace tuple placement");
        ok &= expect(read_string(track_bytes, offset, frame.payload_end, track_name) && track_name == "catalog",
                     label + " track name placement");
        ok &= expect(read_varint(track_bytes, offset, alias) && alias == 7, label + " track alias placement");
        if (draft == DraftVersion::kDraft14) {
            ok &= expect(offset + 3 <= frame.payload_end, label + " draft-14 track fixed fields");
            ok &= expect(track_bytes[offset++] == 1, label + " draft-14 group order");
            ok &= expect(track_bytes[offset++] == 1, label + " draft-14 content exists");
            std::uint64_t largest_group = 0;
            std::uint64_t largest_object = 0;
            ok &= expect(read_varint(track_bytes, offset, largest_group) && largest_group == 3,
                         label + " draft-14 largest group");
            ok &= expect(read_varint(track_bytes, offset, largest_object) && largest_object == 5,
                         label + " draft-14 largest object");
            ok &= expect(track_bytes[offset++] == 1, label + " draft-14 forward preference");
        }
        ok &= expect(read_varint(track_bytes, offset, parameter_count) && parameter_count == 0 && offset == frame.payload_end,
                     label + " track params at end");
        PublishNamespaceOk ok_message;
        if (draft == DraftVersion::kDraft14) {
            std::vector<std::uint8_t> payload;
            append_varint(payload, 44);
            std::vector<std::uint8_t> namespace_ok_bytes;
            append_uint16_length_message(namespace_ok_bytes, 0x07, payload);
            ok &= expect(decode_request_ok(namespace_ok_bytes, draft, ok_message), label + " publish namespace ok decode");
            ok &= expect(ok_message.request_id == 44, label + " publish namespace ok request id");
        } else {
            ok &= expect(decode_request_ok(encode_request_ok_message(draft, 44), draft, ok_message), label + " request ok decode");
            ok &= expect(ok_message.request_id == (draft == DraftVersion::kDraft18 ? 0 : 44), label + " request ok request id");
        }
        RequestError request_error;
        if (draft == DraftVersion::kDraft14) {
            std::vector<std::uint8_t> payload;
            append_varint(payload, 44);
            append_varint(payload, 9);
            append_string(payload, "denied");
            std::vector<std::uint8_t> namespace_error_bytes;
            append_uint16_length_message(namespace_error_bytes, 0x08, payload);
            ok &= expect(decode_request_error(namespace_error_bytes, draft, request_error), label + " publish namespace error decode");
        } else {
            ok &= expect(decode_request_error(encode_request_error_message(draft, 44, 9, 1, "denied"), draft, request_error),
                         label + " request error decode");
        }
        ok &= expect(request_error.request_id == (draft == DraftVersion::kDraft18 ? 0 : 44), label + " request error request id");
        ok &= expect(request_error.error_code == 9 && request_error.reason == "denied", label + " request error fields");

        ok &= expect_uint16_frame(encode_publish_done_message(44, 2), 0x0b, frame, label + " publish done");
        ok &= expect_uint16_frame(encode_publish_namespace_done_message(namespace_message), 0x09, frame,
                                  label + " publish namespace done");
        ok &= expect_uint16_frame(encode_subscribe_ok_message(draft, 44, 7, 3, 5, true), 0x04,
                                  frame, label + " subscribe ok");
        ok &= expect_uint16_frame(encode_subscribe_error_message(44, 2, "missing"), 0x05,
                                  frame, label + " subscribe error");
    }
    return ok;
}

bool test_peer_control_message_decoders_for_all_drafts() {
    bool ok = true;
    for (DraftVersion draft : {DraftVersion::kDraft14, DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        const std::string label = draft_label(draft);

        SubscribeNamespaceMessage subscribe_namespace;
        ok &= expect(decode_subscribe_namespace_message(build_subscribe_namespace_message(draft), draft, subscribe_namespace),
                     label + " subscribe namespace decode");
        ok &= expect(subscribe_namespace.request_id == 91, label + " subscribe namespace request id");
        ok &= expect(subscribe_namespace.track_namespace_prefix == std::vector<std::string>({"live", "alpha"}),
                     label + " subscribe namespace tuple");

        SubscribeMessage subscribe;
        ok &= expect(decode_subscribe_message(build_subscribe_message(draft), draft, subscribe),
                     label + " subscribe decode");
        ok &= expect(subscribe.request_id == 77, label + " subscribe request id");
        ok &= expect(subscribe.track_namespace == std::vector<std::string>({"live", "alpha"}),
                     label + " subscribe namespace tuple");
        ok &= expect(subscribe.track_name == "catalog", label + " subscribe track name");
        ok &= expect(subscribe.forward == 1 && subscribe.filter_type == 3 && subscribe.start_group_id == 9 &&
                         subscribe.start_object_id == 4,
                     label + " subscribe filter fields");

        SubscribeTracksMessage subscribe_tracks;
        if (draft == DraftVersion::kDraft18) {
            ok &= expect(decode_subscribe_tracks_message(build_subscribe_tracks_message(), draft, subscribe_tracks),
                         label + " subscribe tracks decode");
            ok &= expect(subscribe_tracks.request_id == 93, label + " subscribe tracks request id");
            ok &= expect(subscribe_tracks.track_namespace_prefix == std::vector<std::string>({"live", "alpha"}),
                         label + " subscribe tracks namespace tuple");
            ok &= expect(subscribe_tracks.forward == 0, label + " subscribe tracks forward parameter");
        } else {
            ok &= expect(!decode_subscribe_tracks_message(build_subscribe_tracks_message(), draft, subscribe_tracks),
                         label + " subscribe tracks rejected outside draft-18");
        }

        SubscribeUpdateMessage update;
        ok &= expect(decode_subscribe_update_message(build_subscribe_update_message(), update),
                     label + " subscribe update decode");
        ok &= expect(update.request_id == 101 && update.subscription_request_id == 77 && update.start_group_id == 12 &&
                         update.start_object_id == 5 && update.end_group_plus_one == 20,
                     label + " subscribe update fields");

        PublishOk publish_ok;
        ok &= expect(decode_publish_ok(build_publish_ok_message(draft), draft, publish_ok), label + " publish ok decode");
        ok &= expect(publish_ok.request_id == 55 && publish_ok.forward == 1 && publish_ok.filter_type == 3,
                     label + " publish ok fields");

        PublishError publish_error;
        ok &= expect(decode_publish_error(build_publish_error_message(), draft, publish_error), label + " publish error decode");
        ok &= expect(publish_error.request_id == 55 && publish_error.error_code == 2 && publish_error.reason == "nope",
                     label + " publish error fields");

        MaxRequestIdMessage max_request_id;
        std::vector<std::uint8_t> max_payload;
        append_varint(max_payload, 900);
        std::vector<std::uint8_t> max_bytes;
        append_uint16_length_message(max_bytes, 0x15, max_payload);
        ok &= expect(decode_max_request_id_message(max_bytes, max_request_id), label + " max request id decode");
        ok &= expect(max_request_id.max_request_id == 900, label + " max request id field");
    }
    return ok;
}

bool test_subgroup_header_and_object_serdes_for_all_drafts() {
    bool ok = true;
    for (DraftVersion draft : {DraftVersion::kDraft14, DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        const std::string label = draft_label(draft);
        const auto header = encode_subgroup_header(draft, 7, 3, 0, true);
        std::size_t offset = 0;
        std::uint64_t type = 0;
        std::uint64_t alias = 0;
        std::uint64_t group = 0;
        ok &= expect(read_varint(header, offset, type), label + " subgroup header type");
        ok &= expect(type == (draft == DraftVersion::kDraft14 ? 0x18 : 0x38), label + " subgroup header type value");
        ok &= expect(read_varint(header, offset, alias) && alias == 7, label + " subgroup header alias placement");
        ok &= expect(read_varint(header, offset, group) && group == 3, label + " subgroup header group placement");
        ok &= expect(offset == header.size(), label + " subgroup header contains no object fields");

        const std::vector<std::uint8_t> payload = {0xaa, 0xbb, 0xcc};
        const auto first = encode_subgroup_object(draft, std::nullopt, 5, payload);
        offset = 0;
        std::uint64_t object_delta = 0;
        std::uint64_t payload_length = 0;
        ok &= expect(read_varint(first, offset, object_delta) && object_delta == 5,
                     label + " first object absolute id placement");
        ok &= expect(read_varint(first, offset, payload_length) && payload_length == payload.size(),
                     label + " object payload length placement");
        ok &= expect(offset + payload_length == first.size(), label + " payload object has no status before payload");
        ok &= expect(std::vector<std::uint8_t>(first.begin() + static_cast<std::ptrdiff_t>(offset), first.end()) == payload,
                     label + " payload bytes placement");

        const auto second = encode_subgroup_object(draft, 5, 7, payload);
        offset = 0;
        ok &= expect(read_varint(second, offset, object_delta) && object_delta == 1,
                     label + " subsequent object id delta placement");

        const auto empty = encode_subgroup_object(draft, std::nullopt, 0, {});
        offset = 0;
        std::uint64_t object_status = 99;
        ok &= expect(read_varint(empty, offset, object_delta) && object_delta == 0, label + " empty object id");
        ok &= expect(read_varint(empty, offset, payload_length) && payload_length == 0, label + " empty payload length");
        if (draft == DraftVersion::kDraft18) {
            ok &= expect(read_varint(empty, offset, object_status) && object_status == 0,
                         label + " empty object status after zero length");
        }
        ok &= expect(offset == empty.size(), label + " empty object status/payload boundary");
    }
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= test_setup_serdes_for_all_drafts();
    ok &= test_publisher_control_message_encoders_for_all_drafts();
    ok &= test_peer_control_message_decoders_for_all_drafts();
    ok &= test_subgroup_header_and_object_serdes_for_all_drafts();
    return ok ? 0 : 1;
}
