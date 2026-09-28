#include "tools/intrinsic/modality_assist/toolset.hpp"
#include "tools/intrinsic/modality_assist/tools.hpp"
#include "tools/intrinsic/modality_assist/schemas.hpp"

namespace tools::intrinsic {

ModalityAssistToolSet::ModalityAssistToolSet(std::shared_ptr<llm::LLMModel> model)
{
    register_tools({std::make_shared<ModalityAssistTool>(std::move(model))});
    declare_capability_group("modality_assist", {"modality_assist"});
    load_skill(modality_assist::schema_directory() / "skill.yaml");
}

std::string_view ModalityAssistToolSet::name() const noexcept
{
    return "modality_assist";
}

} // namespace tools::intrinsic
