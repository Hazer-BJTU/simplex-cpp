#pragma once

#include <filesystem>
#include <string_view>

namespace fileio {

/**
 * Resolve a nonempty relative persistence child below its configured root.
 * Reject absolute paths, embedded NULs and parent traversal. This is lexical
 * validation only: callers requiring a symlink boundary must check separately.
 * The path uses the target platform's filesystem semantics.
 */
std::filesystem::path child_path(
    const std::filesystem::path& root, std::string_view relative);

} // namespace fileio
