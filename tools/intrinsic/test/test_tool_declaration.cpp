#define BOOST_TEST_MODULE ToolDeclarationTests
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/tool_declaration.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

// Tests for the declaration loader in tools/intrinsic/tool_declaration.hpp: the
// YAML document that states a tool's name, description and argument schema, and
// what the loader refuses.
//
// Two halves, and both matter equally. A well-formed file has to arrive whole
// and VERBATIM — `argument_schema` is what a provider puts on the wire, so a
// loader that quietly normalized it would change what a model is told. And a
// malformed one has to be refused with the file and the in-document path in the
// message, because the alternative to a loud refusal is a tool advertised with
// a schema nobody could read.
//
// No toolset is involved: the files here are written by the test, which is the
// only way to reach the shapes a real declaration should never have.

namespace fs = std::filesystem;
using tools::intrinsic::ToolDeclarationError;
using tools::intrinsic::load_tool_declaration;
using tools::intrinsic::try_load_tool_declaration;

namespace {

/// A scratch directory for one test process, with a writer that puts a document
/// where the loader will find it.
struct Scratch {
    fs::path directory;

    Scratch()
        : directory(fs::temp_directory_path()
                    / ("simplex_tool_declaration_" + std::to_string(::getpid())))
    {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
        fs::create_directories(directory);
    }

    ~Scratch()
    {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator = (const Scratch&) = delete;

    /// Write `text` to `name` under the scratch directory and answer its path.
    [[nodiscard]] fs::path write(std::string_view name, std::string_view text)
    {
        const fs::path file = directory / std::string(name);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
        BOOST_REQUIRE_MESSAGE(out.good(), "could not write " << file);
        return file;
    }
};

/// The message of the ToolDeclarationError `body` throws.
///
/// Most of what this loader owes a caller is WHICH file and WHAT is wrong with
/// it, so the cases below assert the path in the message rather than only the
/// exception's type — an error that does not say where it came from turns a
/// packaging mistake into a hunt.
template <class Body>
std::string refusal(Body&& body)
{
    try {
        body();
    } catch (const ToolDeclarationError& failure) {
        return failure.what();
    }
    BOOST_FAIL("expected a ToolDeclarationError");
    return {};
}

/// A declaration that is wrong in one way: `body` is the whole document.
[[nodiscard]] std::string refusal_of(Scratch& scratch, std::string_view body)
{
    const fs::path file = scratch.write("broken.yaml", body);
    return refusal([&] { (void)load_tool_declaration(file); });
}

[[nodiscard]] bool mentions(const std::string& message, std::string_view needle)
{
    return message.find(needle) != std::string::npos;
}

/// The smallest document that loads: what every refusal case below is a
/// mutation of.
constexpr std::string_view kMinimal = R"(
name: probe
description: a tool that exists only to be declared
argument_schema:
  type: object
  properties: {}
)";

} // namespace

// ---- what a good file gives the caller ---------------------------------------

BOOST_AUTO_TEST_CASE(a_complete_declaration_loads)
{
    Scratch scratch;
    // Everything a real declaration uses: comments, a folded block scalar, a
    // property list with descriptions and defaults, and the readability-only
    // keys a file may carry.
    const fs::path file = scratch.write("spawn_process.yaml", R"(
# The declaration of a tool that does not exist.
name: spawn_process
description: >-
  Run a program. Waits a short while for it to finish, so an ordinary command
  returns its exit code and its whole output in this one call.
type: serial_write
security: require_confirm
owner: nobody-in-particular
argument_schema:
  type: object
  required: [executable]
  properties:
    executable:
      type: string
      description: "Program to run: a name or a path"
    expected_runtime_milliseconds:
      type: integer
      minimum: 0
      default: 5000
      description: "How long to wait for the program before letting it continue"
)");

    const tools::intrinsic::ToolDeclaration declaration =
        load_tool_declaration(file);

    BOOST_TEST(declaration.name == "spawn_process");
    // The folded scalar arrives as one line, with the trailing newline gone.
    BOOST_TEST(declaration.description
               == "Run a program. Waits a short while for it to finish, so an "
                  "ordinary command returns its exit code and its whole output "
                  "in this one call.");
    BOOST_TEST(declaration.argument_schema.is_object());
    BOOST_TEST(declaration.argument_schema.at("type") == "object");
    // `type`, `security` and `owner` are NOT loaded: the first two are behaviour
    // (the tool's write_attributes() declares them) and an unrecognised key is
    // none of the loader's business. Nothing of either may leak into the schema
    // a provider would receive.
    BOOST_TEST(!declaration.argument_schema.contains("security"));
    BOOST_TEST(!declaration.argument_schema.contains("owner"));
}

