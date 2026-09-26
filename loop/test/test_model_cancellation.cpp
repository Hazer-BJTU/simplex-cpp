#define BOOST_TEST_MODULE LoopModelCancellation
#include <boost/test/unit_test.hpp>

#include "loop/loop.hpp"
#include "loop/events.hpp"
#include "tools/registry.hpp"
#include "eventbus/event_bus.hpp"
#include "llm/compat/chat_completions/model.hpp"
#include "llm/compat/responses/model.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <thread>

namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

namespace {

/** Makes a canonical adapter directly constructible for local HTTP tests. */
template<class Adapter>
struct LocalModel : Adapter {
    LocalModel(asio::any_io_executor executor, nlohmann::json config)
        : Adapter(executor, std::move(config)) {
    }
};

/**
 * Starts a real HTTP model exchange and stops after the server has received it.
 * The server either withholds headers or sends an unfinished SSE stream. EOF
 * on the server proves the client producer's socket was closed during unwind;
 * the deadline is only a hang guard, never the cancellation trigger.
 */
template<class Adapter>
void interrupt_http_exchange(bool send_headers) {
    asio::io_context io;
    tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    const auto port = acceptor.local_endpoint().port();
    LocalModel<Adapter> model(io.get_executor(), nlohmann::json{
        {"model", "test"},
        {"endpoint", {{"base_url", "http://127.0.0.1:" + std::to_string(port)}}},
        {"retry", {{"max_attempts", 3}, {"initial_backoff_ms", 1}}}
    });
    BOOST_REQUIRE(model.build());

    std::stop_source stop;
    bool disconnected = false;
    auto server = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        boost::beast::tcp_stream stream(io);
        stream.expires_after(std::chrono::seconds(2));
        co_await acceptor.async_accept(stream.socket(), asio::use_awaitable);
        boost::beast::flat_buffer buffer;
        http::request<http::string_body> request;
        co_await http::async_read(stream, buffer, request, asio::use_awaitable);
        if (send_headers) {
            const std::string start =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                "Transfer-Encoding: chunked\r\n\r\n";
            co_await asio::async_write(stream, asio::buffer(start), asio::use_awaitable);
        }
        stop.request_stop();
        char byte;
        boost::system::error_code error;
        co_await stream.async_read_some(asio::buffer(&byte, 1),
            asio::redirect_error(asio::use_awaitable, error));
        disconnected = error == asio::error::eof || error == asio::error::connection_reset;
    }, asio::use_future);

    tools::ToolRegistry registry;
    eventbus::EventBus bus;
    model_io::AgentInputState state;
    model_io::MessageItem message;
    message.role = "user";
    auto result = asio::co_spawn(io, loop::run(
        model, registry, bus, io.get_executor(), state, true, message, {},
        stop.get_token()), asio::use_future);
    std::thread worker([&] {
        io.run_for(std::chrono::seconds(3));
    });
    io.run_for(std::chrono::seconds(3));
    worker.join();

    BOOST_REQUIRE(result.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    BOOST_REQUIRE(server.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    server.get();
    BOOST_CHECK(result.get().status == loop::RunStatus::Cancelled);
    BOOST_CHECK(disconnected);
    BOOST_CHECK(state.loop->status == model_io::LoopStatus::Cancelled);
    BOOST_CHECK(state.turns.back().agent_loop_step.empty());
    // A retry would leave another client waiting for this unaccepted connection.
    BOOST_CHECK(io.stopped());
}

} // namespace

BOOST_AUTO_TEST_CASE(chat_headers_wait_is_interruptible) {
    interrupt_http_exchange<llm::chat_completions::ChatCompletionsModel>(false);
}

BOOST_AUTO_TEST_CASE(chat_stop_after_headers_is_interruptible) {
    interrupt_http_exchange<llm::chat_completions::ChatCompletionsModel>(true);
}

BOOST_AUTO_TEST_CASE(responses_headers_wait_is_interruptible) {
    interrupt_http_exchange<llm::responses::ResponsesModel>(false);
}

