#pragma once

#include "dataclass/model_io.hpp"

namespace loop {

/** Opt-in controls for controlled research, not unstable-feature switches. */
struct ExperimentalControl {
    std::uint64_t steps = 0;
    std::uint64_t tokens = 0;
    enum class TokenType { Prompt, Generated, Total, CacheHit };
    TokenType token_type = TokenType::Total;
    bool enabled() const noexcept { return steps != 0 || tokens != 0; }
};

/** Host-owned accounting survives history pruning in AgentInputState::extras.
 * Only a committed, external user message may reset it. Account responses before
 * tool dispatch; checking the resulting boundary must wait for tool settlement. */
void reset_experimental(model_io::AgentInputState&, const ExperimentalControl&);
void account_experimental(model_io::AgentInputState&, const ExperimentalControl&,
    const model_io::MessageItem&);
/** Empty means usable, otherwise a stable termination reason. Accounting errors
 * throw explicitly; neither this check nor recovery replays or recounts a response. */
std::string experimental_boundary(const model_io::AgentInputState&, const ExperimentalControl&);
std::optional<nlohmann::json> experimental_report(const model_io::AgentInputState&);
/** Copy only the compact attempt's accounting, including on failed replacement. */
void retain_experimental(model_io::AgentInputState&, const model_io::AgentInputState&);

} // namespace loop
