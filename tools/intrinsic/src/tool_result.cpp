#include "tools/intrinsic/tool_result.hpp"

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace tools::intrinsic {
namespace {

/// The value of one field, as the single line it has to fit on.
///
/// A one-line string is written as itself: a path, a command, a label or an id
/// is what it says it is, and quoting it would be the escaping this format
/// exists to avoid. A string that carries a line break is not something a line
/// can hold verbatim, so it is written as compact JSON — one line, escaped,
/// which is honest about having been folded.
[[nodiscard]] std::string rendered_value(const nlohmann::json& value)
{
    if (value.is_string()) {
        const std::string& raw = value.get_ref<const std::string&>();
        if (raw.find_first_of("\r\n") == std::string::npos) {
            return raw;
        }
        return value.dump();
    }
    return value.dump();
}

/// Whether a value says anything. An empty string, an empty array, an empty
/// object and null all render as no line at all: `description:` followed by
/// nothing is a line a reader has to interpret, and the absence of the line
/// says the same thing without one (file header, the rules).
[[nodiscard]] bool says_nothing(const nlohmann::json& value)
{
    if (value.is_null()) return true;
    if (value.is_string()) return value.get_ref<const std::string&>().empty();
    if (value.is_array() || value.is_object()) return value.empty();
    return false;
}

} // namespace

void ToolResult::end_line()
{
    if (!_text.empty() && _text.back() != '\n') {
        _text += '\n';
    }
}

void ToolResult::blank_line()
{
    // Pending, not written: a blank line is only worth its byte once something
    // follows it, and this is what keeps a block's trailing separation from
    // doubling with the next element's leading one — and from dangling at the
    // end of a result whose last element is a block.
    if (_text.empty()) return;
    _pending_blank = true;
}

void ToolResult::start_element()
{
    end_line();
    if (_pending_blank) {
        _text += '\n';
        _pending_blank = false;
    }
    _last_was_rule = false;
}

ToolResult& ToolResult::field(std::string_view name, nlohmann::json value)
{
    if (says_nothing(value)) {
        return *this;
    }
    start_element();
    _text += name;
    _text += ": ";
    _text += rendered_value(value);
    end_line();
    return *this;
}

ToolResult& ToolResult::block(std::string_view name, std::string text,
                              bool truncated)
{
    // A block belongs to the reader's eye, so it is set apart from the fields
    // around it on both sides.
    blank_line();
    start_element();
    _text += name;
    if (text.empty()) {
        _text += ": (empty)";
        end_line();
        blank_line();
        return *this;
    }
    // The count is of the bytes HERE, which is the whole capture in every case
    // but a truncated one — and there the header says which it is, so the
    // number cannot be read as the size of something it is not.
    _text += std::format(" ({}{} bytes):",
                         truncated ? "truncated, first " : "", text.size());
    end_line();
    _text += text;
    end_line();
    blank_line();
    return *this;
}

ToolResult& ToolResult::separate()
{
    // Nothing to separate from, and nothing to add to an edge that is already
    // the last thing written: two calls in a row are one rule.
    if (_text.empty() || _last_was_rule) {
        return *this;
    }
    // A visible edge rather than a blank line: a block is surrounded by blank
    // lines too, so blank alone could not say "a new record starts here" to a
    // reader (or to a test).
    blank_line();
    start_element();
    _text += "---";
    end_line();
    _last_was_rule = true;
    blank_line();
    return *this;
}

model_io::Content ToolResult::render() const
{
    return model_io::Content{
        .type = model_io::ContentType::Text,
        .raw = _text,
        .extras = std::nullopt,
    };
}

} // namespace tools::intrinsic
