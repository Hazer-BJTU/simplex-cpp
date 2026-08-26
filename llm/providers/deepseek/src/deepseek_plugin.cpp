// deepseek_plugin.cpp — the first concrete provider plugin on the
// provider-neutral chat-completions adapter.
//
// Thin by design: everything protocol-level lives in the shared
// llm_chat_completions adapter (interpreter / stream handler / reader), so a
// provider plugin only supplies its dialect — endpoint defaults (base URL,
// Bearer auth, user agent) plus DeepSeek's deviations from the neutral wire:
// thinking-mode default and effort vocabulary, the reasoning_content replay
// requirement, undocumented/deprecated parameter stripping, and the
// prompt_cache_hit_tokens usage spelling (see llm/deepseek/dialect.hpp). The
// two standard aliases the loader resolves are exported at the bottom (see
// llm/models.hpp).

#include "llm/chat_completions/model.hpp"
#include "llm/deepseek/dialect.hpp"
#include "llm/models.hpp"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/dll/alias.hpp>

namespace llm::deepseek {

namespace {

class DeepSeekChatModel final
    : public llm::chat_completions::ChatCompletionsModel {
public:
    DeepSeekChatModel(boost::asio::any_io_executor executor,
                      nlohmann::json config)
        : ChatCompletionsModel(std::move(executor), std::move(config),
                               deepseek_dialect()) {}
};

class DeepSeekPlugin final : public LLMModelExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override {
        return LLM_PLUGIN_ABI_VERSION;
    }

    std::string_view name() const noexcept override { return "deepseek"; }
};

} // namespace

std::unique_ptr<extension::ExtensionContext> create_llm_plugin() {
    return std::make_unique<DeepSeekPlugin>();
}

std::unique_ptr<LLMModel> create_llm_model(
    boost::asio::any_io_executor executor, const nlohmann::json& config) {
    return std::make_unique<DeepSeekChatModel>(std::move(executor), config);
}

} // namespace llm::deepseek

BOOST_DLL_ALIAS(llm::deepseek::create_llm_plugin, create_llm_plugin)
BOOST_DLL_ALIAS(llm::deepseek::create_llm_model, create_llm_model)
