#include "tools/intrinsic/hub_remote_call/subagents.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <boost/asio/this_coro.hpp>

#include "tools/intrinsic/hub_remote_call/schemas.hpp"
#include "tools/intrinsic/tool_result.hpp"

namespace tools::intrinsic {
namespace {
using Json = nlohmann::json;
constexpr std::uint64_t max_safe_integer = 9007199254740991ULL;
constexpr std::size_t argument_bytes = 64 * 1024;
constexpr std::size_t result_bytes = 256 * 1024;

/** An internal presentation limit, distinct from an invalid Hub reply. */
struct RenderingBudgetExceeded : std::runtime_error {
    RenderingBudgetExceeded() : std::runtime_error("rendered subagent result exceeds budget") {}
};

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

bool index(const Json& value)
{
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() <= max_safe_integer;
    }
    return value.is_number_integer() && value.get<std::int64_t>() >= 0
        && static_cast<std::uint64_t>(value.get<std::int64_t>()) <= max_safe_integer;
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
    // Strict dump also validates UTF-8 in nested extras/provider options.
    std::string encoded;
    try {
        encoded = arguments.dump();
    } catch (const std::exception&) {
        throw std::invalid_argument("arguments must contain valid UTF-8 JSON");
    }
    require(encoded.size() <= argument_bytes, "arguments exceed 64 KiB serialized UTF-8");
}

void validate_content(const Json& parts)
{
    require(parts.is_array() && !parts.empty(), "message requires a nonempty content array");
    for (const auto& part : parts) {
        exact(part, {"type", "modality", "raw", "extras"});
        require(one_of(part.value("type", Json()), {"text", "binary", "external_ref"}),
            "content type must be text, binary or external_ref");
        require(one_of(part.value("modality", Json()),
                {"text", "image", "audio", "video", "document"}),
            "content requires a supported modality");
        require(part.contains("raw") && part.at("raw").is_string()
                && !part.at("raw").get_ref<const std::string&>().empty(),
            "content raw must be a nonempty string");
        require(!part.contains("extras") || part.at("extras").is_object(),
            "content extras must be an object");
    }
}

void validate_options(const Json& options)
{
    exact(options, {"model", "tools"});
    for (const auto& [category, values] : options.items()) {
        require(values.is_object(), "each options category must be an object");
        require(category != "tools" || values.empty(), "options.tools is reserved and must be empty");
    }
}

/** UTF-8-safe prefix length; the parsed input has already been validated. */
std::size_t prefix_length(const std::string& text, std::size_t bytes)
{
    auto end = std::min(bytes, text.size());
    while (end > 0 && end < text.size()
            && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) {
        --end;
    }
    return end;
}

std::string prefix(const std::string& text, std::size_t bytes)
{
    return text.substr(0, prefix_length(text, bytes));
}

/**
 * Metadata is whitelisted and small; user/assistant/summary bodies share a
 * preallocated budget of at most 96 KiB. Latest assistant outputs and compact
 * summaries get space before older content, without changing display order.
 * The parsed reply owns the strings for the lifetime of this local presentation;
 * the pointer keys avoid copying large bodies just to plan their allocation.
 * The complete document includes actual labels and clipping annotations in its
 * 256 KiB check. Body allowances shrink only after a presentation exceeds it.
 */
struct Presentation {
    ToolResult output;
    std::unordered_map<const std::string*, std::size_t> body_limits;
    bool clipped = false;
    std::size_t body_budget = 96 * 1024;

    /**
     * Share the budget among result bodies first. Allocate short results whole,
     * then share what remains among larger ones, so one long answer or summary
     * cannot starve the other results. Older assistant/user bodies use only the
     * remaining space. All allocations end at UTF-8 boundaries.
     */
    void allocate_bodies(
        std::vector<const std::string*> results,
        const std::vector<const std::string*>& context)
    {
        std::size_t remaining = body_budget;
        std::stable_sort(results.begin(), results.end(), [](const auto* left, const auto* right) {
            return left->size() < right->size();
        });
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto* text = results[i];
            const auto allowance = remaining / (results.size() - i);
            const auto bytes = prefix_length(*text, allowance);
            body_limits.emplace(text, bytes);
            remaining -= bytes;
        }
        for (const auto* text : context) {
            const auto bytes = prefix_length(*text, remaining);
            body_limits.emplace(text, bytes);
            remaining -= bytes;
        }
    }

