#pragma once
#include <string>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "dataclass/model_io.hpp"

namespace core {
/** Reject unsafe session path components before any filesystem access. */
void validate_session_id(const std::string& id);
/** Payload option validation failure, distinguishable from other input errors. */
class InputOptionsError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

/** Serialized work admitted by the worker payload consumer. */
enum class InputOperation { Message, Continue, Compact };

/** Stable wire spelling for a validated operation. */
const char* operation_name(InputOperation operation);

/** A validated payload; compact's internal user message is supplied by core. */
struct Input {
    std::string request_id;
    InputOperation operation = InputOperation::Message;
    bool has_message = true;
    model_io::MessageItem message;
    /** Validated category objects; applied only when this payload is admitted. */
    nlohmann::json options = nlohmann::json::object();
};
/**
 * Parse a message's ordered content parts into user MessageItem::content.
 * Each part requires type (text/binary/external_ref) and nonempty raw.
 * Optional object metadata is preserved in Content::extras without injecting
 * category fields. Binary/reference bytes are retained verbatim: this boundary
 * neither decodes nor fetches them. Current Chat Completions adapters map
 * external_ref to image_url; future richer modalities can use extras metadata.
 *
 * Optional options must contain category objects. Model and confirmation values
 * are validated by their handlers; tools is a reserved empty object. Unknown
 * categories are rejected. Parsing never applies options or calls a provider.
 *
 * Reject invalid shapes and attempts to supply roles or tool-call metadata.
 * Continue and compact requests must not carry content or legacy text. Parsing never
 * mutates payload.
 */
Input parse_input(const nlohmann::json& payload);
/** Validated, read-only history page request. It never starts an agent run. */
struct HistoryRequest {
    std::string request_id;
    std::size_t start = 0;
    std::size_t step = 0;
    std::size_t limit = 10;
};
HistoryRequest parse_history_request(const nlohmann::json& payload);
/** Project a bounded display page from the authoritative in-memory turns. */
nlohmann::json history_page(const model_io::AgentInputState& state,
                            const HistoryRequest& request);
/** Generate a process-independent correlation identity; never reuse tool IDs. */
std::string new_identity();
} // namespace core
