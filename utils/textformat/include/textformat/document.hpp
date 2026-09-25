#pragma once

#include <string>
#include <string_view>

namespace textformat {

/**
 * Render one plain metadata line as "[[name]]: value" followed by a newline.
 * Names are developer-owned labels. The caller serializes values into a single
 * line (escaping control characters as needed) and decides which empty fields
 * to omit. The markers distinguish metadata visually without Markdown headings
 * or fences; they are not a machine protocol or a security boundary.
 */
inline std::string metadata_field(std::string_view name, std::string_view value) {
    std::string result = "[[";
    result += name;
    result += "]]: ";
    result += value;
    result += '\n';
    return result;
}

} // namespace textformat
