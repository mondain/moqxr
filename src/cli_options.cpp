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
        } else if (argument == "--draft") {
            options.draft_version = parse_draft(require_value("--draft"));
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

    return options;
}

std::string build_usage(const char* argv0) {
    return std::string("Usage: ") + argv0 +
           " --input <fragmented.mp4> [--draft 14|16] [--dump-plan] [--emit-dir <dir>]";
}

}  // namespace openmoq::publisher
