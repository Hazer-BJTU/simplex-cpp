// Deterministic, offline tests for the Responses-API stream DECODER
// (ResponsesStreamHandler: one delta per SSE event, nothing accumulated).
// A plain boost::asio::io_context drives the put/get coroutines (no
// sockets, no TLS), mirroring test_sse_response_handler.cpp: the channel is
// zero-capacity rendezvous, so consumers are pre-spawned to pair with each
// put() — and `expect` counts events exactly. The accumulation/assembly,
// status and hook behaviour that used to live here moved one layer up with
// the reader — see test_responses_reader.cpp.
#define BOOST_TEST_MODULE responses_stream
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "endpoint/request.hpp"
#include "endpoint/responses/delta.hpp"
#include "endpoint/responses/stream_handler.hpp"

namespace asio = boost::asio;

using endpoint::responses::DeltaKind;
using endpoint::responses::ResponsesDelta;
using endpoint::responses::ResponsesStreamHandler;
using endpoint::responses::is_terminal;

// --- helpers ------------------------------------------------------------------

// Pump the io_context once: run ready handlers without blocking.
static void pump(asio::io_context& io) {
    io.poll();
    io.restart();
}

// Drive put(chunk) with `expect` pre-spawned get() consumers; return the
// deltas in delivery order. Rethrows any get() exception.
static std::vector<ResponsesDelta> exchange(
    asio::io_context& io, ResponsesStreamHandler& handler,
    std::string_view chunk, std::size_t expect) {
    std::vector<std::optional<ResponsesDelta>> received(expect);
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

    std::vector<ResponsesDelta> out;
    for (std::size_t index = 0; index < expect; ++index) {
        if (errors[index]) std::rethrow_exception(*errors[index]);
        out.push_back(std::move(*received[index]));
    }
    return out;
}

// One SSE frame carrying the given event JSON.
static std::string sse(const nlohmann::json& event) {
    return "event: " + event.at("type").get<std::string>() + "\ndata: " +
           event.dump() + "\n\n";
}

static std::string sse_raw(const std::string& data) {
    return "data: " + data + "\n\n";
}

// The canonical happy-path text stream: 10 frames, 10 deltas.
static std::string text_stream() {
    nlohmann::json done_part =
        nlohmann::json{{"type", "output_text"}, {"text", "The answer is 42"},
                       {"annotations", nlohmann::json::array()}};
    const std::string text = "The answer is 42";
    nlohmann::json done_item =
        nlohmann::json{{"id", "msg_1"},
                       {"type", "message"},
                       {"role", "assistant"},
                       {"status", "completed"},
                       {"content", nlohmann::json::array({done_part})}};
    return sse(nlohmann::json{{"type", "response.created"},
                              {"response", {{"id", "resp_1"}}}}) +
           sse(nlohmann::json{{"type", "response.output_item.added"},
                              {"output_index", 0},
                              {"item", {{"id", "msg_1"},
                                        {"type", "message"},
                                        {"role", "assistant"},
                                        {"content", nlohmann::json::array()}}}}) +
           sse(nlohmann::json{{"type", "response.content_part.added"},
                              {"item_id", "msg_1"},
                              {"output_index", 0},
                              {"content_index", 0},
                              {"part", {{"type", "output_text"}, {"text", ""}}}}) +
           sse(nlohmann::json{{"type", "response.output_text.delta"},
                              {"item_id", "msg_1"},
                              {"output_index", 0},
                              {"content_index", 0},
                              {"delta", "The"}}) +
           sse(nlohmann::json{{"type", "response.output_text.delta"},
                              {"item_id", "msg_1"},
                              {"output_index", 0},
                              {"content_index", 0},
                              {"delta", " answer"}}) +
           sse(nlohmann::json{{"type", "response.output_text.delta"},
                              {"item_id", "msg_1"},
                              {"output_index", 0},
                              {"content_index", 0},
                              {"delta", " is 42"}}) +
           sse(nlohmann::json{{"type", "response.output_text.done"},
                              {"item_id", "msg_1"},
                              {"output_index", 0},
                              {"content_index", 0},
                              {"text", text}}) +
           sse(nlohmann::json{{"type", "response.content_part.done"},
                              {"item_id", "msg_1"},
                              {"output_index", 0},
                              {"content_index", 0},
                              {"part", done_part}}) +
           sse(nlohmann::json{{"type", "response.output_item.done"},
                              {"output_index", 0},
                              {"item", done_item}}) +
           sse(nlohmann::json{{"type", "response.completed"},
                              {"response",
                               {{"id", "resp_1"},
                                {"usage", {{"input_tokens", 10}, {"output_tokens", 5}}}}}});
}

