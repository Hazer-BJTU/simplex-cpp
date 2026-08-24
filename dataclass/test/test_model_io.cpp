// Round-trip tests for the model_io records' nlohmann ADL
// to_json/from_json. Pure data checks: no network, no filesystem.
#define BOOST_TEST_MODULE model_io
#include <boost/test/unit_test.hpp>

#include <nlohmann/json.hpp>

#include "dataclass/model_io.hpp"

using namespace model_io;

// Round-trip a record through JSON (json j = x; then j.get<T>()).
template <class T>
static T roundtrip(const T& in) {
    nlohmann::json j = in;
    return j.get<T>();
}

BOOST_AUTO_TEST_CASE(content_roundtrips) {
    Content c;
    c.type = ContentType::Text;
    c.raw = "hello";
    auto c2 = roundtrip(c);
    BOOST_CHECK(c2.type == ContentType::Text);
    BOOST_CHECK_EQUAL(c2.raw, "hello");
}

BOOST_AUTO_TEST_CASE(content_enum_serializes_as_string) {
    Content c;
    c.type = ContentType::ExternalRef;
    c.raw = "file:///x";
    nlohmann::json j = c;
    BOOST_CHECK_EQUAL(j["type"], "external_ref");
}

// Provider-specific content-part fields ride in extras — e.g. the Responses
// API part "type" that our coarse ContentType cannot express.
BOOST_AUTO_TEST_CASE(content_extras_roundtrip) {
    Content c;
    c.type = ContentType::Text;
    c.raw = "hello";
    c.extras = nlohmann::json{{"type", "output_text"},
                              {"annotations", nlohmann::json::array()}};
    nlohmann::json j = c;
    BOOST_REQUIRE(j.contains("extras"));
    BOOST_CHECK_EQUAL(j["extras"]["type"], "output_text");

    auto c2 = roundtrip(c);
    BOOST_REQUIRE(c2.extras.has_value());
    BOOST_CHECK_EQUAL(c2.extras->at("type"), "output_text");
    BOOST_CHECK(c2.extras->at("annotations").is_array());

    // Omitted when unset; a missing key resets (protocol rules).
    Content bare;
    nlohmann::json jb = bare;
    BOOST_CHECK(!jb.contains("extras"));
    nlohmann::json{{"type", "text"}, {"raw", "x"}}.get_to(bare);
    BOOST_CHECK(!bare.extras.has_value());
}

BOOST_AUTO_TEST_CASE(invoke_query_roundtrips_with_arguments) {
    InvokeQuery q;
    q.type = InvokeType::SerialWrite;
    q.security = InvokeSecurity::RequireConfirm;
    q.id = "id1";
    q.name = "search";
    q.arguments = nlohmann::json{{"q", "cpp"}};
    auto q2 = roundtrip(q);
    BOOST_CHECK(q2.type == InvokeType::SerialWrite);
    BOOST_CHECK(q2.security == InvokeSecurity::RequireConfirm);
    BOOST_CHECK_EQUAL(q2.id, "id1");
    BOOST_CHECK_EQUAL(q2.arguments["q"], "cpp");
    BOOST_CHECK(!q2.extras.has_value());
}

BOOST_AUTO_TEST_CASE(invoke_return_roundtrips) {
    InvokeReturn r;
    r.query.type = InvokeType::ReadOnly;
    r.query.security = InvokeSecurity::Trusted;
    r.query.id = "t0";
    r.query.name = "ls";
    r.query.arguments = nlohmann::json::object();
    r.output.type = ContentType::Text;
    r.output.raw = "a.txt\nb.txt";
    auto r2 = roundtrip(r);
    BOOST_CHECK_EQUAL(r2.output.raw, "a.txt\nb.txt");
    BOOST_CHECK_EQUAL(r2.query.name, "ls");
}

BOOST_AUTO_TEST_CASE(message_item_optionals_omitted_when_empty) {
    MessageItem m;
    m.type = MessageItemType::ModelResponse;
    m.role = "assistant";
    m.content.type = ContentType::Text;
    m.content.raw = "hi";
    nlohmann::json j = m;
    BOOST_CHECK(!j.contains("reasoning"));
    BOOST_CHECK(!j.contains("action_status"));
    BOOST_CHECK(!j.contains("invokes"));
    BOOST_CHECK(!j.contains("cost"));
    BOOST_CHECK(!j.contains("invoke_return"));
    BOOST_CHECK(!j.contains("extras"));
}

