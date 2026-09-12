#define BOOST_TEST_MODULE ToolRegistryTests
#include <boost/test/unit_test.hpp>

#include "tools/registry.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// The registry: the host's routing table over tool sets, and the batch it runs
// a model turn's tool calls as. Everything here is in-process — no sockets, no
// child processes — and every batch runs on a private io_context, so the cases
// are independent of each other and of the order they run in.
//
// Three things this suite is deliberately shaped around:
//
//   - NO HANG. The failure mode a collector has is waiting for a report no
//     branch will ever send, and a suite that hangs reports nothing. Every batch
//     goes through a watchdog that turns "never returned" into a failed
//     assertion.
//   - ONE ANSWER PER CALL. The provider rejects a turn whose tool_calls are not
//     each answered exactly once, so the batch's size and order are asserted,
//     not just the records' contents.
//   - RECORDS, NOT THROWS, for anything a tool or a set does wrong: the layer's
//     whole job is that a bad call costs its own call and nothing else.

namespace asio = boost::asio;
using tools::InvokeException;
using tools::ToolRegistry;

namespace {

/// The order of a batch, made observable: one entry per checkpoint that ran,
/// spelled "<tool>:<call id>:<step>". Appends are locked, because the
/// thread-pool case writes from four threads at once.
class Trace {
public:
    void note(std::string text)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        const std::thread::id writer = std::this_thread::get_id();
        if (std::find(_threads.begin(), _threads.end(), writer) == _threads.end()) {
            _threads.push_back(writer);
        }
        _entries.push_back(std::move(text));
    }

    /// Where an entry sits in the order, or -1 when it never happened.
    [[nodiscard]] int index_of(std::string_view wanted) const
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        for (std::size_t index = 0; index < _entries.size(); ++index) {
            if (_entries[index] == wanted) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    [[nodiscard]] std::size_t size() const
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _entries.size();
    }

    /// The threads that wrote to the trace, in the order they first did — which
    /// is how a batch says where it ran.
    [[nodiscard]] std::vector<std::thread::id> threads() const
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _threads;
    }

private:
    mutable std::mutex _mutex;
    std::vector<std::string> _entries;
    std::vector<std::thread::id> _threads;
};

using TracePtr = std::shared_ptr<Trace>;

/// One trace entry: which tool, which call, which checkpoint.
std::string entry(std::string_view tool, std::string_view id, std::string_view step)
{
    return std::format("{}:{}:{}", tool, id, step);
}

/// The position of a trace entry, REQUIRING that it happened at all: comparing
/// against a missing entry would otherwise pass on -1 < n.
int at(const Trace& trace, std::string_view tool, std::string_view id, std::string_view step)
{
    const int position = trace.index_of(entry(tool, id, step));
    BOOST_REQUIRE_MESSAGE(position >= 0, "missing trace entry: " << entry(tool, id, step));
    return position;
}

/// A call as the conversation carries it. The type and security on it are the
/// record's defaults (ReadOnly / DefaultDeny) rather than what the tools below
/// will declare: write_attributes() overwrites both, and every test reads the
/// SETTLED values off the record, so a settle that never ran cannot pass.
model_io::InvokeQuery call_for(std::string name, std::string id)
{
    model_io::InvokeQuery query;
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::DefaultDeny;
    query.id = std::move(id);
    query.name = std::move(name);
    return query;
}

/// A tool whose every checkpoint is scripted, and which writes what it did into
/// a shared trace so a batch's order — and its overlap — is observable.
///
/// Failures are thrown as plain std::exceptions on purpose: a tool that does not
/// classify its own failure is the ordinary case, and it is the SET's job to
/// report it at the checkpoint that was running.
class ScriptedTool final : public tools::ToolInterface {
public:
    /// Which checkpoint fails, when one does.
    enum class Fail {
        None,
        EnsureArguments,
        SecurityCheck,
        Invoke,
        CheckResult
    };

    ScriptedTool(std::string tool_name, TracePtr trace)
        : trace_(std::move(trace))
    {
        details.name = std::move(tool_name);
        details.description = "A scripted tool.";
    }

    model_io::Invocable details;

    Fail fail_in = Fail::None;
    std::string failure = "the checkpoint failed";
    model_io::InvokeType declares_type = model_io::InvokeType::ReadOnly;
    model_io::InvokeSecurity declares_security = model_io::InvokeSecurity::Trusted;
    bool security_passes = true;
    std::string security_reason = "the scripted gate refused the call";
    /// Whether ensure_arguments() fills in an argument a call may omit — the
    /// in-place settling the batch's identity has to survive.
    bool fill_default_argument = false;
    /// How long invoke() waits, so a test can see two calls overlap.
    std::chrono::milliseconds delay{0};
    std::string payload = "the tool ran";

    // Atomics, not ints: the thread-pool case runs these on four threads. Within
    // one thread the only points they change at are suspension points, which is
    // what lets the single-threaded cases read them after the batch returned.
    std::atomic<int> invoke_count{0};
    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};

    const model_io::Invocable& get_details() const noexcept override
    {
        return details;
    }

    void ensure_arguments(model_io::InvokeQuery& query) const override
    {
        note(query, "ensure");
        fail_if(Fail::EnsureArguments);
        if (fill_default_argument && !query.arguments.contains("path")) {
            query.arguments["path"] = "/default/path";
        }
    }

    void write_attributes(model_io::InvokeQuery& query) const override
    {
        note(query, "attributes");
        query.type = declares_type;
        query.security = declares_security;
    }

    boost::asio::awaitable<std::tuple<bool, std::string>> security_check(
        const model_io::InvokeQuery& query) override
    {
        note(query, "security");
        fail_if(Fail::SecurityCheck);
        co_return std::make_tuple(security_passes, security_reason);
    }

    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override
    {
        note(query, "invoke");
        ++invoke_count;
        ++in_flight;
        max_in_flight = std::max(max_in_flight.load(), in_flight.load());

        try {
            fail_if(Fail::Invoke);
            if (delay.count() > 0) {
                asio::steady_timer timer(co_await asio::this_coro::executor, delay);
                co_await timer.async_wait(asio::use_awaitable);
            }
        } catch (...) {
            // The counters stay truthful on the failure paths too.
            --in_flight;
            throw;
        }

        --in_flight;
        note(query, "finished");
        co_return model_io::Content{
            .type = model_io::ContentType::Text, .raw = payload, .extras = {}};
    }

    model_io::InvokeReturn check_result(
        model_io::InvokeQuery query, model_io::Content output) const override
    {
        note(query, "result");
        fail_if(Fail::CheckResult);
        return tools::ToolInterface::check_result(std::move(query), std::move(output));
    }

