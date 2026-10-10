#pragma once

#include "tools/intrinsic/hub_remote_call/tools.hpp"

namespace tools::intrinsic {

/**
 * Shared one-shot invocation for the three fixed subagent routes. Each instance
 * owns an identity callback and immutable route; it keeps no child registry,
 * conversation or transport state. The Hub authorizes direct-child ownership.
 *
 * Invocations shield inherited cancellation and join transport cleanup. A lost
 * fork/send reply can follow a committed mutation: no retry or rollback is attempted.
 * Read failures report transport, protocol or presentation-budget diagnostics
 * without suggesting that receive performed a mutation.
 * Results retain dispatch/completion distinctions and bounded history metadata.
 * Child text is displayed as data; it never becomes parent instructions.
 */
class SubagentToolBase : public HubRemoteCallToolBase {
public:
    /// Fork/send are SerialWrite; receive is ReadOnly. All are Trusted.
    void write_attributes(model_io::InvokeQuery& query) const override;

    /// Capture fresh host identity, perform one RPC, validate and format its result.
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;

protected:
    SubagentToolBase(
        std::string_view declaration,
        std::string route,
        endpoint::ResolvedEndpoint endpoint,
        std::chrono::milliseconds timeout,
        HubRemoteCallIdentityProvider identity);

private:
    const std::string route_;
    HubRemoteCallIdentityProvider identity_;
};

/**
 * Create one clean direct child using the parent's startup configuration.
 * Accepts only {}. Returns an ID and initial lifecycle without waiting for
 * startup or sending a task. Conversation and memory are fresh; configured
 * external workspaces can still be shared. Repeating the call creates a new RPC.
 */
class SubagentForkTool final : public SubagentToolBase {
public:
    SubagentForkTool(
        endpoint::ResolvedEndpoint endpoint,
        std::chrono::milliseconds timeout,
        HubRemoteCallIdentityProvider identity);
    void ensure_arguments(model_io::InvokeQuery& query) const override;
};

/**
 * Send message/continue/compact, or stop one direct child. A message requires
 * payload content; continue/compact forbid it; stop also forbids options.
 * Only model options and reserved empty tools options are accepted. Child
 * confirmation policy remains operator-owned. Dispatch does not await a run.
 * Stop terminates the process family and deletes persistence after cleanup;
 * obtain required output through receive before requesting it.
 */
class SubagentSendTool final : public SubagentToolBase {
public:
    SubagentSendTool(
        endpoint::ResolvedEndpoint endpoint,
        std::chrono::milliseconds timeout,
        HubRemoteCallIdentityProvider identity);
    void ensure_arguments(model_io::InvokeQuery& query) const override;
};

/**
 * List direct children with {}, or inspect one child's status/outcomes/history.
 * Targeted queries accept cursor (default 0) and limit (default 5, range 1..10).
 * Cursors address the Hub's current bounded projection; restart from zero when
 * its revision/worker changes. This is a snapshot with no polling or waiting.
 * Exact answer queries require all source fields, including the fingerprint.
 * Presentation counts actual structural overhead and reduces body allowances
 * only if needed; excessive structure is a read-only budget failure.
 */
class SubagentReceiveTool final : public SubagentToolBase {
public:
    SubagentReceiveTool(
        endpoint::ResolvedEndpoint endpoint,
        std::chrono::milliseconds timeout,
        HubRemoteCallIdentityProvider identity);
    void ensure_arguments(model_io::InvokeQuery& query) const override;
};

} // namespace tools::intrinsic