// The usage accounting a provider's final usage block maps onto: prompt /
// generated / cache_hit as plain counts (cache_hit a subset of prompt).
BOOST_AUTO_TEST_CASE(token_cost_roundtrips_on_a_model_response) {
    MessageItem m;
    m.type = MessageItemType::ModelResponse;
    m.role = "assistant";
    m.content.raw = "hi";
    m.cost = TokenCost{.prompt = 100, .generated = 7, .cache_hit = 64};

    nlohmann::json j = m;
    BOOST_REQUIRE(j.contains("cost"));
    BOOST_CHECK_EQUAL(j["cost"]["prompt"], 100);
    BOOST_CHECK_EQUAL(j["cost"]["generated"], 7);
    BOOST_CHECK_EQUAL(j["cost"]["cache_hit"], 64);

    auto m2 = roundtrip(m);
    BOOST_REQUIRE(m2.cost.has_value());
    BOOST_CHECK_EQUAL(m2.cost->prompt, 100);
    BOOST_CHECK_EQUAL(m2.cost->generated, 7);
    BOOST_CHECK_EQUAL(m2.cost->cache_hit, 64);
}

// Partial cost objects keep the counting fields' zero defaults (protocol
// rule 6), and a null under the key reads as absent (rule 3 hardening).
BOOST_AUTO_TEST_CASE(token_cost_partial_and_null_read) {
    TokenCost partial;
    nlohmann::json{{"generated", 3}}.get_to(partial);
    BOOST_CHECK_EQUAL(partial.prompt, 0);
    BOOST_CHECK_EQUAL(partial.generated, 3);
    BOOST_CHECK_EQUAL(partial.cache_hit, 0);

    MessageItem m;
    nlohmann::json{{"type", "model_response"}, {"role", "assistant"},
                   {"cost", nullptr}}
        .get_to(m);
    BOOST_CHECK(!m.cost.has_value());
}

BOOST_AUTO_TEST_CASE(message_item_optionals_restored_when_set) {
    MessageItem m;
    m.type = MessageItemType::ModelResponse;
    m.role = "assistant";
    m.content.type = ContentType::Text;
    m.content.raw = "hi";
    m.reasoning = Content{};
    m.reasoning->type = ContentType::Text;
    m.reasoning->raw = "thinking...";
    m.invokes = std::vector<InvokeQuery>{};
    auto m2 = roundtrip(m);
    BOOST_CHECK(m2.reasoning.has_value());
    BOOST_CHECK_EQUAL(m2.reasoning->raw, "thinking...");
    BOOST_CHECK(m2.invokes.has_value() && m2.invokes->empty());
}

BOOST_AUTO_TEST_CASE(agent_loop_step_retain_priority_defaults_and_roundtrips) {
    AgentLoopStep s;
    s.model_response.type = MessageItemType::ModelResponse;
    s.model_response.role = "assistant";
    s.model_response.content.raw = "answer";
    // Default member initializer -> Normal.
    BOOST_CHECK(s.retain_priority == RetainPriority::Normal);
    nlohmann::json j = s;
    BOOST_CHECK_EQUAL(j["retain_priority"], "normal");
    s.retain_priority = RetainPriority::Pinned;
    auto s2 = roundtrip(s);
    BOOST_CHECK(s2.retain_priority == RetainPriority::Pinned);
}

BOOST_AUTO_TEST_CASE(agent_loop_step_invoke_returns_roundtrip) {
    AgentLoopStep s;
    s.model_response.type = MessageItemType::ModelResponse;
    s.model_response.role = "assistant";
    s.model_response.content.raw = "calling tools";
    InvokeQuery call;
    call.id = "c0";
    call.name = "ls";
    call.arguments = nlohmann::json::object();
    s.model_response.invokes = std::vector<InvokeQuery>{call};

    MessageItem ret;
    ret.type = MessageItemType::InvokeReturn;
    ret.role = "tool";
    ret.content.raw = "a.txt";
    // Embed the originating record: query.id "c0" correlates the result back
    // to the call above (the wire tool_call_id).
    InvokeReturn record;
    record.query = call;
    record.output.type = ContentType::Text;
    record.output.raw = "a.txt";
    ret.invoke_return = std::move(record);
    s.invoke_returns = std::vector<MessageItem>{ret};

    auto s2 = roundtrip(s);
    BOOST_REQUIRE(s2.model_response.invokes.has_value());
    BOOST_CHECK_EQUAL(s2.model_response.invokes->at(0).name, "ls");
    BOOST_REQUIRE(s2.invoke_returns.has_value());
    BOOST_CHECK_EQUAL(s2.invoke_returns->at(0).content.raw, "a.txt");
    BOOST_REQUIRE(s2.invoke_returns->at(0).invoke_return.has_value());
    BOOST_CHECK_EQUAL(s2.invoke_returns->at(0).invoke_return->query.id, "c0");
}

