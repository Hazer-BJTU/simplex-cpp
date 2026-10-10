#define BOOST_TEST_MODULE CoreProtocol
#include <boost/test/unit_test.hpp>

#include "core/protocol.hpp"
#include "llm/compat/chat_completions/interpreter.hpp"
#include <limits>
#include <filesystem>
#include "load/persistence.hpp"
#include <set>

using Json = nlohmann::json;

namespace {

/** A complete worker message envelope's data, without transport dependencies. */
Json payload(Json parts) {
    return {
        {"operation", "message"},
        {"request_id", "attachments-1"},
        {"content", std::move(parts)}
    };
}

Json text_part() {
    // Both labels are required by the input contract: the encoding and the
    // media category. Text is stated, never implied.
    return {{"type", "text"}, {"raw", "Describe these attachments."},
            {"modality", "text"}};
}

/** Exercise escaping and maximum-width metadata in both event wrappers. */
void check_history_size(const Json& page) {
    BOOST_TEST(page.dump().size() <= core::history_page_max_bytes);
    Json event = {{"type", "event"}, {"event", "history"},
        {"session_id", std::string(128, 's')},
        {"worker_id", "12345678-1234-1234-1234-123456789abc"},
        {"request_id", std::string(128, '\x01')},
        {"run_id", "12345678-1234-1234-1234-123456789abc"},
        {"sequence", std::numeric_limits<std::uint64_t>::max()}, {"data", page}};
    BOOST_TEST(event.dump().size() <= core::history_event_max_bytes);
    const auto raw_event = event;
    event["raw"] = raw_event;
    event["known"] = true;
    event["bytes"] = page.dump().size() + core::history_envelope_max_bytes;
    event["issues"] = Json::array();
    event["connection"] = {{"opened_at", "2026-10-04T00:00:00.000Z"},
        {"protocol_errors", 0}};
    event["hub_sequence"] = std::numeric_limits<std::uint64_t>::max();
    event["received_at"] = "2026-10-04T00:00:00.000Z";
    Json forwarded = {{"v", 1}, {"type", "event"},
        {"session", std::string(128, 's')},
        {"hub_seq", std::numeric_limits<std::uint64_t>::max()}, {"envelope", event}};
    BOOST_TEST(forwarded.dump().size() <= 2 * core::history_event_max_bytes);
}

model_io::Content display_text(std::string raw) {
    return {model_io::ContentType::Text, std::move(raw), {}, model_io::Modality::Text};
}

/** Small on the wire but deliberately larger than one diagnostic traversal. */
Json structured_arguments() {
    auto nodes = Json::array();
    for (std::size_t index = 0; index < 64; ++index) {
        nodes.push_back(Json{{"first", index}, {"second", index}});
    }
    return Json{{"nodes", std::move(nodes)}};
}

} // namespace

BOOST_AUTO_TEST_CASE(compact_display_preserves_summary_and_bounds_diagnostics) {
    for (const auto bytes : {8192u, 32768u}) {
        // Include multibyte UTF-8 and JSON escapes. The limit measures original
        // text, while the event budget also has to allow its encoded form.
        const std::string fragment = "目标🌍\n\"\\\x01";
        std::string summary;
        while (summary.size() + fragment.size() <= bytes) {
            summary += fragment;
        }
        summary.append(bytes - summary.size(), 'S');
        const Json source = {{"summary", summary}, {"durable", true},
            {"archive_cleanup_error", std::string(4096, 'E')}};
        const auto projected = core::display_compact(source);
        BOOST_TEST(projected.at("summary").get<std::string>() == summary);
        BOOST_TEST(projected.at("archive_cleanup_error").get<std::string>().size() == 1024u);
        BOOST_TEST(projected.at("display_truncated") == true);
        BOOST_TEST(source.at("archive_cleanup_error").get<std::string>().size() == 4096u);
        BOOST_TEST(projected.dump().size() < core::display_event_max_bytes);
        BOOST_TEST(Json::parse(projected.dump()).at("summary").get<std::string>() == summary);
        const auto clean = core::display_compact(Json{{"summary", summary}, {"durable", true}});
        BOOST_CHECK(!clean.contains("display_truncated"));
    }
    const auto oversized = core::display_compact(Json{
        {"summary", std::string(core::compact_summary_max_bytes + 1, 'S')}, {"durable", true}});
    BOOST_TEST(oversized.at("summary").at("display_omitted") == true);
    BOOST_TEST(oversized.at("summary").at("bytes") == core::compact_summary_max_bytes + 1);
    BOOST_TEST(core::display_compact(Json{{"summary", Json::array()}, {"durable", false}})
        .at("summary").is_array());
    BOOST_TEST(core::display_compact(Json{{"durable", false}}).at("durable") == false);
}

