#pragma once

#include "tools/intrinsic/toolset_base.hpp"
#include "tools/intrinsic/file_work.hpp"

namespace tools::intrinsic {

/** One built-in editing family, distinct from reading and process tools. */
class EditingToolSet final : public IntrinsicToolSet {
public:
    explicit EditingToolSet(std::shared_ptr<FileWork> work = std::make_shared<FileWork>());
    std::string_view name() const noexcept override;
};

} // namespace tools::intrinsic
