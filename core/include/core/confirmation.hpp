#pragma once
#include <memory>
#include <mutex>
#include <stop_token>
#include <optional>
#include "tools/security_check.hpp"
#include "load/configuration.hpp"

namespace core {
/** Runtime policy for RequireConfirm calls; other tool security levels are unchanged. */
enum class ConfirmationMode { Ask, Approve, Deny };

/**
 * Session-owned confirmation choices, updated synchronously at payload admission.
 * The application serializes access on its strand. Copy this small value before
 * applying a multi-category update; handle_options validates before mutation.
 * No endpoint/timeout configuration or durable conversation state is modified.
 */
class ConfirmationOptions {
public:
    /** Owned descriptors, not current selections; no IO or state mutation. */
    nlohmann::json get_options() const;
    /**
     * Accept an object with optional mode: ask/approve/deny. Empty means no change.
     * Unknown keys, null, and unsupported values throw std::invalid_argument and
     * leave the selection intact. Ask is the default and uses the startup endpoint.
     */
    void handle_options(const nlohmann::json& options);
    ConfirmationMode mode() const noexcept { return mode_; }

private:
    ConfirmationMode mode_ = ConfirmationMode::Ask;
};

/**
 * One run's confirmation admission and cancellation boundary.
 * A short mutex arbitrates approval against stop. No lock spans network work.
 * Stop cannot revoke an approval that won this boundary. The loop may still
 * cancel before dispatch; approval alone is not a promise of tool execution.
 */
class ConfirmationScope {
public:
    /** Freeze the selected policy for this run, including all parallel tool calls. */
    explicit ConfirmationScope(ConfirmationMode mode = ConfirmationMode::Ask)
        : mode_(mode) {}
    ConfirmationMode mode() const noexcept { return mode_; }
    void cancel();
    bool settle_approval(bool approved);
    std::stop_token token() const { return stop_.get_token(); }
private:
    const ConfirmationMode mode_;
    std::mutex mutex_;
    bool stopping_ = false;
    std::stop_source stop_;
};

/** Apply the scope's immutable run policy. Automatic decisions perform no IO.
 * Ask makes one attempt per event and fails closed; all exchange tasks are joined.
 * Automatic approval uses the same cancellation boundary as endpoint approval.
 * The host keeps exactly one authoritative async-bus listener alive through
 * batch completion. In Ask mode, missing endpoint, bad correlation and transport
 * errors deny.
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
