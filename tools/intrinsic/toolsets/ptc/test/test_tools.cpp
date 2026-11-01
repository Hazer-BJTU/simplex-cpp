#define BOOST_TEST_MODULE PtcTools
#include <boost/test/unit_test.hpp>
#include "tools/intrinsic/ptc/toolset.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cstdlib>
#include <fstream>
#include <boost/asio/steady_timer.hpp>

using namespace tools::intrinsic;
using Json = nlohmann::json;
BOOST_AUTO_TEST_CASE(envelope)
{
    auto value = Json::parse(R"({"protocol":1,"execution_id":"e","session_id":"s","epoch":"p","revision":1,"status":"completed","committed":true,"output":"hello","output_omitted_bytes":0,"result":false})");
    BOOST_TEST(render_ptc_response(value, 0, "e", "p").raw.find("false") != std::string::npos);
    BOOST_CHECK_THROW(render_ptc_response(value, 1, "e", "p"), std::exception);
    BOOST_CHECK_THROW(render_ptc_response(value, 0, "other", "p"), std::exception);
    value["result"] = nullptr;
    value["result_truncated"] = true;
    value["result_preview"] = "[1,2...";
    BOOST_TEST(render_ptc_response(value, 0, "e", "p").raw.find("result_preview") != std::string::npos);
}

// Opt-in real CLI/daemon integration. The dedicated CI job supplies this path;
// ordinary native unit tests do not require Python or Monty installation.
BOOST_AUTO_TEST_CASE(real_interpreter)
{
    const auto executable = std::getenv("SIMPLEX_TEST_PTC");
    if (!executable) { BOOST_TEST_MESSAGE("real PTC integration not selected"); return; }
    namespace asio = boost::asio;
    auto directory = std::filesystem::temp_directory_path() /
        ("simplex-ptc-integration-" + boost::uuids::to_string(boost::uuids::random_generator()()));
    asio::io_context io;
    PtcOptions options{executable, {"--module", "simplex_ptc.tools.process", "--module", "simplex_ptc.tools.files"}, directory, {}};
    std::stop_source stop;
    auto runtime = std::make_shared<PtcRuntime>(io.get_executor(), options, [&] { return stop.get_token(); });
    auto task = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        std::exception_ptr error;
        try {
            co_await runtime->initialize();
            auto first = co_await runtime->execute("x = 40\nprint('hello')\nx + 2");
            BOOST_TEST(first.raw.find("42") != std::string::npos);
            auto next = co_await runtime->execute("x + 3");
            BOOST_TEST(next.raw.find("43") != std::string::npos);
            auto command = co_await runtime->execute("c = run_command('printf abc')\ncid = c.id\nc.read().stdout");
            BOOST_TEST(command.raw.find("abc") != std::string::npos);
            auto again = co_await runtime->execute("c = get_command(cid)\nc.release().released");
            BOOST_TEST(again.raw.find("true") != std::string::npos);
            auto large = co_await runtime->execute("'x' * 100000");
            BOOST_TEST(large.raw.find("result_preview") != std::string::npos);
            auto printed = co_await runtime->execute("print('中' * 20000)\n7");
            BOOST_TEST(printed.raw.find("output_omitted_bytes") != std::string::npos);
            const auto file = directory / "edit.txt";
            std::ofstream(file) << "before\n";
            const std::string edit_source = "f = open_text(" + Json(file.string()).dump()
                + ", writable=True)\nf.replace('before', 'after')\nf.commit()\n1 / 0";
            auto edit = co_await runtime->execute(edit_source);
            BOOST_TEST(edit.raw.find("failed") != std::string::npos);
            std::string text;
            std::ifstream(file) >> text;
            BOOST_TEST(text == "after");
            auto failed = co_await runtime->execute("x = 100\n1 / 0");
            BOOST_TEST(failed.raw.find("failed") != std::string::npos);
            auto retained = co_await runtime->execute("x");
            BOOST_TEST(retained.raw.find("40") != std::string::npos);
            const auto marker = directory / "command-started";
            bool cancellation_watcher_done = false;
            auto watch = [&]() -> asio::awaitable<void> {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < deadline) {
                    asio::steady_timer tick(io, std::chrono::milliseconds(20));
                    co_await tick.async_wait(asio::use_awaitable);
                }
                BOOST_TEST(std::filesystem::exists(marker));
                stop.request_stop();
            };
            asio::co_spawn(io, watch, [&](std::exception_ptr error) {
                BOOST_TEST(!error);
                cancellation_watcher_done = true;
            });
            const std::string shell = "touch '" + marker.string() + "'; sleep 30";
            const std::string source = "run_command(" + Json(shell).dump() + ", timeout_seconds=30)\n123";
            auto cancelled = co_await runtime->execute(source);
            while (!cancellation_watcher_done) {
                asio::steady_timer tick(io, std::chrono::milliseconds(20));
                co_await tick.async_wait(asio::use_awaitable);
            }
            BOOST_TEST(cancelled.raw.find("failed") != std::string::npos);
            BOOST_TEST(cancelled.raw.find("commands_created") != std::string::npos);
            co_await runtime->shutdown();
            stop = std::stop_source{};
            runtime = std::make_shared<PtcRuntime>(io.get_executor(), options,
                [&] { return stop.get_token(); });
            co_await runtime->initialize();
            auto restored = co_await runtime->execute("x");
            BOOST_TEST(restored.raw.find("40") != std::string::npos);
            auto isolated_options = options;
            isolated_options.directory = directory / "separate-session";
            auto isolated = std::make_shared<PtcRuntime>(io.get_executor(), isolated_options,
                [] { return std::stop_token{}; });
            std::exception_ptr isolated_failure;
            try {
                co_await isolated->initialize();
                auto absent = co_await isolated->execute("x");
                BOOST_TEST(absent.raw.find("failed") != std::string::npos);
            } catch (...) { isolated_failure = std::current_exception(); }
            co_await isolated->shutdown();
            if (isolated_failure) std::rethrow_exception(isolated_failure);
        } catch (...) { error = std::current_exception(); }
        co_await runtime->shutdown();
        if (error) std::rethrow_exception(error);
    }, asio::use_future);
    io.run();
    task.get();
    std::filesystem::remove_all(directory);
}

