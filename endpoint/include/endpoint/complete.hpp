#pragma once

#include <stdexcept>

#include "endpoint/model_request.hpp"

namespace endpoint {

template<
    typename Delta,
    RequestDriver<typename ModelResponseReader<Delta>::Handler> Driver
>
boost::asio::awaitable<model_io::MessageItem> complete_once(
    ResolvedEndpoint model_endpoint,
    std::shared_ptr<ModelResponseReader<Delta>> response_reader,
    Driver driver
) {
    // Not implemented yet: fail loudly at the call site rather than handing
    // back a default-constructed awaitable, whose co_await is undefined.
    throw std::logic_error("endpoint::complete_once: not implemented yet");
}

}
