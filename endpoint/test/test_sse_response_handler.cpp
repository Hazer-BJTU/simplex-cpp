// Deterministic, offline unit tests for SSEResponseHandler.
//
// These exercise the framing, control, and abort semantics of the handler in
// isolation: a plain boost::asio::io_context drives the put/get coroutines (no
// sockets, no TLS). The underlying concurrent_channel has zero capacity
// (rendezvous), so a producer's async_send only completes once a consumer's
// async_receive is pending. The helpers below therefore spawn the get() side
// alongside put() on the same io_context and let the scheduler interleave them.
//
// io_context::poll() (not run()) is used throughout: it drains ready handlers
// without blocking, which lets a get() that stays blocked (empty channel) remain
// suspended across a control call — exactly what the "channel stays readable"
// tests need to observe.
#define BOOST_TEST_MODULE SSEResponseHandlerTests
#include <boost/test/unit_test.hpp>

#include "endpoint/request.hpp"

#include <boost/asio.hpp>

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace asio = boost::asio;

using endpoint::SSEAborted;
using endpoint::SSEHandlerState;
using endpoint::SSEResponseHandler;

// An SSE field line, matching SSEResponseHandler<...>::LineInfo.
using Field = std::pair<std::string, std::string>;

// A handler whose Product is simply the verbatim field list of each event.
class FieldHandler final : public SSEResponseHandler<std::vector<Field>> {
public:
    using SSEResponseHandler<std::vector<Field>>::SSEResponseHandler;

    std::vector<Field> _handle_message(std::span<const Field> message) override {
        return std::vector<Field>(message.begin(), message.end());
    }
};

// A handler whose decode step always throws — used to test put()'s error path.
class ThrowingHandler final : public SSEResponseHandler<std::vector<Field>> {
public:
    using SSEResponseHandler<std::vector<Field>>::SSEResponseHandler;

    std::vector<Field> _handle_message(std::span<const Field>) override {
        throw std::runtime_error("decode boom");
    }
};

// A handler that exposes the line-buffer internals so the rolling-trim
// behavior can be observed directly.
class InspectableHandler final : public SSEResponseHandler<std::vector<Field>> {
public:
    InspectableHandler(asio::any_io_executor executor, std::size_t line_window)
        : SSEResponseHandler<std::vector<Field>>(executor, line_window) {}

    std::vector<Field> _handle_message(std::span<const Field> message) override {
        return std::vector<Field>(message.begin(), message.end());
    }

    std::size_t line_count() const { return _lines.size(); }
    std::size_t base() const { return _base; }
};

// A handler that overrides _restart_index() to rewind to the boundary of the
// last event carrying an id: field — the Last-Event-ID-style checkpoint. The
// checkpoint is captured from _next_line inside _handle_message, which only
// works if the handler's line numbering stays absolute across rolling trims.
class IdCheckpointHandler final : public SSEResponseHandler<std::vector<Field>> {
public:
    IdCheckpointHandler(asio::any_io_executor executor, std::size_t line_window)
        : SSEResponseHandler<std::vector<Field>>(executor, line_window) {}

    std::vector<Field> _handle_message(std::span<const Field> message) override {
        std::vector<Field> event(message.begin(), message.end());
        for (const auto& line : event) {
            if (line.first == "id") {
                // _next_line already points past this event's blank delimiter:
                // an absolute, trim-stable boundary to resume from.
                _checkpoint = _next_line;
                break;
            }
        }
        return event;
    }

    std::size_t _restart_index() const override { return _checkpoint; }

    std::size_t base() const { return _base; }
    std::size_t checkpoint() const { return _checkpoint; }

private:
    std::size_t _checkpoint = 0;
};

// --- helpers --------------------------------------------------------------

static std::string data_value(const std::vector<Field>& event) {
    for (const auto& [field, value] : event)
        if (field == "data") return value;
    return {};
}

static std::size_t data_line_count(const std::vector<Field>& event) {
    std::size_t count = 0;
    for (const auto& [field, value] : event)
        if (field == "data") ++count;
    return count;
}

static std::string field_value(
    const std::vector<Field>& event, const std::string& name) {
    for (const auto& [field, value] : event)
        if (field == name) return value;
    return {};
}

// Pump the io_context once: run ready handlers (incl. rendezvous completions)
// without blocking on suspended coroutines.
static void pump(asio::io_context& io) {
    io.poll();
    io.restart();
}