BOOST_AUTO_TEST_CASE(tool_display_preserves_batch_identity_and_outcomes_outside_body_budgets) {
    std::vector<model_io::InvokeQuery> calls;
    for (std::size_t index = 0; index < 3; ++index) {
        model_io::InvokeQuery call;
        call.id = "call-" + std::to_string(index);
        call.name = "structured_tool_" + std::to_string(index);
        call.type = model_io::InvokeType::SerialWrite;
        call.security = model_io::InvokeSecurity::RequireConfirm;
        call.arguments = structured_arguments();
        call.extras = Json{{"metadata", structured_arguments()}};
        calls.push_back(std::move(call));
    }
    const Json original_calls = calls;
    BOOST_TEST(original_calls.front().dump().size() < 4096u);
    const auto projected = core::display_calls(calls);
    BOOST_REQUIRE_EQUAL(projected.size(), calls.size());

    model_io::AgentInputState state;
    state.turns.resize(1);
    state.turns.front().agent_loop_step.resize(1);
    state.turns.front().agent_loop_step.front().model_response.invokes = calls;
    const auto response = core::project_response(state, 0, 0, "worker");
    BOOST_TEST(response.at("invokes") == projected);

    std::vector<model_io::MessageItem> messages;
    for (const auto& call : calls) {
        model_io::InvokeReturn record;
        record.query = call;
        record.output = display_text("Tool output 中文🌍");
        record.output.extras = Json{{"metadata", structured_arguments()}};
        record.extras = Json{{"aaa_metadata", structured_arguments()},
            {"status", "failed"}, {"loop_skipped", true},
            {"error", {{"stage", "invoke"}, {"message", "distinct failure reason"}}}};
        model_io::MessageItem message;
        message.role = "tool";
        message.type = model_io::MessageItemType::InvokeReturn;
        message.content = {record.output};
        message.invoke_return = std::move(record);
        messages.push_back(std::move(message));
    }
    const Json original_messages = messages;
    const auto results = core::display_results(messages);
    BOOST_REQUIRE_EQUAL(results.size(), calls.size());
    for (std::size_t index = 0; index < calls.size(); ++index) {
        const auto& call = projected.at(index);
        const auto& returned = results.at(index).at("invoke_return");
        for (const auto* key : {"id", "name", "security", "type"}) {
            BOOST_TEST(call.at(key) == original_calls.at(index).at(key));
            BOOST_TEST(returned.at("query").at(key) == call.at(key));
        }
        BOOST_TEST(returned.at("output").at("raw") == "Tool output 中文🌍");
        BOOST_TEST(returned.at("output").at("type") == "text");
        BOOST_TEST(returned.at("extras").at("status") == "failed");
        BOOST_TEST(returned.at("extras").at("loop_skipped") == true);
        BOOST_TEST(returned.at("extras").at("error").at("stage") == "invoke");
        BOOST_TEST(returned.at("extras").at("error").at("message") == "distinct failure reason");
    }
    BOOST_TEST(Json(calls) == original_calls);
    BOOST_TEST(Json(messages) == original_messages);
}

