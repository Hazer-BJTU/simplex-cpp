#pragma once

#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string_view>
#include <filesystem>
#include <cctype>
#include <algorithm>
#include <tuple>

#include <nlohmann/json.hpp>

#include "split.hpp"
#include "schema.hpp"

namespace indextools {

// =============================================================================
// LangAnalyze - Abstract Source Code Analysis Framework
// =============================================================================
//
// OVERVIEW
// --------
// This header defines `indextools::LangAnalyze`, an abstract base class that
// provides a unified, language-agnostic interface for parsing and analyzing
// source code. Concrete subclasses (e.g. for C++, Python, etc.) implement the
// language-specific parsing logic, while this base class handles the common
// infrastructure: source loading, line indexing, and pattern/identifier lookup.
//
// CORE TYPES
// ----------
// * EntityTag (nested abstract struct)
//     Represents a single semantic element extracted from source code, such as
//     a function, class, or variable. Each entity exposes:
//       - get_key()  : a unique string key identifying the entity.
//       - to_dict()  : a JSON serialization for inspection or export.
//       - clone()    : a deep copy, enabling safe ownership transfer.
//
// * Line representation (internal)
//     Lines are stored in a `SplitedString` (_lines), split by '\n'. Each
//     chunk is an (offset, length) tuple where offsets are absolute positions
//     within the source. SplitedString provides O(log N) offset-to-line lookups
//     via chunk_idx(), zero-copy line access via operator[], and pattern
//     search via locate_pattern() / locate_pattern_chunk().
//
// * EntityList
//     A vector of unique_ptr<EntityTag>, owning the entities produced by analysis.
//
// SOURCE LOADING & INDEXING
// -------------------------
// * load(std::string)            : load source from an in-memory string (move).
// * load(std::string_view)       : load source by copying a view's content.
// * open(std::filesystem::path)  : load source by reading a file from disk.
//
// After loading, _lines holds the source text (accessible via source()) and
// provides indexed line access via operator[] and SplitedString's iterator
// interface. The line index is built immediately, so all query methods are
// ready to use without a separate build step.
//
// ANALYSIS LIFECYCLE (subclass responsibilities)
// ----------------------------------------------
// * analyze()          : perform language-specific parsing on the loaded source.
// * reset()            : clear loaded source and analysis results.
// * result()           : return the list of entities produced by analyze().
// * locate_entity()    : look up an entity by key and return JSON metadata
//                        with its entire descendant subtree.
// * locate_identifier(): return JSON metadata and matching lines with surrounding
//                        context for a given identifier (built during analyze()).
// * get_full_structure(): return the full entity tree starting from the module
//                        root, serialized with all nested children.
//
// QUERY UTILITIES (provided by the base class)
// --------------------------------------------
// * source()                   : view of the entire loaded source.
// * locate_pattern()          : find all non-overlapping occurrences of a
//                                pattern and return JSON with metadata and
//                                matching lines alongside context.
// * get_dedented_lines()       : extract a range of lines with common leading
//                                whitespace removed.

/**
 * @brief Abstract base class for language-specific source code analyzers.
 *
 * LangAnalyze defines a unified interface for parsing and analyzing source
 * files of different programming languages. Concrete subclasses should
 * implement the language-specific parsing logic and produce a list of
 * entities (e.g. functions, classes, variables) extracted from the source.
 */
class LangAnalyze {
public:
    /**
     * @brief Abstract representation of a single entity extracted from source code.
     *
     * An EntityTag describes a meaningful element discovered during analysis,
     * such as a function declaration, a class definition, or a variable.
     * Subclasses should provide language- or entity-specific data and
     * serialization behavior.
     */
    struct EntityTag {
        virtual ~EntityTag() = 0;
        virtual std::string_view get_key() const noexcept = 0;
        // Serializes the entity into a dictionary form.
        virtual nlohmann::json to_dict() const noexcept = 0;
        // Creates a deep copy of this entity.
        virtual std::unique_ptr<EntityTag> clone() const noexcept = 0;
    };


