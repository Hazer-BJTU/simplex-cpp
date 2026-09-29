#define BOOST_TEST_MODULE HubRemoteCallTools
#include <boost/test/unit_test.hpp>
#include "tools/intrinsic/hub_remote_call/toolset.hpp"
#include "tools/intrinsic/hub_remote_call/tools.hpp"
#include "tools/invoke_exception.hpp"
#include "tools/registry.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast.hpp>
#include <type_traits>

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using Json = nlohmann::json;
using tools::intrinsic::HubRemoteCallToolBase;
using tools::intrinsic::HubRemoteCallToolSet;
static_assert(std::is_abstract_v<HubRemoteCallToolBase>);

/** Test-only adapter exposes the protected transport; never registered. */
class Probe final : public HubRemoteCallToolBase {
public:
    Probe(endpoint::ResolvedEndpoint endpoint, std::chrono::milliseconds timeout)
        : HubRemoteCallToolBase(HUB_REMOTE_FIXTURE, std::move(endpoint), timeout) {}
    using HubRemoteCallToolBase::request;
    asio::awaitable<model_io::Content> invoke(const model_io::InvokeQuery&) override
    {
        throw std::logic_error("fixture invoke must not be used");
        co_return model_io::Content{};
    }
};

endpoint::ResolvedEndpoint endpoint_for(unsigned short port = 12345)
{
    return {"127.0.0.1", std::to_string(port), "/agent/session/tools/?token=secret", false};
}

enum class Reply { Rejected, Mismatch, RouteMismatch, WorkerMismatch, SessionMismatch, RunMismatch,
    InvalidJson, InvalidStatus, Binary, Timeout };

void exchange(Reply mode)
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    auto endpoint = endpoint_for(acceptor.local_endpoint().port());
    Probe tool(endpoint, std::chrono::milliseconds(mode == Reply::Timeout ? 200 : 2000));
    auto peer = [&]() -> asio::awaitable<void> {
        ws::stream<asio::ip::tcp::socket> socket(co_await acceptor.async_accept(asio::use_awaitable));
        beast::flat_buffer buffer;
        beast::http::request<beast::http::string_body> upgrade;
        co_await beast::http::async_read(socket.next_layer(), buffer, upgrade, asio::use_awaitable);
        BOOST_TEST(std::string(upgrade.target()) == "/agent/session/tools/files/read?token=secret");
        co_await socket.async_accept(upgrade, asio::use_awaitable);
        co_await socket.async_read(buffer, asio::use_awaitable);
        const auto request = Json::parse(beast::buffers_to_string(buffer.data()));
        BOOST_TEST(request.at("type") == "tool_request");
        auto data = request.at("data");
        BOOST_TEST(data.at("worker_id") == "worker");
        BOOST_TEST(data.at("session_id") == "session");
        BOOST_TEST(data.at("run_id") == "run");
        BOOST_TEST(!data.at("request_id").get<std::string>().empty());
        BOOST_TEST(data.at("arguments").at("value") == 42);
        if (mode == Reply::Timeout) {
            asio::steady_timer timer(io, std::chrono::milliseconds(500));
            co_await timer.async_wait(asio::use_awaitable);
            co_return;
        }
        data.erase("arguments");
        data["route"] = "files/read";
        data["status"] = mode == Reply::InvalidStatus ? "success" : "rejected";
        data["error"] = {{"code", "not_implemented"}, {"message", "not implemented"}};
        if (mode == Reply::Mismatch) data["request_id"] = "unrelated";
        if (mode == Reply::RouteMismatch) data["route"] = "other/route";
        if (mode == Reply::WorkerMismatch) data["worker_id"] = "other";
        if (mode == Reply::SessionMismatch) data["session_id"] = "other";
        if (mode == Reply::RunMismatch) data["run_id"] = "other";
        std::string response = Json{{"type", "tool_response"}, {"data", data}}.dump();
        if (mode == Reply::InvalidJson) response = "secret: broken JSON";
        socket.text(mode != Reply::Binary);
        co_await socket.async_write(asio::buffer(response), asio::use_awaitable);
        boost::system::error_code error;
        co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
    };
    auto server = asio::co_spawn(io, peer, asio::use_future);
    model_io::InvokeQuery query;
    query.name = "remote_request_fixture";
    query.id = "call-1";
    query.arguments = {{"value", 42}};
    auto result = asio::co_spawn(io,
        tool.request(query, "files/read", "worker", "session", "run"), asio::use_future);
    io.run();
    server.get();
    if (mode == Reply::Rejected) {
        const auto data = result.get();
        BOOST_TEST(data.at("status") == "rejected");
        BOOST_TEST(data.at("error").at("code") == "not_implemented");
    } else {
        BOOST_CHECK_EXCEPTION(result.get(), tools::InvokeException, [](const auto& error) {
            return error.stage() == tools::InvokeException::Stage::Invoke
                && error.query().id == "call-1"
                && std::string(error.what()).find("secret") == std::string::npos;
        });
    }
    BOOST_TEST(endpoint.target == "/agent/session/tools/?token=secret");
}
} // namespace

BOOST_AUTO_TEST_CASE(empty_toolset_owns_settings_without_advertising_tools)
{
    auto endpoint = endpoint_for();
    auto set = std::make_shared<HubRemoteCallToolSet>(endpoint, std::chrono::milliseconds(500));
    endpoint.target = "/changed";
    BOOST_TEST(set->name() == "hub_remote_call");
    BOOST_TEST(set->endpoint().target == "/agent/session/tools/?token=secret");
    BOOST_TEST(set->timeout().count() == 500);
    BOOST_TEST(set->tool_count() == 0u);
    BOOST_TEST(set->get_tools().empty());
    BOOST_CHECK(!set->skill());
    tools::ToolRegistry registry;
    registry.add(set);
    BOOST_TEST(registry.size() == 1u);
    BOOST_TEST(!registry.contains("hub_remote_call"));
}

BOOST_AUTO_TEST_CASE(configuration_and_routes_are_validated_before_io)
{
    BOOST_CHECK_THROW(HubRemoteCallToolSet(endpoint_for(), std::chrono::milliseconds(0)),
        std::invalid_argument);
    auto invalid = endpoint_for();
    invalid.host.clear();
    BOOST_CHECK_THROW(HubRemoteCallToolSet(invalid, std::chrono::milliseconds(1)),
        std::invalid_argument);
    asio::io_context io;
    Probe tool(endpoint_for(), std::chrono::milliseconds(500));
    for (const std::string route : {"", "../read", "files//read", "Files", "files?token=x", "files/"}) {
        model_io::InvokeQuery query;
        query.arguments = Json::object();
        io.restart();
        auto future = asio::co_spawn(io,
            tool.request(query, route, "worker", "session", "run"), asio::use_future);
        io.run();
        BOOST_CHECK_THROW(future.get(), tools::InvokeException);
    }
}

BOOST_AUTO_TEST_CASE(one_shot_protocol_and_failure_cases)
{
    for (Reply mode : {Reply::Rejected, Reply::Mismatch, Reply::RouteMismatch,
        Reply::WorkerMismatch, Reply::SessionMismatch, Reply::RunMismatch, Reply::InvalidJson,
        Reply::InvalidStatus, Reply::Binary, Reply::Timeout}) {
        exchange(mode);
    }
}
