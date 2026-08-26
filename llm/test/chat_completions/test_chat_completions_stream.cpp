#define BOOST_TEST_MODULE chat_completions_stream
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "llm/chat_completions/model.hpp"
#include "llm/chat_completions/reader.hpp"

namespace asio = boost::asio;
using llm::chat_completions::ChatCompletionStatus;
using llm::chat_completions::ChatCompletionsDelta;
using llm::chat_completions::ChatCompletionsReader;
using llm::chat_completions::ChatCompletionsStreamHandler;

namespace {

class WrappedChunkDialect final
    : public llm::chat_completions::ChatCompletionsDialect {
public:
    nlohmann::json normalize_chunk(nlohmann::json value) const override {
        return value.at("wrapped");
    }
};

class TestChatCompletionsModel final
    : public llm::chat_completions::ChatCompletionsModel {
public:
    TestChatCompletionsModel(boost::asio::any_io_executor executor,
                             nlohmann::json config)
        : ChatCompletionsModel(std::move(executor), std::move(config)) {}
};

std::string sse(const nlohmann::json& chunk) {
    return "data: " + chunk.dump() + "\n\n";
}

std::string done() { return "data: [DONE]\n\n"; }

void pump(asio::io_context& io) {
    io.poll();
    io.restart();
}

std::vector<ChatCompletionsDelta> decode(
    asio::io_context& io,
    ChatCompletionsStreamHandler& handler,
    std::string_view wire,
    std::size_t count) {
    std::vector<std::optional<ChatCompletionsDelta>> received(count);
    for (std::size_t index = 0; index < count; ++index) {
        asio::co_spawn(
            io,
            [&, index]() -> asio::awaitable<void> {
                received[index] = co_await handler.get();
            },
            asio::detached);
    }
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> { co_await handler.put(wire); },
        asio::detached);
    pump(io);

    std::vector<ChatCompletionsDelta> result;
    for (auto& item : received) result.push_back(std::move(*item));
    return result;
}

std::vector<ChatCompletionsDelta> read_all(
    asio::io_context& io,
    ChatCompletionsReader& reader,
    std::string_view wire) {
    std::vector<ChatCompletionsDelta> result;
    std::exception_ptr error;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                while (auto delta = co_await reader.next()) {
                    result.push_back(std::move(*delta));
                }
            } catch (...) {
                error = std::current_exception();
            }
        },
        asio::detached);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> { co_await reader.handler()->put(wire); },
        asio::detached);
    io.run();
    io.restart();
    if (error) std::rethrow_exception(error);
    return result;
}

nlohmann::json chunk(nlohmann::json delta,
                     nlohmann::json finish_reason = nullptr) {
    return {
        {"id", "chatcmpl_1"},
        {"object", "chat.completion.chunk"},
        {"model", "chat-test"},
        {"choices", nlohmann::json::array({{
            {"index", 0},
            {"delta", std::move(delta)},
            {"finish_reason", std::move(finish_reason)},
        }})},
    };
}

std::string parallel_tool_stream() {
    return sse(chunk({{"role", "assistant"}})) +
           sse(chunk({{"tool_calls", nlohmann::json::array({
               {{"index", 0}, {"id", "call_"}, {"type", "function"},
                {"function", {{"name", "weather"},
                              {"arguments", "{\"city\":"}}}},
               {{"index", 1}, {"id", "call_"}, {"type", "function"},
                {"function", {{"name", "time"},
                              {"arguments", "{\"zone\":"}}}},
           })}})) +
           sse(chunk({{"tool_calls", nlohmann::json::array({
               {{"index", 0}, {"id", "a"},
                {"function", {{"arguments", "\"Paris\"}"}}}},
               {{"index", 1}, {"id", "b"},
                {"function", {{"arguments", "\"UTC\"}"}}}},
           })}})) +
           sse(chunk(nlohmann::json::object(), "tool_calls")) +
           sse({
               {"id", "chatcmpl_1"},
               {"object", "chat.completion.chunk"},
               {"model", "chat-test"},
               {"choices", nlohmann::json::array()},
               {"usage", {
                   {"prompt_tokens", 20},
                   {"completion_tokens", 7},
                   {"total_tokens", 27},
                   {"prompt_tokens_details", {{"cached_tokens", 5}}},
               }},
           }) +
           done();
}

} // namespace