    // Collection of entities discovered during analysis.
    using EntityList = std::vector<std::unique_ptr<EntityTag>>;

protected:
    std::filesystem::path _absolute_path;

    /// Line index: source split by '\n'. Each chunk is an (offset, length)
    /// tuple where offset is absolute within the owned source buffer.
    /// Delegates to indextools::SplitedString for all chunk-level operations
    /// and owns the source text (accessible via source()).
    SplitedString _lines;

    // Number of context lines to include before and after each match in locate_identifier().
    size_t _context_lines = 2;

public:
    virtual ~LangAnalyze() = 0;

    /**
     * @brief Load source code directly from an in-memory string (move).
     *
     * After loading, the source is immediately split into lines via
     * SplitedString with '\n' as the delimiter, making all query and
     * pattern-search methods ready for use.
     */
    LangAnalyze* load(std::string source) {
        _lines.load(std::move(source));
        return this;
    }

    /**
     * @brief Opens and loads a source file from the given absolute path.
     *
     * Delegates to SplitedString::read() which reads the file in binary mode
     * and splits it by '\n' with CRLF stripping enabled.
     *
     * @throws std::runtime_error if the file cannot be opened, its size
     *         cannot be determined, or reading fails.
     */
    LangAnalyze* open(const std::filesystem::path& absolute_path) {
        _absolute_path = absolute_path;
        _lines.read(absolute_path);
        return this;
    }

    // Performs analysis on the currently loaded source.
    virtual LangAnalyze* analyze() noexcept = 0;

    /**
     * @brief Resets the analyzer to its initial state, clearing any loaded
     *        source and previously generated analysis results.
     *
     * Resets _lines to a default-constructed (empty) SplitedString.
     * Subclasses should call this base implementation and then clear their
     * own analysis-specific state.
     */
    virtual LangAnalyze* reset() noexcept {
        _lines = SplitedString();
        _absolute_path.clear();
        return this;
    }

    // Sets the number of context lines to include before/after each match
    // in locate_identifier(). Returns this for method chaining.
    LangAnalyze* set_context_lines(size_t n) noexcept {
        _context_lines = n;
        return this;
    }

    // Returns the current context-lines setting.
    size_t get_context_lines() const noexcept {
        return _context_lines;
    }

    /**
     * @brief Returns a view of the entire source code currently held.
     *
     * The view remains valid for the lifetime of the LangAnalyze object
     * (until the next load/reset call).
     */
    std::string_view source() const noexcept {
        return _lines.source();
    }

    // Returns the list of entities produced by the most recent analysis.
    virtual const EntityList& result() const noexcept = 0;
    // This function queries the entity corresponding to the given key and
    // returns printable/displayable metadata and line-level content in JSON
    // format (using nlohmann json library).
    virtual nlohmann::json locate_entity(std::string_view entity_key) const noexcept = 0;
    // Locates occurrences of the given identifier and returns a displayable JSON
    // array with metadata and matching lines with surrounding context
    // (controlled by the _context_lines member, default 2).
    virtual nlohmann::json locate_identifier(std::string_view identifier) const noexcept = 0;

    /**
     * @brief Returns the full entity tree starting from the module root.
     *
     * Walks the entire entity hierarchy from the top-level module entity and
     * returns a JSON array containing the root entity with all descendants
     * recursively nested under the "sub_entity" key. Each entity in the tree
     * carries "meta", "text", and "sub_entity" sections.
     *
     * This is useful for IDE integration and debugging — it provides a complete
     * structural view of the analyzed source file in a single JSON document.
     *
     * @return A JSON array containing the module entity with its full descendant
     *         tree, or an empty array if no module entity exists (e.g. analysis
     *         has not been run or failed).
     */
    virtual nlohmann::json get_full_structure() const noexcept = 0;

