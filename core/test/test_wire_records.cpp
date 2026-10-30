#define BOOST_TEST_MODULE CoreWireRecords
#include <boost/test/unit_test.hpp>

#include "core/protocol.hpp"
#include "core/wire.hpp"
#include "protocol/wire.hpp"

#include <string>
#include <vector>

/**
 * Producer parity.
 *
 * These assertions are the "before" snapshots of the emission sites this
 * migration replaced: the literals were what the application wrote by hand.
 * Each producer now builds the protocol record, and this test fails if the
 * resulting JSON ever drifts from that recorded shape.
 */

using Json = nlohmann::json;

namespace {

model_io::Content text(std::string raw) {
    return {model_io::ContentType::Text, std::move(raw), {}, model_io::Modality::Text};
}

model_io::InvokeQuery call() {
    model_io::InvokeQuery query;
    query.id = "call-1";
    query.name = "read_text";
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::Trusted;
    query.arguments = Json{{"path", "notes.md"}};
    return query;
}

/** One committed response the projections can read. */
model_io::AgentInputState state() {
    model_io::AgentInputState value;
    value.turns.resize(1);
    auto& turn = value.turns.front();
    turn.user_input.type = model_io::MessageItemType::UserInput;
    turn.user_input.role = "user";
    turn.user_input.content = {text("hello")};
    auto& step = turn.agent_loop_step.emplace_back();
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.content = {text("done")};
    step.model_response.invokes = std::vector<model_io::InvokeQuery>{call()};
    step.commit_sequence = 3;
    return value;
}

/** One event envelope around already-built data. */
protocol::EventEnvelope envelope(protocol::EventName name, Json data) {
    protocol::EventEnvelope event;
    event.event = name;
    event.session_id = "session-1";
    event.worker_id = "worker-1";
    event.request_id = "request-1";
    event.run_id = "run-1";
    event.sequence = 1;
    event.data = std::move(data);
    return event;
}

} // namespace

BOOST_AUTO_TEST_CASE(event_envelope_keeps_the_emitted_shape) {
    // The application used to write this object literal at the emission site.
    const Json expected = {{"type", "event"}, {"event", "status"},
        {"session_id", "session-1"}, {"worker_id", "worker-1"},
        {"request_id", "request-1"}, {"run_id", "run-1"}, {"sequence", 1},
        {"data", {{"active", false}}}};
    const auto event = envelope(protocol::EventName::status, Json{{"active", false}});
    BOOST_TEST(protocol::to_json(event) == expected);
}

BOOST_AUTO_TEST_CASE(metadata_events_keep_their_emitted_shape) {
    protocol::PersistedData persisted;
    persisted.boundary = protocol::PersistBoundary::before_tools;
    persisted.backend = protocol::StorageKind::json;
    persisted.operation_id = "op-1";
    BOOST_TEST(protocol::to_json(persisted) == Json({
        {"boundary", "before_tools"}, {"backend", "json"},
        {"revision", nullptr}, {"operation_id", "op-1"}}));

    protocol::InputRejectedData rejected;
    rejected.request_id = Json(nullptr);
    rejected.message = "payload is invalid";
    rejected.operation = protocol::InputOperation::message;
    rejected.code = protocol::RejectionCode::invalid_options;
    BOOST_TEST(protocol::to_json(rejected) == Json({
        {"request_id", nullptr}, {"message", "payload is invalid"},
        {"operation", "message"}, {"code", "invalid_options"}}));

    protocol::RunOutcomeData finished;
    finished.status = protocol::RunStatus::failed;
    finished.error = "boom";
    finished.exchanges = 2;
    finished.task_exchanges = 2;
    finished.compact_exchanges = 0;
    finished.auto_compact = protocol::AutoCompact{0, 0, 3, 0};
    finished.durable = true;
    finished.failure = protocol::FailureData{protocol::FailureStage::model_request, "task", true};
    finished.rejected_queries = 4;
    finished.unreported_rejections = 0;
    BOOST_TEST(protocol::to_json(finished) == Json({
        {"status", "failed"}, {"error", "boom"}, {"exchanges", 2},
        {"task_exchanges", 2}, {"compact_exchanges", 0},
        {"auto_compact", {{"attempts", 0}, {"succeeded", 0}, {"limit", 3}, {"threshold", 0}}},
        {"durable", true},
        {"failure", {{"stage", "model_request"}, {"operation", "task"}, {"can_continue", true}}},
        {"rejected_queries", 4}, {"unreported_rejections", 0}}));

    protocol::StatusData status;
    status.storage_failed = true;
    status.rejected_payloads = 1;
    status.capabilities = {"session-history", "answer-pages"};
    BOOST_TEST(protocol::to_json(status) == Json({
        {"active", false}, {"stopping", false}, {"storage_failed", true},
        {"rejected_payloads", 1},
        {"capabilities", Json::array({"session-history", "answer-pages"})}}));
}

