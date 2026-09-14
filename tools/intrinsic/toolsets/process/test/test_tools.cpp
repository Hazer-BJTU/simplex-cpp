#define BOOST_TEST_MODULE ProcessToolsTests
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/process/schemas.hpp"
#include "tools/intrinsic/process/toolset.hpp"
#include "tools/intrinsic/process/tools.hpp"
#include "tools/intrinsic/tool_declaration.hpp"

#include "tools/invoke_exception.hpp"
#include "tools/registry.hpp"
#include "tools/security_check.hpp"
#include "yamlconfig/yaml_json.hpp"

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
        // Then the context stopped and joined, and the table LAST. Killing a
        // leaked child needs no executor, so the store's destructor is just as
        // happy at this end — and this is the end it has to be at: the table's
        // strand shares refcounted state with the operations queued on the
        // context, and freeing it while a worker is still finishing one is a
        // data race in that refcount (ThreadSanitizer reports it as a race in
        // operator delete).
        guard.reset();
        io.stop();
        if (runner.joinable()) runner.join();
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

// ---- the declarations on disk -----------------------------------------------
//
// The six tools are DECLARED in schemas/*.yaml, one file per tool, next to this
// package's sources, and the case below is what keeps a declaration and the
// implementation from drifting apart. Every check in it is generic over the
// tools: the file states the properties, their types, their defaults, their
// enum members and which of them are required, and each check asks the
// IMPLEMENTATION the same question. A schema that stopped describing its tool
// therefore fails here, rather than quietly misinforming a model — which is the
// only thing that makes a declaration file safe to keep prose in.

namespace {

/// One tool and a call that must settle: the starting point every check below
/// perturbs.
struct DeclaredTool {
    std::string_view name;
    nlohmann::json minimal_arguments;
};

/// A value of the WRONG JSON kind for a property declaring `kind` — the one
/// disagreement between a schema and its implementation that can be found
/// without knowing anything about the tool. An unrecognised kind gets an
/// object, which is the wrong kind for every kind this project declares.
[[nodiscard]] nlohmann::json wrong_kind_for(std::string_view kind)
{
    if (kind == "string") return 7;
    if (kind == "boolean") return "yes";
    if (kind == "integer") return "soon";
    if (kind == "array") return "not a list";
    return nlohmann::json::object();
}

/// How phase 1 refused a call.
struct Refusal {
    InvokeException::Stage stage = InvokeException::Stage::Unknown;
    std::string message;
};

[[nodiscard]] Refusal refusal_of(Fixture& fixture, model_io::InvokeQuery& query)
{
    try {
        (void)fixture.prepare(query);
    } catch (const InvokeException& failure) {
        return Refusal{failure.stage(), failure.message()};
    }
    BOOST_FAIL("the call was expected to be refused");
    return {};
}

/// The Invocable a set offers under `name`.
[[nodiscard]] model_io::Invocable offered_as(const ProcessToolSet& set,
                                             std::string_view name)
{
    for (const model_io::Invocable& tool : set.get_tools()) {
        if (tool.name == name) return tool;
    }
    BOOST_FAIL("the set offers no tool named " << name);
    return {};
}

/// The InvokeType a declaration states for the reader.
///
/// Matched EXPLICITLY rather than through nlohmann's from_json, which is the
/// trap this whole pair of helpers exists for: NLOHMANN_JSON_SERIALIZE_ENUM
/// answers the FIRST enum value for a word it does not recognise, and for
/// InvokeType that is ReadOnly — the one direction dataclass/model_io.hpp
/// forbids, since an unrecognised type must be treated as serial.
[[nodiscard]] model_io::InvokeType invoke_type_of(std::string_view word)
{
    if (word == "read_only") return model_io::InvokeType::ReadOnly;
    if (word == "parall_write") return model_io::InvokeType::ParallWrite;
    if (word == "serial_write") return model_io::InvokeType::SerialWrite;
    BOOST_FAIL("not an InvokeType word: " << word);
    return model_io::InvokeType::SerialWrite;
}

[[nodiscard]] model_io::InvokeSecurity invoke_security_of(std::string_view word)
{
    if (word == "default_deny") return model_io::InvokeSecurity::DefaultDeny;
    if (word == "require_confirm") return model_io::InvokeSecurity::RequireConfirm;
    if (word == "trusted") return model_io::InvokeSecurity::Trusted;
    BOOST_FAIL("not an InvokeSecurity word: " << word);
    return model_io::InvokeSecurity::DefaultDeny;
}

} // namespace

