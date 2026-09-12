#define BOOST_TEST_MODULE ToolSetTests
#include <boost/test/unit_test.hpp>

#include "tools/toolsets.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace asio = boost::asio;
using tools::ConfirmDecision;
using tools::InvokeConfirmEvent;
using tools::InvokeException;

namespace {

/// A call as the conversation carries it: the id correlates the result, the
/// name is what dispatch() resolves. The type and security on it are
/// deliberately the record's defaults (ReadOnly / DefaultDeny) rather than what
/// a tool will declare: write_attributes() overwrites them, and the tests
/// assert the settled values, so a write_attributes() that never ran cannot
/// pass.
model_io::InvokeQuery call_for(std::string name, std::string id = "call_1")
{
    model_io::InvokeQuery query;
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::DefaultDeny;
    query.id = std::move(id);
    query.name = std::move(name);
    return query;
}

/// The read_file call the failure tests use: it carries the argument the tool
/// expects, so ensure_arguments() has nothing to fill in and the test is about
/// the failure it scripts.
model_io::InvokeQuery read_call()
{
    model_io::InvokeQuery query = call_for("read_file");
    query.arguments = {{"path", "/etc/hosts"}};
    return query;
}

/// What a host's batch builder holds for one call after phase 1: the tool
/// prepare() resolved, or the failure that stopped it — which is already the
/// record, through the exception's own bridge.
struct Prepared {
    tools::ToolSet::ToolHandle tool;
    std::optional<InvokeException> failure;

    [[nodiscard]] bool ok() const { return tool != nullptr; }
};

/// PHASE 1 alone, exactly as a host settling a batch of calls does it: the call
/// is mutated in place and never scheduled on failure.
Prepared prepare_only(tools::ToolSet& set, model_io::InvokeQuery& query)
{
    try {
        return Prepared{set.prepare(query), std::nullopt};
    } catch (const InvokeException& failure) {
        return Prepared{nullptr, failure};
    }
}

/// Both phases, in order, for a test that only cares about the record: the
/// host's loop in miniature. Phase 1's failure is turned into the record by the
/// exception's implicit conversion — nothing else is needed, which is the point
/// of prepare() throwing the module's own type.
asio::awaitable<model_io::InvokeReturn> call_through(
    tools::ToolSet& set, model_io::InvokeQuery query)
{
    tools::ToolSet::ToolHandle tool;
    try {
        tool = set.prepare(query);
    } catch (const InvokeException& failure) {
        co_return failure;
    }
    co_return co_await set.execute(std::move(tool), std::move(query));
}

model_io::InvokeReturn run(tools::ToolSet& set, model_io::InvokeQuery query)
{
    asio::io_context io;
    auto pending =
        asio::co_spawn(io, call_through(set, std::move(query)), asio::use_future);
    io.run();
    return pending.get();
}

/// PHASE 2 on its own, on an executor of its own: what a host does once the
/// schedule says this call may run now — and, being its own io_context, this is
/// also how a test says "this call had the executor to itself". (A context
/// cannot simply be run() twice: it stops when it runs out of work, and a later
/// run() returns without executing anything.)
model_io::InvokeReturn run_prepared(tools::ToolSet& set,
                                    tools::ToolSet::ToolHandle tool,
                                    model_io::InvokeQuery query)
{
    asio::io_context io;
    auto pending = asio::co_spawn(
        io, set.execute(std::move(tool), std::move(query)), asio::use_future);
    io.run();
    return pending.get();
}

/// The stage a failure record was raised at. Unwrapping here keeps every test
/// free of the optional dance; a record that is not a failure record fails the
/// requirement.
InvokeException::Stage stage_of(const model_io::InvokeReturn& record)
{
    const auto stage = tools::error_stage(record);
    BOOST_REQUIRE(stage.has_value());
    return *stage;
}

/// A tool whose every checkpoint is scripted: each hook either does its job or
/// fails, in one of the four shapes a hook can fail in. One type covers every
/// failure path the two phases have to report.
class ScriptedTool final : public tools::ToolInterface {
public:
    /// Which hook fails. The stage the failure is reported at follows from it —
    /// write_attributes shares ArgumentParse with ensure_arguments, being the
    /// same phase of settling the query.
    enum class Hook {
        None,
        EnsureArguments,
        WriteAttributes,
        SecurityCheck,
        Invoke,
        CheckResult
    };

    /// How the failure is thrown: the module's own type (carrying the query it
    /// was invoked with, or carrying no query at all), a plain std::exception,
    /// or something that is not a std::exception.
    enum class Throw {
        InvokeExceptionWithQuery,
        InvokeExceptionWithoutQuery,
        Plain,
        NonStd
    };

    /// Details are filled in the constructor rather than with aggregate
    /// initialization: Invocable leaves two of its members without a default
    /// member initializer, which -Wmissing-field-initializers flags.
    ScriptedTool()
    {
        details.name = "read_file";
        details.description = "Reads a file.";
    }

    model_io::Invocable details;

