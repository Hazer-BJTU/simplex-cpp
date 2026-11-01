#include "tools/intrinsic/ptc/toolset.hpp"
#include "tools/intrinsic/tool_result.hpp"
#include <boost/asio/this_coro.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <stdexcept>

namespace tools::intrinsic {
namespace asio = boost::asio;
using Json = nlohmann::json;
namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
bool string_field(const Json& value, const char* key)
{
    return value.contains(key) && value[key].is_string();
}
}

/** Unknown outcomes deliberately use a separate diagnostic rather than a fake
 * committed=false response. A transport failure cannot establish rollback. */
model_io::Content render_ptc_response(const Json& r, int code,
    const std::string& id, const std::string& epoch)
{
    require(r.is_object() && r.contains("protocol") && r["protocol"].is_number_integer()
        && r["protocol"] == 1, "unsupported PTC protocol");
    require(string_field(r, "status"), "missing PTC status");
    const auto status = r["status"].get<std::string>();
    require(status == "completed" || status == "failed", "invalid PTC status");
    const bool unknown = r.value("commit_status", std::string{}) == "unknown";
    if (r.contains("commit_status")) require(unknown, "invalid PTC commit_status");
    if (unknown) {
        require(!r.contains("committed"), "ambiguous PTC commit status");
        require(status == "failed" && code == 2, "inconsistent PTC boundary failure");
        if (r.contains("execution_id")) require(r["execution_id"] == id, "PTC identity mismatch");
        if (r.contains("epoch")) require(r["epoch"] == epoch, "PTC epoch mismatch");
    } else {
        require(r.value("execution_id", std::string{}) == id, "PTC identity mismatch");
        require(string_field(r, "epoch") && !r["epoch"].get_ref<const std::string&>().empty()
            && (epoch.empty() || r["epoch"] == epoch), "PTC epoch mismatch");
        require(string_field(r, "session_id") && r.contains("revision")
            && r["revision"].is_number_unsigned(), "invalid PTC session revision");
        require(r.contains("committed") && r["committed"].is_boolean(), "missing PTC commit status");
        require(r["committed"] == (status == "completed")
            && code == (status == "completed" ? 0 : 1), "inconsistent PTC completion");
        require(string_field(r, "output") && r.contains("result")
            && r.contains("output_omitted_bytes") && r["output_omitted_bytes"].is_number_unsigned(),
            "invalid PTC result envelope");
    }
    ToolResult out;
    out.field("status", status);
    if (status == "failed") {
        require(r.contains("error") && r["error"].is_object()
            && string_field(r["error"], "code") && string_field(r["error"], "message"),
            "invalid PTC error");
        out.field("error", r["error"]["code"].get<std::string>() + ": "
            + r["error"]["message"].get<std::string>());
        out.field(unknown ? "commit_status" : "committed", unknown ? Json("unknown") : Json(false));
        out.field("execution_id", id);
        out.field("hints", "External effects may remain. Inspect before retrying; do not replay source.");
    }
    if (r.contains("commands_created")) {
        require(r["commands_created"].is_array(), "invalid PTC command identities");
        for (const auto& item : r["commands_created"]) require(item.is_string(), "invalid PTC command ID");
        out.field("commands_created", r["commands_created"]);
    }
    if (r.contains("output_omitted_bytes")) {
        require(r["output_omitted_bytes"].is_number_unsigned(), "invalid PTC output omission count");
        if (r["output_omitted_bytes"] != 0) out.field("output_omitted_bytes", r["output_omitted_bytes"]);
    }
    if (r.contains("output")) {
        require(r["output"].is_string(), "invalid PTC output");
        if (!r["output"].get_ref<const std::string&>().empty()) out.block("output", r["output"]);
    }
    if (r.contains("result_truncated")) require(r["result_truncated"].is_boolean(), "invalid PTC truncation flag");
    if (r.value("result_truncated", false)) {
        require(string_field(r, "result_preview") && r.contains("result") && r["result"].is_null(),
            "invalid PTC result preview");
        out.field("hints", "Result is a truncated preview; return a smaller selected result.");
        out.block("result_preview", r["result_preview"], true);
    } else if (r.contains("result") && !r["result"].is_null()) {
        out.block("result", r["result"].is_string()
            ? r["result"].get<std::string>() : r["result"].dump());
    }
    auto content = out.render();
    Json receipt = {{"execution_id", id}};
    for (const auto* key : {"session_id", "epoch", "revision", "committed", "commit_status"}) {
        if (r.contains(key)) receipt[key] = r[key];
    }
    content.extras = Json{{"simplex.ptc", std::move(receipt)}};
    return content;
}

