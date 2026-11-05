#define BOOST_TEST_MODULE PtcTools
#include <boost/test/unit_test.hpp>
#include "tools/intrinsic/ptc/toolset.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cstdlib>
#include <fstream>
#include <boost/asio/steady_timer.hpp>

using namespace tools::intrinsic;
using Json = nlohmann::json;

namespace {
/** Run an operator-only CLI command independently of the runtime under test.
 * Always join the private process store, including assertion/transport failures. */
boost::asio::awaitable<Json> control_cli(const PtcOptions& options,
    std::vector<std::string> arguments)
{
    namespace asio = boost::asio;
    ProcessSessionStore clients(co_await asio::this_coro::executor);
    std::exception_ptr error;
    Json response;
    try {
        process::LaunchSpec spec;
        spec.executable = options.executable;
        spec.arguments = options.arguments;
        spec.arguments.insert(spec.arguments.end(), arguments.begin(), arguments.end());
        const auto child = co_await clients.spawn(std::move(spec));
        co_await clients.send_input(child.id, "", true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            const auto state = co_await clients.snapshot(child.id);
            BOOST_REQUIRE(state.has_value());
            if (state->exited && state->output_drained) {
                BOOST_REQUIRE(state->result.execution.exit_code.has_value());
                BOOST_REQUIRE(*state->result.execution.exit_code == 0);
                break;
            }
            BOOST_REQUIRE((std::chrono::steady_clock::now() < deadline));
            asio::steady_timer tick(co_await asio::this_coro::executor, std::chrono::milliseconds(10));
            co_await tick.async_wait(asio::use_awaitable);
        }
        const auto output = co_await clients.read_output(child.id, OutputStream::Both, true, false);
        BOOST_REQUIRE(output.has_value());
        response = Json::parse(output->standard_output.text);
    } catch (...) {
        error = std::current_exception();
    }
    co_await clients.shutdown();
    if (error) std::rethrow_exception(error);
    co_return response;
}
} // namespace

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

    // A recovered receipt is an ordinary failed execution and must carry the
    // same result fields; without them the adapter reports an unknown commit
    // instead of a definite "interrupted" outcome.
    const auto recovered = Json::parse(R"({"protocol":1,"execution_id":"e","session_id":"s",
        "epoch":"p","revision":1,"status":"failed","committed":false,"result":null,
        "output":"","output_omitted_bytes":0,
        "error":{"code":"interrupted","message":"external effects are unknown; source was not replayed"}})");
    const auto rendered = render_ptc_response(recovered, 1, "e", "p");
    BOOST_TEST(rendered.raw.find("interrupted") != std::string::npos);
    BOOST_TEST(rendered.raw.find("[[committed]]: false") != std::string::npos);
    BOOST_REQUIRE(rendered.extras);
    BOOST_TEST(rendered.extras->at("simplex.ptc").at("committed") == false);
    for (const auto* field : {"result", "output", "output_omitted_bytes"}) {
        auto missing = recovered;
        missing.erase(field);
        BOOST_CHECK_THROW(render_ptc_response(missing, 1, "e", "p"), std::exception);
    }
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

            // Simulate an operator reset while the worker is idle. The next
            // source must be refused exactly once, not evaluated or replayed.
            const auto epoch = restored.extras->at("simplex.ptc").at("epoch").get<std::string>();
            std::vector<std::string> reset_args{"session", "reset", "--state-directory",
                directory.string(), "--epoch", epoch};
            const auto reset = co_await control_cli(options, std::move(reset_args));
            BOOST_TEST(reset.at("epoch") != epoch);
            const auto forbidden = directory / "must-not-run";
            const auto shell_command = "touch '" + forbidden.string() + "'";
            const auto refusal = co_await runtime->execute(
                "run_command(" + Json(shell_command).dump() + ").id");
            BOOST_TEST(refusal.raw.find("[[submitted]]: false") != std::string::npos);
            BOOST_TEST(refusal.raw.find("stale_epoch") != std::string::npos);
            BOOST_TEST(!std::filesystem::exists(forbidden));
            const auto resumed = co_await runtime->execute("41 + 1");
            BOOST_TEST(resumed.raw.find("42") != std::string::npos);
            BOOST_TEST(resumed.extras->at("simplex.ptc").at("committed") == true);
            BOOST_TEST(!std::filesystem::exists(forbidden));
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

