#define BOOST_TEST_MODULE protocol_codec
#include <boost/test/unit_test.hpp>

#include "protocol/wire.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace {

using namespace protocol;

/** Build a well-formed `message` payload request. */
PayloadRequest message_request() {
    PayloadRequest request;
    request.operation = InputOperation::message;
    request.request_id = "request-1";
    request.content = std::vector<Content>{Content{ContentType::text, Modality::text, "hello"}};
    return request;
}

/** Build a complete answer source. */
AnswerSource answer_source() {
    return AnswerSource{"worker-1", 0, 1, "2",
                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
}

} // namespace

BOOST_AUTO_TEST_CASE(content_round_trips_with_display_markers) {
    Content content{ContentType::text, Modality::text, "truncated text"};
    content.truncated = true;
    content.bytes = 4096;
    content.extras = Json{{"simplex.source", Json{{"run_id", "run-1"}}}};

    const Json encoded = protocol::to_json(content);
    BOOST_TEST(encoded.at("type").get<std::string>() == "text");
    BOOST_TEST(encoded.at("modality").get<std::string>() == "text");
    BOOST_TEST(encoded.at("truncated").get<bool>());
    BOOST_TEST(encoded.at("extras").at("simplex.source").at("run_id").get<std::string>() == "run-1");

    const Content parsed = protocol::from_json<Content>(encoded);
    BOOST_TEST(parsed.raw == content.raw);
    BOOST_TEST(parsed.extras.has_value());
    BOOST_TEST(protocol::to_json(parsed) == encoded);
}

BOOST_AUTO_TEST_CASE(absent_optional_fields_are_omitted_not_null) {
    Content content{ContentType::text, Modality::text, "plain"};
    const Json encoded = protocol::to_json(content);
    BOOST_TEST(!encoded.contains("extras"));
    BOOST_TEST(!encoded.contains("truncated"));
    BOOST_TEST(!encoded.contains("bytes"));
    BOOST_TEST(!encoded.contains("omitted"));
}

BOOST_AUTO_TEST_CASE(message_round_trips_with_nested_records) {
    Message message;
    message.type = MessageType::model_response;
    message.role = "assistant";
    message.content = {Content{ContentType::text, Modality::text, "done"}};
    message.reasoning = Content{ContentType::text, Modality::text, "because"};
    message.invokes = std::vector<Call>{Call{"call-1", "read_text", Json{{"path", "a.txt"}},
                                            CallScheduling::read_only, CallSecurity::trusted}};
    message.cost = Usage{120, 4, 80};

    const Json encoded = protocol::to_json(message);
    const Message parsed = protocol::from_json<Message>(encoded);
    BOOST_TEST(parsed.invokes.has_value());
    BOOST_TEST(parsed.invokes->at(0).arguments.at("path").get<std::string>() == "a.txt");
    BOOST_TEST(static_cast<int>(parsed.invokes->at(0).type)
               == static_cast<int>(CallScheduling::read_only));
    BOOST_TEST(parsed.cost->cache_hit == 80);
    BOOST_TEST(protocol::to_json(parsed) == encoded);
}

BOOST_AUTO_TEST_CASE(scheduling_spelling_matches_the_wire) {
    Call call{"call-1", "write", Json::object(), CallScheduling::parall_write,
              CallSecurity::require_confirm};
    const Json encoded = protocol::to_json(call);
    // A renamed enumerator must not silently rename the wire value.
    BOOST_TEST(encoded.at("type").get<std::string>() == "parall_write");
    BOOST_TEST(encoded.at("security").get<std::string>() == "require_confirm");
}

