#define BOOST_TEST_MODULE protocol_codec
#include <boost/test/unit_test.hpp>

#include "protocol/wire.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace {

using namespace protocol;

/** A complete answer source, as core::answer_source() builds one. */
Json source_json() {
    return Json{{"worker_id", "worker-1"},
                {"turn", 0},
                {"step", 1},
                {"commit_sequence", "2"},
                {"fingerprint", std::string(64, 'a')}};
}

/** One valid request part. */
Json part_json(std::string raw = "hello") {
    return Json{{"type", "text"}, {"modality", "text"}, {"raw", raw}};
}

Json message_payload() {
    return Json{{"operation", "message"},
                {"request_id", "request-1"},
                {"content", Json::array({part_json()})}};
}

/** A tool-result entry exactly as core::display_results() projects it. */
Json tool_result_message() {
    return Json{{"type", "invoke_return"},
                {"role", "tool"},
                {"content", Json::array({part_json("hello")})},
                {"invoke_return",
                 Json{{"query",
                       Json{{"id", "call-1"},
                            {"name", "read_text"},
                            {"type", "read_only"},
                            {"security", "trusted"},
                            {"arguments", Json::object()}}},
                      {"output", part_json("hello")}}}};
}

/** A model-response projection exactly as core::project_response() builds it. */
Json response_projection() {
    return Json{{"index", 0},
                {"type", "model_response"},
                {"role", "assistant"},
                {"content", Json::array({part_json("done")})},
                {"tool_calls", 1},
                {"omitted_parts", 0},
                {"commit_sequence", "3"},
                {"answer_source", source_json()},
                {"invokes",
                 Json::array({Json{{"id", "call-1"},
                                   {"name", "read_text"},
                                   {"type", "read_only"},
                                   {"security", "trusted"},
                                   {"arguments", Json::object()}}})}};
}

bool rejects_payload(const Json& payload) {
    try {
        protocol::from_json<PayloadRequest>(payload);
        return false;
    } catch (const ValidationError&) {
        return true;
    }
}

/** True when one described record refuses this value. */
template <class R>
bool rejects(const Json& value) {
    try {
        protocol::from_json<R>(value);
        return false;
    } catch (const ValidationError&) {
        return true;
    }
}

} // namespace

BOOST_AUTO_TEST_CASE(content_round_trips_with_display_markers) {
    Content content{ContentType::text, Modality::text, "truncated text"};
    content.truncated = true;
    content.bytes = 4096;
    content.extras = Json{{"simplex.source", Json{{"run_id", "run-1"}}}};

    const Json encoded = protocol::to_json(content);
    BOOST_TEST(encoded.at("truncated").get<bool>());
    const Content parsed = protocol::from_json<Content>(encoded);
    BOOST_TEST(protocol::to_json(parsed) == encoded);
}

BOOST_AUTO_TEST_CASE(call_keeps_its_annotations) {
    Call call;
    call.id = "call-1";
    call.name = "run_command";
    call.type = CallScheduling::serial_write;
    call.security = CallSecurity::require_confirm;
    call.extras = Json{{"status", "failed"}, {"loop_skipped", true}};

    const Json encoded = protocol::to_json(call);
    BOOST_TEST(encoded.at("extras").at("loop_skipped").get<bool>());
    BOOST_TEST(encoded.at("type").get<std::string>() == "serial_write");
    BOOST_TEST(protocol::to_json(protocol::from_json<Call>(encoded)) == encoded);
}

