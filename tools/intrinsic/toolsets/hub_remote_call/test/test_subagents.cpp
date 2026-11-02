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

Json detail() {
    auto result = child_status();
    result["preview"] = {{"response", "answer 🌻"}, {"truncated", false},
        {"stale", false}, {"incomplete", false}, {"history_truncated", false}};
    result["latest_request"] = {{"request_id", "child-request"}, {"operation", "message"},
        {"state", "finished"}, {"status", "completed"}};
    result["reasoning"] = "hidden-reasoning";
    return result;
}

/** Run a real local exchange through the registered concrete tool. */
model_io::Content call(const std::string& name, Json arguments, Json result,
    std::function<void(Json&)> change_reply = {}, bool cancel = false, bool binary = false,
    std::string_view transport_failure = {}, std::vector<Json> pages = {})
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
    const auto original_arguments = query.arguments;
    asio::cancellation_signal cancellation;
    int requests = 0;
    const auto peer = [&]() -> asio::awaitable<void> {
      for (std::size_t page = 0; page < std::max(std::size_t(1), pages.size()); ++page) {
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
        auto expected_arguments = original_arguments;
        if (name == "subagent_send" && expected_arguments.at("operation") == "message") {
            const auto text = expected_arguments.at("request");
            expected_arguments.erase("request");
            expected_arguments["content"] = Json::array({{
                {"type", "text"}, {"modality", "text"}, {"raw", text}
            }});
        }
        if (name == "subagent_receive") {
            if (expected_arguments.contains("output_path")) {
                expected_arguments.erase("output_path");
                expected_arguments["export"] = page == 0 ? Json::object()
                    : Json{{"token", pages.at(page - 1).at("export").at("token")},
                        {"offset", pages.at(page - 1).at("export").at("next")}};
            } else expected_arguments["view"] = "preview";
        }
        BOOST_TEST(data.at("arguments") == expected_arguments);
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
        data["result"] = pages.empty() ? result : pages.at(page);
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
      }
    };
    auto server = asio::co_spawn(io, peer, asio::use_future);
    auto future = asio::co_spawn(io, tool->invoke(query),
        asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
    io.run();
    server.get();
    BOOST_TEST(requests == std::max(std::size_t(1), pages.size()));
    BOOST_TEST(query.arguments == original_arguments);
    return future.get();
}

Json message()
{
    return {{"subagent_id", "subagent-child"}, {"operation", "message"},
        {"request", "task"}};
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
            Json{{"subagent_id", "subagent-child"}, {"output_path", "/tmp/result.md"}},
            Json{{"output_path", "x"}}, Json{{"subagent_id", "subagent-child"}, {"output_path", ""}},
            Json{{"subagent_id", "../escape"}}, Json{{"subagent_id", ""}}};
        for (const std::string operation : {"continue", "compact", "stop", "unknown"}) {
            Json args = {{"subagent_id", "subagent-child"}, {"operation", operation}};
            cases.push_back(args);
            args["options"] = {{"model", {{"model", "provider-choice"}}}, {"tools", Json::object()}};
            cases.push_back(args);
            args["request"] = message().at("request");
            cases.push_back(args);
        }
        for (const Json options : {Json(), Json::array(), Json{{"confirmation", {{"mode", "approve"}}}},
                Json{{"model", 1}}, Json{{"tools", {{"future", true}}}}, Json{{"unknown", Json::object()}}}) {
            auto args = message();
            args["options"] = options;
            cases.push_back(args);
        }
        for (const Json request : {Json(), Json(""), Json(1), Json(false),
                Json::array(), Json::object(),
                Json::array({{{"type", "text"}, {"modality", "text"}, {"raw", "task"}}}),
                Json("Inspect /workspace/report.txt.\nReturn findings 🌻.")}) {
            auto args = message();
            args["request"] = request;
            cases.push_back(args);
        }
        for (const char* field : {"content", "type", "modality", "raw", "extras"}) {
            auto args = message();
            args[field] = Json::array({{{"type", "text"}, {"modality", "text"}, {"raw", "task"}}});
            cases.push_back(args);
        }
        cases.push_back({{"subagent_id", "subagent-child"}, {"operation", "message"}});
        cases.push_back({{"subagent_id", "subagent-child"}, {"operation", "message"},
            {"content", Json::array({{{"type", "text"}, {"modality", "text"}, {"raw", "task"}}})}});
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
    auto& raw = query.arguments["request"];
    raw = "";
    auto wire_arguments = query.arguments;
    wire_arguments.erase("request");
    wire_arguments["content"] = Json::array({{
        {"type", "text"}, {"modality", "text"}, {"raw", ""}
    }});
    raw = std::string(65536 - wire_arguments.dump().size(), 'a');
    wire_arguments["content"][0]["raw"] = raw;
    BOOST_TEST(wire_arguments.dump().size() == 65536u);
    BOOST_TEST(query.arguments.dump().size() < 65536u);
    BOOST_CHECK_NO_THROW(tool.ensure_arguments(query));
    raw.get_ref<std::string&>() += 'a';
    BOOST_CHECK_THROW(tool.ensure_arguments(query), tools::InvokeException);
    query.arguments = message();
    query.arguments["options"] = {{"model", {{"private", std::string(65536, 'x')}}}};
    BOOST_CHECK_THROW(tool.ensure_arguments(query), tools::InvokeException);
    query.arguments = message();
    query.arguments["request"] = std::string(1, char(0xff));
    BOOST_CHECK_THROW(tool.ensure_arguments(query), tools::InvokeException);
    query.arguments = message();
    query.arguments["options"] = {{"model", {{"private", std::string(1, char(0xff))}}}};
    BOOST_CHECK_EXCEPTION(tool.ensure_arguments(query), tools::InvokeException, [](const auto& error) {
        return std::string(error.what()).find("private") == std::string::npos;
    });
}

