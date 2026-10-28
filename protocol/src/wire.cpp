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

void wire_validate(const PayloadRequest& request) {
    require(!request.request_id.empty(), "data.request_id", "must be a nonempty string");
    require(request.request_id.size() <= 128, "data.request_id", "must be at most 128 UTF-8 bytes");

    switch (request.operation) {
        case InputOperation::message:
            require(request.content.has_value() && !request.content->empty(), "data.content",
                    "message requires a nonempty content array");
            reject_fields(request, "message", {"start", "step", "limit", "source", "part", "offset"});
            return;
        case InputOperation::continue_request:
            reject_fields(request, "continue", {"content", "start", "step", "limit", "source", "part", "offset"});
            return;
        case InputOperation::compact:
            reject_fields(request, "compact", {"content", "start", "step", "limit", "source", "part", "offset"});
            return;
        case InputOperation::history:
            reject_fields(request, "history", {"content", "options", "source", "part", "offset"});
            if (request.limit) {
                require(*request.limit >= 1 && *request.limit <= 10, "data.limit",
                        "history limit must be between 1 and 10");
            }
            return;
        case InputOperation::answer:
            reject_fields(request, "answer", {"content", "options", "start", "step", "limit"});
            require(request.source.has_value(), "data.source", "answer requires a complete source");
            require(request.part.has_value(), "data.part", "answer requires a part index");
            require(request.offset.has_value(), "data.offset", "answer requires a byte offset");
            require(!request.source->fingerprint.empty(), "data.source.fingerprint",
                    "answer sources must carry the fingerprint");
            return;
    }
    throw ValidationError("data.operation", "unrecognized operation");
}

void wire_validate(const SignalRequest& request) {
    if (request.operation == SignalOperation::cancel) {
        require(request.run_id.has_value() && !request.run_id->empty(), "data.run_id",
                "cancel requires a nonempty run_id");
    } else {
        require(!request.run_id.has_value(), "data.run_id",
                "run_id is only accepted for cancel");
    }
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
        {EventName::model_response, &describe_record<Message>()},
        {EventName::tool_calls, &describe_record<Call>(), true},
        {EventName::tool_results, &describe_record<Result>(), true},
        {EventName::persisted, &describe_record<PersistedData>()},
        {EventName::compact_finished, &describe_record<CompactFinishedData>()},
        {EventName::export_error, &describe_record<ExportErrorData>()},
        {EventName::error, &describe_record<ErrorData>()},
        {EventName::run_finished, &describe_record<RunOutcomeData>()},
    };
    return specs;
}

namespace {

/** Parse one element of a tool batch, accepting the trailing omission marker. */
template <class R>
void validate_batch(const Json& data, std::string_view name) {
    require(data.is_array(), name, "expected an array for this batch event");
    for (std::size_t index = 0; index < data.size(); ++index) {
        const auto& entry = data.at(index);
        const bool last = index + 1 == data.size();
        if (last && entry.is_object() && entry.contains("display_omitted")) {
            protocol::from_json<OmissionMarker>(entry, std::string(name) + "[" + std::to_string(index) + "]");
            continue;
        }
        protocol::from_json<R>(entry, std::string(name) + "[" + std::to_string(index) + "]");
    }
}

} // namespace

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
        case EventName::model_response:
            protocol::from_json<Message>(envelope.data, prefix);
            return;
        case EventName::tool_calls:
            validate_batch<Call>(envelope.data, "data");
            return;
        case EventName::tool_results:
            validate_batch<Result>(envelope.data, "data");
            return;
        case EventName::persisted:
            protocol::from_json<PersistedData>(envelope.data, prefix);
            return;
        case EventName::compact_finished:
            protocol::from_json<CompactFinishedData>(envelope.data, prefix);
            return;
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