BOOST_AUTO_TEST_CASE(projection_producers_agree_with_the_event_registry) {
    const auto value = state();
    const auto projected = core::project_response(value, 0, 0, "worker-1");
    const auto source = core::answer_source(value.turns[0].agent_loop_step[0], 0, 0, "worker-1");

    core::HistoryRequest request;
    request.request_id = "history-1";
    const auto page = core::history_page(value, request, 5, "worker-1");

    const Json query = {{"operation", "answer"}, {"request_id", "answer-1"},
                        {"source", source}, {"part", 0}, {"offset", 0}};
    const auto answer = core::answer_page(value, query, "worker-1");

    std::vector<model_io::MessageItem> messages;
    model_io::InvokeReturn record;
    record.query = call();
    record.output = text("output");
    model_io::MessageItem message;
    message.type = model_io::MessageItemType::InvokeReturn;
    message.role = "tool";
    message.content = {record.output};
    message.invoke_return = std::move(record);
    messages.push_back(std::move(message));

    // Every producer's output must satisfy the record the registry publishes.
    BOOST_CHECK_NO_THROW(protocol::validate_event_data(
        envelope(protocol::EventName::model_response, projected)));
    BOOST_CHECK_NO_THROW(protocol::validate_event_data(
        envelope(protocol::EventName::history, page)));
    BOOST_CHECK_NO_THROW(protocol::validate_event_data(
        envelope(protocol::EventName::answer, answer)));
    BOOST_CHECK_NO_THROW(protocol::validate_event_data(
        envelope(protocol::EventName::tool_calls, core::display_calls({call()}))));
    BOOST_CHECK_NO_THROW(protocol::validate_event_data(
        envelope(protocol::EventName::tool_results, core::display_results(messages))));

    // The shapes themselves stay the literals the old sites produced.
    BOOST_TEST(projected.at("type") == "model_response");
    BOOST_TEST(projected.at("role") == "assistant");
    BOOST_TEST(projected.at("commit_sequence") == "3");
    BOOST_TEST(projected.at("invokes").at(0).at("name") == "read_text");
    BOOST_TEST(page.at("total") == 1);
    BOOST_TEST(page.at("turns").at(0).at("steps").at(0).at("commit_sequence") == "3");
    BOOST_TEST(answer.at("raw") == "done");
    BOOST_TEST(answer.at("done") == true);
}

