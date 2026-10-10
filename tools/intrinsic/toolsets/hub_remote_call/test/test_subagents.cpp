#include <boost/test/unit_test.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast.hpp>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "tools/intrinsic/hub_remote_call/subagents.hpp"
#include "tools/intrinsic/hub_remote_call/toolset.hpp"
#include "tools/intrinsic/hub_remote_call/schemas.hpp"
#include "tools/invoke_exception.hpp"

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using Json = nlohmann::json;
using namespace tools::intrinsic;

endpoint::ResolvedEndpoint endpoint_for(unsigned short port = 12345)
{
    return {"127.0.0.1", std::to_string(port), "/agent/parent/tools/?token=distinctive-secret", false};
}

HubRemoteCallIdentity identity()
{
    return {"worker", "parent", "run"};
}

Json child_status()
{
    return {{"subagent_id", "subagent-child"}, {"parent", "parent"},
        {"lifecycle", "ready"}, {"policy", "ask"}, {"health", "healthy"},
        {"process_state", "running"}, {"connected", true}, {"active", false},
        {"run_id", nullptr}, {"pending_approvals", 0}, {"observed_at", nullptr},
        {"reason", "live identified worker"}, {"error", nullptr}};
}

Json detail()
{
    auto result = child_status();
    result["requests"] = Json::array({{
        {"request_id", "child-request"}, {"operation", "compact"},
        {"run_id", "child-run"}, {"state", "finished"}, {"status", "exchange_limit"},
        {"at", "2026-10-06T00:00:00Z"}, {"summary", "compact summary"}
    }});
    result["requests_truncated"] = false;
    const Json part = {{"type", "text"}, {"modality", "text"}, {"raw", "answer 🌻"},
        {"extras", {{"secret", "hidden-extra"}}}};
    result["conversation"] = {
        {"revision", 0}, {"worker_id", "child-worker"}, {"cursor", 0}, {"next", 1}, {"total", 1},
        {"stale", false}, {"incomplete", false}, {"truncated", false}, {"refreshed_at", nullptr},
        {"turns", Json::array({{
            {"index", 7}, {"request_id", "child-request"}, {"user", Json::array({part})},
            {"steps", Json::array({{{"index", 3}, {"content", Json::array({part})}}})},
            {"reasoning", "hidden-reasoning"}
        }})}
    };
    result["configuration"] = "hidden-configuration";
    return result;
}

/** Run a real local exchange through the registered concrete tool. */
model_io::Content call(const std::string& name, Json arguments, Json result,
    std::function<void(Json&)> change_reply = {}, bool cancel = false, bool binary = false,
    std::string_view transport_failure = {})
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    HubRemoteCallToolSet set(endpoint_for(acceptor.local_endpoint().port()),
        std::chrono::milliseconds(transport_failure == "timeout" ? 50 : 2000), identity);
    model_io::InvokeQuery query;
    query.id = "model-call";
    query.name = name;
    query.arguments = std::move(arguments);
    const auto tool = set.dispatch(query);
    BOOST_REQUIRE(tool);
    tool->ensure_arguments(query);
    asio::cancellation_signal cancellation;
    int requests = 0;
    const auto peer = [&]() -> asio::awaitable<void> {
        ws::stream<asio::ip::tcp::socket> socket(co_await acceptor.async_accept(asio::use_awaitable));
        beast::flat_buffer buffer;
        beast::http::request<beast::http::string_body> upgrade;
        co_await beast::http::async_read(socket.next_layer(), buffer, upgrade, asio::use_awaitable);
        const std::string route = name == "subagent_fork" ? "subagent/clean-fork"
            : name == "subagent_send" ? "subagent/send" : "subagent/receive";
        BOOST_TEST(std::string(upgrade.target()) == "/agent/parent/tools/" + route + "?token=distinctive-secret");
        co_await socket.async_accept(upgrade, asio::use_awaitable);
        co_await socket.async_read(buffer, asio::use_awaitable);
        ++requests;
        auto data = Json::parse(beast::buffers_to_string(buffer.data())).at("data");
        BOOST_TEST(data.at("arguments") == query.arguments);
        BOOST_TEST(data.at("worker_id") == "worker");
        BOOST_TEST(data.at("session_id") == "parent");
        BOOST_TEST(data.at("run_id") == "run");
        BOOST_TEST(data.at("request_id") != query.id);
        if (transport_failure == "disconnect") {
            socket.next_layer().close();
            co_return;
        }
        if (transport_failure == "timeout") {
            asio::steady_timer timer(io, std::chrono::milliseconds(150));
            co_await timer.async_wait(asio::use_awaitable);
            co_return;
        }
        data.erase("arguments");
        data["route"] = route;
        data["status"] = "succeeded";
        data["result"] = result;
        Json reply = {{"type", "tool_response"}, {"data", data}};
        if (change_reply) change_reply(reply);
        if (cancel) {
            cancellation.emit(asio::cancellation_type::all);
            asio::steady_timer timer(io, std::chrono::milliseconds(20));
            co_await timer.async_wait(asio::use_awaitable);
        }
        const auto wire = reply.dump();
        socket.text(!binary);
        boost::system::error_code error;
        // Fragmented oversized replies exercise the assembled message budget.
        co_await socket.async_write_some(false, asio::buffer(wire.data(), wire.size() / 2),
            asio::redirect_error(asio::use_awaitable, error));
        if (!error) {
            co_await socket.async_write_some(true,
                asio::buffer(wire.data() + wire.size() / 2, wire.size() - wire.size() / 2),
                asio::redirect_error(asio::use_awaitable, error));
        }
        if (!error) {
            co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
        }
    };
    auto server = asio::co_spawn(io, peer, asio::use_future);
    auto future = asio::co_spawn(io, tool->invoke(query),
        asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
    io.run();
    server.get();
    BOOST_TEST(requests == 1);
    return future.get();
}

