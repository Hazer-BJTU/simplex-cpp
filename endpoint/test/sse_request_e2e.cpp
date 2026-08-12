// End-to-end check for endpoint::sse_request over a local TLS SSE server.
//
// NOT a deterministic unit test: it spins up a one-shot TLS server on loopback
// (self-signed cert, verify_none client) that serves a fixed SSE body, then
// drives sse_request + a get() consumer and checks the delivered events and the
// handler lifecycle. Like https_connectivity_check it is a plain executable and
// is not registered with CTest.
#include "endpoint/https_stream.hpp"
#include "endpoint/request.hpp"
#include "exceptions/http_request_exception.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;

using tcp = asio::ip::tcp;

using endpoint::SSEAborted;
using endpoint::SSEHandlerState;
using endpoint::SSEResponseHandler;

// An SSE field line, matching SSEResponseHandler<...>::LineInfo.
using Field = std::pair<std::string, std::string>;

// PEM generated with:
//   openssl req -x509 -newkey rsa:2048 -keyout k.pem -out c.pem -days 1 \
//       -nodes -subj "/CN=localhost"
static const char kServerKeyPem[] = R"PEM(
-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQCVcTiq50HqE8Y5
+NHcDSGu5XLdgkCTNbYad0U+mgaRuFvAEGUjR9d1bSDrRv69QsAod9Nw+wWkciKG
VASZCDJdthz0xlwtSV4yPgiMwZIQDeC6hI4yS/RxkfdZo8+sxPh4E9gO8S0UdrbM
Kzv7MOHmeJJgDLTWcBzNGonXkNp9lQkQjsv9jyoWAeNuSiLfVkmlntaSXotejvns
p94twSBjVXX7gaKSZJ56LX2fP7IXZXhjevdAPw3+sEreXtrbkuhcGkzaGXCvOAyq
sRcAZ81/e6FgxpKQ8l6VAFVPgLA7Ho8l3QwyEzXiNqHq+VHAlNKiPlyk6ewFbOG0
g1+SdffrAgMBAAECggEAB8fBP4SStfEZnOMLaf11B3oCpO2nFwLw3CfhfwTd2rBS
dolj+pY+7WolEJq9oHTdND4oz/UBsjIPAhdVtfHrTV2x3DyHiAQN12fLYiihxhwy
2wMTtCKoM1F6IzyYD1Kh7P57fSupQSt9ENwfX1CIMkuMs1t1/sQPe84p5wMvnOIV
HJMcRUm3gI55M0LJ2e8QROLRlpNTZdmyFDzbUMYobGv3L64lMbyaXtUvr2r9wm2H
Egq3KNICt73vfiRsuARJ/3eDtVMgJiEM1NdGsIt4CjIFJKizr6APOPmuTJa0dEYT
cVByjCsK1CyyHuKJ9OzRusZgEPW9keg5CT1e6lQSoQKBgQDF2GexExqusv4oBhU0
2V5aD2gCbnAp3bXJIrQ5L6iIXSQdW97v3Yk4xXoTZSROflt0iZAN+tA220QF8vya
tnuy3ensHv5tYaXTdqCRqdEpitcAhNgdxXcgPdIPt7vm0tdguJvnbStuvPtXFvq8
GeUGNVw4JpU8UO8V57/k/mumDQKBgQDBXo0U4jPlwgDmMZYVlafwfaxJ+1jGQSQ5
pbenkj5RmmvmAV+00O7lmR+Rfzu2n7H34PyHKu9TQWK5DxqbpcPs4gO45wKXxFPg
huOJ2npitJBFH1HE5s+ptHMNmctPoulEFLpmv68hihtOwtabF+w5GSSOL0Owob/m
s1+HNK3P1wKBgBDPEgA5X0r4ah98ZNDYput/45ZRS7ZC3+72w9kX83micC8OXyKB
7+ai4HxFW5BPq/V6uoJ1jLscZesbedqrJldMA1PMTlF8uln8+idmBh9BbILexn3B
CR30IqSzN4Ok5ieRh61h2Q7Pf4smqKbSjGK6pdsfbA5z0dxKlEMgWoUBAoGBALS5
7hoKoH6op7Z9ucxKpz959cDfjcUdtQG8BL41TNFwBlBeEFkqvV0RlBOkWucAspD+
UobydePWRLK5jyDR+SA4zUnPfvjhoZND/v4kmCYQpJY2A3KIVqZB5RZdG0w+8s2S
ofTOSpyKO6ONLo6d+UYUXboWKqm3Q2gXVb1EWv7ZAoGBALGINySdUKhPyq0m550A
hyRFqTZcjh+f6LxfldVp3vxb7fbpyo/mT6K2JMiT3JKcxi33c8zBUEVhCOXtIIkK
CknXIAQWaeq6YOaE4HKZQ76M72ukLeuV6jKcOgdpvsGbPlUm8mf5yyXuPUXgc3Av
VtyN++cpV4RnNKnMgqBhegRo
-----END PRIVATE KEY-----
)PEM";
static const char kServerCertPem[] = R"PEM(
-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUfL5LWVTvEMxaShrjJwsi8PnVxpIwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDgxMjEzNDkwMloXDTI2MDgx
MzEzNDkwMlowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAlXE4qudB6hPGOfjR3A0hruVy3YJAkzW2GndFPpoGkbhb
wBBlI0fXdW0g60b+vULAKHfTcPsFpHIihlQEmQgyXbYc9MZcLUleMj4IjMGSEA3g
uoSOMkv0cZH3WaPPrMT4eBPYDvEtFHa2zCs7+zDh5niSYAy01nAczRqJ15DafZUJ
EI7L/Y8qFgHjbkoi31ZJpZ7Wkl6LXo757KfeLcEgY1V1+4GikmSeei19nz+yF2V4
Y3r3QD8N/rBK3l7a25LoXBpM2hlwrzgMqrEXAGfNf3uhYMaSkPJelQBVT4CwOx6P
Jd0MMhM14jah6vlRwJTSoj5cpOnsBWzhtINfknX36wIDAQABo1MwUTAdBgNVHQ4E
FgQUGncDmZKrRimfx5jxshnOaGVa9GEwHwYDVR0jBBgwFoAUGncDmZKrRimfx5jx
shnOaGVa9GEwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAErLp
9nae+Dlq7Lq+k2wP1JY2uSKvilfARpZrAPi7sFD68OzyJZHo4PlyawrSJN4Pgz/8
aTkq0hewKUDcaompJVp48P8JSSdmuxlofgxtlofpAKNYo+EcOadEzsa1yHeF5DS5
we98o5yjGTKJRTuURR9omu09ppsYFPgrRSyLI3KV869WsCuIbJcvO834v86L/5Dj
/Er/gdVqd9OE6EKtYWtNKNNCqKG4YrdmT/0OPI8Lxgmu7n/AZPmbBaoP/+k8scet
IYHGszLWnQ/dwwdmpi/NX6QEmkyynVYyr/FtBuG+Ie0g5GVyb/3icHDZPgmfwxVV
zKZzsLxp3cvfCXTU7g==
-----END CERTIFICATE-----
)PEM";

