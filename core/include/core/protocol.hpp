#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "dataclass/model_io.hpp"

namespace core {
/** Reject unsafe session path components before any filesystem access. */
void validate_session_id(const std::string& id);
/** A validated user-content payload or explicit continuation request. */
struct Input {
    std::string request_id;
    bool has_message = true;
    model_io::MessageItem message;
};
/**
 * Parse a message's ordered content parts into user MessageItem::content.
 * Each part requires type (text/binary/external_ref), raw, and label. The
 * nonempty label is stored in Content::extras["label"], overriding any nested
 * label while preserving other optional object metadata. Binary/reference
 * bytes are retained verbatim: this boundary neither decodes nor fetches them.
 * Provider adapters decide which content labels they can transmit.
 *
 * Reject invalid shapes and attempts to supply roles or tool-call metadata.
 * A continue request carries no new content. Parsing never mutates payload.
 */
Input parse_input(const nlohmann::json& payload);
/** Generate a process-independent correlation identity; never reuse tool IDs. */
std::string new_identity();
} // namespace core
