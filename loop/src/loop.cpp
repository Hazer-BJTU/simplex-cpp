#include "loop/loop.hpp"
#include "loop/events.hpp"
#include "llm/models.hpp"
#include "tools/registry.hpp"
#include "eventbus/event_bus.hpp"
#include "logging/logger.hpp"

#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>

#include <set>
#include <stdexcept>
#include <type_traits>

namespace loop {

namespace {

using model_io::LoopPhase;
using model_io::LoopStatus;

using State = model_io::AgentInputState;
using Item = model_io::MessageItem;
using Kind = model_io::MessageItemType;
static_assert(std::is_nothrow_move_assignable_v<State>);

/**
 * Converts the exception currently being handled into a diagnostic string.
 * Call only from a catch block: the bare rethrow requires an active exception.
 * Non-standard exceptions receive a fixed message; this function does not log.
 */
std::string error_text() {
    try {
        throw;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown exception";
    }
}

/**
 * Integrates one message with the model's retention policy on a state copy.
 * Copying or integration may throw; the original state remains unchanged.
 * The final move assignment cannot throw, as required by the assertion above.
 */
void integrate(
    llm::LLMModel& model,
    State& state,
    const Item& item) {
    auto draft = state;
    model.integrate(draft, item);
    state = std::move(draft);
}

/**
 * Checks the wire identities of a model response before any tools can run.
 * Empty invocation lists are allowed. Empty names/IDs and duplicate IDs throw
 * invalid_argument without changing the response or dispatching anything.
 */
void validate_calls(const Item& item) {
    if (!item.invokes) {
        return;
    }

    std::set<std::string> ids;
    for (const auto& call : *item.invokes) {
        if (call.id.empty() || call.name.empty() || !ids.insert(call.id).second) {
            throw std::invalid_argument("empty tool identity or duplicate tool id");
        }
    }
}

/**
 * Validates that every historical call has exactly one corresponding result.
 * Results must appear in original call order with the same ID and tool name.
 * Throws on incomplete or mismatched history: an unanswered imported call is
 * not sufficient evidence that its effects never happened, so replay is unsafe.
 */
void validate_history(const State& state) {
    for (const auto& turn : state.turns) {
        for (const auto& step : turn.agent_loop_step) {
            validate_calls(step.model_response);
            const auto& calls = step.model_response.invokes;
            const auto& returns = step.invoke_returns;
            const auto count = calls ? calls->size() : 0;
            if ((returns ? returns->size() : 0) != count) {
                throw std::logic_error("unresolved tool history requires inspection");
            }
            for (std::size_t i = 0; i < count; ++i) {
                const auto& result = (*returns)[i].invoke_return;
                if (!result || result->query.id != (*calls)[i].id ||
                    result->query.name != (*calls)[i].name) {
                    throw std::logic_error("tool history identity mismatch");
                }
            }
        }
    }
}

/**
 * Projects state.loop->pending_results into a candidate conversation as tool
 * messages, validates the resulting history, and commits the whole candidate.
 * Requires an existing loop recovery object. Success clears pending_results
 * and sets phase to ready. Failure preserves the original state and its full
 * result buffer so a later invocation can retry projection, not tool execution.
 */
void project(llm::LLMModel& model, State& state) {
    auto draft = state;
    for (const auto& record : state.loop->pending_results) {
        Item item;
        item.type = Kind::InvokeReturn;
        item.role = "tool";
        item.content.push_back(record.output);
        item.invoke_return = record;
        model.integrate(draft, item);
    }
    validate_history(draft);
    draft.loop->pending_results.clear();
    draft.loop->phase = LoopPhase::Ready;
    state = std::move(draft);
}

/**
 * Builds one explicit non-execution result for each undispatched tool call.
 * Preserves call identity and order, and marks each result with loop_skipped.
 * These records close the conversation protocol; they are not tool output or
 * evidence that any tool function was entered. No external operation runs here.
 */
std::vector<model_io::InvokeReturn> skipped(
    const std::vector<model_io::InvokeQuery>& calls) {
    std::vector<model_io::InvokeReturn> records;
    for (const auto& call : calls) {
        model_io::InvokeReturn record;
        record.query = call;
        record.output.raw = "Not executed: loop stopped before tool dispatch.";
        record.extras = nlohmann::json{{"loop_skipped", true}};
        records.push_back(std::move(record));
    }
    return records;
}

/**
 * Per-exchange cancellation bridge. Both active and signal are accessed only on
 * the exchange strand. Posted stop notifications own this object, so a delayed
 * notification cannot reference a destroyed coroutine or cancel a later request.
 */
struct ExchangeCancellation {
    boost::asio::cancellation_signal signal;
    bool active = true;
};

/**
 * Runs one model exchange in its own cancellable coroutine and joins it.
 *
 * Register the stop callback inside the child, after co_spawn has installed its
 * cancellation slot. Posting (rather than inline emission) avoids racing the
 * model's initiation code or a stop requested from inside a model callback.
 * The strand serializes notification with every exchange continuation, including
 * when the host runs its io_context on several threads.
 *
 * This helper does not classify model failures: its catch-all disables delayed
 * stop notifications and rethrows the original exception. run() distinguishes
 * cooperative cancellation from failures after joining the exchange.
 */
boost::asio::awaitable<Item> converse_interruptibly(
    llm::LLMModel& model,
    State snapshot,
    boost::asio::any_io_executor executor,
    std::stop_token stop) {
    auto exchange_executor = boost::asio::make_strand(executor);
    auto cancellation = std::make_shared<ExchangeCancellation>();

    auto exchange = [&model, snapshot = std::move(snapshot), stop,
                     exchange_executor, cancellation]() mutable
        -> boost::asio::awaitable<Item> {
        std::stop_callback on_stop(stop, [exchange_executor, cancellation] {
            boost::asio::post(exchange_executor, [cancellation] {
                if (cancellation->active) {
                    cancellation->signal.emit(boost::asio::cancellation_type::terminal);
                }
            });
        });

        try {
            if (stop.stop_requested()) {
                throw boost::system::system_error(boost::asio::error::operation_aborted);
            }
            auto response = co_await model.converse(std::move(snapshot));
            cancellation->active = false;
            co_return response;
        } catch (...) {
            cancellation->active = false;
            throw;
        }
    };

    co_return co_await boost::asio::co_spawn(
        exchange_executor,
        std::move(exchange),
        boost::asio::bind_cancellation_slot(
            cancellation->signal.slot(), boost::asio::use_awaitable));
}

/** Maps a terminal return status to the persisted invocation lifecycle. */
LoopStatus progress_status(RunStatus status) {
    switch (status) {
        case RunStatus::Completed:
            return LoopStatus::Completed;
        case RunStatus::Cancelled:
            return LoopStatus::Cancelled;
        case RunStatus::StepLimit:
            return LoopStatus::StepLimit;
        case RunStatus::Failed:
            return LoopStatus::Failed;
    }

    return LoopStatus::Failed;
}
} // namespace

/**
 * Implements the public run contract declared in loop.hpp.
 * The stages below keep recovery, input admission, model work, batch settlement
 * and terminal notification separate. No hook runs inside a state commit.
 */
boost::asio::awaitable<RunResult> run(
    llm::LLMModel& model,
    const tools::ToolRegistry& registry,
    eventbus::EventBus& events,
    boost::asio::any_io_executor executor,
    State& state,
    bool has_message,
    Item message,
    Options options,
    std::stop_token stop) {
    // An inherited Asio cancellation must not abandon registry's join. This
    // stop_token is bridged separately into each model exchange below.
    co_await boost::asio::this_coro::reset_cancellation_state(
        boost::asio::disable_cancellation());

    RunResult result;
    bool admitted = false;

    // Validate the request and recover any results before admitting new work.
    try {
        if (options.max_exchanges == 0) {
            throw std::invalid_argument("max_exchanges must be positive");
        }

        if (has_message) {
            if (message.type != Kind::UserInput || message.invokes ||
                message.invoke_return) {
                throw std::invalid_argument("new message requires a user message without tool calls");
            }
        } else if (state.turns.empty()) {
            throw std::invalid_argument("no turn to continue");
        }

        if (stop.stop_requested()) {
            result.status = RunStatus::Cancelled;
            co_return result;
        }
        if (state.loop &&
            (state.loop->phase == LoopPhase::Tools ||
             state.loop->phase == LoopPhase::Blocked)) {
            throw std::logic_error("previous execution requires inspection");
        }
        if (state.loop) {
            const auto& phase = state.loop->phase;
            if (phase != LoopPhase::Ready &&
                phase != LoopPhase::Model &&
                phase != LoopPhase::Projection) {
                throw std::logic_error("unknown loop recovery phase");
            }
            if (phase != LoopPhase::Projection && !state.loop->pending_results.empty()) {
                throw std::logic_error("pending results outside projection phase");
            }
            if (phase == LoopPhase::Projection) {
                if (state.loop->pending_results.empty()) {
                    throw std::logic_error("projection phase has no results");
                }
                project(model, state);
            }
        }
        validate_history(state);
        if (stop.stop_requested()) {
            result.status = RunStatus::Cancelled;
            // Recovery may have committed old results, but no new input was accepted.
            co_return result;
        }

        // Admission establishes the lifecycle observed by synchronous hooks.
        state.loop = model_io::LoopProgress{};
        state.loop->status = LoopStatus::Running;
        admitted = true;
        events.publish(RunStarted{state});
        if (has_message) {
            events.publish(BeforeInput{message});
            if (message.type != Kind::UserInput ||
                message.invokes ||
                message.invoke_return) {
                throw std::invalid_argument("input hook produced an invalid user message");
            }
            if (!stop.stop_requested()) {
                integrate(model, state, message);
                events.publish(InputCommitted{state});
            }
        }

        // Each exchange is followed by a complete tool batch, if requested.
        result.status = RunStatus::StepLimit;
        for (std::size_t step = 0; step < options.max_exchanges; ++step) {
            if (stop.stop_requested()) {
                result.status = RunStatus::Cancelled;
                break;
            }

            ModelContext context{state.system_prompt, state.tools, state.extras};
            events.publish(BeforeModel{state, context});
            // Validate prompt structure before committing writable hook changes.
            (void)context.system_prompt.render();
            if (stop.stop_requested()) {
                result.status = RunStatus::Cancelled;
                break;
            }

            auto draft = state;
            draft.system_prompt = std::move(context.system_prompt);
            draft.tools = std::move(context.tools);
            draft.extras = std::move(context.extras);
            state = std::move(draft);

            // Commit a complete model response before considering its calls.
            state.loop->phase = LoopPhase::Model;
            Item response;
            try {
                response = co_await converse_interruptibly(model, state, executor, stop);
            } catch (const boost::system::system_error& error) {
                // Cancellation classification only, not the general error boundary.
                // Built-in adapters normalize Asio cancellation to operation_aborted;
                // exhausted HTTP retries and provider errors reach the outer catch.
                // A stop request alone must not hide an independent model failure.
                if (stop.stop_requested() &&
                    error.code() == boost::asio::error::operation_aborted) {
                    state.loop->phase = LoopPhase::Ready;
                    result.status = RunStatus::Cancelled;
                    break;
                }
                throw;
            }
            if (response.type != Kind::ModelResponse || response.invoke_return) {
                throw std::logic_error("model returned an invalid response type");
            }
            validate_calls(response);
            integrate(model, state, response);
            state.loop->phase = LoopPhase::Ready;
            state.loop->completed_exchanges = ++result.completed_exchanges;
            const bool has_calls = response.invokes && !response.invokes->empty();
            std::string hook_error;
            try {
                events.publish(ModelCommitted{state});
                if (has_calls) {
                    events.publish(BeforeToolBatch{*response.invokes});
                }
            } catch (...) {
                hook_error = error_text();
            }

            if (has_calls) {
                // Mark before dispatch: an exceptional return is uncertain,
                // never grounds for replay. Preserve results BEFORE projection.
                state.loop->phase = LoopPhase::Tools;
                std::vector<model_io::InvokeReturn> records;
                if (stop.stop_requested() || !hook_error.empty()) {
                    records = skipped(*response.invokes);
                } else {
                    records = co_await registry.execute(
                        *response.invokes,
                        executor);
                }

                state.loop->pending_results = std::move(records);
                state.loop->phase = LoopPhase::Projection;
                project(model, state);
                // A primary hook error survives closure of its unanswered calls.
                if (!hook_error.empty()) {
                    throw std::runtime_error(hook_error);
                }

                events.publish(ToolResultsCommitted{state});
            } else if (!hook_error.empty()) {
                throw std::runtime_error(hook_error);
            }

            if (!has_calls) {
                result.status = RunStatus::Completed;
                break;
            }

            if (stop.stop_requested()) {
                result.status = RunStatus::Cancelled;
                break;
            }
        }
    } catch (...) {
        // General run-body boundary, including exceptions not handled by the
        // model-wait catch (HTTP failures, provider errors and hook failures).
        // Preserve committed data; recovery markers are settled below. Diagnostic
        // allocation and logging here may themselves throw: run is not noexcept.
        result.status = RunStatus::Failed;
        result.error = error_text();
        logging::Logger::error("loop failed: {}", result.error);
    }

    // Preserve the terminal state before notification, even on hook failure.
    if (admitted) {
        if (state.loop->phase == LoopPhase::Tools) {
            state.loop->phase = LoopPhase::Blocked;
        }
        if (state.loop->phase == LoopPhase::Model) {
            state.loop->phase = LoopPhase::Ready;
        }

        state.loop->status = progress_status(result.status);
        state.loop->error = result.error;
        try {
            events.publish(RunFinished{state, result});
        } catch (...) {
            result.status = RunStatus::Failed;
            const auto error = error_text();
            result.error += (result.error.empty() ? "" : "; ") +
                std::string("RunFinished: ") + error;
            state.loop->status = LoopStatus::Failed;
            state.loop->error = result.error;
            logging::Logger::error("loop finish hook failed: {}", error);
        }
    }
    co_return result;
}

} // namespace loop