// Drive put(chunk) on the handler together with `expect` get() consumers, and
// return the decoded events in delivery order. Rethrows any get() exception.
template<typename Handler>
static std::vector<std::vector<Field>> exchange(
    asio::io_context& io, Handler& handler,
    std::string_view chunk, std::size_t expect) {
    std::vector<std::optional<std::vector<Field>>> received(expect);
    std::vector<std::optional<std::exception_ptr>> errors(expect);

    for (std::size_t index = 0; index < expect; ++index) {
        asio::co_spawn(
            io,
            [&, index]() -> asio::awaitable<void> {
                try {
                    received[index] = co_await handler.get();
                } catch (...) {
                    errors[index] = std::current_exception();
                }
            },
            asio::detached);
    }
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> { co_await handler.put(chunk); },
        asio::detached);

    pump(io);

    std::vector<std::vector<Field>> out;
    for (std::size_t index = 0; index < expect; ++index) {
        if (errors[index]) std::rethrow_exception(*errors[index]);
        out.push_back(std::move(*received[index]));
    }
    return out;
}

// Run a single get() to completion (used by the abort/finish cases).
template<typename Handler>
static void run_get(
    asio::io_context& io, Handler& handler,
    std::optional<std::vector<Field>>& received,
    std::optional<std::exception_ptr>& error) {
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                received = co_await handler.get();
            } catch (...) {
                error = std::current_exception();
            }
        },
        asio::detached);
    pump(io);
}

// --- framing --------------------------------------------------------------

BOOST_AUTO_TEST_CASE(single_event_is_delivered) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    auto events = exchange(io, handler, "data: hello\n\n", 1);
    BOOST_TEST(events.size() == 1u);
    BOOST_TEST(data_value(events[0]) == "hello");
}

BOOST_AUTO_TEST_CASE(multiline_event_groups_fields_until_blank_line) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    auto events =
        exchange(io, handler, "event: add\ndata: a\ndata: b\n\n", 1);
    BOOST_TEST(events.size() == 1u);
    BOOST_TEST(data_line_count(events[0]) == 2u);
    BOOST_TEST(field_value(events[0], "event") == "add");
}

BOOST_AUTO_TEST_CASE(multiple_events_in_one_chunk) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    auto events =
        exchange(io, handler, "data: first\n\ndata: second\n\n", 2);
    BOOST_TEST(events.size() == 2u);
    BOOST_TEST(data_value(events[0]) == "first");
    BOOST_TEST(data_value(events[1]) == "second");
}

BOOST_AUTO_TEST_CASE(events_split_across_chunks_are_reassembled) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    std::optional<std::vector<Field>> received;
    std::optional<std::exception_ptr> error;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                received = co_await handler.get();
            } catch (...) {
                error = std::current_exception();
            }
        },
        asio::detached);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> { co_await handler.put("data: par"); },
        asio::detached);
    pump(io);
    BOOST_TEST(!received);   // incomplete event: nothing delivered yet
    BOOST_TEST(!error);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> { co_await handler.put("tial\n\n"); },
        asio::detached);
    pump(io);

    BOOST_REQUIRE(received);
    BOOST_TEST(data_value(*received) == "partial");
    BOOST_TEST(!error);
}

BOOST_AUTO_TEST_CASE(crlf_line_endings_are_tolerated) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    auto events = exchange(io, handler, "data: crlf\r\n\r\n", 1);
    BOOST_TEST(events.size() == 1u);
    BOOST_TEST(data_value(events[0]) == "crlf");
}

BOOST_AUTO_TEST_CASE(leading_blank_lines_are_skipped) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    auto events = exchange(io, handler, "\n\ndata: x\n\n", 1);
    BOOST_TEST(events.size() == 1u);
    BOOST_TEST(data_value(events[0]) == "x");
}

BOOST_AUTO_TEST_CASE(colonless_lines_are_dropped) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    // A line with no colon is comment/junk and is dropped; the subsequent
    // well-formed event is still delivered intact.
    auto events =
        exchange(io, handler, "this has no colon\ndata: kept\n\n", 1);
    BOOST_TEST(events.size() == 1u);
    BOOST_TEST(events[0].size() == 1u);
    BOOST_TEST(data_value(events[0]) == "kept");
}