private:
    void note(const model_io::InvokeQuery& query, std::string_view step) const
    {
        if (trace_ != nullptr) {
            trace_->note(entry(details.name, query.id, step));
        }
    }

    void fail_if(Fail checkpoint) const
    {
        if (fail_in == checkpoint) {
            throw std::runtime_error(failure);
        }
    }

    TracePtr trace_;
};

/// A set of several tools that resolves by name, and whose documented contracts
/// can be broken on purpose: a set that throws where it promised it would not is
/// exactly what the registry has to turn into a record instead of an exception
/// through the batch.
class ScriptedSet final : public tools::ToolSet {
public:
    ScriptedSet(std::string set_name, std::vector<ToolHandle> tools)
        : name_(std::move(set_name)), tools_(std::move(tools))
    {}

    std::string_view name() const noexcept override { return name_; }

    std::vector<model_io::Invocable> get_tools() const override
    {
        std::vector<model_io::Invocable> offered;
        for (const ToolHandle& tool : tools_) {
            offered.push_back(tool->get_details());
        }
        return offered;
    }

    ToolHandle dispatch(const model_io::InvokeQuery& query) const override
    {
        for (const ToolHandle& tool : tools_) {
            if (tool->get_details().name == query.name) {
                return tool;
            }
        }
        return nullptr; // which prepare() reports as a Dispatch failure
    }

    /// Breaks prepare()'s "ALWAYS throws InvokeException" promise.
    bool prepare_throws = false;
    /// Breaks execute()'s "never throws" promise.
    bool execute_throws = false;
    /// Answers for a call of the set's own making instead of the one it was
    /// given (a retry, a nested invocation, a batching relay).
    bool answer_for_another_call = false;
    std::string broken_how = "the set broke its own contract";

    [[nodiscard]] ToolHandle prepare(model_io::InvokeQuery& query) override
    {
        if (prepare_throws) {
            throw std::runtime_error(broken_how);
        }
        return ToolSet::prepare(query);
    }

    [[nodiscard]] boost::asio::awaitable<model_io::InvokeReturn> execute(
        ToolHandle tool, model_io::InvokeQuery query) override
    {
        if (execute_throws) {
            throw std::runtime_error(broken_how);
        }
        model_io::InvokeReturn record =
            co_await ToolSet::execute(std::move(tool), query);
        if (answer_for_another_call) {
            record.query.id = "inner_call";
        }
        co_return record;
    }

private:
    std::string name_;
    std::vector<ToolHandle> tools_;
};

/// What one batch produced, and whether it produced it at all.
struct BatchRun {
    ToolRegistry::Results results;
    bool returned = false;
};

/// Drive one batch to completion on a private context, with a watchdog.
///
/// The watchdog is the point: the failure mode this layer must never have is a
/// collector suspended on a report no branch will ever send, and a hung suite
/// reports nothing at all. A batch that does not return inside the limit stops
/// the context instead, and `returned` says which happened.
///
/// `start` is how the batch is started — the explicit executor form or the
/// convenience form — so the two differ only in the call they make and share
/// everything else about how the test runs them.
template <typename Start>
BatchRun run_watched(Start&& start,
                     std::chrono::milliseconds limit = std::chrono::seconds(5))
{
    asio::io_context io;
    BatchRun run;

    asio::co_spawn(io,
        [&run, &io, &start]() -> asio::awaitable<void> {
            run.results = co_await start(io);
            run.returned = true;
            io.stop();
        },
        asio::detached);

    asio::steady_timer watchdog(io, limit);
    watchdog.async_wait([&](const boost::system::error_code& error) {
        if (!error) io.stop();
    });

    io.run();
    return run;
}

/// One batch on a context of its own, on an executor the test names.
BatchRun run_batch(const ToolRegistry& registry,
                   std::vector<model_io::InvokeQuery> batch,
                   std::chrono::milliseconds limit = std::chrono::seconds(5))
{
    return run_watched(
        [&registry, &batch](asio::io_context& io) {
            return registry.execute(std::move(batch), io.get_executor());
        },
        limit);
}

/// One batch through the convenience form, which takes no executor: the batch
/// must land on the caller's own, here the context this helper is running.
BatchRun run_batch_on_the_callers_executor(
    const ToolRegistry& registry,
    std::vector<model_io::InvokeQuery> batch,
    std::chrono::milliseconds limit = std::chrono::seconds(5))
{
    return run_watched(
        [&registry, &batch](asio::io_context&) {
            return registry.execute(std::move(batch));
        },
        limit);
}

/// The same, on a context driven by four threads — the case that makes the
/// report channel a CONCURRENT channel: the parallel branches share the raw
/// executor with no strand between them, so several of them report at once.
/// Reaching every record is the property this pins.
///
/// `batches` is a vector of batches rather than one batch, because a host may
/// have more than one model turn in flight: routing is read-only, and two
/// batches going through one registry at once is part of the contract.
std::vector<BatchRun> run_batches_on_threads(
    const ToolRegistry& registry,
    std::vector<std::vector<model_io::InvokeQuery>> batches,
    unsigned int threads = 4,
    std::chrono::milliseconds limit = std::chrono::seconds(5))
{
    asio::io_context io;
    std::vector<BatchRun> runs(batches.size());
    // Keeps every worker inside run() until the batches are done, instead of
    // letting a thread that momentarily runs out of work return early.
    auto work = asio::make_work_guard(io);
    asio::steady_timer watchdog(io, limit);
    std::atomic<std::size_t> running{batches.size()};

    for (std::size_t index = 0; index < batches.size(); ++index) {
        asio::co_spawn(io,
            [&, index]() -> asio::awaitable<void> {
                runs[index].results =
                    co_await registry.execute(std::move(batches[index]), io.get_executor());
                runs[index].returned = true;
                if (--running == 0) {
                    // Cancelled, not merely left behind: a pending timer is
                    // outstanding work of its own, and the workers below would
                    // sit in run() until it expired — which is how this case
                    // first took the whole watchdog limit.
                    watchdog.cancel();
                    work.reset();
                }
            },
            asio::detached);
    }

    watchdog.async_wait([&](const boost::system::error_code& error) {
        if (!error) {
            // A batch still suspended holds the context busy by itself, so
            // releasing the guard is not enough to get the workers out.
            work.reset();
            io.stop();
        }
    });

    std::vector<std::thread> pool;
    for (unsigned int index = 1; index < threads; ++index) {
        pool.emplace_back([&io] { io.run(); });
    }
    io.run();
    for (std::thread& worker : pool) {
        worker.join();
    }
    return runs;
}

