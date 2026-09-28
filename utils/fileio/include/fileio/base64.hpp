#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace fileio {

/**
 * Encode arbitrary bytes as standard RFC 4648 Base64 with '=' padding.
 * The result contains no line breaks, data-URL prefix, or media type.
 */
[[nodiscard]] std::string base64_encode(std::string_view bytes);

/**
 * Decode standard padded Base64 into arbitrary bytes.
 * Rejects whitespace, URL-safe alphabet characters, missing/invalid padding,
 * and nonzero unused bits by throwing std::invalid_argument.
 */
[[nodiscard]] std::string base64_decode(std::string_view encoded);

/**
 * Read a regular file in binary mode and return its Base64 representation.
 * Follows symlinks and inherits read_prefix's file/error behavior. The entire
 * file and encoded result reside in memory; concurrent edits are not a stable
 * snapshot. Empty files encode to an empty string.
 */
[[nodiscard]] std::string base64_encode_file(const std::filesystem::path& path);

/**
 * Decode Base64 and atomically replace/create a file with the original bytes.
 * Invalid input leaves an existing destination untouched. The destination
 * follows atomic_write's permissions, symlink, and durability semantics.
 */
void base64_decode_file(std::string_view encoded, const std::filesystem::path& path);

} // namespace fileio
