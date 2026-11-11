#include "tools/intrinsic/hub_remote_call/tools.hpp"
#include "tools/intrinsic/hub_remote_call/schemas.hpp"
#include "tools/intrinsic/tool_result.hpp"

#include <boost/asio/this_coro.hpp>
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace tools::intrinsic {
namespace {
using Json = nlohmann::json;

constexpr std::size_t max_question_bytes = 8 * 1024;
constexpr std::size_t max_option_bytes = 200;
constexpr std::size_t min_options = 2;
constexpr std::size_t max_options = 8;
/** The hub settles the question first; this slack only covers the reply write. */
constexpr std::chrono::milliseconds reply_slack{10000};

bool nonblank(const std::string& value)
{
    return value.find_first_not_of(" \t\r\n\f\v") != std::string::npos;
}

/** dump() rejects invalid UTF-8 before anything reaches the hub. */
bool valid_utf8(const Json& value)
{
    try {
        (void)value.dump();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string seconds_of(std::chrono::milliseconds budget)
{
    return std::to_string(budget.count() / 1000);
}

model_io::Content unavailable(std::string detail)
{
    ToolResult output;
    output.field("outcome", "unavailable");
    output.block("user reply",
        "No user reply: " + std::move(detail) + ". Treat this as no answer and continue; "
        "state any assumption you make, or ask again later.");
    return output.render();
}

model_io::Content render_result(const Json& result, std::chrono::milliseconds budget)
{
    if (!result.is_object() || !result.contains("outcome")
        || !result.at("outcome").is_string()) {
        throw std::runtime_error("invalid ask_user result");
    }
    const auto outcome = result.at("outcome").get<std::string>();
    ToolResult output;
    output.field("outcome", outcome);
    if (result.contains("elapsed_ms") && result.at("elapsed_ms").is_number_unsigned()) {
        output.field("elapsed_ms", result.at("elapsed_ms"));
    }
    if (outcome == "answered") {
        const auto& reply = result.at("reply");
        if (!reply.is_object()) throw std::runtime_error("invalid ask_user reply");
        if (reply.contains("option_index") && reply.at("option_index").is_number_unsigned()
            && reply.contains("option") && reply.at("option").is_string()) {
            output.field("option_index", reply.at("option_index"));
            output.block("user reply", "Option " + std::to_string(
                reply.at("option_index").get<std::size_t>() + 1) + ": "
                + reply.at("option").get<std::string>());
        } else {
            output.block("user reply", reply.at("text").get<std::string>());
        }
        return output.render();
    }
    if (outcome == "timeout") {
        output.block("user reply",
            "No user reply: the question timed out after " + seconds_of(budget)
            + " s. The user may be busy; assume nothing was confirmed, state your "
              "assumption, or ask again later.");
        return output.render();
    }
    if (outcome == "cancelled") {
        const auto reason = result.value("reason", std::string{"cancelled"});
        output.block("user reply",
            "No user reply: the question was withdrawn (" + reason
            + ") before the user answered. Treat this as no answer and continue; "
              "state any assumption you make.");
        return output.render();
    }
    throw std::runtime_error("unknown ask_user outcome");
}
} // namespace

AskUserTool::AskUserTool(endpoint::ResolvedEndpoint endpoint, std::chrono::milliseconds timeout,
    std::chrono::milliseconds question_timeout, HubRemoteCallIdentityProvider identity)
    : HubRemoteCallToolBase(hub_remote_call::schema_directory() / "ask_user.yaml",
        std::move(endpoint), timeout), identity_(std::move(identity)),
      question_timeout_(question_timeout)
{
    if (!identity_) throw std::invalid_argument("ask_user requires a host identity provider");
    const auto count = question_timeout_.count();
    if (count < 60000 || count > 1800000) {
        throw std::invalid_argument(
            "ask_user requires a question timeout between 60000 and 1800000 ms");
    }
}

void AskUserTool::ensure_arguments(model_io::InvokeQuery& query) const
{
    if (!query.arguments.is_object()) bad_argument("arguments must be an object");
    for (const auto& [key, value] : query.arguments.items()) {
        if (key != "question" && key != "options") {
            bad_argument("unknown ask_user argument: " + key);
        }
    }
    const auto question = require_string(query, "question", "the question to ask the user");
    if (!nonblank(question)) bad_argument("question must not be blank");
    if (question.size() > max_question_bytes) bad_argument("question exceeds 8 KiB");
    if (!valid_utf8(question)) bad_argument("question must be valid UTF-8");
    if (const auto* options = find_argument(query, "options"); options != nullptr) {
        if (!options->is_array()) bad_argument("options must be an array of strings");
        if (options->size() < min_options || options->size() > max_options) {
            bad_argument("options must contain between 2 and 8 entries");
        }
        std::vector<std::string> seen;
        for (const auto& option : *options) {
            if (!option.is_string()) bad_argument("each option must be a string");
            const auto& text = option.get_ref<const std::string&>();
            if (!nonblank(text)) bad_argument("options must not be blank");
            if (text.size() > max_option_bytes) bad_argument("an option exceeds 200 bytes");
            if (!valid_utf8(option)) bad_argument("options must be valid UTF-8");
            if (std::find(seen.begin(), seen.end(), text) != seen.end()) {
                bad_argument("options must be unique");
            }
            seen.push_back(text);
        }
    }
    // The wait budget is host policy, not a model choice. Settling it here keeps
    // the recorded call and the wire request identical.
    write_argument(query, "timeout_ms", question_timeout_.count());
}

void AskUserTool::write_attributes(model_io::InvokeQuery& query) const
{
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::Trusted;
}

boost::asio::awaitable<model_io::Content> AskUserTool::invoke(const model_io::InvokeQuery& query)
{
    co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
    try {
        const auto identity = identity_();
        Json reply;
        try {
            reply = co_await request(query, "ask_user/ask", identity.worker_id,
                identity.session_id, identity.run_id, question_timeout_ + reply_slack);
        } catch (const InvokeException&) {
            // A lost hub must not fail the run: the model needs an answer it can
            // act on, and "no reply" is the honest one.
            co_return unavailable("the hub connection was lost before the user answered");
        }
        if (!reply.is_object() || !reply.contains("status")) {
            co_return unavailable("the hub returned an unexpected reply");
        }
        if (reply.at("status") == "rejected") {
            const auto& error = reply.at("error");
            co_return unavailable("the hub rejected the question ("
                + error.at("code").get<std::string>() + "): "
                + error.at("message").get<std::string>());
        }
        co_return render_result(reply.at("result"), question_timeout_);
    } catch (const std::exception& error) {
        invoke_failed(std::string("ask_user: ") + error.what());
    }
}
} // namespace tools::intrinsic