BOOST_AUTO_TEST_CASE(missing_required_field_names_its_path) {
    Json encoded = protocol::to_json(message_request());
    encoded.erase("request_id");
    try {
        protocol::from_json<PayloadRequest>(encoded, "data");
        BOOST_ERROR("a missing required field must be rejected");
    } catch (const ValidationError& error) {
        BOOST_TEST(std::string(error.what()).find("data.request_id") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(explicit_null_is_only_accepted_where_declared) {
    // `persisted.revision` is null when the backend exposes no revision.
    const Json persisted = Json{{"boundary", "before_tools"},
                                {"backend", "json"},
                                {"revision", nullptr},
                                {"operation_id", "op-1"}};
    const PersistedData parsed = protocol::from_json<PersistedData>(persisted);
    BOOST_TEST(!parsed.revision.has_value());

    Json content = Json{{"type", "text"}, {"modality", "text"}, {"raw", nullptr}};
    BOOST_CHECK_THROW(protocol::from_json<Content>(content), ValidationError);
}

BOOST_AUTO_TEST_CASE(unknown_fields_are_ignored_by_default) {
    Json encoded = protocol::to_json(message_request());
    encoded["future_field"] = "ignored";
    const PayloadRequest parsed = protocol::from_json<PayloadRequest>(encoded);
    BOOST_TEST(parsed.request_id == "request-1");
    BOOST_TEST(!protocol::to_json(parsed).contains("future_field"));
}

BOOST_AUTO_TEST_CASE(unknown_enumeration_spelling_is_rejected) {
    Json content = Json{{"type", "audio"}, {"modality", "text"}, {"raw", "x"}};
    try {
        protocol::from_json<Content>(content);
        BOOST_ERROR("an unknown encoding must not be accepted");
    } catch (const ValidationError& error) {
        BOOST_TEST(std::string(error.what()).find("expected one of") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(payload_operation_conditions_are_enforced) {
    // message requires content
    PayloadRequest empty_message;
    empty_message.request_id = "r";
    BOOST_CHECK_THROW(protocol::from_json<PayloadRequest>(protocol::to_json(empty_message)), ValidationError);

    // history rejects content
    PayloadRequest history;
    history.operation = InputOperation::history;
    history.request_id = "h";
    history.content = std::vector<Content>{Content{}};
    BOOST_CHECK_THROW(protocol::from_json<PayloadRequest>(protocol::to_json(history)), ValidationError);

    // history limit is bounded
    PayloadRequest wide;
    wide.operation = InputOperation::history;
    wide.request_id = "h";
    wide.limit = 11;
    BOOST_CHECK_THROW(protocol::from_json<PayloadRequest>(protocol::to_json(wide)), ValidationError);

    // answer requires a complete source
    PayloadRequest answer;
    answer.operation = InputOperation::answer;
    answer.request_id = "a";
    answer.part = 0;
    answer.offset = 0;
    BOOST_CHECK_THROW(protocol::from_json<PayloadRequest>(protocol::to_json(answer)), ValidationError);

    answer.source = answer_source();
    BOOST_CHECK_NO_THROW(protocol::from_json<PayloadRequest>(protocol::to_json(answer)));

    // continue rejects content
    PayloadRequest continuation;
    continuation.operation = InputOperation::continue_request;
    continuation.request_id = "c";
    continuation.content = std::vector<Content>{Content{}};
    BOOST_CHECK_THROW(protocol::from_json<PayloadRequest>(protocol::to_json(continuation)), ValidationError);
}

BOOST_AUTO_TEST_CASE(signal_cancel_requires_its_run_identity) {
    SignalRequest cancel;
    cancel.operation = SignalOperation::cancel;
    BOOST_CHECK_THROW(protocol::from_json<SignalRequest>(protocol::to_json(cancel)), ValidationError);

    cancel.run_id = "run-1";
    BOOST_CHECK_NO_THROW(protocol::from_json<SignalRequest>(protocol::to_json(cancel)));

    SignalRequest status;
    status.operation = SignalOperation::status;
    status.run_id = "run-1";
    BOOST_CHECK_THROW(protocol::from_json<SignalRequest>(protocol::to_json(status)), ValidationError);
}

BOOST_AUTO_TEST_CASE(tool_response_is_result_or_rejection_never_both) {
    ToolResponse response;
    response.worker_id = "w";
    response.session_id = "s";
    response.run_id = "r";
    response.request_id = "q";
    response.route = "plan/read";
    response.status = ToolStatus::succeeded;
    response.error = ToolError{ToolErrorCode::storage_error, "unexpected"};
    BOOST_CHECK_THROW(protocol::from_json<ToolResponse>(protocol::to_json(response)), ValidationError);

    response.error.reset();
    response.result = Json{{"markdown", "- [ ] Work"}, {"revision", 1}};
    BOOST_CHECK_NO_THROW(protocol::from_json<ToolResponse>(protocol::to_json(response)));

    response.status = ToolStatus::rejected;
    response.result.reset();
    BOOST_CHECK_THROW(protocol::from_json<ToolResponse>(protocol::to_json(response)), ValidationError);
}

BOOST_AUTO_TEST_CASE(event_data_registry_covers_every_event_name) {
    const auto specs = event_data_registry();
    BOOST_TEST(specs.size() == EnumTraits<EventName>::values.size());
    for (const auto& value : EnumTraits<EventName>::values) {
        const auto found = std::find_if(specs.begin(), specs.end(), [&](const EventDataSpec& spec) {
            return spec.event == value.value;
        });
        BOOST_TEST_MESSAGE("event: " << value.spelling);
        BOOST_REQUIRE(found != specs.end());
        BOOST_TEST(!found->record->name.empty());
    }
}

BOOST_AUTO_TEST_CASE(valid_event_data_parses_for_every_family) {
    EventEnvelope envelope;
    envelope.session_id = "demo";
    envelope.worker_id = "worker-1";

    // ready/status snapshot
    envelope.event = EventName::status;
    envelope.data = Json{{"active", false},
                         {"stopping", false},
                         {"storage_failed", false},
                         {"rejected_payloads", 0},
                         {"capabilities", Json::array({"session-history", "answer-pages"})}};
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));

    // run outcome with settlement counters
    envelope.event = EventName::run_finished;
    envelope.data = Json{{"status", "completed"},
                         {"error", ""},
                         {"exchanges", 1},
                         {"durable", true},
                         {"omitted_display_events", 2}};
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));

    // query error with a null request identity
    envelope.event = EventName::history_error;
    envelope.data = Json{{"request_id", nullptr}, {"message", "invalid cursor"}};
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));

    // an empty-object event
    envelope.event = EventName::run_started;
    envelope.data = Json::object();
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));

    // a tool batch with an omission marker
    envelope.event = EventName::tool_calls;
    envelope.data = Json::array({
        Json{{"id", "call-1"}, {"name", "read_text"},
             {"arguments", Json{{"path", "a"}}}, {"type", "read_only"},
             {"security", "trusted"}},
        Json{{"display_omitted", true}, {"omitted_items", 3}},
    });
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));
}

BOOST_AUTO_TEST_CASE(invalid_event_data_is_rejected_with_a_path) {
    EventEnvelope envelope;
    envelope.event = EventName::run_finished;
    envelope.data = Json{{"error", ""}, {"exchanges", 1}, {"durable", true}};
    try {
        validate_event_data(envelope);
        BOOST_ERROR("a run_finished without status must be rejected");
    } catch (const ValidationError& error) {
        BOOST_TEST(std::string(error.what()).find("data.status") != std::string::npos);
    }

    // an omission marker is only legal as the final batch element
    envelope.event = EventName::tool_calls;
    envelope.data = Json::array({
        Json{{"display_omitted", true}, {"omitted_items", 1}},
        Json{{"id", "call-1"}, {"name", "read_text"}, {"arguments", Json::object()},
             {"type", "read_only"}, {"security", "trusted"}},
    });
    BOOST_CHECK_THROW(validate_event_data(envelope), ValidationError);
}

BOOST_AUTO_TEST_CASE(envelope_type_must_match_its_family) {
    EventEnvelope envelope;
    envelope.type = EnvelopeType::payload;
    BOOST_CHECK_THROW(protocol::from_json<EventEnvelope>(protocol::to_json(envelope)), ValidationError);
}
