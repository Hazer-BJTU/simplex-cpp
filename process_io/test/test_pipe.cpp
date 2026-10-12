#define BOOST_TEST_MODULE ProcessIoPipe
#include <boost/test/unit_test.hpp>
#include "harness.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/post.hpp>
#include <thread>
#include <csignal>
#include <pthread.h>
#include <boost/scope/scope_exit.hpp>

using namespace process_io;

BOOST_AUTO_TEST_CASE(capabilities_and_unstarted_cleanup)
{
    Harness h;
    const auto capabilities = h.environment->capabilities();
    BOOST_CHECK(capabilities.topology == OutputTopology::Separate);
    BOOST_CHECK(capabilities.input_end == InputEndMode::HalfClose);
    BOOST_TEST(!capabilities.is_terminal);
    BOOST_TEST(!capabilities.can_resize);
    BOOST_CHECK_THROW((void)h.environment->snapshot(), std::logic_error);
    h.run([](Harness& h) -> asio::awaitable<void> {
        BOOST_CHECK((co_await h.environment->enqueue_input("before")) == InputResult::Closed);
        BOOST_CHECK((co_await h.environment->request_input_end()) == InputResult::Closed);
        BOOST_CHECK((co_await h.environment->resize(80, 24)) == ResizeResult::Unsupported);
        co_await h.environment->cancel_and_join();
        co_await h.environment->cancel_and_join();
        const auto snapshot = h.environment->snapshot();
        BOOST_TEST(snapshot.joined);
        BOOST_TEST(snapshot.output_complete);
        BOOST_TEST(!snapshot.terminal_output);
        BOOST_CHECK_THROW(h.environment->prepare_attachment(), std::logic_error);
    });
}

BOOST_AUTO_TEST_CASE(ordered_input_end_after_more_than_sixteen_large_messages)
{
    Harness h(0);
    h.run([](Harness& h) -> asio::awaitable<void> {
        h.launch("echo");
        co_await h.environment->start();
        std::string expected;
        for (int i = 0; i < 40; ++i) {
            std::string message(8193, static_cast<char>('a' + i % 26));
            message[10] = '\0';
            expected += message;
            BOOST_CHECK(
                (co_await h.environment->enqueue_input(std::move(message)))
                == InputResult::Accepted);
        }
        BOOST_CHECK((co_await h.environment->request_input_end()) == InputResult::Accepted);
        BOOST_CHECK((co_await h.environment->enqueue_input("late")) == InputResult::Closed);
        BOOST_CHECK((co_await h.environment->request_input_end()) == InputResult::Closed);
        BOOST_TEST(co_await h.environment->drain_until(std::chrono::steady_clock::now() + 5s));
        BOOST_TEST((co_await h.wait_exit()) == 0);
        const auto snapshot = h.environment->snapshot();
        BOOST_TEST(snapshot.standard_output->bytes == expected);
        BOOST_TEST(snapshot.standard_error->bytes == "done");
        BOOST_CHECK(snapshot.input_phase == InputPhase::Ended);
        BOOST_TEST(snapshot.input_end_requested);
        BOOST_TEST(!snapshot.joined);
        co_await h.environment->cancel_and_join();
        BOOST_TEST(h.environment->snapshot().joined);
        BOOST_TEST(!h.environment->snapshot().standard_output->truncated);
    });
}

BOOST_AUTO_TEST_CASE(capture_limit_discards_but_continues_draining_both_streams)
{
    Harness h(37);
    h.run([](Harness& h) -> asio::awaitable<void> {
        h.launch("burst");
        co_await h.environment->start();
        BOOST_TEST(co_await h.environment->drain_until(std::chrono::steady_clock::now() + 5s));
        BOOST_TEST((co_await h.wait_exit()) == 0);
        const auto snapshot = h.environment->snapshot();
        // Scheduling determines the split, but cannot increase the total.
        BOOST_TEST(
            snapshot.standard_output->bytes.size() + snapshot.standard_error->bytes.size() == 37U);
        BOOST_TEST(
            snapshot.standard_output->bytes
            == std::string(snapshot.standard_output->bytes.size(), 'x'));
        BOOST_TEST(
            snapshot.standard_error->bytes
            == std::string(snapshot.standard_error->bytes.size(), 'x'));
        BOOST_TEST(snapshot.standard_output->truncated);
        BOOST_TEST(snapshot.standard_error->truncated);
        BOOST_CHECK(snapshot.standard_output->end == StreamEnd::Eof);
        BOOST_CHECK(snapshot.standard_error->end == StreamEnd::Eof);
        BOOST_TEST(snapshot.output_complete);
        BOOST_TEST(!snapshot.joined); // Idle input pump still belongs to us.
    });
}

