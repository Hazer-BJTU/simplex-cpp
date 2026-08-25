// Offline tests for the model request interpreter interface: the lenient
// endpoint resolver, and the interface's implementability/callability
// through a minimal stub. Pure data checks — no network, no filesystem —
// except the connection-stream factory, which is exercised against a
// loopback listener (offline, no external network).
#define BOOST_TEST_MODULE model_request
#include <boost/test/unit_test.hpp>

#include <array>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "dataclass/endpoint_config.hpp"
#include "dataclass/model_io.hpp"
#include "endpoint/http_request_exception.hpp"
#include "endpoint/model_request.hpp"

namespace http = boost::beast::http;
using endpoint::ModelRequestInterpreter;
using endpoint::resolve_endpoint;
using model_io::AgentInputState;
using model_io::ModelEndpoint;

// A minimal concrete interpreter proving the contract is implementable and
// callable offline. Real interpreters (Responses-API layer first) arrive as
// compiled libraries; this stub exists only to pin the interface.
class StubInterpreter final : public ModelRequestInterpreter {
public:
    HttpRequest build_request(
        const AgentInputState& conversation,
        const ModelEndpoint& config,
        const nlohmann::json& generation) override {
        // Contract hard error #1: a non-empty model name is required.
        if (!generation.contains("model") ||
            !generation.at("model").is_string() ||
            generation.at("model").get<std::string>().empty()) {
            throw HttpRequestException(
                HttpRequestException::Stage::CreateRequest,
                "generation carries no non-empty \"model\"");
        }
        // Contract hard error #2 comes with the resolver.
        const auto where = resolve_endpoint(config);

        nlohmann::json body = generation; // pass through verbatim
        nlohmann::json messages = nlohmann::json::array();
        // Leniency sample: the system message appears only when non-empty.
        if (const auto rendered = conversation.system_prompt.render();
            !rendered.markdown.empty()) {
            messages.push_back({{"role", "system"}, {"content", rendered.markdown}});
        }
        body["messages"] = messages; // builder-owned key wins

        HttpRequest request{http::verb::post, where.target, 11};
        request.set(http::field::host, where.host);
        request.set(http::field::content_type, "application/json");
        request.body() = body.dump();
        request.prepare_payload();
        return request;
    }
};

// ---- resolve_endpoint: lenient parsing --------------------------------------

BOOST_AUTO_TEST_CASE(resolve_accepts_schemeless_and_bare_host) {
    ModelEndpoint e;
    e.base_url = "api.deepseek.com";
    auto r = resolve_endpoint(e);
    BOOST_CHECK_EQUAL(r.host, "api.deepseek.com");
    BOOST_CHECK_EQUAL(r.port, "443"); // https assumed
    BOOST_CHECK_EQUAL(r.target, "/chat/completions"); // request_path default
    BOOST_CHECK(r.tls); // https assumed
}

BOOST_AUTO_TEST_CASE(resolve_keeps_scheme_port_and_prefix) {
    ModelEndpoint e;
    e.base_url = "https://gateway.internal:8443/v1/";
    e.request_path = "/chat/completions";
    auto r = resolve_endpoint(e);
    BOOST_CHECK_EQUAL(r.host, "gateway.internal");
    BOOST_CHECK_EQUAL(r.port, "8443");
    BOOST_CHECK_EQUAL(r.target, "/v1/chat/completions");
}

BOOST_AUTO_TEST_CASE(resolve_http_defaults_to_port_80) {
    ModelEndpoint e;
    e.base_url = "http://localhost:11434";
    auto r = resolve_endpoint(e);
    BOOST_CHECK_EQUAL(r.host, "localhost");
    BOOST_CHECK_EQUAL(r.port, "11434"); // explicit port beats scheme default
    BOOST_CHECK_EQUAL(r.target, "/chat/completions");
    BOOST_CHECK(!r.tls); // plain-HTTP transport, local backend

    e.base_url = "http://localhost";
    r = resolve_endpoint(e);
    BOOST_CHECK_EQUAL(r.port, "80");
    BOOST_CHECK(!r.tls);

    // An explicit port does not smuggle in a scheme: only http:// clears tls.
    e.base_url = "https://localhost:80";
    r = resolve_endpoint(e);
    BOOST_CHECK_EQUAL(r.port, "80");
    BOOST_CHECK(r.tls);
}

BOOST_AUTO_TEST_CASE(resolve_appends_request_path_after_prefix) {
    ModelEndpoint e;
    e.base_url = "https://api.deepseek.com/anthropic";
    e.request_path = "/v1/messages"; // Anthropic-style path
    auto r = resolve_endpoint(e);
    BOOST_CHECK_EQUAL(r.target, "/anthropic/v1/messages");
}

BOOST_AUTO_TEST_CASE(resolve_tolerates_doubled_slashes) {
    ModelEndpoint e;
    e.base_url = "https://api.deepseek.com//v2//";
    e.request_path = "chat/completions"; // leading slash missing
    auto r = resolve_endpoint(e);
    BOOST_CHECK_EQUAL(r.target, "/v2/chat/completions");
}

BOOST_AUTO_TEST_CASE(resolve_empty_request_path_targets_root) {
    ModelEndpoint e;
    e.base_url = "https://api.example.com";
    e.request_path = "";
    auto r = resolve_endpoint(e);
    BOOST_CHECK_EQUAL(r.target, "/");
}

