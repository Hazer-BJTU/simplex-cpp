#pragma once

#include <utility>

#include <nlohmann/json.hpp>

#include "dataclass/model_io.hpp"
#include "endpoint/model_request.hpp"
#include "llm/chat_completions/dialect.hpp"

namespace llm::chat_completions {

/** Builds a streaming POST /chat/completions request for one ReAct exchange. */
class ChatCompletionsInterpreter : public endpoint::ModelRequestInterpreter {
public:
    explicit ChatCompletionsInterpreter(
        ChatCompletionsDialectPtr dialect = default_dialect())
        : _dialect(dialect ? std::move(dialect) : default_dialect()) {}

    HttpRequest build_request(
        const model_io::AgentInputState& conversation,
        const model_io::ModelEndpoint& endpoint,
        const nlohmann::json& generation) override;

private:
    ChatCompletionsDialectPtr _dialect;
};

} // namespace llm::chat_completions
