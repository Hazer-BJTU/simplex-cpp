#define BOOST_TEST_MODULE ProcessToolsTests
#include <boost/test/unit_test.hpp>

#include "result_text.hpp"

#include "tools/intrinsic/process/schemas.hpp"
#include "tools/intrinsic/process/toolset.hpp"
#include "tools/intrinsic/process/tools.hpp"
#include "tools/intrinsic/tool_declaration.hpp"

#include "tools/invoke_exception.hpp"
#include "tools/registry.hpp"
#include "tools/security_check.hpp"
#include "yamlconfig/yaml_json.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

// Tests for the five process tools: what each one answers, what it refuses and
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
using process_test::ResultText;
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

    /// One successful tool result, read back into the fields and blocks the
    /// model was given (result_text.hpp). Fails the case when the record is a
    /// failure record instead.
    process_test::ResultText result_of(const model_io::InvokeReturn& record)
    {
        BOOST_TEST_REQUIRE(!tools::is_error(record),
                           "expected a result, got a failure: "
                               << record.output.raw);
        return process_test::ResultText(record.output.raw);
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

/// Point the process toolset's declaration lookup at `directory` for as long as
/// this lives, and put the environment back exactly as it was.
///
/// schema_directory() reads SIMPLEX_PROCESS_SCHEMA_DIR on every call
/// (schemas.cpp), which is what makes a PACKAGE's shape testable in-process:
/// every tool built while this is alive loads its declaration from here, so the
/// set built from them is a set built over that package. Restoring the previous
/// value matters more than it looks — the cases share one process, and a
/// variable left behind would decide what the next case's tools are declared
/// from.
struct SchemaDirectoryOverride {
    std::optional<std::string> previous;

    explicit SchemaDirectoryOverride(const std::filesystem::path& directory)
    {
        if (const char* current = std::getenv("SIMPLEX_PROCESS_SCHEMA_DIR");
            current != nullptr) {
            previous = current;
        }
        ::setenv("SIMPLEX_PROCESS_SCHEMA_DIR", directory.c_str(), 1);
    }

    ~SchemaDirectoryOverride()
    {
        if (previous.has_value()) {
            ::setenv("SIMPLEX_PROCESS_SCHEMA_DIR", previous->c_str(), 1);
        } else {
            ::unsetenv("SIMPLEX_PROCESS_SCHEMA_DIR");
        }
    }

    SchemaDirectoryOverride(const SchemaDirectoryOverride&) = delete;
    SchemaDirectoryOverride& operator = (const SchemaDirectoryOverride&) = delete;
};

/// A package of declarations: `directory` holding a copy of exactly `tools`'s
/// files, taken from the real package — plus the set's skill.yaml, which is a
/// document of the same package rather than a seventh tool (skill_declaration.hpp).
/// Leaving it out would make every package built here one that also fails to
/// describe how its tools are used, which is a second failure these cases are
/// not about.
///
/// Call it BEFORE pointing the override at the result: it reads the files from
/// wherever schema_directory() answers at the time.
[[nodiscard]] std::filesystem::path package_with(
    const std::filesystem::path& directory,
    const std::vector<std::string_view>& tools)
{
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory, ignored);
    const std::filesystem::path source = tools::intrinsic::schema_directory();
    for (const std::string_view tool : tools) {
        const std::string name(tool);
        std::filesystem::copy_file(source / (name + ".yaml"),
                                   directory / (name + ".yaml"),
                                   std::filesystem::copy_options::overwrite_existing,
                                   ignored);
    }
    std::filesystem::copy_file(source / "skill.yaml", directory / "skill.yaml",
                               std::filesystem::copy_options::overwrite_existing,
                               ignored);
    return directory;
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
    return f.result_of(record).field("session_id");
}

} // namespace

// ---- the set itself ---------------------------------------------------------

BOOST_AUTO_TEST_CASE(the_set_offers_five_routable_tools)
{
    Fixture f;
    BOOST_TEST(f.set->name() == std::string_view("process"));

    const std::vector<model_io::Invocable> catalogue = f.set->get_tools();
    BOOST_TEST_REQUIRE(catalogue.size() == std::size_t{5});

    // Presentation order follows the workflow — launch, observe, then act on a
    // live child — because that order is what the model reads.
    BOOST_TEST(catalogue[0].name == std::string(tool_names::kSpawn));
    BOOST_TEST(catalogue[1].name == std::string(tool_names::kPoll));
    BOOST_TEST(catalogue[2].name == std::string(tool_names::kRead));
    BOOST_TEST(catalogue[3].name == std::string(tool_names::kWait));
    BOOST_TEST(catalogue[4].name == std::string(tool_names::kSend));

    // Every tool carries a description and an object schema: this is the whole
    // contract a model has to work from.
    for (const model_io::Invocable& tool : catalogue) {
        BOOST_TEST(!tool.description.empty());
        BOOST_TEST(tool.argument_schema.at("type") == nlohmann::json("object"));
        BOOST_TEST(tool.argument_schema.contains("properties"));
    }

    // The family is whole, and the set says so: the one capability group it
    // declares has every member and nothing missing.
    const std::vector<tools::intrinsic::IntrinsicToolSet::CapabilityGroup>
        groups = f.set->capability_groups();
    BOOST_TEST_REQUIRE(groups.size() == 1u);
    BOOST_TEST(groups[0].name == "process");
    BOOST_TEST(groups[0].missing.empty());
    BOOST_TEST(groups[0].registered.size() == std::size_t{5});
}

