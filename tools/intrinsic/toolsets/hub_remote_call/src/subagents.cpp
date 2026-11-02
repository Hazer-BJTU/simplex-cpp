#include "tools/intrinsic/hub_remote_call/subagents.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <boost/asio/this_coro.hpp>

#include "tools/intrinsic/hub_remote_call/schemas.hpp"
#include "tools/intrinsic/tool_result.hpp"

namespace tools::intrinsic {
namespace {
using Json = nlohmann::json;
constexpr std::size_t argument_bytes = 64 * 1024;

/** These failures describe constraints only; never attach received values. */
void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

bool one_of(const Json& value, std::initializer_list<std::string_view> names)
{
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    return std::find(names.begin(), names.end(), text) != names.end();
}

bool identifier(const Json& value)
{
    return value.is_string() && !value.get_ref<const std::string&>().empty()
        && value.get_ref<const std::string&>().size() <= 128
        && value.get_ref<const std::string&>().find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")
            == std::string::npos;
}

void exact(const Json& object, std::initializer_list<std::string_view> keys)
{
    require(object.is_object(), "arguments must be an object");
    for (const auto& [key, value] : object.items()) {
        require(std::find(keys.begin(), keys.end(), key) != keys.end(),
            "unknown or forbidden argument");
    }
}

void argument_budget(const Json& arguments)
{
    // Strict dump also validates UTF-8 in request text and nested provider options.
    std::string encoded;
    try {
        encoded = arguments.dump();
    } catch (const std::exception&) {
        throw std::invalid_argument("arguments must contain valid UTF-8 JSON");
    }
    require(encoded.size() <= argument_bytes, "arguments exceed 64 KiB serialized UTF-8");
}

/** Convert the text-only tool arguments to the existing Hub payload contract. */
Json send_arguments(Json arguments)
{
    if (arguments.at("operation") == "message") {
        Json text = std::move(arguments.at("request"));
        arguments.erase("request");
        arguments["content"] = Json::array({{
            {"type", "text"}, {"modality", "text"}, {"raw", std::move(text)}
        }});
    }
    return arguments;
}

void validate_options(const Json& options)
{
    exact(options, {"model", "tools"});
    for (const auto& [category, values] : options.items()) {
        require(values.is_object(), "each options category must be an object");
        require(category != "tools" || values.empty(), "options.tools is reserved and must be empty");
    }
}

/** Fork/send acknowledgements contain no conversation or model-facing cursors. */
model_io::Content format_result(const std::string& route, const Json& value, const Json& arguments)
{
    ToolResult result;
    if (route == "subagent/clean-fork") {
        require(identifier(value.at("subagent_id"))
                && value.at("lifecycle") == "preparing", "invalid fork result");
        result.field("subagent_id", value.at("subagent_id"));
        result.field("lifecycle", value.at("lifecycle"));
        result.field("hint", "Receive until ready and connected before sending a task.");
    } else if (route == "subagent/send") {
        require(value.at("subagent_id") == arguments.at("subagent_id")
                && value.at("operation") == arguments.at("operation"), "mismatched send result");
        result.field("subagent_id", value.at("subagent_id"));
        result.field("operation", value.at("operation"));
        if (arguments.at("operation") == "stop") {
            require(identifier(value.at("operation_id")) && value.at("state") == "stopping",
                "invalid stop result");
            result.field("operation_id", value.at("operation_id"));
            result.field("hint", "Shutdown requested; persistence is removed after confirmed cleanup.");
        } else {
            require(identifier(value.at("request_id"))
                    && one_of(value.at("state"), {"sent", "rejected", "unknown"}),
                "invalid dispatch result");
            result.field("request_id", value.at("request_id"));
            result.field("hint", value.at("state") == "rejected"
                ? "Payload was not enqueued. Inspect child status before another attempt."
                : "Dispatch is not task completion. Receive the correlated request outcome; do not resend uncertain work.");
        }
        result.field("state", value.at("state"));
    } else {
        throw std::invalid_argument("unexpected mutation route");
    }
    if (value.contains("replayed")) {
        require(value.at("replayed").is_boolean(), "invalid receipt replay flag");
        result.field("replayed", value.at("replayed"));
    }
    return result.render();
}

/** Only protocol codes, never arbitrary Hub message bodies, enter exceptions. */
std::string rejection_hint(const Json& error, bool mutation)
{
    const auto& code = error.at("code");
    const bool known = one_of(code, {"unauthorized", "invalid_arguments", "policy_forbidden",
        "unsupported_launch", "unsupported_operation", "lifecycle_closed", "disconnected",
        "limit_exceeded", "request_conflict", "recovery_required", "delivery_unknown",
        "storage_error", "answer_unavailable", "result_too_large", "not_implemented", "invalid_parent"});
    const auto name = known ? code.get<std::string>() : std::string("remote_rejection");
    std::string hint = mutation
        ? "inspect subagent_receive before repeating a mutation"
        : "check child status before retrying this read";
    if (name == "not_implemented") {
        hint = "this Hub does not support subagents; use a compatible Hub";
    } else if (name == "unsupported_launch") {
        hint = "fork requires a supported Hub-supervised startup configuration";
    } else if (name == "limit_exceeded") {
        hint = "capacity/depth limit reached; inspect children and stop unneeded work after collecting results";
    } else if (name == "disconnected") {
        hint = "child is disconnected; inspect readiness before sending";
    } else if (name == "unauthorized" || name == "lifecycle_closed") {
        hint = "only live direct children of this worker lifecycle may be controlled";
    } else if (name == "invalid_arguments" || name == "policy_forbidden") {
        hint = "check operation arguments; child confirmation policy is operator-owned";
    } else if (name == "answer_unavailable") {
        hint = "refresh receive and copy a current answer_source, including its fingerprint";
    } else if (name == "result_too_large" && !mutation) {
        hint = "retry receive with a smaller limit or use exact answer pages";
    }
    return "hub rejected subagent request (" + name + "); " + hint;
}
} // namespace

SubagentToolBase::SubagentToolBase(
    std::string_view declaration,
    std::string route,
    endpoint::ResolvedEndpoint endpoint,
    std::chrono::milliseconds timeout,
    HubRemoteCallIdentityProvider identity)
    : HubRemoteCallToolBase(hub_remote_call::schema_directory() / declaration,
          std::move(endpoint), timeout),
      route_(std::move(route)),
      identity_(std::move(identity))
{
    if (!identity_) {
        throw std::invalid_argument("subagent tool requires a host identity provider");
    }
}

void SubagentToolBase::write_attributes(model_io::InvokeQuery& query) const
{
    query.type = route_ == "subagent/receive"
        ? model_io::InvokeType::ReadOnly : model_io::InvokeType::SerialWrite;
    query.security = model_io::InvokeSecurity::Trusted;
}

boost::asio::awaitable<model_io::Content> SubagentToolBase::invoke(
    const model_io::InvokeQuery& query)
{
    co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
    Json reply;
    const bool mutation = route_ != "subagent/receive";
    try {
        const auto identity = identity_();
        // Keep the recorded model call unchanged. Only the outgoing message
        // uses the Hub's content array; other routes retain their arguments.
        auto remote_query = query;
        if (route_ == "subagent/send") {
            remote_query.arguments = send_arguments(std::move(remote_query.arguments));
        }
        reply = co_await request(std::move(remote_query), route_,
            identity.worker_id, identity.session_id, identity.run_id);
    } catch (const tools::InvokeException& error) {
        // request() exposes only sanitized transport/protocol diagnostics.
        // Preserve those for reads; lost write replies still require inspection.
        if (!mutation) {
            invoke_failed(error.message());
        }
        invoke_failed("subagent exchange failed; a mutation may have committed; inspect subagent_receive before repeating");
    } catch (const std::exception&) {
        invoke_failed(mutation
            ? "subagent exchange failed; a mutation may have committed; inspect subagent_receive before repeating"
            : "subagent read exchange failed; check the connection before retrying this read");
    }
    if (reply.at("status") == "rejected") {
        invoke_failed(rejection_hint(reply.at("error"), mutation));
    }
    try {
        co_return format_result(route_, reply.at("result"), query.arguments);
    } catch (const std::exception&) {
        invoke_failed(mutation
            ? "invalid subagent result; a mutation may have committed; inspect subagent_receive before repeating"
            : "invalid Hub subagent result; check the receive protocol before retrying this read");
    }
}

SubagentForkTool::SubagentForkTool(endpoint::ResolvedEndpoint endpoint,
    std::chrono::milliseconds timeout, HubRemoteCallIdentityProvider identity)
    : SubagentToolBase("subagent_fork.yaml", "subagent/clean-fork",
          std::move(endpoint), timeout, std::move(identity))
{
}

void SubagentForkTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    try {
        exact(query.arguments, {});
        argument_budget(query.arguments);
    } catch (const std::exception& error) {
        bad_argument(error.what());
    }
}

