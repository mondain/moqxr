#include "openmoq/publisher/cli_options.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using openmoq::publisher::CliOptions;
using openmoq::publisher::parse_cli_options;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

CliOptions parse(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    return parse_cli_options(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

int main() {
    bool ok = true;

    {
        const CliOptions options = parse({"openmoq-publisher", "--input", "sample.mp4"});
        ok &= expect(options.subscriber_timeout == std::chrono::seconds(3),
                     "expected default subscriber timeout to remain 3 seconds");
        ok &= expect(options.split_cmaf_chunks, "expected chunk splitting to be enabled by default");
        ok &= expect(options.input_source.kind == openmoq::publisher::InputSourceKind::kFile,
                     "expected file input to remain the default input source kind");
        ok &= expect(options.input_source.path == "sample.mp4",
                     "expected file input path to be preserved");
        ok &= expect(!options.include_sap, "expected SAP track creation to be disabled by default");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--timeout", "9", "--forward", "0"});
        ok &= expect(options.subscriber_timeout == std::chrono::seconds(9),
                     "expected --timeout to override subscriber timeout");
    }

    {
        const CliOptions options = parse(
            {"openmoq-publisher", "--input", "sample.mp4", "--endpoint", "203.0.113.10:443", "--sni", "moq-relay.red5.net"});
        ok &= expect(options.endpoint.has_value(), "expected endpoint to be present when parsing --sni");
        ok &= expect(options.endpoint->sni == "moq-relay.red5.net", "expected --sni to populate endpoint SNI");
    }

    {
        bool threw = false;
        try {
            static_cast<void>(parse({"openmoq-publisher", "--input", "sample.mp4", "--sni", "moq-relay.red5.net"}));
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "--alpn and --sni require --endpoint to be provided first";
        }
        ok &= expect(threw, "expected --sni without --endpoint to be rejected");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--publish-catalog"});
        ok &= expect(options.publish_catalog, "expected --publish-catalog to enable proactive catalog publish");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--sap"});
        ok &= expect(options.include_sap, "expected --sap to enable SAP track creation");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--coalesce-cmaf-chunks"});
        ok &= expect(!options.split_cmaf_chunks, "expected --coalesce-cmaf-chunks to disable default chunk splitting");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--coalesce-cmaf-chunk"});
        ok &= expect(!options.split_cmaf_chunks,
                     "expected --coalesce-cmaf-chunk compatibility alias to disable default chunk splitting");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--loop"});
        ok &= expect(options.loop, "expected --loop to keep publishing after the file reaches EOF");
    }

    {
        bool threw = false;
        try {
            static_cast<void>(parse({"openmoq-publisher", "--input", "sample.mp4", "--timeout", "-1"}));
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "subscriber timeout must be zero or greater";
        }
        ok &= expect(threw, "expected negative --timeout to be rejected");
    }

    {
        const CliOptions options = parse({"openmoq-publisher", "--input", "-"});
        ok &= expect(options.input_source.kind == openmoq::publisher::InputSourceKind::kStdin,
                     "expected --input - to select stdin");
        ok &= expect(options.input_source.path.empty(),
                     "expected stdin input source to avoid storing a file path");
    }

    return ok ? 0 : 1;
}
