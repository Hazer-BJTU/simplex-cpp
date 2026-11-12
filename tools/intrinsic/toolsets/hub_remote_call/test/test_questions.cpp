#include <boost/test/unit_test.hpp>
#include "tools/intrinsic/hub_remote_call/toolset.hpp"
#include "tools/intrinsic/hub_remote_call/tools.hpp"
#include "tools/invoke_exception.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast.hpp>
#include <functional>
#include <string>

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using Json = nlohmann::json;
using tools::intrinsic::AskUserTool;
using tools::intrinsic::HubRemoteCallIdentity;

constexpr std::chrono::milliseconds budget{300000};

endpoint::ResolvedEndpoint endpoint_for(unsigned short port)
{
    return {"127.0.0.1", std::to_string(port), "/agent/session/tools/?token=secret", false};
}

model_io::InvokeQuery query_for(Json arguments, std::string id = "call-1")
{
    model_io::InvokeQuery query;
    query.name = "ask_user";
    query.id = std::move(id);
    query.arguments = std::move(arguments);
    return query;
}

/** One accepted connection; `respond` builds the reply, or an empty reply closes early. */
model_io::Content round_trip(Json arguments,
    const std::function<Json(const Json&, const std::string&)>& respond)
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    AskUserTool tool(endpoint_for(acceptor.local_endpoint().port()),
        std::chrono::milliseconds(2000), budget,
        [] { return HubRemoteCallIdentity{"worker", "session", "run"}; });
    auto peer = [&]() -> asio::awaitable<void> {
        ws::stream<asio::ip::tcp::socket> socket(co_await acceptor.async_accept(asio::use_awaitable));
        beast::flat_buffer buffer;
        beast::http::request<beast::http::string_body> upgrade;
        co_await beast::http::async_read(socket.next_layer(), buffer, upgrade, asio::use_awaitable);
        BOOST_TEST(std::string(upgrade.target()) == "/agent/session/tools/ask_user/ask?token=secret");
        co_await socket.async_accept(upgrade, asio::use_awaitable);
        co_await socket.async_read(buffer, asio::use_awaitable);
        const auto request = Json::parse(beast::buffers_to_string(buffer.data()));
        BOOST_TEST(request.at("type") == "tool_request");
        const auto data = request.at("data");
        const auto reply = respond(data, data.at("request_id").get<std::string>());
        if (reply.is_null()) {
            boost::system::error_code ignored;
            socket.next_layer().close(ignored);
            co_return;
        }
        co_await socket.async_write(asio::buffer(reply.dump()), asio::use_awaitable);
        boost::system::error_code error;
        co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
    };
    auto server = asio::co_spawn(io, peer, asio::use_future);
    auto query = query_for(std::move(arguments));
    // The invocation layer settles arguments before invoke(); tests do the same.
    tool.ensure_arguments(query);
    auto result = asio::co_spawn(io, tool.invoke(query), asio::use_future);
    io.run();
    server.get();
    return result.get();
}

Json answered(const std::string& request_id, Json reply)
{
    Json data{{"worker_id", "worker"}, {"session_id", "session"}, {"run_id", "run"},
        {"request_id", request_id}, {"route", "ask_user/ask"}, {"status", "succeeded"},
        {"result", Json{{"outcome", "answered"}, {"elapsed_ms", 1200}, {"reply", reply}}}};
    return Json{{"type", "tool_response"}, {"data", data}};
}

void check_arguments(const Json& arguments)
{
    AskUserTool tool(endpoint_for(1), std::chrono::milliseconds(500), budget,
        [] { return HubRemoteCallIdentity{"worker", "session", "run"}; });
    auto query = query_for(arguments);
    tool.ensure_arguments(query);
    tool.write_attributes(query);
    BOOST_CHECK(query.arguments.at("timeout_ms") == budget.count());
    BOOST_CHECK(query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(query.security == model_io::InvokeSecurity::Trusted);
}
} // namespace

BOOST_AUTO_TEST_CASE(ask_user_settles_arguments_and_scheduling)
{
    check_arguments(Json{{"question", "Which branch should I target?"}});
    check_arguments(Json{{"question", "Pick one"}, {"options", {"A", "B"}}});
    check_arguments(Json{{"question", "Pick one"},
        {"options", {"A", "B", "C", "D", "E", "F", "G", "H"}}});
}

BOOST_AUTO_TEST_CASE(ask_user_rejects_invalid_arguments)
{
    AskUserTool tool(endpoint_for(1), std::chrono::milliseconds(500), budget,
        [] { return HubRemoteCallIdentity{"worker", "session", "run"}; });
    for (const auto& arguments : {
        Json::object(),
        Json{{"question", "   "}},
        Json{{"question", std::string(8 * 1024 + 1, 'q')}},
        Json{{"question", "ok"}, {"extra", 1}},
        Json{{"question", "ok"}, {"options", "A,B"}},
        Json{{"question", "ok"}, {"options", {"A"}}},
        Json{{"question", "ok"}, {"options", {"A", "B", "C", "D", "E", "F", "G", "H", "I"}}},
        Json{{"question", "ok"}, {"options", {"A", "A"}}},
        Json{{"question", "ok"}, {"options", {"A", ""}}},
        Json{{"question", "ok"}, {"options", {"A", std::string(201, 'x')}}},
        Json{{"question", "ok"}, {"options", {"A", 2}}},
    }) {
        auto query = query_for(arguments);
        BOOST_CHECK_THROW(tool.ensure_arguments(query),
            tools::InvokeException);
    }
}