BOOST_AUTO_TEST_CASE(the_set_carries_its_skill_and_hands_it_to_a_prompt)
{
    Fixture f;

    // The one document in the package that is not a tool: how the five are used
    // TOGETHER, which is the thing no per-tool description can say
    // (tools/tool_skill.hpp).
    const std::optional<tools::ToolSetSkill> skill = f.set->skill();
    BOOST_TEST_REQUIRE(skill.has_value());
    BOOST_TEST(skill->name == "process");
    // Every field a host files a skill by, and the text a model reads.
    BOOST_TEST(!skill->title.empty());
    BOOST_TEST(!skill->description.empty());
    BOOST_TEST(!skill->keywords.empty());
    BOOST_TEST(!skill->text.empty());

    // What the skill is FOR, held against the set it belongs to: it must name
    // every tool the set actually registered. A tool renamed in code and not in
    // this file would otherwise leave a model following instructions about a
    // call that no longer exists — the same cross-check the declarations get
    // (every_tool_is_declared_by_its_own_yaml_file), one level up.
    for (const std::string& name : f.set->supported_names()) {
        BOOST_TEST_CONTEXT("the skill mentions " << name) {
            BOOST_TEST(skill->text.find(name) != std::string::npos);
        }
    }

    // And it reaches a prompt whole: one section, named after the skill,
    // carrying the file's text verbatim (which is the loader's promise about
    // `text`, and the reason the whole document is written by hand).
    model_io::PromptTemplate prompt;
    prompt.add_section("persona", "", "You are a helpful assistant.",
                       model_io::SectionStability::Immutable);
    BOOST_TEST(f.set->inject_skill(prompt));

    const auto injected = prompt.find(tools::skill_section_name(skill->name));
    BOOST_REQUIRE(injected != prompt.end());
    BOOST_TEST(injected->title == skill->title);
    BOOST_TEST(injected->text == skill->text);

    const std::string markdown = prompt.render().markdown;
    BOOST_TEST(markdown.find("You are a helpful assistant.") == 0u);
    BOOST_CHECK(markdown.find(skill->text) != std::string::npos);
    // After what the host had already said, never before it.
    BOOST_CHECK(markdown.find("You are a helpful assistant.")
                < markdown.find(skill->text));
}

BOOST_AUTO_TEST_CASE(a_package_missing_a_declaration_reports_a_degraded_family)
{
    // The state the capability group exists for: four tools that can start,
    // read and wait on a session and a fifth that never arrived. The set
    // registers the four — a broken declaration costs its own tool, and no more
    // (tool_declaration.hpp) — and reports the family in one place, rather than
    // leaving the operator to assemble it from four healthy tools and one odd
    // line.
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path()
        / ("simplex_partial_process_package_" + std::to_string(::getpid()));
    package_with(scratch, {tool_names::kSpawn, tool_names::kPoll,
                           tool_names::kRead, tool_names::kWait});
    {
        SchemaDirectoryOverride override(scratch);
        Fixture f;

        BOOST_TEST(f.set->tool_count() == 4u);
        BOOST_TEST(f.set->dispatch(call_for(std::string(tool_names::kSend)))
                   == nullptr);

        const std::vector<tools::intrinsic::IntrinsicToolSet::CapabilityGroup>
            groups = f.set->capability_groups();
        BOOST_TEST_REQUIRE(groups.size() == 1u);
        BOOST_TEST(groups[0].name == "process");
        BOOST_TEST(groups[0].registered.size() == 4u);
        BOOST_TEST_REQUIRE(groups[0].missing.size() == 1u);
        BOOST_TEST(groups[0].missing.front() == std::string(tool_names::kSend));
        // The report is the WHOLE family, not only its bad half: a host can say
        // which family is degraded as well as what is gone from it.
        BOOST_TEST(groups[0].registered.size() + groups[0].missing.size()
                   == std::size_t{5});
    }

    // The other quiet answer: a package carrying none of the declarations is
    // not a degraded set, it is a set that does not offer this family at all —
    // and the report says so rather than leaving five failures to be counted.
    const std::filesystem::path empty = scratch / "empty";
    std::error_code ignored;
    std::filesystem::create_directories(empty, ignored);
    {
        SchemaDirectoryOverride override(empty);
        Fixture f;

        BOOST_TEST(f.set->get_tools().empty());
        const std::vector<tools::intrinsic::IntrinsicToolSet::CapabilityGroup>
            groups = f.set->capability_groups();
        BOOST_TEST_REQUIRE(groups.size() == 1u);
        BOOST_TEST(groups[0].registered.empty());
        BOOST_TEST(groups[0].missing.size() == std::size_t{5});
    }

    std::filesystem::remove_all(scratch, ignored);
}

// ---- the declarations on disk -----------------------------------------------
//
// The five tools are DECLARED in schemas/*.yaml, one file per tool, next to this
// package's sources, and the case below is what keeps a declaration and the
// implementation from drifting apart. Every check in it is generic over the
// tools: the file states the properties, their types, their defaults, their
// enum members and which of them are required, and each check asks the
// IMPLEMENTATION the same question. A schema that stopped describing its tool
// therefore fails here, rather than quietly misinforming a model — which is the
// only thing that makes a declaration file safe to keep prose in.