// ---- invoke-return provenance --------------------------------------------------

// A tool-result MessageItem may embed its originating InvokeReturn; the query
// inside carries the id that correlates the result to the call that produced
// it — the "tool call id" providers require on tool-result messages.
BOOST_AUTO_TEST_CASE(message_item_embeds_invoke_return) {
    MessageItem m;
    m.type = MessageItemType::InvokeReturn;
    m.role = "tool";
    m.content.raw = "a.txt";
    InvokeReturn record;
    record.query.type = InvokeType::ReadOnly;
    record.query.security = InvokeSecurity::Trusted;
    record.query.id = "c0";
    record.query.name = "ls";
    record.query.arguments = nlohmann::json{{"path", "."}};
    record.output.type = ContentType::Text;
    record.output.raw = "a.txt";
    record.extras = nlohmann::json{{"duration_ms", 3}};
    m.invoke_return = std::move(record);

    nlohmann::json j = m;
    BOOST_CHECK(!j.contains("reasoning"));
    BOOST_REQUIRE(j.contains("invoke_return"));
    BOOST_CHECK_EQUAL(j["invoke_return"]["query"]["id"], "c0");
    BOOST_CHECK_EQUAL(j["invoke_return"]["output"]["raw"], "a.txt");

    auto m2 = roundtrip(m);
    BOOST_REQUIRE(m2.invoke_return.has_value());
    BOOST_CHECK_EQUAL(m2.invoke_return->query.id, "c0");
    BOOST_CHECK_EQUAL(m2.invoke_return->query.name, "ls");
    BOOST_CHECK_EQUAL(m2.invoke_return->query.arguments["path"], ".");
    BOOST_CHECK_EQUAL(m2.invoke_return->output.raw, "a.txt");
    BOOST_REQUIRE(m2.invoke_return->extras.has_value());
    BOOST_CHECK_EQUAL(m2.invoke_return->extras->at("duration_ms"), 3);

    // A missing key yields nullopt (protocol rule: optionals reset on read).
    MessageItem bare;
    nlohmann::json{{"type", "invoke_return"}, {"role", "tool"}}.get_to(bare);
    BOOST_CHECK(bare.type == MessageItemType::InvokeReturn);
    BOOST_CHECK(!bare.invoke_return.has_value());
}

BOOST_AUTO_TEST_CASE(json_null_under_optional_keys_reads_as_absent) {
    // Protocol rules 3+6 hardened for laxer external producers: this module
    // never WRITES nulls, but a session file from elsewhere may carry them.
    // Before the guard, "invokes": null threw type_error.302 (failing the
    // whole session load) while "reasoning": null silently produced an
    // engaged optional holding a default Content — both wrong. Null must read
    // exactly like a missing key: std::nullopt.
    const nlohmann::json external = nlohmann::json{
        {"type", "model_response"},
        {"role", "assistant"},
        {"content", {{"type", "text"}, {"raw", "hi"}}},
        {"reasoning", nullptr},
        {"action_status", nullptr},
        {"invokes", nullptr},
        {"invoke_return", nullptr},
        {"extras", nullptr},
    };

    MessageItem m = external.get<MessageItem>();
    BOOST_CHECK(m.type == MessageItemType::ModelResponse);
    BOOST_CHECK_EQUAL(m.content.raw, "hi");
    BOOST_CHECK(!m.reasoning.has_value());
    BOOST_CHECK(!m.action_status.has_value());
    BOOST_CHECK(!m.invokes.has_value());
    BOOST_CHECK(!m.invoke_return.has_value());
    BOOST_CHECK(!m.extras.has_value());

    // The same leniency holds one level up and for the other record kinds.
    const nlohmann::json step = nlohmann::json{
        {"model_response", external},
        {"invoke_returns", nullptr},
        {"extras", nullptr},
    };
    AgentLoopStep s = step.get<AgentLoopStep>();
    BOOST_CHECK(!s.invoke_returns.has_value());
    BOOST_CHECK(!s.extras.has_value());
    BOOST_CHECK(!s.model_response.reasoning.has_value());

    const nlohmann::json tool = nlohmann::json{
        {"name", "ls"},
        {"description", "list"},
        {"argument_schema", nlohmann::json::object()},
        {"remote_type", nullptr},
        {"extras", nullptr},
    };
    Invocable v = tool.get<Invocable>();
    BOOST_CHECK(!v.remote_type.has_value());
    BOOST_CHECK(!v.extras.has_value());

    const nlohmann::json turn = nlohmann::json{
        {"user_input", {{"type", "user_input"}, {"role", "user"}}},
        {"agent_loop_step", nlohmann::json::array()},
        {"extras", nullptr},
    };
    UserLoopStep u = turn.get<UserLoopStep>();
    BOOST_CHECK(!u.extras.has_value());
}