BOOST_AUTO_TEST_CASE(stop_admission_and_expired_drain_do_not_close_transports)
{
    Harness h;
    h.run([](Harness& h) -> asio::awaitable<void> {
        h.launch("echo");
        co_await h.environment->start();
        BOOST_CHECK((co_await h.environment->enqueue_input("accepted")) == InputResult::Accepted);
        co_await h.environment->stop_admission();
        BOOST_CHECK((co_await h.environment->enqueue_input("rejected")) == InputResult::Closed);
        BOOST_TEST(!(co_await h.environment->drain_until(std::chrono::steady_clock::now() + 20ms)));
        BOOST_CHECK(h.environment->snapshot().input_phase == InputPhase::Open);
        BOOST_TEST(!h.environment->snapshot().input_end_requested);
        BOOST_CHECK((co_await h.environment->request_input_end()) == InputResult::Accepted);
        BOOST_TEST(co_await h.environment->drain_until(std::chrono::steady_clock::now() + 5s));
        BOOST_TEST(h.environment->snapshot().standard_output->bytes == "accepted");
        BOOST_TEST((co_await h.wait_exit()) == 0);
    });
}

BOOST_AUTO_TEST_CASE(join_cancels_blocked_input_and_output_and_preserves_prefix)
{
    Harness h;
    h.run([](Harness& h) -> asio::awaitable<void> {
        h.launch("no-read");
        co_await h.environment->start();
        co_await h.ready();
        BOOST_CHECK(
            (co_await h.environment->enqueue_input(std::string(1024 * 1024, 'x')))
            == InputResult::Accepted);
        co_await h.pause(); // Let a write exceed pipe capacity and suspend.
        const auto began = std::chrono::steady_clock::now();
        co_await h.environment->cancel_and_join();
        BOOST_CHECK(std::chrono::steady_clock::now() - began < 1s);
        const auto snapshot = h.environment->snapshot();
        BOOST_TEST(snapshot.joined);
        BOOST_TEST(snapshot.output_complete);
        BOOST_TEST(snapshot.standard_output->bytes == "ready");
        BOOST_TEST(snapshot.standard_output->truncated);
        BOOST_CHECK(snapshot.standard_output->end == StreamEnd::Stopped);
        BOOST_CHECK(snapshot.input_phase == InputPhase::Stopped);
        boost::system::error_code error;
        BOOST_TEST(h.child->running(error)); // The environment never kills it.
    });
}

BOOST_AUTO_TEST_CASE(peer_input_closure_is_observable_and_not_an_unhandled_task_error)
{
    Harness h;
    h.run([](Harness& h) -> asio::awaitable<void> {
        h.launch("close-input");
        co_await h.environment->start();
        co_await h.ready();
        BOOST_CHECK((co_await h.environment->enqueue_input("no reader")) == InputResult::Accepted);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (h.environment->snapshot().input_phase == InputPhase::Open) {
            BOOST_REQUIRE((std::chrono::steady_clock::now() < deadline));
            co_await h.pause();
        }
        BOOST_CHECK(h.environment->snapshot().input_phase == InputPhase::PeerClosed);
        BOOST_TEST(!h.environment->snapshot().failure);
        BOOST_CHECK((co_await h.environment->enqueue_input("late")) == InputResult::Closed);
    });
}

