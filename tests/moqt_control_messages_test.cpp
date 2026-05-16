#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <cstdint>
#include <cstddef>
#include <optional>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using openmoq::publisher::DraftVersion;
using openmoq::publisher::transport::PublishNamespaceOk;
using openmoq::publisher::transport::RequestError;
using openmoq::publisher::transport::ServerSetupMessage;
using openmoq::publisher::transport::SetupMessage;
using openmoq::publisher::transport::TransportKind;
using openmoq::publisher::transport::decode_request_error;
using openmoq::publisher::transport::decode_request_ok;
using openmoq::publisher::transport::decode_server_setup_message;
using openmoq::publisher::transport::decode_varint;
using openmoq::publisher::transport::encode_request_ok_message;
using openmoq::publisher::transport::encode_setup_message;
using openmoq::publisher::transport::encode_subgroup_object;

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

}  // namespace

int main() {
    bool ok = true;

    {
        const SetupMessage setup{
            .draft = DraftVersion::kDraft18,
            .transport = TransportKind::kRawQuic,
            .authority = "relay.example.com:4433",
            .path = "/moq/live",
            .max_request_id = 0,
        };
        const std::vector<std::uint8_t> bytes = encode_setup_message(setup);
        std::size_t offset = 0;
        std::uint64_t type = 0;
        ok &= expect(decode_varint(bytes, offset, type) && type == 0x2f00,
                     "expected draft-18 setup message type 0x2f00");
        ok &= expect(offset + 2 <= bytes.size(), "expected uint16 payload length in setup");
        const std::size_t payload_length =
            (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
        offset += 2;
        ok &= expect(offset + payload_length == bytes.size(), "expected setup payload length to match bytes");

        bool saw_path = false;
        bool saw_authority = false;
        const std::size_t end = offset + payload_length;
        while (offset < end) {
            std::uint64_t option_type = 0;
            std::uint64_t option_length = 0;
            ok &= expect(decode_varint(bytes, offset, option_type), "expected option type in setup payload");
            ok &= expect(decode_varint(bytes, offset, option_length), "expected option length in setup payload");
            ok &= expect(offset + option_length <= end, "expected option length to fit payload");
            if (option_type == 0x01) {
                saw_path = true;
            } else if (option_type == 0x05) {
                saw_authority = true;
            }
            offset += static_cast<std::size_t>(option_length);
        }
        ok &= expect(saw_path, "expected draft-18 raw setup to include PATH option");
        ok &= expect(saw_authority, "expected draft-18 raw setup to include AUTHORITY option");
    }

    {
        const std::vector<std::uint8_t> bytes =
            openmoq::publisher::transport::encode_server_setup_message({
                .draft = DraftVersion::kDraft18,
                .max_request_id = 0,
            });
        ServerSetupMessage message;
        ok &= expect(decode_server_setup_message(std::span<const std::uint8_t>(bytes), message),
                     "expected draft-18 setup decode to succeed");
        ok &= expect(message.draft == DraftVersion::kDraft18,
                     "expected setup decode to infer draft-18");
    }

    {
        const std::vector<std::uint8_t> bytes = encode_request_ok_message(DraftVersion::kDraft18, 999);
        PublishNamespaceOk message;
        ok &= expect(decode_request_ok(std::span<const std::uint8_t>(bytes), DraftVersion::kDraft18, message),
                     "expected draft-18 request_ok decode to succeed");
        ok &= expect(message.request_id == 0, "expected draft-18 request_ok to carry no request_id field");
    }

    {
        std::vector<std::uint8_t> payload;
        append_varint(payload, 0x9);  // error_code
        append_varint(payload, 1);    // retry interval
        append_varint(payload, 4);    // reason length
        payload.insert(payload.end(), {'t', 'e', 's', 't'});
        append_varint(payload, 0);  // redirect URI length
        append_varint(payload, 1);  // track namespace tuple length
        append_varint(payload, 4);
        payload.insert(payload.end(), {'m', 'e', 'd', 'i'});
        append_varint(payload, 3);  // track name length
        payload.insert(payload.end(), {'v', 'i', 'd'});

        std::vector<std::uint8_t> bytes;
        append_varint(bytes, 0x5);  // REQUEST_ERROR
        bytes.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
        bytes.insert(bytes.end(), payload.begin(), payload.end());

        RequestError message;
        ok &= expect(decode_request_error(std::span<const std::uint8_t>(bytes), DraftVersion::kDraft18, message),
                     "expected draft-18 request_error decode with redirect to succeed");
        ok &= expect(message.error_code == 0x9, "expected request_error code to be parsed");
        ok &= expect(message.retry_interval == 1, "expected request_error retry interval to be parsed");
        ok &= expect(message.reason == "test", "expected request_error reason to be parsed");
    }

    {
        const std::vector<std::uint8_t> bytes = encode_request_ok_message(DraftVersion::kDraft16, 7);
        PublishNamespaceOk message;
        ok &= expect(decode_request_ok(std::span<const std::uint8_t>(bytes), DraftVersion::kDraft16, message),
                     "expected draft-16 request_ok decode to remain valid");
        ok &= expect(message.request_id == 7, "expected draft-16 request_ok request_id to remain parsed");
    }

    {
        const std::vector<std::uint8_t> payload = {0xaa, 0xbb, 0xcc};
        const std::vector<std::uint8_t> bytes =
            encode_subgroup_object(DraftVersion::kDraft18, std::nullopt, 0, payload);
        std::size_t offset = 0;
        std::uint64_t object_id_delta = 99;
        std::uint64_t payload_length = 99;
        ok &= expect(decode_varint(bytes, offset, object_id_delta) && object_id_delta == 0,
                     "expected draft-18 first subgroup object id delta");
        ok &= expect(decode_varint(bytes, offset, payload_length) && payload_length == payload.size(),
                     "expected draft-18 payload length immediately after object id delta");
        ok &= expect(offset + payload_length == bytes.size(), "expected draft-18 payload object to omit status");
        ok &= expect(std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end()) == payload,
                     "expected draft-18 payload bytes after length");
    }

    {
        const std::vector<std::uint8_t> bytes =
            encode_subgroup_object(DraftVersion::kDraft18, std::nullopt, 0, {});
        std::size_t offset = 0;
        std::uint64_t object_id_delta = 99;
        std::uint64_t payload_length = 99;
        std::uint64_t object_status = 99;
        ok &= expect(decode_varint(bytes, offset, object_id_delta) && object_id_delta == 0,
                     "expected draft-18 empty object id delta");
        ok &= expect(decode_varint(bytes, offset, payload_length) && payload_length == 0,
                     "expected draft-18 empty object payload length");
        ok &= expect(decode_varint(bytes, offset, object_status) && object_status == 0,
                     "expected draft-18 empty object status after zero length");
        ok &= expect(offset == bytes.size(), "expected draft-18 empty object to contain no payload");
    }

    return ok ? 0 : 1;
}