BOOST_AUTO_TEST_CASE(rejection_envelope_requires_consistent_non_submission)
{
    const auto rejection = Json::parse(R"({"protocol":1,"status":"failed","commit_status":"none",
        "submitted":false,"commands_created":[],
        "error":{"code":"stale_epoch","message":"supply the current session epoch"}})");
    const auto rendered = render_ptc_response(rejection, 2, "e", "old");
    BOOST_TEST(rendered.raw.find("[[submitted]]: false") != std::string::npos);
    BOOST_TEST(rendered.raw.find("External effects may remain") == std::string::npos);
    BOOST_TEST(rendered.extras->at("simplex.ptc").at("commit_status") == "none");
    BOOST_CHECK_THROW(render_ptc_response(rejection, 1, "e", "old"), std::exception);
    for (const char* key : {"submitted", "commands_created"}) {
        auto missing = rejection;
        missing.erase(key);
        BOOST_CHECK_THROW(render_ptc_response(missing, 2, "e", "old"), std::exception);
    }
    for (const Json value : {Json(true), Json(0), Json(nullptr)}) {
        auto wrong = rejection;
        wrong["submitted"] = value;
        BOOST_CHECK_THROW(render_ptc_response(wrong, 2, "e", "old"), std::exception);
    }
    auto wrong = rejection;
    wrong["commands_created"] = Json::array({"command"});
    BOOST_CHECK_THROW(render_ptc_response(wrong, 2, "e", "old"), std::exception);
    wrong = rejection;
    wrong["committed"] = false;
    BOOST_CHECK_THROW(render_ptc_response(wrong, 2, "e", "old"), std::exception);
    wrong = rejection;
    wrong["commit_status"] = "unknown";
    BOOST_CHECK_THROW(render_ptc_response(wrong, 2, "e", "old"), std::exception);
}

BOOST_AUTO_TEST_CASE(rejection_refreshes_metadata_without_replaying_source)
{
    namespace asio = boost::asio;
    for (const bool refresh_fails : {false, true}) {
        const auto root = std::filesystem::temp_directory_path() /
            ("simplex-ptc-rejection-" + boost::uuids::to_string(boost::uuids::random_generator()()));
        std::filesystem::create_directories(root);
        const auto script = root / "interpreter.sh";
        std::ofstream(script) << R"sh(
cd "$(dirname "$0")" || exit 2
case "$1" in
capabilities) printf '%s' '{"protocol":1,"tools":[]}' ;;
daemon) printf '%s' '{"running":false,"stopped":true}' ;;
session)
    echo inspect >> inspections
    if [ -f executed ]; then
)sh" << (refresh_fails
            ? "printf '%s' '{\"error\":{\"message\":\"inspection failed\"}}'; exit 2\n"
            : "printf '%s' '{\"epoch\":\"new\",\"recovery_required\":false}'\n") << R"sh(
    else
        printf '%s' '{"epoch":"old","recovery_required":false}'
    fi ;;
execute)
    request=$(cat)
    echo execute >> executions
    if [ ! -f executed ]; then
        touch executed
        printf '%s' '{"protocol":1,"status":"failed","commit_status":"none","submitted":false,"commands_created":[],"error":{"code":"stale_epoch","message":"not admitted"}}'
        exit 2
    fi
    case "$request" in *'"epoch":"new"'*) ;; *) exit 9 ;; esac
    id=$(printf '%s' "$request" | sed -n 's/.*"execution_id":"\([^"]*\)".*/\1/p')
    printf '{"protocol":1,"execution_id":"%s","session_id":"s","epoch":"new","revision":1,"status":"completed","committed":true,"output":"","output_omitted_bytes":0,"result":42}' "$id"
    ;;
esac
)sh";
        asio::io_context io;
        PtcOptions options{"/bin/sh", {script.string()}, root / "state", {}};
        auto runtime = std::make_shared<PtcRuntime>(io.get_executor(), options,
            [] { return std::stop_token{}; });
        auto task = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            std::exception_ptr error;
            try {
                co_await runtime->initialize();
                const auto first = co_await runtime->execute("print(1)");
                BOOST_TEST(first.raw.find("[[submitted]]: false") != std::string::npos);
                BOOST_TEST(first.extras->at("simplex.ptc").at("commit_status") == "none");
                const auto second = co_await runtime->execute("print(2)");
                BOOST_TEST(second.raw.find(refresh_fails ? "not initialized" : "42") != std::string::npos);
            } catch (...) {
                error = std::current_exception();
            }
            co_await runtime->shutdown();
            if (error) std::rethrow_exception(error);
        }, asio::use_future);
        io.run();
        task.get();
        for (const auto& [name, expected] : std::vector<std::pair<std::string, int>>{
                 {"inspections", 2}, {"executions", refresh_fails ? 1 : 2}}) {
            std::ifstream input(root / name);
            std::string line;
            int count = 0;
            while (std::getline(input, line)) ++count;
            BOOST_TEST(count == expected);
        }
        std::filesystem::remove_all(root);
    }
}