BOOST_AUTO_TEST_CASE(launch_failure_and_start_before_launch_are_cleanup_safe)
{
    Harness h;
    h.run([](Harness& h) -> asio::awaitable<void> {
        auto attachment = h.environment->prepare_attachment();
        BOOST_CHECK_THROW(h.environment->prepare_attachment(), std::logic_error);
        process::LaunchSpec spec;
        spec.executable = "/nonexistent-simplex-process-io-child";
        BOOST_CHECK_THROW(
            subprocess::Child(spec, h.strand, attachment), subprocess::ProcessException);
        bool rejected = false;
        try {
            co_await h.environment->start();
        } catch (const std::logic_error&) {
            rejected = true;
        }
        BOOST_TEST(rejected);
        co_await h.environment->cancel_and_join();
        BOOST_TEST(h.environment->snapshot().joined);
    });
}

BOOST_AUTO_TEST_CASE(repeated_start_is_rejected_without_interrupting_pumps)
{
    Harness h;
    h.run([](Harness& h) -> asio::awaitable<void> {
        h.launch("exit");
        co_await h.environment->start();
        bool rejected = false;
        try {
            co_await h.environment->start();
        } catch (const std::logic_error&) {
            rejected = true;
        }
        BOOST_TEST(rejected);
        BOOST_TEST(co_await h.environment->drain_until(std::chrono::steady_clock::now() + 5s));
        BOOST_TEST(h.environment->snapshot().standard_output->bytes == "tail\n");
        BOOST_TEST(h.environment->snapshot().standard_error->bytes == "error\n");
        BOOST_TEST((co_await h.wait_exit()) == 0);
    });
}

BOOST_AUTO_TEST_CASE(unjoined_facade_destruction_requests_background_cancellation)
{
    Harness h;
    h.run([](Harness& h) -> asio::awaitable<void> {
        h.launch("no-read");
        co_await h.environment->start();
        co_await h.ready();
        std::thread destroy([&] {
            h.environment.reset();
        });
        destroy.join();
        co_return;
    }); // run() drains all task callbacks rather than requiring context.stop().
}

BOOST_AUTO_TEST_CASE(awaitable_operations_marshal_from_another_context_with_three_workers)
{
    asio::io_context caller;
    auto work = asio::make_work_guard(caller);
    std::jthread caller_thread([&] {
        caller.run();
    });
    Harness h;
    try {
        h.run(
            [&](Harness& h) -> asio::awaitable<void> {
                h.launch("echo");
                co_await asio::co_spawn(
                    caller,
                    [&h]() -> asio::awaitable<void> {
                        co_await h.environment->start();
                        BOOST_CHECK_THROW((void)h.environment->snapshot(), std::logic_error);
                        BOOST_CHECK(
                            (co_await h.environment->enqueue_input("cross-context"))
                            == InputResult::Accepted);
                        BOOST_CHECK(
                            (co_await h.environment->request_input_end()) == InputResult::Accepted);
                        BOOST_TEST(co_await h.environment->drain_until(
                            std::chrono::steady_clock::now() + 5s));
                        co_await h.environment->cancel_and_join();
                    },
                    asio::use_awaitable);
                BOOST_TEST(h.environment->snapshot().standard_output->bytes == "cross-context");
                BOOST_TEST(h.environment->snapshot().joined);
                BOOST_TEST((co_await h.wait_exit()) == 0);
            },
            3);
    } catch (...) {
        caller.stop();
        throw;
    }
    work.reset();
}

