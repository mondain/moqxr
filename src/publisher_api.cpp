#include "openmoq/publisher/publisher_api.h"

#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/transport/moqt_session.h"
#include "openmoq/publisher/transport/picoquic_client.h"
#include "openmoq/publisher/transport/webtransport_client.h"

#include <stdexcept>
#include <utility>

namespace openmoq::publisher {

namespace {

std::string webtransport_protocol_offer(DraftVersion version) {
    switch (version) {
        case DraftVersion::kDraft14:
            return "";
        case DraftVersion::kDraft16:
            return "\"moqt-16\"";
        case DraftVersion::kDraft18:
            return "\"moqt-18\"";
    }

    return "";
}

}  // namespace

Publisher::Publisher(PublisherConfig config, TransportFactory transport_factory)
    : config_(std::move(config)),
      transport_factory_(std::move(transport_factory)) {
    if (!transport_factory_) {
        transport_factory_ = default_transport_factory();
    }
}

const PublisherConfig& Publisher::config() const {
    return config_;
}

void Publisher::set_config(const PublisherConfig& config) {
    config_ = config;
}

PreparedPublish Publisher::prepare_file(const std::filesystem::path& path) const {
    const ParsedMp4 parsed_mp4 = parse_mp4_file(path.string());
    const SegmentedMp4 segmented_mp4 = segment_for_cmaf(parsed_mp4, config_.split_cmaf_chunks ? CmafObjectMode::kSplit
                                                                                               : CmafObjectMode::kCoalesced);
    return PreparedPublish{
        .input_bytes = parsed_mp4.bytes,
        .plan = build_publish_plan(segmented_mp4, config_.draft_version, config_.include_sap),
    };
}

PreparedPublish Publisher::prepare_stream(std::istream& input, std::string_view source_name) const {
    const ParsedMp4 parsed_mp4 = parse_mp4_stream(input, source_name);
    const SegmentedMp4 segmented_mp4 = segment_for_cmaf(parsed_mp4, config_.split_cmaf_chunks ? CmafObjectMode::kSplit
                                                                                               : CmafObjectMode::kCoalesced);
    return PreparedPublish{
        .input_bytes = parsed_mp4.bytes,
        .plan = build_publish_plan(segmented_mp4, config_.draft_version, config_.include_sap),
    };
}

std::string Publisher::render_plan(const PreparedPublish& prepared) const {
    return render_publish_plan(prepared.plan);
}

void Publisher::emit_objects(const PreparedPublish& prepared, const std::filesystem::path& output_dir) const {
    emit_plan_objects(prepared.plan, prepared.input_bytes, output_dir);
}

transport::TransportStatus Publisher::publish(const PreparedPublish& prepared,
                                              const transport::EndpointConfig& endpoint,
                                              const transport::TlsConfig& tls,
                                              bool endpoint_alpn_overridden) const {
    if (!transport_factory_) {
        return transport::TransportStatus::failure("publisher transport factory is not configured");
    }
    auto transport = transport_factory_(endpoint.transport);
    if (!transport) {
        return transport::TransportStatus::failure("failed to create requested transport");
    }

    transport::MoqtSession session(
        *transport,
        config_.track_namespace,
        config_.forward,
        config_.publish_catalog,
        config_.paced,
        config_.loop,
        config_.subscriber_timeout);

    const transport::EndpointConfig resolved_endpoint = resolve_endpoint(endpoint, endpoint_alpn_overridden);
    transport::TransportStatus status = session.connect(resolved_endpoint, tls);
    if (!status.ok) {
        return transport::TransportStatus::failure("transport connect failed: " + status.message);
    }

    const PublishPlan materialized = materialize_publish_plan(prepared.plan, prepared.input_bytes);
    status = session.publish(materialized);
    if (!status.ok) {
        return transport::TransportStatus::failure("transport publish failed: " + status.message);
    }

    return transport::TransportStatus::success();
}

transport::TransportStatus Publisher::publish_file(const std::filesystem::path& path,
                                                   const transport::EndpointConfig& endpoint,
                                                   const transport::TlsConfig& tls,
                                                   bool endpoint_alpn_overridden) const {
    const PreparedPublish prepared = prepare_file(path);
    return publish(prepared, endpoint, tls, endpoint_alpn_overridden);
}

transport::TransportStatus Publisher::publish_stream(std::istream& input,
                                                     std::string_view source_name,
                                                     const transport::EndpointConfig& endpoint,
                                                     const transport::TlsConfig& tls,
                                                     bool endpoint_alpn_overridden) const {
    const PreparedPublish prepared = prepare_stream(input, source_name);
    return publish(prepared, endpoint, tls, endpoint_alpn_overridden);
}

transport::TransportStatus Publisher::publish_live(std::istream& input,
                                                   const transport::EndpointConfig& endpoint,
                                                   const transport::TlsConfig& tls,
                                                   bool endpoint_alpn_overridden) const {
    if (!transport_factory_) {
        return transport::TransportStatus::failure("publisher transport factory is not configured");
    }
    auto transport = transport_factory_(endpoint.transport);
    if (!transport) {
        return transport::TransportStatus::failure("failed to create requested transport");
    }

    transport::MoqtSession session(
        *transport,
        config_.track_namespace,
        config_.forward,
        config_.publish_catalog,
        config_.paced,
        config_.loop,
        config_.subscriber_timeout);

    const transport::EndpointConfig resolved_endpoint = resolve_endpoint(endpoint, endpoint_alpn_overridden);
    transport::TransportStatus status = session.connect(resolved_endpoint, tls);
    if (!status.ok) {
        return transport::TransportStatus::failure("transport connect failed: " + status.message);
    }

    status = session.publish_live(input, config_.draft_version, config_.split_cmaf_chunks);
    if (!status.ok) {
        return transport::TransportStatus::failure("transport live publish failed: " + status.message);
    }

    return transport::TransportStatus::success();
}

Publisher::TransportFactory Publisher::default_transport_factory() {
    return [](transport::TransportKind kind) -> std::unique_ptr<transport::PublisherTransport> {
        switch (kind) {
            case transport::TransportKind::kRawQuic:
                return std::make_unique<transport::PicoquicClient>();
            case transport::TransportKind::kWebTransport:
                return std::make_unique<transport::WebTransportClient>();
        }
        return nullptr;
    };
}

transport::EndpointConfig Publisher::resolve_endpoint(const transport::EndpointConfig& endpoint,
                                                      bool endpoint_alpn_overridden) const {
    transport::EndpointConfig resolved = endpoint;
    resolved.application_protocol = resolved.transport == transport::TransportKind::kWebTransport
                                        ? webtransport_protocol_offer(config_.draft_version)
                                        : default_alpn(config_.draft_version);

    if (!endpoint_alpn_overridden && resolved.transport == transport::TransportKind::kRawQuic &&
        config_.draft_version != DraftVersion::kDraft14) {
        resolved.alpn = default_alpn(config_.draft_version);
    } else if (!endpoint_alpn_overridden && resolved.transport == transport::TransportKind::kWebTransport) {
        resolved.alpn = "h3";
    }

    return resolved;
}

}  // namespace openmoq::publisher