BOOST_AUTO_TEST_CASE(decoder_preserves_parallel_call_fragments_and_done) {
    asio::io_context io;
    ChatCompletionsStreamHandler handler(io.get_executor());
    auto deltas = decode(io, handler, parallel_tool_stream(), 6);

    BOOST_REQUIRE_EQUAL(deltas.size(), 6u);
    BOOST_CHECK_EQUAL(deltas[0].role, "assistant");
    BOOST_REQUIRE_EQUAL(deltas[1].tool_calls.size(), 2u);
    BOOST_CHECK_EQUAL(deltas[1].tool_calls[0].index, 0u);
    BOOST_CHECK_EQUAL(deltas[1].tool_calls[0].id, "call_");
    BOOST_CHECK_EQUAL(deltas[1].tool_calls[1].name, "time");
    BOOST_CHECK_EQUAL(deltas[3].finish_reason, "tool_calls");
    BOOST_REQUIRE(deltas[4].usage);
    BOOST_CHECK_EQUAL((*deltas[4].usage)["total_tokens"], 27);
    BOOST_CHECK(deltas[5].done);
}

BOOST_AUTO_TEST_CASE(reader_assembles_parallel_calls_usage_and_metadata) {
    asio::io_context io;
    ChatCompletionsReader reader(io.get_executor());
    auto deltas = read_all(io, reader, parallel_tool_stream());

    BOOST_REQUIRE_EQUAL(deltas.size(), 6u);
    BOOST_CHECK(reader.finished());
    BOOST_CHECK(reader.status() == ChatCompletionStatus::Completed);
    const auto& response = reader.response();
    BOOST_CHECK(response.type == model_io::MessageItemType::ModelResponse);
    BOOST_CHECK_EQUAL(response.role, "assistant");
    BOOST_REQUIRE(response.invokes);
    BOOST_REQUIRE_EQUAL(response.invokes->size(), 2u);
    BOOST_CHECK_EQUAL((*response.invokes)[0].id, "call_a");
    BOOST_CHECK_EQUAL((*response.invokes)[0].name, "weather");
    BOOST_CHECK_EQUAL((*response.invokes)[0].arguments["city"], "Paris");
    BOOST_CHECK_EQUAL((*response.invokes)[1].id, "call_b");
    BOOST_CHECK_EQUAL((*response.invokes)[1].arguments["zone"], "UTC");
    BOOST_REQUIRE(response.cost);
    BOOST_CHECK_EQUAL(response.cost->prompt, 20u);
    BOOST_CHECK_EQUAL(response.cost->generated, 7u);
    BOOST_CHECK_EQUAL(response.cost->cache_hit, 5u);
    BOOST_REQUIRE(response.extras);
    BOOST_CHECK_EQUAL((*response.extras)["completion_id"], "chatcmpl_1");
    BOOST_CHECK_EQUAL((*response.extras)["finish_reason"], "tool_calls");
}

BOOST_AUTO_TEST_CASE(text_and_refusal_stream_into_one_model_response) {
    asio::io_context io;
    ChatCompletionsReader reader(io.get_executor());
    const std::string wire =
        sse(chunk({{"role", "assistant"}, {"content", "Hello "}})) +
        sse(chunk({{"content", "world"}, {"refusal", "blocked detail"}})) +
        sse(chunk(nlohmann::json::object(), "stop")) + done();
    read_all(io, reader, wire);

    BOOST_CHECK(reader.status() == ChatCompletionStatus::Completed);
    BOOST_REQUIRE_EQUAL(reader.response().content.size(), 1u);
    BOOST_CHECK_EQUAL(reader.response().content[0].raw, "Hello world");
    BOOST_REQUIRE(reader.response().content[0].extras);
    BOOST_CHECK_EQUAL(
        (*reader.response().content[0].extras)["refusal"], "blocked detail");
}

