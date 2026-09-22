#pragma once

#include "llm/models.hpp"
#include "tools/registry.hpp"

namespace tools_example {
const char* type_name(model_io::InvokeType type);
const char* security_name(model_io::InvokeSecurity security);

// Run one user turn and report failures without rolling back execution history.
// Returns false on failure; completed model/tool records remain in state.
boost::asio::awaitable<bool> run_user_turn(
    llm::LLMModel& model, const tools::ToolRegistry& registry,
    model_io::AgentInputState& state, model_io::MessageItem input,
    std::size_t max_steps);
} // namespace tools_example
