#include "indextools/viewer.hpp"
#include "indextools/schema.hpp"

namespace indextools {

namespace {

// Add either a canonical "[first, last]" range or "[]" for an empty window.
std::string range_or_empty(bool empty, size_t first, size_t last) {
    return empty ? std::string("[]") : schema::range_str(first, last);
}

} // namespace

nlohmann::json read_lines(
    const SplitedString& source_lines,
    size_t line_start,
    size_t max_lines,
    const std::filesystem::path& file_path
) {
    const size_t total = source_lines.size();

    // Clamp the half-open window [line_start, line_end) to [0, total).
    size_t begin = std::min(line_start, total);
    size_t end = begin;
    if (max_lines > 0 && begin < total) {
        // Guard against begin + max_lines overflowing size_t.
        size_t room = total - begin;
        end = begin + std::min(max_lines, room);
    }

    const bool empty = (begin >= end);
    const bool truncated = (end < total);

    schema::TextBody body;
    for (size_t i = begin; i < end; ++i) {
        body.line(source_lines[i], i, schema::line_type::base);
    }

    nlohmann::json meta = schema::MetaBuilder()
        .field("File", file_path.string())
        .field("Lines", range_or_empty(empty, begin, end - (empty ? 0 : 1)))
        .field("Total Lines", total)
        .field("Truncated", truncated)
        .build();

    nlohmann::json result = nlohmann::json::array();
    result.push_back(schema::text_block(std::move(meta), body.build()));
    return result;
}

nlohmann::json read_bytes(
    const SplitedString& source_lines,
    size_t byte_start,
    size_t max_bytes,
    const std::filesystem::path& file_path
) {
    std::string_view src = source_lines.source();
    const size_t total = src.size();

    // Clamp the half-open window [byte_start, byte_end) to [0, total).
    size_t begin = std::min(byte_start, total);
    size_t len = 0;
    if (max_bytes > 0 && begin < total) {
        size_t room = total - begin;
        len = std::min(max_bytes, room);
    }

    const bool empty = (len == 0);
    const bool truncated = (begin + len < total);

    std::string slice(src.substr(begin, len));
    const bool binary = slice.find('\0') != std::string::npos;

    nlohmann::json meta = schema::MetaBuilder()
        .field("File", file_path.string())
        .field("Bytes", range_or_empty(empty, begin, begin + len - (empty ? 0 : 1)))
        .field("Total Bytes", total)
        .field("Truncated", truncated)
        .field("Binary", binary)
        .build();

    nlohmann::json result = nlohmann::json::array();
    result.push_back(schema::content_block(std::move(meta), std::move(slice)));
    return result;
}

} // namespace indextools
