#pragma once

//
// responses/event_access.hpp — guarded wire-event field access
// =============================================================
//
// The one shared vocabulary both halves of the Responses-API layer use to
// read fields off a wire event's JSON: the decoder (stream_handler.cpp,
// event -> delta) and the reader (reader.cpp, delta/markers ->
// accumulators). Guarded means never throws — a surprising server event
// surfaces as a delta, never as an exception, so put()'s catch(...) stays
// reserved for genuine framing faults on both sides.
//
// summary_part_index lives here precisely because it is easy to get wrong
// twice: the reasoning-summary part an event belongs to is spelled
// `content_index` on the text channel and `summary_index` on the part
// lifecycle events, and the resolution order must be IDENTICAL wherever it
// runs — the decoder resolves it into the delta, the reader resolves it
// when folding a marker's `.done` overwrite. Divergent private copies here
// would send a part's increments and its authoritative done text to
// different accumulators.
//
// detail namespace: implementation-shared, not part of the layer's public
// surface.
//

#include <cstddef>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace endpoint::responses::detail {

/// The nested object under @p key, or null when absent/not an object.
inline const nlohmann::json* find_object(
    const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return nullptr;
    return &*it;
}

/// The string under @p key, or "" when absent/not a string.
inline std::string get_string(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return {};
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

/// The unsigned index under @p key, or nullopt when absent/not a number.
/// (Optional, not zero-default: index 0 is a legitimate value — the first
/// output item, the first content part.)
inline std::optional<std::size_t> get_index(
    const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return std::nullopt;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_unsigned()) return std::nullopt;
    return it->get<std::size_t>();
}

/// The reasoning-summary part an event belongs to: `content_index` (the
/// text channel's documented field — also what the decoder resolves into
/// the delta), else `summary_index` (the part lifecycle events' spelling),
/// else 0 (single-part degenerate case when a server omits both).
inline std::size_t summary_part_index(const nlohmann::json& event) {
    if (auto index = get_index(event, "content_index")) return *index;
    if (auto index = get_index(event, "summary_index")) return *index;
    return 0;
}

} // namespace endpoint::responses::detail
