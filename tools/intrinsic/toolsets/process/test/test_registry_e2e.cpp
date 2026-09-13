#define BOOST_TEST_MODULE ProcessRegistryEndToEndTests
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/process/toolset.hpp"
#include "tools/intrinsic/process/tools.hpp"
#include "tools/intrinsic/tool_base.hpp"

#include "tools/registry.hpp"
#include "tools/security_check.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <nlohmann/json.hpp>

// The third layer of the test stack: ToolRegistry + ProcessToolSet + a real
// ProcessSessionStore + real children, driven the way the agent loop drives
// them.
//
// WHY THIS LAYER EXISTS, given that test_tools already covers each tool and
// test_session_store covers the table. Because InvokeType is not metadata in
// this toolset — it is part of the implementation's correctness. `poll` with
// output, a delta `read` and any `release` advance or remove state that a
// neighbouring call in the same batch can address, so whether a batch gives a
// call the executor to itself decides what the call MEANS. A per-tool test
// cannot see that: it runs one call at a time. These cases run batches, through
// the registry, on a context with SEVERAL worker threads, and assert both what
// came back and how the batch was scheduled.
//
// The shape of a case here is therefore always the same:
//
//   model call(s) -> ToolRegistry::execute -> records -> JSON payload
//
// never a tool hook directly, and never a toolset phase directly. The registry
// is the composition the agent loop actually uses.
//
// WHAT IS REAL AND WHAT IS INSTRUMENTED. The registry, the set, the store, the
// handles and the children are all the production types. The one addition is
// ProbeTool below: a test-only tool whose only job is to record when it ran, so
// the scheduling rules can be asserted (a batch of overlapping calls really
// overlaps; a SerialWrite really does not) instead of being inferred from
// results that would look the same either way.
//
// Children are the same harmless coreutils the other suites use (echo / cat /
// true / sleep / sh), and every case gets its own context, store and registry,
// so what a case observes is only what its own calls made.

namespace asio = boost::asio;
using tools::ConfirmDecision;
using tools::InvokeConfirmEvent;
using tools::intrinsic::ProcessSessionStore;
using tools::intrinsic::ProcessToolSet;
namespace tool_names = tools::intrinsic::tool_names;

namespace {

/// A call as the conversation carries it, with position/identity made explicit
/// so every assertion below can name the call it is talking about.
model_io::InvokeQuery call_for(std::string name, nlohmann::json arguments = {},
                               std::string id = "call_1")
{
    model_io::InvokeQuery query;
    // Deliberately the opposite of what the process tools declare, so a
    // write_attributes() that never ran cannot make an assertion pass.
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::DefaultDeny;
    query.id = std::move(id);
    query.name = std::move(name);
    query.arguments = arguments.is_null() ? nlohmann::json::object()
                                          : std::move(arguments);
    return query;
}

/// The JSON a successful tool result carries. Fails the case when the record is
/// a failure record instead.
nlohmann::json payload_of(const model_io::InvokeReturn& record)
{
    BOOST_TEST_REQUIRE(!tools::is_error(record),
                       "expected a result, got a failure: "
                           << record.output.raw);
    return nlohmann::json::parse(record.output.raw);
}

// ---- the scheduling instrument ----------------------------------------------

/**
 * What the probes recorded: how many were running at once, and in what order
 * they started and finished.
 *
 * `live` is a LOGICAL count (entered, not yet left) rather than a thread count
 * on purpose. What a scheduler promises is that two calls do not overlap, and
 * "overlap" is about time, not about which worker thread happened to pick each
 * one up: two branches of one batch can share a thread and still interleave
 * their awaits, and that is exactly the interference a wrong ReadOnly permits.
 */
struct ProbeLog {
    std::mutex mutex;
    int live = 0;
    int peak = 0;
    std::vector<std::string> events;

    void enter(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++live;
        peak = std::max(peak, live);
        events.push_back("start " + name);
    }

    void leave(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex);
        --live;
        events.push_back("end " + name);
    }

    [[nodiscard]] int peak_concurrency()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return peak;
    }

    [[nodiscard]] std::vector<std::string> sequence()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return events;
    }
};

/**
 * A tool that does nothing but record when it runs, for the length of time it
 * was asked to spend.
 *
 * It derives from IntrinsicTool so it is a tool in every structural sense (the
 * same Invocable storage, the same JSON result shape) and differs from a real
 * one only in what invoke() does. The type and security it declares come from
 * the case, which is the whole point: the same tool can be registered as
 * ReadOnly, ParallWrite or SerialWrite, and the batch's behavior must differ
 * accordingly.
 *
 * IT MUST SUSPEND, and it does, on a timer. A branch that never suspends runs
 * inline on the collector's thread before the next branch is even spawned
 * (tools/registry.hpp, step 3), so a probe that returned immediately would show
 * no overlap even when the scheduler allowed plenty — and the case would pass
 * for the wrong reason.
 */
class ProbeTool final : public tools::intrinsic::IntrinsicTool {
public:
    ProbeTool(std::string name, model_io::InvokeType type,
              std::shared_ptr<ProbeLog> log,
              std::chrono::milliseconds work = std::chrono::milliseconds{150})
        : _type(type), _log(std::move(log)), _work(work)
    {
        _details.name = std::move(name);
        _details.description = "a test-only tool that records when it runs";
        _details.argument_schema = object_schema(nlohmann::json::object());
    }

    void write_attributes(model_io::InvokeQuery& query) const override
    {
        query.type = _type;
        // Trusted, so a probe never needs a confirmation: asking for one would
        // put a second subject in the middle of a scheduling assertion.
        query.security = model_io::InvokeSecurity::Trusted;
    }

    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override
    {
        _log->enter(query.name);
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(_work);
        co_await timer.async_wait(asio::use_awaitable);
        _log->leave(query.name);
        co_return json_content(nlohmann::json{{"probe", query.name}});
    }

private:
    model_io::InvokeType _type;
    std::shared_ptr<ProbeLog> _log;
    std::chrono::milliseconds _work;
};

