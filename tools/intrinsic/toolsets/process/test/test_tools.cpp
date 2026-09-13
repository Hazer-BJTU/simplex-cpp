#define BOOST_TEST_MODULE ProcessToolsTests
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/process/toolset.hpp"
#include "tools/intrinsic/process/tools.hpp"

#include "tools/invoke_exception.hpp"
#include "tools/registry.hpp"
#include "tools/security_check.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <nlohmann/json.hpp>

// Tests for the six process tools: what each one answers, what it refuses and
// at which checkpoint, and what it DECLARES — the type/security pair a batch
// scheduler and the security policy read off a settled call.
//
// Every case goes through the ToolSet's two phases (prepare, then execute)
// rather than calling a tool's hooks directly: that is how a host runs a call,
// and it is what makes the settling assertions meaningful — a
// write_attributes() that never ran could not pass them.
//
// The RequireConfirm tools need a confirmation, and it is answered on a bus
// the test OWNS: default_async_bus() is process-wide, so subscribing there
// would leak a handler into every other case and into any other suite sharing
// the process.

namespace asio = boost::asio;
using tools::ConfirmDecision;
using tools::InvokeConfirmEvent;
using tools::InvokeException;
using tools::intrinsic::ProcessSessionStore;
using tools::intrinsic::ProcessToolSet;
namespace tool_names = tools::intrinsic::tool_names;

namespace {

/// A call as the conversation carries it. type/security are deliberately set
/// to the OPPOSITE of what any tool declares (SerialWrite/DefaultDeny for the
/// readers, and so on is not needed — one wrong pair suffices), so an
/// assertion on the settled values cannot pass by accident.
model_io::InvokeQuery call_for(std::string name, nlohmann::json arguments = {},
                              std::string id = "call_1")
{
    model_io::InvokeQuery query;
    query.type = model_io::InvokeType::SerialWrite;
    query.security = model_io::InvokeSecurity::DefaultDeny;
    query.id = std::move(id);
    query.name = std::move(name);
    query.arguments = arguments.is_null() ? nlohmann::json::object()
                                          : std::move(arguments);
    return query;
}

/// The store, the set, and a context that runs continuously on its own thread
/// — the way a host drives them (see the store test for why the "run to
/// quiescence" pattern cannot work here: the await tasks are perpetual work).
///
/// The bus is the fixture's own, and a handler that approves everything is
/// subscribed for the whole fixture: what these cases test is the tools, not
/// the confirmation policy (tools/test/test_security_check.cpp owns that), so
/// the gate is held open and the DECLARED level is asserted separately.
struct Fixture {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard;
    std::thread runner;
    eventbus::AsyncEventBus bus;
    std::shared_ptr<ProcessSessionStore> store;
    std::shared_ptr<ProcessToolSet> set;

    Fixture()
        : guard(asio::make_work_guard(io)),
          runner([this] { io.run(); }),
          store(std::make_shared<ProcessSessionStore>(io.get_executor())),
          // The set asks its confirmation question on THIS bus, so a handler
          // subscribed below answers only these cases — nothing is left
          // behind on the process-wide bus for another suite to inherit.
          set(std::make_shared<ProcessToolSet>(store, &bus))
    {}