    Hook fail_in = Hook::None;
    Throw throw_as = Throw::Plain;
    std::string failure = "the checkpoint failed";
    /// What the tool calls the query it throws with, when it throws one:
    /// distinct from the caller's id, so a test can tell whose query survived.
    std::string own_query_id;
    /// The argument ensure_arguments() fills in when the call omits it; empty
    /// means "require the argument". This is what makes phase 1's in-place
    /// contract observable.
    std::string ensure_default_path = "/default/path";
    /// What security_check() answers when it is not the failing hook.
    bool security_passes = true;
    std::string security_reason = "the invocation was not confirmed";
    /// What write_attributes() declares about the call.
    model_io::InvokeType declares_type = model_io::InvokeType::ReadOnly;
    model_io::InvokeSecurity declares_security = model_io::InvokeSecurity::Trusted;
    /// What a successful invoke() answers.
    std::string payload = "127.0.0.1 localhost";
    /// Whether check_result() answers for a call of its own making instead of
    /// the one it was handed — a tool that rewires the record's identity.
    bool check_result_reports_another_call = false;

    // What the call did, observed as it ran. Mutable because the hooks that
    // observe are the const ones.
    //
    // The first four are the split's own assertions: after prepare() the
    // settling hooks must have run and the others must not have, and after
    // execute() the ordering is the other way round.
    mutable bool ensure_ran = false;
    mutable bool write_attributes_ran = false;
    mutable bool security_ran = false;
    mutable bool invoke_ran = false;
    mutable bool check_result_ran = false;
    mutable std::string check_result_query_id;
    mutable std::string check_result_output;
    mutable int build_calls = 0;
    mutable int release_calls = 0;

    const model_io::Invocable& get_details() const noexcept override
    {
        return details;
    }

    bool build() noexcept override
    {
        ++build_calls;
        return true;
    }

    void release() noexcept override { ++release_calls; }

    void ensure_arguments(model_io::InvokeQuery& query) const override
    {
        ensure_ran = true;
        fail_if(Hook::EnsureArguments, query);
        if (!ensure_default_path.empty() &&
            !query.arguments.contains("path")) {
            query.arguments["path"] = ensure_default_path;
        }
    }

    void write_attributes(model_io::InvokeQuery& query) const override
    {
        write_attributes_ran = true;
        fail_if(Hook::WriteAttributes, query);
        query.type = declares_type;
        query.security = declares_security;
    }

    boost::asio::awaitable<std::tuple<bool, std::string>> security_check(
        const model_io::InvokeQuery& query) override
    {
        security_ran = true;
        fail_if(Hook::SecurityCheck, query);
        co_return std::make_tuple(security_passes, security_reason);
    }

    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override
    {
        invoke_ran = true;
        fail_if(Hook::Invoke, query);
        co_return model_io::Content{
            .type = model_io::ContentType::Text, .raw = payload, .extras = {}};
    }

    model_io::InvokeReturn check_result(
        model_io::InvokeQuery query, model_io::Content output) const override
    {
        check_result_ran = true;
        check_result_query_id = query.id;
        check_result_output = output.raw;
        fail_if(Hook::CheckResult, query);
        if (check_result_reports_another_call) {
            query.id = "inner_call";
        }
        return tools::ToolInterface::check_result(std::move(query), std::move(output));
    }

private:
    /// Throw the scripted failure if this is the hook it was aimed at.
    void fail_if(Hook hook, const model_io::InvokeQuery& query) const
    {
        if (fail_in != hook) {
            return;
        }
        switch (throw_as) {
            case Throw::InvokeExceptionWithQuery: {
                model_io::InvokeQuery own = query;
                if (!own_query_id.empty()) {
                    own.id = own_query_id;
                }
                throw InvokeException(stage_for(hook), failure, std::move(own));
            }
            case Throw::InvokeExceptionWithoutQuery:
                throw InvokeException(stage_for(hook), failure);
            case Throw::Plain:
                throw std::runtime_error(failure);
            case Throw::NonStd:
                throw 42; // not a std::exception at all
        }
    }

    /// The stage a real tool would raise at this hook — and what the set has to
    /// report it at, since it must agree with the tool.
    static InvokeException::Stage stage_for(Hook hook)
    {
        switch (hook) {
            case Hook::EnsureArguments:
            case Hook::WriteAttributes:
                return InvokeException::Stage::ArgumentParse;
            case Hook::SecurityCheck:
                return InvokeException::Stage::SecurityCheck;
            case Hook::Invoke:
                return InvokeException::Stage::Invoke;
            case Hook::CheckResult:
                return InvokeException::Stage::ResultCheck;
            case Hook::None:
                break;
        }
        return InvokeException::Stage::Unknown;
    }
};

/// A tool that describes itself and nothing else: every hook stays at its
/// default, which is what puts the module's own attribute defaults
/// (write_attributes) and its default security policy (security_check) under
/// test end to end.
class MinimalTool final : public tools::ToolInterface {
public:
    MinimalTool()
    {
        details.name = "noop";
        details.description = "Does nothing.";
    }

    model_io::Invocable details;

    const model_io::Invocable& get_details() const noexcept override
    {
        return details;
    }
};

/// A tool that declares itself trusted and implements nothing else, so a call
/// reaches the DEFAULT invoke() — the one hook with no usable default.
class TrustedStubTool final : public tools::ToolInterface {
public:
    TrustedStubTool()
    {
        details.name = "stub";
        details.description = "Implements no invoke().";
    }

    model_io::Invocable details;

    const model_io::Invocable& get_details() const noexcept override
    {
        return details;
    }

