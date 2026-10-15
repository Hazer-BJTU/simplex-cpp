#pragma once

#include <mutex>
#include "llm/custom_chat/configuration.hpp"
#include "llm/compat/chat_completions/model.hpp"

namespace llm::custom_chat {

class CustomChatCompletionsModel final : public chat_completions::ChatCompletionsModel {
    std::shared_ptr<const CompatibilityConfig> compatibility_;
    mutable std::mutex mutex_;
    Json selected_;
    bool built_ = false;

    static Json wire_config(Json config) {
        config.erase("compatibility");
        return config;
    }
    CustomChatCompletionsModel(boost::asio::any_io_executor executor, Json config,
                              std::shared_ptr<const CompatibilityConfig> compatibility)
        : ChatCompletionsModel(std::move(executor), wire_config(std::move(config)),
              std::make_shared<ConfigurableDialect>(compatibility)),
          compatibility_(std::move(compatibility)), selected_(compatibility_->defaults) {}
protected:
    std::pair<Json, chat_completions::ChatCompletionsDialectPtr>
    exchange_configuration() const override {
        // Serialize with handle_options() and this provider's virtual
        // set_generation() overrides. Inherited apply_generation_patch() and
        // reset_generation() use only the base generation mutex; callers must
        // serialize those APIs themselves if they need a coordinated update.
        std::lock_guard lock(mutex_);
        return {generation_snapshot(), std::make_shared<ConfigurableDialect>(compatibility_, selected_)};
    }
public:
    CustomChatCompletionsModel(boost::asio::any_io_executor executor, const Json& config)
        : CustomChatCompletionsModel(std::move(executor), config,
              std::make_shared<CompatibilityConfig>(config.value("compatibility", Json::object()))) {}

    bool build() noexcept override {
        std::lock_guard lock(mutex_);
        built_ = ChatCompletionsModel::build();
        return built_;
    }
    Json get_options() const override {
        Json result = Json::array();
        for (const auto& option : compatibility_->options) {
            Json choices = Json::array();
            for (const auto& choice : option.choices) choices.push_back(choice.value);
            result.push_back({{"name", option.name}, {"options", std::move(choices)}});
        }
        return result;
    }
    Json get_current_options() const override {
        std::lock_guard lock(mutex_);
        return selected_;
    }
    void handle_options(const Json& patch) override {
        std::lock_guard lock(mutex_);
        if (!built_) throw std::logic_error("chat-completions options require build()");
        require(patch.is_object(), "options update must be an object");
        Json candidate = selected_;
        for (const auto& [name, value] : patch.items()) {
            bool found = false;
            for (const auto& option : compatibility_->options) {
                if (option.name != name) continue;
                for (const auto& choice : option.choices)
                    found = found || value == choice.value;
            }
            require(found, "unsupported option name or value");
            candidate[name] = value;
        }
        selected_ = std::move(candidate);
    }
    void set_generation(Json patch) override {
        std::lock_guard lock(mutex_);
        require(!patch.is_object() || !patch.contains("compatibility"),
                "compatibility is startup configuration");
        apply_generation_patch(std::move(patch));
    }
    void set_generation(GenerationPreset preset) override {
        Json patch = Json::object();
        if (preset.model) patch["model"] = *preset.model;
        // Preserve the shared preset spelling. Native-mode callers declare
        // any required wire translation explicitly in their request rules.
        if (preset.effort) patch["reasoning"] = {{"effort", to_string(*preset.effort)}};
        set_generation(std::move(patch));
    }
    boost::asio::awaitable<Json> provider_info() override {
        // The coroutine owns the returned value; no network or guessed URL.
        co_return compatibility_->models;
    }
};
} // namespace llm::custom_chat