BOOST_AUTO_TEST_CASE(line_with_only_colon_yields_empty_named_field) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    // ":" parses to an empty field name (colon at position 0); it is retained,
    // not dropped, and does not disturb the following event.
    auto events = exchange(io, handler, ":\ndata: ok\n\n", 1);
    BOOST_TEST(events.size() == 1u);
    BOOST_TEST(events[0].size() == 2u);
    BOOST_TEST(data_value(events[0]) == "ok");
}

BOOST_AUTO_TEST_CASE(exactly_one_leading_space_after_colon_is_stripped) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    auto one = exchange(io, handler, "data: spaced\n\n", 1);
    BOOST_TEST(data_value(one[0]) == "spaced");

    FieldHandler handler2(io.get_executor());
    auto two = exchange(io, handler2, "data:  two-spaces\n\n", 1);
    // Only the first space is consumed; the second survives in the value.
    BOOST_TEST(data_value(two[0]) == " two-spaces");
}

BOOST_AUTO_TEST_CASE(id_and_retry_fields_are_parsed) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    auto events =
        exchange(io, handler, "id: 42\nretry: 5000\ndata: payload\n\n", 1);
    BOOST_TEST(events.size() == 1u);
    BOOST_TEST(field_value(events[0], "id") == "42");
    BOOST_TEST(field_value(events[0], "retry") == "5000");
    BOOST_TEST(data_value(events[0]) == "payload");
}

// --- control surface ------------------------------------------------------

BOOST_AUTO_TEST_CASE(state_alias_matches_hoisted_enum) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    SSEResponseHandler<std::vector<Field>>::State alias = SSEHandlerState::RUNNING;
    BOOST_TEST(alias == handler.get_state());
    BOOST_TEST(handler.get_state() == SSEHandlerState::RUNNING);
}

BOOST_AUTO_TEST_CASE(suspend_records_state_and_returns_restart_index) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    exchange(io, handler, "data: first\n\n", 1);

    std::size_t restart_at =
        handler.suspend(SSEResponseHandler<std::vector<Field>>::State::RESUMABLE);
    BOOST_TEST(handler.get_state() == SSEHandlerState::RESUMABLE);
    // _restart_index() defaults to the current cursor (after the last delivered
    // event), so it must have advanced past the consumed lines.
    BOOST_TEST(restart_at >= 1u);

    // ERROR / DONE are accepted too — suspend does not hardcode RESUMABLE.
    handler.suspend(SSEHandlerState::ERROR);
    BOOST_TEST(handler.get_state() == SSEHandlerState::ERROR);
    handler.suspend(SSEHandlerState::DONE);
    BOOST_TEST(handler.get_state() == SSEHandlerState::DONE);
}

BOOST_AUTO_TEST_CASE(reset_rewinds_to_checkpoint_and_returns_to_running) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    exchange(io, handler, "data: a\n\ndata: b\n\n", 2);
    handler.suspend(SSEHandlerState::RESUMABLE);
    BOOST_TEST(handler.get_state() == SSEHandlerState::RESUMABLE);

    handler.reset(0);
    BOOST_TEST(handler.get_state() == SSEHandlerState::RUNNING);

    // After rewinding to 0, fresh input is delivered from scratch.
    auto events = exchange(io, handler, "data: again\n\n", 1);
    BOOST_TEST(data_value(events[0]) == "again");
}

BOOST_AUTO_TEST_CASE(reset_out_of_range_throws) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    BOOST_CHECK_THROW(handler.reset(1), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(suspend_and_reset_keep_the_channel_readable) {
    // A blocked get() must survive a put-side suspend()/reset() unchanged: those
    // operations never touch the channel, so the consumer is unaware. Only
    // finish() (which closes the channel) unblocks it.
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    std::optional<std::vector<Field>> received;
    std::optional<std::exception_ptr> error;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                received = co_await handler.get();
            } catch (...) {
                error = std::current_exception();
            }
        },
        asio::detached);
    pump(io);              // get() runs until it blocks on async_receive
    BOOST_TEST(!received); // still blocked: channel open, no event yet

    handler.suspend(SSEHandlerState::RESUMABLE);
    handler.reset(0);
    pump(io);              // nothing new to do; get() stays blocked
    BOOST_TEST(!received);
    BOOST_TEST(!error);
    BOOST_TEST(handler.get_state() == SSEHandlerState::RUNNING);

    handler.finish(SSEHandlerState::DONE);
    pump(io);              // close() completes the pending receive -> throws
    BOOST_TEST(!received);
    BOOST_REQUIRE(error);
    try {
        std::rethrow_exception(*error);
    } catch (const SSEAborted& aborted) {
        BOOST_TEST(aborted.state() == SSEHandlerState::DONE);
    }
}