    void write_attributes(model_io::InvokeQuery& query) const override
    {
        query.type = model_io::InvokeType::ReadOnly;
        query.security = model_io::InvokeSecurity::Trusted;
    }
};

/// A tool that runs and declares nothing: write_attributes() and
/// security_check() stay at their defaults, so a call is confirmed through the
/// process-wide bus before it can reach invoke().
class ConfirmableTool final : public tools::ToolInterface {
public:
    ConfirmableTool()
    {
        details.name = "confirmable";
        details.description = "Runs once a host confirms it.";
    }

    model_io::Invocable details;
    std::string payload = "the tool ran";

    const model_io::Invocable& get_details() const noexcept override
    {
        return details;
    }

    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery&) override
    {
        co_return model_io::Content{
            .type = model_io::ContentType::Text, .raw = payload, .extras = {}};
    }
};

/// A read-only tool that takes a moment to run and counts how many of its
/// invocations overlap — the measurement the two-phase split exists to make
/// possible. Everything here runs on one thread (the tests' io_context), so the
/// counters need no synchronisation; they only ever change at suspension
/// points.
class SlowReadTool final : public tools::ToolInterface {
public:
    SlowReadTool()
    {
        details.name = "slow_read";
        details.description = "Takes a moment, and may run alongside others.";
    }

    model_io::Invocable details;
    std::chrono::milliseconds delay{2};
    int in_flight = 0;
    int max_in_flight = 0;

    const model_io::Invocable& get_details() const noexcept override
    {
        return details;
    }

    void write_attributes(model_io::InvokeQuery& query) const override
    {
        query.type = model_io::InvokeType::ReadOnly;
        query.security = model_io::InvokeSecurity::Trusted;
    }

    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery&) override
    {
        ++in_flight;
        max_in_flight = std::max(max_in_flight, in_flight);

        asio::steady_timer timer(co_await asio::this_coro::executor, delay);
        co_await timer.async_wait(asio::use_awaitable);

        --in_flight;
        co_return model_io::Content{
            .type = model_io::ContentType::Text, .raw = "read", .extras = {}};
    }
};

/// A set of at most one tool: a test decides what a call resolves to — the
/// tool, nothing (the Dispatch failure), or a resolver that fails. It
/// implements only the lookup, which is the point: prepare() and execute() come
/// from ToolSet.
class FakeToolSet final : public tools::ToolSet {
public:
    FakeToolSet(std::string name, tools::ToolSet::ToolHandle tool)
        : name_(std::move(name)), tool_(std::move(tool))
    {}

    std::string_view name() const noexcept override { return name_; }

    std::vector<model_io::Invocable> get_tools() const override
    {
        if (tool_ == nullptr) {
            return {};
        }
        return {tool_->get_details()};
    }

    tools::ToolSet::ToolHandle dispatch(
        const model_io::InvokeQuery&) const override
    {
        if (dispatch_fails) {
            throw std::runtime_error("the tool registry is unreachable");
        }
        return tool_;
    }

    /// Whether dispatch() fails instead of answering.
    bool dispatch_fails = false;

private:
    std::string name_;
    tools::ToolSet::ToolHandle tool_;
};

// The shape of the split, pinned at compile time rather than only documented.
// Phase 1 must be an ORDINARY function returning the resolved tool: being
// non-suspending is the whole reason a host can settle a batch of calls before
// it schedules any of them, and no runtime assertion can prove a function
// cannot suspend — the type can. Phase 2 is the awaitable, and holds everything
// that may wait.
static_assert(std::is_same_v<
              decltype(std::declval<FakeToolSet&>().prepare(
                  std::declval<model_io::InvokeQuery&>())),
              tools::ToolSet::ToolHandle>);
static_assert(std::is_same_v<
              decltype(std::declval<FakeToolSet&>().execute(
                  std::declval<tools::ToolSet::ToolHandle>(),
                  std::declval<model_io::InvokeQuery>())),
              boost::asio::awaitable<model_io::InvokeReturn>>);

} // namespace

// ===== phase 1: prepare =====================================================

BOOST_AUTO_TEST_CASE(prepare_settles_the_call_in_place_and_resolves_its_tool)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->declares_type = model_io::InvokeType::ParallWrite;
    tool->declares_security = model_io::InvokeSecurity::RequireConfirm;
    FakeToolSet set("local_tools", tool);

    // The call as the model made it: no arguments, and the record's default
    // attributes.
    model_io::InvokeQuery query = call_for("read_file");

    const tools::ToolSet::ToolHandle resolved = set.prepare(query);

    // The tool is resolved, and it is the set's own instance.
    BOOST_REQUIRE(resolved != nullptr);
    BOOST_CHECK(resolved.get() == tool.get());

    // The caller reads the settled call off ITS OWN object — that is what makes
    // the phase synchronous and in place...
    BOOST_TEST(query.arguments["path"] == "/default/path");
    BOOST_CHECK(query.type == model_io::InvokeType::ParallWrite);
    BOOST_CHECK(query.security == model_io::InvokeSecurity::RequireConfirm);
    // ... and what the scheduler needs: the type says this call may run
    // alongside others, the level says a host will be asked about it.
    BOOST_TEST(tool->ensure_ran);
    BOOST_TEST(tool->write_attributes_ran);

    // Nothing that can wait has run: the security check, the invocation and the
    // result check are all on the far side of the split.
    BOOST_TEST(!tool->security_ran);
    BOOST_TEST(!tool->invoke_ran);
    BOOST_TEST(!tool->check_result_ran);
}