BOOST_AUTO_TEST_CASE(the_argument_schema_arrives_verbatim)
{
    Scratch scratch;
    const fs::path file = scratch.write("schema.yaml", R"(
name: probe
description: a probe
argument_schema:
  type: object
  required: [session_id]
  properties:
    session_id:
      type: string
      description: "The session to read"
    stream:
      type: string
      enum: [stdout, stderr, both]
      default: both
      description: "Which stream to read"
    full:
      type: boolean
      default: false
      description: "Whether to return everything captured so far"
)");

    // The whole point of the file: this subtree goes on the wire as it stands
    // (llm/compat/*/interpreter.cpp puts it in `parameters`), so a loader that
    // re-derived or normalized any of it would change what a model is told.
    const nlohmann::json expected{
        {"type", "object"},
        {"required", nlohmann::json::array({"session_id"})},
        {"properties",
         nlohmann::json{
             {"session_id",
              nlohmann::json{{"type", "string"},
                             {"description", "The session to read"}}},
             {"stream",
              nlohmann::json{{"type", "string"},
                             {"enum", nlohmann::json::array(
                                          {"stdout", "stderr", "both"})},
                             {"default", "both"},
                             {"description", "Which stream to read"}}},
             {"full",
              nlohmann::json{
                  {"type", "boolean"},
                  {"default", false},
                  {"description", "Whether to return everything captured so far"}}},
         }},
    };

    BOOST_TEST(load_tool_declaration(file).argument_schema == expected);
}

BOOST_AUTO_TEST_CASE(a_literal_block_description_keeps_its_interior_lines)
{
    Scratch scratch;
    const fs::path file = scratch.write("literal.yaml", R"(
name: probe
description: |
  First line.
  Second line.
argument_schema:
  type: object
)");

    // `|` keeps the interior newlines and, unlike `>-`, a final one: the last
    // is trimmed (a description is prose, not a document), the first is the
    // author's formatting and stays.
    BOOST_TEST(load_tool_declaration(file).description
               == "First line.\nSecond line.");
}

BOOST_AUTO_TEST_CASE(an_empty_object_schema_is_legal)
{
    Scratch scratch;
    // A tool that takes no arguments says so with an empty property list, which
    // is a declaration, not a missing one.
    const fs::path file = scratch.write("none.yaml", R"(
name: probe
description: takes nothing
argument_schema:
  type: object
  required: []
  properties: {}
)");

    const tools::intrinsic::ToolDeclaration declaration =
        load_tool_declaration(file);
    BOOST_TEST(declaration.argument_schema.at("properties").empty());
    BOOST_TEST(declaration.argument_schema.at("required").empty());
}