BOOST_AUTO_TEST_CASE(persisted_revision_is_required_and_nullable) {
    const Json with_null = Json{{"boundary", "before_tools"},
                                {"backend", "json"},
                                {"revision", nullptr},
                                {"operation_id", "op-1"}};
    // A missing revision is not the same statement as a null one.
    Json missing = with_null;
    missing.erase("revision");
    BOOST_CHECK_THROW(protocol::from_json<PersistedData>(missing), ValidationError);

    const PersistedData parsed = protocol::from_json<PersistedData>(with_null);
    BOOST_TEST(!parsed.revision.has_value());
    // The key must survive a round trip instead of disappearing.
    BOOST_TEST(protocol::to_json(parsed) == with_null);

    Json numeric = with_null;
    numeric["revision"] = 7;
    const PersistedData numbered = protocol::from_json<PersistedData>(numeric);
    BOOST_TEST(numbered.revision.value() == 7);
}

BOOST_AUTO_TEST_CASE(payload_structure_is_separate_from_application_policy) {
    // Accepted: a plain message, even with an incidental start cursor.
    Json message = message_payload();
    BOOST_CHECK_NO_THROW(protocol::from_json<PayloadRequest>(message));
    Json tolerated = message_payload();
    tolerated["start"] = 0;
    BOOST_CHECK_NO_THROW(protocol::from_json<PayloadRequest>(tolerated));

    // Accepted: an answer query that omits part and offset.
    Json answer = Json{{"operation", "answer"},
                       {"request_id", "answer-1"},
                       {"source", source_json()}};
    BOOST_CHECK_NO_THROW(protocol::from_json<PayloadRequest>(answer));

    // Empty text is structurally valid; usefulness is application policy.
    Json empty_raw = message_payload();
    empty_raw["content"] = Json::array({part_json("")});
    BOOST_TEST(!rejects_payload(empty_raw));

    // Rejected: content extras that are not an object.
    Json array_extras = message_payload();
    array_extras["content"] = Json::array({Json{{"type", "text"}, {"modality", "text"},
                                                {"raw", "x"}, {"extras", Json::array()}}});
    BOOST_TEST(rejects_payload(array_extras));

    // Rejected: message metadata smuggled into the payload.
    for (const char* field : {"role", "invokes", "invoke_return", "type", "text"}) {
        Json polluted = message_payload();
        polluted[field] = "x";
        BOOST_TEST_MESSAGE("rejecting payload field: " << field);
        BOOST_TEST(rejects_payload(polluted));
    }

    // Accepted: fields that belong to another operation are ignored, whatever
    // their JSON type is; the worker never type-checks them for a message.
    Json ignored = message_payload();
    ignored["limit"] = 0;
    ignored["start"] = "ignored";
    ignored["source"] = Json::object();
    BOOST_CHECK_NO_THROW(protocol::from_json<PayloadRequest>(ignored));

    // Rejected: an answer query has a closed field set.
    Json closed = Json{{"operation", "answer"}, {"request_id", "a"},
                       {"source", source_json()}, {"unexpected", true}};
    BOOST_TEST(rejects_payload(closed));

    // Rejected: the locator is closed as well, so a source cannot smuggle fields
    // past the answer query's field set.
    Json polluted_source = Json{{"operation", "answer"}, {"request_id", "a"}};
    Json extra_source = source_json();
    extra_source["unexpected"] = true;
    polluted_source["source"] = extra_source;
    BOOST_TEST(rejects_payload(polluted_source));

    // Option categories and their values are application-owned.
    Json unknown_category = message_payload();
    unknown_category["options"] = Json{{"unknown", Json::object()}};
    BOOST_TEST(!rejects_payload(unknown_category));

    Json nonempty_tools = message_payload();
    nonempty_tools["options"] = Json{{"tools", Json{{"a", 1}}}};
    BOOST_TEST(!rejects_payload(nonempty_tools));

    Json empty_tools = message_payload();
    empty_tools["options"] = Json{{"tools", Json::object()}};
    BOOST_CHECK_NO_THROW(protocol::from_json<PayloadRequest>(empty_tools));

    // Rejected: continue and compact cannot carry content.
    for (const char* operation : {"continue", "compact"}) {
        Json with_content = message_payload();
        with_content["operation"] = operation;
        BOOST_TEST(rejects_payload(with_content));
    }

    // Rejected: history cannot carry content or options.
    Json history_content = Json{{"operation", "history"}, {"request_id", "h"},
                                {"content", Json::array({part_json()})}};
    BOOST_TEST(rejects_payload(history_content));
    Json history_options = Json{{"operation", "history"}, {"request_id", "h"},
                                {"options", Json{{"tools", Json::object()}}}};
    BOOST_TEST(rejects_payload(history_options));

    // Resource limits are checked by the application, not the protocol.
    Json wide = Json{{"operation", "history"}, {"request_id", "h"}, {"limit", 11}};
    BOOST_TEST(!rejects_payload(wide));
    Json narrow = Json{{"operation", "history"}, {"request_id", "h"}, {"limit", 0}};
    BOOST_TEST(!rejects_payload(narrow));

    // Identifier length is application policy.
    Json empty_id = message_payload();
    empty_id["request_id"] = "";
    BOOST_TEST(!rejects_payload(empty_id));
    Json long_id = message_payload();
    long_id["request_id"] = std::string(129, 'x');
    BOOST_TEST(!rejects_payload(long_id));
}