namespace {

/// One tool and the call that names ONLY what its schema requires — the
/// smallest call the document allows, which the checks below perturb.
///
/// For one of the five it is not a call the implementation accepts at all:
/// send_process states a cross-property rule in `anyOf` (send something, close
/// the input, or name a signal), so the required-only call is invalid by the
/// document's own terms. The case asserts that refusal rather than padding the
/// call until it passes, which is what it used to do.
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

/// A value of `schema`'s declared kind that satisfies EVERY clause the schema
/// states — the witness a probe needs so that the clause under test is the only
/// thing it can be refused for.
///
/// Built from the schema and nothing else, on purpose: the whole claim the
/// cross-check makes is that a declaration is all a caller has to go on, so the
/// calls it builds have to be ones the declaration itself allows.
[[nodiscard]] nlohmann::json satisfying_value(const nlohmann::json& schema)
{
    if (schema.contains("enum")) {
        // The first member, which the declaration lists as allowed by
        // definition.
        return schema.at("enum").at(0);
    }
    const std::string kind = schema.at("type").get<std::string>();
    if (kind == "string") {
        // As long as minLength asks for, and one character when it asks for
        // nothing: the shortest string a caller can be sure is allowed.
        const auto shortest = schema.contains("minLength")
                                  ? schema.at("minLength").get<std::size_t>()
                                  : std::size_t{1};
        return std::string(std::max<std::size_t>(shortest, 1), 'x');
    }
    if (kind == "boolean") return true;
    if (kind == "integer") {
        return schema.contains("minimum") ? schema.at("minimum")
                                          : nlohmann::json(0);
    }
    if (kind == "array") {
        // One element rather than none: an empty array says nothing about the
        // element rule the declaration states.
        return nlohmann::json::array({satisfying_value(schema.at("items"))});
    }
    BOOST_FAIL("the loader accepts no such kind, so no such schema was loaded");
    return {};
}

/// The property's schema with one alternative's narrowing applied over it.
[[nodiscard]] nlohmann::json narrowed(const nlohmann::json& property,
                                      const nlohmann::json& narrowing)
{
    nlohmann::json merged = property;
    for (const auto& clause : narrowing.items()) {
        merged[clause.key()] = clause.value();
    }
    return merged;
}

/// The schema of one property as an alternative sees it: the property's own,
/// narrowed when the alternative states a clause about it.
[[nodiscard]] nlohmann::json as_narrowed_by(const nlohmann::json& properties,
                                            const nlohmann::json& branch,
                                            std::string_view name)
{
    const std::string key(name);
    const nlohmann::json& property = properties.at(key);
    const auto narrowed_properties = branch.find("properties");
    if (narrowed_properties == branch.end()
        || !narrowed_properties->contains(key)) {
        return property;
    }
    return narrowed(property, narrowed_properties->at(key));
}

/// A call that satisfies ONE alternative of the declaration's `anyOf`: the call
/// naming only what the schema requires, plus a satisfying value for every
/// property the alternative requires.
///
/// A value the base call already carried is OVERWRITTEN: what is being built is
/// a witness for this alternative, and a narrowing (`enum: [true]`) is exactly
/// the case where the base's value would not do.
[[nodiscard]] nlohmann::json witness_for(const nlohmann::json& properties,
                                         const nlohmann::json& branch,
                                         const nlohmann::json& base)
{
    nlohmann::json witness = base;
    for (const nlohmann::json& name : branch.at("required")) {
        const std::string key = name.get<std::string>();
        witness[key] = satisfying_value(as_narrowed_by(properties, branch, key));
    }
    return witness;
}

/// A value of the property's declared kind that its `enum` does NOT list — the
/// other half of "are these the same set?".
[[nodiscard]] nlohmann::json outside_enum(const nlohmann::json& schema)
{
    const nlohmann::json& listed = schema.at("enum");
    const std::string kind = schema.at("type").get<std::string>();
    if (kind == "string") return "__not_declared__";
    if (kind == "boolean") {
        if (listed.size() == 2u) {
            // An enum listing both booleans allows every value there is, so it
            // says nothing and has no outside. The loader does not forbid it;
            // this case would have nothing to probe.
            BOOST_FAIL("a boolean enum with both members says nothing");
        }
        return !listed.at(0).get<bool>();
    }
    if (kind == "integer") {
        // Counting up from the first member: a finite list of integers always
        // has one that is not in it.
        std::int64_t candidate = listed.at(0).get<std::int64_t>();
        while (std::find(listed.begin(), listed.end(),
                         nlohmann::json(candidate))
               != listed.end()) {
            ++candidate;
        }
        return candidate;
    }
    BOOST_FAIL("no enum of an " << kind << " property needs a probe");
    return {};
}

/// The two elements the `items` clause is probed with: one the implementation
/// accepts, one it refuses.
///
/// The accepted element comes from the schema wherever the schema is the whole
/// story. For ONE property here it is not: `environment` entries must be
/// "KEY=VALUE" strings, a rule no keyword in the vocabulary can state
/// (tool_declaration.hpp) — which is exactly why it is written down here, as an
/// exception with its own probe, instead of being silently skipped: the
/// schema-shaped element is the one the implementation REFUSES, and the pair
/// together is the proof that the rule exists and belongs to tools.cpp.
struct ElementProbe {
    nlohmann::json accepted;
    nlohmann::json refused;
};

[[nodiscard]] ElementProbe element_probe(std::string_view tool,
                                         std::string_view property,
                                         const nlohmann::json& items)
{
    if (tool == tool_names::kSpawn && property == "environment") {
        return ElementProbe{nlohmann::json::array({"KEY=VALUE"}),
                            nlohmann::json::array({satisfying_value(items)})};
    }
    return ElementProbe{
        nlohmann::json::array({satisfying_value(items)}),
        nlohmann::json::array({wrong_kind_for(items.at("type").get<std::string>())})};
}

/// How phase 1 refused a call, or nothing when it settled.
struct Refusal {
    InvokeException::Stage stage = InvokeException::Stage::Unknown;
    std::string message;
};

[[nodiscard]] std::optional<Refusal> try_prepare(Fixture& fixture,
                                                 model_io::InvokeQuery& query)
{
    try {
        (void)fixture.prepare(query);
    } catch (const InvokeException& failure) {
        return Refusal{failure.stage(), failure.message()};
    }
    return std::nullopt;
}

/// Whether the implementation refused the call's ARGUMENTS — its way of saying
/// "this is not a call I can run as written".
///
/// Anything else phase 1 did counts as the arguments being good: a call it
/// SETTLED, and one it refused later at the security gate (these probes run
/// prepare() alone, and a RequireConfirm tool asks there), are answers about
/// something other than the arguments, which is exactly what the clause probes
/// have to tell apart.
[[nodiscard]] bool arguments_refused(Fixture& fixture, model_io::InvokeQuery& query)
{
    const std::optional<Refusal> refusal = try_prepare(fixture, query);
    return refusal.has_value()
           && refusal->stage == InvokeException::Stage::ArgumentParse;
}

/// How phase 1 refused a call that must be refused, for the assertions that
/// need the stage and the message.
[[nodiscard]] Refusal refusal_of(Fixture& fixture, model_io::InvokeQuery& query)
{
    const std::optional<Refusal> refusal = try_prepare(fixture, query);
    if (!refusal.has_value()) {
        BOOST_FAIL("the call was expected to be refused");
        return {};
    }
    return *refusal;
}

/// `base` with one property set to `value`: how every clause probe below is
/// written.
[[nodiscard]] model_io::InvokeQuery probe_with(std::string_view name,
                                               const nlohmann::json& base,
                                               std::string_view property,
                                               nlohmann::json value)
{
    model_io::InvokeQuery probe = call_for(std::string(name), base);
    probe.arguments[std::string(property)] = std::move(value);
    return probe;
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

    // The smallest call each document allows. For send_process that is NOT a
    // call the implementation accepts: its declaration states a cross-property
    // rule in `anyOf`, and the case asserts the refusal (2) rather than padding
    // the call until it passes — a padded call would hide exactly the
    // disagreement it is here to catch.
    const std::vector<DeclaredTool> declared{
        {tool_names::kSpawn, {{"executable", "true"}}},
        {tool_names::kPoll, nlohmann::json::object()},
        {tool_names::kRead, {{"session_id", "proc_1"}}},
        {tool_names::kWait, {{"session_id", "proc_1"}}},
        {tool_names::kSend, {{"session_id", "proc_1"}}},
    };

    // The directory holds exactly one declaration file per tool — and one
    // document that is not a tool at all, the set's skill.yaml, which the next
    // case loads (a declaration nothing loads is a document nobody will notice
    // going stale; a tool without one is a tool the loader reports and the set
    // then skips).
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(tools::intrinsic::schema_directory())) {
        if (entry.path().extension() != ".yaml") continue;
        const std::string stem = entry.path().stem().string();
        bool known = stem == "skill";
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
            const nlohmann::json& schema = declaration.argument_schema;
            const nlohmann::json& properties = schema.at("properties");
            const nlohmann::json no_alternatives = nlohmann::json::array();
            const nlohmann::json& alternatives = schema.contains("anyOf")
                                                     ? schema.at("anyOf")
                                                     : no_alternatives;

            // (1) What the model is shown IS what the file says — verbatim, so
            //     that reading the file is reading the contract.
            const model_io::Invocable offered = offered_as(*f.set, tool.name);
            BOOST_TEST(offered.name == declaration.name);
            BOOST_TEST(offered.description == declaration.description);
            BOOST_TEST(offered.argument_schema == declaration.argument_schema);
            BOOST_TEST(offered.description.empty() == false);

            // (2) The calls the declaration ALLOWS are calls the implementation
            //     accepts, and each one settles the defaults the declaration
            //     states — including the two numbers (5000 / 30000) a tool
            //     class also holds as a constant, which is what keeps the pair
            //     honest.
            //
            //     A declaration with no `anyOf` allows exactly one such call:
            //     the one naming only what is required, and this asserts it
            //     settles. One WITH `anyOf` says outright that the
            //     required-only call is invalid (every branch must require a
            //     property `required` does not name, tool_declaration.hpp), so
            //     there the refusal is what gets asserted, and the calls that
            //     are allowed are the alternatives' witnesses.
            std::vector<nlohmann::json> allowed;
            if (alternatives.empty()) {
                allowed.push_back(tool.minimal_arguments);
            } else {
                for (const nlohmann::json& branch : alternatives) {
                    allowed.push_back(
                        witness_for(properties, branch, tool.minimal_arguments));
                }
            }
            model_io::InvokeQuery smallest =
                call_for(name, tool.minimal_arguments);
            BOOST_TEST_CONTEXT("a call naming only what is required") {
                if (alternatives.empty()) {
                    BOOST_TEST(!arguments_refused(f, smallest));
                } else {
                    // The document states a cross-property rule, so this call
                    // is invalid by its own terms — and the implementation
                    // agrees, which is the agreement that matters.
                    BOOST_TEST(arguments_refused(f, smallest));
                }
            }

            for (const nlohmann::json& arguments : allowed) {
                BOOST_TEST_CONTEXT("the call " << arguments.dump()) {
                    model_io::InvokeQuery query = call_for(name, arguments);
                    (void)f.prepare(query);
                    for (const auto& property : properties.items()) {
                        const nlohmann::json& property_schema = property.value();
                        BOOST_TEST_CONTEXT("property " << property.key()) {
                            BOOST_TEST(property_schema.contains("type"));
                            BOOST_TEST(!property_schema
                                            .value("description", std::string())
                                            .empty());
                            // A property this call already names is skipped:
                            // there the settled value is the caller's, which is
                            // the other half of the contract — a value that is
                            // present is never overwritten.
                            //
                            // For one the call leaves out, the default contract
                            // holds in BOTH directions: a property the
                            // declaration gives a default to is settled at that
                            // value, and one it does not is left alone, because
                            // the implementation settles nothing a model was not
                            // told about. The second half is what makes a
                            // DELETED `default:` visible — the settled query
                            // keeps carrying the value, and the guards below
                            // keep a missing key reading as ONE failed
                            // expectation rather than an out_of_range from the
                            // comparison that follows it.
                            if (!arguments.contains(property.key())) {
                                if (property_schema.contains("default")) {
                                    BOOST_TEST(
                                        query.arguments.contains(property.key()));
                                    if (query.arguments.contains(
                                            property.key())) {
                                        BOOST_TEST(
                                            query.arguments.at(property.key())
                                            == property_schema.at("default"));
                                    }
                                } else {
                                    BOOST_TEST(!query.arguments.contains(
                                        property.key()));
                                }
                            }
                        }
                    }
                    // ... and it settles nothing the declaration does not name,
                    // so a model reading the schema has been told about every
                    // field it will see in the record.
                    for (const auto& settled : query.arguments.items()) {
                        BOOST_TEST_CONTEXT("settled " << settled.key()) {
                            BOOST_TEST(properties.contains(settled.key()));
                        }
                    }
                }
            }
            // The call the checks below perturb: the first one the declaration
            // allows, so the rest of it is beyond complaint whatever the clause
            // under test is.
            const nlohmann::json& valid = allowed.front();

            // (3) Every declared property is really validated, with the kind
            //     the file declares: a wrong JSON kind must be refused at
            //     ArgumentParse, and the message must name the property.
            for (const auto& property : properties.items()) {
                model_io::InvokeQuery probe =
                    probe_with(name, valid, property.key(),
                              wrong_kind_for(property.value().at("type")
                                                 .get<std::string>()));
                const Refusal refusal = refusal_of(f, probe);
                BOOST_TEST_CONTEXT("wrong kind for " << property.key()) {
                    BOOST_CHECK(refusal.stage
                               == InvokeException::Stage::ArgumentParse);
                    BOOST_TEST(refusal.message.find(property.key())
                               != std::string::npos);
                }
            }

            // (4) Every VALUE CLAUSE the file states is probed from both sides:
            //     a value the clause allows is a call the implementation
            //     accepts, and one it does not is refused. One-sided probes are
            //     what this used to do, and they cannot tell "the implementation
            //     restricts something here" from "the declaration and the
            //     implementation agree" — a `minimum: 1000` would have passed a
            //     probe that only ever tried -1, and an `enum: [foo, bar]` a
            //     probe that only ever tried a value outside both sets.
            for (const auto& property : properties.items()) {
                const nlohmann::json& property_schema = property.value();

                if (property_schema.contains("enum")) {
                    // EVERY declared member, not just one: the members are the
                    // set the declaration promises.
                    for (const nlohmann::json& member :
                         property_schema.at("enum")) {
                        model_io::InvokeQuery probe =
                            probe_with(name, valid, property.key(), member);
                        BOOST_TEST_CONTEXT("declared member " << member
                                                              << " of "
                                                              << property.key()) {
                            BOOST_TEST(!arguments_refused(f, probe));
                        }
                    }
                    model_io::InvokeQuery probe = probe_with(
                        name, valid, property.key(), outside_enum(property_schema));
                    BOOST_TEST_CONTEXT("value outside the enum "
                                       << property.key()) {
                        BOOST_TEST(arguments_refused(f, probe));
                    }
                }

                if (property_schema.contains("minimum")) {
                    // The boundary itself, and one below it: the two together
                    // pin WHERE it is, not merely that there is one.
                    const std::int64_t minimum =
                        property_schema.at("minimum").get<std::int64_t>();
                    model_io::InvokeQuery at_minimum = probe_with(
                        name, valid, property.key(), minimum);
                    BOOST_TEST_CONTEXT("value at the minimum " << minimum
                                                               << " of "
                                                               << property.key()) {
                        BOOST_TEST(!arguments_refused(f, at_minimum));
                    }
                    model_io::InvokeQuery below = probe_with(
                        name, valid, property.key(), minimum - 1);
                    BOOST_TEST_CONTEXT("value below the minimum " << minimum
                                                                  << " of "
                                                                  << property.key()) {
                        BOOST_TEST(arguments_refused(f, below));
                    }
                }

                if (property_schema.contains("minLength")) {
                    const auto shortest = static_cast<std::size_t>(
                        property_schema.at("minLength").get<std::int64_t>());
                    model_io::InvokeQuery at_length = probe_with(
                        name, valid, property.key(),
                        std::string(std::max<std::size_t>(shortest, 1), 'x'));
                    BOOST_TEST_CONTEXT("a string of exactly minLength for "
                                       << property.key()) {
                        BOOST_TEST(!arguments_refused(f, at_length));
                    }
                    if (shortest > 0) {
                        model_io::InvokeQuery shorter = probe_with(
                            name, valid, property.key(),
                            std::string(shortest - 1, 'x'));
                        BOOST_TEST_CONTEXT("a string of minLength - 1 for "
                                           << property.key()) {
                            BOOST_TEST(arguments_refused(f, shorter));
                        }
                    }
                }

                if (property_schema.contains("items")) {
                    // An element of the declared kind, and one that is not: the
                    // outer `type: array` alone says nothing about either.
                    const ElementProbe elements = element_probe(
                        tool.name, property.key(), property_schema.at("items"));
                    model_io::InvokeQuery accepted = probe_with(
                        name, valid, property.key(), elements.accepted);
                    BOOST_TEST_CONTEXT("accepted elements for "
                                       << property.key()) {
                        BOOST_TEST(!arguments_refused(f, accepted));
                    }
                    model_io::InvokeQuery refused = probe_with(
                        name, valid, property.key(), elements.refused);
                    BOOST_TEST_CONTEXT("refused elements for "
                                       << property.key()) {
                        BOOST_TEST(arguments_refused(f, refused));
                    }
                }
            }

            // (4b) Each `anyOf` alternative is a shape of call the
            //      implementation accepts — the other half of (2), and the half
            //      that keeps a branch from being a claim about a call the tool
            //      would refuse.
            for (std::size_t index = 0; index < alternatives.size(); ++index) {
                model_io::InvokeQuery probe = call_for(
                    name, witness_for(properties, alternatives.at(index),
                                      tool.minimal_arguments));
                BOOST_TEST_CONTEXT("alternative " << index << " of the anyOf") {
                    BOOST_TEST(!arguments_refused(f, probe));
                }
            }

            // (5) What the file calls required is required: drop one from a
            //     call that otherwise settles and phase 1 must refuse it.
            for (const nlohmann::json& required : schema.at("required")) {
                model_io::InvokeQuery probe = call_for(name, valid);
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
            model_io::InvokeQuery settled = call_for(name, valid);
            (void)f.prepare(settled);
            BOOST_CHECK(invoke_type_of(document.at("type").get<std::string>())
                       == settled.type);
            BOOST_CHECK(
                invoke_security_of(document.at("security").get<std::string>())
                == settled.security);
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
        {tool_names::kSend, {{"session_id", "proc_1"}, {"signal", "kill"}},
         model_io::InvokeType::SerialWrite,
         model_io::InvokeSecurity::RequireConfirm},
        // Stdin writes change the outside too — the bytes ARE the child's next
        // input — so the call order is what the child reads, and one carrying
        // `close_input` can drop the other outright. SerialWrite. (What does
        // not decide it: the channel is thread-safe, so an overlap cannot
        // corrupt memory. That is the store doing its job.)
        {tool_names::kSend, {{"session_id", "proc_1"}, {"input", "x"}},
         model_io::InvokeType::SerialWrite,
         model_io::InvokeSecurity::RequireConfirm},
        {tool_names::kSend, {{"session_id", "proc_1"}, {"close_input", true}},
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
        {tool_names::kSend,
         {{"session_id", "proc_1"}, {"close_input", true}},
         {{"session_id", "proc_1"},
          {"input", ""},
          {"close_input", true},
          {"signal", ""}}},
        {tool_names::kWait,
         {{"session_id", "proc_1"}},
         {{"session_id", "proc_1"},
          {"timeout_milliseconds", tools::intrinsic::WaitProcessTool::kDefaultTimeoutMilliseconds},
          {"release", false}}},
        {tool_names::kSend,
         {{"session_id", "proc_1"}, {"signal", "term"}},
         {{"session_id", "proc_1"},
          {"input", ""},
          {"close_input", false},
          {"signal", "term"}}},
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
        // send: a call that would neither send, close nor signal does nothing
        // at all; a signal outside the declared words is refused by name.
        {tool_names::kSend, {{"session_id", "proc_1"}}},
        {tool_names::kSend, {{"session_id", "proc_1"}, {"input", 5}}},
        {tool_names::kSend, {{"session_id", "proc_1"}, {"close_input", 1}}},
        {tool_names::kSend,
         {{"session_id", "proc_1"}, {"signal", "SIGKILL"}}},
        {tool_names::kSend, {{"session_id", "proc_1"}, {"signal", "none"}}},
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
        {tool_names::kSend, {{"session_id", "proc_404"}, {"input", "x"}}},
        {tool_names::kSend, {{"session_id", "proc_404"}, {"signal", "kill"}}},
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

    const ResultText result = f.result_of(record);
    BOOST_TEST(result.field("session_id") == "proc_1");
    BOOST_TEST(result.field("executable") == "echo");
    BOOST_TEST(result.field("description") == "a quick command");
    BOOST_TEST(result.field("finished") == "true");
    BOOST_TEST(result.field("state") == "exited");
    BOOST_TEST(result.field("exit_code") == "0");
    BOOST_TEST(std::stoi(result.field("pid")) > 0);
    // The output comes back with it — the whole point of having waited — and
    // it arrives as the bytes the child printed, not as an escaped string
    // inside one.
    BOOST_TEST(result.block("stdout") == "quick\n");
    BOOST_TEST(result.block("stderr").empty());
    BOOST_TEST(result.has("hint"));
    // The result is one text part, and the child's own output is what is in it.
    BOOST_CHECK(record.output.type == model_io::ContentType::Text);

    // A full read was used, so the delta cursor is untouched: a later read
    // still reports everything rather than finding it already consumed.
    const auto read = f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", "proc_1"}, {"stream", "stdout"}}));
    BOOST_TEST(f.result_of(read).block("stdout") == "quick\n");
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

    const ResultText result = f.result_of(record);
    BOOST_TEST(result.field("session_id") == "proc_1");
    BOOST_TEST(result.field("finished") == "false");
    BOOST_TEST(result.field("state") == "running");
    BOOST_TEST(std::stoi(result.field("pid")) > 0);
    // Still running, so there is no exit code and no output slice the caller
    // did not ask for.
    BOOST_TEST(!result.has("exit_code"));
    BOOST_TEST(!result.has("stdout"));
    BOOST_TEST(result.has("hint"));
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

    const ResultText result = f.result_of(record);
    BOOST_TEST(result.field("finished") == "false");
    BOOST_TEST(result.field("state") == "running");
}

BOOST_AUTO_TEST_CASE(wait_reports_the_exit_and_the_whole_output)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "echo", {"hello", "tools"});

    const auto record = f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}}));

    const ResultText result = f.result_of(record);
    BOOST_TEST(result.field("exited") == "true");
    BOOST_TEST(result.field("timed_out") == "false");
    // The two facts behind that pair, spelled out: the child is gone AND its
    // capture is complete, which is why the output below is the whole of it.
    BOOST_TEST(result.field("output_complete") == "true");
    BOOST_TEST(result.field("state") == "exited");
    BOOST_TEST(result.field("exit_code") == "0");
    BOOST_TEST(result.block("stdout") == "hello tools\n");
    BOOST_TEST(result.block("stderr").empty());
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

    const ResultText result = f.result_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 500}})));

    BOOST_TEST(result.field("exited") == "true");
    BOOST_TEST(result.field("state") == "exited");
    BOOST_TEST(result.field("exit_code") == "0");
    BOOST_TEST(result.field("output_complete") == "false");
    BOOST_TEST(result.field("timed_out") == "true");
    // Prose as well as fields: the cause is not visible in the output itself.
    BOOST_TEST(result.has("hint"));

    // Waiting again collects the rest, once the descendant is gone: the flag is
    // a fact about the pipes, not a verdict on the session.
    const ResultText finished = f.result_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 10000}})));
    BOOST_TEST(finished.field("exited") == "true");
    BOOST_TEST(finished.field("output_complete") == "true");
    BOOST_TEST(finished.field("timed_out") == "false");
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

    const ResultText result = f.result_of(record);
    BOOST_TEST(result.field("finished") == "true");
    BOOST_TEST(result.field("output_complete") == "false");
    BOOST_TEST(result.field("state") == "exited");
    // The hint names the way to the rest of the output.
    BOOST_TEST(result.field("hint").find("wait_process") != std::string::npos);
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
    const ResultText result = f.result_of(record);
    BOOST_TEST(result.field("exited") == "false");
    BOOST_TEST(result.field("timed_out") == "true");
    BOOST_TEST(result.field("state") == "running");
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
    const ResultText result = f.result_of(record);
    BOOST_TEST(result.field("exit_code") == "1");
    BOOST_TEST(result.field("exited") == "true");
}