BOOST_AUTO_TEST_CASE(confirmation_and_remote_call_envelopes_keep_their_shape) {
    const auto query = call();
    protocol::ConfirmationRequestEnvelope confirmation;
    confirmation.data.worker_id = "worker-1";
    confirmation.data.session_id = "session-1";
    confirmation.data.run_id = "run-1";
    confirmation.data.confirmation_id = "confirmation-1";
    confirmation.data.call = core::wire_call(query);
    BOOST_TEST(protocol::to_json(confirmation) == Json({
        {"type", "confirmation_request"},
        {"data", {{"worker_id", "worker-1"}, {"session_id", "session-1"},
                  {"run_id", "run-1"}, {"confirmation_id", "confirmation-1"},
                  {"call", {{"id", "call-1"}, {"name", "read_text"},
                            {"arguments", {{"path", "notes.md"}}},
                            {"type", "read_only"}, {"security", "trusted"}}}}}}));

    protocol::ToolRequestEnvelope remote;
    remote.data.worker_id = "worker-1";
    remote.data.session_id = "session-1";
    remote.data.run_id = "run-1";
    remote.data.request_id = "request-1";
    remote.data.arguments = Json{{"path", "notes.md"}};
    BOOST_TEST(protocol::to_json(remote) == Json({
        {"type", "tool_request"},
        {"data", {{"worker_id", "worker-1"}, {"session_id", "session-1"},
                  {"run_id", "run-1"}, {"request_id", "request-1"},
                  {"arguments", {{"path", "notes.md"}}}}}}));

    // A rejection keeps its code as a string, so a Hub-specific code needs no
    // worker protocol change.
    const auto rejected = protocol::from_json<protocol::ToolResponseEnvelope>(Json({
        {"type", "tool_response"},
        {"data", {{"worker_id", "worker-1"}, {"session_id", "session-1"},
                  {"run_id", "run-1"}, {"request_id", "request-1"},
                  {"route", "plan/read"}, {"status", "rejected"},
                  {"error", {{"code", "recovery_required"}, {"message", "inspect first"}}}}}}));
    BOOST_TEST(static_cast<int>(rejected.data.status)
               == static_cast<int>(protocol::ToolStatus::rejected));
    BOOST_TEST(rejected.data.error->code == "recovery_required");
    // Arguments and results stay opaque: an unfamiliar key is preserved.
    const auto accepted = protocol::from_json<protocol::ToolResponseEnvelope>(Json({
        {"type", "tool_response"},
        {"data", {{"worker_id", "worker-1"}, {"session_id", "session-1"},
                  {"run_id", "run-1"}, {"request_id", "request-1"},
                  {"route", "plan/replace"}, {"status", "succeeded"},
                  {"result", {{"revision", 2}, {"unfamiliar", Json::array({1, 2})}}}}}}));
    BOOST_TEST(accepted.data.result->at("unfamiliar").size() == 2);
}

BOOST_AUTO_TEST_CASE(query_rejections_keep_their_code_and_correlation) {
    // The transport refuses an oversized query or a full query mailbox before
    // validation. The refusal must name its reason, and it must answer the
    // request the caller actually sent.
    for (const std::string operation : {"history", "answer"}) {
        const auto expected = operation == "answer" ? protocol::EventName::answer_error
                                                    : protocol::EventName::history_error;
        for (const std::string code : {"query_too_large", "query_queue_full"}) {
            const auto rejection = core::query_rejection(Json("request-9"), operation, code);
            BOOST_TEST(static_cast<int>(rejection.event) == static_cast<int>(expected));
            const auto data = protocol::to_json(rejection.data);
            BOOST_TEST(data.size() == 3u);
            BOOST_TEST(data.at("request_id") == "request-9");
            BOOST_TEST(data.at("code") == code);
            BOOST_TEST(data.at("message")
                       == "Worker query quota exceeded; retry after the connection drains.");
            BOOST_CHECK_NO_THROW(protocol::validate_event_data(envelope(rejection.event, data)));
        }
    }
    // A query that reached validation keeps its previous shape: no code key.
    protocol::QueryErrorData invalid;
    invalid.request_id = Json(nullptr);
    invalid.message = "history request is invalid";
    const auto data = protocol::to_json(invalid);
    BOOST_TEST(data.size() == 2u);
    BOOST_TEST(!data.contains("code"));
    BOOST_CHECK_NO_THROW(protocol::validate_event_data(
        envelope(protocol::EventName::history_error, data)));
}