/// The smallest ToolSet around a list of tools: the routing a set owes, and
/// nothing else. The process cases use IntrinsicToolSet through
/// ProcessToolSet; this one exists so a batch can mix process calls with
/// probes.
class ProbeSet final : public tools::ToolSet {
public:
    explicit ProbeSet(std::vector<ToolHandle> tools) : _tools(std::move(tools))
    {}

    std::string_view name() const noexcept override { return "probe"; }

    std::vector<model_io::Invocable> get_tools() const override
    {
        std::vector<model_io::Invocable> catalogue;
        catalogue.reserve(_tools.size());
        for (const ToolHandle& tool : _tools) {
            catalogue.push_back(tool->get_details());
        }
        return catalogue;
    }

    ToolHandle dispatch(const model_io::InvokeQuery& query) const override
    {
        for (const ToolHandle& tool : _tools) {
            if (tool->get_details().name == query.name) return tool;
        }
        return nullptr;
    }

private:
    std::vector<ToolHandle> _tools;
};

// ---- the fixture -------------------------------------------------------------

/// The real thing, driven the way a host drives it: one registry holding the
/// process set (and the probes), a store on a context with SEVERAL worker
/// threads, and a confirmation handler on a bus the fixture owns.
///
/// Several workers, not one, and that is a requirement rather than a
/// preference: with a single runner nothing in this design is ever concurrent,
/// so a wrong strand assumption, an off-strand read or a serial/parallel
/// mistake cannot show up no matter how wrong it is. The store deliberately
/// runs two levels of strand (its own for the table, one per child for the
/// handle), and those only have something to serialise when more than one
/// thread is running.
///
/// The confirmation handler is subscribed HERE, on the fixture's own bus, and
/// records every question it is asked: default_async_bus() is process-wide, so
/// subscribing there would leak a handler into every other suite, and recording
/// the questions is what lets a case assert that the confirmation showed the
/// call that actually ran.
struct Fixture {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard;
    std::vector<std::thread> workers;
    eventbus::AsyncEventBus bus;
    std::shared_ptr<ProcessSessionStore> store;
    std::shared_ptr<ProcessToolSet> set;
    std::shared_ptr<ProbeLog> probes = std::make_shared<ProbeLog>();
    tools::ToolRegistry registry;

    /// Every confirmation asked, in the order it was asked.
    std::mutex confirmations_mutex;
    std::vector<model_io::InvokeQuery> confirmations;
    eventbus::AsyncEventBus::ScopedSubscription approver;

    static constexpr std::string_view kSerialProbeA = "probe_serial_a";
    static constexpr std::string_view kSerialProbeB = "probe_serial_b";
    static constexpr std::string_view kReadOnlyProbeA = "probe_read_only_a";
    static constexpr std::string_view kReadOnlyProbeB = "probe_read_only_b";
    static constexpr std::string_view kParallelProbe = "probe_parallel_write";

    explicit Fixture(std::size_t worker_count = 3)
        : guard(asio::make_work_guard(io)),
          store(std::make_shared<ProcessSessionStore>(io.get_executor())),
          // The set asks its confirmation question on THIS bus, so the
          // handler below answers only these cases.
          set(std::make_shared<ProcessToolSet>(store, &bus))
    {
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers.emplace_back([this] { io.run(); });
        }

        approver = bus.subscribe<InvokeConfirmEvent>(
            [this](InvokeConfirmEvent event) -> asio::awaitable<InvokeConfirmEvent> {
                {
                    std::lock_guard<std::mutex> lock(confirmations_mutex);
                    confirmations.push_back(event.query);
                }
                event.decision = ConfirmDecision::Approved;
                event.reason = "approved by the end-to-end fixture";
                co_return event;
            });

        registry.add(set);
        registry.add(std::make_shared<ProbeSet>(std::vector<tools::ToolSet::ToolHandle>{
            std::make_shared<ProbeTool>(std::string(kSerialProbeA),
                                        model_io::InvokeType::SerialWrite, probes),
            std::make_shared<ProbeTool>(std::string(kSerialProbeB),
                                        model_io::InvokeType::SerialWrite, probes),
            std::make_shared<ProbeTool>(std::string(kReadOnlyProbeA),
                                        model_io::InvokeType::ReadOnly, probes),
            std::make_shared<ProbeTool>(std::string(kReadOnlyProbeB),
                                        model_io::InvokeType::ReadOnly, probes),
            std::make_shared<ProbeTool>(std::string(kParallelProbe),
                                        model_io::InvokeType::ParallWrite, probes),
        }));
    }

    ~Fixture()
    {
        // Children first, and while the context still runs: terminate_all() is
        // a coroutine and the store's destructor can only fall back to
        // signalling pids (see ProcessSessionStore's header). Then the tools
        // (the registry holds the set, so it goes first), then the workers
        // stopped and joined, and the TABLE last: killing a leaked child needs
        // no executor, while freeing the table's strand before the workers are
        // done with the context races the asio executor refcount they share
        // (ThreadSanitizer reports exactly that).
        if (store) {
            asio::co_spawn(io, store->terminate_all(false), asio::use_future)
                .get();
        }
        registry.clear();
        set.reset();
        approver.disconnect();
        guard.reset();
        io.stop();
        for (std::thread& worker : workers) {
            if (worker.joinable()) worker.join();
        }
        store.reset();
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

    /// One batch, the way the agent loop runs one: every call settled, then the
    /// serial ones alone, then the rest together, and one record per call in
    /// call order.
    std::vector<model_io::InvokeReturn> batch(
        std::vector<model_io::InvokeQuery> queries)
    {
        return run(registry.execute(std::move(queries), io.get_executor()));
    }

    /// A single call, as a one-call batch — never a direct toolset call, so
    /// everything in this suite goes through the composition under test.
    model_io::InvokeReturn call(model_io::InvokeQuery query)
    {
        std::vector<model_io::InvokeReturn> records =
            batch(std::vector<model_io::InvokeQuery>{std::move(query)});
        BOOST_TEST_REQUIRE(records.size() == std::size_t{1});
        return std::move(records[0]);
    }

    /// Spawn a child and return its session id — the starting point of most
    /// cases. `expected_runtime_milliseconds: 0` unless the case says
    /// otherwise: these cases go on to drive the child themselves, so a spawn
    /// that had already waited a quick child out would settle the very thing
    /// they are about to test.
    std::string spawn(std::string executable, std::vector<std::string> arguments,
                      std::uint64_t window = 0, std::string id = "call_spawn")
    {
        const auto record = call(call_for(
            std::string(tool_names::kSpawn),
            nlohmann::json{{"executable", std::move(executable)},
                           {"arguments", std::move(arguments)},
                           {"expected_runtime_milliseconds", window}},
            std::move(id)));
        return payload_of(record).at("session_id").get<std::string>();
    }

    std::vector<model_io::InvokeQuery> questions_asked()
    {
        std::lock_guard<std::mutex> lock(confirmations_mutex);
        return confirmations;
    }
};

} // namespace

