#pragma once

#include "tools/intrinsic/toolset_base.hpp"
#include "llm/models.hpp"

namespace tools::intrinsic {

/**
 * Dependency-injected intrinsic capability. Core registers this set only when
 * modality_assist_model is configured and constructed. The set's tool retains
 * shared model ownership; the set does not discover providers or own session
 * history. Its YAML declaration and skill follow the standard intrinsic rules.
 */
class ModalityAssistToolSet final : public IntrinsicToolSet {
public:
    explicit ModalityAssistToolSet(std::shared_ptr<llm::LLMModel> model);
    std::string_view name() const noexcept override;
};

} // namespace tools::intrinsic
