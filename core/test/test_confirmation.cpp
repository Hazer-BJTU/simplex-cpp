#define BOOST_TEST_MODULE CoreConfirmation
#include <boost/test/unit_test.hpp>
#include "core/confirmation.hpp"
#include "intercom/cancellable_exchange.hpp"
#include <boost/beast.hpp>
#include <boost/asio/use_future.hpp>
#include <future>
#include <thread>
#include <barrier>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using Json = nlohmann::json;

namespace {
/** A local peer that can stop progressing at each established transport stage. */
enum class Mode { LateApprove, Approve, Deny, Mismatch, Binary, Disconnect, ReadWait, CloseWait, UpgradeWait };
tools::InvokeConfirmEvent exercise(Mode mode, bool stop = false) {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    auto endpoint = load::websocket_endpoint("ws://127.0.0.1:"
        + std::to_string(acceptor.local_endpoint().port()) + "/confirm");
    auto scope = std::make_shared<core::ConfirmationScope>();
    auto peer = [&]() -> asio::awaitable<void> {
        websocket::stream<asio::ip::tcp::socket> socket(co_await acceptor.async_accept(asio::use_awaitable));
        if (mode == Mode::UpgradeWait) {
            asio::steady_timer timer(io, std::chrono::milliseconds(100));
            co_await timer.async_wait(asio::use_awaitable);
            co_return;
        }
        co_await socket.async_accept(asio::use_awaitable);
        beast::flat_buffer buffer;
        co_await socket.async_read(buffer, asio::use_awaitable);
        auto data = Json::parse(beast::buffers_to_string(buffer.data())).at("data");
        if (mode == Mode::Disconnect) co_return;
        if (mode == Mode::ReadWait) {
            boost::system::error_code error;
            buffer.consume(buffer.size());
            co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
            co_return;
        }
        if (mode == Mode::LateApprove) {
            // A valid reply is sent only after cancellation has closed approval
            // admission. Transport completion cannot reopen that decision.
            scope->cancel();
        }
        data["decision"] = mode == Mode::Deny ? "denied" : "approved";
        data["reason"] = "fixture";
        if (mode == Mode::Mismatch) data["confirmation_id"] = "wrong";
        const auto wire = Json({{"type", "confirmation_response"}, {"data", data}}).dump();
        socket.text(mode != Mode::Binary);
        boost::system::error_code write_error;
        co_await socket.async_write(asio::buffer(wire),
            asio::redirect_error(asio::use_awaitable, write_error));
        if (write_error && mode == Mode::LateApprove) co_return;
        if (write_error) throw boost::system::system_error(write_error);
        if (mode == Mode::CloseWait) {
            asio::steady_timer timer(io, std::chrono::milliseconds(100));
            co_await timer.async_wait(asio::use_awaitable);
        } else {
            boost::system::error_code error;
            co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
        }
    };
    auto server = asio::co_spawn(io, peer, asio::use_future);
    tools::InvokeConfirmEvent event;
    event.query.name = "fixture";
    event.query.id = "call";
    auto result = asio::co_spawn(io, core::confirm(event, scope, io.get_executor(), endpoint,
        std::chrono::milliseconds(mode == Mode::Approve || mode == Mode::Deny
            || mode == Mode::Mismatch || mode == Mode::Binary || mode == Mode::Disconnect ? 1000 : 50),
        "session", "run"), asio::use_future);
    asio::steady_timer cancel(io, std::chrono::milliseconds(20));
    if (stop) cancel.async_wait([scope](auto) { scope->cancel(); });
    io.run();
    server.get();
    return result.get();
}
}

BOOST_AUTO_TEST_CASE(only_matching_explicit_approval_can_authorize) {
    BOOST_CHECK(exercise(Mode::Approve).decision == tools::ConfirmDecision::Approved);
    for (auto mode : {Mode::Deny, Mode::Mismatch, Mode::Binary, Mode::Disconnect, Mode::LateApprove})
        BOOST_CHECK(exercise(mode).decision == tools::ConfirmDecision::Denied);
}

BOOST_AUTO_TEST_CASE(deadline_and_cancellation_join_upgrade_read_and_close) {
    for (auto mode : {Mode::UpgradeWait, Mode::ReadWait, Mode::CloseWait}) {
        BOOST_CHECK(exercise(mode).decision == tools::ConfirmDecision::Denied);
        BOOST_CHECK(exercise(mode, true).decision == tools::ConfirmDecision::Denied);
    }
}