BOOST_AUTO_TEST_CASE(signal_cancel_requires_its_run_identity) {
    BOOST_CHECK_THROW(protocol::from_json<SignalRequest>(Json{{"operation", "cancel"}}),
                      ValidationError);
    BOOST_CHECK_NO_THROW(protocol::from_json<SignalRequest>(
        Json{{"operation", "cancel"}, {"run_id", "run-1"}}));
    BOOST_CHECK_THROW(protocol::from_json<SignalRequest>(
                          Json{{"operation", "status"}, {"run_id", "run-1"}}),
                      ValidationError);
}

BOOST_AUTO_TEST_CASE(tool_response_is_result_or_rejection_never_both) {
    Json base{{"worker_id", "w"}, {"session_id", "s"}, {"run_id", "r"},
              {"request_id", "q"}, {"route", "plan/read"}};
    Json both = base;
    both["status"] = "succeeded";
    both["result"] = Json{{"markdown", ""}, {"revision", 0}, {"updated_at", nullptr}};
    both["error"] = Json{{"code", "storage_error"}, {"message", "x"}};
    BOOST_CHECK_THROW(protocol::from_json<ToolResponse>(both), ValidationError);

    Json succeeded = base;
    succeeded["status"] = "succeeded";
    succeeded["result"] = Json{{"markdown", "- [ ] Work"}, {"revision", 1},
                               {"updated_at", "2026-09-29T00:00:00Z"}};
    BOOST_CHECK_NO_THROW(protocol::from_json<ToolResponse>(succeeded));
}

BOOST_AUTO_TEST_CASE(every_envelope_pins_its_own_discriminator) {
    const Json body = Json{{"data", Json{{"worker_id", "w"}, {"session_id", "s"}, {"run_id", "r"},
                                         {"confirmation_id", "c"}, {"decision", "approved"},
                                         {"reason", "ok"}}}};
    Json wrong = body;
    wrong["type"] = "payload";
    BOOST_CHECK_THROW(protocol::from_json<ConfirmationResponseEnvelope>(wrong), ValidationError);

    Json right = body;
    right["type"] = "confirmation_response";
    BOOST_CHECK_NO_THROW(protocol::from_json<ConfirmationResponseEnvelope>(right));

    Json unknown = body;
    unknown["type"] = "not_a_type";
    BOOST_CHECK_THROW(protocol::from_json<ConfirmationResponseEnvelope>(unknown), ValidationError);

    // A valid spelling from another family is still the wrong discriminator.
    Json tool = Json{{"type", "event"},
                     {"data", Json{{"worker_id", "w"}, {"session_id", "s"}, {"run_id", "r"},
                                   {"request_id", "q"}, {"arguments", Json::object()}}}};
    BOOST_CHECK_THROW(protocol::from_json<ToolRequestEnvelope>(tool), ValidationError);
    tool["type"] = "tool_request";
    BOOST_CHECK_NO_THROW(protocol::from_json<ToolRequestEnvelope>(tool));
}

