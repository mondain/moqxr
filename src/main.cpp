#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/cli_options.h"
#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/publisher_api.h"
#include "openmoq/publisher/transport/moqt_session.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    using namespace openmoq::publisher;

    try {
        const CliOptions options = parse_cli_options(argc, argv);

        // Live stdin mode: when reading from stdin with an endpoint, use
        // incremental streaming instead of buffering everything to EOF.
        const bool live_stdin = options.input_source.kind == InputSourceKind::kStdin
                                && options.endpoint.has_value();
        const PublisherConfig config{
            .draft_version = options.draft_version,
            .track_namespace = options.track_namespace,
            .forward = options.forward,
            .publish_catalog = options.publish_catalog,
            .include_sap = options.include_sap,
            .split_cmaf_chunks = options.split_cmaf_chunks,
            .paced = options.paced,
            .loop = options.loop,
            .subscriber_timeout = options.subscriber_timeout,
        };
        Publisher publisher(config);

        if (live_stdin) {
            const auto status = publisher.publish_live(
                std::cin,
                *options.endpoint,
                options.tls,
                options.endpoint_alpn_overridden);
            if (!status.ok) {
                throw std::runtime_error(status.message);
            }
        } else {
            // Original batch mode: read entire file/stdin, segment, plan, publish.
            const PreparedPublish prepared = options.input_source.kind == InputSourceKind::kStdin
                                                 ? publisher.prepare_stream(std::cin, "stdin")
                                                 : publisher.prepare_file(options.input_source.path);

            if (options.dump_plan || !options.emit_dir.has_value()) {
                std::cout << publisher.render_plan(prepared);
            }

            if (options.emit_dir.has_value()) {
                publisher.emit_objects(prepared, *options.emit_dir);
            }

            if (options.endpoint.has_value()) {
                const auto status = publisher.publish(
                    prepared,
                    *options.endpoint,
                    options.tls,
                    options.endpoint_alpn_overridden);
                if (!status.ok) {
                    throw std::runtime_error(status.message);
                }
            }
        }
    } catch (const std::exception& exception) {
        if (std::string_view(exception.what()).empty()) {
            std::cerr << build_usage(argv[0]) << '\n';
            return 0;
        }

        std::cerr << "error: " << exception.what() << '\n';
        std::cerr << build_usage(argv[0]) << '\n';
        return 1;
    }

    return 0;
}
