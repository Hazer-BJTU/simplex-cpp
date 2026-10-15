#pragma once

#include <algorithm>
#include <charconv>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "llm/compat/chat_completions/dialect.hpp"

namespace llm::custom_chat {
using Json = nlohmann::json;

inline void require(bool condition, const std::string& where) {
    if (!condition) throw std::invalid_argument("chat-completions: " + where);
}
inline void keys(const Json& value, std::initializer_list<std::string_view> allowed,
                 const std::string& where) {
    require(value.is_object(), where + " must be an object");
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(),
                where + ": unknown field " + key);
    }
}
inline std::string string_field(const Json& value, const char* key, const std::string& where) {
    require(value.contains(key) && value.at(key).is_string()
            && !value.at(key).get_ref<const std::string&>().empty(),
            where + "/" + key + " must be a nonempty string");
    return value.at(key).get<std::string>();
}

// A deliberately small rule language, not RFC 6902. Resolve paths ourselves:
// nlohmann's non-const operator[] can otherwise resize arrays unexpectedly.
class PatchProgram {
    struct Rule {
        std::string op, location;
        std::vector<std::string> path, from;
        Json value;
        bool skip = false;
    };
    std::vector<Rule> rules_;

    static std::vector<std::string> tokens(const std::string& path) {
        Json::json_pointer pointer(path); // validates JSON Pointer escapes
        std::vector<std::string> result;
        while (!pointer.empty()) {
            result.push_back(pointer.back());
            pointer.pop_back();
        }
        std::reverse(result.begin(), result.end());
        return result;
    }
    static std::size_t index(const std::string& token) {
        std::size_t value = 0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        require(!token.empty() && (token.size() == 1 || token[0] != '0')
                && error == std::errc{} && end == token.data() + token.size(),
                "invalid array index");
        return value;
    }
    static Json* locate(Json& root, const std::vector<std::string>& path,
                        std::size_t count, bool create) {
        Json* node = &root;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& key = path[i];
            if (node->is_object()) {
                if (!node->contains(key)) {
                    if (!create) return nullptr;
                    (*node)[key] = Json::object();
                }
                node = &node->at(key);
            } else if (node->is_array()) {
                const auto slot = index(key);
                if (!create && slot >= node->size()) return nullptr;
                node = &node->at(slot);
            } else {
                throw std::invalid_argument("path traverses a non-container");
            }
        }
        return node;
    }
    static void set(Json& root, const std::vector<std::string>& path, Json value) {
        if (path.empty()) { root = std::move(value); return; }
        auto* parent = locate(root, path, path.size() - 1, true);
        if (parent->is_object()) (*parent)[path.back()] = std::move(value);
        else if (parent->is_array()) parent->at(index(path.back())) = std::move(value);
        else throw std::invalid_argument("destination parent is not a container");
    }
    static void remove(Json& root, const std::vector<std::string>& path) {
        require(!path.empty(), "remove cannot delete the document root");
        auto* parent = locate(root, path, path.size() - 1, false);
        if (!parent) return;
        if (parent->is_object()) parent->erase(path.back());
        else if (parent->is_array()) {
            const auto i = index(path.back());
            if (i < parent->size()) parent->erase(parent->begin() + i);
        } else throw std::invalid_argument("remove parent is not a container");
    }