BOOST_AUTO_TEST_CASE(pre_cancelled_and_missing_endpoint_never_connect) {
    asio::io_context io;
    auto scope = std::make_shared<core::ConfirmationScope>();
    auto absent = asio::co_spawn(io, core::confirm({}, scope, io.get_executor(), {},
        std::chrono::seconds(1), "s", "r"), asio::use_future);
    scope->cancel();
    auto endpoint = load::websocket_endpoint("ws://127.0.0.1:1/no-server");
    auto stopped = asio::co_spawn(io, core::confirm({}, scope, io.get_executor(), endpoint,
        std::chrono::seconds(1), "s", "r"), asio::use_future);
    io.run();
    BOOST_CHECK(absent.get().decision == tools::ConfirmDecision::Denied);
    BOOST_CHECK(stopped.get().decision == tools::ConfirmDecision::Denied);
}

BOOST_AUTO_TEST_CASE(approval_and_cancellation_have_one_ordered_boundary) {
    auto scope = std::make_shared<core::ConfirmationScope>();
    BOOST_TEST(scope->settle_approval(true));
    std::thread stopper([scope] { scope->cancel(); });
    stopper.join();
    BOOST_TEST(!scope->settle_approval(true));
    BOOST_TEST(!scope->settle_approval(false));
    BOOST_TEST(scope->token().stop_requested());
}

BOOST_AUTO_TEST_CASE(parallel_pending_confirmations_cancel_together) {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    const auto endpoint = load::websocket_endpoint("ws://127.0.0.1:"
        + std::to_string(acceptor.local_endpoint().port()) + "/confirm");
    auto scope = std::make_shared<core::ConfirmationScope>();
    int received = 0;
    std::vector<std::future<void>> peers;
    auto accept = [&]() -> asio::awaitable<void> {
        for (int i = 0; i < 4; ++i) {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            peers.push_back(asio::co_spawn(io,
                [&, socket = std::move(socket)]() mutable -> asio::awaitable<void> {
                    websocket::stream<asio::ip::tcp::socket> stream(std::move(socket));
                    co_await stream.async_accept(asio::use_awaitable);
                    beast::flat_buffer buffer;
                    co_await stream.async_read(buffer, asio::use_awaitable);
                    if (++received == 4) scope->cancel();
                    boost::system::error_code error;
                    co_await stream.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
                }, asio::use_future));
        }
    };
    auto server = asio::co_spawn(io, accept, asio::use_future);
    std::vector<std::future<tools::InvokeConfirmEvent>> answers;
    for (int i = 0; i < 4; ++i) {
        answers.push_back(asio::co_spawn(io, core::confirm({}, scope, io.get_executor(),
            endpoint, std::chrono::seconds(2), "s", "r"), asio::use_future));
    }
    io.run();
    server.get();
    for (auto& peer : peers) peer.get();
    BOOST_TEST(received == 4);
    for (auto& answer : answers)
        BOOST_CHECK(answer.get().decision == tools::ConfirmDecision::Denied);
}

BOOST_AUTO_TEST_CASE(cancellation_aborts_a_backpressured_write) {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    auto endpoint = load::websocket_endpoint("ws://127.0.0.1:"
        + std::to_string(acceptor.local_endpoint().port()) + "/confirm");
    std::stop_source stop;
    asio::steady_timer cancel(io);
    auto peer = [&]() -> asio::awaitable<void> {
        websocket::stream<asio::ip::tcp::socket> socket(
            co_await acceptor.async_accept(asio::use_awaitable));
        socket.next_layer().set_option(asio::socket_base::receive_buffer_size(1024));
        co_await socket.async_accept(asio::use_awaitable);
        cancel.expires_after(std::chrono::milliseconds(20));
        cancel.async_wait([&](auto) { stop.request_stop(); });
        asio::steady_timer hold(io, std::chrono::milliseconds(80));
        co_await hold.async_wait(asio::use_awaitable);
    };
    auto server = asio::co_spawn(io, peer, asio::use_future);
    auto exchange = asio::co_spawn(io, intercom::cancellable_exchange(
        io.get_executor(), endpoint, std::string(16 * 1024 * 1024, 'x'),
        std::chrono::seconds(2), stop.get_token()), asio::use_future);
    io.run();
    server.get();
    BOOST_CHECK_EXCEPTION(exchange.get(), boost::system::system_error,
        [](const auto& error) { return error.code() == asio::error::operation_aborted; });
}

