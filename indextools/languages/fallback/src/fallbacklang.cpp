/**
 * @file fallbacklang.cpp
 * @brief Implementation of FallbackLanguage — the generic, tokenizer-only
 *        analyzer used for file extensions without a dedicated implementation.
 *
 * Unlike PythonLanguage (which drives tree-sitter to build a real entity
 * tree), FallbackLanguage does no structural parsing. It only builds a
 * token → line-numbers index so that locate_identifier() remains useful for
 * code navigation in files we do not yet understand structurally.
 *
 * The tokenization rule is documented in fallbacklang.hpp: a token is a
 * maximal run of identifier characters (ASCII alnum, '_', '$'); everything
 * else is a separator.
 */

#include "fallbacklang.hpp"

#include <algorithm>
#include <cctype>

namespace indextools {

// ============================================================================
// analyze() — build the identifier line index
// ============================================================================

FallbackLanguage* FallbackLanguage::analyze() noexcept {
    // Clear any previous analysis state before reindexing.
    _identifier_line_map.clear();

    size_t num_lines = _lines.size();
    for (size_t line_idx = 0; line_idx < num_lines; ++line_idx) {
        _tokenize_line(_lines[line_idx], line_idx);
    }
    return this;
}

// ============================================================================
// reset() — clear all state for reuse
// ============================================================================

FallbackLanguage* FallbackLanguage::reset() noexcept {
    LangAnalyze::reset();
    _result.clear();
    _identifier_line_map.clear();
    return this;
}

// ============================================================================
// result() — entities (always empty for the fallback)
// ============================================================================

const FallbackLanguage::EntityList& FallbackLanguage::result() const noexcept {
    return _result;
}

// ============================================================================
// locate_entity — no structural entities, always empty
// ============================================================================

nlohmann::json FallbackLanguage::locate_entity(std::string_view /*entity_key*/) const noexcept {
    return nlohmann::json::array();
}

// ============================================================================
// locate_identifier — query the token index built during analyze()
// ============================================================================

nlohmann::json FallbackLanguage::locate_identifier(std::string_view identifier) const noexcept {
    if (_lines.size() == 0) {
        return nlohmann::json::array();
    }

    // Lowercase the query so it matches the lowercased keys stored by
    // _tokenize_line. This gives case-insensitive lookups, consistent with
    // PythonLanguage::locate_identifier().
    std::string lowered(identifier);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    auto it = _identifier_line_map.find(lowered);
    if (it == _identifier_line_map.end()) {
        // Not found — return an empty array so callers can iterate without
        // special-casing the "no match" path.
        return nlohmann::json::array();
    }

    return _build_identifier_lookup_json("Identifier", lowered, it->second);
}

// ============================================================================
// get_full_structure — no entity tree, always empty
// ============================================================================

nlohmann::json FallbackLanguage::get_full_structure() const noexcept {
    return nlohmann::json::array();
}

const FallbackLanguage::LineIndex& FallbackLanguage::get_identifier_line_map() const noexcept {
    return _identifier_line_map;
}

// ============================================================================
// _is_identifier_char — identifier-character predicate
// ============================================================================

bool FallbackLanguage::_is_identifier_char(unsigned char c) noexcept {
    // std::isalnum is UB for negative char values; the unsigned char
    // parameter makes every byte well-defined. In the C/POSIX locale this
    // is true exactly for [A-Za-z0-9]. Non-ASCII bytes (>= 128, e.g. UTF-8
    // lead/continuation bytes) fall through to "separator" — see the file
    // header comment for the rationale.
    return std::isalnum(c) || c == '_' || c == '$';
}

// ============================================================================
// _tokenize_line — split a line into identifier tokens, record line numbers
// ============================================================================

void FallbackLanguage::_tokenize_line(std::string_view line, size_t line_idx) noexcept {
    const size_t n = line.size();
    size_t i = 0;
    while (i < n) {
        // Skip separators: whitespace, punctuation, brackets, operators,
        // and any other non-identifier byte.
        while (i < n && !_is_identifier_char(static_cast<unsigned char>(line[i]))) {
            ++i;
        }

        size_t start = i;
        while (i < n && _is_identifier_char(static_cast<unsigned char>(line[i]))) {
            ++i;
        }

        if (i > start) {
            // Lowercase the token for case-insensitive matching. We copy
            // out of the string_view because transform needs a mutable
            // target and the view points into the owned (const-access)
            // source buffer.
            std::string token(line.substr(start, i - start));
            std::transform(token.begin(), token.end(), token.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            _identifier_line_map[std::move(token)].insert(line_idx);
        }
    }
}

}
