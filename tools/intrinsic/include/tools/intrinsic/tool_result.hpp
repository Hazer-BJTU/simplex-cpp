#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "dataclass/model_io.hpp"

namespace tools::intrinsic {

/**
 * Model-facing plain text shared by intrinsic tools. Each record renders all
 * metadata first using "[[name]]: value" lines, followed by an optional blank line
 * and named literal output blocks. Fields added after output (including hints)
 * still precede that output. Records are divided by a plain "---" separator.
 * No Markdown headings or fences are added. Output is not escaped, so its text
 * can resemble metadata; the markers are visual cues, not a trust boundary.
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