BOOST_AUTO_TEST_CASE(user_loop_step_compact_preference_roundtrips) {
    UserLoopStep u;
    u.user_input.type = MessageItemType::UserInput;
    u.user_input.role = "user";
    u.user_input.content.raw = "hi";
    u.retain_priority = RetainPriority::Discardable;
    nlohmann::json j = u;
    BOOST_CHECK_EQUAL(j["retain_priority"], "discardable");
    auto u2 = roundtrip(u);
    BOOST_CHECK(u2.retain_priority == RetainPriority::Discardable);
}

BOOST_AUTO_TEST_CASE(records_are_plain_aggregates) {
    // No base class or declared constructors: aggregate initialisation works
    // and records convert through the nlohmann ADL functions.
    Content c{ContentType::Binary, "AA=="};
    nlohmann::json j = c;
    BOOST_CHECK_EQUAL(j["type"], "binary");
    BOOST_CHECK_EQUAL(j["raw"], "AA==");

    auto c2 = j.get<Content>(); // json -> record
    BOOST_CHECK(c2.type == ContentType::Binary);
    BOOST_CHECK_EQUAL(c2.raw, "AA==");

    Content c3;
    j.get_to(c3); // in-place deserialisation
    BOOST_CHECK_EQUAL(c3.raw, "AA==");

    Content c4;
    c4 = c3; // ordinary copy assignment still resolves
    BOOST_CHECK_EQUAL(c4.raw, "AA==");
}

BOOST_AUTO_TEST_CASE(missing_keys_keep_member_defaults) {
    InvokeReturn r;
    nlohmann::json j;
    j["query"]["id"] = "t0";
    j["query"]["name"] = "ls";
    // "output" missing: the plain member keeps its default; optionals reset.
    j.get_to(r);
    BOOST_CHECK_EQUAL(r.query.id, "t0");
    BOOST_CHECK(r.output.raw.empty());
    BOOST_CHECK(!r.extras.has_value());

    MessageItem m;
    nlohmann::json jm{{"type", "model_response"}};
    jm.get_to(m);
    BOOST_CHECK(m.type == MessageItemType::ModelResponse);
    BOOST_CHECK(m.role.empty());
    BOOST_CHECK(!m.reasoning.has_value());
    BOOST_CHECK(!m.invokes.has_value());
}

// ---- tool registration -------------------------------------------------------

BOOST_AUTO_TEST_CASE(invocable_roundtrips) {
    Invocable v;
    v.name = "search";
    v.description = "Full-text search over the index";
    v.argument_schema = nlohmann::json{
        {"type", "object"},
        {"properties", {{"q", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"q"})},
    };
    v.remote_type = "extension";
    v.extras = nlohmann::json{{"vendor", "acme"}};

    auto v2 = roundtrip(v);
    BOOST_CHECK_EQUAL(v2.name, "search");
    BOOST_CHECK_EQUAL(v2.description, "Full-text search over the index");
    // The schema embeds inline as-is.
    BOOST_CHECK_EQUAL(v2.argument_schema["properties"]["q"]["type"], "string");
    BOOST_CHECK_EQUAL(v2.argument_schema["required"][0], "q");
    BOOST_REQUIRE(v2.remote_type.has_value());
    BOOST_CHECK_EQUAL(*v2.remote_type, "extension");
    BOOST_REQUIRE(v2.extras.has_value());
    BOOST_CHECK_EQUAL(v2.extras->at("vendor"), "acme");
}

