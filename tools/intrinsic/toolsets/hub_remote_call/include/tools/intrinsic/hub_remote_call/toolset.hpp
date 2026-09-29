#pragma once

#include <chrono>

#include "tools/intrinsic/toolset_base.hpp"
#include "endpoint/model_request.hpp"

namespace tools::intrinsic {

/**
 * Optional, dependency-injected intrinsic set for hub remote calls. Core creates
 * it only when hub_remote_call is configured. Construction saves transport
 * settings but opens no connection. The set currently contains no tools, skill,
 * or capability groups; enabling it does not advertise a callable capability.
 * Future concrete tools will receive these settings during construction.
 */
class HubRemoteCallToolSet final : public IntrinsicToolSet {
public:
    /// Reject unusable endpoint/deadline settings without attempting a connection.
    HubRemoteCallToolSet(
        endpoint::ResolvedEndpoint endpoint,
        std::chrono::milliseconds timeout);

    std::string_view name() const noexcept override;

    /// Immutable settings owned by this set, valid for its lifetime.
    const endpoint::ResolvedEndpoint& endpoint() const noexcept;
    std::chrono::milliseconds timeout() const noexcept;

private:
    const endpoint::ResolvedEndpoint endpoint_;
    const std::chrono::milliseconds timeout_;
};

} // namespace tools::intrinsic
