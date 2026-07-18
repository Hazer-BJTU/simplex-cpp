/**
 * @file split.hpp
 * @brief Aho-Corasick automaton and source-splitting utilities for the
 *        indextools entity extraction framework.
 *
 * This header provides the core string-processing infrastructure used
 * throughout the cpptools project:
 *
 *   - Ahocorasick:    a compact Aho-Corasick automaton for multi-pattern
 *                     matching. Supports incremental pattern insertion,
 *                     explicit build of failure transitions, and O(N+M)
 *                     scanning (N = source length, M = match count).
 *
 *   - SplitedString:  a read-only, indexed view of a source text that has
 *                     been split into chunks by a set of delimiter patterns.
 *                     Each chunk records the byte offset, content length,
 *                     and the length of its trailing delimiter — keeping
 *                     content and delimiters separate. Provides random
 *                     access (operator[]), delimiter access, pattern search,
 *                     offset-to-chunk lookups (O(log K)), and a full
 *                     random-access iterator interface.
 *
 * The design is zero-copy wherever possible: SplitedString owns a single
 * std::string buffer and all chunk views are std::string_view slices into
 * that buffer. There are no dangling references because the owning string
 * lives inside the SplitedString object itself.
 */

#pragma once

#include <cstdlib>
#include <string>
#include <string_view>
#include <sstream>
#include <vector>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <tuple>
#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>
#include <queue>
#include <array>
#include <list>
#include <regex>

namespace indextools {

// =============================================================================
// Ahocorasick — Aho-Corasick automaton for multi-pattern matching
// =============================================================================
//
// This is a compact, header-only implementation of the classic Aho-Corasick
// string-matching automaton. It supports:
//
//   - Incremental pattern insertion via add_pattern().
//   - Explicit failure-transition construction via build().
//   - Single-pass scanning via match(), which returns all matches as
//     (start_offset, end_offset_inclusive) tuples.
//
// The alphabet is fixed at 256 values (raw bytes). Each node stores a flat
// array of `ALPHABET_SIZE` child pointers plus a fail pointer and a set of
// matched pattern lengths. This trades memory for speed: the flat array
// makes goto-transition O(1) per byte.
//
// Usage pattern:
//
//   Ahocorasick ac;
//   ac.add_pattern("\n");
//   ac.add_pattern("\r\n");
//   ac.build();
//   auto matches = ac.match(source);   // vector<tuple<size_t, size_t>>
//
// build() must be called after all patterns are added and before any match()
// call. Adding patterns after build() throws std::runtime_error.
//
// match() returns matches in scan order. The end offset is INCLUSIVE (i.e.
// the index of the last byte that belongs to the match), consistent with
// how tree-sitter reports byte ranges.

class Ahocorasick {
public:
    /// Number of distinct byte values (full 8-bit range).
    static constexpr size_t ALPHABET_SIZE = 256u;

    /**
     * @brief A single state (node) in the automaton.
     *
     * - _fail:         failure-transition index (std::string::npos if unset).
     * - _next:         goto table indexed by byte value.
     *                  std::string::npos means "no transition defined".
     * - _matched_length: lengths of patterns that end at this node.
     *                    After build(), also includes lengths inherited
     *                    from the failure chain (so match() can report them
     *                    without walking the chain at scan time).
     */
    struct Node {
        size_t _fail;
        std::array<size_t, ALPHABET_SIZE> _next;
        std::unordered_set<size_t> _matched_length;
    };

private:
    bool _built;
    std::vector<Node> _node;

public:
    /// Construct an automaton with a single root node (state 0).
    /// All goto transitions from the root are initialised to npos.
    Ahocorasick(): _built(false), _node() {
        Node root{0, {}, {}};
        root._next.fill(std::string::npos);
        _node.push_back(std::move(root));
    }
    ~Ahocorasick() = default;
    Ahocorasick(const Ahocorasick&) = default;
    Ahocorasick& operator = (const Ahocorasick&) = default;
    Ahocorasick(Ahocorasick&&) noexcept = default;
    Ahocorasick& operator = (Ahocorasick&&) noexcept = default;

