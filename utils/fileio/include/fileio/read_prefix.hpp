#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace fileio {

/**
 * Read at most max_bytes from an opened regular file, stopping at EOF.
 *
 * Follows symlinks. Checks the opened descriptor rather than a preliminary
 * path status; nonblocking open prevents a substituted FIFO from waiting for
 * a writer. Regular-file IO remains synchronous. Uses close-on-exec and owns
 * the descriptor until return or exception. Does not provide a snapshot of
 * concurrent edits. An embedded NUL in path throws std::invalid_argument;
 * open, status, read and non-regular-file failures throw std::system_error.
 * Allocation errors propagate. max_bytes bounds both memory and bytes read.
 */
[[nodiscard]] std::string read_prefix(
    const std::filesystem::path& path,
    std::size_t max_bytes);

} // namespace fileio