// A handler whose Product is simply the verbatim field list of each event.
class FieldHandler final : public SSEResponseHandler<std::vector<Field>> {
public:
    using SSEResponseHandler<std::vector<Field>>::SSEResponseHandler;

    std::vector<Field> _handle_message(std::span<const Field> message) override {
        return std::vector<Field>(message.begin(), message.end());
    }
};

static std::string data_value(const std::vector<Field>& event) {
    for (const auto& [field, value] : event)
        if (field == "data") return value;
    return {};
}

// One-shot TLS server: bind loopback, publish the port, then accept a single
// client, complete the handshake, read the request, and reply with a fixed
// two-event SSE body before closing.
static void run_server(std::promise<unsigned short>& port_promise) {
    asio::io_context io;
    ssl::context ctx(ssl::context::tls_server);
    ctx.use_certificate(asio::buffer(kServerCertPem), ssl::context::pem);
    ctx.use_private_key(asio::buffer(kServerKeyPem), ssl::context::pem);

    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    port_promise.set_value(acceptor.local_endpoint().port());

    ssl::stream<tcp::socket> tls_stream(io, ctx);
    acceptor.accept(tls_stream.lowest_layer());
    tls_stream.handshake(ssl::stream_base::server);

    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    http::read(tls_stream, buffer, request);

    http::response<http::string_body> response(http::status::ok, 11);
    response.set(http::field::content_type, "text/event-stream");
    response.set(http::field::connection, "close");
    response.body() = "data: alpha\n\ndata: beta\n\n";
    response.prepare_payload();
    http::write(tls_stream, response);

    beast::error_code ignored;
    tls_stream.shutdown(ignored);
}

