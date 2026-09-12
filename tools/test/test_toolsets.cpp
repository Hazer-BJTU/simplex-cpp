#define BOOST_TEST_MODULE ToolSetTests
#include <boost/test/unit_test.hpp>

#include "tools/toolsets.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace asio = boost::asio;
using tools::ConfirmDecision;
using tools::InvokeConfirmEvent;
using tools::InvokeException;

namespace {

/// A call as the conversation carries it: the id correlates the result, the
/// name is what dispatch() resolves, and the arguments are what
/// ensure_arguments() settles. The type and security on it are deliberately the
/// record's defaults (ReadOnly / DefaultDeny) rather than what the tool will
/// declare: write_attributes() overwrites them, and the tests assert the
/// settled values, so a write_attributes() that never ran cannot pass.
model_io::InvokeQuery read_call()
{
    model_io::InvokeQuery query;
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::DefaultDeny;
    query.id = "call_1";
    query.name = "read_file";
    query.arguments = {{"path", "/etc/hosts"}};
    return query;
}

/// The call the minimal tool (below) is invoked with.
model_io::InvokeQuery noop_call()
{
    model_io::InvokeQuery query;
    query.id = "call_2";
    query.name = "noop";
    return query;
}

/// Run one call through the set and hand back its record. handle() is a
/// coroutine, so it needs an executor even though nothing here waits on
/// anything but the hooks.
model_io::InvokeReturn run(tools::ToolSet& set, model_io::InvokeQuery query)
{
    asio::io_context io;
    auto pending =
        asio::co_spawn(io, set.handle(std::move(query)), asio::use_future);
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
/// failure path handle() has to report.
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
    /// What security_check() answers when it is not the failing hook.
    bool security_passes = true;
    std::string security_reason = "the invocation was not confirmed";
    /// What write_attributes() declares about the call.
    model_io::InvokeType declares_type = model_io::InvokeType::ReadOnly;
    model_io::InvokeSecurity declares_security = model_io::InvokeSecurity::Trusted;
    /// What a successful invoke() answers.
    std::string payload = "127.0.0.1 localhost";

    // What the call did, observed as it ran. Mutable because the hooks that
    // observe are the const ones.
    mutable bool ensure_ran = false;
    mutable bool write_attributes_ran = false;
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

    /// The stage a real tool would raise at this hook — and what handle() has
    /// to report it at, since it must agree with the tool.
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

/// A set of at most one tool: a test decides what a call resolves to — the
/// tool, nothing (the Dispatch failure), or a resolver that fails.
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

} // namespace

BOOST_AUTO_TEST_CASE(a_trusted_call_runs_and_its_record_carries_the_settled_query)
{
    auto tool = std::make_shared<ScriptedTool>();
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    BOOST_TEST(!tools::is_error(record));
    BOOST_CHECK(record.output.type == model_io::ContentType::Text);
    BOOST_TEST(record.output.raw == "127.0.0.1 localhost");
    // The record answers the call: its identity survives the whole sequence...
    BOOST_TEST(record.query.id == "call_1");
    BOOST_TEST(record.query.name == "read_file");
    BOOST_TEST(record.query.arguments["path"] == "/etc/hosts");
    // ... in its SETTLED form: write_attributes() overwrote the type and
    // security the request arrived with.
    BOOST_CHECK(record.query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(record.query.security == model_io::InvokeSecurity::Trusted);
    // A successful result is the tool's own record, with nothing added.
    BOOST_TEST(!record.extras.has_value());

    // Every checkpoint ran, on the settled query, and check_result() saw the
    // output invoke() produced.
    BOOST_TEST(tool->ensure_ran);
    BOOST_TEST(tool->write_attributes_ran);
    BOOST_TEST(tool->invoke_ran);
    BOOST_TEST(tool->check_result_ran);
    BOOST_TEST(tool->check_result_query_id == "call_1");
    BOOST_TEST(tool->check_result_output == "127.0.0.1 localhost");

    // handle() runs a call, not the tool's lifecycle: build() and release() are
    // the owning set's business, once per tool instance, and take no query.
    BOOST_TEST(tool->build_calls == 0);
    BOOST_TEST(tool->release_calls == 0);
}

BOOST_AUTO_TEST_CASE(an_unresolved_call_is_a_dispatch_failure)
{
    FakeToolSet set("local_tools", nullptr);

    const model_io::InvokeReturn record = run(set, read_call());

    BOOST_TEST(tools::is_error(record));
    BOOST_CHECK(stage_of(record) == InvokeException::Stage::Dispatch);
    BOOST_TEST(record.output.raw ==
               "Failed while dispatching the invocation to a tool: no tool named "
               "\"read_file\" in toolset \"local_tools\" (tool read_file; call call_1)");
    // The record still answers the call the model made.
    BOOST_TEST(record.query.id == "call_1");
    BOOST_TEST(record.query.name == "read_file");
}

BOOST_AUTO_TEST_CASE(a_dispatch_that_throws_is_reported_and_not_fatal)
{
    auto tool = std::make_shared<ScriptedTool>();
    FakeToolSet set("local_tools", tool);
    set.dispatch_fails = true;

    const model_io::InvokeReturn record = run(set, read_call());

    // Resolving a name may fail — a map, a plugin registry, a remote catalogue.
    // That is a Dispatch failure for the model, not a std::terminate().
    BOOST_TEST(tools::is_error(record));
    BOOST_CHECK(stage_of(record) == InvokeException::Stage::Dispatch);
    BOOST_TEST(record.output.raw ==
               "Failed while dispatching the invocation to a tool: "
               "the tool registry is unreachable (tool read_file; call call_1)");
    // The tool itself was never reached.
    BOOST_TEST(!tool->ensure_ran);
}

BOOST_AUTO_TEST_CASE(a_tools_own_argument_failure_reaches_the_model)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->fail_in = ScriptedTool::Hook::EnsureArguments;
    tool->throw_as = ScriptedTool::Throw::InvokeExceptionWithQuery;
    tool->failure = "missing required property \"path\"";
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    BOOST_TEST(tools::is_error(record));
    BOOST_CHECK(stage_of(record) == InvokeException::Stage::ArgumentParse);
    BOOST_TEST(record.output.raw ==
               "Failed while parsing the invocation arguments: "
               "missing required property \"path\" (tool read_file; call call_1)");
    // The bare message is what the marker carries, for a host that classifies
    // the failure without parsing the prose.
    BOOST_REQUIRE(record.extras.has_value());
    BOOST_TEST(record.extras->at("error").at("message") ==
               "missing required property \"path\"");
    BOOST_TEST(!tool->invoke_ran);
}

BOOST_AUTO_TEST_CASE(a_write_attributes_failure_shares_the_argument_stage)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->fail_in = ScriptedTool::Hook::WriteAttributes;
    tool->failure = "the tool cannot classify this call";
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    BOOST_TEST(tools::is_error(record));
    // The same phase as ensure_arguments: settling the query before any check
    // runs, which is why it is the same stage.
    BOOST_CHECK(stage_of(record) == InvokeException::Stage::ArgumentParse);
    BOOST_TEST(record.output.raw ==
               "Failed while parsing the invocation arguments: "
               "the tool cannot classify this call (tool read_file; call call_1)");
}

