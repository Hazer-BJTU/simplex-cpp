#include "agent_loop.hpp"
#include "tools/invoke_exception.hpp"

#include <iostream>

namespace tools_example {
namespace asio = boost::asio;

const char* type_name(model_io::InvokeType type) {
    switch (type) {
        case model_io::InvokeType::ReadOnly:    return "read_only";
        case model_io::InvokeType::ParallWrite: return "parall_write";
        case model_io::InvokeType::SerialWrite: return "serial_write";
    }
    return "unknown";
}

const char* security_name(model_io::InvokeSecurity security) {
    switch (security) {
        case model_io::InvokeSecurity::DefaultDeny:    return "default_deny";
        case model_io::InvokeSecurity::RequireConfirm: return "require_confirm";
        case model_io::InvokeSecurity::Trusted:        return "trusted";
    }
    return "unknown";
}

// ---- one user turn: the ReAct agent loop --------------------------------------

/// One record as the terminal shows it: a line for the SETTLED call (which is
/// what the registry answers with — defaults filled in, type and security
/// written by the tool), then the result text the model is about to read. For
/// these tools that text is mostly the child's own output, and it is printed
/// here as the model receives it — which is why the header line is a header and
/// the result is not indented under it.
void report_record(const model_io::InvokeReturn& record) {
    std::cout << "  [tool] " << record.query.id << " " << record.query.name
              << " " << record.query.arguments.dump() << " — settled "
              << type_name(record.query.type) << "/"
              << security_name(record.query.security) << "\n";

    if (const std::optional<tools::InvokeException::Stage> stage =
            tools::error_stage(record)) {
        // is_error()/error_stage() read the marker the failure record carries
        // in extras; the model itself only ever sees output.raw.
        std::cout << "         FAILED (" << tools::InvokeException::stage_key(*stage)
                  << "): " << record.output.raw << "\n";
        return;
    }
    // The result text VERBATIM, at the left margin: it is the same bytes the
    // model is sent, which is the point of the format — a human reading the
    // transcript over the model's shoulder sees a child's output as the child
    // printed it, not as an escaped string inside an object.
    std::cout << record.output.raw << "\n";
}

/// Run the agent loop for the last user turn through the LLMModel contract and
/// the tool registry: converse() for one exchange, integrate() to fold every
/// item into the growing AgentInputState, and — when the response carries
/// calls — ONE ToolRegistry::execute() batch, whose records go back into the
/// conversation as InvokeReturn items. A response with no invokes is the final
/// answer.
///
/// `max_steps` is the turn's exchange budget (--max-steps), and running out of
/// it is not a failure: every call the model made has already run and its
/// result is already in the conversation, so the next user message continues
/// from there — which is what the message on stderr says, because a terminal
/// that just stops answering reads like a crash otherwise.
asio::awaitable<void> run_turn(llm::LLMModel& model,
                               const tools::ToolRegistry& registry,
                               model_io::AgentInputState& state,
                               std::size_t max_steps) {
    for (std::size_t step = 0; step < max_steps; ++step) {
        model_io::MessageItem item = co_await model.converse(state);
        model.integrate(state, item);

        // Reasoning already streamed live through the bus subscription in
        // main(); the visible answer prints here, per exchange.
        for (const model_io::Content& part : item.content) {
            if (!part.raw.empty()) std::cout << part.raw << "\n";
        }

        if (!item.invokes || item.invokes->empty()) {
            // Final answer. Report the exchange's token accounting (the
            // cache_hit field is DeepSeek's prompt_cache_hit_tokens bridged by
            // the dialect).
            if (item.cost) {
                std::cerr << "[cost] prompt=" << item.cost->prompt
                          << " generated=" << item.cost->generated
                          << " cache_hit=" << item.cost->cache_hit << "\n";
            }
            co_return;
        }

        // The calls as they arrived — the raw form, before anything settled
        // them. What runs is the settled form, which the records below carry.
        std::vector<model_io::InvokeQuery> batch = *item.invokes;
        for (const model_io::InvokeQuery& call : batch) {
            std::cout << "  [call] " << call.id << " " << call.name << " "
                      << call.arguments.dump() << "\n";
        }

        // Steps 1-4 in one await: the registry settles the whole batch, runs
        // the serial calls one at a time, overlaps the ReadOnly ones, and
        // answers with one record per call in call order. The convenience
        // overload takes the executor from this coroutine — the io context the
        // store and the session strands live on.
        std::vector<model_io::InvokeReturn> records =
            co_await registry.execute(std::move(batch));

        for (const model_io::InvokeReturn& record : records) {
            report_record(record);

            // The record IS the tool result: the text the model reads
            // (output), plus the settled query whose id the next request
            // correlates it by (invoke_return). Nothing here rebuilds or
            // annotates it.
            model_io::MessageItem result;
            result.type = model_io::MessageItemType::InvokeReturn;
            result.role = "tool";
            result.content.push_back(record.output);
            result.invoke_return = record;
            model.integrate(state, result);
        }
    }
    std::cerr << "agent loop hit its " << max_steps
              << "-step budget before a final answer.\n"
              << "  Every tool call it made has already run, and the results "
                 "are in the conversation:\n"
              << "  send another message (\"continue\" is enough) and it picks "
                 "up from there, or relaunch with a larger --max-steps.\n";
}

asio::awaitable<bool> run_user_turn(
    llm::LLMModel& model, const tools::ToolRegistry& registry,
    model_io::AgentInputState& state, model_io::MessageItem input,
    std::size_t max_steps) {
    model.integrate(state, input);
    try {
        co_await run_turn(model, registry, state, max_steps);
        co_return true;
    } catch (const std::exception& error) {
        // Tools may already have changed files or live processes. Those effects
        // cannot be rolled back by deleting a conversation turn. Preserve even
        // an unanswered input so recovery sees the complete submitted history.
        std::cerr << "turn failed: " << error.what() << "\n"
                  << "Conversation history retained. Tool effects were not rolled back; "
                     "inspect /sessions before continuing.\n";
        co_return false;
    }
}
} // namespace tools_example