BOOST_AUTO_TEST_CASE(resolve_hostless_base_url_throws_create_request) {
    ModelEndpoint e;
    e.base_url = "";
    BOOST_CHECK_THROW(resolve_endpoint(e), HttpRequestException);

    e.base_url = "https://"; // scheme but no authority
    BOOST_CHECK_THROW(resolve_endpoint(e), HttpRequestException);

    e.base_url = "https:///prefix/only"; // prefix but no host
    try {
        resolve_endpoint(e);
        BOOST_FAIL("expected HttpRequestException");
    } catch (const HttpRequestException& error) {
        BOOST_CHECK(error.stage()
                    == HttpRequestException::Stage::CreateRequest);
        BOOST_CHECK_NE(std::string(error.what()).find("no host"), std::string::npos);
    }
}

// ---- the interface through a stub --------------------------------------------

BOOST_AUTO_TEST_CASE(stub_builds_a_complete_request) {
    StubInterpreter interpreter;
    AgentInputState state; // leniency: empty conversation still builds
    ModelEndpoint endpoint;
    endpoint.base_url = "https://api.deepseek.com";
    nlohmann::json generation{
        {"model", "deepseek-v4-flash"},
        {"temperature", 0.7},
    }; // "stream" absent -> implementations default it, not the resolver

    auto request = interpreter.build_request(state, endpoint, generation);
    BOOST_CHECK(request.method() == http::verb::post);
    BOOST_CHECK_EQUAL(request.target(), "/chat/completions");
    BOOST_CHECK_EQUAL(std::string(request.at(http::field::host)), "api.deepseek.com");

    const auto body = nlohmann::json::parse(request.body());
    BOOST_CHECK_EQUAL(body["model"], "deepseek-v4-flash");
    BOOST_CHECK_EQUAL(body["temperature"], 0.7); // generation passes through
    BOOST_CHECK(body.contains("messages"));       // builder-owned key applied

    // prepare_payload() wired the body into Content-Length.
    const auto length_header = request.at(http::field::content_length);
    BOOST_REQUIRE(!length_header.empty());
    BOOST_CHECK_EQUAL(std::stoul(std::string(length_header)),
                      request.body().size());
}

BOOST_AUTO_TEST_CASE(stub_maps_system_prompt_when_present) {
    StubInterpreter interpreter;
    AgentInputState state;
    state.system_prompt.add_section("identity", "Identity", "You are simplex.");
    ModelEndpoint endpoint;
    endpoint.base_url = "api.deepseek.com";
    nlohmann::json generation{{"model", "m"}};

    auto request = interpreter.build_request(state, endpoint, generation);
    const auto body = nlohmann::json::parse(request.body());
    BOOST_REQUIRE_EQUAL(body["messages"].size(), 1u);
    BOOST_CHECK_EQUAL(body["messages"].at(0)["role"], "system");
    BOOST_CHECK_NE(body["messages"].at(0)["content"].get<std::string>().find("You are simplex."),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(stub_enforces_the_two_hard_errors) {
    StubInterpreter interpreter;
    AgentInputState state;
    ModelEndpoint endpoint;
    endpoint.base_url = "https://api.deepseek.com";

    // Missing model, empty model, non-string model.
    const std::array<nlohmann::json, 3> bad_generations = {
        nlohmann::json::object(),
        nlohmann::json{{"model", ""}},
        nlohmann::json{{"model", 42}},
    };
    for (const auto& generation : bad_generations) {
        try {
            interpreter.build_request(state, endpoint, generation);
            BOOST_FAIL("expected HttpRequestException for missing model");
        } catch (const HttpRequestException& error) {
            BOOST_CHECK(error.stage()
                        == HttpRequestException::Stage::CreateRequest);
        }
    }

    // Hostless base_url surfaces the resolver's CreateRequest error.
    endpoint.base_url.clear();
    BOOST_CHECK_THROW(
        interpreter.build_request(state, endpoint, nlohmann::json{{"model", "m"}}),
        HttpRequestException);
}

// ---- create_connection_stream: scheme to flavour -----------------------------

BOOST_AUTO_TEST_CASE(resolved_stream_factory_connects_the_plain_flavour) {
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    asio::io_context server_io;
    tcp::acceptor acceptor(
        server_io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = acceptor.local_endpoint().port();

    std::thread server([&acceptor] {
        tcp::socket socket(acceptor.get_executor());
        acceptor.accept(socket);
        boost::system::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
    });

    ModelEndpoint e;
    e.base_url = "http://localhost:" + std::to_string(port);
    const auto resolved = resolve_endpoint(e);

    // this_coro::executor convenience overload, driven by co_spawn. The
    // http:// scheme selects the plain flavour INSIDE the returned
    // connection_stream — the runtime choice this factory exists for.
    asio::io_context io;
    auto operation = endpoint::create_connection_stream(resolved);
    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();
    auto stream = result.get();   // rethrows a connect failure, if any

    // A connected, non-TLS stream came back behind the facade.
    BOOST_REQUIRE(!stream.empty());
    BOOST_CHECK(!stream.is_tls());
    stream.close();
    server.join();
}

BOOST_AUTO_TEST_CASE(resolved_stream_factory_reports_tls_refusal) {
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    asio::io_context reservation_io;
    tcp::acceptor reservation(
        reservation_io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto unused_port = reservation.local_endpoint().port();
    reservation.close();

    ModelEndpoint e;
    e.base_url = "https://127.0.0.1:" + std::to_string(unused_port);
    const auto resolved = resolve_endpoint(e);

    asio::io_context io;
    auto operation = endpoint::create_connection_stream(resolved);
    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();

    BOOST_CHECK_THROW(result.get(), boost::system::system_error);
}