BOOST_AUTO_TEST_CASE(prepare_reports_an_unresolved_call)
{
    FakeToolSet set("local_tools", nullptr);
    model_io::InvokeQuery query = read_call();

    const Prepared prepared = prepare_only(set, query);

    BOOST_TEST(!prepared.ok());
    BOOST_REQUIRE(prepared.failure.has_value());
    BOOST_CHECK(prepared.failure->stage() == InvokeException::Stage::Dispatch);

    // The exception IS the record: one catch, no translation table.
    const model_io::InvokeReturn record = *prepared.failure;
    BOOST_TEST(tools::is_error(record));
    BOOST_TEST(record.output.raw ==
               "Failed while dispatching the invocation to a tool: no tool named "
               "\"read_file\" in toolset \"local_tools\" (tool read_file; call call_1)");
    BOOST_TEST(record.query.id == "call_1");

    // The caller's own query is untouched: the failure carried a copy, so a
    // host that keeps filling in its batch still has the call it was given.
    BOOST_TEST(query.id == "call_1");
    BOOST_TEST(query.name == "read_file");
    BOOST_TEST(query.arguments["path"] == "/etc/hosts");
}

BOOST_AUTO_TEST_CASE(prepare_reports_a_dispatch_that_throws)
{
    auto tool = std::make_shared<ScriptedTool>();
    FakeToolSet set("local_tools", tool);
    set.dispatch_fails = true;
    model_io::InvokeQuery query = read_call();

    const Prepared prepared = prepare_only(set, query);

    // Resolving a name may fail — a map, a plugin registry, a remote catalogue.
    // That is a Dispatch failure for the model, not a std::terminate().
    BOOST_TEST(!prepared.ok());
    BOOST_REQUIRE(prepared.failure.has_value());
    BOOST_CHECK(prepared.failure->stage() == InvokeException::Stage::Dispatch);
    BOOST_TEST(prepared.failure->to_invoke_return().output.raw ==
               "Failed while dispatching the invocation to a tool: "
               "the tool registry is unreachable (tool read_file; call call_1)");
    // The tool itself was never reached.
    BOOST_TEST(!tool->ensure_ran);
}

BOOST_AUTO_TEST_CASE(prepare_reports_a_tools_own_argument_failure)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->fail_in = ScriptedTool::Hook::EnsureArguments;
    tool->throw_as = ScriptedTool::Throw::InvokeExceptionWithQuery;
    tool->failure = "missing required property \"path\"";
    FakeToolSet set("local_tools", tool);
    model_io::InvokeQuery query = read_call();

    const Prepared prepared = prepare_only(set, query);

    BOOST_TEST(!prepared.ok());
    BOOST_REQUIRE(prepared.failure.has_value());
    BOOST_CHECK(prepared.failure->stage() == InvokeException::Stage::ArgumentParse);
    // The message the model reads, and the bare one the marker carries.
    const model_io::InvokeReturn record = *prepared.failure;
    BOOST_TEST(record.output.raw ==
               "Failed while parsing the invocation arguments: "
               "missing required property \"path\" (tool read_file; call call_1)");
    BOOST_REQUIRE(record.extras.has_value());
    BOOST_TEST(record.extras->at("error").at("message") ==
               "missing required property \"path\"");
    // The settling phase failed, so the other one never runs.
    BOOST_TEST(!tool->write_attributes_ran);
}

BOOST_AUTO_TEST_CASE(prepare_reports_a_write_attributes_failure_at_the_same_stage)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->fail_in = ScriptedTool::Hook::WriteAttributes;
    tool->failure = "the tool cannot classify this call";
    FakeToolSet set("local_tools", tool);
    model_io::InvokeQuery query = read_call();

    const Prepared prepared = prepare_only(set, query);

    BOOST_TEST(!prepared.ok());
    BOOST_REQUIRE(prepared.failure.has_value());
    // The same phase as ensure_arguments: settling the query before any check
    // runs, which is why it is the same stage.
    BOOST_CHECK(prepared.failure->stage() == InvokeException::Stage::ArgumentParse);
    BOOST_TEST(prepared.failure->to_invoke_return().output.raw ==
               "Failed while parsing the invocation arguments: "
               "the tool cannot classify this call (tool read_file; call call_1)");
}

BOOST_AUTO_TEST_CASE(prepare_never_lets_a_bare_exception_reach_the_caller)
{
    // A host's batch builder catches ONE type — the module's — so a hook that
    // throws anything else has to arrive as that type, at the stage that was
    // running. Both shapes of "anything else" are covered here.
    for (const ScriptedTool::Throw shape :
         {ScriptedTool::Throw::Plain, ScriptedTool::Throw::NonStd}) {
        auto tool = std::make_shared<ScriptedTool>();
        tool->fail_in = ScriptedTool::Hook::EnsureArguments;
        tool->throw_as = shape;
        tool->failure = "the tool blew up";
        FakeToolSet set("local_tools", tool);
        model_io::InvokeQuery query = read_call();

        // prepare_only() catches InvokeException only: anything else escapes
        // and fails this case.
        const Prepared prepared = prepare_only(set, query);

        BOOST_TEST(!prepared.ok());
        BOOST_REQUIRE(prepared.failure.has_value());
        BOOST_CHECK(prepared.failure->stage() ==
                    InvokeException::Stage::ArgumentParse);
        // The call still travels back in the record, whichever shape it was.
        BOOST_TEST(prepared.failure->query().id == "call_1");
    }
}