/// One batch on the same four-thread context.
BatchRun run_batch_on_threads(const ToolRegistry& registry,
                              std::vector<model_io::InvokeQuery> batch,
                              unsigned int threads = 4,
                              std::chrono::milliseconds limit = std::chrono::seconds(5))
{
    std::vector<std::vector<model_io::InvokeQuery>> batches;
    batches.push_back(std::move(batch));
    std::vector<BatchRun> runs =
        run_batches_on_threads(registry, std::move(batches), threads, limit);
    return std::move(runs.front());
}

} // namespace

// ===== configuration =========================================================

BOOST_AUTO_TEST_CASE(add_registers_every_name_a_set_offers)
{
    auto trace = std::make_shared<Trace>();
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    auto writer = std::make_shared<ScriptedTool>("write_file", trace);
    auto set = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{reader, writer});

    ToolRegistry registry;
    registry.add(set);

    // Sets, not tools: the set is what was registered.
    BOOST_CHECK_EQUAL(registry.size(), 1u);
    BOOST_TEST(!registry.empty());

    BOOST_CHECK(registry.contains("read_file"));
    BOOST_CHECK(registry.contains("write_file"));
    BOOST_CHECK(!registry.contains("rm_rf"));
    BOOST_CHECK(registry.find("read_file") == set);
    BOOST_CHECK(registry.find("rm_rf") == nullptr);
    BOOST_CHECK(registry.get_registered().front() == set);

    // The flattened catalogue — every set's tools, set by set in registration
    // order — is what a request builder hands the model.
    const std::vector<model_io::Invocable> catalogue = registry.get_tools();
    BOOST_REQUIRE_EQUAL(catalogue.size(), 2u);
    BOOST_TEST(catalogue[0].name == "read_file");
    BOOST_TEST(catalogue[1].name == "write_file");
    BOOST_TEST(catalogue[0].description == "A scripted tool.");

    const std::vector<std::string> names = registry.supported_names();
    BOOST_REQUIRE_EQUAL(names.size(), 2u);
    BOOST_TEST(names[0] == "read_file");
    BOOST_TEST(names[1] == "write_file");
}