PtcRuntime::PtcRuntime(asio::any_io_executor executor, PtcOptions options,
    std::function<std::stop_token()> stop)
    : options_(std::move(options)), processes_(std::make_shared<ProcessSessionStore>(executor)),
      stop_(std::move(stop))
{
    if (options_.executable.empty() || options_.directory.empty() || !stop_) {
        throw std::invalid_argument("PTC needs an executable, state directory and stop-token provider");
    }
}

asio::awaitable<PtcRuntime::Reply> PtcRuntime::call(std::vector<std::string> tail,
    std::string input, std::stop_token stop, bool execution)
{
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    process::LaunchSpec spec;
    spec.executable = options_.executable;
    spec.arguments = options_.arguments;
    spec.arguments.insert(spec.arguments.end(), tail.begin(), tail.end());
    // A dedicated transport budget, not model output capture. Both streams
    // remain drained; any capture loss makes the envelope unusable.
    spec.max_output_bytes = 256 * 1024;
    if (!options_.workspace.empty()) spec.working_directory = options_.workspace.string();
    const auto child = co_await processes_->spawn(std::move(spec));
    std::exception_ptr failure;
    Reply reply;
    try {
        const bool has_request = !input.empty();
        auto accepted = co_await processes_->send_input(child.id, std::move(input), true);
        require(accepted && (!accepted->input || *accepted->input == process_io::InputResult::Accepted)
            && (!has_request || (accepted->end && *accepted->end == process_io::InputResult::Accepted)),
            "PTC stdin admission failed");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        bool cancelled = false;
        bool exited = false;
        for (;;) {
            const auto state = co_await processes_->snapshot(child.id);
            require(state.has_value(), "PTC client disappeared");
            require(state->io_error.empty(), "PTC client IO failed");
            if (state->exited && state->output_drained) break;
            if (state->exited && !exited) {
                exited = true;
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
            if (execution && stop.stop_requested() && !cancelled) {
                // SIGTERM unwinds Client.execute into its correlated cancel/query.
                co_await processes_->terminate(child.id, true);
                cancelled = true;
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            }
            if ((!execution || cancelled || exited) && std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("PTC client deadline exceeded; effects and commit unknown");
            }
            asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::milliseconds(20));
            co_await timer.async_wait(asio::use_awaitable);
        }
        auto state = co_await processes_->snapshot(child.id);
        auto data = co_await processes_->read_output(child.id, OutputStream::Both, true, false);
        require(data && !data->standard_output.truncated && !data->standard_error.truncated,
            "PTC transport capture truncated; commit unknown");
        require(data->standard_output.text.size() <= 48 * 1024 + 1, "PTC response exceeds budget");
        require(state->result.execution.exit_code.has_value(), "PTC client has no exit code");
        reply = {Json::parse(data->standard_output.text), *state->result.execution.exit_code};
        require(reply.data.is_object(), "PTC response must be an object");
    } catch (...) {
        failure = std::current_exception();
    }
    // The private store has at most one client. On failure its shutdown is
    // the join fence (a signal followed by release is not a reap guarantee).
    // Recreate only the transport store so daemon-stop remains available.
    if (!failure) {
        try {
            require(co_await processes_->release(child.id), "PTC client release failed");
        } catch (...) {
            failure = std::current_exception();
        }
    }
    if (failure) {
        try { co_await processes_->shutdown(); } catch (...) {}
        processes_ = std::make_shared<ProcessSessionStore>(co_await asio::this_coro::executor);
        std::rethrow_exception(failure);
    }
    co_return reply;
}