// ===== phase 2: execute ======================================================

BOOST_AUTO_TEST_CASE(execute_runs_the_settled_call_and_returns_its_record)
{
    auto tool = std::make_shared<ScriptedTool>();
    FakeToolSet set("local_tools", tool);
    model_io::InvokeQuery query = read_call();

    const tools::ToolSet::ToolHandle resolved = set.prepare(query);

    asio::io_context io;
    auto pending = asio::co_spawn(
        io, set.execute(resolved, query), asio::use_future);
    io.run();
    const model_io::InvokeReturn record = pending.get();

    BOOST_TEST(!tools::is_error(record));
    BOOST_CHECK(record.output.type == model_io::ContentType::Text);
    BOOST_TEST(record.output.raw == "127.0.0.1 localhost");
    // The record answers the call, in its settled form.
    BOOST_TEST(record.query.id == "call_1");
    BOOST_TEST(record.query.name == "read_file");
    BOOST_TEST(record.query.arguments["path"] == "/etc/hosts");
    BOOST_CHECK(record.query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(record.query.security == model_io::InvokeSecurity::Trusted);
    BOOST_TEST(!record.extras.has_value());

    // The asynchronous half ran, in order, and check_result() saw the output
    // invoke() produced.
    BOOST_TEST(tool->security_ran);
    BOOST_TEST(tool->invoke_ran);
    BOOST_TEST(tool->check_result_ran);
    BOOST_TEST(tool->check_result_query_id == "call_1");
    BOOST_TEST(tool->check_result_output == "127.0.0.1 localhost");

    // Neither phase runs the tool's lifecycle: build() and release() are the
    // owning set's business, once per tool instance, and take no query.
    BOOST_TEST(tool->build_calls == 0);
    BOOST_TEST(tool->release_calls == 0);
}

BOOST_AUTO_TEST_CASE(execute_reports_a_security_refusal_with_the_tools_words)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->security_passes = false;
    tool->security_reason = "the human did not confirm the write";
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    BOOST_TEST(tools::is_error(record));
    BOOST_CHECK(stage_of(record) == InvokeException::Stage::SecurityCheck);
    BOOST_TEST(record.output.raw ==
               "Failed while validating the invocation's security: "
               "security check denied: the human did not confirm the write "
               "(tool read_file; call call_1)");
    // A refused invocation never reaches the tool.
    BOOST_TEST(!tool->invoke_ran);
}

BOOST_AUTO_TEST_CASE(execute_reports_a_throwing_security_check)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->fail_in = ScriptedTool::Hook::SecurityCheck;
    tool->failure = "the confirmation service is unreachable";
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    BOOST_CHECK(stage_of(record) == InvokeException::Stage::SecurityCheck);
    BOOST_TEST(record.output.raw ==
               "Failed while validating the invocation's security: "
               "the confirmation service is unreachable "
               "(tool read_file; call call_1)");
    BOOST_TEST(!tool->invoke_ran);
}

BOOST_AUTO_TEST_CASE(execute_reports_an_invoke_failure)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->fail_in = ScriptedTool::Hook::Invoke;
    tool->failure = "the tool failed";
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    BOOST_CHECK(stage_of(record) == InvokeException::Stage::Invoke);
    BOOST_TEST(record.output.raw ==
               "Failed while invoking the tool: the tool failed "
               "(tool read_file; call call_1)");
    BOOST_TEST(tool->invoke_ran);
    BOOST_TEST(!tool->check_result_ran);
}

BOOST_AUTO_TEST_CASE(execute_reports_an_unqueried_tool_exception_around_the_call)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->fail_in = ScriptedTool::Hook::Invoke;
    tool->throw_as = ScriptedTool::Throw::InvokeExceptionWithoutQuery;
    tool->failure = "the read failed";
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    BOOST_CHECK(stage_of(record) == InvokeException::Stage::Invoke);
    // A tool may throw knowing nothing about the call it was invoked with; the
    // record still has to answer that call, so the set's query stands in.
    BOOST_TEST(record.query.id == "call_1");
    BOOST_TEST(record.query.name == "read_file");
    BOOST_TEST(record.output.raw ==
               "Failed while invoking the tool: the read failed "
               "(tool read_file; call call_1)");
}