BOOST_AUTO_TEST_CASE(add_refuses_a_second_set_that_wants_the_same_name)
{
    auto trace = std::make_shared<Trace>();
    auto first = std::make_shared<ScriptedSet>("first_tools",
        std::vector<tools::ToolSet::ToolHandle>{
            std::make_shared<ScriptedTool>("read_file", trace)});
    auto second = std::make_shared<ScriptedSet>("second_tools",
        std::vector<tools::ToolSet::ToolHandle>{
            std::make_shared<ScriptedTool>("read_file", trace)});

    ToolRegistry registry;
    registry.add(first);

    // A call that resolved to two tools would have no defined answer, so the
    // conflict is refused where it happens — at registration, at startup — and
    // not discovered per call.
    BOOST_CHECK_THROW(registry.add(second), std::runtime_error);

    // The refused set left NOTHING behind: the table still holds one set, still
    // routes the name to the first, and does not list the name twice.
    BOOST_CHECK_EQUAL(registry.size(), 1u);
    BOOST_CHECK(registry.find("read_file") == first);
    BOOST_CHECK_EQUAL(registry.supported_names().size(), 1u);

    // The message names both sides of the conflict, which is what makes it
    // fixable without reading the registry's source.
    try {
        registry.add(second);
        BOOST_FAIL("the duplicate name was accepted");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        BOOST_TEST(message.find("read_file") != std::string::npos);
        BOOST_TEST(message.find("first_tools") != std::string::npos);
        BOOST_TEST(message.find("second_tools") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(add_refuses_a_null_set_and_a_nameless_tool)
{
    ToolRegistry registry;

    BOOST_CHECK_THROW(registry.add(nullptr), std::invalid_argument);
    BOOST_TEST(registry.empty());

    auto trace = std::make_shared<Trace>();
    auto nameless = std::make_shared<ScriptedTool>("", trace);
    auto set = std::make_shared<ScriptedSet>(
        "nameless_tools", std::vector<tools::ToolSet::ToolHandle>{nameless});

    // A tool no call could ever name is not a tool the model can reach, and
    // registering it would make the empty query.name routable by accident.
    BOOST_CHECK_THROW(registry.add(set), std::invalid_argument);
    BOOST_TEST(registry.empty());

    const BatchRun run = run_batch(registry, {call_for("", "call_1")});

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 1u);
    BOOST_TEST(tools::is_error(run.results[0]));
    BOOST_CHECK(tools::error_stage(run.results[0]) == InvokeException::Stage::Dispatch);
}

BOOST_AUTO_TEST_CASE(remove_takes_the_whole_set_out_of_the_table)
{
    auto trace = std::make_shared<Trace>();
    auto local = std::make_shared<ScriptedSet>("local_tools",
        std::vector<tools::ToolSet::ToolHandle>{
            std::make_shared<ScriptedTool>("read_file", trace),
            std::make_shared<ScriptedTool>("write_file", trace)});
    auto remote = std::make_shared<ScriptedSet>("remote_tools",
        std::vector<tools::ToolSet::ToolHandle>{
            std::make_shared<ScriptedTool>("fetch_url", trace)});

    ToolRegistry registry;
    registry.add(local);
    registry.add(remote);
    BOOST_REQUIRE_EQUAL(registry.size(), 2u);

    // The unit of registration is the set, so removal is too: taking one name
    // out would leave the set half-advertised under its other names.
    BOOST_TEST(registry.remove("read_file"));
    BOOST_CHECK_EQUAL(registry.size(), 1u);
    BOOST_CHECK(!registry.contains("read_file"));
    BOOST_CHECK(!registry.contains("write_file"));
    BOOST_CHECK(registry.contains("fetch_url")); // the other set is untouched
    BOOST_TEST(!registry.remove("read_file"));   // gone is gone

    const BatchRun run = run_batch(registry, {
        call_for("read_file", "call_1"),
        call_for("fetch_url", "call_2"),
    });

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 2u);
    BOOST_CHECK(tools::error_stage(run.results[0]) == InvokeException::Stage::Dispatch);
    BOOST_TEST(!tools::is_error(run.results[1]));
}

BOOST_AUTO_TEST_CASE(clear_unregisters_everything)
{
    auto trace = std::make_shared<Trace>();
    auto set = std::make_shared<ScriptedSet>("local_tools",
        std::vector<tools::ToolSet::ToolHandle>{
            std::make_shared<ScriptedTool>("read_file", trace)});

    ToolRegistry registry;
    registry.add(set);
    BOOST_REQUIRE(!registry.empty());

    registry.clear();

    BOOST_TEST(registry.empty());
    BOOST_TEST(!registry.contains("read_file"));
    BOOST_CHECK(registry.find("read_file") == nullptr);
    BOOST_TEST(registry.get_tools().empty());
    BOOST_TEST(registry.supported_names().empty());
    // The set itself is only released, not dismantled: the test still holds it.
    BOOST_CHECK_EQUAL(set->supported_names().size(), 1u);
}

// ===== routing ===============================================================

BOOST_AUTO_TEST_CASE(a_batch_is_answered_in_the_order_of_its_calls)
{
    auto trace = std::make_shared<Trace>();
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    reader->fill_default_argument = true; // settles the query in place
    reader->delay = std::chrono::milliseconds(2);
    auto writer = std::make_shared<ScriptedTool>("write_file", trace);
    writer->declares_type = model_io::InvokeType::SerialWrite;
    writer->payload = "written";
    auto set = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{reader, writer});

    ToolRegistry registry;
    registry.add(set);

    const BatchRun run = run_batch(registry, {
        call_for("write_file", "call_w"),
        call_for("read_file", "call_r1"),
        call_for("read_file", "call_r2"),
    });

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 3u);

    // results[i] answers the call at index i, whatever the schedule did.
    BOOST_TEST(run.results[0].query.id == "call_w");
    BOOST_TEST(run.results[1].query.id == "call_r1");
    BOOST_TEST(run.results[2].query.id == "call_r2");
    BOOST_TEST(run.results[0].output.raw == "written");
    BOOST_TEST(run.results[1].output.raw == "the tool ran");
    for (const model_io::InvokeReturn& record : run.results) {
        BOOST_TEST(!tools::is_error(record));
    }

    // The record carries the SETTLED call: the argument ensure_arguments()
    // filled in, and the attributes write_attributes() wrote.
    BOOST_TEST(run.results[1].query.arguments["path"] == "/default/path");
    BOOST_CHECK(run.results[1].query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(run.results[1].query.security == model_io::InvokeSecurity::Trusted);
    BOOST_CHECK(run.results[0].query.type == model_io::InvokeType::SerialWrite);
    BOOST_CHECK_EQUAL(reader->invoke_count.load(), 2);
}

BOOST_AUTO_TEST_CASE(an_unknown_tool_is_a_dispatch_record_and_the_batch_goes_on)
{
    auto trace = std::make_shared<Trace>();
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    auto set = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{reader});

    ToolRegistry registry;
    registry.add(set);

    const BatchRun run = run_batch(registry, {
        call_for("read_file", "call_1"),
        call_for("rm_rf", "call_2"),
        call_for("read_file", "call_3"),
    });

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 3u);

    // The call the table cannot route is answered where it failed — at Dispatch,
    // before any set was involved — in the words the model reads.
    const model_io::InvokeReturn& refused = run.results[1];
    BOOST_TEST(tools::is_error(refused));
    BOOST_CHECK(tools::error_stage(refused) == InvokeException::Stage::Dispatch);
    BOOST_TEST(refused.query.id == "call_2");
    BOOST_TEST(refused.query.name == "rm_rf");
    BOOST_TEST(refused.output.raw ==
               "Failed while dispatching the invocation to a tool: no toolset in "
               "the registry provides a tool named \"rm_rf\" (tool rm_rf; call call_2)");

    // One unrouteable name does not cost the batch its other answers.
    BOOST_TEST(!tools::is_error(run.results[0]));
    BOOST_TEST(!tools::is_error(run.results[2]));
    BOOST_CHECK_EQUAL(reader->invoke_count.load(), 2);
}

BOOST_AUTO_TEST_CASE(calls_that_share_an_identity_are_each_answered)
{
    auto trace = std::make_shared<Trace>();
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    auto set = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{reader});

    ToolRegistry registry;
    registry.add(set);

    // mangled_name() is name and id, so two calls agreeing on both are ONE
    // identity — the most a provider could tell them apart by either. They are
    // still each answered: the assembly is positional, and answering a call with
    // nothing loses a tool message and gets the whole turn rejected.
    const BatchRun run = run_batch(registry, {
        call_for("read_file", "call_1"),
        call_for("read_file", "call_1"),
    });

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 2u);
    BOOST_TEST(run.results[0].query.id == "call_1");
    BOOST_TEST(run.results[1].query.id == "call_1");
    BOOST_TEST(!tools::is_error(run.results[0]));
    BOOST_TEST(!tools::is_error(run.results[1]));
    // Both really ran: the batch executes calls, it does not deduplicate them.
    BOOST_CHECK_EQUAL(reader->invoke_count.load(), 2);

    // The degenerate identity — no name and no id, which mangled_name() reports
    // as the empty string — is answered the same way.
    const BatchRun anonymous = run_batch(registry, {
        call_for("", ""),
        call_for("", ""),
    });

    BOOST_REQUIRE(anonymous.returned);
    BOOST_REQUIRE_EQUAL(anonymous.results.size(), 2u);
    BOOST_CHECK(tools::error_stage(anonymous.results[0]) == InvokeException::Stage::Dispatch);
    BOOST_CHECK(tools::error_stage(anonymous.results[1]) == InvokeException::Stage::Dispatch);
}