BOOST_AUTO_TEST_CASE(the_whole_vocabulary_arrives_verbatim)
{
    Scratch scratch;
    // Every keyword the loader knows, in one document: the clauses a property
    // may carry, an array's elements, and the alternatives a call has to
    // satisfy. This is the shape the process toolset's cross-check leans on — a
    // schema whose clauses are worth holding against the implementation is
    // exactly one the loader had to understand first.
    const fs::path file = scratch.write("vocabulary.yaml", R"(
name: probe
description: a probe
argument_schema:
  type: object
  required: [session_id]
  properties:
    session_id:
      type: string
      minLength: 1
      description: "The session"
    stream:
      type: string
      enum: [stdout, stderr]
      default: stdout
      description: "Which stream"
    timeout_milliseconds:
      type: integer
      minimum: 0
      default: 30000
      description: "How long"
    arguments:
      type: array
      items:
        type: string
      default: []
      description: "The arguments"
    input:
      type: string
      default: ""
      description: "Text to send"
    close_input:
      type: boolean
      default: false
      description: "Whether to close"
  anyOf:
    - required: [input]
      properties:
        input:
          minLength: 1
    - required: [close_input]
      properties:
        close_input:
          enum: [true]
)");

    // The loader CHECKED every word below and changed none of them: what a
    // provider receives is what the file says.
    const nlohmann::json schema =
        load_tool_declaration(file).argument_schema;
    const nlohmann::json& properties = schema.at("properties");
    BOOST_TEST(properties.at("session_id").at("minLength") == 1);
    BOOST_TEST(properties.at("stream").at("enum")
               == nlohmann::json::array({"stdout", "stderr"}));
    BOOST_TEST(properties.at("stream").at("default") == "stdout");
    BOOST_TEST(properties.at("timeout_milliseconds").at("minimum") == 0);
    BOOST_TEST(properties.at("arguments").at("items").at("type") == "string");
    BOOST_TEST(properties.at("arguments").at("default")
               == nlohmann::json::array());
    BOOST_TEST(schema.at("anyOf").size() == 2u);
    BOOST_TEST(schema.at("anyOf").at(0).at("required")
               == nlohmann::json::array({"input"}));
    BOOST_TEST(
        schema.at("anyOf").at(0).at("properties").at("input").at("minLength")
        == 1);
    BOOST_TEST(schema.at("anyOf").at(1).at("properties").at("close_input")
                   .at("enum")
               == nlohmann::json::array({true}));
}

// ---- what a broken one gets --------------------------------------------------
//
// The vocabulary is closed on purpose (tool_declaration.hpp): `argument_schema`
// goes on the wire verbatim, so a keyword this loader let through unread would
// be one nothing downstream could notice was wrong. Every case below is a
// document that would otherwise reach a model as the contract for a call.

BOOST_AUTO_TEST_CASE(a_property_that_is_not_documented_is_refused)
{
    Scratch scratch;
    // The description is the whole of what a model is told about a property, so
    // a property without one is a field it has to guess at.
    const std::string missing = refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    executable:
      type: string
)");
    BOOST_TEST(mentions(missing,
                        "/argument_schema/properties/executable/description"));
    BOOST_TEST(mentions(missing, "is missing"));

    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    executable:
      type: string
      description: ""
)"), "must not be empty"));
}

BOOST_AUTO_TEST_CASE(a_type_outside_the_vocabulary_is_refused)
{
    Scratch scratch;
    // The typo this whole layer exists for: `stirng` would reach a provider as
    // a schema nobody can act on, and the implementation would be validating
    // the property against a rule the file never stated.
    const std::string typo = refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    executable:
      type: stirng
      description: the program to run
)");
    BOOST_TEST(mentions(typo, "/argument_schema/properties/executable/type"));
    BOOST_TEST(mentions(typo, "\"stirng\""));
    BOOST_TEST(mentions(typo, "string, boolean, integer, array"));

    // No type at all is the same failure one step earlier.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    executable:
      description: the program to run
)"), "must declare its type"));

    // A type that is not even a string, and the two kinds this tree has no
    // accessor for — nothing here reads a float or a nested object.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    executable:
      type: 7
      description: the program to run
)"), "must be a string"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    executable:
      type: number
      description: the program to run
)"), "\"number\""));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    executable:
      type: object
      description: the program to run
)"), "\"object\""));
}

BOOST_AUTO_TEST_CASE(a_keyword_outside_the_vocabulary_is_refused)
{
    Scratch scratch;
    // A keyword the loader does not know is one it cannot check, and one a
    // provider would be handed as part of the contract while the implementation
    // ignored it. `pattern` and `additionalProperties` are the two an author is
    // most likely to reach for.
    const std::string pattern = refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    executable:
      type: string
      description: the program to run
      pattern: "^[a-z]+$"
)");
    BOOST_TEST(mentions(pattern,
                        "/argument_schema/properties/executable/pattern"));
    BOOST_TEST(mentions(pattern, "not part of the argument-schema vocabulary"));

    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  additionalProperties: false
  properties: {}
)"), "/argument_schema/additionalProperties"));
}

