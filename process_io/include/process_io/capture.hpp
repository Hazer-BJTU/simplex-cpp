#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <boost/system/error_code.hpp>

namespace process_io {

/// A terminal stream is distinct from stdout/stderr; it is never duplicated.
enum class Stream { Stdout, Stderr, Terminal };
enum class OutputTopology { Separate, Terminal };

/// Capture completion is independent of child exit and task joining.
enum class StreamEnd { Open, Eof, Stopped, Failed };

/** Borrowed raw bytes. Valid only until the next mutation of the capture store.
 * Consume this view and update any session cursor in one serialized operation.
 * Bytes are not decoded, escaped, stripped of terminal controls or JSON-ready.
 */
struct StreamView {
    std::string_view bytes;
    bool truncated = false;
    StreamEnd end = StreamEnd::Open;
    boost::system::error_code error;
};

/** One prefix-retaining capture store, reusable by pipe and terminal backends.
 * The current limit applies PER STREAM to preserve the legacy pipe behavior;
 * issue #122 PR 4 will change it to a shared per-environment budget. Zero means
 * unlimited. Count raw bytes, not presentation labels or allocator overhead.
 * Mutation and observation require the owning environment's serialization.
 */
class CaptureBuffer {
public:
    explicit CaptureBuffer(OutputTopology topology, std::size_t max_output_bytes);

    /// Empty optional means absent; an existing empty stream has an empty view.
    [[nodiscard]] std::optional<StreamView> view(Stream stream) const;
    /// Borrowed storage; the same serialized-or-quiescent rule as view().
    [[nodiscard]] const std::string& bytes(Stream stream) const;
    /// Discards bytes beyond the limit and marks truncation; callers KEEP reading.
    void append(Stream stream, std::string_view bytes);
    /// Finish once. Forced stop and failure mark retained data as incomplete.
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
    Record& require(Stream stream);
};

} // namespace process_io