    ~Fixture()
    {
        // The store's children go first, and while the context still runs:
        // terminate_all() is a coroutine, and the destructor could only fall
        // back to killing pids (see ProcessSessionStore's header).
        if (store) {
            asio::co_spawn(io, store->terminate_all(false), asio::use_future)
                .get();
        }
        set.reset();
        store.reset();
        guard.reset();
        io.stop();
        if (runner.joinable()) runner.join();
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator = (const Fixture&) = delete;

    template <typename Awaitable>
    auto run(Awaitable&& work)
    {
        return asio::co_spawn(io, std::forward<Awaitable>(work),
                              asio::use_future)
            .get();
    }

    /// PHASE 1 only: settle the call in place and return its tool. Throws the
    /// InvokeException the phase is documented to throw.
    tools::ToolSet::ToolHandle prepare(model_io::InvokeQuery& query)
    {
        return set->prepare(query);
    }

    /// Both phases, as a host runs them. The security check is answered by
    /// the fixture's own approving handler, so what comes back is the tool's
    /// own result or its own failure.
    model_io::InvokeReturn call(model_io::InvokeQuery query)
    {
        auto approve = bus.subscribe<InvokeConfirmEvent>(
            [](InvokeConfirmEvent event) -> asio::awaitable<InvokeConfirmEvent> {
                event.decision = ConfirmDecision::Approved;
                event.reason = "approved by the test fixture";
                co_return event;
            });

        model_io::InvokeQuery settled = std::move(query);
        tools::ToolSet::ToolHandle tool;
        try {
            tool = set->prepare(settled);
        } catch (const InvokeException& failure) {
            approve.disconnect();
            return failure.to_invoke_return(settled);
        }
        auto record = run(set->execute(std::move(tool), settled));
        approve.disconnect();
        return record;
    }

    /// The JSON a successful tool result carries. Fails the case when the
    /// record is a failure record instead.
    nlohmann::json payload_of(const model_io::InvokeReturn& record)
    {
        BOOST_TEST_REQUIRE(!tools::is_error(record),
                           "expected a result, got a failure: "
                               << record.output.raw);
        return nlohmann::json::parse(record.output.raw);
    }
};

/// The stage a failure record was raised at. Unwrapped here so no case has to
/// do the optional dance.
InvokeException::Stage stage_of(const model_io::InvokeReturn& record)
{
    BOOST_TEST_REQUIRE(tools::is_error(record),
                       "expected a failure, got a result: "
                           << record.output.raw);
    const auto stage = tools::error_stage(record);
    BOOST_TEST_REQUIRE(stage.has_value());
    return *stage;
}

/// Spawn a child through the tool and return its session id — the starting
/// point of most cases below.
std::string spawn_through_tool(Fixture& f, std::string executable,
                               std::vector<std::string> arguments = {})
{
    // WITHOUT the initial wait, which is what the cases using this helper mean
    // by "there is a process": they go on to drive it themselves — read it,
    // write to it, wait on it, kill it — and a spawn that had already waited
    // out a quick child would settle the very thing they are about to test
    // (`echo` would be finished, with its output already collected, before the
    // read under test ran). The cases about the window itself pass their own
    // expected_runtime_milliseconds.
    const auto record = f.call(call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{{"executable", std::move(executable)},
                       {"arguments", std::move(arguments)},
                       {"expected_runtime_milliseconds", 0}}));
    return f.payload_of(record).at("session_id").get<std::string>();
}

} // namespace

// ---- the set itself ---------------------------------------------------------

BOOST_AUTO_TEST_CASE(the_set_offers_six_routable_tools)
{
    Fixture f;
    BOOST_TEST(f.set->name() == std::string_view("process"));

    const std::vector<model_io::Invocable> catalogue = f.set->get_tools();
    BOOST_TEST_REQUIRE(catalogue.size() == std::size_t{6});

    // Presentation order follows the workflow — launch, observe, then act on a
    // live child — because that order is what the model reads.
    BOOST_TEST(catalogue[0].name == std::string(tool_names::kSpawn));
    BOOST_TEST(catalogue[1].name == std::string(tool_names::kPoll));
    BOOST_TEST(catalogue[2].name == std::string(tool_names::kRead));
    BOOST_TEST(catalogue[3].name == std::string(tool_names::kWait));
    BOOST_TEST(catalogue[4].name == std::string(tool_names::kWrite));
    BOOST_TEST(catalogue[5].name == std::string(tool_names::kKill));

    // Every tool carries a description and an object schema: this is the whole
    // contract a model has to work from.
    for (const model_io::Invocable& tool : catalogue) {
        BOOST_TEST(!tool.description.empty());
        BOOST_TEST(tool.argument_schema.at("type") == nlohmann::json("object"));
        BOOST_TEST(tool.argument_schema.contains("properties"));
    }
}

BOOST_AUTO_TEST_CASE(an_unknown_name_is_a_dispatch_failure)
{
    Fixture f;
    // dispatch() answers nullptr, which prepare() turns into the Dispatch
    // record the model reads — never a throw of its own.
    BOOST_TEST(f.set->dispatch(call_for("no_such_tool")) == nullptr);
    BOOST_CHECK(stage_of(f.call(call_for("no_such_tool"))) ==
                InvokeException::Stage::Dispatch);
}

BOOST_AUTO_TEST_CASE(a_null_store_is_refused_at_construction)
{
    // A set with no store would advertise tools it cannot answer.
    BOOST_CHECK_THROW(ProcessToolSet(nullptr), std::invalid_argument);
}

// ---- what each tool declares ------------------------------------------------