BOOST_AUTO_TEST_CASE(drain_is_interruptible_but_join_shields_caller_cancellation)
{
    Harness h;
    h.run([](Harness& h) -> asio::awaitable<void> {
        h.launch("no-read");
        co_await h.environment->start();
        co_await h.ready();
        asio::cancellation_signal cancel_drain;
        bool drain_done = false;
        std::exception_ptr drain_error;
        asio::co_spawn(
            h.strand,
            h.environment->drain_until(std::chrono::steady_clock::now() + 30s),
            asio::bind_cancellation_slot(cancel_drain.slot(), [&](std::exception_ptr error, bool) {
                drain_error = error;
                drain_done = true;
            }));
        co_await h.pause();
        cancel_drain.emit(asio::cancellation_type::all);
        while (!drain_done) {
            co_await h.pause();
        }
        BOOST_REQUIRE(drain_error);
        try {
            std::rethrow_exception(drain_error);
        } catch (const boost::system::system_error& error) {
            BOOST_TEST(error.code() == asio::error::operation_aborted);
        }
        BOOST_TEST(!h.environment->snapshot().output_complete);
        BOOST_TEST(h.environment->snapshot().accepting_input);

        co_await h.environment->enqueue_input(std::string(1024 * 1024, 'x'));
        co_await h.pause();
        asio::cancellation_signal cancel_join;
        bool joined = false;
        std::exception_ptr join_error;
        asio::co_spawn(
            h.strand,
            h.environment->cancel_and_join(),
            asio::bind_cancellation_slot(cancel_join.slot(), [&](std::exception_ptr error) {
                join_error = error;
                joined = true;
            }));
        asio::post(h.strand, [&] {
            cancel_join.emit(asio::cancellation_type::all);
        });
        while (!joined) {
            co_await h.pause();
        }
        BOOST_TEST(!join_error);
        BOOST_TEST(h.environment->snapshot().joined);
        BOOST_TEST(h.environment->snapshot().output_complete);
    });
}

BOOST_AUTO_TEST_CASE(peer_closure_preserves_sigpipe_disposition_mask_and_existing_pending_signal)
{
    Harness h;
    h.run([](Harness& h) -> asio::awaitable<void> {
        h.launch("close-input");
        co_await h.environment->start();
        co_await h.ready();
        struct sigaction original_action{};
        BOOST_REQUIRE(::sigaction(SIGPIPE, nullptr, &original_action) == 0);
        sigset_t blocked, previous;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPIPE);
        BOOST_REQUIRE(::pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0);
        boost::scope::scope_exit restore([&] {
            const timespec immediate{0, 0};
            ::sigtimedwait(&blocked, nullptr, &immediate);
            ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        });
        BOOST_REQUIRE(::raise(SIGPIPE) == 0);
        co_await h.environment->enqueue_input("broken pipe");
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (h.environment->snapshot().input_phase == InputPhase::Open) {
            BOOST_REQUIRE((std::chrono::steady_clock::now() < deadline));
            co_await h.pause();
        }
        BOOST_CHECK(h.environment->snapshot().input_phase == InputPhase::PeerClosed);
        sigset_t pending, after_mask;
        BOOST_REQUIRE(::sigpending(&pending) == 0);
        BOOST_TEST(::sigismember(&pending, SIGPIPE) == 1);
        BOOST_REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &after_mask) == 0);
        BOOST_TEST(::sigismember(&after_mask, SIGPIPE) == 1);
        struct sigaction after_action{};
        BOOST_REQUIRE(::sigaction(SIGPIPE, nullptr, &after_action) == 0);
        BOOST_CHECK(after_action.sa_handler == original_action.sa_handler);
        BOOST_TEST(after_action.sa_flags == original_action.sa_flags);
    });
}

BOOST_AUTO_TEST_CASE(forced_input_stop_preserves_output_until_the_separate_join_fence)
{
    Harness h;
    h.run([](Harness& h) -> asio::awaitable<void> {
        h.launch("no-read");
        co_await h.environment->start();
        co_await h.ready();
        co_await h.environment->enqueue_input(std::string(1024 * 1024, 'x'));
        co_await h.pause();
        co_await h.environment->cancel_input();
        co_await h.pause();
        const auto snapshot = h.environment->snapshot();
        BOOST_TEST(!snapshot.accepting_input);
        BOOST_TEST(!snapshot.output_complete);
        BOOST_TEST(!snapshot.joined);
        BOOST_TEST(!snapshot.failure);
        BOOST_TEST(!snapshot.input_end_requested);
        BOOST_CHECK(snapshot.input_phase == InputPhase::Stopped);
        BOOST_TEST(snapshot.standard_output->bytes == "ready");
        BOOST_CHECK(snapshot.standard_output->end == StreamEnd::Open);
        BOOST_CHECK((co_await h.environment->enqueue_input("late")) == InputResult::Closed);
        BOOST_CHECK((co_await h.environment->request_input_end()) == InputResult::Closed);
        co_await h.environment->cancel_and_join();
        BOOST_TEST(h.environment->snapshot().joined);
    });
}