BOOST_AUTO_TEST_CASE(refusal_only_response_keeps_replay_metadata) {
    asio::io_context io;
    ChatCompletionsReader reader(io.get_executor());
    read_all(io, reader,
             sse(chunk({{"role", "assistant"}, {"refusal", "cannot comply"}})) +
                 sse(chunk(nlohmann::json::object(), "stop")) + done());

    BOOST_CHECK(reader.status() == ChatCompletionStatus::Completed);
    BOOST_REQUIRE_EQUAL(reader.response().content.size(), 1u);
    BOOST_CHECK_EQUAL(reader.response().content[0].raw, "cannot comply");
    BOOST_REQUIRE(reader.response().content[0].extras);
    BOOST_CHECK_EQUAL(
        (*reader.response().content[0].extras)["refusal"], "cannot comply");
}

BOOST_AUTO_TEST_CASE(done_without_finish_reason_is_not_successful) {
    asio::io_context io;
    ChatCompletionsReader reader(io.get_executor());
    read_all(io, reader, sse(chunk({{"content", "partial"}})) + done());

    BOOST_CHECK(reader.finished());
    BOOST_CHECK(reader.status() == ChatCompletionStatus::Failed);
    BOOST_CHECK_EQUAL(reader.response().content[0].raw, "partial");
}

BOOST_AUTO_TEST_CASE(length_and_content_filter_are_non_success_statuses) {
    asio::io_context io;
    ChatCompletionsReader length_reader(io.get_executor());
    read_all(io, length_reader,
             sse(chunk({{"content", "partial"}}, "length")) + done());
    BOOST_CHECK(length_reader.status() ==
                ChatCompletionStatus::LengthLimited);

    ChatCompletionsReader filter_reader(io.get_executor());
    read_all(io, filter_reader,
             sse(chunk(nlohmann::json::object(), "content_filter")) + done());
    BOOST_CHECK(filter_reader.status() ==
                ChatCompletionStatus::ContentFiltered);
}

BOOST_AUTO_TEST_CASE(error_chunk_is_terminal_and_retains_details) {
    asio::io_context io;
    ChatCompletionsReader reader(io.get_executor());
    const nlohmann::json error = {
        {"error", {{"message", "bad request"}, {"type", "invalid_request"}}},
    };
    auto deltas = read_all(io, reader, sse(error));
    BOOST_REQUIRE_EQUAL(deltas.size(), 1u);
    BOOST_CHECK(deltas[0].error);
    BOOST_CHECK(reader.status() == ChatCompletionStatus::Failed);
    BOOST_REQUIRE(reader.error_details());
    BOOST_CHECK_EQUAL((*reader.error_details())["message"], "bad request");
}

BOOST_AUTO_TEST_CASE(malformed_frame_is_observable_and_stream_survives) {
    asio::io_context io;
    ChatCompletionsStreamHandler handler(io.get_executor());
    auto deltas = decode(io, handler,
                         "data: not-json\n\n" +
                             sse(chunk({{"content", "ok"}})) + done(),
                         3);
    BOOST_CHECK(deltas[0].ignored);
    BOOST_CHECK_EQUAL(deltas[1].content, "ok");
    BOOST_CHECK(deltas[2].done);
}

BOOST_AUTO_TEST_CASE(generic_model_builds_without_registering_a_provider) {
    asio::io_context io;
    TestChatCompletionsModel model(
        io.get_executor(),
        {
            {"model", "compatible-model"},
            {"temperature", 0.2},
            {"provider", "host-only"},
            {"retry", {{"max_attempts", 0}}},
            {"endpoint", {{"base_url", "https://compatible.example.com"}}},
        });
    BOOST_REQUIRE(model.build());
    BOOST_CHECK_EQUAL(model.endpoint().request_path, "/v1/chat/completions");
    BOOST_CHECK_EQUAL(model.generation()["model"], "compatible-model");
    BOOST_CHECK_EQUAL(model.generation()["temperature"], 0.2);
    BOOST_CHECK(!model.generation().contains("provider"));
    BOOST_CHECK(!model.generation().contains("endpoint"));
    BOOST_CHECK(!model.generation().contains("retry"));

    TestChatCompletionsModel missing_model(
        io.get_executor(),
        {{"endpoint", {{"base_url", "https://compatible.example.com"}}}});
    BOOST_CHECK(!missing_model.build());
}