BOOST_AUTO_TEST_CASE(subagent_send_converts_only_request_text_and_preserves_options)
{
    auto arguments = message();
    arguments["request"] = "Inspect /workspace/report.txt.\nReturn findings 🌻; do not edit files.";
    arguments["options"] = {{"model", {{"model", "provider-choice"}}}, {"tools", Json::object()}};
    const auto sent = call("subagent_send", arguments, {{"subagent_id", "subagent-child"},
        {"operation", "message"}, {"request_id", "child-request"}, {"state", "sent"}});
    BOOST_TEST(sent.raw.find("[[request_id]]: child-request") != std::string::npos);
    BOOST_TEST(sent.raw.find("[[state]]: sent") != std::string::npos);
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


BOOST_AUTO_TEST_CASE(receive_preview_is_bounded_and_hides_internal_fields) {
    auto value = detail();
    value["preview"]["response"] = std::string(20000, 'a') + "🌻";
    auto result = call("subagent_receive", {{"subagent_id", "subagent-child"}}, value);
    BOOST_TEST(result.raw.size() < 20u * 1024);
    BOOST_TEST(result.raw.find("hidden-reasoning") == std::string::npos);
    BOOST_TEST(result.raw.find("output_path") != std::string::npos);
    BOOST_TEST(result.raw.find("child-request") != std::string::npos);
    auto listed = call("subagent_receive", Json::object(), {{"subagents", Json::array({child_status()})}});
    BOOST_TEST(listed.raw.find("subagent-child") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(receive_invalid_replies_never_reveal_raw_data) {
    for (const std::string mode : {"target", "missing", "oversize", "binary", "timeout", "disconnect"}) {
        auto value = detail();
        if (mode == "target") value["subagent_id"] = "foreign";
        if (mode == "missing") value.erase("preview");
        if (mode == "oversize") value["private"] = std::string(300000, 'x');
        BOOST_CHECK_THROW(call("subagent_receive", {{"subagent_id", "subagent-child"}}, value,
            {}, false, mode == "binary", mode), tools::InvokeException);
    }
}

BOOST_AUTO_TEST_CASE(receive_export_streams_and_never_overwrites) {
    const auto root = std::filesystem::temp_directory_path()
        / ("simplex-receive-" + boost::uuids::to_string(boost::uuids::random_generator()()));
    std::filesystem::create_directory(root);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::filesystem::remove_all(root); } } cleanup{root};
    const auto path = root / "result.md";
    const auto make = [](std::string text, std::size_t offset, std::size_t total) {
        return Json{{"subagent_id", "subagent-child"}, {"export", {{"source", "persisted_snapshot"},
            {"token", "snapshot"}, {"text", text}, {"offset", offset}, {"next", offset + text.size()},
            {"total", total}, {"done", offset + text.size() == total}, {"blocks", 1},
            {"omitted", false}, {"latest_block", "simplex:block:fixture:response"}}}};
    };
    const std::string first = "# Conversation\n", second = "正文🌻\n";
    const auto total = first.size() + second.size();
    Json args = {{"subagent_id", "subagent-child"}, {"output_path", path.string()}};
    const auto result = call("subagent_receive", args, {}, {}, false, false, "",
        {make(first, 0, total), make(second, first.size(), total)});
    BOOST_TEST(result.raw.find("Latest saved snapshot") != std::string::npos);
    BOOST_TEST(result.raw.find("正文") == std::string::npos);
    std::ifstream input(path);
    BOOST_TEST(std::string(std::istreambuf_iterator<char>(input), {}) == first + second);
    BOOST_CHECK_THROW(call("subagent_receive", args, make("new", 0, 3)), tools::InvokeException);
    std::ifstream original(path);
    BOOST_TEST(std::string(std::istreambuf_iterator<char>(original), {}) == first + second);
    std::filesystem::remove(path);
    auto bad = make("partial", 0, 7);
    bad["export"]["next"] = 8;
    BOOST_CHECK_THROW(call("subagent_receive", args, bad), tools::InvokeException);
    BOOST_TEST(std::filesystem::is_empty(root));
    auto changed = make(second, first.size(), total);
    changed["export"]["token"] = "different-snapshot";
    BOOST_CHECK_THROW(call("subagent_receive", args, {}, {}, false, false, "",
        {make(first, 0, total), changed}), tools::InvokeException);
    BOOST_TEST(std::filesystem::is_empty(root));
    std::filesystem::create_symlink(root / "absent", path);
    BOOST_CHECK_THROW(call("subagent_receive", args, make("new", 0, 3)), tools::InvokeException);
    BOOST_TEST(!std::filesystem::exists(root / "absent"));
}

BOOST_AUTO_TEST_CASE(receive_export_requires_approval_and_serial_scheduling) {
    SubagentReceiveTool tool(endpoint_for(), std::chrono::milliseconds(100), identity);
    model_io::InvokeQuery query;
    query.arguments = {{"subagent_id", "child"}, {"output_path", "/tmp/new.md"}};
    tool.ensure_arguments(query);
    tool.write_attributes(query);
    BOOST_CHECK(query.type == model_io::InvokeType::SerialWrite);
    BOOST_CHECK(query.security == model_io::InvokeSecurity::RequireConfirm);
    query.arguments.erase("output_path");
    tool.write_attributes(query);
    BOOST_CHECK(query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(query.security == model_io::InvokeSecurity::Trusted);
}
