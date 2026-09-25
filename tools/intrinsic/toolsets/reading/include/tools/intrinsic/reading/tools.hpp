#pragma once

#include "tools/intrinsic/tool_base.hpp"

namespace tools::intrinsic {

/**
 * Read a regular file as text lines or bytes. Stateless, ReadOnly and Trusted;
 * relative paths use the host working directory, with no workspace sandbox.
 * IO is synchronous and bounded by the textedit file limit. No confirmation,
 * file writes or shared cursors are involved. Concurrent invocations are safe
 * but do not promise a snapshot of files modified by other actors.
 */
class ReadTextTool final : public DeclaredTool {
public:
    static constexpr std::size_t kMaxFileBytes = 16 * 1024 * 1024;
    static constexpr std::size_t kMaxOutputBytes = 64 * 1024;
    static constexpr std::uint64_t kDefaultCount = 200;

    /// Load the runtime YAML declaration; a missing declaration disables routing.
    ReadTextTool();

    /// Validate combinations and materialize defaults before registry execution.
    void ensure_arguments(model_io::InvokeQuery& query) const override;
    /// Set fixed read-only/trusted attributes; YAML cannot change this policy.
    void write_attributes(model_io::InvokeQuery& query) const override;
    /// Read, render bounded output and append advisory encoding/truncation hints.
    /// File/range errors become the shared InvokeException failure contract.
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

} // namespace tools::intrinsic