BOOST_AUTO_TEST_CASE(each_tool_declares_its_type_and_security)
{
    Fixture f;
    // Asserted on the SETTLED query, after phase 1: these are the values a
    // batch scheduler groups by and the security policy judges. The calls
    // start with the opposite pair, so a write_attributes() that never ran
    // could not produce these.
    struct Expectation {
        std::string_view name;
        model_io::InvokeType type;
        model_io::InvokeSecurity security;
    };
    const Expectation expected[] = {
        // State changes outside this process: they ask.
        {tool_names::kSpawn, model_io::InvokeType::SerialWrite,
         model_io::InvokeSecurity::RequireConfirm},
        {tool_names::kKill, model_io::InvokeType::SerialWrite,
         model_io::InvokeSecurity::RequireConfirm},
        // A write, but through the handle's thread-safe stdin channel, so it
        // needs no exclusive turn on the executor.
        {tool_names::kWrite, model_io::InvokeType::ParallWrite,
         model_io::InvokeSecurity::RequireConfirm},
        // Observation: parallelisable, and nothing to authorise.
        {tool_names::kPoll, model_io::InvokeType::ReadOnly,
         model_io::InvokeSecurity::Trusted},
        {tool_names::kRead, model_io::InvokeType::ReadOnly,
         model_io::InvokeSecurity::Trusted},
        {tool_names::kWait, model_io::InvokeType::ReadOnly,
         model_io::InvokeSecurity::Trusted},
    };

    for (const Expectation& expectation : expected) {
        // Arguments that satisfy each tool's own contract, so settling
        // reaches write_attributes() rather than failing first.
        nlohmann::json arguments = nlohmann::json::object();
        if (expectation.name == tool_names::kSpawn) {
            arguments["executable"] = "true";
        } else if (expectation.name != tool_names::kPoll) {
            arguments["session_id"] = "proc_1";
            if (expectation.name == tool_names::kWrite) {
                arguments["input"] = "x";
            }
        }

        model_io::InvokeQuery query =
            call_for(std::string(expectation.name), std::move(arguments));
        const auto tool = f.prepare(query);
        BOOST_TEST_REQUIRE(tool != nullptr);
        BOOST_TEST_CONTEXT("tool " << expectation.name) {
            BOOST_CHECK(query.type == expectation.type);
            BOOST_CHECK(query.security == expectation.security);
        }
    }
}

// ---- argument checking ------------------------------------------------------

BOOST_AUTO_TEST_CASE(malformed_arguments_fail_at_the_argument_parse_stage)
{
    Fixture f;
    // Every one of these is a mistake the MODEL can fix by re-reading its own
    // call, which is exactly what Stage::ArgumentParse means — so each must
    // fail there, in phase 1, before anything runs or asks for confirmation.
    const std::pair<std::string_view, nlohmann::json> bad_calls[] = {
        // spawn: the one required property, and the shapes of the rest.
        {tool_names::kSpawn, nlohmann::json::object()},
        {tool_names::kSpawn, {{"executable", ""}}},
        {tool_names::kSpawn, {{"executable", 42}}},
        {tool_names::kSpawn, {{"executable", "true"}, {"arguments", "not a list"}}},
        {tool_names::kSpawn, {{"executable", "true"},
                              {"arguments", nlohmann::json::array({1})}}},
        {tool_names::kSpawn, {{"executable", "true"},
                              {"environment", nlohmann::json::array({"NOEQUALS"})}}},
        {tool_names::kSpawn, {{"executable", "true"},
                              {"environment", nlohmann::json::array({"=novalue"})}}},
        {tool_names::kSpawn, {{"executable", "true"}, {"working_directory", 7}}},
        {tool_names::kSpawn, {{"executable", "true"},
                              {"inherit_environment", "yes"}}},
        // the session-id tools: missing, wrong type, empty.
        {tool_names::kRead, nlohmann::json::object()},
        {tool_names::kRead, {{"session_id", 1}}},
        {tool_names::kRead, {{"session_id", ""}}},
        {tool_names::kRead, {{"session_id", "proc_1"}, {"stream", "stdlog"}}},
        {tool_names::kRead, {{"session_id", "proc_1"}, {"full", "yes"}}},
        {tool_names::kWait, {{"session_id", "proc_1"},
                             {"timeout_milliseconds", -5}}},
        {tool_names::kWait, {{"session_id", "proc_1"},
                             {"timeout_milliseconds", "soon"}}},
        {tool_names::kKill, {{"session_id", "proc_1"}, {"graceful", 1}}},
        // write: a call that would neither send nor close does nothing at all.
        {tool_names::kWrite, {{"session_id", "proc_1"}}},
        {tool_names::kWrite, {{"session_id", "proc_1"}, {"input", 5}}},
        // poll: the id list's shape.
        {tool_names::kPoll, {{"session_ids", "proc_1"}}},
        {tool_names::kPoll, {{"session_ids", nlohmann::json::array({7})}}},
    };

    for (const auto& [name, arguments] : bad_calls) {
        const auto record = f.call(call_for(std::string(name), arguments));
        BOOST_TEST_CONTEXT("tool " << name << " args " << arguments.dump()) {
            BOOST_CHECK(stage_of(record) ==
                        InvokeException::Stage::ArgumentParse);
            // The record answers the call that was made — the identity the
            // conversation correlates by.
            BOOST_TEST(record.query.id == std::string("call_1"));
        }
    }
}

