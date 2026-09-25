#define BOOST_TEST_MODULE CoreProtocol
#include <boost/test/unit_test.hpp>

#include "core/protocol.hpp"
#include "llm/compat/chat_completions/interpreter.hpp"

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
    return {{"type", "text"}, {"raw", "Describe these attachments."}};
}

} // namespace

BOOST_AUTO_TEST_CASE(ordered_content_and_metadata_survive_message_serialization) {
    const auto request = payload(Json::array({
        text_part(),
        {{"type", "external_ref"}, {"raw", "https://example.com/photo.png"},
         {"extras", {{"detail", "low"}}}},
        {{"type", "binary"}, {"raw", "AAEC"},
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
        {"type", "external_ref"}, {"raw", "https://example.com/photo.png"}
    }})));
    BOOST_REQUIRE_EQUAL(input.message.content.size(), 1u);
    BOOST_CHECK(!input.message.content.front().extras);
    BOOST_TEST(input.message.content.front().raw == "https://example.com/photo.png");
}

BOOST_AUTO_TEST_CASE(invalid_content_is_rejected_without_mutating_the_input) {
    for (const auto& parts : {Json(nullptr), Json::object(), Json("text"),
                              Json::array(), Json::array({"text"})}) {
        BOOST_CHECK_THROW(core::parse_input(payload(parts)), std::invalid_argument);
    }
    for (const auto* field : {"type", "raw"}) {
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
}

BOOST_AUTO_TEST_CASE(worker_text_and_image_reach_chat_completions_in_order) {
    const auto input = core::parse_input(payload(Json::array({
        text_part(),
        {{"type", "external_ref"}, {"raw", "https://example.com/photo.png"},
         {"extras", {{"detail", "low"}}}}
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
        {"tools", Json::object()}, {"confirmation", Json::object()}
    };
    message["options"] = options;
    BOOST_TEST(core::parse_input(message).options == options);
    message["operation"] = "continue";
    BOOST_TEST(core::parse_input(message).options == options);
    for (const auto& bad : std::vector<Json>{
        nullptr, Json::array(), {{"model", "name"}},
        {{"tools", {{"enabled", true}}}}, {{"confirmation", false}},
        {{"unknown", Json::object()}}
    }) {
        message["options"] = bad;
        const auto before = message;
        BOOST_CHECK_THROW(core::parse_input(message), std::invalid_argument);
        BOOST_TEST(message == before);
    }
}
