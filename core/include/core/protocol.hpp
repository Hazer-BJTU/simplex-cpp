#pragma once
#include <cstddef>
#include <cstdint>
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
 * Each part requires type (text/binary/external_ref) as its encoding and
 * modality (text/image/audio/video/document) as its media category, plus
 * nonempty raw. Both labels are explicit and required: the boundary neither
 * infers a category from the encoding nor accepts an unknown one, so an
 * attachment can never arrive as text by omission. Optional object metadata is
 * preserved in Content::extras without injecting category fields.
 * Binary/reference bytes are retained verbatim: this boundary neither decodes
 * nor fetches them. An adapter maps the modalities it supports and rejects the
 * rest; this boundary does not decide what a provider can carry.
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
/** Complete UTF-8 JSON history event budget, excluding WebSocket framing. */
inline constexpr std::size_t history_event_max_bytes = 256 * 1024;
/** Reserved for the worker envelope, including escaped correlation IDs. */
inline constexpr std::size_t history_envelope_max_bytes = 4 * 1024;
inline constexpr std::size_t history_page_max_bytes =
    history_event_max_bytes - history_envelope_max_bytes;
/**
 * Project a display page of at most history_page_max_bytes when dump() is used
 * with its default compact UTF-8 encoding. Count user content, response content,
 * reasoning, JSON escaping, separators and all page metadata, including revision.
 * The remaining envelope allowance keeps worker history events within
 * history_event_max_bytes without changing the start/step cursor contract.
 * The Hub also preserves a raw copy of the worker event; its forwarded panel
 * frame has a separate 512 KiB bound for this built-in projection.
 *
 * Admit complete turns or steps only. A page with remaining history always
 * advances its cursor; user content is repeated when a turn spans pages. The
 * existing four-part and per-part byte limits ensure a turn plus its first
 * remaining step fits an empty page, even with maximal JSON escaping. If future
 * projection changes invalidate that guarantee, throw length_error rather than
 * emit an oversized page, silently discard content or return a stalled cursor.
 * It does not copy the entire AgentInputState or mutate the source state.
 */
nlohmann::json history_page(const model_io::AgentInputState& state,
                            const HistoryRequest& request,
                            std::uint64_t revision = 0);
/** Generate a process-independent correlation identity; never reuse tool IDs. */
std::string new_identity();
} // namespace core