BOOST_AUTO_TEST_CASE(read_returns_the_delta_then_nothing_and_full_repeats_it)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "echo", {"once"});
    f.call(call_for(std::string(tool_names::kWait),
                    nlohmann::json{{"session_id", id},
                                   {"timeout_milliseconds", 5000}}));

    const ResultText first = f.result_of(f.call(call_for(
        std::string(tool_names::kRead), nlohmann::json{{"session_id", id}})));
    BOOST_TEST(first.block("stdout") == "once\n");
    BOOST_TEST(first.field("stdout_bytes_read") == "5");

    // The property a poll loop relies on: a second delta read is empty — which
    // is what the block says rather than what a missing line would.
    const ResultText second = f.result_of(f.call(call_for(
        std::string(tool_names::kRead), nlohmann::json{{"session_id", id}})));
    BOOST_TEST(second.block("stdout").empty());

    // full re-reads everything and does NOT consume the delta.
    const ResultText full = f.result_of(f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", id}, {"full", true}})));
    BOOST_TEST(full.block("stdout") == "once\n");
    BOOST_TEST(full.field("full") == "true");
}

BOOST_AUTO_TEST_CASE(read_can_select_one_stream)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "echo", {"out"});
    f.call(call_for(std::string(tool_names::kWait),
                    nlohmann::json{{"session_id", id},
                                   {"timeout_milliseconds", 5000}}));

    const ResultText only_err = f.result_of(f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", id}, {"stream", "stderr"}})));
    // Only the stream that was asked for is in the result — a caller reading
    // stderr is not handed stdout it did not ask for (and whose delta cursor
    // it would then have consumed). Both are visible: stderr as a block that
    // is empty, stdout as no line at all.
    BOOST_TEST(only_err.field("stream") == "stderr");
    BOOST_TEST(only_err.has("stderr"));
    BOOST_TEST(only_err.block("stderr").empty());
    BOOST_TEST(!only_err.has("stdout"));

    const ResultText only_out = f.result_of(f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", id}, {"stream", "stdout"}})));
    BOOST_TEST(only_out.block("stdout") == "out\n");
    BOOST_TEST(!only_out.has("stderr"));
}

BOOST_AUTO_TEST_CASE(poll_reports_every_session_and_only_new_output)
{
    Fixture f;
    const std::string finished = spawn_through_tool(f, "echo", {"done"});
    const std::string running = spawn_through_tool(f, "sleep", {"30"});
    f.call(call_for(std::string(tool_names::kWait),
                    nlohmann::json{{"session_id", finished},
                                   {"timeout_milliseconds", 5000}}));

    const ResultText first = f.result_of(
        f.call(call_for(std::string(tool_names::kPoll))));
    // One record per session, after the count: the `---` the renderer writes
    // is what makes a poll readable when it answers about several at once.
    BOOST_TEST_REQUIRE(first.records().size() == std::size_t{3});
    // RETAINED, and the name says so: one of these two has already exited and
    // is still in the table (and still counted) until it is released — which is
    // also what the session cap counts.
    BOOST_TEST(first.field("retained_session_count") == "2");

    // Sorted by id, so a poll loop's output reads the same way every turn.
    const process_test::ResultRecord& done_entry = first.records()[1];
    BOOST_TEST(done_entry.field("session_id") == finished);
    BOOST_TEST(done_entry.field("state") == "exited");
    BOOST_TEST(done_entry.block("new_stdout") == "done\n");
    BOOST_TEST(first.records()[2].field("state") == "running");

    // Polled again: the same sessions, but nothing NEW to report — which the
    // empty block says, rather than a line that is not there.
    const ResultText second = f.result_of(
        f.call(call_for(std::string(tool_names::kPoll))));
    BOOST_TEST(second.records()[1].block("new_stdout").empty());

    // A named subset, without output.
    const ResultText subset = f.result_of(f.call(call_for(
        std::string(tool_names::kPoll),
        nlohmann::json{{"session_ids", nlohmann::json::array({running})},
                       {"include_output", false}})));
    BOOST_TEST_REQUIRE(subset.records().size() == std::size_t{2});
    BOOST_TEST(subset.records()[1].field("session_id") == running);
    BOOST_TEST(!subset.records()[1].has("new_stdout"));
}

BOOST_AUTO_TEST_CASE(poll_releases_exited_sessions_only_after_reporting_them)
{
    Fixture f;
    const std::string finished = spawn_through_tool(f, "echo", {"last words"});
    const std::string running = spawn_through_tool(f, "sleep", {"30"});
    f.call(call_for(std::string(tool_names::kWait),
                    nlohmann::json{{"session_id", finished},
                                   {"timeout_milliseconds", 5000}}));

    const ResultText result = f.result_of(f.call(call_for(
        std::string(tool_names::kPoll),
        nlohmann::json{{"release_exited", true}})));

    // The output is in THIS result — reaping before reading would have lost a
    // dead child's last words for good. Two session records, then the one
    // naming what was let go.
    BOOST_TEST_REQUIRE(result.records().size() == std::size_t{4});
    BOOST_TEST(result.records()[1].block("new_stdout") == "last words\n");
    BOOST_TEST(result.field("released") == "[" + std::string("\"") + finished +
                                           "\"]");

    // Gone now; the running one is untouched, since release refuses a live
    // child.
    const ResultText after = f.result_of(
        f.call(call_for(std::string(tool_names::kPoll))));
    BOOST_TEST_REQUIRE(after.records().size() == std::size_t{2});
    BOOST_TEST(after.records()[1].field("session_id") == running);
}

BOOST_AUTO_TEST_CASE(release_is_refused_while_the_process_runs)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "sleep", {"30"});
    const ResultText result = f.result_of(f.call(call_for(
        std::string(tool_names::kRead),
        nlohmann::json{{"session_id", id}, {"release", true}})));

    // Honest about what happened rather than about what was asked: releasing
    // a live session would drop the last handle reference and kill the child.
    BOOST_TEST(result.field("released") == "false");
    BOOST_TEST(f.run(f.store->size()) == std::size_t{1});
}