BOOST_AUTO_TEST_CASE(approval_and_cancel_contend_at_the_control_boundary) {
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto scope = std::make_shared<core::ConfirmationScope>();
        std::barrier start(3);
        bool approved = false;
        std::thread decision([&] {
            start.arrive_and_wait();
            approved = scope->settle_approval(true);
        });
        std::thread cancel([&] {
            start.arrive_and_wait();
            scope->cancel();
        });
        start.arrive_and_wait();
        decision.join();
        cancel.join();
        // Either first decision is legal; a later one can never be admitted.
        BOOST_TEST(!scope->settle_approval(true));
        BOOST_TEST(scope->token().stop_requested());
        if (!approved) BOOST_TEST(!scope->settle_approval(true));
    }
    // Explicitly coordinate each legal ordering without sleep-based races.
    for (bool cancel_first : {true, false}) {
        auto scope = std::make_shared<core::ConfirmationScope>();
        std::promise<void> first_done;
        auto ready = first_done.get_future();
        bool approved = false;
        std::thread first([&] {
            if (cancel_first) scope->cancel();
            else approved = scope->settle_approval(true);
            first_done.set_value();
        });
        std::thread second([&] {
            ready.wait();
            if (cancel_first) approved = scope->settle_approval(true);
            else scope->cancel();
        });
        first.join();
        second.join();
        BOOST_TEST(approved == !cancel_first);
        BOOST_TEST(!scope->settle_approval(true));
    }
}

BOOST_AUTO_TEST_CASE(confirmation_options_validate_atomically_and_return_owned_metadata) {
    core::ConfirmationOptions options;
    const auto& readonly = options;
    const auto expected = Json::array({{
        {"name", "mode"}, {"options", {"ask", "approve", "deny"}}
    }});
    BOOST_TEST(readonly.get_options() == expected);
    BOOST_CHECK(options.mode() == core::ConfirmationMode::Ask);
    auto copy = readonly.get_options();
    copy.clear();
    BOOST_TEST(readonly.get_options() == expected);
    options.handle_options({{"mode", "deny"}});
    options.handle_options(Json::object());
    for (const auto& invalid : std::vector<Json>{
        nullptr, Json::array(), {{"mode", nullptr}}, {{"mode", 1}},
        {{"mode", "unknown"}}, {{"mode", "approve"}, {"timeout_ms", 1}},
        {{"endpoint", "ws://other"}}
    }) {
        BOOST_CHECK_THROW(options.handle_options(invalid), std::invalid_argument);
        BOOST_CHECK(options.mode() == core::ConfirmationMode::Deny);
    }
    const core::ConfirmationScope previous(options.mode());
    options.handle_options({{"mode", "approve"}});
    BOOST_CHECK(previous.mode() == core::ConfirmationMode::Deny);
    BOOST_CHECK(options.mode() == core::ConfirmationMode::Approve);
    options.handle_options({{"mode", "ask"}});
    BOOST_CHECK(options.mode() == core::ConfirmationMode::Ask);
    BOOST_TEST(readonly.get_options() == expected);
}

BOOST_AUTO_TEST_CASE(local_confirmation_modes_respect_scope_and_cancellation) {
    for (const auto mode : {core::ConfirmationMode::Ask,
                           core::ConfirmationMode::Approve, core::ConfirmationMode::Deny}) {
        for (const bool stopped : {false, true}) {
            asio::io_context io;
            auto scope = std::make_shared<core::ConfirmationScope>(mode);
            if (stopped) scope->cancel();
            // No endpoint: Ask fails closed, but explicit local modes need no IO.
            auto result = asio::co_spawn(io, core::confirm({}, scope, io.get_executor(), {},
                std::chrono::seconds(1), "s", "r"), asio::use_future);
            io.run();
            BOOST_CHECK(result.get().decision == (!stopped && mode == core::ConfirmationMode::Approve
                ? tools::ConfirmDecision::Approved : tools::ConfirmDecision::Denied));
        }
    }
    asio::io_context io;
    auto absent = asio::co_spawn(io, core::confirm({}, {}, io.get_executor(), {},
        std::chrono::seconds(1), "s", "r"), asio::use_future);
    io.run();
    BOOST_CHECK(absent.get().decision == tools::ConfirmDecision::Denied);
}