BOOST_AUTO_TEST_CASE(a_tool_exception_without_a_query_is_rebuilt_around_the_call)
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

BOOST_AUTO_TEST_CASE(a_query_the_tool_carried_itself_wins)
{
    auto tool = std::make_shared<ScriptedTool>();
    tool->fail_in = ScriptedTool::Hook::Invoke;
    tool->throw_as = ScriptedTool::Throw::InvokeExceptionWithQuery;
    tool->own_query_id = "inner_call";
    FakeToolSet set("local_tools", tool);

    const model_io::InvokeReturn record = run(set, read_call());

    // A tool may fail on a call of its own making — a retry, a nested
    // invocation — and that is the call the record answers.
    BOOST_TEST(record.query.id == "inner_call");
    BOOST_CHECK(stage_of(record) == InvokeException::Stage::Invoke);
    BOOST_TEST(record.output.raw ==
               "Failed while invoking the tool: the checkpoint failed "
               "(tool read_file; call inner_call)");
}

BOOST_AUTO_TEST_CASE(a_security_refusal_is_reported_with_the_tools_words)
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

BOOST_AUTO_TEST_CASE(a_throwing_security_check_is_reported_at_its_stage)
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

BOOST_AUTO_TEST_CASE(an_invoke_failure_is_reported_at_the_invoke_stage)
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

BOOST_AUTO_TEST_CASE(a_result_check_failure_still_correlates_to_the_call)
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

BOOST_AUTO_TEST_CASE(a_throw_that_is_not_a_std_exception_is_still_reported)
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

BOOST_AUTO_TEST_CASE(a_tool_that_declares_nothing_is_denied_without_a_confirmer)
{
    auto tool = std::make_shared<MinimalTool>();
    FakeToolSet set("local_tools", tool);

    eventbus::AsyncEventBus& bus = eventbus::default_async_bus();
    BOOST_REQUIRE(bus.subscriber_count<InvokeConfirmEvent>() == 0u);

    const model_io::InvokeReturn record = run(set, noop_call());

    // The default write_attributes() declared the cautious pair and the default
    // security_check() refused it, because nothing is subscribed to confirm.
    BOOST_TEST(tools::is_error(record));
    BOOST_CHECK(stage_of(record) == InvokeException::Stage::SecurityCheck);
    BOOST_TEST(record.output.raw ==
               "Failed while validating the invocation's security: security check "
               "denied: no handler is subscribed to confirm the invocation "
               "(tool noop; call call_2)");
    BOOST_CHECK(record.query.type == model_io::InvokeType::SerialWrite);
    BOOST_CHECK(record.query.security == model_io::InvokeSecurity::RequireConfirm);
}

BOOST_AUTO_TEST_CASE(a_tool_that_implements_no_invoke_refuses_at_that_checkpoint)
{
    auto tool = std::make_shared<TrustedStubTool>();
    FakeToolSet set("local_tools", tool);

    model_io::InvokeQuery query = noop_call();
    query.name = "stub";
    const model_io::InvokeReturn record = run(set, std::move(query));

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

        model_io::InvokeQuery query = noop_call();
        query.name = "confirmable";
        const model_io::InvokeReturn record = run(set, std::move(query));

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

        const model_io::InvokeReturn record = run(set, noop_call());

        // The policy propagates a confirmer's failure, and handle() reports it
        // at the checkpoint that was running: a broken confirmer denies the
        // call, with its own message rather than a silent "nobody answered".
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