BOOST_AUTO_TEST_CASE(execute_never_lets_a_nested_call_take_over_the_record)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->fail_in = ScriptedTool::Hook::Invoke;
    tool->throw_as = ScriptedTool::Throw::InvokeExceptionWithQuery;
    tool->own_query_id = "inner_call";
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    // A tool may fail on a call of its own making — a retry, a nested
    // invocation — but the record still answers the call the MODEL made.
    // query.id is the wire's tool_call_id (emit_tool_results), so answering with
    // the nested id would leave call_1 unanswered and the provider would reject
    // the turn.
    BOOST_CHECK(stage_of(record) == InvokeException::Stage::Invoke);
    BOOST_TEST(record.query.id == "call_1");
    BOOST_TEST(record.query.name == "read_file");
    // ... the prose follows the identity, so the record reads as one answer...
    BOOST_TEST(record.output.raw ==
               "Failed while invoking the tool: the checkpoint failed "
               "(tool read_file; call call_1)");

    // ... and the nested call is preserved rather than dropped: a host can still
    // see which call the failure was really about.
    BOOST_REQUIRE(record.extras.has_value());
    const nlohmann::json& cause =
        record.extras->at(std::string(InvokeException::cause_key));
    BOOST_TEST(cause.at("id") == "inner_call");
    BOOST_TEST(cause.at("name") == "read_file");
    BOOST_TEST(cause.at("arguments").at("path") == "/etc/hosts");
    // The marker's own fields are untouched by the cause.
    BOOST_TEST(record.extras->at("error").at("stage") == "invoke");
    BOOST_TEST(record.extras->at("error").at("message") == "the checkpoint failed");
}

BOOST_AUTO_TEST_CASE(execute_omits_the_cause_when_the_tool_named_the_same_call)
{
    // The ordinary case: the tool threw the query it was handed (or one equal to
    // it), so there is no second call to preserve and extras stays a plain
    // failure marker — a host reading extras.cause_query is reading "this
    // failure named a DIFFERENT call", not "this failure had a query".
    auto tool = std::make_shared<ScriptedTool>();
    tool->fail_in = ScriptedTool::Hook::Invoke;
    tool->throw_as = ScriptedTool::Throw::InvokeExceptionWithQuery;
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    BOOST_REQUIRE(record.extras.has_value());
    BOOST_CHECK(!record.extras->contains(std::string(InvokeException::cause_key)));
    BOOST_TEST(record.query.id == "call_1");
}

BOOST_AUTO_TEST_CASE(execute_re_correlates_a_record_the_tool_built_itself)
{
    // check_result() builds the record, so a tool can put a query of its own in
    // it. The set takes that identity back the same way it takes it back from a
    // failure: the record answers the call the caller made, and the tool's own
    // call is preserved as the cause.
    auto tool = std::make_shared<ScriptedTool>();
    tool->check_result_reports_another_call = true;
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    BOOST_TEST(!tools::is_error(record));   // the tool ran fine; only its wiring moved
    BOOST_TEST(record.query.id == "call_1");
    BOOST_TEST(record.query.name == "read_file");
    BOOST_TEST(record.output.raw == "127.0.0.1 localhost");
    BOOST_REQUIRE(record.extras.has_value());
    BOOST_TEST(record.extras->at(std::string(InvokeException::cause_key)).at("id") ==
               "inner_call");
}

BOOST_AUTO_TEST_CASE(execute_reports_a_result_check_failure_and_keeps_the_call)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->fail_in = ScriptedTool::Hook::CheckResult;
    tool->failure = "result is not valid JSON";
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    BOOST_CHECK(stage_of(record) == InvokeException::Stage::ResultCheck);
    // The tool RAN: this is the one failure that is not a failure to run.
    BOOST_TEST(tool->invoke_ran);
    BOOST_TEST(tool->check_result_ran);

    // The regression this guards: check_result() takes the query by value, so
    // moving ours into it would leave the catch block holding a moved-from
    // query — and the record would lose the call it exists to answer.
    BOOST_TEST(record.query.id == "call_1");
    BOOST_TEST(record.query.name == "read_file");
    BOOST_TEST(record.query.arguments["path"] == "/etc/hosts");
    BOOST_TEST(record.output.raw ==
               "Failed while validating the tool result: "
               "result is not valid JSON (tool read_file; call call_1)");
}

BOOST_AUTO_TEST_CASE(execute_reports_a_throw_that_is_not_a_std_exception)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->fail_in = ScriptedTool::Hook::Invoke;
    tool->throw_as = ScriptedTool::Throw::NonStd;
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    BOOST_CHECK(stage_of(record) == InvokeException::Stage::Invoke);
    BOOST_TEST(record.output.raw ==
               "Failed while invoking the tool: an unknown error, not a "
               "std::exception (tool read_file; call call_1)");
}

BOOST_AUTO_TEST_CASE(execute_without_a_tool_returns_a_record_not_a_crash)
{
    auto tool = std::make_shared<ScriptedTool>();
    FakeToolSet set("local_tools", tool);
    model_io::InvokeQuery query = read_call();

    // What a caller that ignored prepare()'s failure would pass.
    asio::io_context io;
    auto pending = asio::co_spawn(
        io, set.execute(nullptr, query), asio::use_future);
    io.run();
    const model_io::InvokeReturn record = pending.get();

    BOOST_TEST(tools::is_error(record));
    BOOST_CHECK(stage_of(record) == InvokeException::Stage::Dispatch);
    BOOST_TEST(record.query.id == "call_1");
    BOOST_TEST(!tool->security_ran);
}

