#pragma once

#include <memory>

#include <nlohmann/json.hpp>

#include "dataclass/endpoint_config.hpp"

namespace llm::chat_completions {

/** Provider spelling around the OpenAI-compatible Chat Completions protocol. */
class ChatCompletionsDialect {
public:
    virtual ~ChatCompletionsDialect() = default;

    virtual model_io::ModelEndpoint default_endpoint() const {
        model_io::ModelEndpoint endpoint;
        endpoint.request_path = "/v1/chat/completions";
        endpoint.auth.scheme = model_io::AuthScheme::Bearer;
        return endpoint;
    }

    virtual void transform_request(nlohmann::json&) const {}

    virtual nlohmann::json normalize_chunk(nlohmann::json chunk) const {
        return chunk;
    }
};

using ChatCompletionsDialectPtr =
    std::shared_ptr<const ChatCompletionsDialect>;

inline ChatCompletionsDialectPtr default_dialect() {
    static const auto dialect =
        std::make_shared<const ChatCompletionsDialect>();
    return dialect;
}

} // namespace llm::chat_completions