// --- text stream ---------------------------------------------------------------

BOOST_AUTO_TEST_CASE(text_stream_decodes_one_delta_per_event) {
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());

    auto deltas = exchange(io, handler, text_stream(), 10);
    BOOST_REQUIRE_EQUAL(deltas.size(), 10u);

    // Lifecycle frames are Markers; frames 3-5 are the Text increments.
    for (std::size_t index = 0; index < deltas.size(); ++index) {
        const bool is_text = (index >= 3 && index < 6);
        BOOST_CHECK_MESSAGE(
            deltas[index].kind == (is_text ? DeltaKind::Text : DeltaKind::Marker),
            "index " << index << " kind " << deltas[index].kind);
    }
    std::string streamed;
    for (std::size_t index = 3; index < 6; ++index) {
        BOOST_CHECK_EQUAL(deltas[index].item_id, "msg_1");
        BOOST_CHECK_EQUAL(deltas[index].content_index.value_or(999),
                          std::size_t{0});
        streamed += deltas[index].text;
    }
    BOOST_CHECK_EQUAL(streamed, "The answer is 42");

    // output_text.done is a Marker with the authoritative text — NOT a
    // second Text delta (consumers would print everything twice) — and it
    // carries the full event for the reader's overwrite.
    BOOST_CHECK(deltas[6].kind == DeltaKind::Marker);
    BOOST_CHECK_EQUAL(deltas[6].text, "response.output_text.done");
    BOOST_REQUIRE(deltas[6].extras);
    BOOST_CHECK_EQUAL((*deltas[6].extras)["text"], "The answer is 42");

    // Terminal bookkeeping: only the response.completed marker is terminal.
    BOOST_CHECK(is_terminal(deltas[9]));
    BOOST_CHECK(!is_terminal(deltas[0]));
    BOOST_CHECK(!is_terminal(deltas[3]));
}

// --- function calling ----------------------------------------------------------