// ---- the settled call at the registry boundary ------------------------------

BOOST_AUTO_TEST_CASE(the_registry_answers_the_settled_call_it_ran)
{
    // The chain the whole layer exists for, asserted at the end where a host
    // can see all of it:
    //
    //   model call -> registry -> prepare -> ensure_arguments -> write_attributes
    //              -> confirmation -> invoke -> InvokeReturn
    //
    // What comes back must be the SETTLED query — the one the confirmer was
    // shown, the one invoke() read and the one the conversation correlates by —
    // with its defaults materialized, not the raw call the model sent and not a
    // query that only the tool ever saw the settled form of.
    Fixture f;
    BOOST_TEST(f.workers.size() >= std::size_t{2});

    const std::string id = f.spawn("cat", {}, 0);

    const std::vector<model_io::InvokeReturn> records =
        f.batch(std::vector<model_io::InvokeQuery>{
            call_for(std::string(tool_names::kWrite),
                     nlohmann::json{{"session_id", id},
                                    {"input", "hello\n"},
                                    {"close_input", true}},
                     "call_write"),
            call_for(std::string(tool_names::kRead),
                     nlohmann::json{{"session_id", id}, {"full", true}},
                     "call_read"),
        });
    BOOST_TEST_REQUIRE(records.size() == std::size_t{2});

    // Records are filed by POSITION: results[i] answers queries[i], whatever
    // each one settled to.
    BOOST_TEST(records[0].query.id == std::string("call_write"));
    BOOST_TEST(records[1].query.id == std::string("call_read"));

    // The write settled as the serial, confirmed call it is...
    BOOST_CHECK(records[0].query.type == model_io::InvokeType::SerialWrite);
    BOOST_CHECK(records[0].query.security ==
                model_io::InvokeSecurity::RequireConfirm);
    // ...and the read as the observation, with the defaults written in.
    BOOST_CHECK(records[1].query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(records[1].query.security == model_io::InvokeSecurity::Trusted);
    BOOST_TEST(records[1].query.arguments ==
               nlohmann::json({{"session_id", id}, {"full", true},
                               {"stream", "both"}, {"release", false}}));
    // The write's own defaults are in its settled query too: `close_input` was
    // named, `input` was named, and nothing was invented.
    BOOST_TEST(records[0].query.arguments ==
               nlohmann::json({{"session_id", id}, {"input", "hello\n"},
                               {"close_input", true}}));

    // And the human (here: the fixture's handler) was asked about EXACTLY the
    // call that ran. Two calls in this case ask — the spawn that made the
    // session and the write — and each was asked once, which is the other half
    // of the property: a confirmation is per call, not per batch.
    const std::vector<model_io::InvokeQuery> questions = f.questions_asked();
    BOOST_TEST_REQUIRE(questions.size() == std::size_t{2});
    BOOST_TEST(questions[0].id == std::string("call_spawn"));
    BOOST_TEST(questions[1].id == records[0].query.id);
    BOOST_TEST(questions[1].name == records[0].query.name);
    BOOST_CHECK(questions[1].type == records[0].query.type);
    BOOST_CHECK(questions[1].security == records[0].query.security);
    // The assertion a confirmer's correctness rests on: the question was about
    // the settled call, defaults and all. If invoke() applied a default
    // privately, the question would have been about a different call from the
    // one that executed.
    BOOST_TEST(questions[1].arguments == records[0].query.arguments);
}

// ---- every tool, through the registry ---------------------------------------

BOOST_AUTO_TEST_CASE(a_quick_command_finishes_inside_its_spawn_window)
{
    // The optional behavior that makes one tool serve two jobs: a window long
    // enough for the command, and the answer arrives complete — exit code,
    // output, and the statement that the capture behind them is finished.
    Fixture f;
    const auto record = f.call(call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{{"executable", "echo"},
                       {"arguments", nlohmann::json::array({"done", "already"})},
                       {"expected_runtime_milliseconds", 5000}},
        "call_spawn"));

    const nlohmann::json payload = payload_of(record);
    BOOST_TEST(payload.at("finished") == nlohmann::json(true));
    BOOST_TEST(payload.at("output_complete") == nlohmann::json(true));
    BOOST_TEST(payload.at("exit_code") == nlohmann::json(0));
    BOOST_TEST(payload.at("stdout_text") == nlohmann::json("done already\n"));

    // The session is kept, so the release is a call of its own — a ReadOnly
    // one, because removing a table entry is this layer's own bookkeeping and
    // nothing outside the host changes.
    const std::string id = payload.at("session_id").get<std::string>();
    const auto released = f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", id}, {"full", true}, {"release", true}},
        "call_release"));
    BOOST_CHECK(released.query.type == model_io::InvokeType::ReadOnly);
    BOOST_TEST(payload_of(released).at("released") == nlohmann::json(true));
}