BOOST_AUTO_TEST_CASE(invocable_optionals_omitted_when_empty) {
    Invocable v;
    v.name = "ls";
    v.description = "List files";
    nlohmann::json j = v;
    BOOST_CHECK_EQUAL(j["name"], "ls");
    BOOST_CHECK_EQUAL(j["description"], "List files");
    BOOST_CHECK(j["argument_schema"].is_object());
    BOOST_CHECK(!j.contains("remote_type"));
    BOOST_CHECK(!j.contains("extras"));
}

BOOST_AUTO_TEST_CASE(invocable_missing_keys_keep_member_defaults) {
    nlohmann::json j = nlohmann::json::object(); // every key missing
    Invocable v;
    j.get_to(v);
    BOOST_CHECK(v.name.empty());
    BOOST_CHECK(v.description.empty());
    // Default is the vacuously-accepting object schema, never null (null is
    // not a valid JSON Schema).
    BOOST_CHECK(v.argument_schema.is_object());
    BOOST_CHECK(!v.remote_type.has_value());
    BOOST_CHECK(!v.extras.has_value());
}

// ---- session input container --------------------------------------------------

// AgentInputState has NO to_json/from_json (it embeds PromptTemplate, which
// is deliberately outside the JSON contract) — what CAN round-trip are its
// serialisable members, tools and turns.
BOOST_AUTO_TEST_CASE(agent_input_state_holds_the_session_together) {
    AgentInputState state; // fresh session: everything empty
    BOOST_CHECK(state.tools.empty());
    BOOST_CHECK(state.turns.empty());
    BOOST_CHECK(!state.extras.has_value());

    state.system_prompt
        .add_section("identity", "Identity", "You are a coding agent.")
        .add_section("tools", "Tools", "", SectionStability::Growing)
        .add_section("clock", "", "Date: 2026-08-15.", SectionStability::Volatile);

    Invocable ls;
    ls.name = "ls";
    ls.description = "List files";
    state.tools.push_back(std::move(ls));

    UserLoopStep turn;
    turn.user_input.role = "user";
    turn.user_input.content.raw = "list the files";
    AgentLoopStep cycle;
    cycle.model_response.type = MessageItemType::ModelResponse;
    cycle.model_response.role = "assistant";
    InvokeQuery q;
    q.id = "c0";
    q.name = "ls";
    cycle.model_response.invokes = std::vector<InvokeQuery>{q};
    MessageItem result;
    result.type = MessageItemType::InvokeReturn;
    result.role = "tool";
    result.content.raw = "a.txt";
    InvokeReturn record;   // provenance: query.id "c0" names the call above
    record.query = q;
    record.output.type = ContentType::Text;
    record.output.raw = "a.txt";
    result.invoke_return = std::move(record);
    cycle.invoke_returns = std::vector<MessageItem>{result};
    turn.agent_loop_step = {cycle};
    state.turns.push_back(std::move(turn));

    // The system prompt renders through the template, spans first section
    // immutable (what the interpreter turns into messages[0]).
    auto rendered = state.system_prompt.render();
    BOOST_CHECK_NE(rendered.markdown.find("## Identity"), std::string::npos);
    BOOST_REQUIRE_EQUAL(rendered.spans.size(), 3u);
    BOOST_CHECK_EQUAL(rendered.spans.front().name, "identity");
    BOOST_CHECK(rendered.spans.front().stability == SectionStability::Immutable);

    // The serialisable members keep their ADL pairs.
    auto tools2 = nlohmann::json(state.tools).get<std::vector<Invocable>>();
    BOOST_REQUIRE_EQUAL(tools2.size(), 1u);
    BOOST_CHECK_EQUAL(tools2[0].name, "ls");

    auto turns2 = nlohmann::json(state.turns).get<std::vector<UserLoopStep>>();
    BOOST_REQUIRE_EQUAL(turns2.size(), 1u);
    BOOST_CHECK_EQUAL(turns2[0].user_input.content.raw, "list the files");
    const auto& step = turns2[0].agent_loop_step.at(0);
    BOOST_REQUIRE(step.model_response.invokes.has_value());
    BOOST_CHECK_EQUAL(step.model_response.invokes->at(0).name, "ls");
    BOOST_REQUIRE(step.invoke_returns.has_value());
    BOOST_CHECK_EQUAL(step.invoke_returns->at(0).content.raw, "a.txt");
    // The embedded record survives, correlating the result to its call.
    BOOST_REQUIRE(step.invoke_returns->at(0).invoke_return.has_value());
    BOOST_CHECK_EQUAL(step.invoke_returns->at(0).invoke_return->query.id, "c0");
}