BOOST_AUTO_TEST_CASE(a_tool_that_implements_no_invoke_refuses_at_that_checkpoint)
{
    auto tool = std::make_shared<TrustedStubTool>();
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, call_for("stub", "call_2"));

    // The one hook with no usable default: answering an empty text part would
    // look to the model like a result, so the default refuses instead — and
    // says which hook is missing.
    BOOST_TEST(tools::is_error(record));
    BOOST_CHECK(stage_of(record) == InvokeException::Stage::Invoke);
    BOOST_TEST(record.output.raw ==
               "Failed while invoking the tool: tool \"stub\" does not "
               "implement invoke() (tool stub; call call_2)");
    // The gate in front of it passed, so the refusal is the tool's own.
    BOOST_CHECK(record.query.security == model_io::InvokeSecurity::Trusted);
}

// ===== what the split is for: settle a batch, then schedule it ===============

BOOST_AUTO_TEST_CASE(a_batch_is_settled_before_any_of_it_runs)
{
    // Two tools with different schedules, in different sets: the host settles
    // every call first, and only then decides what may overlap.
    auto parallel_tool = std::make_shared<ScriptedTool>();
    parallel_tool->declares_type = model_io::InvokeType::ParallWrite;
    FakeToolSet parallel_set("parallel_tools", parallel_tool);

    auto serial_tool = std::make_shared<ScriptedTool>();
    serial_tool->declares_type = model_io::InvokeType::SerialWrite;
    FakeToolSet serial_set("serial_tools", serial_tool);

    model_io::InvokeQuery parallel_call = read_call();
    model_io::InvokeQuery serial_call = read_call();

    // Phase 1 for the whole batch, synchronously, before anything is awaited.
    Prepared first = prepare_only(parallel_set, parallel_call);
    Prepared second = prepare_only(serial_set, serial_call);
    BOOST_REQUIRE(first.ok());
    BOOST_REQUIRE(second.ok());

    // The scheduling decision is readable off the settled calls, and nothing
    // that can wait has started for either of them.
    BOOST_CHECK(parallel_call.type == model_io::InvokeType::ParallWrite);
    BOOST_CHECK(serial_call.type == model_io::InvokeType::SerialWrite);
    BOOST_TEST(!parallel_tool->security_ran);
    BOOST_TEST(!serial_tool->security_ran);
    BOOST_TEST(!parallel_tool->invoke_ran);
    BOOST_TEST(!serial_tool->invoke_ran);

    // Phase 2, in the order that decision implies: the serial call gets the
    // executor to itself.
    const model_io::InvokeReturn parallel_record =
        run_prepared(parallel_set, first.tool, parallel_call);
    BOOST_TEST(!tools::is_error(parallel_record));

    const model_io::InvokeReturn serial_record =
        run_prepared(serial_set, second.tool, serial_call);
    BOOST_TEST(!tools::is_error(serial_record));

    BOOST_TEST(parallel_tool->invoke_ran);
    BOOST_TEST(serial_tool->invoke_ran);
}

BOOST_AUTO_TEST_CASE(prepared_parallel_calls_really_do_overlap)
{
    // One tool, two calls, one executor: the split lets the host await both at
    // once, and this is what "may run alongside others" means in practice.
    auto tool = std::make_shared<SlowReadTool>();
    FakeToolSet set("local_tools", tool);

    model_io::InvokeQuery first_call = call_for("slow_read", "call_1");
    model_io::InvokeQuery second_call = call_for("slow_read", "call_2");

    const tools::ToolSet::ToolHandle first = set.prepare(first_call);
    const tools::ToolSet::ToolHandle second = set.prepare(second_call);
    BOOST_CHECK(first_call.type == model_io::InvokeType::ReadOnly);

    asio::io_context io;
    auto first_run = asio::co_spawn(
        io, set.execute(first, first_call), asio::use_future);
    auto second_run = asio::co_spawn(
        io, set.execute(second, second_call), asio::use_future);
    io.run();

    const model_io::InvokeReturn first_record = first_run.get();
    const model_io::InvokeReturn second_record = second_run.get();
    BOOST_TEST(!tools::is_error(first_record));
    BOOST_TEST(!tools::is_error(second_record));
    BOOST_TEST(first_record.query.id == "call_1");
    BOOST_TEST(second_record.query.id == "call_2");
    // Both were inside invoke() at the same time: the second did not wait for
    // the first to finish, which is the scheduling freedom prepare() exposed.
    BOOST_TEST(tool->max_in_flight == 2);
    BOOST_TEST(tool->in_flight == 0);
}

BOOST_AUTO_TEST_CASE(a_prepared_call_run_alone_never_overlaps)
{
    // The other half of the same contract: a host that runs a serial call on
    // its own sees one invocation in flight, start to finish.
    auto tool = std::make_shared<SlowReadTool>();
    FakeToolSet set("local_tools", tool);

    model_io::InvokeQuery first_call = call_for("slow_read", "call_1");
    model_io::InvokeQuery second_call = call_for("slow_read", "call_2");
    const tools::ToolSet::ToolHandle first = set.prepare(first_call);
    const tools::ToolSet::ToolHandle second = set.prepare(second_call);

    BOOST_TEST(!tools::is_error(run_prepared(set, first, first_call)));
    BOOST_TEST(tool->max_in_flight == 1);

    BOOST_TEST(!tools::is_error(run_prepared(set, second, second_call)));
    // Still one at a time: the calls were awaited in sequence, not together.
    BOOST_TEST(tool->max_in_flight == 1);
}

// ===== the default policy, end to end through both phases ====================

