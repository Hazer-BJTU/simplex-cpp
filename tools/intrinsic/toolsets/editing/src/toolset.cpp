#include "tools/intrinsic/editing/toolset.hpp"
#include "tools/intrinsic/editing/schemas.hpp"
#include "tools/intrinsic/editing/tools.hpp"

#include <memory>

namespace tools::intrinsic {

EditingToolSet::EditingToolSet()
{
    register_tools({std::make_shared<StrReplaceEditTool>()});
    declare_capability_group("editing", {"str_replace_edit"});
    load_skill(editing::schema_directory() / "skill.yaml");
}

std::string_view EditingToolSet::name() const noexcept
{
    return "editing";
}

} // namespace tools::intrinsic
