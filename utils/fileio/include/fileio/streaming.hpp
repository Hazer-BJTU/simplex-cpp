#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <vector>

#include "fileio/replace_existing.hpp"

namespace fileio {

/** Default fixed read buffer for streaming scans (64 KiB). */
inline constexpr std::size_t kStreamBufferBytes = 64 * 1024;

/**
 * One sequential scan of an opened regular file with a fixed-size buffer.
 *
 * Every call to next() returns the next bytes of the file as a borrowed view
 * valid until the following call; an empty view means EOF. Interrupted reads
 * are retried, short reads are returned as they arrive, and the descriptor is
 * close-on-exec and owned until destruction. The file is opened nonblocking so
 * a substituted FIFO cannot wait for a writer, then checked on the descriptor:
 * only regular files are accepted. Symlinks in the final path component are
 * followed, matching ordinary reads.
 *
 * observed_bytes() counts only bytes actually returned so far; it is not the
 * size reported at open and it does not authorize an unbounded scan. The
 * identity captured at open is available for callers that compare it against a
 * later inspection. This scan is synchronous and is not a snapshot: bytes
 * written by another actor after they were read are not seen, and a shrinking
 * file simply ends earlier. Open/status/read failures throw std::system_error;
 * an embedded NUL in the path throws std::invalid_argument. Memory use is the
 * fixed buffer plus caller state and never scales with file size.
 */
class SequentialFile {
public:
    /**
     * Open `path` for one bounded scan.
     *
     * buffer_bytes must be between 4 and 1 MiB so terminator and UTF-8
     * lookahead can straddle a buffer edge without unbounded input copying.
     * Throws std::invalid_argument for an unusable buffer size or NUL path.
     */
    [[nodiscard]] static SequentialFile open(
        const std::filesystem::path& path,
        std::size_t buffer_bytes = kStreamBufferBytes);

    SequentialFile(SequentialFile&& other) noexcept;
    SequentialFile& operator=(SequentialFile&& other) noexcept;
    ~SequentialFile();

    SequentialFile(const SequentialFile&) = delete;
    SequentialFile& operator=(const SequentialFile&) = delete;

    /** Next bytes, or an empty view at EOF. Borrowed until the next call. */
    [[nodiscard]] std::string_view next();

    /** Bytes returned by next() so far. */
    [[nodiscard]] std::uintmax_t observed_bytes() const noexcept;

    /** Metadata captured from the descriptor at open. */
    [[nodiscard]] const FileIdentity& identity() const noexcept;

private:
    SequentialFile(int descriptor, FileIdentity identity, std::size_t buffer_bytes);

    [[nodiscard]] std::string_view fill();

    int descriptor_ = -1;
    FileIdentity identity_;
    std::vector<char> buffer_;
    std::uintmax_t observed_bytes_ = 0;
};

} // namespace fileio
