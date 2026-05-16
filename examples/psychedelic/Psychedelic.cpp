#include "openmoq/publisher/publisher_api.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <streambuf>
#include <string>

#ifdef _WIN32
#define OPENMOQ_POPEN _popen
#define OPENMOQ_PCLOSE _pclose
#define OPENMOQ_POPEN_READ_MODE "rb"
#else
#define OPENMOQ_POPEN popen
#define OPENMOQ_PCLOSE pclose
#define OPENMOQ_POPEN_READ_MODE "r"
#endif

namespace {

class PopenStreamBuf final : public std::streambuf {
public:
    explicit PopenStreamBuf(FILE* pipe) : pipe_(pipe) {
        setg(buffer_, buffer_, buffer_);
    }

protected:
    int_type underflow() override {
        if (pipe_ == nullptr) {
            return traits_type::eof();
        }
        const std::size_t bytes = std::fread(buffer_, 1, sizeof(buffer_), pipe_);
        if (bytes == 0) {
            return traits_type::eof();
        }
        setg(buffer_, buffer_, buffer_ + static_cast<std::ptrdiff_t>(bytes));
        return traits_type::to_int_type(*gptr());
    }

private:
    FILE* pipe_ = nullptr;
    char buffer_[16 * 1024]{};
};

class PopenIStream final : public std::istream {
public:
    explicit PopenIStream(FILE* pipe) : std::istream(nullptr), buffer_(pipe) {
        rdbuf(&buffer_);
    }

private:
    PopenStreamBuf buffer_;
};

struct Args {
    std::string endpoint = "https://127.0.0.1:4433/moq";
    std::string namespace_name = "live.psychedelic.stream";
    int seconds = 15;
    openmoq::publisher::DraftVersion draft = openmoq::publisher::DraftVersion::kDraft16;
};

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        auto require_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            ++i;
            return argv[i];
        };
        if (flag == "--endpoint") {
            args.endpoint = require_value("--endpoint");
        } else if (flag == "--namespace") {
            args.namespace_name = require_value("--namespace");
        } else if (flag == "--seconds") {
            args.seconds = std::stoi(require_value("--seconds"));
            if (args.seconds <= 0) {
                throw std::runtime_error("--seconds must be > 0");
            }
        } else if (flag == "--draft") {
            const std::string value = require_value("--draft");
            if (value == "14") {
                args.draft = openmoq::publisher::DraftVersion::kDraft14;
            } else if (value == "16") {
                args.draft = openmoq::publisher::DraftVersion::kDraft16;
            } else if (value == "18") {
                args.draft = openmoq::publisher::DraftVersion::kDraft18;
            } else {
                throw std::runtime_error("--draft must be one of: 14, 16, 18");
            }
        } else {
            throw std::runtime_error("unknown argument: " + flag);
        }
    }
    return args;
}

std::string build_ffmpeg_video_command(int seconds) {
    return "ffmpeg -hide_banner -loglevel error -re "
           "-f lavfi -i \"nullsrc=size=640x480:rate=30,"
           "geq="
           "r='128+90*sin(0.020*X+1.30*T)+70*sin(0.045*sqrt((X-W/2)^2+(Y-H/2)^2)-0.70*T)':"
           "g='128+90*sin(0.022*Y-1.10*T)+70*sin(0.040*sqrt((X-W/2)^2+(Y-H/2)^2)+0.80*T)':"
           "b='128+80*sin(0.018*(X+Y)+1.00*T)+80*sin(0.050*sqrt((X-W/2)^2+(Y-H/2)^2)-0.45*T)',"
           "gblur=sigma=2.4,"
           "eq=saturation=1.35:contrast=1.05:brightness=0.01\" "
           "-f lavfi -i \"aevalsrc="
           "(0.16*sin(2*PI*196*t))*(0.92+0.08*sin(2*PI*0.08*t))"
           ":s=48000:c=stereo\" "
           "-af \"lowpass=f=1200,volume=0.7\" "
           "-map 0:v:0 -map 1:a:0 "
           "-t " + std::to_string(seconds) + " "
           "-c:v libx264 -preset veryfast -tune zerolatency "
           "-g 8 -keyint_min 8 -sc_threshold 0 -pix_fmt yuv420p "
           "-c:a libopus -b:a 128k -application lowdelay "
           "-muxdelay 0 -flush_packets 1 "
           "-movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof+frag_custom "
           "-frag_duration 50000 "
           "-f mp4 -";
}