    /**
     * @brief Insert a pattern into the automaton.
     *
     * Traverses the trie from the root, creating new nodes as needed.
     * Records the pattern length in the terminal node's _matched_length set.
     *
     * An empty pattern is silently ignored.
     *
     * @throws std::runtime_error if called after build().
     */
    void add_pattern(std::string_view pattern) {
        if (_built) {
            throw std::runtime_error("unable to add patterns after fail transitions are built");
        }

        if (pattern == "") {
            return;
        }

        size_t curr = 0;
        for (size_t i = 0; i < pattern.size(); ++i) {
            auto c = static_cast<unsigned char>(pattern[i]);
            if (_node[curr]._next[c] == std::string::npos) {
                _node[curr]._next[c] = _node.size();
                Node new_node{std::string::npos, {}, {}};
                new_node._next.fill(std::string::npos);
                _node.push_back(std::move(new_node));
            }
            curr = _node[curr]._next[c];
        }
        _node[curr]._matched_length.insert(pattern.size());
        return;
    }

    /**
     * @brief Build failure transitions (BFS over the trie).
     *
     * This is the standard Aho-Corasick construction:
     *
     *   1. Root's direct children point back to root on failure.
     *   2. For each node, missing goto transitions are redirected to the
     *      failure node's goto (dictionary-suffix links).
     *   3. Each node inherits the _matched_length set of its failure node,
     *      so match() can report suffix matches without extra traversal.
     *
     * Idempotent: calling build() multiple times is a no-op.
     */
    void build() noexcept {
        if (_node.empty() || _built) {
            return;
        }

        std::queue<size_t> node_queue;
        for (size_t i = 0; i < ALPHABET_SIZE; ++i) {
            size_t child = _node[0]._next[i];
            if (child == std::string::npos) {
                _node[0]._next[i] = 0;
            } else {
                _node[child]._fail = 0;
                node_queue.push(child);
            }
        }

        while(!node_queue.empty()) {
            size_t curr = node_queue.front();
            node_queue.pop();

            _node[curr]._matched_length.insert(
                _node[_node[curr]._fail]._matched_length.cbegin(),
                _node[_node[curr]._fail]._matched_length.cend()
            );

            for (size_t i = 0; i < ALPHABET_SIZE; ++i) {
                size_t child = _node[curr]._next[i];
                if (child == std::string::npos) {
                    _node[curr]._next[i] = _node[_node[curr]._fail]._next[i];
                } else {
                    _node[child]._fail = _node[_node[curr]._fail]._next[i];
                    node_queue.push(child);
                }
            }
        }

        _built = true;
        return;
    }

