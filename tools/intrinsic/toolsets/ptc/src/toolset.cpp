#include "tools/intrinsic/ptc/toolset.hpp"
#include "tools/intrinsic/ptc/schemas.hpp"

namespace tools::intrinsic {
PtcTool::PtcTool(std::shared_ptr<PtcRuntime> runtime, eventbus::AsyncEventBus* bus)
    : DeclaredTool(ptc::schema_directory() / "ptc_execute.yaml", bus),
      runtime_(std::move(runtime)) {}
void PtcTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    const auto source = require_string(query, "source", "Python source");
    if (source.size() > 128 * 1024) bad_argument("source exceeds 128 KiB");
}
void PtcTool::write_attributes(model_io::InvokeQuery& query) const
{
    query.type = model_io::InvokeType::SerialWrite;
    query.security = model_io::InvokeSecurity::RequireConfirm;
}
boost::asio::awaitable<model_io::Content> PtcTool::invoke(const model_io::InvokeQuery& query)
{
    co_return co_await runtime_->execute(require_string(query, "source", "Python source"));
}
PtcToolSet::PtcToolSet(std::shared_ptr<PtcRuntime> runtime, eventbus::AsyncEventBus* bus)
{
    register_tools({std::make_shared<PtcTool>(std::move(runtime), bus)});
    declare_capability_group("ptc", {"ptc_execute"});
    load_skill(ptc::schema_directory() / "skill.yaml");
}
}