BOOST_AUTO_TEST_CASE(function_call_stream_decodes_channels) {
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());

    const std::string chunk =
        sse(nlohmann::json{{"type", "response.created"},
                           {"response", {{"id", "resp_2"}}}}) +
        sse(nlohmann::json{{"type", "response.output_item.added"},
                           {"output_index", 0},
                           {"item", {{"id", "rs_1"},
                                     {"type", "reasoning"},
                                     {"summary", nlohmann::json::array()}}}}) +
        sse(nlohmann::json{{"type", "response.reasoning_text.delta"},
                           {"item_id", "rs_1"},
                           {"delta", "Hmm"}}) +
        sse(nlohmann::json{{"type", "response.reasoning_text.delta"},
                           {"item_id", "rs_1"},
                           {"delta", " ..."}}) +
        sse(nlohmann::json{
            {"type", "response.reasoning_text.done"},
            {"item_id", "rs_1"},
            {"text", "Hmm ... let me check the weather."}}) +
        sse(nlohmann::json{{"type", "response.output_item.done"},
                           {"output_index", 0},
                           {"item", {{"id", "rs_1"},
                                     {"type", "reasoning"},
                                     {"summary", nlohmann::json::array()},
                                     {"encrypted_content", "ENC"}}}}) +
        sse(nlohmann::json{
            {"type", "response.output_item.added"},
            {"output_index", 1},
            {"item", {{"id", "fc_1"},
                      {"type", "function_call"},
                      {"call_id", "call_1"},
                      {"name", "get_weather"},
                      {"arguments", ""}}}}) +
        sse(nlohmann::json{{"type", "response.function_call_arguments.delta"},
                           {"item_id", "fc_1"},
                           {"delta", "{\"city\":"}}) +
        sse(nlohmann::json{{"type", "response.function_call_arguments.delta"},
                           {"item_id", "fc_1"},
                           {"delta", "\"London\"}"}}) +
        sse(nlohmann::json{{"type", "response.function_call_arguments.done"},
                           {"item_id", "fc_1"},
                           {"name", "get_weather"},
                           {"arguments", "{\"city\":\"London\"}"}}) +
        sse(nlohmann::json{{"type", "response.output_item.done"},
                           {"output_index", 1},
                           {"item", {{"id", "fc_1"},
                                     {"type", "function_call"},
                                     {"call_id", "call_1"},
                                     {"name", "get_weather"},
                                     {"arguments", "{\"city\":\"London\"}"},
                                     {"status", "completed"}}}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "resp_2"}}}});

    auto deltas = exchange(io, handler, chunk, 12);
    BOOST_REQUIRE_EQUAL(deltas.size(), 12u);

    // Reasoning channel deltas are ReasoningText; done is a Marker.
    BOOST_CHECK(deltas[2].kind == DeltaKind::ReasoningText);
    BOOST_CHECK(deltas[3].kind == DeltaKind::ReasoningText);
    BOOST_CHECK(deltas[4].kind == DeltaKind::Marker);
    // Arguments stream as ToolCallArgs fragments; done is a Marker.
    BOOST_CHECK(deltas[7].kind == DeltaKind::ToolCallArgs);
    BOOST_CHECK_EQUAL(deltas[7].text, "{\"city\":");
    BOOST_CHECK(deltas[8].kind == DeltaKind::ToolCallArgs);
    BOOST_CHECK(deltas[9].kind == DeltaKind::Marker);
    // The terminal marker.
    BOOST_CHECK(is_terminal(deltas[11]));
}

// The summary text channel spells its part index `summary_index`; the
// decoder resolves it into the delta's content_index so the delta alone
// identifies its part (the reader never re-parses the wire spelling).
BOOST_AUTO_TEST_CASE(summary_deltas_carry_their_resolved_part_index) {
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());

    auto deltas = exchange(
        io, handler,
        sse(nlohmann::json{{"type", "response.reasoning_summary_text.delta"},
                           {"item_id", "rs_s"},
                           {"summary_index", 1},
                           {"delta", "part one"}}) +
            sse(nlohmann::json{{"type", "response.reasoning_summary_text.delta"},
                               {"item_id", "rs_s"},
                               {"content_index", 0},
                               {"delta", "part zero"}}),
        2);
    BOOST_REQUIRE_EQUAL(deltas.size(), 2u);
    BOOST_CHECK(deltas[0].kind == DeltaKind::ReasoningSummary);
    BOOST_REQUIRE(deltas[0].content_index);
    BOOST_CHECK_EQUAL(*deltas[0].content_index, std::size_t{1});
    BOOST_CHECK(deltas[1].kind == DeltaKind::ReasoningSummary);
    BOOST_REQUIRE(deltas[1].content_index);
    BOOST_CHECK_EQUAL(*deltas[1].content_index, std::size_t{0});
}

// A comment-only frame (": keep-alive\n\n") between events is NOT an event:
// before the framing fix it leaked into the handler as a bogus Marker delta.
BOOST_AUTO_TEST_CASE(comment_frames_produce_no_deltas) {
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());
    const std::string chunk =
        sse(nlohmann::json{{"type", "response.created"},
                           {"response", {{"id", "r"}}}}) +
        ": keep-alive\n\n" +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});
    auto deltas = exchange(io, handler, chunk, 2);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Marker);
    BOOST_CHECK_EQUAL(deltas[0].text, "response.created");
    BOOST_CHECK(deltas[1].kind == DeltaKind::Marker);
    BOOST_CHECK_EQUAL(deltas[1].text, "response.completed");
}

// --- explicit placeholders --------------------------------------------------------