    /**
     * @brief Scan @p source and return all pattern matches.
     *
     * Runs the automaton over the source text byte-by-byte. At each position
     * where one or more patterns end, emits a (start, end) tuple where `end`
     * is the INCLUSIVE byte offset of the last matching character.
     *
     * Matches are returned in the order they are discovered (ascending
     * end offset).
     *
     * @param source  The text to scan.
     * @return        Vector of (start_offset, end_offset_inclusive) tuples.
     * @throws std::runtime_error if build() has not been called.
     */
    std::vector<std::tuple<size_t, size_t>> match(std::string_view source) const {
        if (!_built) {
            throw std::runtime_error("fail transitions are not built: call build() first");
        }

        size_t curr = 0;
        std::vector<std::tuple<size_t, size_t>> result;
        for (size_t i = 0; i < source.size(); ++i) {
            auto c = static_cast<unsigned char>(source[i]);
            curr = _node[curr]._next[c];
            for (auto len: _node[curr]._matched_length) {
                result.emplace_back(i + 1 - len, i);
            }
        }
        return result;
    }
};

// =============================================================================
// expand_interval — expand each interval by a fixed radius and merge overlaps
// =============================================================================
//
// Given a sorted list of (start, end) intervals (end inclusive), this function
// expands each interval by `expand_length` positions on both sides (clamped to
// [start_index, total_nums), sorts, and merges overlapping or directly
// adjacent intervals.
//
// This is used by pattern/identifier lookup to add surrounding context lines:
// each matched line is expanded by `_context_lines` lines before and after,
// then overlapping context windows are merged so adjacent/nearby matches
// produce a single contiguous block.
//
// Example:
//   intervals: [(2,2), (5,5)], total_nums=10, expand_length=1, start_index=0
//   expanded:  [(1,3)] after first, [(1,3),(4,6)] after second
//   merged:    [(1,6)]  (intervals [1,3] and [4,6] are adjacent → merged)
//
// @param original_intervals  Sorted list of (start, end) inclusive intervals.
// @param total_nums          Total number of elements (e.g. lines in source).
// @param expand_length       Number of positions to expand on each side.
// @param start_index         Lower bound for clamping (default 0).
// @return                    Merged, expanded intervals.

inline std::vector<std::tuple<size_t, size_t>> expand_interval(
    std::vector<std::tuple<size_t, size_t>> original_intervals,
    size_t total_nums,
    size_t expand_length,
    size_t start_index = 0u
) noexcept {
    if (total_nums == 0) {
        return {};
    }

    sort(original_intervals.begin(), original_intervals.end());

    std::vector<std::tuple<size_t, size_t>> expanded_intervals;
    for (const auto& [index_start, index_end /* inclusive */ ]: original_intervals) {
        if (index_end < index_start) {
            continue;
        }

        size_t clipped_index_start = index_start >= start_index + expand_length ? index_start - expand_length : start_index;
        size_t clipped_index_end = index_end + expand_length < total_nums ? index_end + expand_length : total_nums - 1;

        if (expanded_intervals.empty()) {
            expanded_intervals.emplace_back(clipped_index_start, clipped_index_end);
        } else {
            auto& [last_index_start, last_index_end] = expanded_intervals.back();
            if (clipped_index_start <= last_index_end + 1) {
                last_index_end = std::max(last_index_end, clipped_index_end);
            } else {
                expanded_intervals.emplace_back(clipped_index_start, clipped_index_end);
            }
        }
    }
    return expanded_intervals;
}

// =============================================================================
// non_overlapping — filter intervals to keep only non-overlapping ones
// =============================================================================
//
// Given a sorted list of (start, end) inclusive intervals, returns a subset
// where no two intervals overlap. The selection strategy is controlled by
// `prefer_longer`:
//
//   - prefer_longer == false (default): first-come-first-served.
//     Each interval is kept if its start is strictly after the end of the
//     last kept interval. This is the "earliest finish time" greedy.
//     Use this when you want the earliest match to win.
//
//   - prefer_longer == true: when two intervals share the same start
//     position, the longer one (larger end) replaces the shorter one.
//     This is essential when one delimiter pattern is a prefix of another
//     (e.g. "\n\n" vs "\n", or "\r\n" vs "\n") — without it, the shorter
//     pattern would be matched first, and the longer pattern would be
//     discarded as overlapping, causing a double-straddle delimiter like
//     "\n\n" to be incorrectly split into two separate "\n" matches
//     (producing a spurious empty chunk between them).
//
// This is used internally by SplitedString::_build_chunks() with
// prefer_longer=true to ensure that when multiple delimiter patterns
// match at the same starting offset, the longest match is kept. This
// guarantees that "\r\n" is treated as one delimiter (not "\r" content
// plus a "\n" delimiter), and that "\n\n" paragraph breaks are not
// misidentified as two consecutive line breaks.

inline std::vector<std::tuple<size_t, size_t>> non_overlapping(
    std::vector<std::tuple<size_t, size_t>> original_intervals,
    bool prefer_longer = false
) noexcept {
    if (original_intervals.empty()) {
        return {};
    }

    sort(original_intervals.begin(), original_intervals.end());

    std::vector<std::tuple<size_t, size_t>> result;
    for (const auto& [index_start, index_end]: original_intervals) {
        if (result.empty()) {
            result.emplace_back(index_start, index_end);
            continue;
        }

        auto& [last_index_start, last_index_end] = result.back();
        if (prefer_longer && index_start == last_index_start) {
            last_index_end = index_end;
            continue;
        }

        if (index_start > last_index_end) {
            result.emplace_back(index_start, index_end);
        }
    }
    return result;
}

// =============================================================================
// SplitedString — read-only, indexed view of delimiter-split source text
// =============================================================================
//
// SplitedString owns a source buffer and maintains a chunk index produced
// by splitting the source on a user-specified set of delimiter patterns.
// Each chunk records:
//
//   - index_start:      byte offset where the chunk's content begins.
//   - content_length:   number of bytes in the chunk content (NOT including
//                       the delimiter).
//   - delimiter_length: number of bytes in the delimiter that follows this
//                       chunk, or 0 for the final chunk.
//
// This design keeps content and delimiters separate: operator[] returns a
// string_view of just the content, and get_delimiter() returns a view of
// the delimiter. Callers that need to reconstruct the original source can
// interleave content and delimiters themselves.
//
// Key properties:
//
//   - Zero-copy access:  operator[] and source() return std::string_view
//                        into the internal buffer. These views remain valid
//                        until the next load()/read()/move/destruction.
//
//   - O(log K) lookup:   get_chunk_index() uses binary search over the
//                        chunk array (K = number of chunks).
//
//   - Multi-delimiter:   The constructor accepts a set of delimiter patterns
//                        (default: {"\n", "\r\n"}). Splitting uses an
//                        Aho-Corasick automaton internally. When multiple
//                        patterns match at the same starting offset, the
//                        longest match wins (via non_overlapping with
//                        prefer_longer=true). This ensures that "\r\n" is
//                        never split into "\r" (as content) + "\n" (as
//                        delimiter), and that multi-character delimiters
//                        like "\n\n" (paragraph breaks) are correctly
//                        identified even when "\n" (line breaks) is also
//                        in the delimiter set.
//
//   - Read-only design:  There is no append() or incremental modification.
//                        To process new content, call load() or read() again
//                        which replaces the entire source and chunk index.
//
//   - Full iterator support: Random-access iterators (begin/end, rbegin/rend)
//                        enable range-for loops and STL algorithm integration.
//
// Usage:
//
//   // From string:
//   SplitedString lines;
//   lines.load("first line\nsecond line\nthird line");
//   for (auto line : lines) { ... }
//
//   // From file:
//   SplitedString lines;
//   lines.read("/path/to/file.py");
//   std::cout << lines[0];           // first line content
//   std::cout << lines.get_delimiter(0); // delimiter after first line
//
//   // Custom delimiters:
//   SplitedString paragraphs({"\n\n", "\r\n\r\n"});
//   paragraphs.load(source);

class SplitedString {
public:
    /**
     * @brief Descriptor for one chunk (a content segment + its trailing delimiter).
     *
     * All offsets and lengths are in bytes, relative to the start of the
     * owned source buffer.
     */
    struct ChunkIndex {
        size_t index_start;       ///< Byte offset where chunk content begins.
        size_t content_length;    ///< Length of the content (without delimiter).
        size_t delimiter_length;  ///< Length of the trailing delimiter, 0 for last chunk.
    };

private:
    /// The complete source text. All string_view results point into this buffer.
    std::string _bytes;

