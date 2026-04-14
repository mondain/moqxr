#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/cli_options.h"
#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/transport/moqt_session.h"
#include "openmoq/publisher/transport/picoquic_client.h"
#include "openmoq/publisher/transport/webtransport_client.h"

#include <exception>
#include <iostream>
#include <memory>

namespace {

std::string webtransport_protocol_offer(openmoq::publisher::DraftVersion version) {
    switch (version) {
        case openmoq::publisher::DraftVersion::kDraft14:
            // Draft-14 (MOQT version 0xff00000e) predates the versioned
            // moqt-XX subprotocol tokens. Offering moq-00 alone is not
            // universally accepted: some deployments only advertise
            // moqt-15/moqt-16. Offering no WebTransport subprotocol at all
            // lets the server fall back to reading the version from
            // CLIENT_SETUP/SERVER_SETUP, which is the interoperable path
            // for draft-14 over WebTransport.
            return "";
        case openmoq::publisher::DraftVersion::kDraft16:
            return "\"moqt-16\"";
    }

    return "";
}

std::unique_ptr<openmoq::publisher::transport::PublisherTransport> create_transport(
    openmoq::publisher::transport::TransportKind transport_kind) {
    using namespace openmoq::publisher::transport;

    switch (transport_kind) {
        case TransportKind::kRawQuic:
            return std::make_unique<PicoquicClient>();
        case TransportKind::kWebTransport:
            return std::make_unique<WebTransportClient>();
    }

    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace openmoq::publisher;

    try {
        const CliOptions options = parse_cli_options(argc, argv);
        const ParsedMp4 parsed_mp4 = options.input_source.kind == InputSourceKind::kStdin
                                         ? parse_mp4_stream(std::cin, "stdin")
                                         : parse_mp4_file(options.input_source.path.string());
        const SegmentedMp4 segmented_mp4 = segment_for_cmaf(parsed_mp4,
                                                            options.split_cmaf_chunks ? CmafObjectMode::kSplit
                                                                                      : CmafObjectMode::kCoalesced);
        const PublishPlan plan = build_publish_plan(segmented_mp4, options.draft_version, options.include_sap);

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
            endpoint.application_protocol = endpoint.transport == transport::TransportKind::kWebTransport
                                               ? webtransport_protocol_offer(options.draft_version)
                                               : default_alpn(options.draft_version);
            if (!options.endpoint_alpn_overridden && endpoint.transport == transport::TransportKind::kRawQuic &&
                options.draft_version != DraftVersion::kDraft14) {
                endpoint.alpn = default_alpn(options.draft_version);
            } else if (!options.endpoint_alpn_overridden &&
                       endpoint.transport == transport::TransportKind::kWebTransport) {
                endpoint.alpn = "h3";
            }
            auto transport = create_transport(endpoint.transport);
            if (!transport) {
                throw std::runtime_error("failed to create requested transport");
            }
            MoqtSession session(
                *transport,
                options.track_namespace,
                options.forward,
                options.publish_catalog,
                options.paced,
                options.loop,
                options.subscriber_timeout);

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