    void field(std::string_view key, const Json& value)
    {
        // ToolResult omits null, but null has meaning for revision/freshness.
        output.field(key, value.is_null() ? Json("null") : value);
    }

    void diagnostic(std::string_view key, const Json& value)
    {
        require(value.is_null() || value.is_string(), "invalid diagnostic");
        if (value.is_string()) {
            const auto& text = value.get_ref<const std::string&>();
            clipped |= text.size() > 512;
            field(key, prefix(text, 512));
        }
    }

    void block(std::string_view label, const std::string& text)
    {
        auto body = text.substr(0, body_limits.at(&text));
        const bool truncated = body.size() != text.size();
        clipped |= truncated;
        output.block(label, std::move(body), truncated);
    }

    model_io::Content render()
    {
        output.field("output_truncated", clipped);
        if (output.text().size() > result_bytes) {
            throw RenderingBudgetExceeded();
        }
        return output.render();
    }
};

/** Collect only the body strings that the formatter will display. */
void collect_content_bodies(const Json& parts, std::vector<const std::string*>& bodies)
{
    require(parts.is_array(), "invalid visible content");
    for (const auto& part : parts) {
        bodies.push_back(&part.at("raw").get_ref<const std::string&>());
    }
}

/**
 * Pagination addresses whole turns, so clipping a final answer cannot be
 * repaired by requesting another page. Reserve each turn's latest nonempty
 * assistant step and compact summaries before intermediate steps and user input.
 * The subsequent formatter still validates all route-specific shapes and emits
 * the original indices, order and explicit per-body truncation markers.
 */
void plan_bodies(Presentation& result, const Json& page, const Json& requests)
{
    std::vector<const std::string*> answers;
    std::vector<const std::string*> intermediate;
    std::vector<const std::string*> users;
    require(requests.is_array(), "invalid request outcomes");
    for (const auto& request : requests) {
        if (request.contains("summary")) {
            answers.push_back(&request.at("summary").get_ref<const std::string&>());
        }
    }
    if (!page.is_null()) {
        const auto& turns = page.at("turns");
        require(turns.is_array(), "invalid conversation turns");
        for (auto turn = turns.rbegin(); turn != turns.rend(); ++turn) {
            const auto& steps = turn->at("steps");
            require(steps.is_array(), "invalid conversation steps");
            bool found_answer = false;
            for (auto step = steps.rbegin(); step != steps.rend(); ++step) {
                std::vector<const std::string*> bodies;
                collect_content_bodies(step->at("content"), bodies);
                const bool nonempty = std::any_of(bodies.begin(), bodies.end(), [](const auto* body) {
                    return !body->empty();
                });
                auto& destination = !found_answer && nonempty ? answers : intermediate;
                destination.insert(destination.end(), bodies.begin(), bodies.end());
                found_answer |= nonempty;
            }
            collect_content_bodies(turn->at("user"), users);
        }
    }
    intermediate.insert(intermediate.end(), users.begin(), users.end());
    result.allocate_bodies(std::move(answers), intermediate);
}

void status(Presentation& result, const Json& child)
{
    require(child.is_object() && identifier(child.at("subagent_id"))
            && identifier(child.at("parent")), "invalid child identity");
    require(one_of(child.at("lifecycle"),
            {"preparing", "starting", "ready", "stopping", "stopped", "cleanup-pending"})
            && one_of(child.at("policy"), {"ask", "deny", "approve"})
            && one_of(child.at("health"), {"healthy", "degraded", "unknown"}),
        "invalid child lifecycle or health");
    require(child.at("process_state").is_string()
            && child.at("process_state").get_ref<const std::string&>().size() <= 64
            && child.at("connected").is_boolean() && child.at("active").is_boolean()
            && index(child.at("pending_approvals")), "invalid child status");
    require(child.at("run_id").is_null() || identifier(child.at("run_id")), "invalid run identity");
    require(child.at("observed_at").is_null()
            || (child.at("observed_at").is_string()
                && child.at("observed_at").get_ref<const std::string&>().size() <= 128),
        "invalid observation timestamp");
    for (const char* key : {"subagent_id", "parent", "lifecycle", "policy", "health",
            "process_state", "connected", "active", "run_id", "pending_approvals", "observed_at"}) {
        result.field(key, child.at(key));
    }
    result.diagnostic("reason", child.at("reason"));
    result.diagnostic("error", child.at("error"));
}

