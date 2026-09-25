#pragma once

#include "tools/intrinsic/toolset_base.hpp"

namespace tools::intrinsic {

/** One built-in editing family, distinct from reading and process tools. */
class EditingToolSet final : public IntrinsicToolSet {
public:
    EditingToolSet();
    std::string_view name() const noexcept override;
};

} // namespace tools::intrinsic