BOOST_AUTO_TEST_CASE(clauses_that_disagree_with_their_type_are_refused)
{
    Scratch scratch;
    // An enum is a list of values of the declared kind ...
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    stream:
      type: string
      enum: stdout
      description: which stream
)"), "must be a non-empty array"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    stream:
      type: string
      enum: []
      description: which stream
)"), "must be a non-empty array"));
    const std::string member = refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    stream:
      type: string
      enum: [stdout, 3]
      description: which stream
)");
    BOOST_TEST(mentions(member, "/argument_schema/properties/stream/enum/1"));
    BOOST_TEST(mentions(member, "must be a string"));

    // ... a default is a value the declaration itself would let a caller send,
    // which the type, the enum and the minimum all have a say in ...
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    full:
      type: boolean
      default: "false"
      description: whether to read everything
)"), "must be a boolean"));
    const std::string outside = refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    stream:
      type: string
      enum: [stdout, stderr]
      default: both
      description: which stream
)");
    BOOST_TEST(mentions(outside, "/argument_schema/properties/stream/default"));
    BOOST_TEST(mentions(outside, "not one of the values"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    timeout_milliseconds:
      type: integer
      minimum: 0
      default: -1
      description: how long to wait
)"), "is below the minimum"));

    // ... and the clauses are held against each other, not only against the
    // type: an enum member its own minimum forbids is a document two readers
    // would take two ways.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    count:
      type: integer
      minimum: 3
      enum: [0, 5]
      description: how many
)"), "is below the minimum"));

    // A clause on the wrong kind says nothing about any call, so it is refused
    // rather than passed on as decoration.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    executable:
      type: string
      minimum: 0
      description: the program to run
)"), "minimum applies to an integer property"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    count:
      type: integer
      minLength: 2
      description: how many
)"), "minLength applies to a string property"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    count:
      type: integer
      minimum: 0.5
      description: how many
)"), "minimum must be an integer"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    executable:
      type: string
      minLength: -1
      description: the program to run
)"), "minLength must not be negative"));
}

BOOST_AUTO_TEST_CASE(integer_maximum_is_checked_against_other_clauses)
{
    Scratch scratch;
    const auto accepted = load_tool_declaration(scratch.write("bounded.yaml", R"(
name: bounded
description: integer bounds
argument_schema:
  type: object
  properties:
    count:
      type: integer
      description: how many
      minimum: 0
      maximum: 20
      default: 20
)"));
    BOOST_TEST(accepted.argument_schema.at("properties").at("count").at("maximum") == 20);

    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: bounded
description: integer bounds
argument_schema:
  type: object
  properties:
    count: {type: integer, description: count, maximum: 20, default: 21}
)"), "is above the maximum"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: bounded
description: integer bounds
argument_schema:
  type: object
  properties:
    count: {type: integer, description: count, maximum: 20, enum: [21]}
)"), "is above the maximum"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: bounded
description: integer bounds
argument_schema:
  type: object
  properties:
    count: {type: integer, description: count, minimum: 21, maximum: 20}
)"), "maximum must not be below minimum"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: bounded
description: integer bounds
argument_schema:
  type: object
  properties:
    count: {type: string, description: count, maximum: 20}
)"), "maximum applies to an integer property"));
}

BOOST_AUTO_TEST_CASE(an_array_must_declare_string_elements)
{
    Scratch scratch;
    // An array with no element rule is one a model can only guess about ...
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    arguments:
      type: array
      description: the arguments
)"), "must declare what its elements are"));

    // ... and the one array accessor in this tree reads strings, so anything
    // else is a rule nothing here could apply.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    arguments:
      type: array
      items: nonsense
      description: the arguments
)"), "items must be a mapping"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    arguments:
      type: array
      items:
        type: integer
      description: the arguments
)"), "elements must be strings"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    arguments:
      type: array
      items:
        type: string
        minLength: 1
      description: the arguments
)"), "not something this project declares about an array's elements"));

    // And `items` on something that is not an array describes nothing.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    executable:
      type: string
      items:
        type: string
      description: the program to run
)"), "only an array property takes items"));
}