BOOST_AUTO_TEST_CASE(dialect_can_normalize_provider_chunk_envelopes) {
    asio::io_context io;
    ChatCompletionsStreamHandler handler(
        io.get_executor(), endpoint::DEFAULT_SSE_LINE_WINDOW,
        std::make_shared<WrappedChunkDialect>());
    auto deltas = decode(
        io, handler,
        sse({{"wrapped", chunk({{"content", "normalized"}})}}) + done(), 2);
    BOOST_CHECK_EQUAL(deltas[0].content, "normalized");
    BOOST_CHECK(deltas[1].done);
}

// --- the thinking-mode reasoning channel ---------------------------------------

BOOST_AUTO_TEST_CASE(reasoning_deltas_assemble_into_reasoning_not_content) {
    asio::io_context io;
    ChatCompletionsStreamHandler handler(io.get_executor());
    auto deltas = decode(io, handler,
                         sse(chunk({{"reasoning_content", "think "}})) +
                             sse(chunk({{"reasoning_content", "hard"}})) + done(),
                         3);
    BOOST_CHECK_EQUAL(deltas[0].reasoning, "think ");
    BOOST_CHECK_EQUAL(deltas[1].reasoning, "hard");
    BOOST_CHECK(deltas[2].done);

    ChatCompletionsReader reader(io.get_executor());
    read_all(io, reader,
             sse(chunk({{"role", "assistant"},
                        {"reasoning_content", "think "}})) +
                 sse(chunk({{"reasoning_content", "hard"}})) +
                 sse(chunk({{"content", "Answer"}}, "stop")) + done());

    BOOST_CHECK(reader.status() == ChatCompletionStatus::Completed);
    const auto& response = reader.response();
    BOOST_REQUIRE(response.reasoning);
    BOOST_CHECK_EQUAL(response.reasoning->raw, "think hard");
    BOOST_CHECK_EQUAL(response.content[0].raw, "Answer");
}

BOOST_AUTO_TEST_CASE(reasoning_tool_and_usage_assemble_together) {
    asio::io_context io;
    ChatCompletionsReader reader(io.get_executor());
    read_all(io, reader,
             sse(chunk({{"role", "assistant"},
                        {"reasoning_content", "call the tool"}})) +
                 sse(chunk({{"tool_calls", nlohmann::json::array({
                     {{"index", 0}, {"id", "call_x"}, {"type", "function"},
                      {"function", {{"name", "weather"},
                                    {"arguments", "{\"city\":\"Paris\"}"}}}},
                 })}})) +
                 sse(chunk(nlohmann::json::object(), "tool_calls")) +
                 sse({
                     {"id", "chatcmpl_r"},
                     {"object", "chat.completion.chunk"},
                     {"model", "chat-test"},
                     {"choices", nlohmann::json::array()},
                     {"usage", {
                         {"prompt_tokens", 12},
                         {"completion_tokens", 4},
                         {"total_tokens", 16},
                         {"prompt_tokens_details", {{"cached_tokens", 3}}},
                     }},
                 }) +
                 done());

    BOOST_CHECK(reader.status() == ChatCompletionStatus::Completed);
    const auto& response = reader.response();
    BOOST_REQUIRE(response.reasoning);
    BOOST_CHECK_EQUAL(response.reasoning->raw, "call the tool");
    BOOST_REQUIRE(response.invokes);
    BOOST_REQUIRE_EQUAL(response.invokes->size(), 1u);
    BOOST_CHECK_EQUAL((*response.invokes)[0].name, "weather");
    BOOST_CHECK_EQUAL((*response.invokes)[0].arguments["city"], "Paris");
    BOOST_REQUIRE(response.cost);
    BOOST_CHECK_EQUAL(response.cost->cache_hit, 3u);
    BOOST_REQUIRE(response.extras);
    BOOST_CHECK_EQUAL((*response.extras)["finish_reason"], "tool_calls");
}