BOOST_AUTO_TEST_CASE(an_empty_batch_is_answered_with_nothing)
{
    ToolRegistry registry; // not a single set

    const BatchRun run = run_batch(registry, {});
    BOOST_TEST(run.returned);
    BOOST_TEST(run.results.empty());

    // A registry with nothing in it still answers a call — with the failure that
    // explains itself.
    const BatchRun single = run_batch(registry, {call_for("read_file", "call_1")});
    BOOST_REQUIRE(single.returned);
    BOOST_REQUIRE_EQUAL(single.results.size(), 1u);
    BOOST_CHECK(tools::error_stage(single.results[0]) == InvokeException::Stage::Dispatch);
}

BOOST_AUTO_TEST_CASE(a_refused_call_does_not_stop_the_batch)
{
    auto trace = std::make_shared<Trace>();
    auto guarded = std::make_shared<ScriptedTool>("guarded_write", trace);
    guarded->declares_type = model_io::InvokeType::SerialWrite;
    guarded->fail_in = ScriptedTool::Fail::SecurityCheck;
    guarded->failure = "the human did not confirm the write";
    auto refusing = std::make_shared<ScriptedTool>("refusing_read", trace);
    refusing->security_passes = false;
    refusing->security_reason = "this build has no read path";
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    auto set = std::make_shared<ScriptedSet>("local_tools",
        std::vector<tools::ToolSet::ToolHandle>{guarded, refusing, reader});

    ToolRegistry registry;
    registry.add(set);

    const BatchRun run = run_batch(registry, {
        call_for("guarded_write", "call_w"),
        call_for("refusing_read", "call_r"),
        call_for("read_file", "call_ok"),
    });

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 3u);

    // The TOOL layer's own classification arrives untouched: this layer files
    // records, it does not rewrite their stage. A throwing gate and a refusing
    // gate are the same stage, with each gate's own words.
    BOOST_CHECK(tools::error_stage(run.results[0]) == InvokeException::Stage::SecurityCheck);
    BOOST_TEST(run.results[0].output.raw ==
               "Failed while validating the invocation's security: the human did "
               "not confirm the write (tool guarded_write; call call_w)");
    BOOST_CHECK(tools::error_stage(run.results[1]) == InvokeException::Stage::SecurityCheck);
    BOOST_TEST(run.results[1].output.raw ==
               "Failed while validating the invocation's security: security check "
               "denied: this build has no read path (tool refusing_read; call call_r)");

    // Neither refusal reached its tool, and the call behind them still ran.
    BOOST_CHECK_EQUAL(guarded->invoke_count.load(), 0);
    BOOST_CHECK_EQUAL(refusing->invoke_count.load(), 0);
    BOOST_TEST(!tools::is_error(run.results[2]));
}

// ===== scheduling ============================================================

BOOST_AUTO_TEST_CASE(serial_calls_run_one_at_a_time_before_the_parallel_ones)
{
    auto trace = std::make_shared<Trace>();
    auto writer = std::make_shared<ScriptedTool>("write_file", trace);
    writer->declares_type = model_io::InvokeType::SerialWrite;
    writer->delay = std::chrono::milliseconds(2);
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    reader->delay = std::chrono::milliseconds(2);
    auto set = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{writer, reader});

    ToolRegistry registry;
    registry.add(set);

    // The parallel call is FIRST in the batch and still runs LAST: a batch runs
    // its serial calls with the executor to themselves before it spawns
    // anything, which is what SerialWrite asks for.
    const BatchRun run = run_batch(registry, {
        call_for("read_file", "call_p"),
        call_for("write_file", "call_s1"),
        call_for("write_file", "call_s2"),
    });

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 3u);
    BOOST_TEST(run.results[0].query.id == "call_p"); // still the call order

    // Step 1 is a single pass over the batch, so EVERY call was settled — its
    // ensure_arguments and write_attributes — before any call ran: the last
    // settling checkpoint precedes the first check that needs it. (This is also
    // why the trace's "ensure" entries are all clustered at the front.)
    BOOST_TEST(at(*trace, "read_file", "call_p", "attributes") <
               at(*trace, "write_file", "call_s1", "security"));

    // Two serial calls, one invocation in flight at a time...
    BOOST_CHECK_EQUAL(writer->max_in_flight.load(), 1);
    BOOST_TEST(at(*trace, "write_file", "call_s1", "result") <
               at(*trace, "write_file", "call_s2", "security"));
    // ... and the parallel call had not started when the last serial one ended.
    BOOST_TEST(at(*trace, "write_file", "call_s2", "result") <
               at(*trace, "read_file", "call_p", "invoke"));
}

BOOST_AUTO_TEST_CASE(parallel_calls_really_overlap)
{
    auto trace = std::make_shared<Trace>();
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    reader->delay = std::chrono::milliseconds(5);
    auto set = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{reader});

    ToolRegistry registry;
    registry.add(set);

    const BatchRun run = run_batch(registry, {
        call_for("read_file", "call_1"),
        call_for("read_file", "call_2"),
    });

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 2u);
    BOOST_TEST(!tools::is_error(run.results[0]));
    BOOST_TEST(!tools::is_error(run.results[1]));

    // Both calls were inside invoke() at the same time: the second did not wait
    // for the first, which is the freedom the settled type declared.
    BOOST_CHECK_EQUAL(reader->max_in_flight.load(), 2);
    BOOST_CHECK_EQUAL(reader->in_flight.load(), 0);
    // The trace says the same thing the counter does: one call was still running
    // when the other had finished.
    BOOST_TEST(at(*trace, "read_file", "call_2", "invoke") <
               at(*trace, "read_file", "call_1", "finished"));
}

