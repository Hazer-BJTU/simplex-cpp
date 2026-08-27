#pragma once

#include <memory>
#include <string_view>

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

    /**
     * The provider's name as broadcast in llm/chat_completions/events.hpp
     * (empty for a generic no-dialect model). Purely informational — one
     * word identifying who generated the output.
     */
    virtual std::string_view provider_name() const { return {}; }

    /**
     * The catalogue path provider_info() queries (OpenAI-compatible
     * "list models"), sibling of the default exchange path's /v1 prefix.
     * Override when the provider drops the prefix (e.g. DeepSeek: /models).
     */
    virtual std::string models_path() const { return "/v1/models"; }

    /**
     * The account-balance companion provider_info() attaches: when
     * non-empty, a second single-shot GET over the same endpoint/auth
     * whose JSON answer rides back as the "balance" member beside the
     * models array (DeepSeek: /user/balance). Empty — the default — means
     * the provider exposes no such endpoint and provider_info() returns
     * the bare models array.
     */
    virtual std::string balance_path() const { return {}; }

    /**
     * Whether replayed assistant messages re-emit MessageItem::reasoning as
     * the wire's reasoning_content field. DeepSeek thinking mode rejects
     * (400) a tools request whose intermediate assistant messages omit it,
     * while strict OpenAI-compatible servers reject the unknown field — so
     * the shared default stays off and only thinking-mode providers opt in.
     */
    virtual bool replay_assistant_reasoning() const { return false; }

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
