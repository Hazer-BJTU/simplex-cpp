#include "core/confirmation.hpp"
#include "core/protocol.hpp"
#include "intercom/cancellable_exchange.hpp"

namespace core {
nlohmann::json ConfirmationOptions::get_options() const {
    return nlohmann::json::array({{
        {"name", "mode"}, {"options", {"ask", "approve", "deny"}}
    }});
}

void ConfirmationOptions::handle_options(const nlohmann::json& options) {
    if (!options.is_object()) {
        throw std::invalid_argument("confirmation options must be an object");
    }
    auto selected = mode_;
    for (const auto& [name, value] : options.items()) {
        if (name != "mode") {
            throw std::invalid_argument("unsupported confirmation option: " + name);
        }
        if (value == "ask") {
            selected = ConfirmationMode::Ask;
        } else if (value == "approve") {
            selected = ConfirmationMode::Approve;
        } else if (value == "deny") {
            selected = ConfirmationMode::Deny;
        } else {
            throw std::invalid_argument("confirmation mode must be ask, approve or deny");
        }
    }
    mode_ = selected;
}

void ConfirmationScope::cancel() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    stop_.request_stop();
}
bool ConfirmationScope::settle_approval(bool approved) {
    std::lock_guard lock(mutex_);
    return approved && !stopping_;
}

boost::asio::awaitable<tools::InvokeConfirmEvent> confirm(
    tools::InvokeConfirmEvent event, std::shared_ptr<ConfirmationScope> scope,
    boost::asio::any_io_executor executor, std::optional<endpoint::ResolvedEndpoint> endpoint,
    std::chrono::milliseconds timeout, std::string session_id, std::string run_id) {
    event.decision = tools::ConfirmDecision::Denied;
    event.reason = "confirmation endpoint is not configured";
    if (!scope || scope->token().stop_requested()) {
        event.reason = "run cancelled before confirmation";
        co_return event;
    }
    if (scope->mode() == ConfirmationMode::Deny) {
        event.reason = "confirmation policy denies the invocation";
        co_return event;
    }
    if (scope->mode() == ConfirmationMode::Approve) {
        if (scope->settle_approval(true)) {
            event.decision = tools::ConfirmDecision::Approved;
            event.reason = "confirmation policy approves the invocation";
        } else {
            event.reason = "run cancelled during confirmation";
        }
        co_return event;
    }
    if (!endpoint) co_return event;
    const auto id = new_identity();
    nlohmann::json request = {{"type", "confirmation_request"}, {"data", {
        {"session_id", session_id}, {"run_id", run_id}, {"confirmation_id", id},
        {"call", event.query}}}};
    try {
        const auto wire = co_await intercom::cancellable_exchange(
            executor, *endpoint, request.dump(), timeout, scope->token());
        const auto reply = nlohmann::json::parse(wire);
        const auto& data = reply.at("data");
        if (reply.at("type") != "confirmation_response"
            || data.at("session_id") != session_id || data.at("run_id") != run_id
            || data.at("confirmation_id") != id)
            throw std::invalid_argument("confirmation correlation mismatch");
        const auto decision = data.at("decision").get<std::string>();
        if (decision != "approved" && decision != "denied")
            throw std::invalid_argument("invalid confirmation decision");
        event.reason = data.value("reason", std::string("operator decision"));
        if (scope->settle_approval(decision == "approved"))
            event.decision = tools::ConfirmDecision::Approved;
        else if (decision == "approved" || scope->token().stop_requested())
            event.reason = "run cancelled during confirmation";
    } catch (const std::exception& error) {
        event.reason = std::string("confirmation failed: ") + error.what();
    }
    co_return event;
}
} // namespace core
