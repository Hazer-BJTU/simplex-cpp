#include "tools/intrinsic/hub_remote_call/tools.hpp"
#include "tools/intrinsic/hub_remote_call/schemas.hpp"
#include "tools/intrinsic/tool_result.hpp"
#include <boost/asio/this_coro.hpp>

namespace tools::intrinsic {

PlanTool::PlanTool(endpoint::ResolvedEndpoint endpoint, std::chrono::milliseconds timeout,
    HubRemoteCallIdentityProvider identity)
    : HubRemoteCallToolBase(hub_remote_call::schema_directory() / "plan.yaml",
        std::move(endpoint), timeout), identity_(std::move(identity))
{
    if (!identity_) throw std::invalid_argument("plan requires a host identity provider");
}

void PlanTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    if (!query.arguments.is_object()) bad_argument("arguments must be an object");
    for (const auto& [key, value] : query.arguments.items()) {
        if (key != "operation" && key != "markdown") bad_argument("unknown plan argument: " + key);
    }
    const auto operation = require_string(query, "operation", "read or replace");
    if (operation == "read") {
        if (query.arguments.contains("markdown")) bad_argument("read does not accept markdown");
    } else if (operation == "replace") {
        if (!query.arguments.contains("markdown") || !query.arguments.at("markdown").is_string()) {
            bad_argument("replace requires a markdown string; use an empty string to clear");
        }
        if (query.arguments.at("markdown").get_ref<const std::string&>().size() > 64 * 1024) {
            bad_argument("markdown exceeds 64 KiB");
        }
        // dump rejects invalid UTF-8 before any request is sent.
        try { (void)query.arguments.dump(); }
        catch (const std::exception&) { bad_argument("markdown must be valid UTF-8"); }
    } else {
        bad_argument("operation must be read or replace");
    }
}

void PlanTool::write_attributes(model_io::InvokeQuery& query) const
{
    query.type = query.arguments.at("operation") == "read"
        ? model_io::InvokeType::ReadOnly : model_io::InvokeType::SerialWrite;
    query.security = model_io::InvokeSecurity::Trusted;
}

boost::asio::awaitable<model_io::Content> PlanTool::invoke(const model_io::InvokeQuery& query)
{
    co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
    try {
        const auto identity = identity_();
        const auto operation = query.arguments.at("operation").get<std::string>();
        const auto reply = co_await request(query, "plan/" + operation,
            identity.worker_id, identity.session_id, identity.run_id);
        if (reply.at("status") == "rejected") {
            throw std::runtime_error("hub rejected plan request ("
                + reply.at("error").at("code").get<std::string>() + "): "
                + reply.at("error").at("message").get<std::string>());
        }
        const auto& result = reply.at("result");
        if (!result.at("revision").is_number_unsigned()
            || !(result.at("updated_at").is_null() || result.at("updated_at").is_string())) {
            throw std::runtime_error("invalid plan result");
        }
        ToolResult output;
        output.field("revision", result.at("revision"));
        if (operation == "read") {
            const auto text = result.at("markdown").get<std::string>();
            if (text.size() > 64 * 1024) throw std::runtime_error("plan result exceeds 64 KiB");
            output.block("plan", text);
        } else {
            const bool changed = result.at("changed").get<bool>();
            output.field("status", changed ? "plan updated" : "plan unchanged");
        }
        co_return output.render();
    } catch (const std::exception& error) {
        invoke_failed(std::string("plan: ") + error.what());
    }
}
} // namespace tools::intrinsic
