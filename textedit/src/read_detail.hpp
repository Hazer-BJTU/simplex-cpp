#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace textedit::detail {

/** Load an existing regular file with one byte of oversize lookahead. */
[[nodiscard]] std::string load_file(
    const std::filesystem::path& path,
    std::size_t max_file_bytes);

} // namespace textedit::detail