BOOST_AUTO_TEST_CASE(send_feeds_stdin_and_close_input_ends_it)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "cat");

    const ResultText first = f.result_of(f.call(call_for(
        std::string(tool_names::kSend),
        nlohmann::json{{"session_id", id}, {"input", "one\n"}})));
    BOOST_TEST(first.field("bytes_queued") == "4");
    BOOST_TEST(first.field("input_closed") == "false");
    // What was not asked for is not reported: no signal, no signal lines.
    BOOST_TEST(!first.has("signal"));
    BOOST_TEST(!first.has("signalled"));
    // Queued, not delivered: the result must not claim the child has read it.
    BOOST_TEST(first.has("note"));

    const ResultText closing = f.result_of(f.call(call_for(
        std::string(tool_names::kSend),
        nlohmann::json{{"session_id", id},
                       {"input", "two\n"},
                       {"close_input", true}})));
    BOOST_TEST(closing.field("input_closed") == "true");

    // cat echoes both lines and exits on the EOF the close produced.
    const ResultText waited = f.result_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    BOOST_TEST(waited.field("exit_code") == "0");
    BOOST_TEST(waited.block("stdout") == "one\ntwo\n");
}

BOOST_AUTO_TEST_CASE(send_ends_a_running_process_and_leaves_it_readable)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "sleep", {"30"});

    const ResultText killed = f.result_of(f.call(call_for(
        std::string(tool_names::kSend),
        nlohmann::json{{"session_id", id}, {"signal", "kill"}})));
    BOOST_TEST(killed.field("signal") == "kill");
    BOOST_TEST(killed.field("signalled") == "true");
    // A signal-only call says nothing about input it was not asked to send.
    BOOST_TEST(!killed.has("bytes_queued"));
    BOOST_TEST(!killed.has("input_closed"));

    const ResultText waited = f.result_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    BOOST_TEST(waited.field("exited") == "true");
    // SIGKILL, as the signal number.
    BOOST_TEST(waited.field("exit_code") == "9");

    // Sending again says so instead of failing: the session is still there,
    // its child simply is not.
    const ResultText again = f.result_of(f.call(call_for(
        std::string(tool_names::kSend),
        nlohmann::json{{"session_id", id}, {"signal", "kill"}})));
    BOOST_TEST(again.field("signalled") == "false");
    BOOST_TEST(again.has("warning"));
}

