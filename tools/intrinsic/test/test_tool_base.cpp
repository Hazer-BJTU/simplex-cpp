#define BOOST_TEST_MODULE IntrinsicToolBaseTests
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/tool_base.hpp"
#include "tools/intrinsic/tool_result.hpp"
#include "tools/intrinsic/toolset_base.hpp"

#include "tools/invoke_exception.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <nlohmann/json.hpp>

// Tests for the shared core every intrinsic toolset derives from, with no
// toolset in sight: the argument accessors' accept/refuse rules, the JSON
// result shape, the schema builders, the bus a confirmation is routed at, and
// the toolset base's catalogue/routing/lifecycle.
//
// Worth testing here rather than only through a toolset, because these are the
// pieces a SECOND family will inherit unchanged — a regression in them would
// surface as a puzzling failure in whichever toolset happened to notice first.

namespace asio = boost::asio;
using tools::ConfirmDecision;
using tools::InvokeConfirmEvent;
using tools::InvokeException;
using tools::intrinsic::IntrinsicTool;
using tools::intrinsic::IntrinsicToolSet;
using tools::intrinsic::ToolResult;

namespace {

/// A tool that exposes the protected accessors so the cases can drive them
/// directly. Every reader takes the same shape: read one property off the
/// query, hand it back as JSON.
class ProbeTool final : public IntrinsicTool {
public:
    explicit ProbeTool(std::string name, eventbus::AsyncEventBus* bus = nullptr)
        : IntrinsicTool(bus)
    {
        _details.name = std::move(name);
        _details.description = "a probe";
        _details.argument_schema = object_schema(
            nlohmann::json{{"text", string_property("some text")}},
            {"text"});
    }

    /// Set per case: what invoke() should answer with — the result text
    /// itself, so a case can shape it any way it likes. Unset means an empty
    /// text part.
    std::function<model_io::Content(const model_io::InvokeQuery&)> body;

    /// Whether build() should refuse — the toolset base must then leave this
    /// tool out entirely.
    bool refuse_build = false;

    bool build() noexcept override { return !refuse_build; }

    void write_attributes(model_io::InvokeQuery& query) const override
    {
        query.type = model_io::InvokeType::ReadOnly;
        query.security = declared_security;
    }

    model_io::InvokeSecurity declared_security =
        model_io::InvokeSecurity::Trusted;

    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override
    {
        co_return body ? body(query) : ToolResult{}.render();
    }

    // The accessors under test, reachable from a case.
    using IntrinsicTool::bool_property;
    using IntrinsicTool::enum_property;
    using IntrinsicTool::find_argument;
    using IntrinsicTool::object_schema;
    using IntrinsicTool::optional_bool;
    using IntrinsicTool::optional_string;
    using IntrinsicTool::optional_string_list;
    using IntrinsicTool::optional_uint;
    using IntrinsicTool::require_string;
    using IntrinsicTool::settle_bool;
    using IntrinsicTool::settle_string;
    using IntrinsicTool::settle_string_list;
    using IntrinsicTool::settle_uint;
    using IntrinsicTool::string_list_property;
    using IntrinsicTool::string_property;
    using IntrinsicTool::uint_property;
    using IntrinsicTool::write_argument;
};

/// A minimal set over whatever tools a case gives it — the toolset base with
/// nothing else on top.
class ProbeSet final : public IntrinsicToolSet {
public:
    explicit ProbeSet(std::vector<ToolHandle> tools)
    {
        register_tools(std::move(tools));
    }

    std::string_view name() const noexcept override { return "probe"; }
};

model_io::InvokeQuery query_with(nlohmann::json arguments)
{
    model_io::InvokeQuery query;
    query.id = "call_1";
    query.name = "probe";
    query.arguments = std::move(arguments);
    return query;
}

/// The message of the ArgumentParse failure `read` raises, or "" when it
/// raises nothing. The message IS the product here: it is what a model reads
/// to fix its own call.
std::string refusal_message(const std::function<void()>& read)
{
    try {
        read();
    } catch (const InvokeException& failure) {
        BOOST_CHECK(failure.stage() == InvokeException::Stage::ArgumentParse);
        return failure.message();
    }
    return {};
}

} // namespace

