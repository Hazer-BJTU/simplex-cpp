/**
 * @file protocol/wire.cpp
 * @brief Cross-field rules and the event-name to data-shape registry.
 *
 * Structural parsing lives in the codec; the rules here are the ones that only
 * make sense for a whole message: which fields an operation requires, which
 * envelope a type belongs to, and which record an event's `data` must match.
 * Semantic checks that need application state (ownership, cursors, run
 * liveness) stay in their owning modules.
 */

#include "protocol/wire.hpp"

#include <string>
#include <string_view>

namespace protocol {

namespace {

void require(bool condition, std::string_view path, std::string_view message) {
    if (!condition) {
        throw ValidationError(std::string(path), std::string(message));
    }
}

bool contains(const PayloadRequest& request, std::string_view field) {
    if (field == "content") return request.content.has_value();
    if (field == "options") return request.options.has_value();
    if (field == "start") return request.start.has_value();
    if (field == "step") return request.step.has_value();
    if (field == "limit") return request.limit.has_value();
    if (field == "source") return request.source.has_value();
    if (field == "part") return request.part.has_value();
    if (field == "offset") return request.offset.has_value();
    return false;
}

void reject_fields(const PayloadRequest& request, std::string_view operation,
                   std::initializer_list<std::string_view> fields) {
    for (const auto field : fields) {
        if (contains(request, field)) {
            throw ValidationError("data." + std::string(field),
                                  std::string(operation) + " does not accept this field");
        }
    }
}

} // namespace

std::span<const Rule> wire_rules(const PayloadRequest&) {
    static const std::array<std::string_view, 1> content{"content"};
    static const std::array<std::string_view, 2> history_forbidden{"content", "options"};
    static const std::array<std::string_view, 5> answer_forbidden{"content", "options", "start",
                                                                  "step", "limit"};
    static const std::array<std::string_view, 1> source{"source"};
    static const Rule table[] = {
        {"operation", "message", content, {}},
        {"operation", "continue", {}, content},
        {"operation", "compact", {}, content},
        {"operation", "history", {}, history_forbidden},
        {"operation", "answer", source, answer_forbidden, true},
    };
    return table;
}

std::span<const std::string_view> wire_forbidden_fields(const PayloadRequest&) {
    // The worker refuses these names outright even though it tolerates other
    // unknown fields on a payload (for example an incidental `start`).
    static const std::array<std::string_view, 5> forbidden{
        "role", "invokes", "invoke_return", "type", "text"};
    return forbidden;
}

bool wire_ignored_field(const PayloadRequest& request, std::string_view name) {
    // The worker parses a payload per operation and ignores the cursors that
    // belong to another one, whatever their JSON type happens to be.
    const bool cursor = name == "start" || name == "step" || name == "limit";
    const bool answer_only = name == "source" || name == "part" || name == "offset";
    switch (request.operation) {
        case InputOperation::message:
        case InputOperation::continue_request:
        case InputOperation::compact:
            return cursor || answer_only;
        case InputOperation::history:
            return answer_only;
        case InputOperation::answer:
            return false;
    }
    return false;
}

bool wire_reject_unknown(const PayloadRequest& request) {
    // An answer query has a closed field set; the other operations tolerate
    // fields that do not belong to them.
    return request.operation == InputOperation::answer;
}

std::span<const Rule> wire_rules(const SignalRequest&) {
    static const std::array<std::string_view, 1> run_id{"run_id"};
    static const Rule table[] = {
        {"operation", "cancel", run_id, {}},
        {"operation", "status", {}, run_id},
        {"operation", "options", {}, run_id},
        {"operation", "shutdown", {}, run_id},
    };
    return table;
}

void wire_validate(const ToolResponse& response) {
    if (response.status == ToolStatus::succeeded) {
        require(response.result.has_value(), "data.result", "succeeded requires a result object");
        require(!response.error.has_value(), "data.error", "succeeded must not carry an error");
    } else {
        require(response.error.has_value(), "data.error", "rejected requires an error object");
        require(!response.result.has_value(), "data.result", "rejected must not carry a result");
    }
}

void wire_validate(const EventEnvelope& envelope) {
    require(envelope.type == EnvelopeType::event, "type", "expected \"event\"");
}

void wire_validate(const PayloadEnvelope& envelope) {
    require(envelope.type == EnvelopeType::payload, "type", "expected \"payload\"");
}

void wire_validate(const SignalEnvelope& envelope) {
    require(envelope.type == EnvelopeType::signal, "type", "expected \"signal\"");
}

std::span<const MessageSpec> message_registry() {
    static const MessageSpec specs[] = {
        {"connection", &describe_record<EventEnvelope>()},
        {"connection", &describe_record<PayloadEnvelope>()},
        {"connection", &describe_record<SignalEnvelope>()},
        {"confirmation", &describe_record<ConfirmationRequestEnvelope>()},
        {"confirmation", &describe_record<ConfirmationResponseEnvelope>()},
        {"remote_tool", &describe_record<ToolRequestEnvelope>()},
        {"remote_tool", &describe_record<ToolResponseEnvelope>()},
    };
    return specs;
}

std::span<const RouteSpec> route_registry() {
    static const RouteSpec routes[] = {
        {"plan/read", &describe_record<PlanReadArguments>(), &describe_record<PlanReadResult>(),
         "Read the session plan; forbidden arguments include markdown."},
        {"plan/replace", &describe_record<PlanReplaceArguments>(),
         &describe_record<PlanReplaceResult>(),
         "Replace or clear the session plan; markdown is limited to 64 KiB."},
        {"subagent/clean-fork", nullptr, nullptr,
         "Start an independent headless worker; argument and result shapes are "
         "defined by the Hub subagent contract."},
        {"subagent/send", nullptr, nullptr,
         "Send message/continue/compact/stop to a child; Hub contract."},
        {"subagent/receive", nullptr, nullptr,
         "Read bounded child status and conversation; Hub contract."},
    };
    return routes;
}

std::span<const EventDataSpec> event_data_registry() {
    static const EventDataSpec specs[] = {
        {EventName::ready, &describe_record<StatusData>()},
        {EventName::status, &describe_record<StatusData>()},
        {EventName::options, &describe_record<OptionsData>()},
        {EventName::history, &describe_record<HistoryPage>()},
        {EventName::answer, &describe_record<AnswerPage>()},
        {EventName::answer_error, &describe_record<QueryErrorData>()},
        {EventName::history_error, &describe_record<QueryErrorData>()},
        {EventName::input_admitted, &describe_record<InputAdmittedData>()},
        {EventName::input_rejected, &describe_record<InputRejectedData>()},
        {EventName::run_started, &describe_record<EmptyData>()},
        {EventName::input_committed, &describe_record<EmptyData>()},
        {EventName::model_response, &describe_record<ResponseProjection>()},
        {EventName::tool_calls, &describe_record<Call>(), true},
        {EventName::tool_results, &describe_record<ToolResultMessage>(), true},
        {EventName::persisted, &describe_record<PersistedData>()},
        {EventName::compact_finished, &describe_record<CompactFinishedData>()},
        {EventName::export_error, &describe_record<ExportErrorData>()},
        {EventName::error, &describe_record<ErrorData>()},
        {EventName::run_finished, &describe_record<RunOutcomeData>()},
    };
    return specs;
}

void validate_event_data(const EventEnvelope& envelope) {
    const std::string prefix = "data";
    switch (envelope.event) {
        case EventName::ready:
        case EventName::status:
            protocol::from_json<StatusData>(envelope.data, prefix);
            return;
        case EventName::options:
            protocol::from_json<OptionsData>(envelope.data, prefix);
            return;
        case EventName::history:
            protocol::from_json<HistoryPage>(envelope.data, prefix);
            return;
        case EventName::answer:
            protocol::from_json<AnswerPage>(envelope.data, prefix);
            return;
        case EventName::answer_error:
        case EventName::history_error:
            protocol::from_json<QueryErrorData>(envelope.data, prefix);
            return;
        case EventName::input_admitted:
            protocol::from_json<InputAdmittedData>(envelope.data, prefix);
            return;
        case EventName::input_rejected:
            protocol::from_json<InputRejectedData>(envelope.data, prefix);
            return;
        case EventName::run_started:
        case EventName::input_committed:
            protocol::from_json<EmptyData>(envelope.data, prefix);
            return;
        case EventName::model_response: {
            const auto projection = protocol::from_json<ResponseProjection>(envelope.data, prefix);
            if (projection.invokes) {
                parse_batch<Call>(*projection.invokes, prefix + ".invokes");
            }
            return;
        }
        case EventName::tool_calls:
            parse_batch<Call>(envelope.data, "data");
            return;
        case EventName::tool_results:
            parse_batch<ToolResultMessage>(envelope.data, "data");
            return;
        case EventName::persisted:
            protocol::from_json<PersistedData>(envelope.data, prefix);
            return;
        case EventName::compact_finished: {
            const auto finished = protocol::from_json<CompactFinishedData>(envelope.data, prefix);
            if (finished.summary.is_object()) {
                protocol::from_json<OmissionSummary>(finished.summary, prefix + ".summary");
            } else if (!finished.summary.is_string()) {
                throw ValidationError(prefix + ".summary",
                                      "expected a summary string or an omission object");
            }
            return;
        }
        case EventName::export_error:
            protocol::from_json<ExportErrorData>(envelope.data, prefix);
            return;
        case EventName::error:
            protocol::from_json<ErrorData>(envelope.data, prefix);
            return;
        case EventName::run_finished:
            protocol::from_json<RunOutcomeData>(envelope.data, prefix);
            return;
    }
    throw ValidationError("event", "unrecognized event name");
}

} // namespace protocol