public:
    PatchProgram() = default;
    PatchProgram(const Json& rules, const std::string& where) {
        require(rules.is_array(), where + " must be an array");
        for (std::size_t i = 0; i < rules.size(); ++i) {
            const auto location = where + "/" + std::to_string(i);
            try {
                const auto& source = rules[i];
                Rule rule;
                rule.location = location;
                rule.op = string_field(source, "op", location);
                require(source.contains("path") && source["path"].is_string(), "path must be a string");
                rule.path = tokens(source["path"].get<std::string>());
                if (rule.op == "set") {
                    keys(source, {"op", "path", "value"}, location);
                    require(source.contains("value"), "set requires value");
                    rule.value = source["value"];
                } else if (rule.op == "remove") {
                    keys(source, {"op", "path"}, location);
                    require(!rule.path.empty(), "remove cannot delete the document root");
                } else {
                    require(rule.op == "copy" || rule.op == "move", "unsupported operation");
                    keys(source, {"op", "path", "from", "if_missing"}, location);
                    require(source.contains("from") && source["from"].is_string(), "from must be a string");
                    rule.from = tokens(source["from"].get<std::string>());
                    if (source.contains("if_missing")) {
                        require(source["if_missing"] == "skip" || source["if_missing"] == "error",
                                "if_missing must be skip or error");
                        rule.skip = source["if_missing"] == "skip";
                    }
                    if (rule.op == "move") {
                        require(!rule.from.empty(), "move cannot remove the document root");
                        require(rule.path.size() <= rule.from.size()
                                || !std::equal(rule.from.begin(), rule.from.end(), rule.path.begin()),
                                "move destination cannot descend from source");
                    }
                }
                rules_.push_back(std::move(rule));
            } catch (const std::exception&) {
                // Do not include JSON exception text: it may contain user data.
                throw std::invalid_argument("chat-completions: invalid rule at " + location);
            }
        }
    }
    void apply(Json& document) const {
        for (const auto& rule : rules_) {
            try {
                if (rule.op == "set") set(document, rule.path, rule.value);
                else if (rule.op == "remove") remove(document, rule.path);
                else {
                    auto* source = locate(document, rule.from, rule.from.size(), false);
                    if (!source && rule.skip) continue;
                    require(source != nullptr, "source is missing");
                    if (rule.op == "move" && rule.from == rule.path) continue;
                    Json value = *source;
                    if (rule.op == "move") remove(document, rule.from);
                    set(document, rule.path, std::move(value));
                }
            } catch (const std::exception&) {
                throw std::invalid_argument("chat-completions: rule failed at " + rule.location);
            }
        }
    }
};

struct OptionChoice { std::string value; PatchProgram patch; };
struct Option { std::string name; std::vector<OptionChoice> choices; };
struct MessagePatch { std::string role; PatchProgram patch; };

struct CompatibilityConfig {
    bool replay = false;
    PatchProgram request, response;
    std::vector<MessagePatch> messages;
    Json models = Json::array();
    std::vector<Option> options;
    Json defaults = Json::object();
    std::optional<Json::json_pointer> prompt{Json::json_pointer("/prompt_tokens")};
    std::optional<Json::json_pointer> generated{Json::json_pointer("/completion_tokens")};
    std::optional<Json::json_pointer> cache{Json::json_pointer("/prompt_tokens_details/cached_tokens")};

    explicit CompatibilityConfig(const Json& config = Json::object()) {
        keys(config, {"replay_assistant_reasoning", "request", "response", "token_cost", "models", "options"},
             "compatibility");
        if (config.contains("replay_assistant_reasoning")) {
            require(config["replay_assistant_reasoning"].is_boolean(), "replay_assistant_reasoning must be Boolean");
            replay = config["replay_assistant_reasoning"].get<bool>();
        }
        if (config.contains("request")) {
            const auto& r = config["request"];
            keys(r, {"patch", "message_patches"}, "compatibility/request");
            if (r.contains("patch")) request = PatchProgram(r["patch"], "compatibility/request/patch");
            if (r.contains("message_patches")) {
                require(r["message_patches"].is_array(), "message_patches must be an array");
                for (std::size_t i = 0; i < r["message_patches"].size(); ++i) {
                    const auto& m = r["message_patches"][i];
                    const auto where = "compatibility/request/message_patches/" + std::to_string(i);
                    keys(m, {"role", "patch"}, where);
                    require(m.contains("patch"), where + " requires patch");
                    messages.push_back({string_field(m, "role", where), PatchProgram(m["patch"], where + "/patch")});
                }
            }
        }
        if (config.contains("response")) {
            keys(config["response"], {"chunk_patch"}, "compatibility/response");
            if (config["response"].contains("chunk_patch"))
                response = PatchProgram(config["response"]["chunk_patch"], "compatibility/response/chunk_patch");
        }
        if (config.contains("token_cost")) {
            const auto& cost = config["token_cost"];
            keys(cost, {"prompt", "generated", "cache_hit"}, "compatibility/token_cost");
            const auto read = [&](const char* key, auto& target) {
                if (!cost.contains(key)) return;
                if (cost[key].is_null()) { target.reset(); return; }
                require(cost[key].is_string(), std::string("token_cost/") + key + " must be a pointer or null");
                try { target = Json::json_pointer(cost[key].get<std::string>()); }
                catch (...) { throw std::invalid_argument(std::string("invalid token_cost pointer: ") + key); }
            };
            read("prompt", prompt); read("generated", generated); read("cache_hit", cache);
        }
        if (config.contains("models")) {
            models = config["models"];
            require(models.is_array(), "models must be an array");
            std::set<std::string> ids;
            for (const auto& model : models) {
                require(model.is_object(), "models entries must be objects");
                require(ids.insert(string_field(model, "id", "models")).second, "duplicate model id");
            }
        }
        if (config.contains("options")) {
            require(config["options"].is_array(), "options must be an array");
            std::set<std::string> names;
            for (std::size_t i = 0; i < config["options"].size(); ++i) {
                const auto& entry = config["options"][i];
                const auto where = "compatibility/options/" + std::to_string(i);
                keys(entry, {"name", "default", "choices"}, where);
                Option option{string_field(entry, "name", where), {}};
                require(names.insert(option.name).second, where + ": duplicate name");
                require(entry.contains("choices") && entry["choices"].is_array()
                        && !entry["choices"].empty(), where + "/choices must be a nonempty array");
                std::set<std::string> values;
                for (std::size_t j = 0; j < entry["choices"].size(); ++j) {
                    const auto& choice = entry["choices"][j];
                    const auto location = where + "/choices/" + std::to_string(j);
                    keys(choice, {"value", "patch"}, location);
                    const auto value = string_field(choice, "value", location);
                    require(values.insert(value).second, location + ": duplicate value");
                    require(choice.contains("patch"), location + " requires patch");
                    option.choices.push_back({value, PatchProgram(choice["patch"], location + "/patch")});
                }
                if (entry.contains("default")) {
                    const auto value = string_field(entry, "default", where);
                    require(values.contains(value), where + ": default must name a choice");
                    defaults[option.name] = value;
                }
                options.push_back(std::move(option));
            }
        }
    }
};

