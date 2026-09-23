#pragma once

#include "dataclass/model_io.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <stdexcept>
#include <string>

namespace llm {
class LLMModel;
}

namespace tools {
class ToolRegistry;
}

namespace eventbus {
class EventBus;
}

namespace loop {

/** Describes why one invocation ended, independently of the session lifetime. */
enum class RunStatus {
    Completed, // A final model response was committed without tool calls.
    Cancelled, // A stop request was observed and required results were committed.
    ExchangeLimit, // The exchange budget ended after settling the last tool batch.
    Failed     // Inspect error and state.loop before deciding how to continue.
};

/**
 * Admission is refused because earlier tool effects cannot be replayed safely.
 *
 * The caller receives the original Tools or Blocked phase and decides how to
 * inspect or repair the persistent state. run() does not change that state or
 * convert this condition into an ordinary Failed result.
 */
class RecoveryRequired : public std::runtime_error {
public:
    RecoveryRequired(model_io::LoopPhase phase, std::string message);
    ~RecoveryRequired() override;

    [[nodiscard]] model_io::LoopPhase phase() const noexcept { return phase_; }

private:
    model_io::LoopPhase phase_;
};

/**
 * Summary returned to the caller after a run, including edit-hook failures.
 *
 * This is not a second persistence object. Accepted runs write their status and
 * error into AgentInputState::loop before returning. Failures before admission
 * do not create a new progress record; their diagnostic is available only here
 * and in logging. RecoveryRequired is a distinct exception, not a RunResult.
 * Recovery may already have committed previously buffered results.
 */
struct RunResult {
    RunStatus status = RunStatus::Failed;

    /// Number of model responses committed by this invocation, not the session.
    std::size_t completed_exchanges = 0;

    /// Diagnostic text on failure; tool-level failures remain in tool results.
    std::string error;
};

/** Limits one invocation; it does not change model generation configuration. */
struct Options {
    /// Must be positive. The last exchange's tool batch is settled even when
    /// that exchange consumes the remaining budget.
    std::size_t max_exchanges = 32;
};

} // namespace loop
