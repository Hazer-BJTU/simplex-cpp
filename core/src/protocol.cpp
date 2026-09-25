#include "core/protocol.hpp"
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <stdexcept>

namespace core {
void validate_session_id(const std::string& id) {
    if (id.empty() || id.size() > 128 || id == "." || id == ".."
        || id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
        throw std::invalid_argument("session ID must contain 1..128 ASCII letters, digits, underscores or hyphens");
}
Input parse_input(const nlohmann::json& payload) {
    if (!payload.is_object()) throw std::invalid_argument("payload must be an object");
    Input input;
    input.request_id = payload.at("request_id").get<std::string>();
    if (input.request_id.empty() || input.request_id.size() > 128)
        throw std::invalid_argument("request_id must contain 1..128 bytes");
    const auto operation = payload.at("operation").get<std::string>();
    if (operation != "message" && operation != "continue")
        throw std::invalid_argument("operation must be message or continue");
    for (const char* field : {"role", "invokes", "invoke_return", "type"})
        if (payload.contains(field)) throw std::invalid_argument("payload cannot contain message metadata");
    input.has_message = operation == "message";
    input.message.role = "user";
    if (input.has_message) {
        if (payload.contains("text")) {
            throw std::invalid_argument("message requires content instead of text");
        }
        const auto& parts = payload.at("content");
        if (!parts.is_array() || parts.empty()) {
            throw std::invalid_argument("content must be a nonempty array");
        }
        input.message.content.reserve(parts.size());
        for (const auto& part : parts) {
            if (!part.is_object()) {
                throw std::invalid_argument("each content part must be an object");
            }
            model_io::Content content;
            const auto type = part.at("type").get<std::string>();
            // Content's general JSON decoder tolerates unknown enum labels.
            // The public input boundary must reject them rather than silently
            // turn an attachment into text.
            if (type == "text") {
                content.type = model_io::ContentType::Text;
            } else if (type == "binary") {
                content.type = model_io::ContentType::Binary;
            } else if (type == "external_ref") {
                content.type = model_io::ContentType::ExternalRef;
            } else {
                throw std::invalid_argument("unknown content type");
            }
            content.raw = part.at("raw").get<std::string>();
            if (content.raw.empty()) {
                throw std::invalid_argument("content raw must not be empty");
            }
            const auto extras = part.find("extras");
            if (extras != part.end() && !extras->is_object()) {
                throw std::invalid_argument("content extras must be an object");
            }
            if (extras != part.end()) {
                content.extras = *extras;
            }
            input.message.content.push_back(std::move(content));
        }
    } else if (payload.contains("content") || payload.contains("text")) {
        throw std::invalid_argument("continue cannot contain content or text");
    }
    if (const auto options = payload.find("options"); options != payload.end()) {
        if (!options->is_object()) {
            throw std::invalid_argument("options must be an object");
        }
        // Validate every category before any provider is allowed to mutate state.
        // Future category handlers belong at the same serialized admission point.
        for (const auto& [category, values] : options->items()) {
            if (!values.is_object()) {
                throw std::invalid_argument("each options category must be an object");
            }
            if (category == "model" || category == "confirmation") {
                continue;
            }
            if (category != "tools" || !values.empty()) {
                throw std::invalid_argument("unsupported options category: " + category);
            }
        }
        input.options = *options;
    }
    return input;
}
std::string new_identity() {
    return boost::uuids::to_string(boost::uuids::random_generator()());
}
} // namespace core