BOOST_AUTO_TEST_CASE(every_tool_is_declared_by_its_own_yaml_file)
{
    Fixture f;

    const std::vector<DeclaredTool> declared{
        {tool_names::kSpawn, {{"executable", "true"}}},
        {tool_names::kPoll, nlohmann::json::object()},
        {tool_names::kRead, {{"session_id", "proc_1"}}},
        {tool_names::kWait, {{"session_id", "proc_1"}}},
        {tool_names::kWrite, {{"session_id", "proc_1"}, {"input", "x"}}},
        {tool_names::kKill, {{"session_id", "proc_1"}}},
    };

    // The directory holds exactly one file per tool. A declaration nothing
    // loads is a document nobody will notice going stale; a tool without one is
    // a tool the loader reports and the set then skips.
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(tools::intrinsic::schema_directory())) {
        if (entry.path().extension() != ".yaml") continue;
        const std::string stem = entry.path().stem().string();
        bool known = false;
        for (const DeclaredTool& tool : declared) {
            known = known || stem == tool.name;
        }
        BOOST_TEST_CONTEXT("unexpected declaration file " << entry.path()) {
            BOOST_TEST(known);
        }
    }
    std::size_t present = 0;
    for (const DeclaredTool& tool : declared) {
        present += std::filesystem::exists(tools::intrinsic::schema_directory()
                              / (std::string(tool.name) + ".yaml"))
                       ? 1u
                       : 0u;
    }
    BOOST_TEST(present == declared.size());

    for (const DeclaredTool& tool : declared) {
        const std::string name(tool.name);
        BOOST_TEST_CONTEXT("declaration of " << name) {
            const std::filesystem::path file = tools::intrinsic::schema_directory() / (name + ".yaml");
            const tools::intrinsic::ToolDeclaration declaration =
                tools::intrinsic::load_tool_declaration(file);
            const nlohmann::json& properties =
                declaration.argument_schema.at("properties");

            // (1) What the model is shown IS what the file says — verbatim, so
            //     that reading the file is reading the contract.
            const model_io::Invocable offered = offered_as(*f.set, tool.name);
            BOOST_TEST(offered.name == declaration.name);
            BOOST_TEST(offered.description == declaration.description);
            BOOST_TEST(offered.argument_schema == declaration.argument_schema);
            BOOST_TEST(offered.description.empty() == false);

            // (2) A call naming only what is required settles, every property
            //     is documented for the model, and every declared default is
            //     the value the implementation really settles — including the
            //     two numbers (5000 / 30000) a tool class also holds as a
            //     constant, which is what keeps the pair honest. A property the
            //     minimal call already names is skipped: there the settled
            //     value is the caller's, which is the other half of the
            //     contract — a value that is present is never overwritten.
            model_io::InvokeQuery query = call_for(name, tool.minimal_arguments);
            (void)f.prepare(query);
            for (const auto& property : properties.items()) {
                const nlohmann::json& schema = property.value();
                BOOST_TEST_CONTEXT("property " << property.key()) {
                    BOOST_TEST(schema.contains("type"));
                    BOOST_TEST(!schema.value("description", std::string()).empty());
                    if (schema.contains("default")
                        && !tool.minimal_arguments.contains(property.key())) {
                        // Guarded: a property the implementation does not
                        // settle at all should read as ONE failed expectation
                        // here, not as an out_of_range from the comparison that
                        // follows it.
                        BOOST_TEST(query.arguments.contains(property.key()));
                        if (query.arguments.contains(property.key())) {
                            BOOST_TEST(query.arguments.at(property.key())
                                       == schema.at("default"));
                        }
                    }
                }
            }
            // ... and it settles nothing the declaration does not name, so a
            // model reading the schema has been told about every field it will
            // see in the record.
            for (const auto& settled : query.arguments.items()) {
                BOOST_TEST_CONTEXT("settled " << settled.key()) {
                    BOOST_TEST(properties.contains(settled.key()));
                }
            }

            // (3) Every declared property is really validated, with the kind
            //     the file declares: a wrong JSON kind must be refused at
            //     ArgumentParse, and the message must name the property.
            for (const auto& property : properties.items()) {
                model_io::InvokeQuery probe =
                    call_for(name, tool.minimal_arguments);
                probe.arguments[property.key()] = wrong_kind_for(
                    property.value().at("type").get<std::string>());
                const Refusal refusal = refusal_of(f, probe);
                BOOST_TEST_CONTEXT("wrong kind for " << property.key()) {
                    BOOST_CHECK(refusal.stage
                               == InvokeException::Stage::ArgumentParse);
                    BOOST_TEST(refusal.message.find(property.key())
                               != std::string::npos);
                }
            }

            // (4) The enum members and the minimum the file states are the ones
            //     enforced — the clauses a model reads as its allowed values.
            for (const auto& property : properties.items()) {
                const nlohmann::json& schema = property.value();
                if (schema.contains("enum")) {
                    model_io::InvokeQuery probe =
                        call_for(name, tool.minimal_arguments);
                    probe.arguments[property.key()] = "__not_declared__";
                    BOOST_TEST_CONTEXT("value outside enum " << property.key()) {
                        BOOST_CHECK(refusal_of(f, probe).stage
                                   == InvokeException::Stage::ArgumentParse);
                    }
                }
                if (schema.contains("minimum")) {
                    model_io::InvokeQuery probe =
                        call_for(name, tool.minimal_arguments);
                    probe.arguments[property.key()] = -1;
                    BOOST_TEST_CONTEXT("value below minimum " << property.key()) {
                        BOOST_CHECK(refusal_of(f, probe).stage
                                   == InvokeException::Stage::ArgumentParse);
                    }
                }
            }

            // (5) What the file calls required is required: drop one from a
            //     call that otherwise settles and phase 1 must refuse it.
            for (const nlohmann::json& required :
                 declaration.argument_schema.at("required")) {
                model_io::InvokeQuery probe =
                    call_for(name, tool.minimal_arguments);
                probe.arguments.erase(required.get<std::string>());
                BOOST_TEST_CONTEXT("missing " << required) {
                    BOOST_CHECK(refusal_of(f, probe).stage
                               == InvokeException::Stage::ArgumentParse);
                }
            }

            // (6) The type/security pair the file states FOR THE READER is the
            //     pair the tool really declares. The loader ignores those keys
            //     on purpose — they are behaviour, and write_attributes() owns
            //     them — so this comparison is the only thing standing between
            //     a declaration and a wrong claim about what a call will do and
            //     whether it asks first.
            const nlohmann::json document = yamlconfig::load_file(file);
            BOOST_CHECK(invoke_type_of(document.at("type").get<std::string>())
                       == query.type);
            BOOST_CHECK(
                invoke_security_of(document.at("security").get<std::string>())
                == query.security);
        }
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
    // batch scheduler groups by and the security policy judges.
    //
    // The RULE, and the rows below are chosen to pin its edges: InvokeType
    // describes the effect a call has OUTSIDE this host. Internal bookkeeping
    // never makes a call a write — a delta read advancing a cursor and a poll
    // reaping a session are this layer's own state, and the store is what makes
    // them safe to overlap (table on the store's strand, handle and cursors on
    // the session's). What is a write is launching a program, putting bytes in
    // a live child's input, and ending it.
    struct Expectation {
        std::string_view name;
        nlohmann::json arguments;
        model_io::InvokeType type;
        model_io::InvokeSecurity security;
    };
    const Expectation expected[] = {
        // State changes outside this process: they ask, and they take the
        // executor to themselves.
        {tool_names::kSpawn, {{"executable", "true"}},
         model_io::InvokeType::SerialWrite,
         model_io::InvokeSecurity::RequireConfirm},
        {tool_names::kKill, {{"session_id", "proc_1"}},
         model_io::InvokeType::SerialWrite,
         model_io::InvokeSecurity::RequireConfirm},
        // Stdin writes change the outside too — the bytes ARE the child's next
        // input — so the call order is what the child reads, and one carrying
        // `close_input` can drop the other outright. SerialWrite. (What does
        // not decide it: the channel is thread-safe, so an overlap cannot
        // corrupt memory. That is the store doing its job.)
        {tool_names::kWrite, {{"session_id", "proc_1"}, {"input", "x"}},
         model_io::InvokeType::SerialWrite,
         model_io::InvokeSecurity::RequireConfirm},
        {tool_names::kWrite, {{"session_id", "proc_1"}, {"close_input", true}},
         model_io::InvokeType::SerialWrite,
         model_io::InvokeSecurity::RequireConfirm},

        // poll: ReadOnly for EVERY shape, including the two that move internal
        // state — consuming each session's new output and reaping the exited
        // ones. Those are the rows that matter here: if the rule ever drifts
        // back to "touches state, therefore a write", this is what fails.
        {tool_names::kPoll, nlohmann::json::object(),
         model_io::InvokeType::ReadOnly, model_io::InvokeSecurity::Trusted},
        {tool_names::kPoll, {{"include_output", true}, {"release_exited", false}},
         model_io::InvokeType::ReadOnly, model_io::InvokeSecurity::Trusted},
        {tool_names::kPoll, {{"include_output", false}, {"release_exited", true}},
         model_io::InvokeType::ReadOnly, model_io::InvokeSecurity::Trusted},
        {tool_names::kPoll, {{"session_ids", {"proc_1"}}, {"include_output", false}},
         model_io::InvokeType::ReadOnly, model_io::InvokeSecurity::Trusted},

        // read: ReadOnly for every shape too — the delta that consumes the
        // cursor and the read that releases the session included.
        {tool_names::kRead, {{"session_id", "proc_1"}},
         model_io::InvokeType::ReadOnly, model_io::InvokeSecurity::Trusted},
        {tool_names::kRead, {{"session_id", "proc_1"}, {"full", false}},
         model_io::InvokeType::ReadOnly, model_io::InvokeSecurity::Trusted},
        {tool_names::kRead,
         {{"session_id", "proc_1"}, {"full", true}, {"release", false}},
         model_io::InvokeType::ReadOnly, model_io::InvokeSecurity::Trusted},
        {tool_names::kRead,
         {{"session_id", "proc_1"}, {"full", true}, {"release", true}},
         model_io::InvokeType::ReadOnly, model_io::InvokeSecurity::Trusted},

        // wait: ReadOnly too. Waiting watches a child the host already started;
        // `release` removes the session, which is the table's business.
        {tool_names::kWait, {{"session_id", "proc_1"}},
         model_io::InvokeType::ReadOnly, model_io::InvokeSecurity::Trusted},
        {tool_names::kWait, {{"session_id", "proc_1"}, {"release", false}},
         model_io::InvokeType::ReadOnly, model_io::InvokeSecurity::Trusted},
        {tool_names::kWait, {{"session_id", "proc_1"}, {"release", true}},
         model_io::InvokeType::ReadOnly, model_io::InvokeSecurity::Trusted},
    };

    for (const Expectation& expectation : expected) {
        // The query starts with the OPPOSITE pair of what this row expects, so
        // neither assertion can pass by accident: a write_attributes() that
        // never ran would leave the wrong values in place and fail.
        model_io::InvokeQuery query =
            call_for(std::string(expectation.name), expectation.arguments);
        query.type = expectation.type == model_io::InvokeType::ReadOnly
                         ? model_io::InvokeType::SerialWrite
                         : model_io::InvokeType::ReadOnly;
        query.security =
            expectation.security == model_io::InvokeSecurity::Trusted
                ? model_io::InvokeSecurity::DefaultDeny
                : model_io::InvokeSecurity::Trusted;

        const auto tool = f.prepare(query);
        BOOST_TEST_REQUIRE(tool != nullptr);
        BOOST_TEST_CONTEXT("tool " << expectation.name << " with "
                                   << expectation.arguments.dump()) {
            BOOST_CHECK(query.type == expectation.type);
            BOOST_CHECK(query.security == expectation.security);
        }
    }
}