BOOST_AUTO_TEST_CASE(tool_display_batches_bound_escaped_bodies_and_mark_omitted_entries) {
    std::vector<model_io::InvokeQuery> calls(100);
    std::vector<model_io::MessageItem> messages(100);
    for (std::size_t index = 0; index < calls.size(); ++index) {
        calls[index].id = "call-" + std::to_string(index);
        calls[index].name = "tool";
        calls[index].arguments = Json{{"long", std::string(65536, '\x01')}};
        model_io::InvokeReturn record;
        record.query = calls[index];
        record.output = display_text(std::string(65536, '\x01'));
        record.extras = Json{{"error", {{"stage", "invoke"},
            {"message", std::string(65536, '\x01')}}}};
        messages[index].content = {record.output};
        messages[index].invoke_return = std::move(record);
    }
    const auto proposed = core::display_calls(calls);
    const auto returned = core::display_results(messages);
    BOOST_TEST(proposed.at(63).at("id") == "call-63");
    BOOST_TEST(returned.at(63).at("invoke_return").at("query").at("id") == "call-63");
    BOOST_TEST(proposed.back().at("omitted_items") == 36);
    BOOST_TEST(returned.back().at("omitted_items") == 36);
    BOOST_TEST(proposed.dump().size() < 128 * 1024u);
    BOOST_TEST(returned.dump().size() < 512 * 1024u);
    BOOST_TEST(returned.at(0).at("invoke_return").at("output").at("truncated") == true);
    BOOST_TEST(returned.at(0).at("invoke_return").at("extras").at("error").at("display_truncated") == true);
}

BOOST_AUTO_TEST_CASE(ordered_content_and_metadata_survive_message_serialization) {
    const auto request = payload(Json::array({
        text_part(),
        {{"type", "external_ref"}, {"raw", "https://example.com/photo.png"},
         {"modality", "image"}, {"extras", {{"detail", "low"}}}},
        {{"type", "binary"}, {"raw", "AAEC"}, {"modality", "video"},
         {"extras", {{"mime_type", "video/mp4"}}}}
    }));
    const auto original = request;
    const auto input = core::parse_input(request);
    BOOST_TEST(input.has_message);
    BOOST_TEST(input.message.role == "user");
    BOOST_CHECK(input.message.type == model_io::MessageItemType::UserInput);
    BOOST_REQUIRE_EQUAL(input.message.content.size(), 3u);
    BOOST_TEST(input.message.content[0].raw == "Describe these attachments.");
    BOOST_CHECK(input.message.content[1].type == model_io::ContentType::ExternalRef);
    BOOST_CHECK(input.message.content[2].type == model_io::ContentType::Binary);
    // The category is carried through unchanged and independently of the
    // encoding: a base64 payload can be a video, a reference can be an image.
    BOOST_CHECK(input.message.content[1].modality == model_io::Modality::Image);
    BOOST_CHECK(input.message.content[2].modality == model_io::Modality::Video);
    BOOST_TEST(input.message.content[2].raw == "AAEC");
    BOOST_CHECK(!input.message.content[0].extras);
    BOOST_TEST(input.message.content[1].extras->at("detail") == "low");
    BOOST_TEST(input.message.content[2].extras->at("mime_type") == "video/mp4");
    const Json serialized = input.message;
    const auto restored = serialized.get<model_io::MessageItem>();
    BOOST_TEST(Json(restored) == serialized);
    BOOST_TEST(request == original);
}

BOOST_AUTO_TEST_CASE(attachment_only_input_is_admitted) {
    const auto input = core::parse_input(payload(Json::array({{
        {"type", "external_ref"}, {"raw", "https://example.com/photo.png"},
        {"modality", "image"}
    }})));
    BOOST_REQUIRE_EQUAL(input.message.content.size(), 1u);
    BOOST_CHECK(!input.message.content.front().extras);
    BOOST_CHECK(input.message.content.front().modality == model_io::Modality::Image);
    BOOST_TEST(input.message.content.front().raw == "https://example.com/photo.png");
}

// The boundary admits every defined category, including the ones no current
// adapter can send: what the worker may carry is the contract's business, what
// a provider can receive is the adapter's.
BOOST_AUTO_TEST_CASE(every_modality_label_is_admitted_and_carried_through) {
    const std::pair<const char*, model_io::Modality> cases[] = {
        {"text", model_io::Modality::Text},
        {"image", model_io::Modality::Image},
        {"audio", model_io::Modality::Audio},
        {"video", model_io::Modality::Video},
        {"document", model_io::Modality::Document},
    };
    for (const auto& [name, expected] : cases) {
        auto part = text_part();
        part["modality"] = name;
        const auto input = core::parse_input(payload(Json::array({part})));
        BOOST_REQUIRE_EQUAL(input.message.content.size(), 1u);
        BOOST_CHECK(input.message.content.front().modality == expected);
    }
}

