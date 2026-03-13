#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher {

struct CliOptions {
    std::filesystem::path input_path;
    std::optional<std::filesystem::path> emit_dir;
    std::optional<transport::EndpointConfig> endpoint;
    transport::TlsConfig tls;
    DraftVersion draft_version = DraftVersion::kDraft14;
    std::string track_namespace = "media";
    bool forward = false;
    bool dump_plan = false;
};

CliOptions parse_cli_options(int argc, char** argv);
std::string build_usage(const char* argv0);

}  // namespace openmoq::publisher