BOOST_AUTO_TEST_CASE(an_unknown_session_fails_at_the_invoke_stage)
{
    Fixture f;
    // Well-formed arguments naming something that is gone. Not
    // ArgumentParse: the call is correct, the world simply moved on — which a
    // model fixes by polling, not by re-reading its own call.
    const std::pair<std::string_view, nlohmann::json> calls[] = {
        {tool_names::kRead, {{"session_id", "proc_404"}}},
        {tool_names::kWait, {{"session_id", "proc_404"},
                             {"timeout_milliseconds", 10}}},
        {tool_names::kWrite, {{"session_id", "proc_404"}, {"input", "x"}}},
        {tool_names::kKill, {{"session_id", "proc_404"}}},
    };
    for (const auto& [name, arguments] : calls) {
        BOOST_TEST_CONTEXT("tool " << name) {
            BOOST_CHECK(stage_of(f.call(call_for(std::string(name), arguments))) ==
                        InvokeException::Stage::Invoke);
        }
    }
}

BOOST_AUTO_TEST_CASE(a_failed_launch_is_an_invoke_failure_carrying_the_context)
{
    Fixture f;
    const auto record = f.call(call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{{"executable", "simplex-no-such-executable-xyz"}}));

    BOOST_CHECK(stage_of(record) == InvokeException::Stage::Invoke);
    // The launch context survives into the text the model reads.
    BOOST_TEST(record.output.raw.find("simplex-no-such-executable-xyz") !=
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_missing_working_directory_is_reported_to_the_model)
{
    Fixture f;
    const auto record = f.call(call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{{"executable", "pwd"},
                       {"working_directory", "/simplex-no-such-dir-xyz"}}));

    BOOST_CHECK(stage_of(record) == InvokeException::Stage::Invoke);
    BOOST_TEST(record.output.raw.find("/simplex-no-such-dir-xyz") !=
               std::string::npos);
}

// ---- the tools' results -----------------------------------------------------

BOOST_AUTO_TEST_CASE(spawn_returns_the_whole_result_for_a_quick_command)
{
    // The ordinary case, and the reason the window exists: an everyday command
    // finishes inside it, so ONE call carries the exit code and the whole
    // output. A model never pays a second round trip for `ls`.
    Fixture f;
    const auto record = f.call(call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{{"executable", "echo"},
                       {"arguments", nlohmann::json::array({"quick"})},
                       {"description", "a quick command"}}));

    const nlohmann::json payload = f.payload_of(record);
    BOOST_TEST(payload.at("session_id") == nlohmann::json("proc_1"));
    BOOST_TEST(payload.at("executable") == nlohmann::json("echo"));
    BOOST_TEST(payload.at("description") == nlohmann::json("a quick command"));
    BOOST_TEST(payload.at("finished") == nlohmann::json(true));
    BOOST_TEST(payload.at("state") == nlohmann::json("exited"));
    BOOST_TEST(payload.at("exit_code") == nlohmann::json(0));
    BOOST_TEST(payload.at("pid").get<int>() > 0);
    // The output comes back with it — the whole point of having waited.
    BOOST_TEST(payload.at("stdout_text") == nlohmann::json("quick\n"));
    BOOST_TEST(payload.at("stderr_text") == nlohmann::json(""));
    BOOST_TEST(payload.contains("hint"));
    // The result is a text part carrying JSON — structure a model can read
    // without parsing prose.
    BOOST_CHECK(record.output.type == model_io::ContentType::Text);

    // A full read was used, so the delta cursor is untouched: a later read
    // still reports everything rather than finding it already consumed.
    const auto read = f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", "proc_1"}, {"stream", "stdout"}}));
    BOOST_TEST(f.payload_of(read).at("stdout_text") == nlohmann::json("quick\n"));
}

BOOST_AUTO_TEST_CASE(spawn_hands_back_a_session_for_a_program_that_keeps_running)
{
    // The other half: a program still running when the window closes becomes a
    // session, and the answer is the id plus what to do with it. The window is
    // short on purpose — the case is about the branch, not about waiting.
    Fixture f;
    const auto record = f.call(call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{{"executable", "sleep"},
                       {"arguments", nlohmann::json::array({"30"})},
                       {"description", "a long nap"},
                       {"expected_runtime_milliseconds", 50}}));

    const nlohmann::json payload = f.payload_of(record);
    BOOST_TEST(payload.at("session_id") == nlohmann::json("proc_1"));
    BOOST_TEST(payload.at("finished") == nlohmann::json(false));
    BOOST_TEST(payload.at("state") == nlohmann::json("running"));
    BOOST_TEST(payload.at("pid").get<int>() > 0);
    // Still running, so there is no exit code and no output slice the caller
    // did not ask for.
    BOOST_TEST(!payload.contains("exit_code"));
    BOOST_TEST(!payload.contains("stdout_text"));
    BOOST_TEST(payload.contains("hint"));
}

