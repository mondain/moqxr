#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher::transport {

TransportStatus TransportStatus::success() {
    return {.ok = true, .message = {}};
}

TransportStatus TransportStatus::failure(std::string_view error_message) {
    return {.ok = false, .message = std::string(error_message)};
}

}  // namespace openmoq::publisher::transport