BOOST_AUTO_TEST_CASE(responses_stop_after_headers_is_interruptible) {
    interrupt_http_exchange<llm::responses::ResponsesModel>(true);
}

namespace {

/**
 * Exhausts a real adapter's retry budget with two HTTP 503 responses.
 * Exact request counts prove transport retry occurred without an extra retry
 * from loop. The deadline only guards a regression that leaves either side
 * waiting; no cancellation is requested in this failure scenario.
 */
template<class Adapter>
void exhausted_http_retries_return_failed() {
    asio::io_context io;
    tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    const auto port = acceptor.local_endpoint().port();
    LocalModel<Adapter> model(io.get_executor(), nlohmann::json{
        {"model", "test"},
        {"endpoint", {{"base_url", "http://127.0.0.1:" + std::to_string(port)}}},
        {"retry", {{"max_attempts", 1}, {"initial_backoff_ms", 1}}}
    });
    BOOST_REQUIRE(model.build());

    int requests = 0;
    auto server = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        for (int attempt = 0; attempt < 2; ++attempt) {
            boost::beast::tcp_stream stream(io);
            stream.expires_after(std::chrono::seconds(2));
            co_await acceptor.async_accept(stream.socket(), asio::use_awaitable);
            boost::beast::flat_buffer buffer;
            http::request<http::string_body> request;
            co_await http::async_read(stream, buffer, request, asio::use_awaitable);
            ++requests;

            http::response<http::string_body> response{
                http::status::service_unavailable, 11};
            response.set(http::field::content_type, "application/json");
            response.keep_alive(false);
            response.body() = R"({"error":{"message":"temporarily unavailable"}})";
            response.prepare_payload();
            co_await http::async_write(stream, response, asio::use_awaitable);
        }
    }, asio::use_future);

    tools::ToolRegistry registry;
    eventbus::EventBus bus;
    model_io::AgentInputState state;
    model_io::MessageItem message;
    message.role = "user";
    int finished = 0;
    auto subscription = bus.subscribe<loop::RunFinished>([&](const auto& event) {
        ++finished;
        BOOST_CHECK(event.result.status == loop::RunStatus::Failed);
        BOOST_CHECK(event.state.loop->status == model_io::LoopStatus::Failed);
        BOOST_CHECK(event.state.loop->phase == model_io::LoopPhase::Ready);
        BOOST_CHECK_EQUAL(event.state.loop->error, event.result.error);
    });
    auto result = asio::co_spawn(io, loop::run(
        model, registry, bus, io.get_executor(), state, true, message),
        asio::use_future);
    io.run_for(std::chrono::seconds(3));

    BOOST_REQUIRE(result.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    BOOST_REQUIRE(server.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    server.get();
    const auto outcome = result.get();
    BOOST_CHECK(outcome.status == loop::RunStatus::Failed);
    BOOST_CHECK(outcome.failure_stage == loop::RunFailureStage::ModelRequest);
    BOOST_CHECK(outcome.error.find("503") != std::string::npos);
    BOOST_CHECK_EQUAL(outcome.completed_exchanges, 0u);
    BOOST_CHECK_EQUAL(requests, 2);
    BOOST_CHECK_EQUAL(finished, 1);
    BOOST_REQUIRE_EQUAL(state.turns.size(), 1u);
    BOOST_CHECK(state.turns.front().agent_loop_step.empty());
    BOOST_CHECK(state.loop->pending_results.empty());
    BOOST_CHECK(io.stopped());
}

} // namespace

BOOST_AUTO_TEST_CASE(chat_http_retry_exhaustion_is_a_loop_failure) {
    exhausted_http_retries_return_failed<llm::chat_completions::ChatCompletionsModel>();
}

BOOST_AUTO_TEST_CASE(responses_http_retry_exhaustion_is_a_loop_failure) {
    exhausted_http_retries_return_failed<llm::responses::ResponsesModel>();
}
