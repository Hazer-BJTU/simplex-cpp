#pragma once

#include "tools/intrinsic/tool_base.hpp"

namespace tools::intrinsic {

/** Confirmed exact replacement of one unique byte sequence in an existing file. */
class StrReplaceEditTool final : public DeclaredTool {
public:
    static constexpr std::uint64_t kDefaultContextLines = 3;
    static constexpr std::uint64_t kMaxContextLines = 20;

    StrReplaceEditTool();

    /// Materialize context default and validate all arguments before confirmation.
    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    /// Synchronous editing after the security gate; no coroutine suspension here.
    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override;
};

} // namespace tools::intrinsic
