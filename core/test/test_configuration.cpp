#define BOOST_TEST_MODULE CoreConfiguration
#include <boost/test/unit_test.hpp>
#include "load/configuration.hpp"
#include "core/protocol.hpp"
#include <cstdlib>

using Json = nlohmann::json;
namespace {
Json configuration() {
    return {{"providers", {{"local", {{"plugin", "deepseek"}, {"model", "fixture"},
        {"endpoint", {{"auth", {{"api_key", "literal"}}}}}}}}},
        {"driver_model", "local"}, {"client", {{"endpoint", "ws://localhost:8765/agent/events"}}}};
}
}

BOOST_AUTO_TEST_CASE(selected_provider_and_independent_endpoints) {
    auto value = configuration();
    value["providers"]["unused"] = {{"endpoint", {{"auth", {{"api_key", "${NEVER_DEFINED_CORE_TEST}"}}}}}};
    value["security"]["confirmation"] = {{"endpoint", "wss://confirm.example:9443/approve?mode=one"},
        {"timeout_ms", 50}};
    value["unknown_future"] = true;
    auto config = load::parse_configuration(value, "/tmp/config");
    BOOST_TEST(config.provider == "deepseek");
    BOOST_TEST(config.client.host == "localhost");
    BOOST_TEST(config.client.target == "/agent/events");
    BOOST_REQUIRE(config.confirmation);
    BOOST_TEST(config.confirmation->host == "confirm.example");
    BOOST_TEST(config.confirmation->target == "/approve?mode=one");
    BOOST_TEST(config.confirmation_timeout.count() == 50);
    BOOST_TEST(config.storage == "/tmp/config/data/sessions");
    BOOST_CHECK(!load::parse_configuration(configuration(), "/tmp").confirmation);
}

BOOST_AUTO_TEST_CASE(validation_and_expansion_do_not_leak_credentials) {
    auto value = configuration();
    value["providers"]["local"]["endpoint"]["auth"]["api_key"] = "${CORE_TEST_MISSING_KEY}";
    BOOST_CHECK_THROW(load::parse_configuration(value, "/tmp"), std::invalid_argument);
    value["providers"]["local"]["endpoint"]["auth"]["api_key"] = "$$${CORE_TEST_VALUE}";
    ::setenv("CORE_TEST_VALUE", "secret-value", 1);
    auto parsed = load::parse_configuration(value, "/tmp");
    BOOST_TEST(parsed.model["endpoint"]["auth"]["api_key"] == "$secret-value");
    ::unsetenv("CORE_TEST_VALUE");
    for (const auto url : {"http://localhost", "ws://", "ws://user@host/x", "ws://host/x#fragment"}) {
        BOOST_CHECK_THROW(load::websocket_endpoint(url), std::invalid_argument);
    }
    for (const auto invalid : {Json(0), Json(-1), Json("64"), Json(nullptr), Json(1.2)}) {
        auto bad = configuration();
        bad["client"]["payload_capacity"] = invalid;
        BOOST_CHECK_THROW(load::parse_configuration(bad, "/tmp"), std::invalid_argument);
    }
    value = configuration();
    value["security"]["confirmation"] = {{"endpoint", "ws://localhost/confirm"}, {"timeout_ms", 0}};
    BOOST_CHECK_THROW(load::parse_configuration(value, "/tmp"), std::invalid_argument);
    value["security"]["confirmation"] = Json::object();
    BOOST_CHECK_THROW(load::parse_configuration(value, "/tmp"), std::invalid_argument);
    value = configuration();
    value["providers"]["local"]["config"]["endpoint"] = Json::object();
    BOOST_CHECK_THROW(load::parse_configuration(value, "/tmp"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(protocol_never_admits_fabricated_tool_messages) {
    for (const auto id : {"", ".", "..", "a/b", "../x", "a.b", "x y"})
        BOOST_CHECK_THROW(core::validate_session_id(id), std::invalid_argument);
    core::validate_session_id("session_01-demo");
    Json payload = {
        {"operation", "message"},
        {"request_id", "input-1"},
        {"content", Json::array({{
            {"type", "text"}, {"raw", "hello"}
        }})}
    };
    auto input = core::parse_input(payload);
    BOOST_TEST(input.message.role == "user");
    BOOST_TEST(input.message.content.front().raw == "hello");
    for (const char* key : {"role", "invokes", "invoke_return", "type"}) {
        auto bad = payload;
        bad[key] = "tool";
        BOOST_CHECK_THROW(core::parse_input(bad), std::invalid_argument);
    }
    payload["operation"] = "continue";
    BOOST_CHECK_THROW(core::parse_input(payload), std::invalid_argument);
    payload.erase("content");
    BOOST_TEST(!core::parse_input(payload).has_message);
    payload["operation"] = "execute";
    BOOST_CHECK_THROW(core::parse_input(payload), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(compact_configuration_defaults_and_validation) {
    auto config = load::parse_configuration(configuration(), "/tmp/config");
    BOOST_TEST(config.memory == "/tmp/config/.data/memory");
    BOOST_TEST(config.compact_prompt.find("Do not call tools") != std::string::npos);
    auto value = configuration();
    value["persistence"]["memory"] = "../archives";
    BOOST_TEST(load::parse_configuration(value, "/tmp/config").memory == "/tmp/archives");
    value["persistence"]["memory"] = "";
    BOOST_CHECK_THROW(load::parse_configuration(value, "/tmp"), std::invalid_argument);
    value = configuration();
    value["worker"]["compact_prompt_file"] = "missing-compact.yaml";
    BOOST_CHECK_THROW(load::parse_configuration(value, "/tmp"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(compact_payload_is_explicit_and_has_no_external_content) {
    Json payload = {{"operation", "compact"}, {"request_id", "compact-1"}};
    auto input = core::parse_input(payload);
    BOOST_CHECK(input.operation == core::InputOperation::Compact);
    BOOST_CHECK(!input.has_message);
    BOOST_TEST(std::string(core::operation_name(input.operation)) == "compact");
    payload["options"] = {{"model", {{"model", "fixture"}}}};
    BOOST_TEST(core::parse_input(payload).options["model"]["model"] == "fixture");
    for (const char* field : {"content", "text", "role", "invokes"}) {
        auto invalid = payload;
        invalid[field] = "external instruction";
        BOOST_CHECK_THROW(core::parse_input(invalid), std::exception);
    }
}