    /// Aho-Corasick automaton built from the configured delimiter patterns.
    Ahocorasick _automaton;

    /// Chunk index: one entry per content segment.
    std::vector<ChunkIndex> _chunks;

public:
    /**
     * @brief Construct a SplitedString with the given delimiter patterns.
     *
     * The default delimiters {"\n", "\r\n"} handle both Unix and Windows
     * line endings, splitting the source into lines.
     *
     * The automaton is built immediately (patterns are fixed after construction),
     * so load()/read() can be called without a separate build step.
     *
     * @param delimiters  Set of delimiter pattern strings to split on.
     */
    SplitedString(const std::unordered_set<std::string>& delimiters = {"\n", "\r\n"}): _bytes(), _automaton(), _chunks() {
        for (const auto& delimiter: delimiters) {
            _automaton.add_pattern(delimiter);
        }
        _automaton.build();
    }
    ~SplitedString() = default;
    SplitedString(const SplitedString&) = default;
    SplitedString& operator = (const SplitedString&) = default;
    SplitedString(SplitedString&&) noexcept = default;
    SplitedString& operator = (SplitedString&&) noexcept = default;

private:
    /**
     * @brief Rebuild the chunk index from the current _bytes content.
     *
     * Runs the automaton over _bytes, extracts non-overlapping delimiter matches
     * with prefer_longer=true (so longer patterns like "\r\n" or "\n\n" win over
     * "\n" when both match at the same starting offset), and builds the chunk
     * list. Each chunk captures the content between consecutive delimiters plus
     * the delimiter that follows it.
     *
     * This is called automatically by load() and read().
     */
    void _build_chunks() noexcept {
        _chunks.clear();
        auto matched_delimiters = non_overlapping(_automaton.match(_bytes), true);
        size_t prev = 0;
        for (const auto& [index_start, index_end]: matched_delimiters) {
            _chunks.push_back({prev, index_start - prev, index_end - index_start + 1});
            prev = index_end + 1;
        }

        if (prev <= _bytes.size()) {
            _chunks.push_back({prev, _bytes.size() - prev, 0});
        }
        return;
    }

public:
    /**
     * @brief Load source text from an in-memory string (moved in).
     *
     * Replaces any previously loaded source and rebuilds the chunk index.
     *
     * @param source  The source text to split (ownership transferred).
     * @return        *this for method chaining.
     */
    SplitedString& load(std::string source) & noexcept {
        _bytes = std::move(source);
        _build_chunks();
        return *this;
    }