BOOST_AUTO_TEST_CASE(clear_resets_the_reasoning_accumulator) {
    asio::io_context io;
    ChatCompletionsReader reader(io.get_executor());
    read_all(io, reader,
             sse(chunk({{"reasoning_content", "first"}})) +
                 sse(chunk({{"content", "answer"}}, "stop")) + done());
    BOOST_REQUIRE(reader.response().reasoning);

    reader.clear();
    read_all(io, reader,
             sse(chunk({{"content", "plain"}})) +
                 sse(chunk(nlohmann::json::object(), "stop")) + done());
    BOOST_CHECK(reader.status() == ChatCompletionStatus::Completed);
    BOOST_CHECK(!reader.response().reasoning);
    BOOST_CHECK_EQUAL(reader.response().content[0].raw, "plain");
}

// --- DeepSeek live wire shapes (captured 2026-08-27, api.deepseek.com) ---------
//
// The two streams below are the byte shapes a live deepseek-v4-flash
// exchange produced (ids trimmed, fields otherwise verbatim). They pin the
// deviations that matter to the decoder:
//   - `usage` is null on ordinary chunks and arrives ON the finish_reason
//     chunk, whose choices array is NON-empty — DeepSeek never sends the
//     separate empty-choices usage trailer this suite's synthetic fixtures
//     use (both shapes must decode);
//   - the first chunk's delta carries role plus an EMPTY content string;
//     reasoning deltas carry content: null alongside reasoning_content;
//   - a tool call's first fragment carries index/id/type/name and
//     arguments as an EMPTY STRING (not null); later fragments are bare
//     index+arguments pieces, sometimes one character long.

nlohmann::json live_frame(nlohmann::json delta,
                          nlohmann::json finish_reason = nullptr,
                          nlohmann::json usage = nullptr) {
    return {
        {"id", "bc1a4c5b-361c-4f22-998b-2ace33c6b5e3"},
        {"object", "chat.completion.chunk"},
        {"created", 1787766372},
        {"model", "deepseek-v4-flash"},
        {"system_fingerprint", "a26a7955944dc5c60445bff77fac9c8e"},
        {"choices", nlohmann::json::array({{
            {"index", 0},
            {"delta", std::move(delta)},
            {"logprobs", nullptr},
            {"finish_reason", std::move(finish_reason)},
        }})},
        {"usage", std::move(usage)},
    };
}

BOOST_AUTO_TEST_CASE(deepseek_live_minimal_stream_usage_rides_the_finish_chunk) {
    asio::io_context io;
    ChatCompletionsReader reader(io.get_executor());
    read_all(io, reader,
             sse(live_frame({{"role", "assistant"}, {"content", ""}})) +
                 sse(live_frame({{"content", "OK"}})) +
                 sse(live_frame({{"content", ""}}, "stop", {
                     {"prompt_tokens", 9},
                     {"completion_tokens", 1},
                     {"total_tokens", 10},
                     {"prompt_tokens_details", {{"cached_tokens", 0}}},
                     {"prompt_cache_hit_tokens", 0},
                     {"prompt_cache_miss_tokens", 9},
                 })) +
                 done());

    BOOST_CHECK(reader.status() == ChatCompletionStatus::Completed);
    const auto& response = reader.response();
    BOOST_CHECK_EQUAL(response.content[0].raw, "OK");
    // Cost assembled from a usage object that arrived on the finish chunk.
    BOOST_REQUIRE(response.cost);
    BOOST_CHECK_EQUAL(response.cost->prompt, 9u);
    BOOST_CHECK_EQUAL(response.cost->generated, 1u);
    BOOST_CHECK_EQUAL(response.cost->cache_hit, 0u);
    BOOST_REQUIRE(response.extras);
    BOOST_CHECK_EQUAL((*response.extras)["finish_reason"], "stop");
    BOOST_CHECK_EQUAL((*response.extras)["model"], "deepseek-v4-flash");
    // The native cache spelling survives verbatim in extras for consumers.
    BOOST_CHECK_EQUAL((*response.extras)["usage"]["prompt_cache_miss_tokens"], 9);
}