BOOST_AUTO_TEST_CASE(a_zero_window_returns_a_session_without_waiting)
{
    // "Start it and give me the id." 0 means wait indefinitely to the handle
    // underneath, so the store has to read it as the opposite — otherwise this
    // call would hang on a child that never exits.
    Fixture f;
    const auto record = f.call(call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{{"executable", "sleep"},
                       {"arguments", nlohmann::json::array({"30"})},
                       {"expected_runtime_milliseconds", 0}}));

    const nlohmann::json payload = f.payload_of(record);
    BOOST_TEST(payload.at("finished") == nlohmann::json(false));
    BOOST_TEST(payload.at("state") == nlohmann::json("running"));
}

BOOST_AUTO_TEST_CASE(wait_reports_the_exit_and_the_whole_output)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "echo", {"hello", "tools"});

    const auto record = f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}}));

    const nlohmann::json payload = f.payload_of(record);
    BOOST_TEST(payload.at("exited") == nlohmann::json(true));
    BOOST_TEST(payload.at("timed_out") == nlohmann::json(false));
    BOOST_TEST(payload.at("state") == nlohmann::json("exited"));
    BOOST_TEST(payload.at("exit_code") == nlohmann::json(0));
    BOOST_TEST(payload.at("stdout_text") == nlohmann::json("hello tools\n"));
    BOOST_TEST(payload.at("stderr_text") == nlohmann::json(""));
}

BOOST_AUTO_TEST_CASE(wait_reports_a_timeout_as_a_result_not_a_failure)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "sleep", {"30"});

    const auto record = f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 150}}));

    // A timeout is a result: the model is told the process is still running
    // and can decide what to do, rather than being handed an error.
    const nlohmann::json payload = f.payload_of(record);
    BOOST_TEST(payload.at("exited") == nlohmann::json(false));
    BOOST_TEST(payload.at("timed_out") == nlohmann::json(true));
    BOOST_TEST(payload.at("state") == nlohmann::json("running"));
}

BOOST_AUTO_TEST_CASE(a_nonzero_exit_is_a_result_not_a_failure)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "false");
    const auto record = f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}}));

    // The tool ran and the process ran: a command that failed is something
    // the model must reason about, not a failure of the invocation.
    const nlohmann::json payload = f.payload_of(record);
    BOOST_TEST(payload.at("exit_code") == nlohmann::json(1));
    BOOST_TEST(payload.at("exited") == nlohmann::json(true));
}

BOOST_AUTO_TEST_CASE(read_returns_the_delta_then_nothing_and_full_repeats_it)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "echo", {"once"});
    f.call(call_for(std::string(tool_names::kWait),
                    nlohmann::json{{"session_id", id},
                                   {"timeout_milliseconds", 5000}}));

    const nlohmann::json first = f.payload_of(f.call(call_for(
        std::string(tool_names::kRead), nlohmann::json{{"session_id", id}})));
    BOOST_TEST(first.at("stdout_text") == nlohmann::json("once\n"));
    BOOST_TEST(first.at("stdout_bytes_read") == nlohmann::json(5));

    // The property a poll loop relies on: a second delta read is empty.
    const nlohmann::json second = f.payload_of(f.call(call_for(
        std::string(tool_names::kRead), nlohmann::json{{"session_id", id}})));
    BOOST_TEST(second.at("stdout_text") == nlohmann::json(""));

    // full re-reads everything and does NOT consume the delta.
    const nlohmann::json full = f.payload_of(f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", id}, {"full", true}})));
    BOOST_TEST(full.at("stdout_text") == nlohmann::json("once\n"));
    BOOST_TEST(full.at("full") == nlohmann::json(true));
}