    /**
     * @brief Locates all non-overlapping occurrences of @p pattern and returns
     *        a displayable JSON array with metadata and matching lines alongside
     *        surrounding context lines.
     *
     * Uses SplitedString::locate_pattern_chunk() to find matching line ranges,
     * then delegates to expand_interval() to expand each match with
     * _context_lines of surrounding context and merge overlapping or adjacent
     * intervals. The result is a JSON structure matching the format of
     * locate_identifier().
     *
     * When @p use_regex is true, @p pattern is interpreted as an ECMAScript
     * regular expression (delegated to SplitedString::locate_pattern_chunk()).
     * Regex matching is non-overlapping and zero-length matches are skipped,
     * matching the plain-substring semantics — see SplitedString::locate_pattern().
     *
     * Each element in the returned array has:
     *   - "meta": metadata section (file path, pattern, match line count, total line count)
     *   - "text": line-by-line content section (line_content, line_number, line_type)
     *     where line_type is "match" for lines directly containing the pattern
     *     and "base" for surrounding context lines.
     *
     * @param pattern The substring or (when use_regex) regex to search for.
     * @param use_regex If true, treat @p pattern as an ECMAScript regex.
     * @return A JSON array where each element has "meta" and "text" sections.
     *         Returns an empty array if @p pattern is empty, longer than the
     *         source, an invalid regex, or not found.
     */
    virtual nlohmann::json locate_pattern(std::string_view pattern, bool use_regex = false) const noexcept {
        size_t num_lines = _lines.size();
        if (num_lines == 0) {
            return nlohmann::json::array();
        }

        auto line_matches = _lines.locate_pattern_chunk(pattern, use_regex);
        if (line_matches.empty()) {
            return nlohmann::json::array();
        }

        size_t matched_cnt = 0;
        std::vector<bool> matched_type(num_lines, false);
        for (const auto& [line_start, line_end]: line_matches) {
            std::fill(matched_type.begin() + line_start, matched_type.begin() + line_end + 1, true);
            matched_cnt += line_end - line_start + 1;
        }

        auto expanded_matches = expand_interval(std::move(line_matches), num_lines, _context_lines);

        // Build the block via the shared schema builders so the display-block
        // shape stays identical to every other producer (see schema.hpp).
        nlohmann::json meta = schema::MetaBuilder()
            .field("File", _absolute_path.string())
            .field("Pattern", std::string(pattern))
            .field("Matches", matched_cnt)
            .field("Total Length", num_lines)
            .build();

        nlohmann::json result = nlohmann::json::array();
        result.push_back(schema::text_block(
            std::move(meta),
            schema::matched_line_text(_lines, expanded_matches, matched_type)));
        return result;
    }