BOOST_AUTO_TEST_CASE(a_batch_of_parallel_calls_is_safe_on_a_thread_pool)
{
    auto trace = std::make_shared<Trace>();
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    reader->delay = std::chrono::milliseconds(2);
    auto set = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{reader});

    ToolRegistry registry;
    registry.add(set);

    std::vector<model_io::InvokeQuery> batch;
    for (int index = 0; index < 8; ++index) {
        batch.push_back(call_for("read_file", std::format("call_{}", index)));
    }

    const BatchRun run = run_batch_on_threads(registry, std::move(batch));

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 8u);
    for (int index = 0; index < 8; ++index) {
        const auto position = static_cast<std::size_t>(index);
        BOOST_TEST(run.results[position].query.id == std::format("call_{}", index));
        BOOST_TEST(!tools::is_error(run.results[position]));
    }
    BOOST_CHECK_EQUAL(reader->invoke_count.load(), 8);
    BOOST_CHECK_EQUAL(reader->in_flight.load(), 0);
}

BOOST_AUTO_TEST_CASE(two_batches_run_at_once_on_one_registry)
{
    // Routing reads the table and writes nothing, and that is a contract rather
    // than an accident: a host with two model turns in flight dispatches both
    // through the one registry. The two batches here are routed and run from
    // four threads at once, and neither loses a record to the other.
    auto trace = std::make_shared<Trace>();
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    reader->delay = std::chrono::milliseconds(2);
    auto set = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{reader});

    ToolRegistry registry;
    registry.add(set);

    std::vector<std::vector<model_io::InvokeQuery>> batches;
    for (int batch = 0; batch < 2; ++batch) {
        std::vector<model_io::InvokeQuery> calls;
        for (int call = 0; call < 3; ++call) {
            calls.push_back(call_for(
                "read_file", std::format("batch{}_call{}", batch, call)));
        }
        batches.push_back(std::move(calls));
    }

    const std::vector<BatchRun> runs =
        run_batches_on_threads(registry, std::move(batches));

    BOOST_REQUIRE_EQUAL(runs.size(), 2u);
    for (int batch = 0; batch < 2; ++batch) {
        const auto position = static_cast<std::size_t>(batch);
        BOOST_REQUIRE(runs[position].returned);
        BOOST_REQUIRE_EQUAL(runs[position].results.size(), 3u);
        for (int call = 0; call < 3; ++call) {
            const auto slot = static_cast<std::size_t>(call);
            BOOST_TEST(runs[position].results[slot].query.id ==
                       std::format("batch{}_call{}", batch, call));
            BOOST_TEST(!tools::is_error(runs[position].results[slot]));
        }
    }
    BOOST_CHECK_EQUAL(reader->invoke_count.load(), 6);
    BOOST_CHECK_EQUAL(reader->in_flight.load(), 0);
}

// ===== the failures a batch has to survive ===================================

BOOST_AUTO_TEST_CASE(a_broken_settle_is_a_record_and_not_an_aborted_batch)
{
    auto trace = std::make_shared<Trace>();
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    auto other = std::make_shared<ScriptedTool>("read_from_broken", trace);
    auto good = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{reader});
    auto broken = std::make_shared<ScriptedSet>(
        "broken_tools", std::vector<tools::ToolSet::ToolHandle>{other});
    broken->prepare_throws = true;
    broken->broken_how = "the catalogue is unreachable";

    ToolRegistry registry;
    registry.add(good);
    registry.add(broken);

    const BatchRun run = run_batch(registry, {
        call_for("read_file", "call_1"),
        call_for("read_from_broken", "call_2"),
        call_for("read_file", "call_3"),
    });

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 3u);

    // prepare() promises it ALWAYS throws InvokeException; this set threw a plain
    // std::runtime_error. The call is answered at Unknown — no checkpoint owns a
    // throw that escaped the sequence — and the set is named, so the record says
    // which contract broke.
    BOOST_TEST(tools::is_error(run.results[1]));
    BOOST_CHECK(tools::error_stage(run.results[1]) == InvokeException::Stage::Unknown);
    BOOST_TEST(run.results[1].query.id == "call_2");
    BOOST_TEST(run.results[1].output.raw ==
               "Failed at an unknown stage: toolset \"broken_tools\" broke its "
               "contract while settling the call: the catalogue is unreachable "
               "(tool read_from_broken; call call_2)");

    // The rest of the batch kept its answers, which is the whole point of
    // catching a broken set in this layer.
    BOOST_TEST(!tools::is_error(run.results[0]));
    BOOST_TEST(!tools::is_error(run.results[2]));
    BOOST_CHECK_EQUAL(other->invoke_count.load(), 0);
}

BOOST_AUTO_TEST_CASE(a_broken_execute_is_a_record_in_the_serial_path)
{
    auto trace = std::make_shared<Trace>();
    auto writer = std::make_shared<ScriptedTool>("write_file", trace);
    writer->declares_type = model_io::InvokeType::SerialWrite;
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    auto broken = std::make_shared<ScriptedSet>(
        "broken_tools", std::vector<tools::ToolSet::ToolHandle>{writer});
    broken->execute_throws = true;
    auto good = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{reader});

    ToolRegistry registry;
    registry.add(good);
    registry.add(broken);

    const BatchRun run = run_batch(registry, {
        call_for("read_file", "call_1"),
        call_for("write_file", "call_2"),
        call_for("read_file", "call_3"),
    });

    // execute() promises it NEVER throws. A set that does anyway must not take
    // the caller's await with it — `returned` is that assertion, and the
    // watchdog in run_batch() is what makes it fail instead of hanging.
    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 3u);

    BOOST_TEST(tools::is_error(run.results[1]));
    BOOST_CHECK(tools::error_stage(run.results[1]) == InvokeException::Stage::Unknown);
    BOOST_TEST(run.results[1].query.id == "call_2");
    BOOST_TEST(run.results[1].output.raw ==
               "Failed at an unknown stage: toolset \"broken_tools\" broke its "
               "contract out of execute(): the set broke its own contract "
               "(tool write_file; call call_2)");
    BOOST_TEST(!tools::is_error(run.results[0]));
    BOOST_TEST(!tools::is_error(run.results[2]));
    // The set threw before the tool ran: nothing was invoked.
    BOOST_CHECK_EQUAL(writer->invoke_count.load(), 0);
}