BOOST_AUTO_TEST_CASE(invalid_content_is_rejected_without_mutating_the_input) {
    for (const auto& parts : {Json(nullptr), Json::object(), Json("text"),
                              Json::array(), Json::array({"text"})}) {
        BOOST_CHECK_THROW(core::parse_input(payload(parts)), std::invalid_argument);
    }
    for (const auto* field : {"type", "raw", "modality"}) {
        auto part = text_part();
        part.erase(field);
        BOOST_CHECK_THROW(core::parse_input(payload(Json::array({part}))), Json::exception);
        for (const auto& value : {Json(nullptr), Json(1), Json::object()}) {
            part = text_part();
            part[field] = value;
            BOOST_CHECK_THROW(core::parse_input(payload(Json::array({part}))), Json::exception);
        }
        part = text_part();
        part[field] = "";
        BOOST_CHECK_THROW(core::parse_input(payload(Json::array({part}))), std::invalid_argument);
    }
    auto part = text_part();
    part["type"] = "image";
    BOOST_CHECK_THROW(core::parse_input(payload(Json::array({part}))), std::invalid_argument);
    // The category label is strict: a type is never read as a modality, so a
    // misspelled or invented category is rejected instead of becoming text.
    for (const auto* label : {"image_url", "imgae", "Text", "application/pdf"}) {
        part = text_part();
        part["modality"] = label;
        BOOST_CHECK_THROW(core::parse_input(payload(Json::array({part}))), std::invalid_argument);
    }
    for (const auto& value : {Json(nullptr), Json::array(), Json("metadata")}) {
        part = text_part();
        part["extras"] = value;
        BOOST_CHECK_THROW(core::parse_input(payload(Json::array({part}))), std::invalid_argument);
    }
    auto request = payload(Json::array({text_part(), {{"type", "unknown"}}}));
    const auto original = request;
    BOOST_CHECK_THROW(core::parse_input(request), std::invalid_argument);
    BOOST_TEST(request == original);
    request = payload(Json::array({text_part()}));
    request["text"] = "ambiguous legacy input";
    BOOST_CHECK_THROW(core::parse_input(request), std::invalid_argument);
    request.erase("content");
    BOOST_CHECK_THROW(core::parse_input(request), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(continuation_has_no_new_content) {
    const auto input = core::parse_input({{"operation", "continue"}, {"request_id", "next"}});
    BOOST_TEST(!input.has_message);
    BOOST_TEST(input.message.content.empty());
    for (const char* field : {"content", "text"}) {
        auto request = Json{{"operation", "continue"}, {"request_id", "next"}};
        for (const auto& value : {Json(nullptr), Json("ignored"),
                                  Json::array({text_part()})}) {
            request[field] = value;
            const auto original = request;
            BOOST_CHECK_THROW(core::parse_input(request), std::invalid_argument);
            BOOST_TEST(request == original);
        }
    }
}

BOOST_AUTO_TEST_CASE(worker_text_and_image_reach_chat_completions_in_order) {
    const auto input = core::parse_input(payload(Json::array({
        text_part(),
        {{"type", "external_ref"}, {"raw", "https://example.com/photo.png"},
         {"modality", "image"}, {"extras", {{"detail", "low"}}}}
    })));
    model_io::AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input = input.message;
    state.turns.push_back(std::move(turn));
    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "https://example.com";
    endpoint.request_path = "/v1/chat/completions";
    endpoint.auth.scheme = model_io::AuthScheme::None;
    const auto request = llm::chat_completions::ChatCompletionsInterpreter{}.build_request(
        state, endpoint, {{"model", "fixture"}});
    const auto parts = Json::parse(request.body()).at("messages").at(0).at("content");
    BOOST_REQUIRE_EQUAL(parts.size(), 2u);
    BOOST_TEST(parts[0]["type"] == "text");
    BOOST_TEST(parts[0]["text"] == "Describe these attachments.");
    BOOST_TEST(parts[1]["type"] == "image_url");
    BOOST_TEST(parts[1]["image_url"]["url"] == "https://example.com/photo.png");
    BOOST_TEST(parts[1]["image_url"]["detail"] == "low");
}

BOOST_AUTO_TEST_CASE(payload_options_validate_all_categories_without_mutation) {
    auto message = payload(Json::array({text_part()}));
    BOOST_TEST(core::parse_input(message).options == Json::object());
    const Json options = {
        {"model", {{"model", "deepseek-v4-pro"}}},
        {"tools", Json::object()}, {"confirmation", {{"mode", "deny"}}}
    };
    message["options"] = options;
    BOOST_TEST(core::parse_input(message).options == options);
    message["operation"] = "continue";
    message.erase("content");
    BOOST_TEST(core::parse_input(message).options == options);
    for (const auto& bad : std::vector<Json>{
        nullptr, Json::array(), {{"model", "name"}},
        {{"tools", {{"enabled", true}}}}, {{"confirmation", false}},
        {{"unknown", Json::object()}}
    }) {
        message["options"] = bad;
        const auto before = message;
        BOOST_CHECK_THROW(core::parse_input(message), core::InputOptionsError);
        BOOST_TEST(message == before);
    }
}

BOOST_AUTO_TEST_CASE(history_query_projects_turns_without_tool_data_or_binary_bytes) {
    const Json query = {{"operation", "history"}, {"request_id", "history-1"},
        {"start", 0}, {"limit", 1}};
    const auto request = core::parse_history_request(query);
    model_io::AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.content = {
        {model_io::ContentType::Text, "hello", {}, model_io::Modality::Text},
        {model_io::ContentType::Binary, "AAEC", {}, model_io::Modality::Video}
    };
    model_io::AgentLoopStep step;
    step.model_response.content = {
        {model_io::ContentType::Text, "answer", {}, model_io::Modality::Text}};
    turn.agent_loop_step.push_back(std::move(step));
    state.turns.push_back(std::move(turn));
    const auto page = core::history_page(state, request);
    BOOST_TEST(page.at("request_id") == "history-1");
    BOOST_TEST(page.at("total") == 1);
    BOOST_TEST(page.at("next") == 1);
    BOOST_TEST(page.at("turns")[0]["user"][0]["raw"] == "hello");
    // The projection carries both labels to the panel.
    BOOST_TEST(page.at("turns")[0]["user"][0]["modality"] == "text");
    BOOST_TEST(page.at("turns")[0]["user"][1]["modality"] == "video");
    BOOST_TEST(page.at("turns")[0]["user"][1]["omitted"] == true);
    BOOST_TEST(page.dump().find("AAEC") == std::string::npos);
    BOOST_TEST(page.dump().find("system_prompt") == std::string::npos);
    BOOST_TEST(page.at("turns")[0]["steps"][0]["content"][0]["raw"] == "answer");
    BOOST_CHECK_THROW(core::parse_history_request({{"operation", "history"},
        {"request_id", "bad"}, {"options", Json::object()}}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(history_query_pages_within_a_long_turn) {
    model_io::AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.content = {
        {model_io::ContentType::Text, "question", {}, model_io::Modality::Text}};
    for (int index = 0; index < 90; ++index) {
        model_io::AgentLoopStep step;
        step.model_response.content = {{model_io::ContentType::Text,
            std::string(4000, 'a'), {}, model_io::Modality::Text}};
        turn.agent_loop_step.push_back(std::move(step));
    }
    state.turns.push_back(std::move(turn));
    auto request = core::parse_history_request({{"operation", "history"},
        {"request_id", "long-turn"}, {"start", 0}, {"limit", 1}});
    const auto first = core::history_page(state, request);
    BOOST_TEST(first.at("next") == 0);
    BOOST_TEST(first.at("next_step") > 0);
    request.step = first.at("next_step").get<std::size_t>();
    const auto second = core::history_page(state, request);
    BOOST_TEST(second.at("next") == 1);
    BOOST_TEST(second.at("next_step") == 0);
    BOOST_TEST(first.at("turns")[0]["steps"].size()
        + second.at("turns")[0]["steps"].size() == 90);
    check_history_size(first);
    check_history_size(second);
}

BOOST_AUTO_TEST_CASE(history_query_bounds_escaped_user_only_turns_and_metadata) {
    model_io::AgentInputState state;
    const std::string raw(4096, '\x01');
    for (int index = 0; index < 10; ++index) {
        model_io::UserLoopStep turn;
        turn.user_input.content.assign(5, display_text(raw));
        state.turns.push_back(std::move(turn));
    }
    const Json original = state;
    auto request = core::parse_history_request({{"operation", "history"},
        {"request_id", std::string(128, '\x02')}, {"limit", 10}});
    std::size_t seen = 0;
    std::size_t pages = 0;
    do {
        const auto page = core::history_page(
            state, request, std::numeric_limits<std::uint64_t>::max());
        check_history_size(page);
        BOOST_TEST(page.at("revision") == std::numeric_limits<std::uint64_t>::max());
        BOOST_REQUIRE(!page.at("turns").empty());
        for (const auto& turn : page.at("turns")) {
            BOOST_TEST(turn.at("index") == seen++);
            BOOST_TEST(turn.at("omitted_user_parts") == 1);
            BOOST_TEST(turn.at("omitted_steps") == 0);
            BOOST_TEST(turn.at("steps").empty());
            BOOST_TEST(turn.at("user").size() == 4u);
            for (const auto& part : turn.at("user")) {
                BOOST_TEST(part.at("raw") == raw);
                BOOST_TEST(!part.contains("truncated"));
            }
        }
        const auto next = page.at("next").get<std::size_t>();
        BOOST_REQUIRE(next > request.start);
        BOOST_TEST(page.at("next_step") == 0);
        request.start = next;
        ++pages;
    } while (request.start < state.turns.size());
    BOOST_TEST(seen == state.turns.size());
    BOOST_TEST(pages > 1u);
    BOOST_TEST(Json(state) == original);
    check_history_size(core::history_page(state, request));
}

BOOST_AUTO_TEST_CASE(history_query_preserves_every_step_across_mixed_page_boundaries) {
    model_io::AgentInputState state;
    std::string multibyte;
    for (int index = 0; index < 1500; ++index) multibyte += "\xe4\xb8\xad";
    for (int index = 0; index < 6; ++index) {
        model_io::UserLoopStep turn;
        turn.user_input.content.assign(4, display_text(std::string(4096, '\x01')));
        // Include empty-step turns, normal small steps, and worst-case steps
        // with four escaped content parts plus a full escaped reasoning part.
        for (int step_index = 0; step_index < index % 3; ++step_index) {
            model_io::AgentLoopStep step;
            step.model_response.content.assign(5, display_text(
                index == 2 ? multibyte : std::string(4096, '\x02')));
            step.model_response.reasoning = display_text(std::string(4097, '\x03'));
            turn.agent_loop_step.push_back(std::move(step));
        }
        state.turns.push_back(std::move(turn));
    }
    auto request = core::parse_history_request({{"operation", "history"},
        {"request_id", "mixed"}, {"limit", 10}});
    std::set<std::pair<std::size_t, std::size_t>> seen;
    std::set<std::size_t> seen_turns;
    bool continued_turn = false;
    for (std::size_t pages = 0; request.start < state.turns.size(); ++pages) {
        BOOST_REQUIRE(pages < 20u);
        const auto page = core::history_page(state, request, 17);
        check_history_size(page);
        for (const auto& turn : page.at("turns")) {
            const auto index = turn.at("index").get<std::size_t>();
            seen_turns.insert(index);
            const auto& source = state.turns.at(index);
            const auto& steps = turn.at("steps");
            std::size_t expected_step = index == request.start ? request.step : 0;
            for (const auto& step : steps) {
                BOOST_TEST(step.at("index") == expected_step);
                BOOST_TEST(seen.emplace(index, expected_step++).second);
                BOOST_TEST(step.at("content").size() + step.at("omitted_parts").get<std::size_t>() == 5u);
                BOOST_TEST(step.at("reasoning").at("truncated") == true);
                for (const auto& part : step.at("content")) {
                    const auto raw = part.at("raw").get<std::string>();
                    const auto& original = source.agent_loop_step.at(expected_step - 1).model_response.content.front().raw;
                    BOOST_TEST(raw == original.substr(0, raw.size()));
                    BOOST_TEST(part.value("truncated", false) == (raw.size() < original.size()));
                }
            }
            BOOST_TEST(turn.at("omitted_steps") == source.agent_loop_step.size() - expected_step);
        }
        const auto next = page.at("next").get<std::size_t>();
        const auto next_step = page.at("next_step").get<std::size_t>();
        BOOST_REQUIRE(next > request.start || (next == request.start && next_step > request.step));
        continued_turn = continued_turn || next_step != 0;
        request.start = next;
        request.step = next_step;
    }
    BOOST_TEST(seen_turns.size() == state.turns.size());
    BOOST_TEST(seen.size() == 6u);
    BOOST_TEST(continued_turn);
}

BOOST_AUTO_TEST_CASE(history_query_fits_one_maximal_entry_and_handles_terminal_cursors) {
    model_io::AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.content.assign(4, display_text(std::string(4096, '\x01')));
    model_io::AgentLoopStep step;
    step.model_response.content.assign(4, display_text(std::string(4096, '\x02')));
    step.model_response.reasoning = display_text(std::string(4096, '\x03'));
    turn.agent_loop_step.push_back(std::move(step));
    state.turns.push_back(std::move(turn));
    core::HistoryRequest request{std::string(128, '\x04'), 0, 0, 10};
    const auto page = core::history_page(state, request);
    check_history_size(page);
    BOOST_TEST(page.at("next") == 1);
    BOOST_TEST(page.at("next_step") == 0);
    BOOST_TEST(page.at("turns")[0].at("steps").size() == 1u);
    request.step = 1;
    const auto end_of_turn = core::history_page(state, request);
    check_history_size(end_of_turn);
    BOOST_TEST(end_of_turn.at("next") == 1);
    BOOST_TEST(end_of_turn.at("turns")[0].at("steps").empty());
    request.step = 2;
    BOOST_CHECK_THROW(core::history_page(state, request), std::invalid_argument);
    request.start = 1;
    request.step = 0;
    const auto terminal = core::history_page(state, request);
    check_history_size(terminal);
    BOOST_TEST(terminal.at("turns").empty());
    BOOST_TEST(terminal.at("next") == 1);
}

BOOST_AUTO_TEST_CASE(history_hides_only_host_owned_internal_input_and_keeps_responses) {
    model_io::AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.content.push_back(display_text("PRIVATE RESUME"));
    turn.user_input.extras = Json{{"simplex.internal_input", "auto_compact_continue"},
        {"simplex.source", {{"worker_id", "worker"}, {"run_id", "run"}, {"request_id", "request"}}}};
    model_io::AgentLoopStep step;
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.content.push_back(display_text("visible answer"));
    step.commit_sequence = 7;
    step.extras = Json{{"simplex.execution", {{"worker_id", "restarted-worker"},
        {"run_id", "continued-run"}, {"request_id", "continued-request"}}}};
    turn.agent_loop_step.push_back(step);
    // A new execution may append to a turn created before cancellation/restart.
    // Serialization must retain both identities without exposing the instruction.
    state.turns.push_back(turn);
    state = Json(state).get<model_io::AgentInputState>();
    core::HistoryRequest request;
    request.request_id = "query";
    const auto page = core::history_page(state, request, 1);
    BOOST_TEST(page.dump().find("PRIVATE RESUME") == std::string::npos);
    BOOST_TEST(page["turns"][0]["user"].empty());
    BOOST_TEST(page["turns"][0]["source"]["run_id"] == "run");
    BOOST_TEST(page["turns"][0]["steps"][0]["execution"]["run_id"] == "continued-run");
    BOOST_TEST(page["turns"][0]["steps"][0]["execution"]["worker_id"] == "restarted-worker");
    BOOST_TEST(page["turns"][0]["steps"][0]["commit_sequence"] == "7");
    BOOST_TEST(page["turns"][0]["steps"][0]["content"][0]["raw"] == "visible answer");
    state.turns[0].user_input.extras->erase("simplex.internal_input");
    const auto ordinary = core::history_page(state, request, 1);
    BOOST_TEST(ordinary["turns"][0]["user"][0]["raw"] == "PRIVATE RESUME");
    BOOST_TEST(ordinary["turns"][0]["source"]["request_id"] == "request");
    BOOST_TEST(ordinary["turns"][0]["steps"][0]["execution"]["request_id"] == "continued-request");
    BOOST_TEST(!ordinary["turns"][0].contains("internal_input"));
    state.turns[0].user_input.content[0].extras = state.turns[0].user_input.extras;
    state.turns[0].user_input.extras.reset();
    BOOST_TEST(core::history_page(state, request, 1)["turns"][0]["user"][0]["raw"] == "PRIVATE RESUME");
}

BOOST_AUTO_TEST_CASE(answer_pages_preserve_large_multipart_state_and_disk_snapshot) {
    model_io::AgentInputState state;
    state.turns.resize(1);
    auto& steps = state.turns.front().agent_loop_step;
    steps.resize(1);
    auto& step = steps.front();
    step.commit_sequence = 7;
    for (int index = 0; index < 8; ++index) {
        std::string text = std::to_string(index) + ":";
        for (int count = 0; count < 40000; ++count) text += "中文🌍\\\"\n";
        step.model_response.content.push_back(display_text(text));
    }
    step.model_response.reasoning = display_text(std::string(3 * 1024 * 1024, 'r'));
    step.model_response.extras = {{"native", std::string(1024 * 1024, 'n')}};
    const Json original = state;
    const auto live = core::project_response(state, 0, 0, "worker-one");
    BOOST_TEST(live.dump().size() < core::display_event_max_bytes);
    BOOST_TEST(!live.contains("extras"));
    BOOST_TEST(live.at("reasoning").at("truncated") == true);
    const auto source = live.at("answer_source");
    Json query = {{"operation", "answer"}, {"request_id", "read"},
        {"source", source}, {"part", 0}, {"offset", 0}};
    std::vector<std::string> assembled(8);
    for (std::size_t pages = 0;; ++pages) {
        BOOST_REQUIRE(pages < 1000u);
        const auto page = core::answer_page(state, query, "worker-one");
        BOOST_TEST(page.dump().size() < core::history_page_max_bytes);
        BOOST_TEST(page.at("offset") == query.at("offset"));
        const auto part = query.at("part").get<std::size_t>();
        assembled.at(part) += page.at("raw").get<std::string>();
        if (page.at("done") == true) break;
        query["offset"] = page.at("next_part") == query.at("part") ? page.at("next_offset") : Json(0);
        query["part"] = page.at("next_part");
    }
    for (std::size_t index = 0; index < assembled.size(); ++index) {
        BOOST_TEST(assembled[index] == step.model_response.content[index].raw);
    }
    BOOST_TEST(Json(state) == original);
    const auto file = std::filesystem::temp_directory_path() / ("simplex-answer-" + core::new_identity() + ".json");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{file};
    load::save_state(file, state);
    const auto restored = load::load_state(file);
    BOOST_TEST(Json(restored) == original);
    BOOST_CHECK_THROW(core::answer_page(restored, query, "different-worker"), std::invalid_argument);
    query["source"]["commit_sequence"] = "8";
    BOOST_CHECK_THROW(core::answer_page(state, query, "worker-one"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(answer_queries_reject_invalid_offsets_and_expired_commits) {
    model_io::AgentInputState state;
    state.turns.resize(1);
    state.turns[0].agent_loop_step.resize(1);
    auto& step = state.turns[0].agent_loop_step[0];
    step.commit_sequence = 1;
    step.model_response.content = {display_text("🌍hello")};
    Json query = {{"operation", "answer"}, {"request_id", "read"},
        {"source", core::answer_source(step, 0, 0, "worker")},
        {"part", 0}, {"offset", 1}};
    BOOST_CHECK_THROW(core::answer_page(state, query, "worker"), std::invalid_argument);
    query["offset"] = 0;
    query["options"] = Json::object();
    BOOST_CHECK_THROW(core::answer_page(state, query, "worker"), std::invalid_argument);
    query.erase("options");
    const auto initial = core::answer_page(state, query, "worker");
    BOOST_TEST(initial.at("raw") == "🌍hello");
    step.model_response.content[0].raw = "🌍other"; // Equal byte length; offsets alone cannot detect this edit.
    BOOST_CHECK_THROW(core::answer_page(state, query, "worker"), std::invalid_argument);
    query["source"] = core::answer_source(step, 0, 0, "worker");
    BOOST_TEST(core::answer_page(state, query, "worker").at("raw") == "🌍other");
    state.turns.clear();
    BOOST_CHECK_THROW(core::answer_page(state, query, "worker"), std::invalid_argument);
}
