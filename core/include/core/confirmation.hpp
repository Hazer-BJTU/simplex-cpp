#pragma once
#include <memory>
#include <mutex>
#include <stop_token>
#include <optional>
#include "tools/security_check.hpp"
#include "load/configuration.hpp"

namespace core {
/**
 * One run's confirmation admission and cancellation boundary.
 * A short mutex arbitrates approval against stop. No lock spans network work.
 * Stop cannot revoke an approval that won this boundary. The loop may still
 * cancel before dispatch; approval alone is not a promise of tool execution.
 */
class ConfirmationScope {
public:
    void cancel();
    bool settle_approval(bool approved);
    std::stop_token token() const { return stop_.get_token(); }
private:
    std::mutex mutex_;
    bool stopping_ = false;
    std::stop_source stop_;
};

/** One attempt per event. Always fail closed; all exchange tasks are joined.
 * The host keeps exactly one authoritative async-bus listener alive through
 * batch completion. Missing endpoint, bad correlation and transport errors deny.
 * Timeout invalidates approval but completion can wait for an already-running
 * system DNS backend. See intercom::cancellable_exchange for the lifetime contract.
 */
boost::asio::awaitable<tools::InvokeConfirmEvent> confirm(
    tools::InvokeConfirmEvent event,
    std::shared_ptr<ConfirmationScope> scope,
    boost::asio::any_io_executor executor,
    std::optional<endpoint::ResolvedEndpoint> endpoint,
    std::chrono::milliseconds timeout,
    std::string session_id,
    std::string run_id);
} // namespace core