BOOST_AUTO_TEST_CASE(a_broken_execute_is_a_record_in_the_parallel_path)
{
    // The regression this pins: a branch that threw on its way to its report
    // left the collector waiting for a message no branch would ever send, so the
    // batch — and with it the agent loop — never returned. The branch now turns
    // any throw into a record, and the completion handler reports for it if the
    // report itself was what failed.
    auto trace = std::make_shared<Trace>();
    auto other = std::make_shared<ScriptedTool>("read_from_broken", trace);
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    auto broken = std::make_shared<ScriptedSet>(
        "broken_tools", std::vector<tools::ToolSet::ToolHandle>{other});
    broken->execute_throws = true;
    auto good = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{reader});

    ToolRegistry registry;
    registry.add(good);
    registry.add(broken);

    const BatchRun run = run_batch(registry, {
        call_for("read_file", "call_1"),
        call_for("read_from_broken", "call_2"),
        call_for("read_file", "call_3"),
    });

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 3u);

    BOOST_TEST(tools::is_error(run.results[1]));
    BOOST_CHECK(tools::error_stage(run.results[1]) == InvokeException::Stage::Unknown);
    BOOST_TEST(run.results[1].query.id == "call_2");
    BOOST_TEST(run.results[1].output.raw ==
               "Failed at an unknown stage: toolset \"broken_tools\" broke its "
               "contract out of execute(): the set broke its own contract "
               "(tool read_from_broken; call call_2)");

    // The neighbours of the broken branch were still answered, in call order.
    BOOST_TEST(!tools::is_error(run.results[0]));
    BOOST_TEST(!tools::is_error(run.results[2]));
    BOOST_CHECK_EQUAL(other->invoke_count.load(), 0);
    BOOST_CHECK_EQUAL(reader->invoke_count.load(), 2);
}

BOOST_AUTO_TEST_CASE(a_record_is_filed_under_the_call_that_was_made)
{
    // A set may answer for a call of its own making — a retry, a nested
    // invocation, a batching relay — and the record then carries THAT call. The
    // batch still has to answer the call IT made, so records are filed under the
    // identity of the call the registry was given, never under the identity that
    // came back. Both schedules are covered: the parallel report travels with
    // its key, and the serial path files by the call it is running.
    for (const model_io::InvokeType declared : {model_io::InvokeType::ReadOnly,
                                                model_io::InvokeType::SerialWrite}) {
        auto trace = std::make_shared<Trace>();
        auto nested = std::make_shared<ScriptedTool>("nested_tool", trace);
        nested->declares_type = declared;
        auto ordinary = std::make_shared<ScriptedTool>("read_file", trace);
        auto relay = std::make_shared<ScriptedSet>(
            "relay_tools", std::vector<tools::ToolSet::ToolHandle>{nested});
        relay->answer_for_another_call = true;
        auto local = std::make_shared<ScriptedSet>(
            "local_tools", std::vector<tools::ToolSet::ToolHandle>{ordinary});

        ToolRegistry registry;
        registry.add(relay);
        registry.add(local);

        const BatchRun run = run_batch(registry, {
            call_for("nested_tool", "call_1"),
            call_for("read_file", "call_2"),
        });

        BOOST_REQUIRE(run.returned);
        BOOST_REQUIRE_EQUAL(run.results.size(), 2u);

        // The set's own answer survives in the record...
        BOOST_TEST(run.results[0].query.id == "inner_call");
        BOOST_TEST(!tools::is_error(run.results[0]));
        // ... and the call it was asked about is answered by it rather than
        // reported as a record that never arrived.
        BOOST_TEST(run.results[1].query.id == "call_2");
        BOOST_TEST(!tools::is_error(run.results[1]));
    }
}

BOOST_AUTO_TEST_CASE(every_call_of_a_broken_batch_is_still_answered_exactly_once)
{
    // The wire rule this layer exists to keep: an assistant message whose
    // tool_calls are not each answered by exactly one tool message is rejected by
    // the provider. So a batch that mixes every failure this layer has must come
    // back with one record per call, in the order of the calls.
    auto trace = std::make_shared<Trace>();

    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    auto refusing = std::make_shared<ScriptedTool>("guarded", trace);
    refusing->security_passes = false;
    auto failing = std::make_shared<ScriptedTool>("failing", trace);
    failing->fail_in = ScriptedTool::Fail::Invoke;
    failing->failure = "the disk is gone";
    auto unparsed = std::make_shared<ScriptedTool>("strict", trace);
    unparsed->fail_in = ScriptedTool::Fail::EnsureArguments;
    unparsed->failure = "missing required property \"path\"";

    auto local = std::make_shared<ScriptedSet>("local_tools",
        std::vector<tools::ToolSet::ToolHandle>{reader, refusing, failing, unparsed});
    auto broken_execute = std::make_shared<ScriptedSet>("execute_tools",
        std::vector<tools::ToolSet::ToolHandle>{
            std::make_shared<ScriptedTool>("broken_execute", trace)});
    broken_execute->execute_throws = true;
    auto broken_settle = std::make_shared<ScriptedSet>("settle_tools",
        std::vector<tools::ToolSet::ToolHandle>{
            std::make_shared<ScriptedTool>("broken_settle", trace)});
    broken_settle->prepare_throws = true;

    ToolRegistry registry;
    registry.add(local);
    registry.add(broken_execute);
    registry.add(broken_settle);

    const BatchRun run = run_batch(registry, {
        call_for("read_file", "call_1"),
        call_for("rm_rf", "call_2"),          // the table does not know it
        call_for("broken_settle", "call_3"),  // prepare() threw a bare exception
        call_for("strict", "call_4"),         // the arguments did not parse
        call_for("guarded", "call_5"),        // refused at the security check
        call_for("broken_execute", "call_6"), // execute() threw a bare exception
        call_for("failing", "call_7"),        // the tool itself failed
        call_for("read_file", "call_8"),
    });

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 8u);

    // One record per call, each answering the call at its index: the count is
    // exact and the order is the batch's.
    const std::vector<std::string> expected_ids = {"call_1", "call_2", "call_3",
        "call_4", "call_5", "call_6", "call_7", "call_8"};
    for (std::size_t index = 0; index < expected_ids.size(); ++index) {
        BOOST_TEST(run.results[index].query.id == expected_ids[index]);
    }

    // Each failure is classified where it happened, and the two working calls
    // are the ones that worked.
    BOOST_TEST(!tools::is_error(run.results[0]));
    BOOST_CHECK(tools::error_stage(run.results[1]) == InvokeException::Stage::Dispatch);
    BOOST_CHECK(tools::error_stage(run.results[2]) == InvokeException::Stage::Unknown);
    BOOST_CHECK(tools::error_stage(run.results[3]) == InvokeException::Stage::ArgumentParse);
    BOOST_CHECK(tools::error_stage(run.results[4]) == InvokeException::Stage::SecurityCheck);
    BOOST_CHECK(tools::error_stage(run.results[5]) == InvokeException::Stage::Unknown);
    BOOST_CHECK(tools::error_stage(run.results[6]) == InvokeException::Stage::Invoke);
    BOOST_TEST(!tools::is_error(run.results[7]));

    // Every failure record carries the marker a host classifies by, without
    // parsing the prose the model reads.
    for (const model_io::InvokeReturn& record : run.results) {
        if (!tools::is_error(record)) {
            continue;
        }
        BOOST_REQUIRE(record.extras.has_value());
        BOOST_TEST(record.extras->at("error").at("stage").is_string());
        BOOST_TEST(!record.output.raw.empty());
    }
}