BOOST_AUTO_TEST_CASE(a_session_is_fed_waited_on_and_read_through_the_registry)
{
    // The workflow the session tools exist for, in registry batches: feed a
    // child, end its input, wait for it, then read the delta and the whole
    // capture, then reap it with a poll.
    Fixture f;
    const std::string id = f.spawn("cat", {});

    BOOST_TEST(!tools::is_error(
        f.call(call_for(std::string(tool_names::kWrite),
                        nlohmann::json{{"session_id", id},
                                       {"input", "through the registry\n"},
                                       {"close_input", true}}))));

    const nlohmann::json waited = payload_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    BOOST_TEST(waited.at("exited") == nlohmann::json(true));
    BOOST_TEST(waited.at("output_complete") == nlohmann::json(true));
    BOOST_TEST(waited.at("timed_out") == nlohmann::json(false));
    BOOST_TEST(waited.at("stdout_text") ==
               nlohmann::json("through the registry\n"));

    // The delta still holds the text — the wait read the capture with
    // `full = true`, which is an observation and consumes nothing — and a
    // second delta is then empty.
    const nlohmann::json delta = payload_of(f.call(call_for(
        std::string(tool_names::kRead), nlohmann::json{{"session_id", id}})));
    BOOST_TEST(delta.at("stdout_text") ==
               nlohmann::json("through the registry\n"));
    const nlohmann::json empty = payload_of(f.call(call_for(
        std::string(tool_names::kRead), nlohmann::json{{"session_id", id}})));
    BOOST_TEST(empty.at("stdout_text") == nlohmann::json(""));

    // Reaping through a poll: the session goes, and the poll says which ones
    // went. `release_exited` makes that batch a SerialWrite, which is why the
    // removal cannot race a neighbour addressing the same id.
    const nlohmann::json polled = payload_of(f.call(call_for(
        std::string(tool_names::kPoll),
        nlohmann::json{{"release_exited", true}}, "call_poll")));
    BOOST_TEST(polled.at("released") == nlohmann::json::array({id}));
    BOOST_TEST(polled.at("retained_session_count") == nlohmann::json(0));
}

