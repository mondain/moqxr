#include "openmoq/publisher/cli_options.h"

#include <stdexcept>
#include <string_view>

namespace openmoq::publisher {

namespace {

DraftVersion parse_draft(std::string_view value) {
    if (value == "14") {
        return DraftVersion::kDraft14;
    }
    if (value == "16") {
        return DraftVersion::kDraft16;
    }

    throw std::runtime_error("unsupported draft value: expected 14 or 16");
}

transport::EndpointConfig parse_endpoint(std::string_view value) {
    transport::EndpointConfig endpoint;
    std::string_view authority = value;

    constexpr std::string_view kScheme = "moqt://";
    if (authority.starts_with(kScheme)) {
        authority.remove_prefix(kScheme.size());
        const std::size_t slash = authority.find('/');
        if (slash == std::string_view::npos) {
            endpoint.path = "/";
        } else {
            endpoint.path = std::string(authority.substr(slash));
            authority = authority.substr(0, slash);
        }
    }

    const std::size_t colon = authority.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= authority.size()) {
        throw std::runtime_error("endpoint must be in host:port or moqt://host:port/path form");
    }

    endpoint.host = std::string(authority.substr(0, colon));
    const std::string port_text(authority.substr(colon + 1));
    const int port = std::stoi(port_text);
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("endpoint port must be between 1 and 65535");
    }
    endpoint.port = static_cast<std::uint16_t>(port);
    return endpoint;
}

bool parse_forward_flag(std::string_view value) {
    if (value == "0") {
        return false;
    }
    if (value == "1") {
        return true;
    }
    throw std::runtime_error("unsupported --forward value: expected 0 or 1");
}

std::chrono::seconds parse_timeout(std::string_view value) {
    const int timeout = std::stoi(std::string(value));
    if (timeout < 0) {
        throw std::runtime_error("subscriber timeout must be zero or greater");
    }
    return std::chrono::seconds(timeout);
}

}  // namespace

CliOptions parse_cli_options(int argc, char** argv) {
    CliOptions options;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];

        auto require_value = [&](const char* flag) -> std::string_view {
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + flag);
            }
            ++index;
            return argv[index];
        };

        if (argument == "--input") {
            options.input_path = require_value("--input");
        } else if (argument == "--endpoint") {
            options.endpoint = parse_endpoint(require_value("--endpoint"));
        } else if (argument == "--alpn") {
            if (!options.endpoint.has_value()) {
                options.endpoint = transport::EndpointConfig{};
            }
            options.endpoint->alpn = std::string(require_value("--alpn"));
            options.endpoint_alpn_overridden = true;
        } else if (argument == "--cert") {
            options.tls.certificate_path = std::string(require_value("--cert"));
        } else if (argument == "--key") {
            options.tls.private_key_path = std::string(require_value("--key"));
        } else if (argument == "--ca") {
            options.tls.ca_path = std::string(require_value("--ca"));
        } else if (argument == "--insecure") {
            options.tls.insecure_skip_verify = true;
        } else if (argument == "--draft") {
            options.draft_version = parse_draft(require_value("--draft"));
        } else if (argument == "--namespace") {
            options.track_namespace = std::string(require_value("--namespace"));
        } else if (argument == "--forward") {
            options.forward = parse_forward_flag(require_value("--forward"));
        } else if (argument == "--timeout") {
            options.subscriber_timeout = parse_timeout(require_value("--timeout"));
        } else if (argument == "--paced") {
            options.paced = true;
        } else if (argument == "--emit-dir") {
            options.emit_dir = std::filesystem::path(require_value("--emit-dir"));
        } else if (argument == "--dump-plan") {
            options.dump_plan = true;
        } else if (argument == "--help" || argument == "-h") {
            throw std::runtime_error("");
        } else {
            throw std::runtime_error(std::string("unknown argument: ") + std::string(argument));
        }
    }

    if (options.input_path.empty()) {
        throw std::runtime_error("missing required --input argument");
    }

    if (options.endpoint.has_value() && options.endpoint->host.empty()) {
        throw std::runtime_error("--alpn requires --endpoint to be provided first");
    }
    if (options.track_namespace.empty()) {
        throw std::runtime_error("--namespace must not be empty");
    }

    return options;
}

std::string build_usage(const char* argv0) {
    return std::string("Usage: ") + argv0 +
           " --input <mp4> [--draft 14|16] [--namespace <value>] [--forward 0|1] [--timeout <seconds>]"
           " [--paced] [--dump-plan] [--emit-dir <dir>]"
           " [--endpoint host:port|moqt://host:port/path] [--alpn value]"
           " [--cert file] [--key file] [--ca file] [--insecure]";
}

}  // namespace openmoq::publisher