// ---- argument accessors -----------------------------------------------------

BOOST_AUTO_TEST_CASE(null_reads_as_absent_like_a_missing_key)
{
    // The data contract's rule (dataclass/model_io.hpp, protocol rules 3+6):
    // a JSON null under an optional key is ABSENT, not an engaged value and
    // not a type error.
    const model_io::InvokeQuery query = query_with({
        {"present", "value"},
        {"nulled", nullptr},
    });

    BOOST_TEST(ProbeTool::find_argument(query, "present") != nullptr);
    BOOST_TEST(ProbeTool::find_argument(query, "nulled") == nullptr);
    BOOST_TEST(ProbeTool::find_argument(query, "absent") == nullptr);
    // So an optional read falls back rather than failing.
    BOOST_TEST(ProbeTool::optional_bool(query, "nulled", true) == true);
    BOOST_TEST(ProbeTool::optional_string(query, "nulled", "fallback") ==
               std::string("fallback"));
}

BOOST_AUTO_TEST_CASE(a_non_object_arguments_value_reads_as_empty)
{
    // A model that sent a bare array as its arguments should get "the property
    // is missing", not an exception from inside find().
    model_io::InvokeQuery query = query_with(nlohmann::json::array({1, 2}));
    BOOST_TEST(ProbeTool::find_argument(query, "anything") == nullptr);
    BOOST_TEST(ProbeTool::optional_bool(query, "flag", false) == false);
}

BOOST_AUTO_TEST_CASE(require_string_names_the_property_and_what_it_is_for)
{
    // Missing: the message has to say both which property and where its value
    // was supposed to come from, since that is all a model has to work from.
    const std::string missing = refusal_message([] {
        // The result is discarded on purpose — these calls exist to throw, and
        // refusal_message() reads the message off the exception.
        (void)ProbeTool::require_string(query_with(nlohmann::json::object()),
                                        "session_id",
                                        "the id spawn_process returned");
    });
    BOOST_TEST(missing.find("session_id") != std::string::npos);
    BOOST_TEST(missing.find("the id spawn_process returned") !=
               std::string::npos);

    // Wrong type, and empty, are distinct refusals — both name the property.
    const std::string wrong_type = refusal_message([] {
        (void)ProbeTool::require_string(query_with({{"session_id", 7}}),
                                        "session_id", "an id");
    });
    BOOST_TEST(wrong_type.find("must be a string") != std::string::npos);

    const std::string empty = refusal_message([] {
        (void)ProbeTool::require_string(query_with({{"session_id", ""}}),
                                        "session_id", "an id");
    });
    BOOST_TEST(empty.find("must not be empty") != std::string::npos);

    // The accepting case.
    BOOST_TEST(ProbeTool::require_string(query_with({{"session_id", "proc_1"}}),
                                         "session_id", "an id") ==
               std::string("proc_1"));
}

BOOST_AUTO_TEST_CASE(booleans_are_refused_rather_than_coerced)
{
    // The whole point: a "false" STRING is truthy under every coercion rule,
    // so coercing would hand a model the OPPOSITE of what it asked for with
    // nothing in the result to explain it.
    BOOST_TEST(!refusal_message([] {
        (void)ProbeTool::optional_bool(query_with({{"flag", "false"}}), "flag",
                                       true);
    }).empty());
    BOOST_TEST(!refusal_message([] {
        (void)ProbeTool::optional_bool(query_with({{"flag", 0}}), "flag", true);
    }).empty());

    BOOST_TEST(ProbeTool::optional_bool(query_with({{"flag", false}}),
                                        "flag", true) == false);
}

