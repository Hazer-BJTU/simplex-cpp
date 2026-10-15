#pragma once

#include <iosfwd>
#include <string_view>

namespace logging {

/**
 * Write unformatted text and flush before returning, protecting POSIX console
 * writes from SIGPIPE without changing the process-wide signal disposition.
 * Return false on stream failure or if signal protection cannot be established.
 * Stream exceptions propagate after the original thread signal mask is restored.
 * Existing stream error flags are retained; no retry or fallback is performed.
 *
 * Callers must serialize access to the stream. Use this for synchronous console
 * output only, never across coroutine suspension. Logger provides its own lock.
 */
bool write_console(std::ostream& stream, std::string_view text);

} // namespace logging
