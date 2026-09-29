#pragma once

#include "tools/intrinsic/tool_base.hpp"
#include "intercom/cancellable_exchange.hpp"

namespace tools::intrinsic {

/**
 * Abstract base for future YAML-declared hub request tools. It owns an immutable
 * endpoint and deadline; it borrows no Application or AgentInputState. Each call
 * creates its own connection and correlation ID, so requests share no mutable
 * transport state. The owning tool must remain alive until its request completes.
 *
 * Derived tools supply their declaration, argument/security policy and invoke().
 * This base grants no security policy and publishes no generic arbitrary-route
 * tool. Host worker/session/run identifiers must never come from model arguments.
 */
class HubRemoteCallToolBase : public DeclaredTool {
public:
    ~HubRemoteCallToolBase() override;

    /// Keep the base abstract: only a future concrete operation may be registered.
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override = 0;

protected:
    /**
     * Copy the resolved base endpoint and positive timeout. The optional event
     * bus has the same borrowed lifetime as DeclaredTool's confirmation bus.
     * Invalid transport settings throw invalid_argument before any network IO.
     */
    HubRemoteCallToolBase(
        const std::filesystem::path& declaration_file,
        endpoint::ResolvedEndpoint endpoint,
        std::chrono::milliseconds timeout,
        eventbus::AsyncEventBus* bus = nullptr);

    /**
     * Send query.arguments to a code-selected route, echo-check all identifiers,
     * and return the validated response data. Parameters are copied into the
     * coroutine frame so the caller cannot change an in-flight envelope.
     *
     * Only the protocol's rejected/not_implemented response exists today. A
     * returned envelope is NOT a successful tool result: future invoke() methods
     * must interpret its status and format an appropriate InvokeReturn. No
     * success/result wire shape is invented by this scaffolding.
     *
     * Transport, malformed replies and correlation errors raise InvokeException
     * at Stage::Invoke, correlated to query. Diagnostics never include the
     * endpoint query (which can contain a session token) or raw response bytes.
     * There are no retries or run-cancellation hooks. The bounded exchange joins
     * its transport cleanup before returning; system DNS can delay that cleanup
     * beyond the reply-validity deadline, as documented by cancellable_exchange.
     */
    boost::asio::awaitable<nlohmann::json> request(
        model_io::InvokeQuery query,
        std::string route,
        std::string worker_id,
        std::string session_id,
        std::string run_id) const;

private:
    const endpoint::ResolvedEndpoint endpoint_;
    const std::chrono::milliseconds timeout_;
};

} // namespace tools::intrinsic
