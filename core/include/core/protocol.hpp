#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "dataclass/model_io.hpp"

namespace core {
/** Reject unsafe session path components before any filesystem access. */
void validate_session_id(const std::string& id);
/** A validated payload. Only ordinary user text and explicit continuation exist. */
struct Input {
    std::string request_id;
    bool has_message = true;
    model_io::MessageItem message;
};
Input parse_input(const nlohmann::json& payload);
/** Generate a process-independent correlation identity; never reuse tool IDs. */
std::string new_identity();
} // namespace core