void request_outcomes(Presentation& result, const Json& requests)
{
    require(requests.is_array() && requests.size() <= 20, "invalid request outcomes");
    for (const auto& request : requests) {
        require(request.is_object() && identifier(request.at("request_id"))
                && one_of(request.at("operation"), {"message", "continue", "compact"})
                && one_of(request.at("state"),
                    {"intent", "sent", "admitted", "rejected", "unknown", "finished"})
                && request.at("run_id").is_string()
                && (request.at("run_id") == "" || identifier(request.at("run_id")))
                && request.at("at").is_string()
                && request.at("at").get_ref<const std::string&>().size() <= 128,
            "invalid request outcome");
        result.output.separate();
        for (const char* key : {"request_id", "operation", "state", "run_id", "at"}) {
            result.field(key, request.at(key));
        }
        if (request.contains("status")) {
            require(one_of(request.at("status"), {"completed", "cancelled", "failed", "exchange_limit", "unknown"}),
                "invalid request run status");
            result.field("run_status", request.at("status"));
        }
        if (request.contains("detail")) {
            result.diagnostic("detail", request.at("detail"));
        }
        if (request.contains("summary")) {
            require(request.at("summary").is_string(), "invalid compact summary");
            result.block("compact summary", request.at("summary").get_ref<const std::string&>());
        }
    }
}

void content(Presentation& result, const Json& parts, const std::string& owner)
{
    require(parts.is_array() && parts.size() <= 128, "invalid visible content");
    for (const auto& part : parts) {
        require(part.is_object()
                && one_of(part.at("type"), {"text", "external_ref"})
                && one_of(part.at("modality"), {"text", "image", "audio", "video", "document"})
                && part.at("raw").is_string(), "invalid visible content part");
        const auto label = owner + " (" + part.at("type").get<std::string>()
            + "/" + part.at("modality").get<std::string>() + ")";
        result.block(label, part.at("raw").get_ref<const std::string&>());
    }
}

void conversation(Presentation& result, const Json& page, const Json& arguments)
{
    result.field("conversation_available", !page.is_null());
    if (page.is_null()) {
        return;
    }
    require(page.is_object() && (page.at("revision").is_null() || index(page.at("revision")))
            && (page.at("worker_id").is_null() || identifier(page.at("worker_id")))
            && index(page.at("cursor")) && index(page.at("next")) && index(page.at("total"))
            && page.at("cursor") == arguments.value("cursor", Json(0))
            && page.at("next").get<std::uint64_t>() >= page.at("cursor").get<std::uint64_t>()
            && page.at("turns").is_array()
            && page.at("turns").size() <= arguments.value("limit", 5),
        "invalid conversation page");
    require(page.at("refreshed_at").is_null()
            || (page.at("refreshed_at").is_string()
                && page.at("refreshed_at").get_ref<const std::string&>().size() <= 128),
        "invalid refresh timestamp");
    for (const char* key : {"stale", "incomplete", "truncated"}) {
        require(page.at(key).is_boolean(), "invalid conversation completeness flag");
    }
    const auto cursor = page.at("cursor").get<std::uint64_t>();
    const auto total = page.at("total").get<std::uint64_t>();
    const auto count = page.at("turns").size();
    require(count <= (cursor < total ? total - cursor : 0), "conversation exceeds total");
    auto expected_next = cursor + count;
    // The Hub advances past one omitted oversized turn even with an empty page.
    if (count == 0 && cursor < total && page.at("truncated") == true) {
        ++expected_next;
    }
    require(page.at("next").get<std::uint64_t>() == expected_next,
        "invalid conversation next cursor");
    for (const char* key : {"revision", "worker_id", "cursor", "next", "total",
            "stale", "incomplete", "truncated", "refreshed_at"}) {
        result.field(key, page.at(key));
    }
    std::optional<std::uint64_t> previous_turn;
    for (const auto& turn : page.at("turns")) {
        require(turn.is_object() && index(turn.at("index"))
                && turn.at("steps").is_array() && turn.at("steps").size() <= 32,
            "invalid conversation turn");
        const auto turn_index = turn.at("index").get<std::uint64_t>();
        require(!previous_turn || turn_index > *previous_turn, "unordered conversation turns");
        previous_turn = turn_index;
        result.output.separate();
        result.field("turn", turn_index);
        if (turn.contains("request_id")) {
            require(identifier(turn.at("request_id")), "invalid turn request identity");
            result.field("request_id", turn.at("request_id"));
        }
        content(result, turn.at("user"), "user");
        std::optional<std::uint64_t> previous_step;
        for (const auto& step : turn.at("steps")) {
            require(step.is_object() && index(step.at("index")), "invalid conversation step");
            const auto step_index = step.at("index").get<std::uint64_t>();
            require(!previous_step || step_index > *previous_step, "unordered conversation steps");
            previous_step = step_index;
            if (step.contains("answer_source")) result.field("answer_source", step.at("answer_source"));
            content(result, step.at("content"), "assistant step " + std::to_string(step_index));
        }
    }
}