// ===== the batch belongs to the frame, not to the caller =====================

BOOST_AUTO_TEST_CASE(the_batch_belongs_to_the_coroutine_frame)
{
    // execute() takes its batch BY VALUE, and this is what that buys: the
    // awaitable is created here and first resumed later, so a reference
    // parameter would be reading a vector the caller has since emptied. The
    // caller destroys what its batch holds before a single checkpoint has run,
    // and both calls still have to be answered. (The tree states the rule at
    // length in endpoint/include/endpoint/https_stream.hpp; this is the same
    // trap, made visible without a sanitizer.)
    auto trace = std::make_shared<Trace>();
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    reader->fill_default_argument = true;
    auto set = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{reader});

    ToolRegistry registry;
    registry.add(set);

    asio::io_context io;
    std::vector<model_io::InvokeQuery> batch = {
        call_for("read_file", "call_1"),
        call_for("read_file", "call_2"),
    };

    auto pending = registry.execute(std::move(batch), io.get_executor());
    // The caller is done with its batch, and says so the strongest way short of
    // going out of scope.
    batch.clear();
    batch.shrink_to_fit();

    std::optional<ToolRegistry::Results> results;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            results = co_await std::move(pending);
        },
        asio::detached);
    io.run();

    BOOST_REQUIRE(results.has_value());
    BOOST_REQUIRE_EQUAL(results->size(), 2u);
    BOOST_TEST((*results)[0].query.id == "call_1");
    BOOST_TEST((*results)[1].query.id == "call_2");
    BOOST_TEST(!tools::is_error((*results)[0]));
    BOOST_TEST(!tools::is_error((*results)[1]));
    // The queries were settled in the frame's own copies, not in the caller's.
    BOOST_TEST((*results)[0].query.arguments["path"] == "/default/path");
    BOOST_CHECK_EQUAL(reader->invoke_count.load(), 2);
}

BOOST_AUTO_TEST_CASE(the_convenience_form_runs_the_batch_on_the_callers_executor)
{
    // execute(queries) exists because a host dispatching from inside a coroutine
    // is already on the executor it wants the batch on — its own strand, its own
    // context — and should not have to name it again. What this pins is that the
    // batch really lands THERE: this helper runs the batch on a private context
    // that only the caller drives, and the batch completes on it without ever
    // being told which one it is.
    //
    // The delay is what makes the executor observable, and it is why this case
    // is not a duplicate of the explicit form's: `co_spawn` runs a branch INLINE
    // on the collector's thread until the branch's first real suspension, and
    // dispatches everything after it on the executor the branch was spawned
    // with. So the checkpoints a wrong executor would move are the ones after
    // invoke()'s wait — which is why the tool waits here.
    auto trace = std::make_shared<Trace>();
    auto reader = std::make_shared<ScriptedTool>("read_file", trace);
    reader->fill_default_argument = true;
    reader->delay = std::chrono::milliseconds(2);
    auto writer = std::make_shared<ScriptedTool>("write_file", trace);
    writer->declares_type = model_io::InvokeType::SerialWrite;
    writer->payload = "written";
    auto set = std::make_shared<ScriptedSet>(
        "local_tools", std::vector<tools::ToolSet::ToolHandle>{reader, writer});

    ToolRegistry registry;
    registry.add(set);

    const BatchRun run = run_batch_on_the_callers_executor(registry, {
        call_for("write_file", "call_w"),
        call_for("read_file", "call_r1"),
        call_for("read_file", "call_r2"),
    });

    BOOST_REQUIRE(run.returned);
    BOOST_REQUIRE_EQUAL(run.results.size(), 3u);
    // Same records, same order — the convenience form is the same call.
    BOOST_TEST(run.results[0].query.id == "call_w");
    BOOST_TEST(run.results[0].output.raw == "written");
    BOOST_TEST(run.results[1].query.id == "call_r1");
    BOOST_TEST(run.results[2].query.id == "call_r2");
    for (const model_io::InvokeReturn& record : run.results) {
        BOOST_TEST(!tools::is_error(record));
    }
    BOOST_TEST(run.results[1].query.arguments["path"] == "/default/path");

    // ... and every checkpoint of every call ran on the CALLER's executor: one
    // thread drove this context, and it is this one.
    const std::vector<std::thread::id> writers = trace->threads();
    BOOST_REQUIRE_EQUAL(writers.size(), 1u);
    BOOST_TEST(writers.front() == std::this_thread::get_id());
    // The serial call and both parallel calls all went through it.
    BOOST_CHECK_EQUAL(reader->invoke_count.load(), 2);
    BOOST_CHECK_EQUAL(writer->invoke_count.load(), 1);
}
