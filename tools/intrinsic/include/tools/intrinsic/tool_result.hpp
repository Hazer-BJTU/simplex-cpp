#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "dataclass/model_io.hpp"

namespace tools::intrinsic {

/**
 * Model-facing text result shared by intrinsic tools. Each record renders a
 * fenced Metadata region followed by an optional Output region. Fields added
 * after output (including hints) are still placed in Metadata. Separate records
 * are divided by a Markdown rule. Literal output uses fences longer than any
 * backtick run it contains, preserving readable text without JSON escaping.
 * This is a presentation format, not a machine protocol or trust boundary.
 */
class ToolResult {
public:
    /**
     * Append a metadata field in insertion order. Empty strings/containers and
     * null are omitted; false and zero remain. Multiline/control-bearing strings
     * use compact JSON so a value cannot introduce additional metadata lines.
     * Names are developer-owned labels, not user input.
     */
    ToolResult& field(std::string_view name, nlohmann::json value);

    /**
     * Append a named literal output block, including its byte count. Empty output
     * is shown explicitly; truncated output is labeled. Output is always rendered
     * after all metadata for this record, regardless of call order.
     */
    ToolResult& block(std::string_view name, std::string text, bool truncated = false);

    /** Start another record. Leading, repeated, and trailing separators are omitted. */
    ToolResult& separate();

    /** Wrap the formatted document in one model_io text content part. */
    [[nodiscard]] model_io::Content render() const;

    /** Cached rendered document; a later mutation invalidates its contents. */
    [[nodiscard]] const std::string& text() const;
    [[nodiscard]] bool empty() const noexcept;

private:
    struct Record {
        std::string metadata;
        std::string output;
    };
    std::vector<Record> _records{1};
    mutable std::string _text;
    mutable bool _dirty = true;
};

} // namespace tools::intrinsic