BOOST_AUTO_TEST_CASE(send_asks_the_process_to_stop_gracefully)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "sleep", {"30"});
    const ResultText result = f.result_of(f.call(call_for(
        std::string(tool_names::kSend),
        nlohmann::json{{"session_id", id}, {"signal", "term"}})));
    BOOST_TEST(result.field("signal") == "term");
    BOOST_TEST(result.field("signalled") == "true");
    // The signal is sent, but the death is noticed a moment later — the result
    // says how to confirm it rather than implying the child is already gone.
    BOOST_TEST(result.has("hint"));

    const ResultText waited = f.result_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    // sleep does not catch SIGTERM, so it dies of the signal (15) — which is
    // the difference between "term" and "kill", reported where it can be seen.
    BOOST_TEST(waited.field("exit_code") == "15");
}

BOOST_AUTO_TEST_CASE(send_carries_input_and_a_signal_in_one_call)
{
    Fixture f;
    // A program that reads a line and then stays up: the one call feeds it and
    // asks it to stop, which is the case two tools could not spell.
    const std::string id = spawn_through_tool(f, "sh", {"-c", "read line; sleep 30"});

    const ResultText sent = f.result_of(f.call(call_for(
        std::string(tool_names::kSend),
        nlohmann::json{{"session_id", id}, {"input", "stop\n"},
                       {"signal", "term"}})));
    BOOST_TEST(sent.field("bytes_queued") == "5");
    BOOST_TEST(sent.field("signal") == "term");
    BOOST_TEST(sent.field("signalled") == "true");

    const ResultText waited = f.result_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    BOOST_TEST(waited.field("exited") == "true");
}

