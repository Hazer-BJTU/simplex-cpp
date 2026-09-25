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

#include "llm/compat/chat_completions/model.hpp"
#include "llm/deepseek/dialect.hpp"
#include "llm/models.hpp"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/dll/alias.hpp>

#include "extensions/plugin_magic.hpp"

namespace llm::deepseek {

namespace {

class DeepSeekChatModel final
    : public llm::chat_completions::ChatCompletionsModel {
public:
    DeepSeekChatModel(boost::asio::any_io_executor executor,
                      nlohmann::json config)
        : ChatCompletionsModel(std::move(executor), std::move(config),
                               deepseek_dialect()) {}

    /** Fixed UI choices; independent of credentials, network and current settings. */
    nlohmann::json get_options() const override {
        return nlohmann::json::array({
            {
                {"name", "model"},
                {"options", {"deepseek-flash", "deepseek-v4-pro"}}
            },
            {
                {"name", "reasoning_effort"},
                {"options", {"low", "high", "max"}}
            }
        });
    }

    /** Report effective provider choices, including startup generation values. */
    nlohmann::json get_current_options() const override {
        const auto snapshot = generation();
        nlohmann::json current = nlohmann::json::object();
        if (const auto model = snapshot.find("model"); model != snapshot.end()) {
            current["model"] = *model;
        }
        // The chat adapter prefers an explicit top-level effort to the
        // shared reasoning envelope; use that same precedence here.
        if (const auto effort = snapshot.find("reasoning_effort");
            effort != snapshot.end()) {
            current["reasoning_effort"] = *effort;
        } else if (const auto reasoning = snapshot.find("reasoning");
                   reasoning != snapshot.end() && reasoning->is_object()) {
            if (const auto nested = reasoning->find("effort");
                nested != reasoning->end()) {
                current["reasoning_effort"] = *nested;
            }
        }
        return current;
    }

    /** Validate all advertised choices before atomically merging generation knobs. */
    void handle_options(const nlohmann::json& options) override {
        if (!options.is_object()) {
            throw std::invalid_argument("model options must be an object");
        }
        const auto descriptors = get_options();
        for (const auto& [name, value] : options.items()) {
            bool supported = false;
            for (const auto& descriptor : descriptors) {
                if (descriptor.at("name") == name) {
                    for (const auto& choice : descriptor.at("options")) {
                        supported = supported || value == choice;
                    }
                }
            }
            if (!supported) {
                throw std::invalid_argument("unsupported DeepSeek option: " + name);
            }
        }
        if (!options.empty()) {
            apply_generation_patch(options);
        }
    }

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

// Admission block: toolchain fingerprint, checked by the loader before any
// alias is resolved (a module from any other build context is rejected).
SIMPLEX_EXPORT_PLUGIN_MAGIC;
