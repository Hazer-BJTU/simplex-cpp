// Round-trip tests for the endpoint_config records' nlohmann ADL
// to_json/from_json. Pure data checks: no network, no filesystem.
#define BOOST_TEST_MODULE endpoint_config
#include <boost/test/unit_test.hpp>

#include <nlohmann/json.hpp>

#include "dataclass/endpoint_config.hpp"

using namespace model_io;

// Round-trip a record through JSON (json j = x; then j.get<T>()).
template <class T>
static T roundtrip(const T& in) {
    nlohmann::json j = in;
    return j.get<T>();
}

BOOST_AUTO_TEST_CASE(auth_roundtrips_with_custom_header_scheme) {
    EndpointAuth a;
    a.scheme = AuthScheme::CustomHeader;
    a.api_key = "sk-123";
    a.header_name = "x-goog-api-key";
    a.extras = nlohmann::json{{"key_file", "/etc/keys/gcp.json"}};

    nlohmann::json j = a;
    BOOST_CHECK_EQUAL(j["scheme"], "custom_header");
    BOOST_REQUIRE(j.contains("extras"));

    auto a2 = roundtrip(a);
    BOOST_CHECK(a2.scheme == AuthScheme::CustomHeader);
    BOOST_CHECK_EQUAL(a2.api_key, "sk-123");
    BOOST_CHECK_EQUAL(a2.header_name, "x-goog-api-key");
    BOOST_REQUIRE(a2.extras.has_value());
    BOOST_CHECK_EQUAL(a2.extras->at("key_file"), "/etc/keys/gcp.json");
}

BOOST_AUTO_TEST_CASE(auth_unknown_scheme_fails_closed) {
    // None is listed first: an unrecognised scheme string deserialises to it,
    // so no credential is ever sent for a typo'd scheme.
    EndpointAuth a;
    nlohmann::json j = nlohmann::json{{"scheme", "mutual_tls"}};
    j.get_to(a);
    BOOST_CHECK(a.scheme == AuthScheme::None);
}

BOOST_AUTO_TEST_CASE(auth_defaults_and_omitted_optionals) {
    EndpointAuth a; // fresh: bearer, no key, default header
    nlohmann::json j = a;
    BOOST_CHECK_EQUAL(j["scheme"], "bearer");
    BOOST_CHECK_EQUAL(j["header_name"], "x-api-key");
    BOOST_CHECK(!j.contains("extras"));

    // Missing keys keep member defaults.
    EndpointAuth b;
    nlohmann::json{{"api_key", "sk-x"}}.get_to(b);
    BOOST_CHECK(b.scheme == AuthScheme::Bearer);
    BOOST_CHECK_EQUAL(b.api_key, "sk-x");
    BOOST_CHECK_EQUAL(b.header_name, "x-api-key");
    BOOST_CHECK(!b.extras.has_value());
}

BOOST_AUTO_TEST_CASE(model_endpoint_roundtrips) {
    ModelEndpoint e;
    e.base_url = "https://api.deepseek.com/anthropic";
    e.request_path = "/v1/messages";
    e.auth.scheme = AuthScheme::CustomHeader;
    e.auth.api_key = "sk-123";
    e.user_agent = "simplex-cpp-agent";
    e.extra_headers = {{"anthropic-version", "2023-06-01"}, {"x-trace", "1"}};
    e.extras = nlohmann::json{{"region", "eu"}};

    auto e2 = roundtrip(e);
    BOOST_CHECK_EQUAL(e2.base_url, "https://api.deepseek.com/anthropic");
    BOOST_CHECK_EQUAL(e2.request_path, "/v1/messages");
    BOOST_CHECK(e2.auth.scheme == AuthScheme::CustomHeader);
    BOOST_CHECK_EQUAL(e2.auth.api_key, "sk-123");
    BOOST_CHECK_EQUAL(e2.user_agent, "simplex-cpp-agent");
    BOOST_CHECK_EQUAL(e2.extra_headers.at("anthropic-version"), "2023-06-01");
    BOOST_CHECK_EQUAL(e2.extra_headers.size(), 2u);
    BOOST_REQUIRE(e2.extras.has_value());
    BOOST_CHECK_EQUAL(e2.extras->at("region"), "eu");
}

BOOST_AUTO_TEST_CASE(model_endpoint_defaults_and_omitted_optionals) {
    ModelEndpoint e; // fresh: OpenAI-style defaults, nothing set
    nlohmann::json j = e;
    BOOST_CHECK_EQUAL(j["request_path"], "/chat/completions");
    BOOST_CHECK_EQUAL(j["user_agent"], "simplex-cpp");
    BOOST_CHECK_EQUAL(j["auth"]["scheme"], "bearer");
    BOOST_CHECK(!j.contains("extras"));

    // Missing keys keep member defaults; unknown keys are ignored.
    ModelEndpoint b;
    nlohmann::json{{"base_url", "https://api.deepseek.com"},
                   {"vendor_novelty", true}}
        .get_to(b);
    BOOST_CHECK_EQUAL(b.base_url, "https://api.deepseek.com");
    BOOST_CHECK_EQUAL(b.request_path, "/chat/completions");
    BOOST_CHECK(b.auth.scheme == AuthScheme::Bearer);
    BOOST_CHECK(b.extra_headers.empty());
    BOOST_CHECK(!b.extras.has_value());
}