BOOST_AUTO_TEST_CASE(sending_to_an_exited_process_warns_rather_than_fails)
{
    Fixture f;
    const std::string id = spawn_through_tool(f, "true");
    f.call(call_for(std::string(tool_names::kWait),
                    nlohmann::json{{"session_id", id},
                                   {"timeout_milliseconds", 5000}}));

    // The session exists, so neither half is a failure — but nothing sent had
    // anywhere to go, and nothing else in the result would reveal that.
    const ResultText wrote = f.result_of(f.call(call_for(
        std::string(tool_names::kSend),
        nlohmann::json{{"session_id", id}, {"input", "ignored\n"}})));
    BOOST_TEST(wrote.field("state") == "exited");
    BOOST_TEST(wrote.field("warning")
               == "the process has already exited, so the input was discarded");

    const ResultText signalled = f.result_of(f.call(call_for(
        std::string(tool_names::kSend),
        nlohmann::json{{"session_id", id}, {"signal", "term"}})));
    BOOST_TEST(signalled.field("signalled") == "false");
    BOOST_TEST(signalled.field("warning")
               == "the process has already exited, so no signal was sent");

    // Both halves in one call: the warning names what was lost, in one line.
    const ResultText both = f.result_of(f.call(call_for(
        std::string(tool_names::kSend),
        nlohmann::json{{"session_id", id}, {"input", "ignored\n"},
                       {"signal", "kill"}})));
    BOOST_TEST(both.field("warning")
               == "the process has already exited, so the input was discarded "
                  "and no signal was sent");
}

