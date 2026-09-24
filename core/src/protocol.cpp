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
        const auto text = payload.at("text").get<std::string>();
        if (text.empty()) throw std::invalid_argument("message text must not be empty");
        model_io::Content content;
        content.raw = text;
        input.message.content.push_back(std::move(content));
    }
    return input;
}
std::string new_identity() {
    return boost::uuids::to_string(boost::uuids::random_generator()());
}
} // namespace core
