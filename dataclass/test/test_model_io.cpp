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
    BOOST_CHECK(!j.contains("extras"));
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
    s.invoke_returns = std::vector<MessageItem>{ret};

    auto s2 = roundtrip(s);
    BOOST_REQUIRE(s2.model_response.invokes.has_value());
    BOOST_CHECK_EQUAL(s2.model_response.invokes->at(0).name, "ls");
    BOOST_REQUIRE(s2.invoke_returns.has_value());
    BOOST_CHECK_EQUAL(s2.invoke_returns->at(0).content.raw, "a.txt");
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