BOOST_AUTO_TEST_CASE(event_registry_covers_every_event_name) {
    const auto specs = event_data_registry();
    BOOST_TEST(specs.size() == EnumTraits<EventName>::values.size());
    for (const auto& value : EnumTraits<EventName>::values) {
        const auto found = std::find_if(specs.begin(), specs.end(), [&](const EventDataSpec& spec) {
            return spec.event == value.value;
        });
        BOOST_TEST_MESSAGE("event: " << value.spelling);
        const bool present = found != specs.end();
        BOOST_TEST(present);
    }
}

BOOST_AUTO_TEST_CASE(producer_shapes_validate_without_loss) {
    EventEnvelope envelope;
    envelope.session_id = "demo";
    envelope.worker_id = "worker-1";

    // A returned batch is made of message projections, not bare results.
    envelope.event = EventName::tool_results;
    envelope.data = Json::array({tool_result_message()});
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));

    // With an omitted suffix appended by display_results().
    Json truncated_batch = Json::array({tool_result_message(),
                                        Json{{"display_omitted", true}, {"omitted_items", 3}}});
    envelope.data = truncated_batch;
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));

    // A committed response keeps its cursor fields and its call batch.
    envelope.event = EventName::model_response;
    envelope.data = response_projection();
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));

    // 65 calls: 64 represented plus one omission marker.
    Json many = response_projection();
    Json calls = Json::array();
    for (int index = 0; index < 64; ++index) {
        calls.push_back(Json{{"id", "call-" + std::to_string(index)},
                             {"name", "read_text"},
                             {"type", "read_only"},
                             {"security", "trusted"},
                             {"arguments", Json::object()}});
    }
    calls.push_back(Json{{"display_omitted", true}, {"omitted_items", 1}});
    many["invokes"] = calls;
    many["tool_calls"] = 65;
    envelope.data = many;
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));

    // An engaged but empty call array is what an empty invoke list projects to.
    Json empty_calls = response_projection();
    empty_calls["invokes"] = Json::array();
    empty_calls["tool_calls"] = 0;
    envelope.data = empty_calls;
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));

    // A model can propose null arguments; the projection must survive that.
    Json null_arguments = response_projection();
    null_arguments["invokes"] = Json::array({Json{{"id", "call-1"}, {"name", "read_text"},
                                                  {"type", "read_only"},
                                                  {"security", "default_deny"},
                                                  {"arguments", nullptr}}});
    envelope.data = null_arguments;
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));
    const auto parsed = protocol::from_json<ResponseProjection>(null_arguments);
    BOOST_TEST(parsed.invokes->at(0).at("arguments").is_null());
    BOOST_TEST(protocol::to_json(parsed)["invokes"].at(0).at("arguments").is_null());

    // An oversized summary travels as an omission object.
    envelope.event = EventName::compact_finished;
    envelope.data = Json{{"summary", Json{{"display_omitted", true},
                                          {"bytes", 40000},
                                          {"reason", "compact summary exceeds 32768 byte limit"}}},
                         {"removed_turns", 3},
                         {"revision", 2},
                         {"durable", true}};
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));

    // An automatic compaction adds its correlation fields.
    Json automatic = envelope.data;
    automatic["origin"] = "automatic";
    automatic["cycle"] = 2;
    automatic["call_id"] = "call-9";
    envelope.data = automatic;
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));

    // The status snapshot and an empty-object event still validate.
    envelope.event = EventName::status;
    envelope.data = Json{{"active", false}, {"stopping", false}, {"storage_failed", false},
                         {"rejected_payloads", 0}, {"capabilities", Json::array()}};
    BOOST_CHECK_NO_THROW(validate_event_data(envelope));
    envelope.event = EventName::run_started;
    envelope.data = Json::object();
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

    // A marker may only be the final batch element.
    envelope.event = EventName::tool_calls;
    envelope.data = Json::array({
        Json{{"display_omitted", true}, {"omitted_items", 1}},
        Json{{"id", "call-1"}, {"name", "read_text"}, {"arguments", Json::object()},
             {"type", "read_only"}, {"security", "trusted"}},
    });
    BOOST_CHECK_THROW(validate_event_data(envelope), ValidationError);

    // A summary must be a string or an omission object.
    envelope.event = EventName::compact_finished;
    envelope.data = Json{{"summary", 42}, {"removed_turns", 0}, {"revision", 0},
                         {"durable", true}};
    BOOST_CHECK_THROW(validate_event_data(envelope), ValidationError);
}