    SplitedString load(std::string source) && noexcept {
        _bytes = std::move(source);
        _build_chunks();
        return *this;
    }

    /**
     * @brief Load source text by reading a file from disk.
     *
     * Opens the file in binary mode, reads its entire contents, and rebuilds
     * the chunk index. Handles files of any size (within memory limits).
     *
     * @param file_path  Absolute or relative path to the source file.
     * @return           *this for method chaining.
     * @throws std::runtime_error if the file cannot be opened, its size cannot
     *         be determined, or reading fails.
     */
    SplitedString& read(const std::filesystem::path& file_path) & {
        std::ifstream file_in(file_path, std::ios::in | std::ios::binary | std::ios::ate);
        if (!file_in.is_open()) {
            throw std::runtime_error((std::ostringstream{} << "unable to open file: " << file_path.string()).str());
        }

        std::streamsize file_size = file_in.tellg();
        if (file_size < 0) {
            throw std::runtime_error((std::ostringstream{} << "unable to determine file size: " << file_path.string()).str());
        }
        file_in.seekg(0, std::ios::beg);

        _bytes.clear();
        _bytes.resize(static_cast<size_t>(file_size));
        if (file_size > 0 && !file_in.read(_bytes.data(), file_size)) {
            throw std::runtime_error((std::ostringstream{} << "failed to read from file: " << file_path.string()).str());
        }

        _build_chunks();
        return *this;
    }

    SplitedString read(const std::filesystem::path& file_path) && {
        this->read(file_path);
        return *this;
    }

    /**
     * @brief Return a view of the entire source text.
     *
     * The returned string_view is valid until the next load()/read()/move/destruction.
     */
    std::string_view source() const noexcept {
        return _bytes;
    }

    /**
     * @brief Return the number of chunks (content segments).
     *
     * This is 0 for a default-constructed SplitedString (no source loaded),
     * and at least 1 for any loaded source (even an empty string produces
     * one empty chunk).
     */
    size_t size() const noexcept {
        return _chunks.size();
    }

    /**
     * @brief Access the content of chunk @p idx (without delimiter).
     *
     * @param idx  0-based chunk index.
     * @return     A string_view of the chunk's content (may be empty).
     * @throws std::out_of_range if idx >= size().
     */
    std::string_view operator [] (size_t idx) const {
        if (idx >= _chunks.size()) {
            throw std::out_of_range((std::ostringstream{} << "index " << idx << " is out of range; valid range is [0, " << _chunks.size() << ")").str());
        }
        const auto& [start_point, content_len, delimiter_length] = _chunks[idx];
        return std::string_view(_bytes).substr(start_point, content_len);
    }

    /**
     * @brief Access the delimiter that follows chunk @p idx.
     *
     * For the last chunk, this returns an empty string_view (delimiter_length == 0).
     *
     * @param idx  0-based chunk index.
     * @return     A string_view of the delimiter (may be empty).
     * @throws std::out_of_range if idx >= size().
     */
    std::string_view get_delimiter(size_t idx) const {
        if (idx >= _chunks.size()) {
            throw std::out_of_range((std::ostringstream{} << "index " << idx << " is out of range; valid range is [0, " << _chunks.size() << ")").str());
        }
        const auto& [start_point, content_len, delimiter_length] = _chunks[idx];
        return std::string_view(_bytes).substr(start_point + content_len, delimiter_length);
    }

    /**
     * @brief Map a byte offset to the chunk index that contains it.
     *
     * Uses binary search over the chunk start positions (O(log K)).
     *
     * @param offset  Byte offset into the source (0-based).
     * @return        The chunk index containing this offset, or std::string::npos
     *                if _chunks is empty or offset is beyond the source.
     */
    size_t get_chunk_index(size_t offset) const noexcept {
        if (_chunks.empty() || offset >= _bytes.size()) {
            return std::string::npos;
        }

        auto it = std::upper_bound(
            _chunks.begin(),
            _chunks.end(),
            offset,
            [](size_t offset, const ChunkIndex& chunk_index) -> bool {
                return offset < chunk_index.index_start;
            }
        );

        if (it == _chunks.begin()) {
            return std::string::npos;
        }
        return static_cast<size_t>((it - _chunks.begin()) - 1);
    }