BOOST_AUTO_TEST_CASE(settling_materializes_the_defaults_into_the_query)
{
    // The settled query IS the call: it is what the security policy judges,
    // what a human confirmer is shown, what invoke() reads and what the record
    // carries back. A default the tool applied privately — validated but never
    // written into arguments — would mean all four of those see a different
    // call from the one that runs, so this asserts the DEFAULTS ARE IN THERE.
    Fixture f;
    struct Expectation {
        std::string_view name;
        nlohmann::json given;
        nlohmann::json settled;
    };
    const Expectation expected[] = {
        {tool_names::kSpawn,
         {{"executable", "echo"}},
         {{"executable", "echo"},
          {"arguments", nlohmann::json::array()},
          {"description", ""},
          {"environment", nlohmann::json::array()},
          {"inherit_environment", true},
          {"expected_runtime_milliseconds",
           tools::intrinsic::SpawnProcessTool::kDefaultExpectedRuntimeMilliseconds}}},
        {tool_names::kPoll,
         nlohmann::json::object(),
         {{"session_ids", nlohmann::json::array()},
          {"include_output", true},
          {"release_exited", false}}},
        {tool_names::kRead,
         {{"session_id", "proc_1"}},
         {{"session_id", "proc_1"},
          {"stream", "both"},
          {"full", false},
          {"release", false}}},
        {tool_names::kWrite,
         {{"session_id", "proc_1"}, {"close_input", true}},
         {{"session_id", "proc_1"},
          {"input", ""},
          {"close_input", true}}},
        {tool_names::kWait,
         {{"session_id", "proc_1"}},
         {{"session_id", "proc_1"},
          {"timeout_milliseconds", tools::intrinsic::WaitProcessTool::kDefaultTimeoutMilliseconds},
          {"release", false}}},
        {tool_names::kKill,
         {{"session_id", "proc_1"}},
         {{"session_id", "proc_1"}, {"graceful", false}}},
    };

    for (const Expectation& expectation : expected) {
        model_io::InvokeQuery query =
            call_for(std::string(expectation.name), expectation.given);
        (void)f.prepare(query);
        BOOST_TEST_CONTEXT("tool " << expectation.name) {
            // The whole object, not one key at a time: a property the tool
            // materialized that it should not have (a defaulted
            // working_directory, say) is as wrong as one it failed to
            // materialize, and only the full comparison catches both.
            BOOST_CHECK(query.arguments == expectation.settled);
        }
    }

    // The values the caller DID send are left exactly as they came, even when
    // they happen to equal the default — settling fills gaps, it does not
    // normalize.
    model_io::InvokeQuery named = call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", "proc_9"}, {"full", true},
                       {"stream", "stderr"}});
    (void)f.prepare(named);
    BOOST_CHECK(named.arguments ==
                nlohmann::json({{"session_id", "proc_9"}, {"full", true},
                                {"stream", "stderr"}, {"release", false}}));

    // working_directory is the one optional property with no default to write:
    // absent means "inherit the host's", and absent is how it stays.
    model_io::InvokeQuery directory = call_for(
        std::string(tool_names::kSpawn), nlohmann::json{{"executable", "true"}});
    (void)f.prepare(directory);
    BOOST_CHECK(!directory.arguments.contains("working_directory"));

    // And when the caller names one, it is untouched (no rewriting, no
    // normalization), which is what makes the confirmation and the launch
    // agree.
    model_io::InvokeQuery named_directory = call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{{"executable", "true"}, {"working_directory", "/tmp"}});
    (void)f.prepare(named_directory);
    BOOST_CHECK(named_directory.arguments.at("working_directory") ==
                nlohmann::json("/tmp"));
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
        // An EMPTY working directory is refused rather than read as "absent":
        // the two are different calls, and a child cannot be started in "".
        // The model's typo is a mistake it can fix, so it belongs here.
        {tool_names::kSpawn, {{"executable", "true"}, {"working_directory", ""}}},
        {tool_names::kSpawn, {{"executable", "true"},
                              {"inherit_environment", "yes"}}},
        // `arguments` that is not an object at all: every property would read
        // as absent and the call would run on defaults the model never chose,
        // so it is refused where the model can see why.
        {tool_names::kPoll, nlohmann::json::array({1, 2})},
        {tool_names::kPoll, "not an object"},
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
    // The two facts behind that pair, spelled out: the child is gone AND its
    // capture is complete, which is why the output below is the whole of it.
    BOOST_TEST(payload.at("output_complete") == nlohmann::json(true));
    BOOST_TEST(payload.at("state") == nlohmann::json("exited"));
    BOOST_TEST(payload.at("exit_code") == nlohmann::json(0));
    BOOST_TEST(payload.at("stdout_text") == nlohmann::json("hello tools\n"));
    BOOST_TEST(payload.at("stderr_text") == nlohmann::json(""));
}