BOOST_AUTO_TEST_CASE(an_any_of_branch_must_narrow_the_schema)
{
    Scratch scratch;
    // A branch is an alternative about the CALL, so it can only require
    // properties the schema declares ...
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    session_id:
      type: string
      description: the session
  anyOf: stdout
)"), "anyOf must be a non-empty array"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    session_id:
      type: string
      description: the session
  anyOf: []
)"), "anyOf must be a non-empty array"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    session_id:
      type: string
      description: the session
  anyOf: [stdout, stderr]
)"), "must be a mapping"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    session_id:
      type: string
      description: the session
  anyOf:
    - required: [nope]
)"), "no such property is declared"));

    // ... it must actually require something new: a branch satisfied by every
    // call the schema already allows would make the anyOf say nothing, and the
    // tests lean on that rule when they ask whether the required-only call is
    // valid.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  required: [session_id]
  properties:
    session_id:
      type: string
      description: the session
  anyOf:
    - required: [session_id]
)"), "must require a property the schema does not already require"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    session_id:
      type: string
      description: the session
  anyOf:
    - properties:
        session_id:
          minLength: 1
)"), "must require at least one property"));

    // What a branch states about a property is a NARROWING of the property the
    // schema declares: it cannot introduce one, it cannot restate the type or
    // the description, and it has to say something about the values.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  required: [session_id]
  properties:
    session_id:
      type: string
      description: the session
    input:
      type: string
      description: the text to send
  anyOf:
    - required: [input]
      properties:
        nope:
          minLength: 1
)"), "it does not introduce one"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  required: [session_id]
  properties:
    session_id:
      type: string
      description: the session
    input:
      type: string
      description: the text to send
  anyOf:
    - required: [input]
      properties:
        input:
          type: string
)"), "is not something an alternative may narrow"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  required: [session_id]
  properties:
    session_id:
      type: string
      description: the session
    input:
      type: string
      description: the text to send
  anyOf:
    - required: [input]
      properties:
        input: {}
)"), "a narrowing must state one of"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  required: [session_id]
  properties:
    session_id:
      type: string
      description: the session
    count:
      type: integer
      description: how many
  anyOf:
    - required: [count]
      properties:
        count:
          minLength: 1
)"), "minLength applies to a string property"));

    // A branch is a mapping with two keys, and nothing else.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a probe
argument_schema:
  type: object
  properties:
    session_id:
      type: string
      description: the session
  anyOf:
    - description: an alternative written as prose
      required: [session_id]
)"), "is not something an alternative may state"));
}


BOOST_AUTO_TEST_CASE(a_file_that_is_not_there_is_refused)
{
    Scratch scratch;
    const fs::path missing = scratch.directory / "absent.yaml";

    const std::string message =
        refusal([&] { (void)load_tool_declaration(missing); });
    // The path is what makes a packaging mistake findable.
    BOOST_TEST(mentions(message, "absent.yaml"));
}

BOOST_AUTO_TEST_CASE(yaml_that_does_not_parse_is_refused)
{
    Scratch scratch;
    const std::string message =
        refusal_of(scratch, "name: [unterminated\n");
    BOOST_TEST(mentions(message, "broken.yaml"));
}

BOOST_AUTO_TEST_CASE(two_documents_are_refused)
{
    Scratch scratch;
    // One file, one tool: `---` twice is a structural mistake, and yamlconfig
    // rejects it before this loader sees anything.
    const std::string message =
        refusal_of(scratch, "name: one\n---\nname: two\n");
    BOOST_TEST(mentions(message, "broken.yaml"));
}

BOOST_AUTO_TEST_CASE(a_document_that_is_not_a_mapping_is_refused)
{
    Scratch scratch;
    const std::string message = refusal_of(scratch, "- name: probe\n");
    BOOST_TEST(mentions(message, "must be a YAML mapping"));
}