    /**
     * @brief Find all byte-offset ranges where @p pattern occurs in the source.
     *
     * By default (@p use_regex == false) this performs plain substring search via
     * std::string_view::find (not the automaton). Matches are non-overlapping:
     * each match advances past the pattern length.
     *
     * When @p use_regex == true, @p pattern is interpreted as an ECMAScript
     * regular expression and the source is scanned with std::sregex_iterator.
     * Regex matching is likewise non-overlapping — the iterator naturally resumes
     * scanning at the end of each match. Zero-length matches (e.g. produced by
     * patterns like "a*" at positions with no 'a') are skipped: they carry no
     * content, would yield an inverted (end < start) byte range, and are not
     * produced by the plain-substring path. An invalid regex yields an empty
     * result (the construction error is swallowed to preserve noexcept).
     *
     * @param pattern   The substring or (when use_regex) regex to search for.
     * @param use_regex If true, treat @p pattern as an ECMAScript regex.
     * @return          Vector of (start_byte, end_byte_inclusive) tuples.
     *                  Empty if pattern is empty, longer than the source (plain
     *                  mode only), an invalid regex, or not found.
     */
    std::vector<std::tuple<size_t, size_t>> locate_pattern(std::string_view pattern, bool use_regex = false) const noexcept {
        std::vector<std::tuple<size_t, size_t>> result;
        if (pattern.empty()) {
            return result;
        }

        if (use_regex) {
            // Note: a regex pattern's byte length bears no relation to its
            // match length (e.g. "\w" is 2 bytes but matches 1 char, while
            // "a{100}" is 6 bytes but may match nothing in a short source).
            // So the "pattern longer than source" short-circuit used by the
            // plain path does not apply here.
            std::regex re;
            try {
                re = std::regex(std::string(pattern));
            } catch (const std::regex_error&) {
                // Invalid regex: behave as "no match" rather than terminating
                // (this method is noexcept).
                return result;
            }

            for (auto it = std::sregex_iterator(_bytes.begin(), _bytes.end(), re),
                      end = std::sregex_iterator();
                 it != end; ++it) {
                const std::smatch& m = *it;
                if (m.length() == 0) {
                    // Skip zero-length matches: they hold no content and would
                    // invert the (start, end) range. std::sregex_iterator
                    // already advances past them, so the loop still terminates.
                    continue;
                }
                size_t start_pos = static_cast<size_t>(m.position());
                size_t end_pos = start_pos + static_cast<size_t>(m.length()) - 1;
                result.emplace_back(start_pos, end_pos);
            }
            return result;
        }

        // Plain substring search: a pattern longer than the source cannot
        // match, so short-circuit before scanning.
        if (pattern.size() > _bytes.size()) {
            return result;
        }

        size_t pos = 0;
        std::string_view source_view(_bytes);
        while ((pos = source_view.find(pattern, pos)) != std::string_view::npos) {
            result.emplace_back(pos, pos + pattern.size() - 1);
            pos += pattern.size();
        }
        return result;
    }

    /**
     * @brief Find all chunk-index ranges where @p pattern occurs.
     *
     * First calls locate_pattern() to find byte-offset matches, then maps each
     * match's start and end byte offsets to chunk indices via get_chunk_index().
     *
     * The @p use_regex flag is forwarded to locate_pattern(): when true, @p
     * pattern is treated as an ECMAScript regular expression (non-overlapping,
     * zero-length matches skipped — see locate_pattern() for details). This
     * makes regex matching a native capability of the chunk lookup rather than
     * something callers have to implement themselves.
     *
     * This is the primary method used by LangAnalyze::locate_pattern() to
     * determine which lines contain a given pattern.
     *
     * @param pattern   The substring or (when use_regex) regex to search for.
     * @param use_regex If true, treat @p pattern as an ECMAScript regex.
     * @return          Vector of (start_chunk, end_chunk_inclusive) tuples.
     *                  A match whose byte offsets cannot be mapped is silently
     *                  skipped (should not happen for valid source/pattern pairs).
     */
    std::vector<std::tuple<size_t, size_t>> locate_pattern_chunk(std::string_view pattern, bool use_regex = false) const noexcept {
        std::vector<std::tuple<size_t, size_t>> result;
        auto matches = locate_pattern(pattern, use_regex);
        result.reserve(matches.size());

        for (const auto& [start_pos, end_pos]: matches) {
            size_t chunk_start = get_chunk_index(start_pos), chunk_end = get_chunk_index(end_pos);
            if (chunk_start == std::string::npos || chunk_end == std::string::npos) {
                continue;
            }
            result.emplace_back(chunk_start, chunk_end);
        }
        return result;
    }