BOOST_AUTO_TEST_CASE(unhandled_categories_surface_as_ignored_deltas) {
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());

    const std::string chunk =
        sse(nlohmann::json{{"type", "response.created"},
                           {"response", {{"id", "r"}}}}) +
        sse(nlohmann::json{{"type", "response.web_search_call.in_progress"},
                           {"item_id", "ws_1"},
                           {"output_index", 0}}) +
        sse(nlohmann::json{{"type", "response.web_search_call.completed"},
                           {"item_id", "ws_1"},
                           {"output_index", 0}}) +
        sse(nlohmann::json{{"type", "response.mcp_list_tools.completed"},
                           {"item_id", "mcp_1"},
                           {"output_index", 1}}) +
        sse(nlohmann::json{{"type", "response.output_text.annotation.added"},
                           {"item_id", "msg_1"},
                           {"annotation_index", 0}}) +
        sse(nlohmann::json{{"type", "response.output_item.added"},
                           {"output_index", 2},
                           {"item", {{"id", "fs_1"},
                                     {"type", "file_search_call"}}}}) +
        sse(nlohmann::json{{"type", "response.novel_event"},
                           {"item_id", "x"}}) +
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}});

    auto deltas = exchange(io, handler, chunk, 8);
    // Every category names itself; unknown types are prefixed.
    BOOST_CHECK(deltas[1].kind == DeltaKind::Ignored);
    BOOST_CHECK_EQUAL(deltas[1].text, "response.web_search_call.in_progress");
    BOOST_CHECK(deltas[2].kind == DeltaKind::Ignored);
    BOOST_CHECK(deltas[3].kind == DeltaKind::Ignored);
    BOOST_CHECK(deltas[4].kind == DeltaKind::Ignored);
    BOOST_CHECK(deltas[5].kind == DeltaKind::Ignored);
    BOOST_CHECK_EQUAL(deltas[5].text, "response.output_item.added");
    BOOST_CHECK(deltas[6].kind == DeltaKind::Ignored);
    BOOST_CHECK_EQUAL(deltas[6].text, "unknown:response.novel_event");
    // The stream itself stays alive around placeholders.
    BOOST_CHECK(handler.get_state() == endpoint::SSEHandlerState::RUNNING);
    BOOST_CHECK(deltas[7].kind == DeltaKind::Marker);
}

// --- leniency --------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(malformed_frames_are_ignored_without_killing_the_stream) {
    asio::io_context io;
    ResponsesStreamHandler handler(io.get_executor());

    // Garbage data, a type-less object, [DONE], and an event:-only frame all
    // surface as deltas — and the handler stays RUNNING throughout.
    std::vector<ResponsesDelta> deltas = exchange(
        io, handler, "data: not json at all\n\n", 1);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Ignored);
    BOOST_CHECK_EQUAL(deltas[0].text, "unparsable-data");
    BOOST_CHECK(handler.get_state() == endpoint::SSEHandlerState::RUNNING);

    deltas = exchange(io, handler, sse_raw("{\"foo\":1}"), 1);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Ignored);
    BOOST_CHECK_EQUAL(deltas[0].text, "missing-type");

    deltas = exchange(io, handler, sse_raw("[DONE]"), 1);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Ignored);
    BOOST_CHECK_EQUAL(deltas[0].text, "[DONE]");

    deltas = exchange(io, handler, "event: response.created\n\n", 1);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Marker);
    BOOST_CHECK_EQUAL(deltas[0].text, "response.created");

    // Multi-line data frames join per the SSE spec before parsing.
    deltas = exchange(
        io, handler,
        "data: {\"type\": \"response.output_text.delta\",\ndata: "
        "\"item_id\": \"m\", \"delta\": \"hi\"}\n\n",
        1);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Text);
    BOOST_CHECK_EQUAL(deltas[0].text, "hi");

    // And a real event still decodes afterwards — the stream never died.
    deltas = exchange(
        io, handler,
        sse(nlohmann::json{{"type", "response.completed"},
                           {"response", {{"id", "r"}}}}),
        1);
    BOOST_CHECK(deltas[0].kind == DeltaKind::Marker);
    BOOST_CHECK(is_terminal(deltas[0]));
}