BOOST_AUTO_TEST_CASE(read_can_select_one_stream)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "echo", {"out"});
    f.call(call_for(std::string(tool_names::kWait),
                    nlohmann::json{{"session_id", id},
                                   {"timeout_milliseconds", 5000}}));

    const nlohmann::json only_err = f.payload_of(f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", id}, {"stream", "stderr"}})));
    // Only the stream that was asked for is in the result — a caller reading
    // stderr is not handed stdout it did not ask for (and whose delta cursor
    // it would then have consumed).
    BOOST_TEST(only_err.at("stream") == nlohmann::json("stderr"));
    BOOST_TEST(only_err.at("stderr_text") == nlohmann::json(""));
    BOOST_TEST(!only_err.contains("stdout_text"));

    const nlohmann::json only_out = f.payload_of(f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", id}, {"stream", "stdout"}})));
    BOOST_TEST(only_out.at("stdout_text") == nlohmann::json("out\n"));
    BOOST_TEST(!only_out.contains("stderr_text"));
}

BOOST_AUTO_TEST_CASE(poll_reports_every_session_and_only_new_output)
{
    Fixture f;
    const std::string finished = spawn_through_tool(f, "echo", {"done"});
    const std::string running = spawn_through_tool(f, "sleep", {"30"});
    f.call(call_for(std::string(tool_names::kWait),
                    nlohmann::json{{"session_id", finished},
                                   {"timeout_milliseconds", 5000}}));

    const nlohmann::json first = f.payload_of(
        f.call(call_for(std::string(tool_names::kPoll))));
    BOOST_TEST_REQUIRE(first.at("sessions").size() == std::size_t{2});
    BOOST_TEST(first.at("live_session_count") == nlohmann::json(2));

    // Sorted by id, so a poll loop's output reads the same way every turn.
    const nlohmann::json& done_entry = first.at("sessions")[0];
    BOOST_TEST(done_entry.at("session_id") == nlohmann::json(finished));
    BOOST_TEST(done_entry.at("state") == nlohmann::json("exited"));
    BOOST_TEST(done_entry.at("new_stdout") == nlohmann::json("done\n"));
    BOOST_TEST(first.at("sessions")[1].at("state") == nlohmann::json("running"));

    // Polled again: the same sessions, but nothing NEW to report.
    const nlohmann::json second = f.payload_of(
        f.call(call_for(std::string(tool_names::kPoll))));
    BOOST_TEST(second.at("sessions")[0].at("new_stdout") == nlohmann::json(""));

    // A named subset, without output.
    const nlohmann::json subset = f.payload_of(f.call(call_for(
        std::string(tool_names::kPoll),
        nlohmann::json{{"session_ids", nlohmann::json::array({running})},
                       {"include_output", false}})));
    BOOST_TEST_REQUIRE(subset.at("sessions").size() == std::size_t{1});
    BOOST_TEST(subset.at("sessions")[0].at("session_id") ==
               nlohmann::json(running));
    BOOST_TEST(!subset.at("sessions")[0].contains("new_stdout"));
}

BOOST_AUTO_TEST_CASE(poll_releases_exited_sessions_only_after_reporting_them)
{
    Fixture f;
    const std::string finished = spawn_through_tool(f, "echo", {"last words"});
    const std::string running = spawn_through_tool(f, "sleep", {"30"});
    f.call(call_for(std::string(tool_names::kWait),
                    nlohmann::json{{"session_id", finished},
                                   {"timeout_milliseconds", 5000}}));

    const nlohmann::json payload = f.payload_of(f.call(call_for(
        std::string(tool_names::kPoll),
        nlohmann::json{{"release_exited", true}})));

    // The output is in THIS result — reaping before reading would have lost a
    // dead child's last words for good.
    BOOST_TEST_REQUIRE(payload.at("sessions").size() == std::size_t{2});
    BOOST_TEST(payload.at("sessions")[0].at("new_stdout") ==
               nlohmann::json("last words\n"));
    BOOST_TEST_REQUIRE(payload.contains("released"));
    BOOST_TEST(payload.at("released").size() == std::size_t{1});
    BOOST_TEST(payload.at("released")[0] == nlohmann::json(finished));

    // Gone now; the running one is untouched, since release refuses a live
    // child.
    const nlohmann::json after = f.payload_of(
        f.call(call_for(std::string(tool_names::kPoll))));
    BOOST_TEST_REQUIRE(after.at("sessions").size() == std::size_t{1});
    BOOST_TEST(after.at("sessions")[0].at("session_id") ==
               nlohmann::json(running));
}

BOOST_AUTO_TEST_CASE(release_is_refused_while_the_process_runs)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "sleep", {"30"});
    const nlohmann::json payload = f.payload_of(f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", id}, {"release", true}})));

    // Honest about what happened rather than about what was asked: releasing
    // a live session would drop the last handle reference and kill the child.
    BOOST_TEST(payload.at("released") == nlohmann::json(false));
    BOOST_TEST(f.run(f.store->size()) == std::size_t{1});
}