// Connect a client stream (verify_none for the self-signed local cert) by
// reusing the same async connect/handshake shape as create_https_connection_stream.
static std::unique_ptr<endpoint::https_stream> connect_client(
    asio::io_context& io, ssl::context& ctx, unsigned short port) {
    auto future = asio::co_spawn(
        io,
        [&ctx, port]() -> asio::awaitable<std::unique_ptr<endpoint::https_stream>> {
            auto executor = co_await asio::this_coro::executor;
            auto stream = std::make_unique<endpoint::https_stream>(executor, ctx);
            tcp::resolver resolver(executor);
            auto endpoints = co_await resolver.async_resolve(
                "127.0.0.1", std::to_string(port), asio::use_awaitable);
            beast::get_lowest_layer(*stream).expires_after(std::chrono::seconds(30));
            co_await beast::get_lowest_layer(*stream).async_connect(
                endpoints, asio::use_awaitable);
            beast::get_lowest_layer(*stream).expires_after(std::chrono::seconds(30));
            co_await stream->async_handshake(ssl::stream_base::client);
            beast::get_lowest_layer(*stream).expires_never();
            co_return stream;
        },
        asio::use_future);
    io.run();
    io.restart();
    return future.get();
}

int main() {
    std::promise<unsigned short> port_promise;
    auto port_future = port_promise.get_future();

    std::thread server([&port_promise] {
        try {
            run_server(port_promise);
        } catch (const std::exception& error) {
            std::cerr << "server thread error: " << error.what() << "\n";
            try {
                port_promise.set_exception(std::current_exception());
            } catch (...) {}
        }
    });

    ssl::context client_ctx(ssl::context::tls_client);
    client_ctx.set_verify_mode(ssl::verify_none);   // self-signed local cert

    asio::io_context io;
    const unsigned short port = port_future.get();  // server is now listening
    auto stream = connect_client(io, client_ctx, port);

    http::request<http::string_body> request{http::verb::get, "/events", 11};
    request.set(http::field::host, "localhost");
    request.set(http::field::accept, "text/event-stream");

    auto handler = std::make_shared<FieldHandler>(io.get_executor());

    struct Results {
        std::vector<std::vector<Field>> events;
        std::optional<std::string> put_error;
        std::optional<SSEHandlerState> consumer_end;
    } results;

    // Producer: drive sse_request, then finish() so the consumer unblocks.
    asio::co_spawn(
        io,
        [&results, handler,
         stream = std::move(stream), request = std::move(request)]() mutable
            -> asio::awaitable<void> {
            try {
                co_await endpoint::sse_request<std::vector<Field>>(
                    handler, std::move(stream), std::move(request));
            } catch (const HttpRequestException& error) {
                results.put_error = error.what();
            }
            handler->finish(SSEHandlerState::DONE);
            co_return;
        },
        asio::detached);

    // Consumer: drain get() until the channel is closed.
    asio::co_spawn(
        io,
        [&results, handler]() -> asio::awaitable<void> {
            try {
                for (;;) {
                    results.events.push_back(co_await handler->get());
                }
            } catch (const SSEAborted& aborted) {
                results.consumer_end = aborted.state();
            }
        },
        asio::detached);

    io.run();
    server.join();

    int failures = 0;
    auto check = [&](bool condition, const std::string& message) {
        if (condition) {
            std::cout << "ok   : " << message << "\n";
        } else {
            std::cerr << "FAIL : " << message << "\n";
            ++failures;
        }
    };

    check(!results.put_error.has_value(), "sse_request completed without error");
    if (results.put_error) {
        std::cerr << "        put error: " << *results.put_error << "\n";
    }
    check(results.events.size() == 2, "two events were delivered");
    if (results.events.size() >= 2) {
        check(data_value(results.events[0]) == "alpha", "first event data == alpha");
        check(data_value(results.events[1]) == "beta", "second event data == beta");
    }
    check(results.consumer_end.has_value(), "consumer observed the channel close");
    check(results.consumer_end.value() == SSEHandlerState::DONE,
          "consumer saw DONE state");

    if (failures == 0) {
        std::cout << "E2E PASSED\n";
        return 0;
    }
    std::cout << "E2E FAILED (" << failures << " checks failed)\n";
    return 1;
}