BOOST_AUTO_TEST_CASE(malformed_transport_blocks_replay)
{
    namespace asio = boost::asio;
    const auto root = std::filesystem::temp_directory_path() /
        ("simplex-ptc-fixture-" + boost::uuids::to_string(boost::uuids::random_generator()()));
    std::filesystem::create_directories(root);
    const auto script = root / "interpreter.sh";
    const auto count = root / "executions";
    std::ofstream(script) <<
        "case \"$1\" in\n"
        "capabilities) printf '%s' '{\"protocol\":1,\"tools\":[]}' ;;\n"
        "session) printf '%s' '{\"epoch\":null,\"recovery_required\":false}' ;;\n"
        "execute) cat >/dev/null; echo called >> '" + count.string() + "'; printf not-json ;;\n"
        "daemon) printf '%s' '{\"stopped\":true}' ;;\n"
        "esac\n";
    asio::io_context io;
    PtcOptions options{"/bin/sh", {script.string()}, root / "state", {}};
    auto runtime = std::make_shared<PtcRuntime>(io.get_executor(), options, [] { return std::stop_token{}; });
    auto task = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await runtime->initialize();
        auto first = co_await runtime->execute("print(1)");
        auto second = co_await runtime->execute("print(2)");
        BOOST_TEST(first.raw.find("unknown") != std::string::npos);
        BOOST_TEST(second.raw.find("not initialized") != std::string::npos);
        co_await runtime->shutdown();
    }, asio::use_future);
    io.run();
    task.get();
    std::ifstream input(count);
    std::string first, second;
    std::getline(input, first);
    std::getline(input, second);
    BOOST_TEST(first == "called");
    BOOST_TEST(second.empty());
    std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(error_envelope_keeps_effects_and_rejects_contradictions)
{
    auto value = Json::parse(R"({"protocol":1,"status":"failed","commit_status":"unknown","execution_id":"e","commands_created":["command"],"error":{"code":"io","message":"inspect"}})");
    auto result = render_ptc_response(value, 2, "e", "epoch");
    BOOST_TEST(result.raw.find("command") != std::string::npos);
    BOOST_REQUIRE(result.extras);
    BOOST_TEST(result.extras->at("simplex.ptc").at("execution_id") == "e");
    BOOST_CHECK_THROW(render_ptc_response(value, 0, "e", "epoch"), std::exception);
    value["committed"] = true;
    BOOST_CHECK_THROW(render_ptc_response(value, 2, "e", "epoch"), std::exception);
    value.erase("committed");
    value["protocol"] = true;
    BOOST_CHECK_THROW(render_ptc_response(value, 2, "e", "epoch"), std::exception);
}