class ConfigurableDialect final : public chat_completions::ChatCompletionsDialect {
    std::shared_ptr<const CompatibilityConfig> config_;
    Json selected_;
    static std::optional<std::uint64_t> count(const Json& usage, const std::optional<Json::json_pointer>& path) {
        if (!path || !usage.contains(*path)) return std::nullopt;
        const auto& value = usage.at(*path);
        if (value.is_number_unsigned()) return value.get<std::uint64_t>();
        if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
            return static_cast<std::uint64_t>(value.get<std::int64_t>());
        return std::nullopt;
    }
public:
    explicit ConfigurableDialect(std::shared_ptr<const CompatibilityConfig> config, Json selected = Json::object())
        : config_(std::move(config)), selected_(std::move(selected)) {}
    std::string_view provider_name() const override { return "chat-completions"; }
    bool native_request() const override { return true; }
    bool strict_chunk_transform() const override { return true; }
    bool maps_raw_usage() const override { return true; }
    bool replay_assistant_reasoning() const override { return config_->replay; }
    void transform_request(Json& body) const override {
        // The interpreter supplies this shape. Validate it before message rules
        // run so direct dialect callers also get path-tagged, payload-free errors.
        // Request-only patches may deliberately operate on a different shape.
        if (!config_->messages.empty()) {
            require(body.is_object() && body.contains("messages")
                    && body.at("messages").is_array(),
                    "request/messages must be an array");
        }
        for (const auto& rule : config_->messages) {
            auto& messages = body.at("messages");
            for (std::size_t i = 0; i < messages.size(); ++i) {
                auto& message = messages[i];
                const auto location = "request/messages/" + std::to_string(i);
                require(message.is_object(), location + " must be an object");
                require(!message.contains("role") || message.at("role").is_string(),
                        location + "/role must be a string");
                if (message.value("role", "") == rule.role) rule.patch.apply(message);
            }
        }
        config_->request.apply(body);
        for (const auto& option : config_->options) {
            if (!selected_.contains(option.name)) continue;
            for (const auto& choice : option.choices) {
                if (selected_[option.name] == choice.value) choice.patch.apply(body);
            }
        }
    }
    Json normalize_chunk(Json chunk) const override {
        config_->response.apply(chunk);
        return chunk;
    }
    std::optional<model_io::TokenCost> extract_cost(const Json& usage) const override {
        const auto prompt = count(usage, config_->prompt);
        const auto generated = count(usage, config_->generated);
        if (!prompt || !generated) return std::nullopt;
        return model_io::TokenCost{*prompt, *generated, count(usage, config_->cache).value_or(0)};
    }
};
} // namespace llm::custom_chat
