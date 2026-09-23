#pragma once

#include "loop/intrinsic/hook_config.hpp"
#include "loop/events.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace loop::intrinsic {

/**
 * Stateless context-usage hook over the persistent AgentInputState.
 *
 * The instance holds only the configured context-window length. Its statistics
 * and within-run accounting marker live in the flat external_status slot named
 * context_statistic, so JSON round-trips and history pruning do not reset
 * cumulative usage. See README.md for every field and its precise formula.
 *
 * Register before hooks that prune steps in EditOnStepFinished or
 * EditOnRunFinished; the response being accounted must still be present at
 * this hook's callback. The host must not mutate this hook's status slot.
 */
class ContextStatisticHook final : public IntrinsicLoopHook {
public:
    static constexpr std::string_view kName = "context_statistic";

    explicit ContextStatisticHook(HookConfig config);

    /** Load the installed/source YAML, validate it, then construct the hook. */
    [[nodiscard]] static std::shared_ptr<ContextStatisticHook> from_config();

protected:
    Subscriptions subscribe(eventbus::EventBus& bus) override;

private:
    void before_model(const BeforeModel& event) const;
    void on_step_finished(const EditOnStepFinished& event) const;
    void on_run_finished(const EditOnRunFinished& event) const;
    void update(model_io::AgentInputState& state) const;

    std::uint64_t context_window_tokens_;
};

} // namespace loop::intrinsic