BOOST_AUTO_TEST_CASE(unsigned_reads_accept_both_integer_kinds_and_refuse_the_rest)
{
    // The trap this accessor exists to avoid: nlohmann stores a plain positive
    // literal as a SIGNED integer, so a check written only against
    // is_number_unsigned() would reject every ordinary value. Both kinds must
    // be accepted.
    const model_io::InvokeQuery signed_literal = query_with({{"n", 5000}});
    BOOST_TEST(!signed_literal.arguments.at("n").is_number_unsigned());
    BOOST_TEST(ProbeTool::optional_uint(signed_literal, "n", 1) ==
               std::uint64_t{5000});

    const model_io::InvokeQuery unsigned_value =
        query_with({{"n", std::uint64_t{7}}});
    BOOST_TEST(ProbeTool::optional_uint(unsigned_value, "n", 1) ==
               std::uint64_t{7});

    // Zero is a value, not an absence: it means "no deadline" to the tools
    // that read a timeout, so it must survive rather than fall back.
    BOOST_TEST(ProbeTool::optional_uint(query_with({{"n", 0}}), "n", 99) ==
               std::uint64_t{0});

    // Refused: a negative would wrap into an enormous unsigned value, and a
    // float or a string would truncate or parse into a different call.
    BOOST_TEST(refusal_message([] {
        (void)ProbeTool::optional_uint(query_with({{"n", -5}}), "n", 1);
    }).find("must not be negative") != std::string::npos);
    BOOST_TEST(!refusal_message([] {
        (void)ProbeTool::optional_uint(query_with({{"n", 1.9}}), "n", 1);
    }).empty());
    BOOST_TEST(!refusal_message([] {
        (void)ProbeTool::optional_uint(query_with({{"n", "soon"}}), "n", 1);
    }).empty());

    BOOST_TEST(ProbeTool::optional_uint(query_with(nlohmann::json::object()),
                                        "n", 42) == std::uint64_t{42});
}

BOOST_AUTO_TEST_CASE(string_lists_are_checked_element_by_element)
{
    // A bad element names its own INDEX: the alternative is a type error from
    // inside the whole-array conversion, which says nothing about which entry
    // was wrong.
    const std::string message = refusal_message([] {
        (void)ProbeTool::optional_string_list(
            query_with({{"items", nlohmann::json::array({"ok", 7, "also ok"})}}),
            "items");
    });
    BOOST_TEST(message.find("items") != std::string::npos);
    BOOST_TEST(message.find("[1]") != std::string::npos);

    BOOST_TEST(!refusal_message([] {
        (void)ProbeTool::optional_string_list(
            query_with({{"items", "not a list"}}), "items");
    }).empty());

    const std::vector<std::string> read = ProbeTool::optional_string_list(
        query_with({{"items", nlohmann::json::array({"a", "b"})}}), "items");
    BOOST_TEST_REQUIRE(read.size() == std::size_t{2});
    BOOST_TEST(read[0] == std::string("a"));
    // Absent reads as empty, and an engaged-but-empty list reads as empty too:
    // a caller that must tell those apart uses find_argument().
    BOOST_TEST(ProbeTool::optional_string_list(
                   query_with(nlohmann::json::object()), "items").empty());
}

// ---- settling -----------------------------------------------------------------
//
// The settle_* family is the optional_* family plus a write-back, and the write
// is the part that matters: the settled query is what the security policy
// judges, what a human confirmer is shown, what invoke() reads and what the
// record carries. So these cases assert on the QUERY afterwards, not on the
// value returned — a default that is known but not written is the bug the whole
// family exists to prevent.

BOOST_AUTO_TEST_CASE(settling_writes_the_default_into_the_arguments)
{
    model_io::InvokeQuery query = query_with(nlohmann::json::object());
    BOOST_TEST(ProbeTool::settle_bool(query, "flag", true) == true);
    BOOST_TEST(query.arguments.at("flag") == nlohmann::json(true));

    BOOST_TEST(ProbeTool::settle_uint(query, "limit", 5000) ==
               std::uint64_t{5000});
    BOOST_TEST(query.arguments.at("limit") == nlohmann::json(5000));

    BOOST_TEST(ProbeTool::settle_string(query, "label", "none") ==
               std::string("none"));
    BOOST_TEST(query.arguments.at("label") == nlohmann::json("none"));

    // A list settles as an EMPTY LIST, not as a missing key: every reader of
    // the settled query then answers "none of them" without a second question.
    BOOST_TEST(ProbeTool::settle_string_list(query, "items").empty());
    BOOST_TEST(query.arguments.at("items") == nlohmann::json::array());
}