SubagentSendTool::SubagentSendTool(endpoint::ResolvedEndpoint endpoint,
    std::chrono::milliseconds timeout, HubRemoteCallIdentityProvider identity)
    : SubagentToolBase("subagent_send.yaml", "subagent/send",
          std::move(endpoint), timeout, std::move(identity))
{
}

void SubagentSendTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    try {
        exact(query.arguments, {"subagent_id", "operation", "request", "options"});
        const auto& arguments = query.arguments;
        require(identifier(arguments.value("subagent_id", Json())), "subagent_id must be a valid session ID");
        require(one_of(arguments.value("operation", Json()), {"message", "continue", "compact", "stop"}),
            "operation must be message, continue, compact or stop");
        if (arguments.at("operation") == "message") {
            require(arguments.contains("request") && arguments.at("request").is_string()
                    && !arguments.at("request").get_ref<const std::string&>().empty(),
                "message requires a nonempty text request");
        } else {
            require(!arguments.contains("request"), "only message accepts request");
        }
        if (arguments.at("operation") == "stop") {
            require(!arguments.contains("options"), "stop does not accept options");
        } else if (arguments.contains("options")) {
            validate_options(arguments.at("options"));
        }
        argument_budget(arguments);
        // The Hub enforces the same limit after adding the text-part wrapper.
        // Reject locally rather than send an oversized remote argument object.
        argument_budget(send_arguments(arguments));
    } catch (const std::exception& error) {
        bad_argument(error.what());
    }
}

SubagentReceiveTool::SubagentReceiveTool(endpoint::ResolvedEndpoint endpoint,
    std::chrono::milliseconds timeout, HubRemoteCallIdentityProvider identity)
    : SubagentToolBase("subagent_receive.yaml", "subagent/receive",
          std::move(endpoint), timeout, std::move(identity))
{
}

} // namespace tools::intrinsic
