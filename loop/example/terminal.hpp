#pragma once

#include <ostream>
#include <string_view>

namespace loop_example {

/**
 * Writes one complete, indented transcript block through a single stream.
 * Newlines remain readable; control bytes (including ANSI escapes and carriage
 * returns) are shown as hex so child output cannot overwrite prompts or headings.
 * UTF-8 bytes pass through. This is presentation only: model history is unchanged.
 * Process tool results already name stdout/stderr; never parse that prose as a
 * machine protocol or merge the child's stderr into the host's diagnostic stream.
 */
inline void block(std::ostream& output, std::string_view title, std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    output << "\n=== ";
    // Titles can contain model-provided call IDs; keep them on one safe line.
    for (unsigned char byte : title) {
        if (byte < 32 || byte == 127) {
            output << "\\x" << hex[byte >> 4] << hex[byte & 15];
        } else {
            output.put(static_cast<char>(byte));
        }
    }
    output << " ===\n  | ";
    if (text.empty()) {
        output << "(empty)";
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte == '\n') {
            output << '\n';
            if (index + 1 < text.size()) {
                output << "  | ";
            }
        } else if (byte < 32 || byte == 127) {
            output << "\\x" << hex[byte >> 4] << hex[byte & 15];
        } else {
            output.put(static_cast<char>(byte));
        }
    }
    if (text.empty() || text.back() != '\n') {
        output << '\n';
    }
    output << "=== end ===\n" << std::flush;
}

} // namespace loop_example
