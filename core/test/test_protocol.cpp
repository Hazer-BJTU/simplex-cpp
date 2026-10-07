#define BOOST_TEST_MODULE CoreProtocol
#include <boost/test/unit_test.hpp>

#include "core/protocol.hpp"
#include "llm/compat/chat_completions/interpreter.hpp"
#include <limits>
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

} // namespace

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
                BOOST_TEST(step.at("omitted_parts") == 1);
                BOOST_TEST(step.at("reasoning").at("truncated") == true);
                for (const auto& part : step.at("content")) {
                    if (index == 2) {
                        BOOST_TEST(part.at("raw") == multibyte.substr(0, 4095));
                        BOOST_TEST(part.at("truncated") == true);
                    } else {
                        BOOST_TEST(part.at("raw") == std::string(4096, '\x02'));
                        BOOST_TEST(!part.contains("truncated"));
                    }
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
    turn.agent_loop_step.push_back(step);
    state.turns.push_back(turn);
    core::HistoryRequest request;
    request.request_id = "query";
    const auto page = core::history_page(state, request, 1);
    BOOST_TEST(page.dump().find("PRIVATE RESUME") == std::string::npos);
    BOOST_TEST(page["turns"][0]["user"].empty());
    BOOST_TEST(page["turns"][0]["source"]["run_id"] == "run");
    BOOST_TEST(page["turns"][0]["steps"][0]["content"][0]["raw"] == "visible answer");
    state.turns[0].user_input.content[0].extras = state.turns[0].user_input.extras;
    state.turns[0].user_input.extras.reset();
    BOOST_TEST(core::history_page(state, request, 1)["turns"][0]["user"][0]["raw"] == "PRIVATE RESUME");
}