// --- rolling trim ---------------------------------------------------------

BOOST_AUTO_TEST_CASE(consumed_history_is_trimmed_to_the_retention_window) {
    asio::io_context io;
    // Window of 8 consumed lines: each event is a data line + a blank line.
    InspectableHandler handler(io.get_executor(), 8);

    for (std::size_t i = 0; i < 50; ++i) {
        auto events =
            exchange(io, handler, "data: event-" + std::to_string(i) + "\n\n", 1);
        // Decoding must stay correct across many trims.
        BOOST_TEST(data_value(events[0]) == "event-" + std::to_string(i));
    }

    // 50 events × 2 lines = 100 lines consumed; only the 8 most recent are
    // retained, and _base advanced so absolute indices stay stable.
    BOOST_TEST(handler.line_count() == 8u);
    BOOST_TEST(handler.base() == 92u);
}

BOOST_AUTO_TEST_CASE(suspend_reset_round_trip_survives_rolling_trim) {
    asio::io_context io;
    InspectableHandler handler(io.get_executor(), 8);

    for (std::size_t i = 0; i < 10; ++i)
        exchange(io, handler, "data: e" + std::to_string(i) + "\n\n", 1);

    // 20 lines consumed, 8 retained: the cursor sits at absolute line 20.
    BOOST_TEST(handler.line_count() == 8u);
    BOOST_TEST(handler.base() == 12u);

    std::size_t restart_at = handler.suspend(SSEHandlerState::RESUMABLE);
    BOOST_TEST(restart_at == 20u);   // absolute cursor, stable across trims
    handler.reset(restart_at);       // still inside the retained window
    BOOST_TEST(handler.get_state() == SSEHandlerState::RUNNING);

    auto events = exchange(io, handler, "data: resumed\n\n", 1);
    BOOST_TEST(data_value(events[0]) == "resumed");

    // The oldest retained checkpoint (the window frontier) is reachable too.
    handler.suspend(SSEHandlerState::RESUMABLE);
    handler.reset(handler.base());
    events = exchange(io, handler, "data: tail\n\n", 1);
    BOOST_TEST(data_value(events[0]) == "tail");
}

BOOST_AUTO_TEST_CASE(reset_below_the_retention_window_throws) {
    asio::io_context io;
    InspectableHandler handler(io.get_executor(), 8);

    for (std::size_t i = 0; i < 10; ++i)
        exchange(io, handler, "data: e" + std::to_string(i) + "\n\n", 1);

    // 20 lines consumed, 8 retained — the first 12 are trimmed away.
    BOOST_TEST(handler.base() == 12u);
    BOOST_CHECK_THROW(handler.reset(0), std::out_of_range);
    BOOST_CHECK_THROW(handler.reset(11), std::out_of_range);

    // The frontier itself is still a valid checkpoint.
    handler.reset(12);
    BOOST_TEST(handler.get_state() == SSEHandlerState::RUNNING);
}