Json message()
{
    return {{"subagent_id", "subagent-child"}, {"operation", "message"},
        {"content", Json::array({{{"type", "text"}, {"modality", "text"}, {"raw", "task"}}})}};
}

/** Evaluate the schema keywords used by these declarations independently. */
bool matches(const Json& schema, const Json& value)
{
    if (schema.contains("type")) {
        const auto type = schema.at("type");
        if ((type == "object" && !value.is_object()) || (type == "array" && !value.is_array())
            || (type == "string" && !value.is_string())
            || (type == "integer" && !(value.is_number_integer() || value.is_number_unsigned()))) return false;
    }
    if (schema.contains("enum") && std::find(schema.at("enum").begin(),
            schema.at("enum").end(), value) == schema.at("enum").end()) return false;
    if (schema.contains("required")) {
        for (const auto& key : schema.at("required")) if (!value.contains(key.get<std::string>())) return false;
    }
    if (value.is_object() && schema.contains("properties")) {
        for (const auto& [key, item] : value.items()) {
            const auto found = schema.at("properties").find(key);
            if (found == schema.at("properties").end()) {
                if (schema.value("additionalProperties", true) == false) return false;
            } else if (!matches(*found, item)) return false;
        }
    }
    if (value.is_array()) {
        if (value.size() < schema.value("minItems", 0u)) return false;
        if (schema.contains("items")) for (const auto& item : value) if (!matches(schema.at("items"), item)) return false;
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        // JSON Schema counts Unicode code points, whereas the explicit RPC
        // argument budget below counts the complete serialized UTF-8 bytes.
        const auto length = static_cast<std::size_t>(std::count_if(
            text.begin(), text.end(), [](unsigned char byte) { return (byte & 0xc0) != 0x80; }));
        if (length < schema.value("minLength", 0u)
            || length > schema.value("maxLength", length)) return false;
        if (schema.contains("pattern")
            && !std::regex_search(text, std::regex(schema.at("pattern").get<std::string>()))) return false;
    }
    if (value.is_number()) {
        if (schema.contains("minimum") && value.get<double>() < schema.at("minimum").get<double>()) return false;
        if (schema.contains("maximum") && value.get<double>() > schema.at("maximum").get<double>()) return false;
    }
    for (const char* key : {"anyOf", "oneOf"}) {
        if (!schema.contains(key)) continue;
        const auto count = std::count_if(schema.at(key).begin(), schema.at(key).end(),
            [&](const Json& branch) { return matches(branch, value); });
        if (count == 0 || (std::string_view(key) == "oneOf" && count != 1)) return false;
    }
    return !schema.contains("not") || !matches(schema.at("not"), value);
}
} // namespace

BOOST_AUTO_TEST_CASE(schema_comparison_counts_unicode_code_points)
{
    const Json schema = {{"type", "string"}, {"minLength", 1}, {"maxLength", 1}};
    BOOST_TEST(matches(schema, "中"));
    BOOST_TEST(matches(schema, "🌻"));
    BOOST_TEST(!matches(schema, "中🌻"));
    BOOST_TEST(!matches(schema, "e\u0301"));
    BOOST_TEST(!matches(schema, ""));
}