BOOST_AUTO_TEST_CASE(deepseek_live_reasoning_and_fragmented_tool_call_shape) {
    asio::io_context io;
    ChatCompletionsReader reader(io.get_executor());
    const auto call_fragment = [](nlohmann::json function) {
        return live_frame({{"tool_calls", nlohmann::json::array({
            {{"index", 0}, {"function", std::move(function)}},
        })}});
    };
    read_all(io, reader,
             // role frame: content null, reasoning_content empty string
             sse(live_frame({{"role", "assistant"},
                             {"content", nullptr},
                             {"reasoning_content", ""}})) +
                 // reasoning frames: content null alongside every increment
                 sse(live_frame({{"content", nullptr},
                                 {"reasoning_content", "The user wants 2 + 3. "}})) +
                 sse(live_frame({{"content", nullptr},
                                 {"reasoning_content", "Call the tool."}})) +
                 // first tool fragment: id/type/name + arguments EMPTY STRING
                 sse(live_frame({{"tool_calls", nlohmann::json::array({
                     {{"index", 0},
                      {"id", "call_00_yVK46xe3ZVlUsOHa3jzk2841"},
                      {"type", "function"},
                      {"function", {{"name", "calculate"}, {"arguments", ""}}}},
                 })}})) +
                 // then bare index + one-character argument pieces
                 sse(call_fragment({{"arguments", "{"}})) +
                 sse(call_fragment({{"arguments", "\""}})) +
                 sse(call_fragment({{"arguments", "a"}})) +
                 sse(call_fragment({{"arguments", "\": 2, \"b\": 3, \"operation\": \"add\"}"}})) +
                 // finish frame: content empty, reasoning null, usage attached
                 sse(live_frame({{"content", ""}, {"reasoning_content", nullptr}},
                                "tool_calls", {
                                    {"prompt_tokens", 423},
                                    {"completion_tokens", 100},
                                    {"total_tokens", 523},
                                    {"prompt_tokens_details", {{"cached_tokens", 384}}},
                                    {"completion_tokens_details",
                                     {{"reasoning_tokens", 25}}},
                                    {"prompt_cache_hit_tokens", 384},
                                    {"prompt_cache_miss_tokens", 39},
                                })) +
                 done());

    BOOST_CHECK(reader.status() == ChatCompletionStatus::Completed);
    const auto& response = reader.response();
    // The leading empty-string increment must not corrupt the assembly.
    BOOST_REQUIRE(response.reasoning);
    BOOST_CHECK_EQUAL(response.reasoning->raw,
                      "The user wants 2 + 3. Call the tool.");
    // Fragmented arguments reconstruct into parsed JSON.
    BOOST_REQUIRE(response.invokes);
    BOOST_REQUIRE_EQUAL(response.invokes->size(), 1u);
    BOOST_CHECK_EQUAL((*response.invokes)[0].id,
                      "call_00_yVK46xe3ZVlUsOHa3jzk2841");
    BOOST_CHECK_EQUAL((*response.invokes)[0].name, "calculate");
    BOOST_CHECK_EQUAL((*response.invokes)[0].arguments["a"], 2);
    BOOST_CHECK_EQUAL((*response.invokes)[0].arguments["operation"], "add");
    // Native cached_tokens spelling feeds cache_hit (no dialect needed).
    BOOST_REQUIRE(response.cost);
    BOOST_CHECK_EQUAL(response.cost->cache_hit, 384u);
    BOOST_REQUIRE(response.extras);
    BOOST_CHECK_EQUAL((*response.extras)["usage"]["completion_tokens_details"]
                                    ["reasoning_tokens"], 25);
}