BOOST_AUTO_TEST_CASE(settling_leaves_what_the_caller_sent_alone)
{
    model_io::InvokeQuery query = query_with({
        {"flag", false},
        {"limit", 7},
        {"label", "given"},
        {"items", nlohmann::json::array({"a"})},
        // Null reads as absent everywhere in this module, so this one IS
        // settled — with the default spelled into the query.
        {"nulled", nullptr},
    });

    BOOST_TEST(ProbeTool::settle_bool(query, "flag", true) == false);
    BOOST_TEST(query.arguments.at("flag") == nlohmann::json(false));
    BOOST_TEST(ProbeTool::settle_uint(query, "limit", 5000) ==
               std::uint64_t{7});
    BOOST_TEST(query.arguments.at("limit") == nlohmann::json(7));
    BOOST_TEST(ProbeTool::settle_string(query, "label", "none") ==
               std::string("given"));
    BOOST_TEST(query.arguments.at("label") == nlohmann::json("given"));
    BOOST_TEST(ProbeTool::settle_bool(query, "nulled", true) == true);
    BOOST_TEST(query.arguments.at("nulled") == nlohmann::json(true));

    // Nothing else appeared: settling fills gaps, it does not normalize.
    BOOST_TEST(query.arguments.size() == std::size_t{5});
}

BOOST_AUTO_TEST_CASE(a_settled_value_that_is_wrong_is_refused_before_anything_is_written)
{
    // The validation still runs first, so a malformed value cannot be
    // quietly replaced by the default — the call fails where the model can
    // see why, and the query keeps what it was given.
    model_io::InvokeQuery query = query_with({{"flag", "yes"}});
    const std::string message =
        refusal_message([&query] {
            (void)ProbeTool::settle_bool(query, "flag", true);
        });
    BOOST_TEST(message.find("flag") != std::string::npos);
    BOOST_TEST(query.arguments.at("flag") == nlohmann::json("yes"));
}

BOOST_AUTO_TEST_CASE(settling_refuses_arguments_that_are_not_an_object)
{
    // A call whose arguments are a bare array would read as "every property
    // absent" and run on defaults the model never chose — a different call
    // from the one that was sent, dressed up as a successful one. Refused
    // instead, at the checkpoint a model can fix.
    model_io::InvokeQuery query = query_with(nlohmann::json::array({1, 2}));
    const std::string message =
        refusal_message([&query] {
            (void)ProbeTool::settle_bool(query, "flag", true);
        });
    BOOST_TEST(message.find("arguments") != std::string::npos);
    BOOST_TEST(message.find("object") != std::string::npos);

    // JSON null is the module's spelling of absent, so a null arguments is the
    // empty object it means, and settling turns it into one.
    model_io::InvokeQuery nulled = query_with(nullptr);
    BOOST_TEST(ProbeTool::settle_bool(nulled, "flag", true) == true);
    BOOST_TEST(nulled.arguments.is_object());
    BOOST_TEST(nulled.arguments.at("flag") == nlohmann::json(true));
}

// ---- results and schemas ----------------------------------------------------

BOOST_AUTO_TEST_CASE(results_are_the_fields_and_text_a_reader_was_given)
{
    ProbeTool tool("probe");
    tool.body = [](const model_io::InvokeQuery&) {
        ToolResult result;
        result.field("answer", 42);
        result.field("path", "/tmp/some file");
        result.block("stdout", "hello\nworld\n");
        return result.render();
    };

    asio::io_context io;
    auto pending = asio::co_spawn(
        io, tool.invoke(query_with(nlohmann::json::object())), asio::use_future);
    io.run();
    const model_io::Content content = pending.get();

    BOOST_CHECK(content.type == model_io::ContentType::Text);
    // One text part, and it is the result as a reader sees it: a value written
    // as itself rather than quoted, and a block of text with nothing escaped
    // in it. That is the whole reason this is not a JSON object — see
    // tool_result.hpp.
    BOOST_TEST(content.raw ==
               "answer: 42\n"
               "path: /tmp/some file\n"
               "\n"
               "stdout (12 bytes):\n"
               "hello\n"
               "world\n");
    BOOST_TEST(!content.extras.has_value());
}

