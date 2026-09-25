#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace textformat {

/**
 * Frame literal text as a Markdown code block. The fence is longer than any
 * backtick run in the body, so embedded Markdown cannot close the block.
 * The body is not escaped or truncated. A missing final newline is added only
 * to place the closing fence on its own line; callers retain byte counts when
 * exact output length matters. This is presentation, not a security boundary.
 */
inline std::string literal_block(std::string_view body) {
    std::size_t longest = 0;
    std::size_t run = 0;
    for (const char character : body) {
        run = character == '`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const std::string fence(std::max(std::size_t{3}, longest + 1), '`');
    std::string result = fence + "text\n";
    result += body;
    if (!body.empty() && body.back() != '\n') {
        result += '\n';
    }
    result += fence + "\n";
    return result;
}

/**
 * Render one metadata region using the shared heading and literal framing.
 * Callers provide field lines; keeping serialization outside this helper makes
 * it usable without tool, model, or JSON dependencies. Empty metadata is omitted.
 */
inline std::string metadata(std::string_view fields) {
    if (fields.empty()) {
        return {};
    }
    return "## Metadata\n" + literal_block(fields);
}

} // namespace textformat