BOOST_AUTO_TEST_CASE(write_feeds_stdin_and_close_input_ends_it)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "cat");

    const nlohmann::json first = f.payload_of(f.call(call_for(
        std::string(tool_names::kWrite),
        nlohmann::json{{"session_id", id}, {"input", "one\n"}})));
    BOOST_TEST(first.at("bytes_queued") == nlohmann::json(4));
    BOOST_TEST(first.at("input_closed") == nlohmann::json(false));
    // Queued, not delivered: the result must not claim the child has read it.
    BOOST_TEST(first.contains("note"));

    const nlohmann::json closing = f.payload_of(f.call(call_for(
        std::string(tool_names::kWrite),
        nlohmann::json{{"session_id", id},
                       {"input", "two\n"},
                       {"close_input", true}})));
    BOOST_TEST(closing.at("input_closed") == nlohmann::json(true));

    // cat echoes both lines and exits on the EOF the close produced.
    const nlohmann::json waited = f.payload_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    BOOST_TEST(waited.at("exit_code") == nlohmann::json(0));
    BOOST_TEST(waited.at("stdout_text") == nlohmann::json("one\ntwo\n"));
}

BOOST_AUTO_TEST_CASE(writing_to_an_exited_process_warns_rather_than_fails)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "true");
    f.call(call_for(std::string(tool_names::kWait),
                    nlohmann::json{{"session_id", id},
                                   {"timeout_milliseconds", 5000}}));

    // The session exists, so this is not a failure — but the input goes
    // nowhere, and nothing else in the result would reveal that.
    const nlohmann::json payload = f.payload_of(f.call(call_for(
        std::string(tool_names::kWrite),
        nlohmann::json{{"session_id", id}, {"input", "ignored\n"}})));
    BOOST_TEST(payload.at("state") == nlohmann::json("exited"));
    BOOST_TEST_REQUIRE(payload.contains("warning"));
}

BOOST_AUTO_TEST_CASE(kill_ends_a_running_process_and_leaves_it_readable)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "sleep", {"30"});

    const nlohmann::json killed = f.payload_of(f.call(call_for(
        std::string(tool_names::kKill), nlohmann::json{{"session_id", id}})));
    BOOST_TEST(killed.at("signalled") == nlohmann::json(true));
    BOOST_TEST(killed.at("graceful") == nlohmann::json(false));

    const nlohmann::json waited = f.payload_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    BOOST_TEST(waited.at("exited") == nlohmann::json(true));
    // SIGKILL, as the signal number.
    BOOST_TEST(waited.at("exit_code") == nlohmann::json(9));

    // Killing again says so instead of failing: the session is still there,
    // its child simply is not.
    const nlohmann::json again = f.payload_of(f.call(call_for(
        std::string(tool_names::kKill), nlohmann::json{{"session_id", id}})));
    BOOST_TEST(again.at("signalled") == nlohmann::json(false));
}

BOOST_AUTO_TEST_CASE(a_graceful_kill_sends_the_softer_signal)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "sleep", {"30"});
    const nlohmann::json payload = f.payload_of(f.call(call_for(
        std::string(tool_names::kKill),
        nlohmann::json{{"session_id", id}, {"graceful", true}})));
    BOOST_TEST(payload.at("graceful") == nlohmann::json(true));
    BOOST_TEST(payload.at("signalled") == nlohmann::json(true));

    const nlohmann::json waited = f.payload_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    // sleep does not catch SIGTERM, so it dies of the signal (15).
    BOOST_TEST(waited.at("exit_code") == nlohmann::json(15));
}

BOOST_AUTO_TEST_CASE(spawn_honours_the_working_directory_and_environment)
{
    Fixture f;
    const std::string temp =
        std::filesystem::canonical(std::filesystem::temp_directory_path())
            .string();

    const std::string id = f.payload_of(f.call(call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{{"executable", "pwd"},
                       {"arguments", nlohmann::json::array({"-P"})},
                       {"working_directory", temp}}))).at("session_id");
    const nlohmann::json waited = f.payload_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    BOOST_TEST(waited.at("stdout_text") == nlohmann::json(temp + "\n"));
    BOOST_TEST(waited.at("working_directory") == nlohmann::json(temp));

    const std::string env_id = f.payload_of(f.call(call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{
            {"executable", "sh"},
            {"arguments", nlohmann::json::array({"-c", "printf %s \"$MARKER\""})},
            {"environment",
             nlohmann::json::array({"MARKER=sentinel-value"})}}))).at("session_id");
    const nlohmann::json env_waited = f.payload_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", env_id},
                       {"timeout_milliseconds", 5000}})));
    BOOST_TEST(env_waited.at("stdout_text") == nlohmann::json("sentinel-value"));
}

// ---- through the registry ---------------------------------------------------

