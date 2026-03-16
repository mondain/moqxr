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
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--timeout", "9", "--forward", "0"});
        ok &= expect(options.subscriber_timeout == std::chrono::seconds(9),
                     "expected --timeout to override subscriber timeout");
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

    return ok ? 0 : 1;
}