BOOST_AUTO_TEST_CASE(ask_user_timeout_is_validated_at_construction)
{
    auto identity = [] { return HubRemoteCallIdentity{"worker", "session", "run"}; };
    BOOST_CHECK_THROW(AskUserTool(endpoint_for(1), std::chrono::milliseconds(500),
        std::chrono::milliseconds(59999), identity), std::invalid_argument);
    BOOST_CHECK_THROW(AskUserTool(endpoint_for(1), std::chrono::milliseconds(500),
        std::chrono::milliseconds(1800001), identity), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ask_user_renders_text_answer)
{
    const auto content = round_trip(Json{{"question", "Which branch?"}},
        [](const Json& request, const std::string& id) {
            BOOST_TEST(request.at("arguments").at("timeout_ms") == budget.count());
            BOOST_TEST(request.at("arguments").at("question") == "Which branch?");
            return answered(id, Json{{"text", "target feat/ask-user-question"}});
        });
    BOOST_TEST(content.raw.find("answered") != std::string::npos);
    BOOST_TEST(content.raw.find("target feat/ask-user-question") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ask_user_renders_option_answer)
{
    const auto content = round_trip(Json{{"question", "Pick one"}, {"options", {"A", "B"}}},
        [](const Json& request, const std::string& id) {
            BOOST_TEST(request.at("arguments").at("options").at(1) == "B");
            return answered(id, Json{{"option_index", 1}, {"option", "B"}});
        });
    BOOST_TEST(content.raw.find("Option 2: B") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ask_user_timeout_is_a_normal_result)
{
    const auto content = round_trip(Json{{"question", "Anyone there?"}},
        [](const Json& request, const std::string& id) {
            Json data{{"worker_id", "worker"}, {"session_id", "session"}, {"run_id", "run"},
                {"request_id", id}, {"route", "ask_user/ask"}, {"status", "succeeded"},
                {"result", Json{{"outcome", "timeout"}, {"elapsed_ms", 300001}}}};
            return Json{{"type", "tool_response"}, {"data", data}};
        });
    BOOST_TEST(content.raw.find("timeout") != std::string::npos);
    BOOST_TEST(content.raw.find("No user reply") != std::string::npos);
    BOOST_TEST(content.raw.find("timed out after 300 s") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ask_user_reports_the_granted_wait)
{
    // The hub may clamp the budget; the text must name the wait it granted.
    const auto content = round_trip(Json{{"question", "Anyone there?"}},
        [](const Json&, const std::string& id) {
            Json data{{"worker_id", "worker"}, {"session_id", "session"}, {"run_id", "run"},
                {"request_id", id}, {"route", "ask_user/ask"}, {"status", "succeeded"},
                {"result", Json{{"outcome", "timeout"}, {"timeout_ms", 120000},
                    {"elapsed_ms", 120001}}}};
            return Json{{"type", "tool_response"}, {"data", data}};
        });
    BOOST_TEST(content.raw.find("timed out after 120 s") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ask_user_cancelled_is_a_normal_result)
{
    const auto content = round_trip(Json{{"question", "Still needed?"}},
        [](const Json&, const std::string& id) {
            Json data{{"worker_id", "worker"}, {"session_id", "session"}, {"run_id", "run"},
                {"request_id", id}, {"route", "ask_user/ask"}, {"status", "succeeded"},
                {"result", Json{{"outcome", "cancelled"}, {"reason", "run_cancelled"},
                    {"elapsed_ms", 800}}}};
            return Json{{"type", "tool_response"}, {"data", data}};
        });
    BOOST_TEST(content.raw.find("withdrawn") != std::string::npos);
    BOOST_TEST(content.raw.find("run_cancelled") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ask_user_rejection_is_a_normal_result)
{
    const auto content = round_trip(Json{{"question", "Anyone there?"}},
        [](const Json&, const std::string& id) {
            Json data{{"worker_id", "worker"}, {"session_id", "session"}, {"run_id", "run"},
                {"request_id", id}, {"route", "ask_user/ask"}, {"status", "rejected"},
                {"error", Json{{"code", "too_many_questions"}, {"message", "too many"}}}};
            return Json{{"type", "tool_response"}, {"data", data}};
        });
    BOOST_TEST(content.raw.find("unavailable") != std::string::npos);
    BOOST_TEST(content.raw.find("too_many_questions") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ask_user_lost_connection_is_a_normal_result)
{
    const auto content = round_trip(Json{{"question", "Anyone there?"}},
        [](const Json&, const std::string&) { return Json(); });
    BOOST_TEST(content.raw.find("unavailable") != std::string::npos);
    BOOST_TEST(content.raw.find("connection was lost") != std::string::npos);
}
