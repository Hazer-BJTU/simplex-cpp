#include "llm/qwen/dialect.hpp"
#include "llm/compat/chat_completions/model.hpp"
#include "extensions/plugin_magic.hpp"

#include <boost/dll/alias.hpp>
#include <mutex>

namespace llm::qwen {
namespace {

/**
 * Provider choices on the shared, reentrant Chat Completions implementation.
 * The extra mutex serializes validation plus mutation across both low-level
 * setters and remote options; the base mutex still protects every reader.
 * No lock is held across a coroutine suspension or external request.
 */
class QwenChatModel final : public llm::chat_completions::ChatCompletionsModel {
public:
    QwenChatModel(boost::asio::any_io_executor executor, nlohmann::json config)
        : ChatCompletionsModel(std::move(executor), std::move(config), qwen_dialect()) {}

    bool build() noexcept override {
        try {
            auto candidate = _config;
            prepare_generation(candidate);
            return ChatCompletionsModel::build();
        } catch (const std::exception& error) {
            logging::Logger::error(std::string("Qwen configuration rejected: ") + error.what());
            return false;
        }
    }

    /** Local, stable descriptors; never query the endpoint or account catalog. */
    nlohmann::json get_options() const override {
        return nlohmann::json::array({
            {{"name", "model"}, {"options", {"qwen3.8-flash", "qwen3.8-max"}}},
            {{"name", "enable_thinking"}, {"options", {"enabled", "disabled"}}},
            {{"name", "reasoning_effort"}, {"options", {"low", "medium", "xhigh"}}}
        });
    }

    /** Report effective values; a budget is mapped to its documented effort band. */
    nlohmann::json get_current_options() const override {
        auto snapshot = generation();
        prepare_generation(snapshot);
        nlohmann::json current = nlohmann::json::object();
        for (const char* key : {"model", "enable_thinking", "reasoning_effort"}) {
            if (snapshot.contains(key)) {
                current[key] = snapshot[key];
            }
        }
        if (current.contains("enable_thinking")) {
            current["enable_thinking"] = current["enable_thinking"].get<bool>()
                ? "enabled" : "disabled";
        }
        if (is_qwen38(snapshot.at("model").get<std::string>())
            && snapshot.contains("thinking_budget")) {
            const auto& budget = snapshot["thinking_budget"];
            current["reasoning_effort"] = budget <= 4096 ? "low"
                : budget <= 16384 ? "medium" : "xhigh";
        }
        return current;
    }

    /** All advertised fields commit together; an effort choice replaces a budget. */
    void handle_options(const nlohmann::json& options) override {
        if (!options.is_object()) {
            throw std::invalid_argument("Qwen model options must be an object");
        }
        const auto descriptors = get_options();
        for (const auto& [name, value] : options.items()) {
            bool supported = false;
            for (const auto& descriptor : descriptors) {
                if (descriptor.at("name") == name) {
                    for (const auto& choice : descriptor.at("options")) {
                        supported = supported || (value.type() == choice.type() && value == choice);
                    }
                }
            }
            if (!supported) {
                throw std::invalid_argument("unsupported Qwen model option");
            }
        }
        if (options.empty()) {
            return;
        }
        auto patch = options;
        if (patch.contains("enable_thinking")) {
            patch["enable_thinking"] = patch["enable_thinking"] == "enabled";
        }
        if (patch.contains("reasoning_effort")) {
            patch["thinking_budget"] = nullptr;
            patch["reasoning"] = {{"effort", nullptr}};
        }
        set_generation(std::move(patch));
    }

    void set_generation(nlohmann::json patch) override {
        std::lock_guard lock(options_mutex_);
        if (!patch.is_object()) {
            throw std::invalid_argument("Qwen generation patch must be an object");
        }
        auto candidate = generation();
        candidate.merge_patch(patch);
        prepare_generation(candidate);
        apply_generation_patch(std::move(patch));
    }

    /** An explicit typed effort replaces both effort spellings and any budget atomically. */
    void set_generation(GenerationPreset preset) override {
        nlohmann::json patch = nlohmann::json::object();
        if (preset.model) {
            patch["model"] = *preset.model;
        }
        if (preset.effort) {
            patch["reasoning_effort"] = to_string(*preset.effort);
            patch["reasoning"] = {{"effort", nullptr}};
            patch["thinking_budget"] = nullptr;
        }
        set_generation(std::move(patch));
    }

    /** No verified catalog API contract on this platform; never guess a URL. */
    boost::asio::awaitable<nlohmann::json> provider_info() override {
        throw LLMUnsupportedOperation("Qwen provider_info is not supported; use the platform model catalog");
        co_return nlohmann::json{};
    }

private:
    std::mutex options_mutex_;
};

class QwenPlugin final : public LLMModelExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override { return LLM_PLUGIN_ABI_VERSION; }
    std::string_view name() const noexcept override { return "qwen"; }
};

} // namespace

std::unique_ptr<extension::ExtensionContext> create_llm_plugin() {
    return std::make_unique<QwenPlugin>();
}

std::unique_ptr<LLMModel> create_llm_model(
    boost::asio::any_io_executor executor, const nlohmann::json& config) {
    return std::make_unique<QwenChatModel>(std::move(executor), config);
}

} // namespace llm::qwen

BOOST_DLL_ALIAS(llm::qwen::create_llm_plugin, create_llm_plugin)
BOOST_DLL_ALIAS(llm::qwen::create_llm_model, create_llm_model)
SIMPLEX_EXPORT_PLUGIN_MAGIC;