// ---- the result format's rules ----------------------------------------------

BOOST_AUTO_TEST_CASE(a_result_is_field_lines_and_verbatim_blocks)
{
    ToolResult result;
    result.field("session_id", "proc_1");
    result.field("arguments", nlohmann::json::array({"1", "5"}));
    result.field("finished", true);
    result.block("stdout", "1\n2\n");
    result.field("hint", "call wait_process to collect the rest");

    BOOST_TEST(result.render().raw ==
               "session_id: proc_1\n"
               "arguments: [\"1\",\"5\"]\n"
               "finished: true\n"
               "\n"
               "stdout (4 bytes):\n"
               "1\n"
               "2\n"
               "\n"
               "hint: call wait_process to collect the rest\n");
}

BOOST_AUTO_TEST_CASE(a_field_with_nothing_in_it_writes_no_line)
{
    ToolResult result;
    result.field("session_id", "proc_1");
    // Absent, empty and null are the same answer to a reader, and the answer is
    // no line at all: `description:` followed by nothing would be a line to
    // interpret.
    result.field("description", "");
    result.field("arguments", nlohmann::json::array());
    result.field("working_directory", nlohmann::json());
    result.field("state", "running");

    BOOST_TEST(result.render().raw == "session_id: proc_1\nstate: running\n");
}

BOOST_AUTO_TEST_CASE(a_string_that_spans_lines_is_the_one_value_written_as_json)
{
    ToolResult result;
    result.field("label", "two\nlines");

    // A line cannot hold it verbatim, so it is written the only way one line
    // can — compact JSON — rather than silently folded into two lines that
    // would read like two fields.
    BOOST_TEST(result.render().raw == "label: \"two\\nlines\"\n");
}

BOOST_AUTO_TEST_CASE(an_empty_block_says_so_and_a_truncated_one_says_which)
{
    ToolResult result;
    result.block("stdout", "");
    result.block("stderr", "tail only", true);

    // "(empty)" is an answer — the stream printed nothing — and the truncated
    // header says the bytes here are the beginning of something longer, so a
    // reader cannot take a cut-off capture for the whole of it.
    BOOST_TEST(result.render().raw ==
               "stdout: (empty)\n"
               "\n"
               "stderr (truncated, first 9 bytes):\n"
               "tail only\n");
}

BOOST_AUTO_TEST_CASE(separate_edges_one_record_off_the_next)
{
    ToolResult result;
    result.field("retained_session_count", 2);
    result.separate();
    result.field("session_id", "proc_1");
    result.separate();
    result.separate();          // two in a row write one edge
    result.field("session_id", "proc_2");

    BOOST_TEST(result.render().raw ==
               "retained_session_count: 2\n"
               "\n"
               "---\n"
               "\n"
               "session_id: proc_1\n"
               "\n"
               "---\n"
               "\n"
               "session_id: proc_2\n");
}

BOOST_AUTO_TEST_CASE(a_result_with_nothing_in_it_is_an_empty_text_part)
{
    const ToolResult nothing;
    BOOST_TEST(nothing.empty());
    BOOST_TEST(nothing.render().raw.empty());
    BOOST_CHECK(nothing.render().type == model_io::ContentType::Text);

    // A leading separate() has nothing to separate from.
    ToolResult later;
    later.separate();
    later.field("state", "running");
    BOOST_TEST(later.render().raw == "state: running\n");
}