BOOST_AUTO_TEST_CASE(subagent_schemas_validation_and_attributes_agree)
{
    HubRemoteCallToolSet set(endpoint_for(), std::chrono::milliseconds(100), identity);
    BOOST_TEST(set.capability_groups().at(1).registered.size() == 3u);
    for (const std::string name : {"subagent_fork", "subagent_send", "subagent_receive"}) {
        std::vector<Json> cases = {Json::object(), Json::array(), Json{{"unknown", 1}},
            message(), Json{{"subagent_id", "subagent-child"}}, Json{{"cursor", 0}},
            Json{{"subagent_id", "subagent-child"}, {"cursor", 0}, {"limit", 5}},
            Json{{"subagent_id", "subagent-child"}, {"cursor", -1}},
            Json{{"subagent_id", "subagent-child"}, {"cursor", 1.5}},
            Json{{"subagent_id", "subagent-child"}, {"cursor", 9007199254740992ULL}},
            Json{{"subagent_id", "subagent-child"}, {"limit", 0}},
            Json{{"subagent_id", "subagent-child"}, {"limit", 11}},
            Json{{"subagent_id", "../escape"}}, Json{{"subagent_id", ""}}};
        for (const std::string operation : {"continue", "compact", "stop", "unknown"}) {
            Json args = {{"subagent_id", "subagent-child"}, {"operation", operation}};
            cases.push_back(args);
            args["options"] = {{"model", {{"model", "provider-choice"}}}, {"tools", Json::object()}};
            cases.push_back(args);
            args["content"] = message().at("content");
            cases.push_back(args);
        }
        for (const Json options : {Json(), Json::array(), Json{{"confirmation", {{"mode", "approve"}}}},
                Json{{"model", 1}}, Json{{"tools", {{"future", true}}}}, Json{{"unknown", Json::object()}}}) {
            auto args = message();
            args["options"] = options;
            cases.push_back(args);
        }
        for (const Json content : {Json::array(), Json::array({Json::object()}),
                Json::array({{{"type", "image"}, {"modality", "image"}, {"raw", "url"}}}),
                Json::array({{{"type", "text"}, {"raw", "text"}}})}) {
            auto args = message();
            args["content"] = content;
            cases.push_back(args);
        }
        model_io::InvokeQuery query;
        query.name = name;
        const auto tool = set.dispatch(query);
        BOOST_REQUIRE(tool);
        tool->write_attributes(query);
        BOOST_CHECK(query.security == model_io::InvokeSecurity::Trusted);
        BOOST_CHECK(query.type == (name == "subagent_receive"
            ? model_io::InvokeType::ReadOnly : model_io::InvokeType::SerialWrite));
        for (const auto& args : cases) {
            query.arguments = args;
            bool accepted = true;
            try { tool->ensure_arguments(query); }
            catch (const tools::InvokeException&) { accepted = false; }
            BOOST_TEST_CONTEXT(name << ": " << args.dump()) {
                BOOST_TEST(matches(tool->get_details().argument_schema, args) == accepted);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(subagent_argument_budget_counts_nested_json_and_utf8)
{
    SubagentSendTool tool(endpoint_for(), std::chrono::milliseconds(100), identity);
    model_io::InvokeQuery query;
    query.arguments = message();
    auto& raw = query.arguments["content"][0]["raw"];
    raw = "";
    raw = std::string(65536 - query.arguments.dump().size(), 'a');
    BOOST_TEST(query.arguments.dump().size() == 65536u);
    BOOST_CHECK_NO_THROW(tool.ensure_arguments(query));
    raw.get_ref<std::string&>() += 'a';
    BOOST_CHECK_THROW(tool.ensure_arguments(query), tools::InvokeException);
    query.arguments = message();
    query.arguments["options"] = {{"model", {{"private", std::string(65536, 'x')}}}};
    BOOST_CHECK_THROW(tool.ensure_arguments(query), tools::InvokeException);
    query.arguments = message();
    query.arguments["content"][0]["extras"] = {{"private", std::string(1, char(0xff))}};
    BOOST_CHECK_EXCEPTION(tool.ensure_arguments(query), tools::InvokeException, [](const auto& error) {
        return std::string(error.what()).find("private") == std::string::npos;
    });
}

BOOST_AUTO_TEST_CASE(subagent_result_rendering_retains_outcomes_pagination_and_whitelists_content)
{
    const auto fork = call("subagent_fork", Json::object(),
        {{"subagent_id", "subagent-child"}, {"lifecycle", "preparing"}}, {}, true);
    BOOST_TEST(fork.raw.find("[[lifecycle]]: preparing") != std::string::npos);
    for (const std::string state : {"sent", "rejected", "unknown"}) {
        const auto sent = call("subagent_send", message(), {{"subagent_id", "subagent-child"},
            {"operation", "message"}, {"request_id", "child-request"}, {"state", state}});
        BOOST_TEST(sent.raw.find("[[state]]: " + state) != std::string::npos);
        BOOST_TEST((sent.raw.find("task") == std::string::npos || state != "rejected"));
    }
    const auto stopped = call("subagent_send", {{"subagent_id", "subagent-child"}, {"operation", "stop"}},
        {{"subagent_id", "subagent-child"}, {"operation", "stop"}, {"operation_id", "stop-id"}, {"state", "stopping"}});
    BOOST_TEST(stopped.raw.find("[[operation_id]]: stop-id") != std::string::npos);
    const auto list = call("subagent_receive", Json::object(), {{"subagents", Json::array({child_status()})}});
    BOOST_TEST(list.raw.find("[[subagents]]: 1") != std::string::npos);
    const auto received = call("subagent_receive", {{"subagent_id", "subagent-child"}}, detail());
    for (const char* expected : {"[[revision]]: 0", "[[next]]: 1", "[[requests_truncated]]: false",
            "[[stale]]: false", "[[run_status]]: exchange_limit", "[[turn]]: 7", "answer 🌻",
            "assistant step 3", "compact summary", "[[refreshed_at]]: null"}) {
        BOOST_TEST(received.raw.find(expected) != std::string::npos);
    }
    for (const char* hidden : {"hidden-extra", "hidden-configuration", "hidden-reasoning"}) {
        BOOST_TEST(received.raw.find(hidden) == std::string::npos);
    }
    auto startup = detail();
    startup["conversation"] = nullptr;
    startup["requests"] = Json::array();
    BOOST_TEST(call("subagent_receive", {{"subagent_id", "subagent-child"}}, startup).raw
        .find("[[conversation_available]]: false") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(subagent_failure_diagnostics_are_safe_and_never_retry)
{
    for (const std::string mode : {"rejected", "target", "missing", "oversize", "correlation", "cursor",
            "binary", "timeout", "disconnect"}) {
        auto result = detail();
        if (mode == "target") result["subagent_id"] = "distinctive-target";
        if (mode == "missing") result.erase("requests");
        if (mode == "oversize") result["private"] = std::string(300000, 'x');
        if (mode == "cursor") result["conversation"]["next"] = 3;
        const auto modifier = [&](Json& reply) {
            if (mode == "rejected") {
                reply["data"].erase("result");
                reply["data"]["status"] = "rejected";
                reply["data"]["error"] = {{"code", "not_implemented"}, {"message", "distinctive-diagnostic"}};
            }
            if (mode == "correlation") reply["data"]["worker_id"] = "distinctive-worker";
        };
        std::string expected = "hub remote call transport failed";
        if (mode == "rejected") expected = "not_implemented";
        if (mode == "target" || mode == "missing" || mode == "cursor") expected = "invalid Hub subagent result";
        if (mode == "correlation") expected = "invalid or mismatched hub remote call response";
        BOOST_TEST_CONTEXT("failure mode: " << mode) {
            BOOST_CHECK_EXCEPTION(call("subagent_receive", {{"subagent_id", "subagent-child"}},
                result, modifier, false, mode == "binary", mode), tools::InvokeException, [&](const auto& error) {
                    const std::string text = error.what();
                    return text.find("distinctive-") == std::string::npos
                        && text.find("mutation") == std::string::npos
                        && text.find(expected) != std::string::npos;
                });
        }
    }
}

BOOST_AUTO_TEST_CASE(subagent_fragmented_pages_use_actual_presentation_overhead)
{
    for (const int parts : {2480, 3000, 3900}) {
        auto value = detail();
        auto& turn = value["conversation"]["turns"][0];
        turn["user"] = Json::array();
        turn["steps"] = Json::array();
        for (int offset = 0; offset < parts; offset += 128) {
            Json content = Json::array();
            for (int i = offset; i < std::min(parts, offset + 128); ++i) {
                content.push_back({{"type", "text"}, {"modality", "text"}, {"raw", "x"}});
            }
            turn["steps"].push_back({{"index", 9007199254740900ULL + offset / 128}, {"content", content}});
        }
        turn["steps"].push_back({{"index", 9007199254740900ULL + (parts + 127) / 128},
            {"content", Json::array({{{"type", "text"}, {"modality", "text"},
                {"raw", "DISTINCTIVE_FINAL_BODY " + std::string(parts == 3900 ? 100 : 110000, 'a')}}})}});
        // A valid assembled reply, within every declared turn/step/part limit.
        BOOST_REQUIRE(turn["steps"].size() <= 32u);
        BOOST_REQUIRE(value.dump().size() < 255u * 1024);
        const auto received = call("subagent_receive", {{"subagent_id", "subagent-child"}, {"limit", 1}}, value);
        BOOST_TEST(received.raw.size() <= 256u * 1024);
        BOOST_TEST(received.raw.find("DISTINCTIVE_FINAL_BODY") != std::string::npos);
        BOOST_CHECK_NO_THROW(Json(received.raw).dump());
        if (parts == 3900) {
            // The complete one-turn page fits. No body may be clipped merely
            // because its empty-block header plus a worst-case reserve is large.
            BOOST_TEST(received.raw.find("[[output_truncated]]: false") != std::string::npos);
            BOOST_TEST(received.raw.find("truncated, first") == std::string::npos);
            const std::regex short_body("\\(1 bytes\\):\\nx\\n");
            const auto count = std::distance(std::sregex_iterator(received.raw.begin(), received.raw.end(), short_body),
                std::sregex_iterator());
            BOOST_TEST(count == parts);
        } else {
            BOOST_TEST(received.raw.find("[[output_truncated]]: true") != std::string::npos);
            if (parts == 3000) {
                // The usual 96 KiB body allocation overflows once real headers
                // are included. A reduced allowance must retain the final prefix.
                BOOST_TEST(received.raw.find(std::string(90000, 'a')) == std::string::npos);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(subagent_excessive_actual_structure_reports_a_read_budget_failure)
{
    auto value = detail();
    auto& turn = value["conversation"]["turns"][0];
    turn["user"] = Json::array();
    turn["steps"] = Json::array();
    for (int step = 0; step < 32; ++step) {
        Json content = Json::array();
        for (int part = 0; part < 128; ++part) {
            content.push_back({{"type", "external_ref"}, {"modality", "document"}, {"raw", "x"}});
        }
        turn["steps"].push_back({{"index", 9007199254740900ULL + step}, {"content", content}});
    }
    // Both complete and omitted-body headers exceed 256 KiB, even though the
    // actual reply remains below the assembled WebSocket transport limit.
    BOOST_REQUIRE(value.dump().size() < 255u * 1024);
    BOOST_CHECK_EXCEPTION(call("subagent_receive", {{"subagent_id", "subagent-child"}, {"limit", 1}}, value),
        tools::InvokeException, [](const auto& error) {
            const std::string text = error.what();
            return text.find("256 KiB budget") != std::string::npos
                && text.find("smaller limit") != std::string::npos
                && text.find("invalid Hub") == std::string::npos
                && text.find("mutation") == std::string::npos;
        });
}

BOOST_AUTO_TEST_CASE(subagent_read_rejections_are_actionable_without_mutation_warnings)
{
    for (const std::string code : {"answer_unavailable", "result_too_large", "storage_error"}) {
        const auto reject = [&](Json& reply) {
            reply["data"].erase("result");
            reply["data"]["status"] = "rejected";
            reply["data"]["error"] = {{"code", code}, {"message", "distinctive-private-diagnostic"}};
        };
        BOOST_CHECK_EXCEPTION(call("subagent_receive", {{"subagent_id", "subagent-child"}}, detail(), reject),
            tools::InvokeException, [&](const auto& error) {
                const std::string text = error.what();
                return text.find(code) != std::string::npos && text.find("mutation") == std::string::npos
                    && text.find("distinctive-") == std::string::npos
                    && (code != "answer_unavailable" || text.find("fingerprint") != std::string::npos)
                    && (code != "result_too_large" || text.find("smaller limit") != std::string::npos);
            });
    }
}

BOOST_AUTO_TEST_CASE(subagent_mutation_failures_preserve_uncertain_outcome_warnings)
{
    for (const std::string name : {"subagent_fork", "subagent_send"}) {
        for (const std::string mode : {"disconnect", "invalid-result"}) {
            BOOST_CHECK_EXCEPTION(call(name, name == "subagent_fork" ? Json::object() : message(),
                {{"private", "distinctive-private-result"}}, {}, false, false, mode),
                tools::InvokeException, [](const auto& error) {
                    const std::string text = error.what();
                    return text.find("mutation may have committed") != std::string::npos
                        && text.find("distinctive-") == std::string::npos;
                });
        }
    }
}

BOOST_AUTO_TEST_CASE(subagent_large_visible_result_is_clipped_on_utf8_boundary)
{
    auto value = detail();
    std::string large;
    for (int i = 0; i < 40000; ++i) large += "🌻";
    value["conversation"]["turns"][0]["user"] = Json::array();
    value["conversation"]["turns"][0]["steps"][0]["content"][0]["raw"] = large;
    const auto received = call("subagent_receive", {{"subagent_id", "subagent-child"}}, value);
    BOOST_TEST(received.raw.size() <= 256u * 1024);
    BOOST_CHECK_NO_THROW(Json(received.raw).dump());
    BOOST_TEST(received.raw.find("[[output_truncated]]: true") != std::string::npos);
    BOOST_TEST(received.raw.find("[[next]]: 1") != std::string::npos);
    BOOST_TEST(received.raw.find("[[request_id]]: child-request") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(subagent_presentation_preserves_final_answer_after_many_intermediate_steps)
{
    auto value = detail();
    auto& turn = value["conversation"]["turns"][0];
    turn["user"] = Json::array({{
        {"type", "text"}, {"modality", "text"}, {"raw", std::string(4096, 'u')}
    }});
    turn["steps"] = Json::array();
    for (int i = 0; i < 24; ++i) {
        turn["steps"].push_back({{"index", i}, {"content", Json::array({{
            {"type", "text"}, {"modality", "text"}, {"raw", std::string(4096, 'a')}
        }})}});
    }
    turn["steps"].push_back({{"index", 24}, {"content", Json::array({{
        {"type", "text"}, {"modality", "text"}, {"raw", "DISTINCTIVE_FINAL_ANSWER 🌻"}
    }})}});
    // A trailing empty step must not hide the last visible answer.
    turn["steps"].push_back({{"index", 25}, {"content", Json::array()}});
    value["requests"][0]["status"] = "completed";
    value["requests"][0]["summary"] = "DISTINCTIVE_COMPACT_SUMMARY 中";
    BOOST_REQUIRE(value.dump().size() < 256u * 1024);

    for (const Json arguments : {Json{{"subagent_id", "subagent-child"}},
            Json{{"subagent_id", "subagent-child"}, {"cursor", 0}, {"limit", 1}}}) {
        const auto received = call("subagent_receive", arguments, value);
        BOOST_TEST(received.raw.size() <= 256u * 1024);
        BOOST_CHECK_NO_THROW(Json(received.raw).dump());
        for (const char* expected : {"DISTINCTIVE_FINAL_ANSWER 🌻", "DISTINCTIVE_COMPACT_SUMMARY 中",
                "[[output_truncated]]: true", "[[turn]]: 7", "[[revision]]: 0",
                "[[next]]: 1", "[[total]]: 1", "[[requests_truncated]]: false",
                "[[request_id]]: child-request", "[[state]]: finished", "[[run_status]]: completed",
                "[[stale]]: false", "[[incomplete]]: false", "[[truncated]]: false"}) {
            BOOST_TEST(received.raw.find(expected) != std::string::npos);
        }
        BOOST_TEST(received.raw.find("user (text/text): (empty, truncated)") != std::string::npos);
        BOOST_TEST(received.raw.find("assistant step 0 (text/text) (truncated, first ") != std::string::npos);
        BOOST_TEST(received.raw.find("assistant step 0") < received.raw.find("assistant step 24"));
        BOOST_TEST(received.raw.find("user (text/text)") < received.raw.find("assistant step 0"));
    }
}

BOOST_AUTO_TEST_CASE(subagent_presentation_shares_result_budget_across_turns_and_compact_summary)
{
    auto value = detail();
    auto& turn = value["conversation"]["turns"][0];
    std::string long_answer = "OLDER_ANSWER 中🌻 ";
    for (int i = 0; i < 30000; ++i) {
        long_answer += "🌻";
    }
    turn["user"] = Json::array();
    turn["steps"][0]["content"][0]["raw"] = long_answer;
    value["conversation"]["turns"].push_back({{"index", 8}, {"user", Json::array()},
        {"steps", Json::array({{{"index", 0}, {"content", Json::array({{
            {"type", "text"}, {"modality", "text"}, {"raw", "LATEST_ANSWER 🌻"}
        }})}}})}});
    value["conversation"]["next"] = 2;
    value["conversation"]["total"] = 2;
    // Both a long previous answer and a long summary compete with a short
    // latest answer. All result bodies must get space before older context.
    value["requests"][0]["summary"] = "COMPACT_RESULT 中 " + long_answer;
    BOOST_REQUIRE(value.dump().size() < 256u * 1024);
    const auto received = call("subagent_receive", {{"subagent_id", "subagent-child"}}, value);
    BOOST_TEST(received.raw.size() <= 256u * 1024);
    BOOST_CHECK_NO_THROW(Json(received.raw).dump());
    for (const char* expected : {"OLDER_ANSWER 中🌻", "LATEST_ANSWER 🌻", "COMPACT_RESULT 中",
            "[[output_truncated]]: true", "[[turn]]: 7", "[[turn]]: 8", "[[next]]: 2"}) {
        BOOST_TEST(received.raw.find(expected) != std::string::npos);
    }
    BOOST_TEST(received.raw.find("[[turn]]: 7") < received.raw.find("[[turn]]: 8"));
}

BOOST_AUTO_TEST_CASE(subagent_calls_capture_fresh_identity_and_use_distinct_rpc_ids)
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    int snapshots = 0;
    SubagentForkTool tool(endpoint_for(acceptor.local_endpoint().port()),
        std::chrono::milliseconds(2000), [&] {
            return HubRemoteCallIdentity{"worker", "parent", "run-" + std::to_string(++snapshots)};
        });
    std::set<std::string> rpc_ids;
    const auto peer = [&]() -> asio::awaitable<void> {
        for (int request = 1; request <= 2; ++request) {
            ws::stream<asio::ip::tcp::socket> socket(co_await acceptor.async_accept(asio::use_awaitable));
            co_await socket.async_accept(asio::use_awaitable);
            beast::flat_buffer buffer;
            co_await socket.async_read(buffer, asio::use_awaitable);
            auto data = Json::parse(beast::buffers_to_string(buffer.data())).at("data");
            BOOST_TEST(data.at("run_id") == "run-" + std::to_string(request));
            rpc_ids.insert(data.at("request_id").get<std::string>());
            data.erase("arguments");
            data["route"] = "subagent/clean-fork";
            data["status"] = "succeeded";
            data["result"] = {{"subagent_id", "subagent-" + std::to_string(request)}, {"lifecycle", "preparing"}};
            const auto reply = Json{{"type", "tool_response"}, {"data", data}}.dump();
            socket.text(true);
            co_await socket.async_write(asio::buffer(reply), asio::use_awaitable);
            boost::system::error_code error;
            co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
        }
    };
    const auto invoke_twice = [&]() -> asio::awaitable<void> {
        model_io::InvokeQuery query;
        query.arguments = Json::object();
        for (int count = 0; count < 2; ++count) {
            (void)co_await tool.invoke(query);
        }
    };
    auto server = asio::co_spawn(io, peer, asio::use_future);
    auto client = asio::co_spawn(io, invoke_twice, asio::use_future);
    io.run();
    server.get();
    client.get();
    BOOST_TEST(snapshots == 2);
    BOOST_TEST(rpc_ids.size() == 2u);
}

BOOST_AUTO_TEST_CASE(subagent_declaration_failure_preserves_plan_and_reports_partial_group)
{
    const auto source = hub_remote_call::schema_directory();
    const auto root = std::filesystem::temp_directory_path()
        / ("simplex-subagent-schemas-" + boost::uuids::to_string(boost::uuids::random_generator()()));
    std::filesystem::create_directory(root);
    struct Restore {
        std::filesystem::path root;
        std::optional<std::string> previous;
        ~Restore() {
            if (previous) setenv("SIMPLEX_HUB_REMOTE_CALL_SCHEMA_DIR", previous->c_str(), 1);
            else unsetenv("SIMPLEX_HUB_REMOTE_CALL_SCHEMA_DIR");
            std::filesystem::remove_all(root);
        }
    } restore{root, std::nullopt};
    if (const auto value = std::getenv("SIMPLEX_HUB_REMOTE_CALL_SCHEMA_DIR")) restore.previous = value;
    for (const auto& file : std::filesystem::directory_iterator(source)) {
        std::filesystem::copy_file(file.path(), root / file.path().filename());
    }
    std::filesystem::remove(root / "subagent_fork.yaml");
    std::ofstream(root / "subagent_send.yaml") << "[invalid declaration";
    setenv("SIMPLEX_HUB_REMOTE_CALL_SCHEMA_DIR", root.c_str(), 1);
    HubRemoteCallToolSet set(endpoint_for(), std::chrono::milliseconds(100), identity);
    BOOST_TEST(set.tool_count() == 2u);
    BOOST_TEST(set.capability_groups().at(0).registered.size() == 1u);
    BOOST_TEST(set.capability_groups().at(1).missing.size() == 2u);
    BOOST_TEST(set.capability_groups().at(1).registered.at(0) == "subagent_receive");
    BOOST_REQUIRE(set.skill());
}

BOOST_AUTO_TEST_CASE(subagent_answer_pages_return_exact_text_and_actionable_cursor)
{
    const Json source = {{"worker_id", "child-worker"}, {"turn", 7}, {"step", 3}, {"commit_sequence", "42"}, {"fingerprint", std::string(64, 'a')}};
    const Json cursor = {{"source", source}, {"part", 0}, {"offset", 0}};
    Json result = child_status();
    const std::string text = "# exact page\n中文🌍\n" + std::string(12000, 'x');
    result["answer"] = {{"source", source}, {"part", 0}, {"offset", 0},
        {"raw", text}, {"bytes", text.size() + 10}, {"next_offset", text.size()},
        {"next_part", 0}, {"total_parts", 1}, {"done", false}};
    const auto answer = call("subagent_receive", {{"subagent_id", "subagent-child"}, {"answer", cursor}}, result);
    BOOST_TEST(answer.raw.find(text) != std::string::npos);
    BOOST_TEST(answer.raw.find("[[next_offset]]") != std::string::npos);
    BOOST_TEST(answer.raw.find("\"commit_sequence\":\"42\"") != std::string::npos);
    result["answer"]["next_offset"] = 1;
    BOOST_CHECK_THROW(call("subagent_receive", {{"subagent_id", "subagent-child"}, {"answer", cursor}}, result), tools::InvokeException);
}

BOOST_AUTO_TEST_CASE(subagent_answer_query_rejects_ambiguous_or_malformed_cursors)
{
    SubagentReceiveTool tool(endpoint_for(), std::chrono::milliseconds(100), identity);
    model_io::InvokeQuery query;
    query.name = "subagent_receive";
    const Json source = {{"worker_id", "child-worker"}, {"turn", 0}, {"step", 0}, {"commit_sequence", "1"}, {"fingerprint", std::string(64, 'a')}};
    const Json valid = {{"subagent_id", "subagent-child"}, {"answer", {
        {"source", source}, {"part", 0}, {"offset", 0}}}};
    query.arguments = valid;
    BOOST_CHECK_NO_THROW(tool.ensure_arguments(query));
    BOOST_TEST(matches(tool.get_details().argument_schema, valid));
    for (int test = 0; test < 10; ++test) {
        query.arguments = valid;
        if (test == 0) query.arguments["cursor"] = 0;
        if (test == 1) query.arguments.erase("subagent_id");
        if (test == 2) query.arguments["answer"]["offset"] = -1;
        if (test == 3) query.arguments["answer"]["source"]["commit_sequence"] = "garbage";
        if (test == 4) query.arguments["answer"]["source"]["path"] = "/private";
        if (test == 5) query.arguments["answer"]["source"].erase("fingerprint");
        if (test == 6) query.arguments["answer"]["source"]["fingerprint"] = std::string(63, 'a');
        if (test == 7) query.arguments["answer"]["source"]["fingerprint"] = std::string(64, 'g');
        if (test == 8) query.arguments["answer"]["source"]["fingerprint"] = 1;
        if (test == 9) query.arguments["limit"] = 5;
        BOOST_CHECK_THROW(tool.ensure_arguments(query), tools::InvokeException);
        BOOST_TEST(!matches(tool.get_details().argument_schema, query.arguments));
    }
    query.arguments = valid;
    query.arguments["answer"]["source"].erase("fingerprint");
    BOOST_CHECK_EXCEPTION(tool.ensure_arguments(query), tools::InvokeException, [](const auto& error) {
        return error.stage() == tools::InvokeException::Stage::ArgumentParse
            && error.message().find("fingerprint") != std::string::npos;
    });
}