BOOST_AUTO_TEST_CASE(remote_tool_contents_belong_to_the_handler) {
    // These arguments are invalid for today's plan/read handler, but a generic
    // envelope parser must not impersonate that handler or a future Hub.
    const Json arguments{{"operation", "read"}, {"markdown", "handler decides"},
                         {"future_option", Json::array({1, false, nullptr})}};
    const Json request{{"worker_id", "w"}, {"session_id", "s"}, {"run_id", "r"},
                       {"request_id", "q"}, {"arguments", arguments}};
    BOOST_TEST(protocol::to_json(protocol::from_json<ToolRequest>(request)) == request);

    for (const std::string route : {"plan/read", "subagent/send", "future/extension"}) {
        // No lookup by route and no interpretation of result keys.
        const Json reply{{"worker_id", "w"}, {"session_id", "s"}, {"run_id", "r"},
                         {"request_id", "q"}, {"route", route}, {"status", "succeeded"},
                         {"result", Json{{"new_shape", arguments}}}};
        BOOST_TEST(protocol::to_json(protocol::from_json<ToolResponse>(reply)) == reply);
        Json rejected = reply;
        rejected.erase("result");
        rejected["status"] = "rejected";
        rejected["error"] = Json{{"code", "future_hub_policy"}, {"message", "refused"}};
        BOOST_TEST(protocol::to_json(protocol::from_json<ToolResponse>(rejected)) == rejected);
    }

    // The envelope contract still requires object containers and identifiers.
    Json malformed = request;
    malformed["arguments"] = Json::array();
    BOOST_TEST(rejects<ToolRequest>(malformed));
    malformed = request;
    malformed.erase("request_id");
    BOOST_TEST(rejects<ToolRequest>(malformed));
    const Json scalar_result{{"worker_id", "w"}, {"session_id", "s"}, {"run_id", "r"},
                             {"request_id", "q"}, {"route", "future/extension"},
                             {"status", "succeeded"}, {"result", 1}};
    BOOST_TEST(rejects<ToolResponse>(scalar_result));
}

BOOST_AUTO_TEST_CASE(options_are_preserved_for_runtime_validation) {
    Json request = message_payload();
    for (const Json options : {
             Json{{"confirmation", Json{{"mode", "future-mode"}, {"new_policy", true}}}},
             Json{{"confirmation", 42}, {"model", nullptr},
                  {"tools", Json{{"enabled", true}}}, {"future_category", "value"}}}) {
        request["options"] = options;
        const auto parsed = protocol::from_json<PayloadRequest>(request);
        BOOST_TEST(protocol::to_json(parsed).at("options") == options);
    }
    request["options"] = Json::array();
    BOOST_TEST(rejects_payload(request));
    request["options"] = Json::object();
    request["request_id"] = 42;
    BOOST_TEST(rejects_payload(request));
    request = Json{{"operation", "history"}, {"request_id", "h"}, {"limit", "10"}};
    BOOST_TEST(rejects_payload(request));
}
