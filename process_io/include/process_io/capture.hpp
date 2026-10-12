#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <boost/system/error_code.hpp>

namespace process_io {

/// A terminal stream is distinct from stdout/stderr; it is never duplicated.
enum class Stream {
    Stdout,
    Stderr,
    Terminal
};
/// Separate pipes retain stdout/stderr independently; a PTY has one merged stream.
enum class OutputTopology {
    Separate,
    Terminal
};

/// Capture completion is independent of child exit and task joining.
enum class StreamEnd {
    Open,    // The reader can still append bytes.
    Eof,     // The peer ended the stream naturally.
    Stopped, // The owner forced capture to stop before natural EOF.
    Failed   // An unexpected read/task failure ended capture.
};

/** Borrowed raw bytes. Valid only until the next mutation of the capture store.
 * Consume this view and update any session cursor in one serialized operation.
 * Bytes are not decoded, escaped, stripped of terminal controls or JSON-ready.
 */
struct StreamView {
    std::string_view bytes;
    // True for budget loss or forced/failed capture, even when bytes is empty.
    bool truncated = false;
    StreamEnd end = StreamEnd::Open;
    boost::system::error_code error;
};

/** One prefix-retaining capture store, reusable by pipe and terminal backends.
 * One budget covers all present streams. Zero means unlimited. Count raw
 * retained bytes, not presentation labels or allocator/task overhead. Read
 * scheduling decides which pipe stream obtains remaining capacity; no fixed
 * split or reconstructed stdout/stderr interleaving is promised.
 * Mutation and observation require the owning environment's serialization.
 */
class CaptureBuffer {
public:
    /// Allocate only the streams named by topology; a zero limit retains all bytes.
    explicit CaptureBuffer(OutputTopology topology, std::size_t max_output_bytes);

    /// Empty optional means absent; an existing empty stream has an empty view.
    [[nodiscard]] std::optional<StreamView> view(Stream stream) const;
    /// Borrowed storage; the same serialized-or-quiescent rule as view().
    /// Unlike view(), requesting an absent stream throws std::invalid_argument.
    [[nodiscard]] const std::string& bytes(Stream stream) const;
    /// Discards bytes beyond the limit and marks truncation; callers KEEP reading.
    /// Appending to an absent or already ended stream is a caller error.
    void append(Stream stream, std::string_view bytes);
    /// Finish once. Forced stop and failure mark retained data as incomplete.
    /// Later finish calls preserve the first end reason and its error code.
    void finish(Stream stream, StreamEnd end, boost::system::error_code error = {});
    /// True when every existing output stream ended, even after failure/stop.
    [[nodiscard]] bool complete() const noexcept;

private:
    struct Record {
        std::string bytes;
        bool truncated = false;
        StreamEnd end = StreamEnd::Open;
        boost::system::error_code error;
    };
    std::array<std::optional<Record>, 3> _streams;
    std::size_t _limit;
    // Used only with a finite limit; always <= _limit. EOF/reads do not refund
    // retained bytes, and each environment owns an independent budget.
    std::size_t _retained_bytes = 0;
    /// Access mutable storage, rejecting a stream absent from this topology.
    Record& require(Stream stream);
};

} // namespace process_io
