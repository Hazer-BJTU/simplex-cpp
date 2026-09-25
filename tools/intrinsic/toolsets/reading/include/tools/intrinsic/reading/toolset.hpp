#pragma once

#include "tools/intrinsic/toolset_base.hpp"

namespace tools::intrinsic {

/**
 * Built-in reading capabilities, currently read_text only. Future multimodal
 * readers belong to this family; editing remains a separate toolset. Owns no
 * sessions, registers declarations at construction, and carries YAML guidance
 * consumed by ToolRegistry::inject_skills().
 */
class ReadingToolSet final : public IntrinsicToolSet {
public:
    /// Build the tool catalogue and load the optional model-facing skill.
    ReadingToolSet();
    std::string_view name() const noexcept override;
};

} // namespace tools::intrinsic
