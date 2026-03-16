#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/cli_options.h"
#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/transport/moqt_session.h"
#include "openmoq/publisher/transport/picoquic_client.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    using namespace openmoq::publisher;

    try {
        const CliOptions options = parse_cli_options(argc, argv);
        const ParsedMp4 parsed_mp4 = parse_mp4_file(options.input_path.string());
        const SegmentedMp4 segmented_mp4 = segment_for_cmaf(parsed_mp4);
        const PublishPlan plan = build_publish_plan(segmented_mp4, options.draft_version);

        if (options.dump_plan || !options.emit_dir.has_value()) {
            std::cout << render_publish_plan(plan);
        }

        if (options.emit_dir.has_value()) {
            emit_plan_objects(plan, parsed_mp4.bytes, *options.emit_dir);
        }

        if (options.endpoint.has_value()) {
            using namespace openmoq::publisher::transport;

            const PublishPlan materialized_plan = materialize_publish_plan(plan, parsed_mp4.bytes);
            EndpointConfig endpoint = *options.endpoint;
            if (!options.endpoint_alpn_overridden && options.draft_version != DraftVersion::kDraft14) {
                endpoint.alpn = default_alpn(options.draft_version);
            }
            PicoquicClient transport;
            MoqtSession session(
                transport, options.track_namespace, options.forward, options.paced, options.subscriber_timeout);

            TransportStatus status = session.connect(endpoint, options.tls);
            if (!status.ok) {
                throw std::runtime_error("transport connect failed: " + status.message);
            }

            status = session.publish(materialized_plan);
            if (!status.ok) {
                throw std::runtime_error("transport publish failed: " + status.message);
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
