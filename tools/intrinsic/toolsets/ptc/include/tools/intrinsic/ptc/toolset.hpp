#pragma once
#include "tools/intrinsic/toolset_base.hpp"
#include "tools/intrinsic/tool_base.hpp"
#include "tools/intrinsic/process/session_store.hpp"
#include <filesystem>
#include <functional>
#include <stop_token>

namespace tools::intrinsic {

/** Startup-only interpreter settings. Arguments are argv, never shell source. */
struct PtcOptions {
    std::string executable;
    std::vector<std::string> arguments;
    std::filesystem::path directory;
    std::filesystem::path workspace;
};

/** Validate an execution envelope and render only useful model-facing fields.
 * Unknown fields and arbitrary JSON result values are accepted. Identity,
 * commit semantics, and process exit status must agree. Never replays source. */
model_io::Content render_ptc_response(const nlohmann::json& response, int exit_code,
    const std::string& execution_id, const std::string& epoch);

/** One worker-owned session. All methods run serially on the application strand.
 * initialize() probes before admission; shutdown() stops the daemon before its
 * directory lease is released. stop tokens cancel a CLI cooperatively, allowing
 * its existing identity-correlated cancellation path to settle the receipt.
 */
class PtcRuntime {
public:
    PtcRuntime(boost::asio::any_io_executor executor, PtcOptions options,
        std::function<std::stop_token()> stop);
    boost::asio::awaitable<void> initialize();
    boost::asio::awaitable<model_io::Content> execute(std::string source);
    boost::asio::awaitable<void> shutdown();
private:
    struct Reply { nlohmann::json data; int code; };
    boost::asio::awaitable<Reply> call(std::vector<std::string> tail,
        std::string input = {}, std::stop_token stop = {}, bool execution = false);
    /** Refresh the epoch without evaluating source or acknowledging recovery.
     * Used at startup and after a verified refusal to admit an execution. */
    boost::asio::awaitable<void> inspect_session();
    PtcOptions options_;
    std::shared_ptr<ProcessSessionStore> processes_;
    std::function<std::stop_token()> stop_;
    std::string epoch_;
    bool initialized_ = false;
    bool touched_ = false;
};

/** Serial, program-level confirmed execution; fine-grained approvals are not
 * implemented. Only source is model-controlled; session and modules are not. */
class PtcTool final : public DeclaredTool {
public:
    PtcTool(std::shared_ptr<PtcRuntime> runtime, eventbus::AsyncEventBus* bus);
    void ensure_arguments(model_io::InvokeQuery& query) const override;
    void write_attributes(model_io::InvokeQuery& query) const override;
    boost::asio::awaitable<model_io::Content> invoke(const model_io::InvokeQuery& query) override;
private:
    std::shared_ptr<PtcRuntime> runtime_;
};

/** Opt-in capability; native process/file tools remain available. */
class PtcToolSet final : public IntrinsicToolSet {
public:
    PtcToolSet(std::shared_ptr<PtcRuntime> runtime, eventbus::AsyncEventBus* bus);
    std::string_view name() const noexcept override { return "ptc"; }
};
}
