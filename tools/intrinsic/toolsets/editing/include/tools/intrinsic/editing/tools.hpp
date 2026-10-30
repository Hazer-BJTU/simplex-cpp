#pragma once

#include "tools/intrinsic/tool_base.hpp"
#include "tools/intrinsic/file_work.hpp"

namespace tools::intrinsic {

/**
 * Confirmed exact replacement of one unique byte sequence in an existing file.
 *
 * Editing streams the source once, stages the result in a private candidate and
 * verifies it before publication, so the source and the result may each reach
 * the configured host budget without being buffered in memory.
 */
class StrReplaceEditTool final : public DeclaredTool {
public:
    static constexpr std::uint64_t kDefaultContextLines = 3;
    static constexpr std::uint64_t kMaxContextLines = 20;
    /// Default and largest configurable source/result size (1 GiB).
    static constexpr std::size_t kMaxFileBytes = 1024 * 1024 * 1024;
    static constexpr std::size_t kMaxConfiguredFileBytes = 1024 * 1024 * 1024;

    explicit StrReplaceEditTool(std::shared_ptr<FileWork> work = std::make_shared<FileWork>());

    /// Materialize context default and validate all arguments before confirmation.
    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    /// Await blocking editing after the security gate. Pre-publication stop
    /// discards the candidate; past the final commit checkpoint, publication
    /// and any durability failure are reported normally. Always joins the job.
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;

private:
    std::shared_ptr<FileWork> work_;
    std::size_t max_file_bytes_ = kMaxFileBytes;
};

} // namespace tools::intrinsic