BOOST_AUTO_TEST_CASE(schema_builders_produce_the_wire_shapes)
{
    const nlohmann::json schema = ProbeTool::object_schema(
        nlohmann::json{
            {"text", ProbeTool::string_property("some text")},
            {"flag", ProbeTool::bool_property("a switch", true)},
            {"count", ProbeTool::uint_property("how many", 10)},
            {"names", ProbeTool::string_list_property("some names")},
            {"mode", ProbeTool::enum_property("which mode", {"a", "b"}, "a")},
        },
        {"text"});

    BOOST_TEST(schema.at("type") == nlohmann::json("object"));
    BOOST_TEST(schema.at("required") == nlohmann::json::array({"text"}));

    const nlohmann::json& properties = schema.at("properties");
    // Every property carries a description: it is the whole contract a model
    // works from, so an undescribed property is a bug rather than a shortcut.
    for (const auto& [key, property] : properties.items()) {
        BOOST_TEST_CONTEXT("property " << key) {
            BOOST_TEST(property.contains("description"));
            BOOST_TEST(!property.at("description").get<std::string>().empty());
        }
    }
    BOOST_TEST(properties.at("flag").at("default") == nlohmann::json(true));
    BOOST_TEST(properties.at("count").at("minimum") == nlohmann::json(0));
    BOOST_TEST(properties.at("names").at("items").at("type") ==
               nlohmann::json("string"));
    BOOST_TEST(properties.at("mode").at("enum") ==
               nlohmann::json::array({"a", "b"}));

    // "required" is written even when empty: omitting it reads as
    // "unspecified" to some consumers, while [] states it.
    BOOST_TEST(ProbeTool::object_schema(nlohmann::json::object())
                   .at("required") == nlohmann::json::array());
}

// ---- the confirmation's bus -------------------------------------------------

BOOST_AUTO_TEST_CASE(the_confirmation_is_asked_on_the_tools_own_bus)
{
    // A tool given a bus asks THERE, which is what lets a component (or a
    // test) keep its confirmations to itself instead of subscribing to the
    // process-wide bus, where a handler would answer for everyone else.
    eventbus::AsyncEventBus bus;
    ProbeTool tool("probe", &bus);
    tool.declared_security = model_io::InvokeSecurity::RequireConfirm;

    bool asked = false;
    auto subscription = bus.subscribe<InvokeConfirmEvent>(
        [&asked](InvokeConfirmEvent event)
            -> asio::awaitable<InvokeConfirmEvent> {
            asked = true;
            event.decision = ConfirmDecision::Approved;
            event.reason = "approved by the test";
            co_return event;
        });

    model_io::InvokeQuery query = query_with({{"text", "x"}});
    tool.write_attributes(query);

    asio::io_context io;
    auto pending =
        asio::co_spawn(io, tool.security_check(query), asio::use_future);
    io.run();
    const auto [allowed, reason] = pending.get();

    BOOST_TEST(asked);
    BOOST_TEST(allowed);
    BOOST_TEST(reason == std::string("approved by the test"));
    subscription.disconnect();
}

BOOST_AUTO_TEST_CASE(silence_refuses_and_trusted_needs_no_answer)
{
    // Fail closed: RequireConfirm with nobody subscribed is refused, which is
    // the module's policy and not something this base may soften.
    eventbus::AsyncEventBus bus;
    ProbeTool tool("probe", &bus);
    tool.declared_security = model_io::InvokeSecurity::RequireConfirm;

    model_io::InvokeQuery query = query_with({{"text", "x"}});
    tool.write_attributes(query);

    asio::io_context io;
    auto refused =
        asio::co_spawn(io, tool.security_check(query), asio::use_future);
    io.run();
    BOOST_TEST(!std::get<0>(refused.get()));

    // Trusted passes without the bus being consulted at all.
    tool.declared_security = model_io::InvokeSecurity::Trusted;
    model_io::InvokeQuery trusted = query_with({{"text", "x"}});
    tool.write_attributes(trusted);
    io.restart();
    auto allowed =
        asio::co_spawn(io, tool.security_check(trusted), asio::use_future);
    io.run();
    BOOST_TEST(std::get<0>(allowed.get()));
}

// ---- the toolset base -------------------------------------------------------