    /**
     * @brief Get the multi-line paragraph with redundant leading whitespace
     *        removed.
     *
     * For a closed range of lines [line_start, line_end] (inclusive on both
     * ends), this extracts each line via _lines[i], computes the minimum
     * indentation among non-blank lines, and strips that amount of leading
     * whitespace from every non-blank line. Blank lines and whitespace-only
     * lines are preserved as-is (empty or original content).
     *
     * This is used by subclasses to extract compact entity content (e.g.
     * function signatures or class definitions) without surrounding
     * indentation noise.
     *
     * @param line_start  Start line index (0-based, inclusive).
     * @param line_end    End line index (0-based, inclusive). Clamped to
     *                    the actual number of source lines.
     * @return Vector of dedented line strings. Returns empty for an
     *         invalid range (line_start > line_end or line_start beyond
     *         the source).
     */
    std::vector<std::string> get_dedented_lines(size_t line_start, size_t line_end) const {
        std::vector<bool> empty_line;
        std::vector<std::string> result;
        if (_lines.source().empty()) {
            return result;
        }
        size_t num_lines = _lines.size();
        if (line_start > line_end || line_start >= num_lines) {
            return result;
        }
        size_t actual_end = std::min(line_end + 1, num_lines);

        // Determine the minimum leading whitespace among non-blank lines.
        size_t min_indent = std::string::npos;
        for (size_t i = line_start; i < actual_end; ++i) {
            std::string_view line = _lines[i];
            if (line.empty()) {
                empty_line.push_back(true);
                continue;
            }

            size_t first_non_ws = 0;
            // Cast to unsigned char: std::isspace has undefined behavior for
            // negative char values (non-ASCII bytes like UTF-8 sequences) on
            // platforms where char is signed (x86 Linux).
            for (; first_non_ws < line.size()
                 && std::isspace(static_cast<unsigned char>(line[first_non_ws]));
                 ++first_non_ws);
            // Skip lines that are entirely whitespace.
            if (first_non_ws == line.size()) {
                empty_line.push_back(true);
                continue;
            }

            empty_line.push_back(false);
            if (first_non_ws < min_indent) {
                min_indent = first_non_ws;
            }
        }
        if (min_indent == std::string::npos) {
            min_indent = 0;
        }

        result.reserve(actual_end - line_start);
        for (size_t i = line_start; i < actual_end; ++i) {
            std::string_view line = _lines[i];
            if (empty_line[i - line_start]) {
                result.emplace_back(line);
                continue;
            }

            // Strip min_indent leading characters.
            if (line.size() > min_indent) {
                result.emplace_back(line.substr(min_indent));
            } else {
                result.emplace_back(line);
            }
        }
        return result;
    }

protected:
    /**
     * @brief Build a displayable JSON array for a set of matched line indices.
     *
     * This is the shared implementation behind locate_identifier() across
     * language analyzers. Given a set of line numbers where an identifier
     * occurs, it expands each match with _context_lines of surrounding
     * context (merging overlapping/adjacent windows via expand_interval())
     * and produces a single JSON entry with the standard "meta" + "text"
     * layout — identical in shape to locate_pattern()'s output.
     *
     * The "meta" section is labeled with @p field_name / @p field_value
     * (e.g. "Identifier" / "foo") so callers can reuse this for any
     * identifier-style lookup. @p matched_lines are 0-based line indices
     * and must be < _lines.size(); out-of-range entries are skipped.
     *
     * @param field_name    Label for the match column in "meta" (e.g. "Identifier").
     * @param field_value   The matched value (e.g. the lowercased identifier).
     * @param matched_lines Set of 0-based line indices where the match occurs.
     * @return A JSON array with one entry (meta + text), or an empty array
     *         if no source is loaded or @p matched_lines is empty.
     */
    nlohmann::json _build_identifier_lookup_json(
        std::string_view field_name,
        std::string_view field_value,
        const std::unordered_set<size_t>& matched_lines
    ) const noexcept {
        size_t num_lines = _lines.size();
        if (num_lines == 0 || matched_lines.empty()) {
            return nlohmann::json::array();
        }

        size_t matched_cnt = 0;
        std::vector<bool> matched_type(num_lines, false);
        std::vector<std::tuple<size_t, size_t>> line_matches;
        line_matches.reserve(matched_lines.size());
        for (const auto matched_idx: matched_lines) {
            // Guard against out-of-range indices from a corrupt/stale map.
            if (matched_idx >= num_lines) {
                continue;
            }
            line_matches.emplace_back(matched_idx, matched_idx);
        }
        if (line_matches.empty()) {
            return nlohmann::json::array();
        }
        for (const auto& [line_start, line_end]: line_matches) {
            std::fill(matched_type.begin() + line_start, matched_type.begin() + line_end + 1, true);
            matched_cnt += line_end - line_start + 1;
        }

        auto expanded_matches = expand_interval(std::move(line_matches), num_lines, _context_lines);

        // Build the block via the shared schema builders (see schema.hpp), so
        // this stays byte-identical to locate_pattern() modulo the meta label.
        nlohmann::json meta = schema::MetaBuilder()
            .field("File", _absolute_path.string())
            .field(field_name, std::string(field_value))
            .field("Matches", matched_cnt)
            .field("Total Length", num_lines)
            .build();

        nlohmann::json result = nlohmann::json::array();
        result.push_back(schema::text_block(
            std::move(meta),
            schema::matched_line_text(_lines, expanded_matches, matched_type)));
        return result;
    }
};

inline LangAnalyze::~LangAnalyze() {}

inline LangAnalyze::EntityTag::~EntityTag() {}

}
