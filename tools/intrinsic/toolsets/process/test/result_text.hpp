#pragma once

//
// result_text.hpp — reading a tool result back in the tests
// =========================================================
//
// A TEST-ONLY HEADER. Nothing in this package parses a tool result: the text is
// written for a model and for a human (tools/intrinsic/tool_result.hpp), and
// the whole point of the format is that a reader does not have to un-encode it.
// A test, though, has to ask precise questions — "what exit code", "did the
// hint survive", "what did THIS session print" — and asking them by searching
// the text would make every case a substring hunt.
//
// So this is the renderer's own grammar, read back:
//
//   name: value        a field, one line
//   name (N bytes):    a block header, and the N bytes under it are its body —
//   <N bytes>          verbatim, exact, because the header counts them
//   ---                one record's end (ToolResult::separate())
//
// The count in the header is what makes this exact rather than approximate: a
// blank line separates a block from whatever follows it, so the bytes between
// them could be read as the body plus a newline the tool did not produce. The
// header says how many there really are, and the reader takes that many.
//
// and the reader answers with the values, grouped by record. It is deliberately
// strict: a case that asks for "the" value of a name that appears twice fails
// rather than silently taking the first, because a result with two session ids
// is a result a test should be reading record by record.
//
// The known limit, and it is the format's: a block's TEXT is whatever a tool
// produced, so a line inside it may look like a field. The cases here drive
// children whose output they chose (`echo`, `seq`, `printf`), so nothing they
// print looks like `session_id: …`, and the format is documented as being for
// readers rather than for parsers.
//

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace process_test {

/// One `name: value` line.
struct Field {
    std::string name;
    std::string value;
};

/// One block: its header's name and the text under it, verbatim.
struct Block {
    std::string name;
    std::string body;
};

/// One record of a result: the fields and blocks written between two `---`
/// rules (`ToolResult::separate()`), or the whole result when it has none.
struct ResultRecord {
    std::vector<Field> fields;
    std::vector<Block> blocks;

    [[nodiscard]] bool has(std::string_view name) const;
    /// The value written under `name`, which must appear exactly once.
    [[nodiscard]] std::string field(std::string_view name) const;
    /// The body of the block written under `name`, which must appear once.
    [[nodiscard]] std::string block(std::string_view name) const;
};

/// A rendered tool result, read back.
class ResultText {
public:
    explicit ResultText(std::string_view raw);

    /// The records, in order. A result that never called separate() has one.
    [[nodiscard]] const std::vector<ResultRecord>& records() const
    {
        return _records;
    }

    /// Whether anything at all was written under `name` — a field or a block,
    /// in any record.
    [[nodiscard]] bool has(std::string_view name) const;

    /// The single value written under `name`, searched across every record.
    /// Fails the case when there is none, or more than one: "the exit code" is
    /// a question about a result that has one.
    [[nodiscard]] std::string field(std::string_view name) const;

    /// The single block body written under `name`, same rule as field().
    [[nodiscard]] std::string block(std::string_view name) const;

    /// The text as the model received it, for the cases that are about the
    /// rendering itself.
    [[nodiscard]] const std::string& text() const noexcept { return _text; }

private:
    std::string _text;
    std::vector<ResultRecord> _records;
};

namespace detail {

/// Whether `line` is `name:` or `name (…):` — a block header.
[[nodiscard]] inline bool is_block_header(std::string_view line)
{
    if (line.empty() || line.back() != ':') return false;
    return line.find(": ") == std::string_view::npos;
}

/// Whether `line` is `name: value` — a field line. `name` is what precedes the
/// first colon-space, and it must look like a name this tree writes.
[[nodiscard]] inline bool is_field_line(std::string_view line)
{
    const std::size_t colon = line.find(": ");
    if (colon == std::string_view::npos || colon == 0) return false;
    for (std::size_t index = 0; index < colon; ++index) {
        const char character = line[index];
        const bool word = (character >= 'a' && character <= 'z')
                          || (character >= 'A' && character <= 'Z')
                          || (character >= '0' && character <= '9')
                          || character == '_';
        if (!word) return false;
    }
    return true;
}

[[nodiscard]] inline std::string_view name_of(std::string_view line)
{
    const std::size_t end = line.find_first_of(": ");
    return line.substr(0, end);
}

/// How many bytes the body of a block header holds: the `(N bytes)` or
/// `(truncated, first N bytes)` the renderer writes. std::nullopt for a line
/// that is not a block header.
[[nodiscard]] inline std::optional<std::size_t> body_length(
    std::string_view line)
{
    const std::size_t marker = line.rfind(" bytes)");
    if (marker == std::string_view::npos) return std::nullopt;
    std::size_t begin = marker;
    while (begin > 0 && line[begin - 1] >= '0' && line[begin - 1] <= '9') {
        --begin;
    }
    if (begin == marker) return std::nullopt;
    std::size_t length = 0;
    for (std::size_t index = begin; index < marker; ++index) {
        length = length * 10 + static_cast<std::size_t>(line[index] - '0');
    }
    return length;
}

} // namespace detail

inline bool ResultRecord::has(std::string_view name) const
{
    for (const Field& field : fields) {
        if (field.name == name) return true;
    }
    for (const Block& block : blocks) {
        if (block.name == name) return true;
    }
    return false;
}

inline std::string ResultRecord::field(std::string_view name) const
{
    std::vector<std::string> found;
    for (const Field& field : fields) {
        if (field.name == name) found.push_back(field.value);
    }
    BOOST_REQUIRE_MESSAGE(found.size() == 1,
                          "expected one field \"" << name << "\", found "
                                                  << found.size());
    return found.front();
}

inline std::string ResultRecord::block(std::string_view name) const
{
    std::vector<std::string> found;
    for (const Block& block : blocks) {
        if (block.name == name) found.push_back(block.body);
    }
    BOOST_REQUIRE_MESSAGE(found.size() == 1,
                          "expected one block \"" << name << "\", found "
                                                  << found.size());
    return found.front();
}

inline ResultText::ResultText(std::string_view raw) : _text(raw)
{
    // A record is what lies between two `---` rules; the rules themselves are
    // the renderer's, and nothing else in a result is one.
    _records.emplace_back();

    std::size_t position = 0;
    while (position < raw.size()) {
        const std::size_t newline = raw.find('\n', position);
        const std::size_t line_end =
            newline == std::string_view::npos ? raw.size() : newline;
        const std::string_view line = raw.substr(position, line_end - position);
        const std::size_t next =
            newline == std::string_view::npos ? raw.size() : newline + 1;
        position = next;

        if (line == "---") {
            _records.emplace_back();
            continue;
        }
        if (detail::is_block_header(line)) {
            const std::size_t length =
                detail::body_length(line).value_or(0);
            // Exactly the bytes the header counted, from where the body
            // starts. What follows them is the renderer's separation.
            _records.back().blocks.push_back(Block{
                std::string(detail::name_of(line)),
                std::string(raw.substr(position, length))});
            position = std::min(raw.size(), position + length);
            continue;
        }
        if (detail::is_field_line(line)) {
            const std::string_view name = detail::name_of(line);
            const std::string value = std::string(line.substr(name.size() + 2));
            // The renderer's spelling of a block with no text under it.
            if (value == "(empty)") {
                _records.back().blocks.push_back(
                    Block{std::string(name), {}});
                continue;
            }
            _records.back().fields.push_back(Field{std::string(name), value});
            continue;
        }
        // A blank line, or anything the format does not claim: a separator
        // between elements, which carries no value of its own.
    }
}

inline bool ResultText::has(std::string_view name) const
{
    for (const ResultRecord& record : _records) {
        if (record.has(name)) return true;
    }
    return false;
}

inline std::string ResultText::field(std::string_view name) const
{
    std::vector<std::string> found;
    for (const ResultRecord& record : _records) {
        for (const Field& field : record.fields) {
            if (field.name == name) found.push_back(field.value);
        }
    }
    BOOST_REQUIRE_MESSAGE(found.size() == 1,
                          "expected one field \"" << name << "\" in\n"
                                                  << _text << "\nfound "
                                                  << found.size());
    return found.front();
}

inline std::string ResultText::block(std::string_view name) const
{
    std::vector<std::string> found;
    for (const ResultRecord& record : _records) {
        for (const Block& block : record.blocks) {
            if (block.name == name) found.push_back(block.body);
        }
    }
    BOOST_REQUIRE_MESSAGE(found.size() == 1,
                          "expected one block \"" << name << "\" in\n"
                                                  << _text << "\nfound "
                                                  << found.size());
    return found.front();
}

} // namespace process_test