BOOST_AUTO_TEST_CASE(zero_window_trims_all_consumed_history) {
    asio::io_context io;
    InspectableHandler handler(io.get_executor(), 0);

    auto events = exchange(io, handler, "data: a\n\ndata: b\n\n", 2);
    BOOST_TEST(data_value(events[1]) == "b");
    BOOST_TEST(handler.line_count() == 0u);   // everything consumed is dropped
    BOOST_TEST(handler.base() == 4u);

    // The cursor checkpoint is always inside the window, so the suspend/reset
    // round trip still works when nothing older is retained.
    std::size_t restart_at = handler.suspend(SSEHandlerState::RESUMABLE);
    handler.reset(restart_at);
    events = exchange(io, handler, "data: again\n\n", 1);
    BOOST_TEST(data_value(events[0]) == "again");

    BOOST_CHECK_THROW(handler.reset(0), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(overridden_restart_checkpoint_stays_absolute_across_trims) {
    asio::io_context io;
    // Window of 16 lines; ids appear every 4th event, so the checkpoint lags
    // the cursor by at most 3 events (6 lines) and always stays in-window.
    IdCheckpointHandler handler(io.get_executor(), 16);

    for (std::size_t i = 0; i < 20; ++i) {
        std::string chunk = (i % 4 == 0)
            ? "id: " + std::to_string(i) + "\ndata: e" + std::to_string(i) + "\n\n"
            : "data: e" + std::to_string(i) + "\n\n";
        auto events = exchange(io, handler, chunk, 1);
        BOOST_TEST(data_value(events[0]) == "e" + std::to_string(i));
    }

    // 20 events of 2 lines plus 5 extra id lines = 45 lines; with a window of
    // 16, the 29 oldest are trimmed. The checkpoint captured inside
    // _handle_message (the boundary after event 16, the last one with an id)
    // must keep its absolute value nonetheless.
    BOOST_TEST(handler.base() == 29u);
    BOOST_TEST(handler.checkpoint() == 39u);

    std::size_t restart_at = handler.suspend(SSEHandlerState::RESUMABLE);
    BOOST_TEST(restart_at == 39u);
    BOOST_TEST(restart_at >= handler.base());   // still in the retained window

    handler.reset(restart_at);
    BOOST_TEST(handler.get_state() == SSEHandlerState::RUNNING);
    auto events = exchange(io, handler, "data: resumed\n\n", 1);
    BOOST_TEST(data_value(events[0]) == "resumed");
}

// --- abort semantics ------------------------------------------------------

BOOST_AUTO_TEST_CASE(sseaborted_carries_captured_state) {
    for (auto state : {SSEHandlerState::RUNNING, SSEHandlerState::RESUMABLE,
                       SSEHandlerState::ERROR, SSEHandlerState::DONE}) {
        SSEAborted aborted("boom", state);
        BOOST_TEST(std::string(aborted.what()) == "boom");
        BOOST_TEST(aborted.state() == state);
    }
}

BOOST_AUTO_TEST_CASE(finish_default_aborts_get_as_done) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    handler.finish();      // defaults to DONE
    BOOST_TEST(handler.get_state() == SSEHandlerState::DONE);

    std::optional<std::vector<Field>> received;
    std::optional<std::exception_ptr> error;
    run_get(io, handler, received, error);

    BOOST_TEST(!received);
    BOOST_REQUIRE(error);
    try {
        std::rethrow_exception(*error);
    } catch (const SSEAborted& aborted) {
        BOOST_TEST(aborted.state() == SSEHandlerState::DONE);
    }
}

BOOST_AUTO_TEST_CASE(finish_error_aborts_get_as_error) {
    asio::io_context io;
    FieldHandler handler(io.get_executor());

    handler.finish(SSEHandlerState::ERROR);
    BOOST_TEST(handler.get_state() == SSEHandlerState::ERROR);

    std::optional<std::vector<Field>> received;
    std::optional<std::exception_ptr> error;
    run_get(io, handler, received, error);

    BOOST_REQUIRE(error);
    try {
        std::rethrow_exception(*error);
    } catch (const SSEAborted& aborted) {
        BOOST_TEST(aborted.state() == SSEHandlerState::ERROR);
    }
}

BOOST_AUTO_TEST_CASE(put_decode_fault_drives_handler_to_error_and_aborts_get) {
    asio::io_context io;
    ThrowingHandler handler(io.get_executor());

    // put() evaluates _handle_message during async_send's argument list; the
    // throw is caught by put()'s catch(...), which finish(ERROR)s the channel
    // and rethrows the original exception to the producer.
    std::optional<std::exception_ptr> put_error;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await handler.put("data: bad\n\n");
            } catch (...) {
                put_error = std::current_exception();
            }
        },
        asio::detached);
    pump(io);

    BOOST_REQUIRE(put_error);
    try {
        std::rethrow_exception(*put_error);
        BOOST_FAIL("expected runtime_error to propagate from put()");
    } catch (const std::runtime_error& error) {
        BOOST_TEST(std::string(error.what()) == "decode boom");
    }
    BOOST_TEST(handler.get_state() == SSEHandlerState::ERROR);

    // The closed channel must now abort a waiting get() with state ERROR.
    std::optional<std::vector<Field>> received;
    std::optional<std::exception_ptr> get_error;
    run_get(io, handler, received, get_error);
    BOOST_REQUIRE(get_error);
    try {
        std::rethrow_exception(*get_error);
    } catch (const SSEAborted& aborted) {
        BOOST_TEST(aborted.state() == SSEHandlerState::ERROR);
    }
}