Presentation format_presentation(const std::string& route, const Json& value,
    const Json& arguments, std::size_t body_budget)
{
    Presentation result;
    result.body_budget = body_budget;
    if (arguments.contains("answer")) {
        require(route == "subagent/receive" && value.at("subagent_id") == arguments.at("subagent_id"), "mismatched answer target");
        const auto& page = value.at("answer");
        const auto& cursor = arguments.at("answer");
        require(page.at("source") == cursor.at("source") && page.at("part") == cursor.at("part")
            && page.at("offset") == cursor.at("offset") && page.at("raw").is_string()
            && page.at("raw").get_ref<const std::string&>().size() <= 32768
            && index(page.at("next_offset")) && index(page.at("next_part"))
            && index(page.at("bytes")) && index(page.at("total_parts")) && page.at("done").is_boolean(),
            "invalid answer page");
        status(result, value);
        const auto& text = page.at("raw").get_ref<const std::string&>();
        const auto offset = page.at("offset").get<std::uint64_t>();
        const auto next = page.at("next_offset").get<std::uint64_t>();
        require(next == offset + text.size() && next <= page.at("bytes").get<std::uint64_t>(), "invalid answer byte cursor");
        const auto part = page.at("part").get<std::uint64_t>();
        const auto total = page.at("total_parts").get<std::uint64_t>();
        const bool finished = next == page.at("bytes").get<std::uint64_t>();
        require(part < total && page.at("next_part") == part + (finished ? 1 : 0)
            && page.at("done") == (finished && part + 1 == total)
            && (finished || !text.empty()), "invalid answer completion cursor");
        for (const char* key : {"source", "part", "offset", "next_offset", "next_part", "bytes", "total_parts", "done"}) {
            result.field(key, page.at(key));
        }
        result.field("subagent_id", value.at("subagent_id"));
        result.output.block("answer page", text);
        return result;
    }
    if (route == "subagent/receive" && arguments.contains("subagent_id")) {
        plan_bodies(result, value.at("conversation"), value.at("requests"));
    }
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
    } else if (!arguments.contains("subagent_id")) {
        require(value.at("subagents").is_array(), "invalid child list");
        result.field("subagents", value.at("subagents").size());
        for (const auto& child : value.at("subagents")) {
            result.output.separate();
            status(result, child);
        }
    } else {
        require(value.at("subagent_id") == arguments.at("subagent_id"), "mismatched receive target");
        status(result, value);
        require(value.at("requests_truncated").is_boolean(), "invalid request completeness flag");
        result.field("requests_truncated", value.at("requests_truncated"));
        // Publish page metadata with child status before the separate outcome records.
        conversation(result, value.at("conversation"), arguments);
        request_outcomes(result, value.at("requests"));
    }
    if (value.contains("replayed")) {
        require(value.at("replayed").is_boolean(), "invalid receipt replay flag");
        result.field("replayed", value.at("replayed"));
    }
    return result;
}