BOOST_AUTO_TEST_CASE(spawn_honours_the_working_directory_and_environment)
{
    Fixture f;
    const std::string temp =
        std::filesystem::canonical(std::filesystem::temp_directory_path())
            .string();

    const std::string id = f.result_of(f.call(call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{{"executable", "pwd"},
                       {"arguments", nlohmann::json::array({"-P"})},
                       {"working_directory", temp}}))).field("session_id");
    const ResultText waited = f.result_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", id}, {"timeout_milliseconds", 5000}})));
    BOOST_TEST(waited.block("stdout") == temp + "\n");
    BOOST_TEST(waited.field("working_directory") == temp);

    const std::string env_id = f.result_of(f.call(call_for(
        std::string(tool_names::kSpawn),
        nlohmann::json{
            {"executable", "sh"},
            {"arguments", nlohmann::json::array({"-c", "printf %s \"$MARKER\""})},
            {"environment",
             nlohmann::json::array({"MARKER=sentinel-value"})}}))).field("session_id");
    const ResultText env_waited = f.result_of(f.call(call_for(
        std::string(tool_names::kWait),
        nlohmann::json{{"session_id", env_id},
                       {"timeout_milliseconds", 5000}})));
    BOOST_TEST(env_waited.block("stdout") == "sentinel-value");
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
    BOOST_TEST(registry.get_tools().size() == std::size_t{5});
    BOOST_TEST(registry.contains(std::string(tool_names::kSpawn)));
    BOOST_TEST(registry.contains(std::string(tool_names::kSend)));

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
        ResultText(first_batch[0].output.raw).field("session_id");

    // Turn 2: feed it, end its input, wait, and read — the whole workflow.
    auto second_batch = f.run(registry.execute(std::vector<model_io::InvokeQuery>{
        call_for(std::string(tool_names::kSend),
                 nlohmann::json{{"session_id", id},
                                {"input", "through the registry\n"},
                                {"close_input", true}}, "call_send"),
    }));
    BOOST_TEST_REQUIRE(!tools::is_error(second_batch[0]));

    auto third_batch = f.run(registry.execute(std::vector<model_io::InvokeQuery>{
        call_for(std::string(tool_names::kWait),
                 nlohmann::json{{"session_id", id},
                                {"timeout_milliseconds", 5000}}, "call_wait"),
    }));
    BOOST_TEST_REQUIRE(!tools::is_error(third_batch[0]));
    const ResultText waited(third_batch[0].output.raw);
    BOOST_TEST(waited.field("exited") == "true");
    BOOST_TEST(waited.block("stdout") == "through the registry\n");

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
