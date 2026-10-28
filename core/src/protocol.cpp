#include "core/protocol.hpp"
#include "core/wire.hpp"
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <stdexcept>
#include <algorithm>
#include <limits>

namespace core {
namespace {

/** A wire cursor is a safe nonnegative integer; a page offset must fit too. */
std::size_t narrow(std::uint64_t value, const char* what) {
    if (value > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(std::string(what) + " is out of range");
    return static_cast<std::size_t>(value);
}

} // namespace

void validate_session_id(const std::string& id) {
    if (id.empty() || id.size() > 128 || id == "." || id == ".."
        || id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
        throw std::invalid_argument("session ID must contain 1..128 ASCII letters, digits, underscores or hyphens");
}
const char* operation_name(InputOperation operation) {
    switch (operation) {
        case InputOperation::Message: return "message";
        case InputOperation::Continue: return "continue";
        case InputOperation::Compact: return "compact";
    }
    throw std::invalid_argument("invalid input operation");
}

Input parse_input(const nlohmann::json& payload) {
    // The category shape is application policy, and its rejection class is part
    // of the admission contract; decide it before the structural codec runs.
    if (payload.is_object()) {
        if (const auto options = payload.find("options"); options != payload.end()
            && !options->is_object()) {
            throw InputOptionsError("options must be an object");
        }
    }
    protocol::PayloadRequest request;
    try {
        request = protocol::from_json<protocol::PayloadRequest>(payload);
    } catch (const protocol::ValidationError& error) {
        throw std::invalid_argument(error.what());
    }
    Input input;
    input.request_id = std::move(request.request_id);
    if (input.request_id.empty() || input.request_id.size() > 128)
        throw std::invalid_argument("request_id must contain 1..128 bytes");
    switch (request.operation) {
        case protocol::InputOperation::message:
            input.operation = InputOperation::Message;
            break;
        case protocol::InputOperation::continue_request:
            input.operation = InputOperation::Continue;
            break;
        case protocol::InputOperation::compact:
            input.operation = InputOperation::Compact;
            break;
        case protocol::InputOperation::history:
        case protocol::InputOperation::answer:
            // A query is answered by the query path and never admitted as input.
            throw std::invalid_argument("operation must be message, continue, or compact");
    }
    input.has_message = input.operation == InputOperation::Message;
    input.message.role = "user";
    if (input.has_message) {
        // The record guarantees the field for a message; the admission boundary
        // still refuses an empty batch and an empty part.
        if (!request.content || request.content->empty())
            throw std::invalid_argument("content must be a nonempty array");
        input.message.content.reserve(request.content->size());
        for (const auto& part : *request.content) {
            if (part.raw.empty()) throw std::invalid_argument("content raw must not be empty");
            input.message.content.push_back(model_content(part));
        }
    }
    if (request.options) {
        // Validate every category before any provider is allowed to mutate state.
        // Future category handlers belong at the same serialized admission point.
        for (const auto& [category, values] : request.options->items()) {
            if (!values.is_object()) {
                throw InputOptionsError("each options category must be an object");
            }
            if (category == "model" || category == "confirmation") {
                continue;
            }
            if (category != "tools" || !values.empty()) {
                throw InputOptionsError("unsupported options category: " + category);
            }
        }
        input.options = *request.options;
    }
    return input;
}

HistoryRequest parse_history_request(const nlohmann::json& payload) {
    protocol::PayloadRequest request;
    try {
        request = protocol::from_json<protocol::PayloadRequest>(payload);
    } catch (const protocol::ValidationError& error) {
        throw std::invalid_argument(error.what());
    }
    if (request.operation != protocol::InputOperation::history)
        throw std::invalid_argument("history request must be an object with operation history");
    HistoryRequest result;
    result.request_id = std::move(request.request_id);
    if (result.request_id.empty() || result.request_id.size() > 128)
        throw std::invalid_argument("request_id must contain 1..128 bytes");
    result.start = narrow(request.start.value_or(0), "history start");
    result.step = narrow(request.step.value_or(0), "history step");
    result.limit = narrow(request.limit.value_or(10), "history limit");
    if (result.limit == 0 || result.limit > 10)
        throw std::invalid_argument("history limit must be between 1 and 10");
    return result;
}

QueryRejection query_rejection(const nlohmann::json& request_id,
                               const std::string& operation, const std::string& code) {
    QueryRejection rejection;
    rejection.event = operation == "answer" ? protocol::EventName::answer_error
                                            : protocol::EventName::history_error;
    rejection.data.request_id = request_id;
    rejection.data.message =
        "Worker query quota exceeded; retry after the connection drains.";
    rejection.data.code = code;
    return rejection;
}

std::string new_identity() {
    return boost::uuids::to_string(boost::uuids::random_generator()());
}
} // namespace core