model_io::Content format_result(const std::string& route, const Json& value, const Json& arguments)
{
    std::size_t body_budget = 96 * 1024;
    const bool can_clip_bodies = route == "subagent/receive"
        && arguments.contains("subagent_id") && !arguments.contains("answer");
    // Try the ordinary body allocation first: short complete blocks often have
    // smaller headers than empty/truncated ones. Only an actual oversized
    // presentation warrants clipping, never a worst-case header reservation.
    // Halving the allowance bounds this to 18 local passes including zero.
    // Every pass uses the same immutable reply; no RPC is repeated. Exact
    // answer pages stay byte-identical and do not enter this clipping fallback.
    for (;;) {
        auto presentation = format_presentation(route, value, arguments, body_budget);
        try {
            return presentation.render();
        } catch (const RenderingBudgetExceeded&) {
            if (!can_clip_bodies || body_budget == 0) {
                throw;
            }
        }
        body_budget /= 2;
    }
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
        reply = co_await request(query, route_, identity.worker_id, identity.session_id, identity.run_id);
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
    } catch (const RenderingBudgetExceeded&) {
        invoke_failed(mutation
            ? "rendered subagent result exceeds budget; a mutation may have committed; inspect subagent_receive before repeating"
            : "rendered subagent result exceeds the 256 KiB budget; retry receive with a smaller limit or use exact answer pages");
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
        exact(query.arguments, {"subagent_id", "operation", "content", "options"});
        const auto& arguments = query.arguments;
        require(identifier(arguments.value("subagent_id", Json())), "subagent_id must be a valid session ID");
        require(one_of(arguments.value("operation", Json()), {"message", "continue", "compact", "stop"}),
            "operation must be message, continue, compact or stop");
        if (arguments.at("operation") == "message") {
            require(arguments.contains("content"), "message requires content");
            validate_content(arguments.at("content"));
        } else {
            require(!arguments.contains("content"), "only message accepts content");
        }
        if (arguments.at("operation") == "stop") {
            require(!arguments.contains("options"), "stop does not accept options");
        } else if (arguments.contains("options")) {
            validate_options(arguments.at("options"));
        }
        argument_budget(arguments);
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

void SubagentReceiveTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    try {
        exact(query.arguments, {"subagent_id", "cursor", "limit", "answer"});
        const auto& arguments = query.arguments;
        if (arguments.contains("subagent_id")) {
            require(identifier(arguments.at("subagent_id")), "subagent_id must be a valid session ID");
        } else {
            require(!arguments.contains("cursor") && !arguments.contains("limit") && !arguments.contains("answer"),
                "pagination requires subagent_id");
        }
        require(!arguments.contains("cursor") || index(arguments.at("cursor")),
            "cursor must be a nonnegative safe integer");
        require(!arguments.contains("limit") || (index(arguments.at("limit"))
                && arguments.at("limit").get<std::uint64_t>() >= 1
                && arguments.at("limit").get<std::uint64_t>() <= 10),
            "limit must be an integer in 1..10");
        if (arguments.contains("answer")) {
            require(!arguments.contains("cursor") && !arguments.contains("limit"), "answer cannot carry turn pagination");
            const auto& answer = arguments.at("answer");
            exact(answer, {"source", "part", "offset"});
            require(index(answer.at("part")) && index(answer.at("offset")), "invalid answer cursor");
            const auto& source = answer.at("source");
            exact(source, {"worker_id", "turn", "step", "commit_sequence", "fingerprint"});
            require(identifier(source.at("worker_id")) && index(source.at("turn")) && index(source.at("step"))
                && source.at("commit_sequence").is_string()
                && !source.at("commit_sequence").get_ref<const std::string&>().empty()
                && source.at("commit_sequence").get_ref<const std::string&>().size() <= 20,
                "invalid answer source");
            require(source.contains("fingerprint") && source.at("fingerprint").is_string(),
                "answer source requires a fingerprint");
            const auto& digest = source.at("fingerprint").get_ref<const std::string&>();
            require(digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](char character) {
                return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            }), "invalid answer fingerprint");
            const auto& sequence = source.at("commit_sequence").get_ref<const std::string&>();
            require(sequence.front() != '0' && std::all_of(sequence.begin(), sequence.end(),
                [](unsigned char digit) { return digit >= '0' && digit <= '9'; }), "invalid answer commit sequence");
        }
        argument_budget(arguments);
    } catch (const std::exception& error) {
        bad_argument(error.what());
    }
}

} // namespace tools::intrinsic