BOOST_AUTO_TEST_CASE(an_empty_file_is_refused)
{
    Scratch scratch;
    // Deliberately not the "empty optional config" yamlconfig accepts: a file
    // with nothing in it declares no tool at all.
    const std::string message = refusal_of(scratch, "");
    BOOST_TEST(mentions(message, "must be a YAML mapping"));
}

BOOST_AUTO_TEST_CASE(a_name_that_is_missing_empty_or_not_a_string_is_refused)
{
    Scratch scratch;
    BOOST_TEST(mentions(refusal_of(scratch, R"(
description: no name here
argument_schema:
  type: object
)"), "/name"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: ""
description: an empty name
argument_schema:
  type: object
)"), "must not be empty"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: 42
description: a name that is not a name
argument_schema:
  type: object
)"), "must be a string"));
}

BOOST_AUTO_TEST_CASE(a_description_that_is_missing_or_empty_is_refused)
{
    Scratch scratch;
    // A tool the model is given with no prose is a tool it has to guess about,
    // so this is a refusal rather than an empty string.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
argument_schema:
  type: object
)"), "/description"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: ""
argument_schema:
  type: object
)"), "must not be empty"));
}

BOOST_AUTO_TEST_CASE(a_missing_or_malformed_schema_is_refused)
{
    Scratch scratch;
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: no schema
)"), "/argument_schema"));

    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a schema that is not a mapping
argument_schema: "type: object"
)"), "must be a mapping"));

    // Anything but an object schema: a tool takes one object of named
    // arguments, and a caller here has nowhere to put a different one.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a schema that is not an object schema
argument_schema:
  type: array
)"), "type: object"));
}

BOOST_AUTO_TEST_CASE(properties_that_are_not_a_mapping_of_mappings_are_refused)
{
    Scratch scratch;
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: properties is not a mapping
argument_schema:
  type: object
  properties: [executable]
)"), "/argument_schema/properties"));

    // A property written as a bare scalar is the shape a hurried edit
    // produces, and it would reach a model as a schema no provider accepts.
    const std::string message = refusal_of(scratch, R"(
name: probe
description: a property that is not a mapping
argument_schema:
  type: object
  properties:
    executable: just a string
)");
    BOOST_TEST(mentions(message, "/argument_schema/properties/executable"));
    BOOST_TEST(mentions(message, "must be a mapping"));
}

BOOST_AUTO_TEST_CASE(a_required_list_that_is_malformed_is_refused)
{
    Scratch scratch;
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: required is not a list
argument_schema:
  type: object
  properties: {}
  required: executable
)"), "required must be an array"));

    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: a required entry that is not a name
argument_schema:
  type: object
  properties: {}
  required: [7]
)"), "must be property names"));

    // The one that matters most: a required name nobody declared is a typo that
    // would otherwise reach a model as a field it can never fill in.
    const std::string message = refusal_of(scratch, R"(
name: probe
description: required names a property that does not exist
argument_schema:
  type: object
  required: [executable]
  properties:
    command:
      type: string
      description: the property that IS declared
)");
    BOOST_TEST(mentions(message, "/argument_schema/required/0"));
    BOOST_TEST(mentions(message, "\"executable\" is required"));
}

// ---- the reporting form ------------------------------------------------------

BOOST_AUTO_TEST_CASE(try_load_answers_a_good_file_and_reports_a_bad_one)
{
    Scratch scratch;
    const fs::path good = scratch.write("good.yaml", kMinimal);

    const std::optional<tools::intrinsic::ToolDeclaration> loaded =
        try_load_tool_declaration(good);
    BOOST_TEST_REQUIRE(loaded.has_value());
    BOOST_TEST(loaded->name == "probe");

    // A broken declaration is a packaging mistake, and the contract here is
    // that it costs the tool and not the host: no throw, no value, and an error
    // line naming the file (the visible half of this is the log the loader
    // writes; what a test can hold onto is the answer).
    const fs::path broken = scratch.write("broken.yaml", "name: [unterminated\n");
    BOOST_TEST(!try_load_tool_declaration(broken).has_value());

    const fs::path missing = scratch.directory / "absent.yaml";
    BOOST_TEST(!try_load_tool_declaration(missing).has_value());
}