BOOST_AUTO_TEST_CASE(wait_separates_a_finished_child_from_a_finished_capture)
{
    // The case the two fields exist for, and the one a single `exited` bool
    // gets wrong: the direct child exits at once, while a descendant it
    // started inherited stdout/stderr and keeps them open. The wait's deadline
    // then fires with the child GONE and its output still arriving.
    //
    // Reported as `exited: true, output_complete: false, timed_out: true` —
    // and `timed_out` is deliberately defined over the COMPLETE condition
    // rather than over the exit, so the three fields cannot disagree about
    // whether this result is the finished article.
    Fixture f;
    const std::string id =
        spawn_through_tool(f, "sh", {"-c", "sleep 2 & exit 0"});

    const nlohmann::json payload = f.payload_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 500}})));

    BOOST_TEST(payload.at("exited") == nlohmann::json(true));
    BOOST_TEST(payload.at("state") == nlohmann::json("exited"));
    BOOST_TEST(payload.at("exit_code") == nlohmann::json(0));
    BOOST_TEST(payload.at("output_complete") == nlohmann::json(false));
    BOOST_TEST(payload.at("timed_out") == nlohmann::json(true));
    // Prose as well as fields: the cause is not visible in the output itself.
    BOOST_TEST(payload.contains("hint"));

    // Waiting again collects the rest, once the descendant is gone: the flag is
    // a fact about the pipes, not a verdict on the session.
    const nlohmann::json finished = f.payload_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 10000}})));
    BOOST_TEST(finished.at("exited") == nlohmann::json(true));
    BOOST_TEST(finished.at("output_complete") == nlohmann::json(true));
    BOOST_TEST(finished.at("timed_out") == nlohmann::json(false));
}

