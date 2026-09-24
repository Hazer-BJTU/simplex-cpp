#pragma once

#include <chrono>
#include <stop_token>
#include <string>
#include "intercom/websocket_stream.hpp"

namespace intercom {
/**
 * One text request/reply with an overall deadline and explicit cancellation.
 *
 * No retries. The deadline covers DNS, TCP/TLS, upgrade, write, read and close.
 * Stop/deadline notifications are posted to the operation's private strand;
 * transport abortion and cancellation-slot emission never race socket access.
 * Completion joins the exchange and its timer before returning. Cancellation
 * throws operation_aborted; expiry throws timed_out. A binary reply is fatal.
 * Keep the TLS context alive until completion. Inherited Asio cancellation is
 * shielded: use stop to request shutdown without abandoning cleanup.
 */
boost::asio::awaitable<std::string> cancellable_exchange(
    boost::asio::any_io_executor executor,
    endpoint::ResolvedEndpoint endpoint,
    std::string request,
    std::chrono::milliseconds timeout,
    std::stop_token stop = {},
    endpoint::ssl_context& context = endpoint::get_global_ssl_context());
} // namespace intercom