void print_usage(const char* argv0) {
    std::cout << "Usage: " << argv0
              << " [--endpoint https://host:port/moq] [--namespace name] [--draft 14|16|18] [--seconds N]\n";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace openmoq::publisher;
    using namespace openmoq::publisher::transport;

    try {
        const Args args = parse_args(argc, argv);

        PublisherConfig base_config;
        base_config.draft_version = args.draft;
        base_config.split_cmaf_chunks = true;
        base_config.include_sap = false;
        base_config.paced = false;
        base_config.loop = false;

        EndpointConfig endpoint;
        endpoint.transport = TransportKind::kWebTransport;
        endpoint.host = "127.0.0.1";
        endpoint.port = 4433;
        endpoint.path = "/moq";
        endpoint.path_explicit = true;

        // Parse endpoint through the same supported forms as CLI by using a tiny bridge:
        // host:port, moqt://host:port/path, or https://host:port/path.
        {
            const std::string& raw = args.endpoint;
            std::string authority = raw;
            const auto consume_scheme = [&](const char* prefix) {
                const std::string p(prefix);
                if (authority.rfind(p, 0) == 0) {
                    authority = authority.substr(p.size());
                    return true;
                }
                return false;
            };
            const bool had_moqt = consume_scheme("moqt://");
            const bool had_https = consume_scheme("https://");
            if (had_moqt || had_https) {
                const std::size_t slash = authority.find('/');
                if (slash != std::string::npos) {
                    endpoint.path = authority.substr(slash);
                    endpoint.path_explicit = true;
                    authority = authority.substr(0, slash);
                } else if (had_https) {
                    endpoint.path = "/";
                    endpoint.path_explicit = true;
                }
                endpoint.transport = had_https ? TransportKind::kWebTransport : TransportKind::kRawQuic;
            } else {
                endpoint.transport = TransportKind::kRawQuic;
            }

            const std::size_t colon = authority.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= authority.size()) {
                throw std::runtime_error("endpoint must be host:port, moqt://host:port/path, or https://host:port/path");
            }
            endpoint.host = authority.substr(0, colon);
            endpoint.port = static_cast<std::uint16_t>(std::stoi(authority.substr(colon + 1)));
            if (endpoint.transport == TransportKind::kWebTransport && !endpoint.path_explicit) {
                endpoint.path = "/moq";
                endpoint.path_explicit = true;
            }
        }

        PublisherConfig config = base_config;
        config.track_namespace = args.namespace_name;
        Publisher publisher(config);

        TlsConfig tls;
        tls.insecure_skip_verify = true;
        const bool endpoint_alpn_overridden = false;

        const std::string ffmpeg_cmd = build_ffmpeg_video_command(args.seconds);
        std::cerr << "[psychedelic] launching ffmpeg: " << ffmpeg_cmd << '\n';
        FILE* ffmpeg_pipe = OPENMOQ_POPEN(ffmpeg_cmd.c_str(), OPENMOQ_POPEN_READ_MODE);
        if (ffmpeg_pipe == nullptr) {
            throw std::runtime_error("failed to start ffmpeg. Is it installed and in PATH?");
        }
        const std::unique_ptr<FILE, int (*)(FILE*)> pipe_guard(ffmpeg_pipe, OPENMOQ_PCLOSE);
        PopenIStream live_input(pipe_guard.get());

        const TransportStatus status = publisher.publish_live(live_input, endpoint, tls, endpoint_alpn_overridden);
        if (!status.ok) {
            throw std::runtime_error("publish_live failed: " + status.message);
        }
        const TransportStatus disconnect_status = publisher.disconnect(0);
        if (!disconnect_status.ok) {
            throw std::runtime_error("disconnect failed: " + disconnect_status.message);
        }

        const auto stats = publisher.stats();
        std::cout << "[psychedelic] done (single namespace, separate A/V tracks)\n";
        std::cout << "[psychedelic] published bytes=" << stats.bytes_published
                  << " objects=" << stats.objects_published
                  << " groups=" << stats.groups_published << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