BOOST_AUTO_TEST_CASE(a_spawn_that_finishes_with_an_incomplete_capture_says_so)
{
    // The same distinction one call earlier: spawn reports `finished` (a fact
    // about the child) and `output_complete` (a fact about its pipes)
    // separately, because `finished` alone would let a caller read the text
    // below as everything the child printed.
    Fixture f;
    const auto record = f.call(call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{{"executable", "sh"},
                       {"arguments", nlohmann::json::array(
                           {"-c", "sleep 2 & exit 0"})},
                       // Short enough that the child exits inside it and the
                       // drain cannot finish inside it.
                       {"expected_runtime_milliseconds", 500}}));

    const nlohmann::json payload = f.payload_of(record);
    BOOST_TEST(payload.at("finished") == nlohmann::json(true));
    BOOST_TEST(payload.at("output_complete") == nlohmann::json(false));
    BOOST_TEST(payload.at("state") == nlohmann::json("exited"));
    // The hint names the way to the rest of the output.
    BOOST_TEST(payload.at("hint").get<std::string>().find("wait_process") !=
               std::string::npos);
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
    // RETAINED, and the name says so: one of these two has already exited and
    // is still in the table (and still counted) until it is released — which is
    // also what the session cap counts.
    BOOST_TEST(first.at("retained_session_count") == nlohmann::json(2));

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
