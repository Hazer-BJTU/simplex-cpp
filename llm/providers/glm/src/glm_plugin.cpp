// glm_plugin.cpp — GLM (Zhipu AI / BigModel) on the provider-neutral
// chat-completions adapter.
//
// Thin by design: everything protocol-level lives in the shared
// llm_chat_completions adapter (interpreter / stream handler / reader), so
// this plugin only supplies its dialect — endpoint defaults, the always-on
// thinking policy of the GLM-5.3 family, the effort translation, and the
// finish-reason mapping (see llm/glm/dialect.hpp) — plus the runtime option
// surface the panel drives. The two standard aliases the loader resolves are
// exported at the bottom (see llm/models.hpp).

#include "llm/compat/chat_completions/model.hpp"
#include "llm/glm/dialect.hpp"
#include "llm/models.hpp"

#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include <boost/dll/alias.hpp>

#include "extensions/plugin_magic.hpp"

namespace llm::glm {

namespace {

class GlmChatModel final
    : public llm::chat_completions::ChatCompletionsModel {
public:
    GlmChatModel(boost::asio::any_io_executor executor,
                 nlohmann::json config)
        : ChatCompletionsModel(std::move(executor), std::move(config),
                               glm_dialect()) {}

    /**
     * Validate startup configuration before the instance is exposed.
     *
     * The service rejects an impossible control with HTTP 400 after a billed
     * round trip; rejecting it here turns the same mistake into a refused
     * model build the host already reports. The base build still runs, so
     * endpoint/retry parsing keeps one owner.
     */
    bool build() noexcept override {
        try {
            auto candidate = _config;
            prepare_generation(candidate);
            return ChatCompletionsModel::build();
        } catch (const std::exception& error) {
            logging::Logger::error(
                std::string("GLM configuration rejected: ") + error.what());
            return false;
        }
    }

    /** Fixed UI choices; independent of credentials, network and current settings. */
    nlohmann::json get_options() const override {
        return nlohmann::json::array({
            {
                {"name", "model"},
                {"options", {"glm-5.3-flash", "glm-5.3-flashx", "glm-5.3",
                             "glm-5.2", "glm-4.7"}}
            },
            {
                {"name", "reasoning_effort"},
                {"options", {"low", "high", "max"}}
            }
        });
    }

    /** Report effective provider choices, including startup generation values. */
    nlohmann::json get_current_options() const override {
        auto snapshot = generation();
        // Normalise first: an alias the dialect translates (medium/xhigh on
        // the 5.3 family) must be reported as the depth actually sent.
        prepare_generation(snapshot);
        nlohmann::json current = nlohmann::json::object();
        for (const char* key : {"model", "reasoning_effort"}) {
            if (const auto it = snapshot.find(key); it != snapshot.end()) {
                current[key] = *it;
            }
        }
        return current;
    }

    /** Validate every advertised choice before atomically merging the patch. */
    void handle_options(const nlohmann::json& options) override {
        if (!options.is_object()) {
            throw std::invalid_argument("GLM model options must be an object");
        }
        const auto descriptors = get_options();
        for (const auto& [name, value] : options.items()) {
            bool supported = false;
            for (const auto& descriptor : descriptors) {
                if (descriptor.at("name") != name) continue;
                for (const auto& choice : descriptor.at("options")) {
                    supported = supported ||
                        (value.type() == choice.type() && value == choice);
                }
            }
            if (!supported) {
                // The name, never the value: an option patch is caller data.
                throw std::invalid_argument("unsupported GLM model option");
            }
        }
        if (options.empty()) return;
        set_generation(options);
    }

    void set_generation(nlohmann::json patch) override {
        std::lock_guard lock(options_mutex_);
        if (!patch.is_object()) {
            throw std::invalid_argument("GLM generation patch must be an object");
        }
        auto candidate = generation();
        candidate.merge_patch(patch);
        prepare_generation(candidate);
        apply_generation_patch(std::move(patch));
    }

    /**
     * The typed preset carries minimal|low|medium|high. Only low/high/medium
     * are expressible on the 5.3 family (medium arriving as its high alias),
     * so an effort the family cannot honour is refused rather than silently
     * re-pointed at a different depth; `max` is reachable through the JSON
     * option surface, which is what the panel uses.
     */
    void set_generation(GenerationPreset preset) override {
        nlohmann::json patch = nlohmann::json::object();
        if (preset.model) {
            patch["model"] = *preset.model;
        }
        if (preset.effort) {
            patch["reasoning_effort"] = to_string(*preset.effort);
            patch["reasoning"] = {{"effort", nullptr}};
        }
        set_generation(std::move(patch));
    }

private:
    // Serialises validation plus mutation across both setter tiers and the
    // remote option patch; the base mutex still protects every reader. No
    // lock is held across a coroutine suspension or an external request.
    std::mutex options_mutex_;
};

class GlmPlugin final : public LLMModelExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override {
        return LLM_PLUGIN_ABI_VERSION;
    }

    std::string_view name() const noexcept override { return "glm"; }
};

} // namespace

std::unique_ptr<extension::ExtensionContext> create_llm_plugin() {
    return std::make_unique<GlmPlugin>();
}

std::unique_ptr<LLMModel> create_llm_model(
    boost::asio::any_io_executor executor, const nlohmann::json& config) {
    return std::make_unique<GlmChatModel>(std::move(executor), config);
}

} // namespace llm::glm

BOOST_DLL_ALIAS(llm::glm::create_llm_plugin, create_llm_plugin)
BOOST_DLL_ALIAS(llm::glm::create_llm_model, create_llm_model)

// Admission block: toolchain fingerprint, checked by the loader before any
// alias is resolved (a module from any other build context is rejected).
SIMPLEX_EXPORT_PLUGIN_MAGIC;