BOOST_AUTO_TEST_CASE(a_graceful_kill_ends_a_child_and_leaves_it_readable)
{
    Fixture f;
    const std::string id = f.spawn("sleep", {"30"});

    const nlohmann::json killed = payload_of(f.call(call_for(
        std::string(tool_names::kKill),
        nlohmann::json{{"session_id", id}, {"graceful", true}}, "call_kill")));
    BOOST_TEST(killed.at("graceful") == nlohmann::json(true));
    BOOST_TEST(killed.at("signalled") == nlohmann::json(true));

    const nlohmann::json waited = payload_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    BOOST_TEST(waited.at("exited") == nlohmann::json(true));
    // sleep does not catch SIGTERM, so it dies of the signal — the difference
    // between `graceful` and the default, which reports 9.
    BOOST_TEST(waited.at("exit_code") == nlohmann::json(15));
    // The session survives the kill, so its output is still collectable.
    BOOST_TEST(!tools::is_error(f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", id}, {"full", true}}))));
}

BOOST_AUTO_TEST_CASE(a_zero_window_hands_back_a_live_session)
{
    // The other half of the spawn's contract: no initial wait means the answer
    // is the id, and the child is a session the following turns can drive.
    Fixture f;
    const std::string id = f.spawn("sleep", {"30"});

    const nlohmann::json polled = payload_of(f.call(call_for(
        std::string(tool_names::kPoll),
        nlohmann::json{{"session_ids", nlohmann::json::array({id})},
                       // Non-consuming, so this poll is ReadOnly — the shape a
                       // batch may run beside anything.
                       {"include_output", false}})));
    BOOST_TEST_REQUIRE(polled.at("sessions").size() == std::size_t{1});
    BOOST_TEST(polled.at("sessions")[0].at("state") == nlohmann::json("running"));
    BOOST_TEST(!polled.at("sessions")[0].contains("new_stdout"));

    const nlohmann::json killed = payload_of(f.call(call_for(
        std::string(tool_names::kKill), nlohmann::json{{"session_id", id}})));
    BOOST_TEST(killed.at("signalled") == nlohmann::json(true));
    const nlohmann::json waited = payload_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    BOOST_TEST(waited.at("exit_code") == nlohmann::json(9));
}

// ---- mixed batches -----------------------------------------------------------

BOOST_AUTO_TEST_CASE(a_serial_call_runs_before_the_observing_ones_in_its_batch)
{
    // The observable consequence of "serial first" (tools/registry.hpp, step 2),
    // and the reason it is a rule rather than an optimization: the poll in this
    // batch is ReadOnly, so it is allowed to run alongside its neighbours — but
    // the spawn is SerialWrite, so the whole batch waits for it. Which means the
    // poll MUST see the session the spawn just created. If the two ran
    // together, or the poll ran first, it would report an empty table.
    Fixture f;
    const std::vector<model_io::InvokeReturn> records =
        f.batch(std::vector<model_io::InvokeQuery>{
            call_for(std::string(tool_names::kSpawn),
                     nlohmann::json{{"executable", "cat"},
                                    {"expected_runtime_milliseconds", 0}},
                     "call_spawn"),
            call_for(std::string(tool_names::kPoll),
                     nlohmann::json{{"include_output", false}}, "call_poll"),
        });
    BOOST_TEST_REQUIRE(records.size() == std::size_t{2});
    BOOST_CHECK(records[0].query.type == model_io::InvokeType::SerialWrite);
    BOOST_CHECK(records[1].query.type == model_io::InvokeType::ReadOnly);

    const std::string id =
        payload_of(records[0]).at("session_id").get<std::string>();
    const nlohmann::json polled = payload_of(records[1]);
    BOOST_TEST_REQUIRE(polled.at("sessions").size() == std::size_t{1});
    BOOST_TEST(polled.at("sessions")[0].at("session_id") == nlohmann::json(id));
}

BOOST_AUTO_TEST_CASE(two_delta_reads_of_one_session_split_the_output_between_them)
{
    // Two delta reads of one session in one batch, and what ReadOnly promises
    // here: SAFETY, not a schedule. The call declares no effect outside this
    // host, so the batch runs both at once; the store serialises them on the
    // session's strand, so neither can tear or double-hand a byte — and the
    // session's new output goes to exactly one of them, the other answering
    // "nothing new".
    //
    // Which of the two gets it is NOT asserted, and that is the point of the
    // case: the type says what the call does, not that a batch's result is
    // independent of its interleaving. A model that asks for the same session's
    // new output twice in one turn has asked an ambiguous question, and the
    // store's contract (no loss, no duplication) is what it gets.
    Fixture f;
    const std::string id = f.spawn("echo", {"only-once"});
    (void)f.call(call_for(std::string(tool_names::kWait),
                          nlohmann::json{{"session_id", id},
                                         {"timeout_milliseconds", 5000}}));

    const std::vector<model_io::InvokeReturn> records =
        f.batch(std::vector<model_io::InvokeQuery>{
            call_for(std::string(tool_names::kRead),
                     nlohmann::json{{"session_id", id}}, "call_first"),
            call_for(std::string(tool_names::kRead),
                     nlohmann::json{{"session_id", id}}, "call_second"),
        });
    BOOST_TEST_REQUIRE(records.size() == std::size_t{2});
    BOOST_CHECK(records[0].query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(records[1].query.type == model_io::InvokeType::ReadOnly);

    // The bytes went to one of them, whole, and to exactly one: never split
    // across the two answers, never handed out twice.
    const std::string first =
        payload_of(records[0]).at("stdout_text").get<std::string>();
    const std::string second =
        payload_of(records[1]).at("stdout_text").get<std::string>();
    const std::size_t delivered = (first == "only-once\n" ? 1 : 0) +
                                  (second == "only-once\n" ? 1 : 0);
    BOOST_TEST(delivered == std::size_t{1});
    BOOST_TEST((first.empty() || second.empty()));
    // And the session is still readable afterwards: a delta read consumes only
    // what it handed over.
    const nlohmann::json afterwards = payload_of(f.call(call_for(
        std::string(tool_names::kRead), nlohmann::json{{"session_id", id}})));
    BOOST_TEST(afterwards.at("stdout_text") == nlohmann::json(""));
}

BOOST_AUTO_TEST_CASE(reads_of_two_sessions_in_one_batch_do_not_interfere)
{
    // Independent sessions are the case ReadOnly is FOR: two whole captures,
    // neither consuming anything, running in the same batch on a context with
    // several workers. Each answer must describe its own session.
    Fixture f;
    const std::string first = f.spawn("echo", {"first-output"}, 0, "call_a");
    const std::string second = f.spawn("echo", {"second-output"}, 0, "call_b");

    const std::vector<model_io::InvokeReturn> records =
        f.batch(std::vector<model_io::InvokeQuery>{
            call_for(std::string(tool_names::kRead),
                     nlohmann::json{{"session_id", first}, {"full", true}},
                     "call_read_a"),
            call_for(std::string(tool_names::kRead),
                     nlohmann::json{{"session_id", second}, {"full", true}},
                     "call_read_b"),
        });
    BOOST_TEST_REQUIRE(records.size() == std::size_t{2});
    BOOST_CHECK(records[0].query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(records[1].query.type == model_io::InvokeType::ReadOnly);

    // Both may answer before the children have drained — a full read does not
    // wait for anything — so the assertion is on the CROSSOVER: neither answer
    // carries the other session's text, whichever of the two happened to run
    // first.
    const std::string first_text =
        payload_of(records[0]).at("stdout_text").get<std::string>();
    const std::string second_text =
        payload_of(records[1]).at("stdout_text").get<std::string>();
    BOOST_TEST(first_text.find("second-output") == std::string::npos);
    BOOST_TEST(second_text.find("first-output") == std::string::npos);
    BOOST_TEST(payload_of(records[0]).at("session_id") ==
               nlohmann::json(first));
    BOOST_TEST(payload_of(records[1]).at("session_id") ==
               nlohmann::json(second));

    // And after the drains have finished, each session holds its own output —
    // proof that the batch's two readers did not mix them up.
    (void)f.call(call_for(std::string(tool_names::kWait),
                          nlohmann::json{{"session_id", first},
                                         {"timeout_milliseconds", 5000}}));
    (void)f.call(call_for(std::string(tool_names::kWait),
                          nlohmann::json{{"session_id", second},
                                         {"timeout_milliseconds", 5000}}));
    const nlohmann::json delta_a = payload_of(f.call(call_for(
        std::string(tool_names::kRead), nlohmann::json{{"session_id", first}})));
    const nlohmann::json delta_b = payload_of(f.call(call_for(
        std::string(tool_names::kRead), nlohmann::json{{"session_id", second}})));
    BOOST_TEST(delta_a.at("stdout_text") == nlohmann::json("first-output\n"));
    BOOST_TEST(delta_b.at("stdout_text") == nlohmann::json("second-output\n"));
}

BOOST_AUTO_TEST_CASE(read_only_calls_in_one_batch_consume_nothing)
{
    // What ReadOnly has to MEAN for this toolset to be allowed to declare it:
    // the calls that run together leave everything as they found it. Two full
    // reads and a non-consuming poll overlap here, and afterwards the session's
    // delta cursor must be exactly where it was — otherwise the batch stole
    // output from the call that comes next.
    Fixture f;
    const std::string id = f.spawn("echo", {"kept"}, 5000);
    const nlohmann::json spawned = payload_of(f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", id}, {"full", true}})));
    BOOST_TEST(spawned.at("stdout_text") == nlohmann::json("kept\n"));

    const std::vector<model_io::InvokeReturn> records =
        f.batch(std::vector<model_io::InvokeQuery>{
            call_for(std::string(tool_names::kRead),
                     nlohmann::json{{"session_id", id}, {"full", true}},
                     "call_full_a"),
            call_for(std::string(tool_names::kRead),
                     nlohmann::json{{"session_id", id}, {"full", true}},
                     "call_full_b"),
            call_for(std::string(tool_names::kPoll),
                     nlohmann::json{{"include_output", false}}, "call_poll"),
        });
    for (const model_io::InvokeReturn& record : records) {
        // Every one of them is the parallelisable kind — that is the premise of
        // the case, and the registry groups by exactly this.
        BOOST_CHECK(record.query.type == model_io::InvokeType::ReadOnly);
        BOOST_TEST(!tools::is_error(record));
    }
    // The non-consuming poll reports no output at all, which is the difference
    // between this call and the consuming one.
    const nlohmann::json polled = payload_of(records[2]);
    BOOST_TEST_REQUIRE(polled.at("sessions").size() == std::size_t{1});
    BOOST_TEST(!polled.at("sessions")[0].contains("new_stdout"));

    // The proof: the delta is still untouched after three overlapping readers.
    const nlohmann::json delta = payload_of(f.call(call_for(
        std::string(tool_names::kRead), nlohmann::json{{"session_id", id}})));
    BOOST_TEST(delta.at("stdout_text") == nlohmann::json("kept\n"));
}

BOOST_AUTO_TEST_CASE(wait_and_poll_on_one_session_in_one_batch)
{
    // A wait and a non-consuming poll address the same session and are both
    // ReadOnly, so they overlap by design: waiting observes, and this poll
    // consumes nothing. The wait's deadline is what makes it a bounded
    // neighbour — an unbounded wait would hold a parallel branch forever.
    Fixture f;
    const std::string id = f.spawn("sleep", {"30"});

    const std::vector<model_io::InvokeReturn> records =
        f.batch(std::vector<model_io::InvokeQuery>{
            call_for(std::string(tool_names::kWait),
                     nlohmann::json{{"session_id", id},
                                    {"timeout_milliseconds", 300}},
                     "call_wait"),
            call_for(std::string(tool_names::kPoll),
                     nlohmann::json{{"session_ids",
                                     nlohmann::json::array({id})},
                                    {"include_output", false}},
                     "call_poll"),
        });
    BOOST_TEST_REQUIRE(records.size() == std::size_t{2});
    BOOST_CHECK(records[0].query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(records[1].query.type == model_io::InvokeType::ReadOnly);

    // A timeout is a result, not a failure: the child is still running.
    const nlohmann::json waited = payload_of(records[0]);
    BOOST_TEST(waited.at("exited") == nlohmann::json(false));
    BOOST_TEST(waited.at("timed_out") == nlohmann::json(true));
    // And the poll beside it saw the same running child.
    BOOST_TEST_REQUIRE(payload_of(records[1]).at("sessions").size() ==
                       std::size_t{1});
    BOOST_TEST(payload_of(records[1]).at("sessions")[0].at("state") ==
               nlohmann::json("running"));
}

BOOST_AUTO_TEST_CASE(kill_wait_and_read_in_one_batch)
{
    // A batch that reads like an agent's own plan: end the child, collect how it
    // ended, and read what it printed. The kill is SerialWrite, so it runs while
    // the rest of the batch waits — which is exactly why the wait that follows
    // it in the same batch can already see the exit instead of timing out.
    Fixture f;
    const std::string id = f.spawn("sh", {"-c", "printf 'before the signal\\n'; sleep 30"});
    (void)f.call(call_for(std::string(tool_names::kWait),
                          nlohmann::json{{"session_id", id},
                                         {"timeout_milliseconds", 300}}));

    const std::vector<model_io::InvokeReturn> records =
        f.batch(std::vector<model_io::InvokeQuery>{
            call_for(std::string(tool_names::kKill),
                     nlohmann::json{{"session_id", id}}, "call_kill"),
            call_for(std::string(tool_names::kWait),
                     nlohmann::json{{"session_id", id},
                                    {"timeout_milliseconds", 5000}},
                     "call_wait"),
            call_for(std::string(tool_names::kRead),
                     nlohmann::json{{"session_id", id}, {"full", true}},
                     "call_read"),
        });
    BOOST_TEST_REQUIRE(records.size() == std::size_t{3});
    BOOST_CHECK(records[0].query.type == model_io::InvokeType::SerialWrite);
    BOOST_CHECK(records[1].query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(records[2].query.type == model_io::InvokeType::ReadOnly);

    BOOST_TEST(payload_of(records[0]).at("signalled") == nlohmann::json(true));
    const nlohmann::json waited = payload_of(records[1]);
    BOOST_TEST(waited.at("exited") == nlohmann::json(true));
    BOOST_TEST(waited.at("exit_code") == nlohmann::json(9));
    BOOST_TEST(waited.at("stdout_text") ==
               nlohmann::json("before the signal\n"));
    // The read ran beside the wait and sees the same capture.
    BOOST_TEST(payload_of(records[2]).at("stdout_text") ==
               nlohmann::json("before the signal\n"));
}

BOOST_AUTO_TEST_CASE(a_release_in_one_batch_leaves_the_table_empty_for_the_next_one)
{
    // `release` removes a session, and the type says what that is: this layer's
    // own bookkeeping, changed by a call that has no effect outside the host,
    // so the call is ReadOnly and may overlap its neighbours. What follows from
    // that is worth pinning, because it is the limit of the rule: a poll BESIDE
    // a release of the same session may or may not see it (both are ReadOnly
    // and both run in the parallel phase), while the release's own effect is
    // not in doubt — the session is gone for every call after the batch.
    Fixture f;
    const std::string id = f.spawn("echo", {"gone"}, 5000);

    const std::vector<model_io::InvokeReturn> records =
        f.batch(std::vector<model_io::InvokeQuery>{
            call_for(std::string(tool_names::kRead),
                     nlohmann::json{{"session_id", id}, {"full", true},
                                    {"release", true}},
                     "call_release"),
            call_for(std::string(tool_names::kPoll),
                     nlohmann::json{{"include_output", false}}, "call_poll"),
        });
    BOOST_TEST_REQUIRE(records.size() == std::size_t{2});
    BOOST_CHECK(records[0].query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(records[1].query.type == model_io::InvokeType::ReadOnly);

    // The read answered for its own session, with its whole capture, and the
    // removal it asked for happened.
    const nlohmann::json released = payload_of(records[0]);
    BOOST_TEST(released.at("released") == nlohmann::json(true));
    BOOST_TEST(released.at("stdout_text") == nlohmann::json("gone\n"));
    // The poll beside it answered honestly either way — a session it saw, or
    // none — but never a half-removed one.
    const nlohmann::json polled = payload_of(records[1]);
    BOOST_TEST(polled.at("retained_session_count").get<int>() <= 1);
    BOOST_TEST(polled.at("sessions").size() ==
               static_cast<std::size_t>(
                   polled.at("retained_session_count").get<int>()));

    // The next batch sees a table the release has already emptied: the effect
    // is not "may or may not", it is "done by the time this batch answers".
    const nlohmann::json afterwards = payload_of(f.call(call_for(
        std::string(tool_names::kPoll),
        nlohmann::json{{"include_output", false}}, "call_after")));
    BOOST_TEST(afterwards.at("sessions") == nlohmann::json::array());
    BOOST_TEST(afterwards.at("retained_session_count") == nlohmann::json(0));
}

BOOST_AUTO_TEST_CASE(stdin_writes_and_a_close_keep_their_order_in_one_batch)
{
    // Two writes and a close in one batch: all three are SerialWrite, so the
    // batch runs them in call order — and that order is what the child sees. A
    // ParallWrite declaration here would let the close overtake a write, which
    // is a different program, not a faster one.
    Fixture f;
    const std::string id = f.spawn("cat", {});

    const std::vector<model_io::InvokeReturn> records =
        f.batch(std::vector<model_io::InvokeQuery>{
            call_for(std::string(tool_names::kWrite),
                     nlohmann::json{{"session_id", id}, {"input", "first\n"}},
                     "call_write_a"),
            call_for(std::string(tool_names::kWrite),
                     nlohmann::json{{"session_id", id}, {"input", "second\n"},
                                    {"close_input", true}},
                     "call_write_b"),
        });
    BOOST_TEST_REQUIRE(records.size() == std::size_t{2});
    for (const model_io::InvokeReturn& record : records) {
        BOOST_CHECK(record.query.type == model_io::InvokeType::SerialWrite);
        BOOST_TEST(!tools::is_error(record));
    }

    const nlohmann::json waited = payload_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    BOOST_TEST(waited.at("exited") == nlohmann::json(true));
    BOOST_TEST(waited.at("stdout_text") ==
               nlohmann::json("first\nsecond\n"));
}

// ---- scheduling, asserted rather than inferred -------------------------------

BOOST_AUTO_TEST_CASE(a_serial_write_never_overlaps_its_neighbours)
{
    // What SerialWrite promises, measured: the two serial probes must not
    // overlap each other, and nothing else in the batch may overlap them either
    // — including the ReadOnly probe, which would otherwise be free to run
    // beside them.
    Fixture f;
    const std::vector<model_io::InvokeReturn> records =
        f.batch(std::vector<model_io::InvokeQuery>{
            call_for(std::string(Fixture::kSerialProbeA), {}, "call_a"),
            call_for(std::string(Fixture::kReadOnlyProbeA), {}, "call_b"),
            call_for(std::string(Fixture::kSerialProbeB), {}, "call_c"),
        });
    BOOST_TEST_REQUIRE(records.size() == std::size_t{3});
    for (const model_io::InvokeReturn& record : records) {
        BOOST_TEST(!tools::is_error(record));
    }

    // Never more than one at a time, whatever the executor had to offer.
    BOOST_TEST(f.probes->peak_concurrency() == 1);
    // And in batch order, which is the other half of the rule: `c` is the second
    // serial call and runs after `a`, while `b` — the parallel one — waits for
    // both.
    const std::vector<std::string> events = f.probes->sequence();
    const std::vector<std::string> expected{
        "start " + std::string(Fixture::kSerialProbeA),
        "end " + std::string(Fixture::kSerialProbeA),
        "start " + std::string(Fixture::kSerialProbeB),
        "end " + std::string(Fixture::kSerialProbeB),
        "start " + std::string(Fixture::kReadOnlyProbeA),
        "end " + std::string(Fixture::kReadOnlyProbeA),
    };
    BOOST_TEST(events == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(read_only_and_parallel_write_calls_are_allowed_to_overlap)
{
    // The other half of the scheduler's contract, and the reason the wrong
    // ReadOnly is a data race rather than a slowdown: ReadOnly and ParallWrite
    // calls are started together and run concurrently. Three probes, three
    // workers, all three overlapping — asserted on the logical overlap, not on
    // which thread ran what.
    Fixture f;
    const std::vector<model_io::InvokeReturn> records =
        f.batch(std::vector<model_io::InvokeQuery>{
            call_for(std::string(Fixture::kReadOnlyProbeA), {}, "call_a"),
            call_for(std::string(Fixture::kReadOnlyProbeB), {}, "call_b"),
            call_for(std::string(Fixture::kParallelProbe), {}, "call_c"),
        });
    BOOST_TEST_REQUIRE(records.size() == std::size_t{3});
    for (const model_io::InvokeReturn& record : records) {
        BOOST_TEST(!tools::is_error(record));
    }
    BOOST_TEST(f.probes->peak_concurrency() == 3);
}

BOOST_AUTO_TEST_CASE(a_batch_mixing_process_calls_and_probes_keeps_both_rules)
{
    // A mixed batch, which is what an agent turn really is: process calls and
    // probes go through the same two-phase schedule, and neither kind disturbs
    // the other's classification. The probes are the only instrument here that
    // can see OVERLAP — a process call records nothing — so they come in pairs
    // and the process calls are asserted on their results.
    Fixture f;
    const std::string id = f.spawn("echo", {"mixed"}, 5000);

    const std::vector<model_io::InvokeReturn> records =
        f.batch(std::vector<model_io::InvokeQuery>{
            // A consuming poll and everything else in this batch are ReadOnly —
            // the poll's cursor work is this layer's own state — so this whole
            // batch runs in the parallel phase, together.
            call_for(std::string(tool_names::kPoll),
                     nlohmann::json{{"session_id", id},
                                    {"include_output", true}},
                     "call_poll"),
            call_for(std::string(Fixture::kReadOnlyProbeA), {}, "call_probe_a"),
            call_for(std::string(Fixture::kReadOnlyProbeB), {}, "call_probe_b"),
            call_for(std::string(tool_names::kRead),
                     nlohmann::json{{"session_id", id}, {"full", true}},
                     "call_read"),
        });
    BOOST_TEST_REQUIRE(records.size() == std::size_t{4});
    for (const model_io::InvokeReturn& record : records) {
        BOOST_CHECK(record.query.type == model_io::InvokeType::ReadOnly);
    }

    // The poll took the session's new output, and the full read beside it still
    // sees the whole capture: `full` consumes nothing, so the two do not fight
    // over the same bytes even though they ran together.
    BOOST_TEST(payload_of(records[0]).at("sessions")[0].at("new_stdout") ==
               nlohmann::json("mixed\n"));
    // The full read in the parallel phase still sees the whole capture, which is
    // what `full` promises against a neighbour that consumes.
    BOOST_TEST(payload_of(records[3]).at("stdout_text") ==
               nlohmann::json("mixed\n"));
    // And the two probes did overlap: the batch's parallel phase really did
    // start them together, with the process call running beside them.
    BOOST_TEST(f.probes->peak_concurrency() == 2);
}

// ---- repetition ---------------------------------------------------------------

BOOST_AUTO_TEST_CASE(repeated_turns_keep_their_records_and_their_sessions)
{
    // Scheduler-sensitive regressions are the kind that pass a single run. This
    // case repeats a whole small turn — spawn, three overlapping observations,
    // wait, read, release — enough times that an ordering mistake or a strand
    // hazard has somewhere to show up, and asserts the invariants that must
    // hold on EVERY pass: one record per call, every call answered, every
    // session accounted for, and nothing left in the table at the end.
    //
    // The batch in the middle is the interesting one: a poll that consumes
    // nothing, a full read, and a wait are all ReadOnly, so the registry starts
    // all three together on a context with three workers, every iteration.
    Fixture f;
    constexpr int kTurns = 50;

    for (int turn = 0; turn < kTurns; ++turn) {
        BOOST_TEST_CONTEXT("turn " << turn) {
            const std::string spawn_call = std::format("call_{}_spawn", turn);
            const auto spawned = f.call(call_for(
                std::string(tool_names::kSpawn),
                nlohmann::json{{"executable", "echo"},
                               {"arguments", nlohmann::json::array({"turn"})},
                               {"expected_runtime_milliseconds", 0}},
                spawn_call));
            const std::string id =
                payload_of(spawned).at("session_id").get<std::string>();

            const std::vector<model_io::InvokeReturn> records =
                f.batch(std::vector<model_io::InvokeQuery>{
                    call_for(std::string(tool_names::kPoll),
                             nlohmann::json{{"session_ids",
                                             nlohmann::json::array({id})},
                                            {"include_output", false}},
                             std::format("call_{}_poll", turn)),
                    call_for(std::string(tool_names::kRead),
                             nlohmann::json{{"session_id", id}, {"full", true}},
                             std::format("call_{}_read", turn)),
                    call_for(std::string(tool_names::kWait),
                             nlohmann::json{{"session_id", id},
                                            {"timeout_milliseconds", 5000}},
                             std::format("call_{}_wait", turn)),
                });
            BOOST_TEST_REQUIRE(records.size() == std::size_t{3});
            for (std::size_t index = 0; index < records.size(); ++index) {
                // Every call answered, by the call at its own position.
                BOOST_TEST(!tools::is_error(records[index]));
                BOOST_TEST(records[index].query.id ==
                           std::format("call_{}_{}", turn,
                                       index == 0   ? "poll"
                                       : index == 1 ? "read"
                                                    : "wait"));
                BOOST_CHECK(records[index].query.type ==
                            model_io::InvokeType::ReadOnly);
            }
            // The wait is the one that cannot return before the capture is
            // complete, so it is the one the output is asserted on.
            BOOST_TEST(payload_of(records[2]).at("exited") ==
                       nlohmann::json(true));
            BOOST_TEST(payload_of(records[2]).at("output_complete") ==
                       nlohmann::json(true));
            BOOST_TEST(payload_of(records[2]).at("stdout_text") ==
                       nlohmann::json("turn\n"));

            // Reap it, so the table is empty at the end of every turn and the
            // session cap can never be what fails a later iteration.
            const auto released = f.call(call_for(
                std::string(tool_names::kRead),
                nlohmann::json{{"session_id", id}, {"full", true},
                               {"release", true}},
                std::format("call_{}_release", turn)));
            BOOST_TEST(payload_of(released).at("released") ==
                       nlohmann::json(true));
        }
    }

    // Nothing is left behind: every turn released what it spawned, so the table
    // is empty and no child is running.
    const nlohmann::json polled = payload_of(
        f.call(call_for(std::string(tool_names::kPoll), {}, "call_final")));
    BOOST_TEST(polled.at("sessions") == nlohmann::json::array());
    BOOST_TEST(polled.at("retained_session_count") == nlohmann::json(0));
}
