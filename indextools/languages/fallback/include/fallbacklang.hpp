#pragma once

/**
 * @file fallbacklang.hpp
 * @brief Fallback, language-agnostic source analyzer for the indextools
 *        entity extraction framework.
 *
 * FallbackLanguage is the catch-all analyzer used for every file extension
 * that does not yet have a dedicated, tree-sitter-backed implementation
 * (the extension list lives in languages/src/fallbacklang_plugin.cpp). It
 * performs no structural parsing —
 * locate_entity() always returns an empty result and no entities are
 * produced — but it *does* build a lightweight identifier index so that
 * locate_identifier() works out of the box.
 *
 * ## Tokenization model
 *
 * The index is built by scanning each line and splitting it into tokens on
 * every character that is NOT an identifier character. An identifier
 * character is, by design, a deliberately inclusive set covering the
 * identifier rules of the vast majority of programming languages:
 *
 *   - ASCII letters and digits (std::isalnum)
 *   - underscore  '_'
 *   - dollar sign '$'   (JavaScript, Java, PHP, ...)
 *
 * Everything else — whitespace, punctuation, brackets, operators, and all
 * non-ASCII (UTF-8 lead/continuation) bytes — acts as a separator. Tokens
 * are lowercased for case-insensitive lookup and mapped to the set of line
 * numbers on which they appear.
 *
 * This is intentionally conservative: it will not perfectly model languages
 * with unusual identifier characters (e.g. Lisp kebab-case, Ruby `?`/`!`
 * suffixes, or Unicode identifiers), but it correctly handles ordinary
 * identifiers across C, C++, Java, JavaScript/TypeScript, Go, Rust, Python,
 * shell, and most config/data formats. When a dedicated analyzer is added
 * for a language, its extension is moved out of the Fallback plugin's
 * extension list (fallbacklang_plugin.cpp) and this class simply stops being
 * selected for it.
 */

#include "lang.hpp"

#include <unordered_map>
#include <unordered_set>

namespace indextools {

/**
 * @brief Concrete LangAnalyze used as the default for unsupported extensions.
 *
 * Produces no entities (locate_entity / get_full_structure / result are all
 * empty). The only meaningful work happens in analyze(), which builds the
 * identifier line index, and locate_identifier(), which queries it.
 */
class FallbackLanguage: public LangAnalyze {
public:
    /**
     * @brief Maps a lowercased token to the set of 0-based line numbers on
     *        which it appears.
     *
     * Built by analyze() and consumed by locate_identifier(). Mirrors the
     * LineIndex typedef used by PythonLanguage so downstream tooling can
     * treat them uniformly.
     */
    using LineIndex = std::unordered_map<std::string, std::unordered_set<size_t>>;

private:
    /// Flat entity list — always empty for the fallback analyzer. Kept as a
    /// member so result() can return a stable reference.
    EntityList _result;

    /// Identifier → line-numbers index, populated by analyze().
    LineIndex _identifier_line_map;

public:
    FallbackLanguage() = default;
    ~FallbackLanguage() override = default;
    FallbackLanguage(const FallbackLanguage&) = default;
    FallbackLanguage(FallbackLanguage&&) noexcept = default;
    FallbackLanguage& operator = (const FallbackLanguage&) = default;
    FallbackLanguage& operator = (FallbackLanguage&&) noexcept = default;

    // --- LangAnalyze interface ------------------------------------------------

    /// Build the identifier line index by tokenizing every loaded line.
    FallbackLanguage* analyze() noexcept override;

    /// Clear the loaded source and the identifier index.
    FallbackLanguage* reset() noexcept override;

    /// Always empty — the fallback analyzer extracts no entities.
    const EntityList& result() const noexcept override;

    /// Always an empty JSON array — there are no entities to locate.
    nlohmann::json locate_entity(std::string_view entity_key) const noexcept override;

    /// Look up a token in the index built by analyze() and return the
    /// standard meta/text JSON with surrounding context lines.
    nlohmann::json locate_identifier(std::string_view identifier) const noexcept override;

    /// Always an empty JSON array — there is no entity tree.
    nlohmann::json get_full_structure() const noexcept override;

    /// Expose the full token → lines map (debugging / IDE integration).
    const LineIndex& get_identifier_line_map() const noexcept;

private:
    /**
     * @brief Classify a byte as an identifier character.
     *
     * True for ASCII letters/digits, '_', and '$'. The argument is taken as
     * unsigned char so std::isalnum is well-defined for all byte values.
     */
    static bool _is_identifier_char(unsigned char c) noexcept;

    /**
     * @brief Tokenize a single line and record each token's line number.
     *
     * Splits @p line into maximal runs of identifier characters, lowercases
     * each run, and inserts @p line_idx into _identifier_line_map[token].
     */
    void _tokenize_line(std::string_view line, size_t line_idx) noexcept;
};

}
