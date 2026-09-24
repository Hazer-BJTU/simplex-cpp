#define BOOST_TEST_MODULE CoreSlowResolver
#include <boost/test/unit_test.hpp>
#include "core/confirmation.hpp"
#include "intercom/cancellable_exchange.hpp"
#include <boost/asio/use_future.hpp>
#include <future>
#include <thread>

extern "C" void resolver_reset();
extern "C" void resolver_wait_entered();
extern "C" void resolver_release();
namespace asio = boost::asio;

BOOST_AUTO_TEST_CASE(started_resolution_is_joined_after_timeout_or_stop) {
    for (bool cancel : {false, true}) {
        resolver_reset();
        asio::io_context io;
        auto scope = std::make_shared<core::ConfirmationScope>();
        const auto endpoint = load::websocket_endpoint(
            "ws://controlled-resolution.invalid:1/confirm");
        auto answer = asio::co_spawn(io, core::confirm({}, scope,
            io.get_executor(), endpoint,
            cancel ? std::chrono::seconds(30) : std::chrono::milliseconds(50),
            "s", "r"), asio::use_future);
        std::thread runner([&] { io.run(); });
        resolver_wait_entered(); // The backend is executing, not queued.
        if (cancel) scope->cancel();
        // A strand timer proves IO continues while the backend is blocked.
        std::promise<void> elapsed;
        asio::post(io, [&] {
            auto timer = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(100));
            timer->async_wait([&, timer](auto) { elapsed.set_value(); });
        });
        elapsed.get_future().wait();
        const bool joined = answer.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        resolver_release();
        runner.join();
        BOOST_TEST(joined);
        const auto result = answer.get();
        BOOST_CHECK(result.decision == tools::ConfirmDecision::Denied);
        // Even a successful late resolver result cannot authorize a tool.
        BOOST_TEST(result.reason.find(cancel ? "Operation canceled" : "timed out") != std::string::npos);
    }
}