asio::awaitable<void> PtcRuntime::initialize()
{
    std::vector<std::string> probe_args{"capabilities"};
    auto capabilities = co_await call(std::move(probe_args));
    if (capabilities.code != 0) {
        throw std::runtime_error("PTC capability probe failed: "
            + capabilities.data.value("error", Json::object()).value("message", "interpreter rejected probe"));
    }
    require(capabilities.data.value("protocol", 0) == 1
        && capabilities.data.contains("tools") && capabilities.data["tools"].is_array(),
        "PTC capability probe failed");
    // Do not stop an already-running, incompatible daemon merely because a
    // changed operator configuration fails inspection. A newly started daemon
    // remains ours to clean up even when its initial inspection fails.
    std::vector<std::string> status_args{"daemon", "status", "--state-directory", options_.directory.string()};
    auto previous = co_await call(std::move(status_args));
    require(previous.code == 0, "PTC daemon status probe failed");
    touched_ = previous.data.contains("running") && previous.data["running"] == false;
    std::vector<std::string> inspect_args{"session", "inspect", "--state-directory", options_.directory.string()};
    auto state = co_await call(std::move(inspect_args));
    if (state.code != 0) {
        throw std::runtime_error("PTC session inspection failed: "
            + state.data.value("error", Json::object()).value("message", "interpreter rejected inspection"));
    }
    require(state.data.contains("epoch")
        && (state.data["epoch"].is_null() || state.data["epoch"].is_string()),
        "PTC session inspection failed");
    touched_ = true;
    require(!state.data.value("recovery_required", true), "PTC session requires explicit recovery");
    if (state.data["epoch"].is_string()) epoch_ = state.data["epoch"];
    initialized_ = true;
}

asio::awaitable<model_io::Content> PtcRuntime::execute(std::string source)
{
    const auto id = boost::uuids::to_string(boost::uuids::random_generator()());
    const auto stop = stop_();
    if (stop.stop_requested()) {
        co_return ToolResult{}.field("status", "cancelled")
            .field("hints", "Cancelled before execution; source was not submitted.").render();
    }
    try {
        require(initialized_, "PTC runtime is not initialized");
        Json request{{"execution_id", id}, {"source", std::move(source)}};
        if (!epoch_.empty()) request["epoch"] = epoch_;
        auto input = request.dump(-1, ' ', true);
        require(input.size() <= 1024 * 1024, "PTC request exceeds byte budget");
        std::vector<std::string> execute_args{"execute", "--state-directory", options_.directory.string()};
        auto reply = co_await call(std::move(execute_args),
            std::move(input), stop, true);
        auto result = render_ptc_response(reply.data, reply.code, id, epoch_);
        if (reply.code == 2) initialized_ = false;
        else epoch_ = reply.data["epoch"].get<std::string>();
        co_return result;
    } catch (const std::exception& error) {
        initialized_ = false;
        auto content = ToolResult{}.field("status", "failed").field("commit_status", "unknown")
            .field("execution_id", id).field("error", error.what())
            .field("hints", "Inspect the PTC session before retrying; never replay uncertain source.").render();
        content.extras = Json{{"simplex.ptc", {{"execution_id", id}, {"commit_status", "unknown"}}}};
        co_return content;
    }
}

asio::awaitable<void> PtcRuntime::shutdown()
{
    initialized_ = false;
    std::exception_ptr failure;
    if (touched_) {
        try {
            std::vector<std::string> stop_args{"daemon", "stop", "--state-directory", options_.directory.string()};
            auto reply = co_await call(std::move(stop_args));
            require(reply.code == 0 && (reply.data.value("stopped", false)
                || (reply.data.contains("running") && reply.data["running"] == false)),
                "PTC daemon shutdown failed; retain its state directory");
            touched_ = false;
        } catch (...) { failure = std::current_exception(); }
    }
    try { co_await processes_->shutdown(); }
    catch (...) { if (!failure) failure = std::current_exception(); }
    if (failure) std::rethrow_exception(failure);
}
}
