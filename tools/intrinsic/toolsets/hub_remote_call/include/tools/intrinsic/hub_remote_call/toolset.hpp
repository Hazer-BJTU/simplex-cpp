#pragma once

#include <chrono>

#include "tools/intrinsic/toolset_base.hpp"
#include "tools/intrinsic/hub_remote_call/tools.hpp"

namespace tools::intrinsic {

/**
 * Optional host-injected toolset for session-scoped hub operations. Core creates
 * it only when hub_remote_call is configured. It registers plan/subagent tools and its
 * installed YAML skill without connecting during construction. The identity
 * provider returns a fresh trusted snapshot for each invocation.
 */
class HubRemoteCallToolSet final : public IntrinsicToolSet {
public:
    /// Reject unusable endpoint/deadline settings without attempting a connection.
    HubRemoteCallToolSet(
        endpoint::ResolvedEndpoint endpoint,
        std::chrono::milliseconds timeout,
        HubRemoteCallIdentityProvider identity);

    std::string_view name() const noexcept override;

    /// Immutable settings owned by this set, valid for its lifetime.
    const endpoint::ResolvedEndpoint& endpoint() const noexcept;
    std::chrono::milliseconds timeout() const noexcept;

private:
    const endpoint::ResolvedEndpoint endpoint_;
    const std::chrono::milliseconds timeout_;
};

} // namespace tools::intrinsic
