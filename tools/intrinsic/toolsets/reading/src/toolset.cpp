#include "tools/intrinsic/reading/toolset.hpp"
#include "tools/intrinsic/reading/schemas.hpp"
#include "tools/intrinsic/reading/tools.hpp"

#include <memory>

namespace tools::intrinsic {

ReadingToolSet::ReadingToolSet(std::shared_ptr<FileWork> work)
{
    register_tools({std::make_shared<ReadTextTool>(std::move(work))});
    declare_capability_group("reading", {"read_text"});
    load_skill(reading::schema_directory() / "skill.yaml");
}

std::string_view ReadingToolSet::name() const noexcept
{
    return "reading";
}

} // namespace tools::intrinsic