BOOST_AUTO_TEST_CASE(a_tool_that_declares_nothing_is_denied_without_a_confirmer)
{
    auto tool = std::make_shared<MinimalTool>();
    FakeToolSet set("local_tools", tool);

    eventbus::AsyncEventBus& bus = eventbus::default_async_bus();
    BOOST_REQUIRE(bus.subscriber_count<InvokeConfirmEvent>() == 0u);

    model_io::InvokeQuery query = call_for("noop", "call_2");
    const tools::ToolSet::ToolHandle resolved = set.prepare(query);

    // Phase 1 succeeded — the default write_attributes() declared the cautious
    // pair, which is exactly what the scheduler reads...
    BOOST_REQUIRE(resolved != nullptr);
    BOOST_CHECK(query.type == model_io::InvokeType::SerialWrite);
    BOOST_CHECK(query.security == model_io::InvokeSecurity::RequireConfirm);

    // ... and phase 2 refused it, because nothing is subscribed to confirm.
    asio::io_context io;
    auto pending = asio::co_spawn(
        io, set.execute(resolved, query), asio::use_future);
    io.run();
    const model_io::InvokeReturn record = pending.get();

    BOOST_TEST(tools::is_error(record));
    BOOST_CHECK(stage_of(record) == InvokeException::Stage::SecurityCheck);
    BOOST_TEST(record.output.raw ==
               "Failed while validating the invocation's security: security check "
               "denied: no handler is subscribed to confirm the invocation "
               "(tool noop; call call_2)");
}

BOOST_AUTO_TEST_CASE(a_confirmer_on_the_process_wide_bus_lets_the_call_run)
{
    auto tool = std::make_shared<ConfirmableTool>();
    FakeToolSet set("local_tools", tool);

    eventbus::AsyncEventBus& bus = eventbus::default_async_bus();
    {
        // The host that answers: it approves, and remembers what it was shown.
        bool asked = false;
        model_io::InvokeQuery seen;
        eventbus::AsyncEventBus::ScopedSubscription subscription =
            bus.subscribe<InvokeConfirmEvent>(
                [&asked, &seen](const InvokeConfirmEvent& request)
                    -> asio::awaitable<InvokeConfirmEvent> {
                    asked = true;
                    seen = request.query;
                    InvokeConfirmEvent out = request;
                    out.decision = ConfirmDecision::Approved;
                    out.reason = "approved in the test";
                    co_return out;
                });

        const model_io::InvokeReturn record =
            run(set, call_for("confirmable", "call_2"));

        BOOST_TEST(!tools::is_error(record));
        BOOST_TEST(record.output.raw == "the tool ran");
        BOOST_TEST(asked);
        // The confirmer was shown the SETTLED call, attributes included.
        BOOST_TEST(seen.id == "call_2");
        BOOST_TEST(seen.name == "confirmable");
        BOOST_CHECK(seen.type == model_io::InvokeType::SerialWrite);
        BOOST_CHECK(seen.security == model_io::InvokeSecurity::RequireConfirm);
        BOOST_CHECK(record.query.security == model_io::InvokeSecurity::RequireConfirm);
    }

    // Scoped, so the process-wide bus is left as the tests found it.
    BOOST_TEST(bus.subscriber_count<InvokeConfirmEvent>() == 0u);
}

BOOST_AUTO_TEST_CASE(a_confirmer_that_throws_denies_at_the_security_stage)
{
    auto tool = std::make_shared<MinimalTool>();
    FakeToolSet set("local_tools", tool);

    eventbus::AsyncEventBus& bus = eventbus::default_async_bus();
    {
        eventbus::AsyncEventBus::ScopedSubscription subscription =
            bus.subscribe<InvokeConfirmEvent>(
                [](const InvokeConfirmEvent& request)
                    -> asio::awaitable<InvokeConfirmEvent> {
                    if (request.query.name == "noop") {
                        throw std::runtime_error("the confirmation dialog crashed");
                    }
                    co_return request;
                });

        const model_io::InvokeReturn record =
            run(set, call_for("noop", "call_2"));

        // The policy propagates a confirmer's failure, and the set reports it at
        // the stage that was running: a broken confirmer denies the call, with
        // its own message rather than a silent "nobody answered".
        BOOST_TEST(tools::is_error(record));
        BOOST_CHECK(stage_of(record) == InvokeException::Stage::SecurityCheck);
        BOOST_TEST(record.output.raw ==
                   "Failed while validating the invocation's security: "
                   "the confirmation dialog crashed (tool noop; call call_2)");
    }
    BOOST_TEST(bus.subscriber_count<InvokeConfirmEvent>() == 0u);
}

BOOST_AUTO_TEST_CASE(supported_names_lists_what_the_set_offers)
{
    auto tool = std::make_shared<ScriptedTool>();
    FakeToolSet set("local_tools", tool);

    const auto tools_offered = set.get_tools();
    BOOST_REQUIRE(tools_offered.size() == 1u);
    BOOST_TEST(tools_offered.front().name == "read_file");
    BOOST_TEST(tools_offered.front().description == "Reads a file.");

    const auto names = set.supported_names();
    BOOST_REQUIRE(names.size() == 1u);
    BOOST_TEST(names.front() == "read_file");

    FakeToolSet empty_set("empty_tools", nullptr);
    BOOST_TEST(empty_set.supported_names().empty());
    BOOST_TEST(empty_set.get_tools().empty());
}