BOOST_AUTO_TEST_CASE(the_set_keeps_presentation_order_and_routes_by_name)
{
    auto first = std::make_shared<ProbeTool>("tool_one");
    auto second = std::make_shared<ProbeTool>("tool_two");
    ProbeSet set({first, second});

    // get_tools() preserves the order it was given — it is what the model
    // reads — while dispatch() is a lookup. Two containers, one reason each.
    const std::vector<model_io::Invocable> catalogue = set.get_tools();
    BOOST_TEST_REQUIRE(catalogue.size() == std::size_t{2});
    BOOST_TEST(catalogue[0].name == std::string("tool_one"));
    BOOST_TEST(catalogue[1].name == std::string("tool_two"));

    model_io::InvokeQuery query = query_with(nlohmann::json::object());
    query.name = "tool_two";
    BOOST_TEST(set.dispatch(query) == second);
    query.name = "tool_missing";
    // nullptr rather than a throw: prepare() turns it into the Dispatch record
    // the model reads.
    BOOST_TEST(set.dispatch(query) == nullptr);
}

BOOST_AUTO_TEST_CASE(a_tool_that_will_not_build_is_not_advertised)
{
    auto good = std::make_shared<ProbeTool>("tool_good");
    auto bad = std::make_shared<ProbeTool>("tool_bad");
    bad->refuse_build = true;
    ProbeSet set({good, bad});

    // Left out of BOTH containers: advertising it would promise the model
    // something dispatch() could only answer with a half-built tool.
    BOOST_TEST(set.tool_count() == std::size_t{1});
    BOOST_TEST_REQUIRE(set.get_tools().size() == std::size_t{1});
    BOOST_TEST(set.get_tools()[0].name == std::string("tool_good"));

    model_io::InvokeQuery query = query_with(nlohmann::json::object());
    query.name = "tool_bad";
    BOOST_TEST(set.dispatch(query) == nullptr);
}

BOOST_AUTO_TEST_CASE(a_duplicate_or_unnamed_tool_is_refused_by_the_set)
{
    // The registry rejects a whole set for either of these (one name must
    // resolve to one tool), so the set catches them itself and keeps the
    // first — which leaves it registrable instead of poisoning the host's
    // whole table.
    auto first = std::make_shared<ProbeTool>("same_name");
    auto duplicate = std::make_shared<ProbeTool>("same_name");
    auto unnamed = std::make_shared<ProbeTool>("");
    ProbeSet set({first, duplicate, unnamed, nullptr});

    BOOST_TEST(set.tool_count() == std::size_t{1});
    model_io::InvokeQuery query = query_with(nlohmann::json::object());
    query.name = "same_name";
    BOOST_TEST(set.dispatch(query) == first);

    // supported_names() (the ToolSet base's own helper over get_tools()) sees
    // the same single tool, so the catalogue and the routing agree.
    const std::vector<std::string> names = set.supported_names();
    BOOST_TEST_REQUIRE(names.size() == std::size_t{1});
    BOOST_TEST(names[0] == std::string("same_name"));
}

BOOST_AUTO_TEST_CASE(the_set_runs_a_call_through_the_inherited_phases)
{
    // The base deliberately does NOT override prepare()/execute(), so a call
    // through this minimal set still gets the module's whole checkpoint
    // sequence: settled arguments, the security check, the invocation, and a
    // record correlated to the call.
    auto tool = std::make_shared<ProbeTool>("tool_one");
    tool->body = [](const model_io::InvokeQuery& query) {
        return ToolResult{}
            .field("echoed", query.arguments.at("text"))
            .render();
    };
    ProbeSet set({tool});

    model_io::InvokeQuery query = query_with({{"text", "hello"}});
    query.name = "tool_one";
    const auto handle = set.prepare(query);
    BOOST_TEST_REQUIRE(handle != nullptr);
    // write_attributes ran during settling.
    BOOST_CHECK(query.type == model_io::InvokeType::ReadOnly);
    BOOST_CHECK(query.security == model_io::InvokeSecurity::Trusted);

    asio::io_context io;
    auto pending = asio::co_spawn(io, set.execute(handle, query),
                                  asio::use_future);
    io.run();
    const model_io::InvokeReturn record = pending.get();

    BOOST_TEST(!tools::is_error(record));
    BOOST_TEST(record.query.id == std::string("call_1"));
    BOOST_TEST(record.output.raw == "echoed: hello\n");
}