BOOST_AUTO_TEST_CASE(a_whole_turn_runs_through_the_registry)
{
    // The end-to-end shape: the set registered with a ToolRegistry, and calls
    // arriving as batches the way an agent loop delivers them. What this adds
    // over the cases above is the routing and the batch scheduling — the
    // registry settles every call first, runs the serial ones alone, and
    // answers with one record per call in call order.
    Fixture f;
    tools::ToolRegistry registry;
    registry.add(f.set);

    // The catalogue a request builder would hand the model.
    BOOST_TEST(registry.get_tools().size() == std::size_t{6});
    BOOST_TEST(registry.contains(std::string(tool_names::kSpawn)));

    auto approve = f.bus.subscribe<InvokeConfirmEvent>(
        [](InvokeConfirmEvent event) -> asio::awaitable<InvokeConfirmEvent> {
            event.decision = ConfirmDecision::Approved;
            co_return event;
        });

    // Turn 1: spawn cat, alongside a poll — a SerialWrite and a ReadOnly in
    // one batch, which the registry runs serial-first and joins.
    auto first_batch = f.run(registry.execute(std::vector<model_io::InvokeQuery>{
        call_for(std::string(tool_names::kSpawn),
                 nlohmann::json{{"executable", "cat"}}, "call_spawn"),
        call_for(std::string(tool_names::kPoll), {}, "call_poll"),
    }));
    BOOST_TEST_REQUIRE(first_batch.size() == std::size_t{2});
    // Records come back in CALL order, never completion order, each
    // answering the call at its position.
    BOOST_TEST(first_batch[0].query.id == std::string("call_spawn"));
    BOOST_TEST(first_batch[1].query.id == std::string("call_poll"));
    BOOST_TEST_REQUIRE(!tools::is_error(first_batch[0]));
    const std::string id =
        nlohmann::json::parse(first_batch[0].output.raw).at("session_id");

    // Turn 2: feed it, end its input, wait, and read — the whole workflow.
    auto second_batch = f.run(registry.execute(std::vector<model_io::InvokeQuery>{
        call_for(std::string(tool_names::kWrite),
                 nlohmann::json{{"session_id", id},
                                {"input", "through the registry\n"},
                                {"close_input", true}}, "call_write"),
    }));
    BOOST_TEST_REQUIRE(!tools::is_error(second_batch[0]));

    auto third_batch = f.run(registry.execute(std::vector<model_io::InvokeQuery>{
        call_for(std::string(tool_names::kWait),
                 nlohmann::json{{"session_id", id},
                                {"timeout_milliseconds", 5000}}, "call_wait"),
    }));
    BOOST_TEST_REQUIRE(!tools::is_error(third_batch[0]));
    const nlohmann::json waited =
        nlohmann::json::parse(third_batch[0].output.raw);
    BOOST_TEST(waited.at("exited") == nlohmann::json(true));
    BOOST_TEST(waited.at("stdout_text") ==
               nlohmann::json("through the registry\n"));

    // An unknown tool in a batch is answered, not dropped: every call gets
    // exactly one record, which is a wire requirement.
    auto mixed = f.run(registry.execute(std::vector<model_io::InvokeQuery>{
        call_for("no_such_tool", {}, "call_bogus"),
        call_for(std::string(tool_names::kPoll), {}, "call_poll_2"),
    }));
    BOOST_TEST_REQUIRE(mixed.size() == std::size_t{2});
    BOOST_CHECK(stage_of(mixed[0]) == InvokeException::Stage::Dispatch);
    BOOST_TEST(!tools::is_error(mixed[1]));

    approve.disconnect();
}

BOOST_AUTO_TEST_CASE(an_unconfirmed_state_change_is_refused)
{
    // The gate the RequireConfirm tools declare, with NOBODY subscribed to
    // answer: per the module's policy, silence refuses. This is the one case
    // that must not hold the fixture's approving handler open, so it drives
    // the set directly rather than through Fixture::call().
    Fixture f;
    model_io::InvokeQuery query = call_for(
        std::string(tool_names::kSpawn), nlohmann::json{{"executable", "true"}});
    const auto tool = f.prepare(query);
    BOOST_TEST_REQUIRE(tool != nullptr);
    BOOST_CHECK(query.security == model_io::InvokeSecurity::RequireConfirm);

    const auto record = f.run(f.set->execute(tool, query));
    BOOST_CHECK(stage_of(record) == InvokeException::Stage::SecurityCheck);
    // Nothing was launched.
    BOOST_TEST(f.run(f.store->size()) == std::size_t{0});

    // A Trusted tool needs no confirmation and runs in the same conditions.
    model_io::InvokeQuery poll = call_for(std::string(tool_names::kPoll));
    const auto poll_tool = f.prepare(poll);
    BOOST_CHECK(poll.security == model_io::InvokeSecurity::Trusted);
    BOOST_TEST(!tools::is_error(f.run(f.set->execute(poll_tool, poll))));
}