    // =========================================================================
    // Iterator support (random-access)
    // =========================================================================
    //
    // Provides full random-access iteration over chunks. Each dereference
    // returns a std::string_view of the chunk's content (same as operator[]).
    //
    // Both const_iterator and iterator are the same type (SplitedString is
    // read-only). Reverse iterators are provided via std::reverse_iterator.
    //
    // Usage:
    //
    //   for (auto line : ss) { ... }                    // range-for
    //   auto it = ss.begin(); it += 3; auto v = *it;    // random access
    //   for (auto it = ss.rbegin(); it != ss.rend(); ++it) // reverse

    class iterator {
    private:
        const SplitedString* _owner = nullptr;
        std::ptrdiff_t _idx = 0;

    public:
        using iterator_category = std::random_access_iterator_tag;
        using iterator_concept  = std::random_access_iterator_tag;
        using value_type        = std::string_view;
        using difference_type   = std::ptrdiff_t;
        using pointer           = void;          ///< No raw pointer to string_view (it's a value type).
        using reference         = std::string_view;

        iterator() noexcept = default;
        iterator(const SplitedString* owner, std::ptrdiff_t idx) noexcept
            : _owner(owner), _idx(idx) {}

        /// Dereference: return a view of the chunk at the current position.
        reference operator*() const {
            return (*_owner)[static_cast<size_t>(_idx)];
        }

        /// Random access by offset from current position.
        reference operator[](difference_type n) const {
            return (*_owner)[static_cast<size_t>(_idx + n)];
        }

        // ---- Forward / backward traversal ----
        iterator& operator++() noexcept { ++_idx; return *this; }
        iterator  operator++(int) noexcept { iterator t = *this; ++_idx; return t; }
        iterator& operator--() noexcept { --_idx; return *this; }
        iterator  operator--(int) noexcept { iterator t = *this; --_idx; return t; }

        // ---- Compound assignment ----
        iterator& operator+=(difference_type n) noexcept { _idx += n; return *this; }
        iterator& operator-=(difference_type n) noexcept { _idx -= n; return *this; }
        iterator operator+(difference_type n) const noexcept { return iterator(_owner, _idx + n); }
        iterator operator-(difference_type n) const noexcept { return iterator(_owner, _idx - n); }

        /// Friend: n + iterator (enables expressions like `it + 3` and `3 + it`).
        friend iterator operator+(difference_type n, const iterator& it) noexcept {
            return iterator(it._owner, it._idx + n);
        }

        /// Return the distance between two iterators (as number of chunks).
        difference_type operator-(const iterator& other) const noexcept {
            return _idx - other._idx;
        }

        // ---- Comparison ----
        bool operator==(const iterator& o) const noexcept { return _idx == o._idx; }
        bool operator!=(const iterator& o) const noexcept { return _idx != o._idx; }
        bool operator< (const iterator& o) const noexcept { return _idx <  o._idx; }
        bool operator> (const iterator& o) const noexcept { return _idx >  o._idx; }
        bool operator<=(const iterator& o) const noexcept { return _idx <= o._idx; }
        bool operator>=(const iterator& o) const noexcept { return _idx >= o._idx; }
    };

    /// const_iterator is the same as iterator (SplitedString is read-only).
    using const_iterator = iterator;

    // ---- Range access ----

    /// Return an iterator to the first chunk.
    iterator begin() const noexcept { return iterator(this, 0); }

    /// Return an iterator past the last chunk.
    iterator end()   const noexcept {
        return iterator(this, static_cast<std::ptrdiff_t>(_chunks.size()));
    }

    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend()   const noexcept { return end(); }

    /// Reverse iterator (rbegin → last chunk, rend → before first chunk).
    std::reverse_iterator<iterator> rbegin() const noexcept {
        return std::reverse_iterator<iterator>(end());
    }

    std::reverse_iterator<iterator> rend() const noexcept {
        return std::reverse_iterator<iterator>(begin());
    }
};

} // namespace indextools
