/**
 * @file test_yaml_json.cpp
 * @brief Unit tests for yamlconfig/yaml_json.hpp (YAML -> nlohmann JSON).
 *
 * The conversion matrix: the scalar table (YAML 1.1 booleans, quoted numbers
 * staying strings, the int64/uint64 band, finite doubles, null spellings),
 * nesting and empty containers, anchors/aliases, merge keys (map value,
 * sequence-of-maps value, explicit keys overriding), textualised scalar
 * keys, every error path (non-finite numbers, non-scalar keys, multiple
 * documents, syntax errors carrying marks, cyclic aliases, missing files,
 * malformed merge values), the file entry point over a temp file, a
 * realistic llm-style config checked field-by-field, and the legality
 * round-trip (dump() re-parses with nlohmann).
 */

#define BOOST_TEST_MODULE YamlJsonTests
#include <boost/test/unit_test.hpp>

#include "yamlconfig/yaml_json.hpp"

#include <boost/test/tools/floating_point_comparison.hpp>

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace {

/// RAII temp file carrying `contents`, removed on destruction.
struct scoped_yaml_file {
    std::filesystem::path p;
    explicit scoped_yaml_file(const std::string& contents) {
        p = std::filesystem::temp_directory_path() / "simplex_yamlconfig_test.yaml";
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << contents;
    }
    ~scoped_yaml_file() {
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(YamlJsonSuite)

// ---------------------------------------------------------------------------
// Scalars
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(null_spellings_convert_to_null) {
    BOOST_CHECK(yamlconfig::parse("key:").at("key").is_null());
    BOOST_CHECK(yamlconfig::parse("key: ~").at("key").is_null());
    BOOST_CHECK(yamlconfig::parse("key: null").at("key").is_null());
    BOOST_CHECK(yamlconfig::parse("key: Null").at("key").is_null());
    BOOST_CHECK(yamlconfig::parse("key: NULL").at("key").is_null());
}

BOOST_AUTO_TEST_CASE(yaml11_booleans_convert) {
    for (const char* truthy : {"true", "True", "TRUE", "yes", "Yes", "on", "On"}) {
        BOOST_CHECK_EQUAL(yamlconfig::parse(std::string("k: ") + truthy).at("k"),
                          true);
    }
    for (const char* falsy : {"false", "False", "no", "No", "off", "Off"}) {
        BOOST_CHECK_EQUAL(yamlconfig::parse(std::string("k: ") + falsy).at("k"),
                          false);
    }
}

BOOST_AUTO_TEST_CASE(integers_and_the_uint64_band) {
    BOOST_CHECK_EQUAL(yamlconfig::parse("k: 0").at("k"), 0);
    BOOST_CHECK_EQUAL(yamlconfig::parse("k: -42").at("k"), -42);
    // int64 max stays signed...
    BOOST_CHECK_EQUAL(
        yamlconfig::parse("k: 9223372036854775807").at("k").get<std::int64_t>(),
        std::numeric_limits<std::int64_t>::max());
    // ...and the above-int64 band falls back to uint64, exactly.
    BOOST_CHECK_EQUAL(
        yamlconfig::parse("k: 18446744073709551615").at("k").get<std::uint64_t>(),
        std::numeric_limits<std::uint64_t>::max());
    // A real int, not a float: nlohmann stores it as an integer type.
    BOOST_CHECK(yamlconfig::parse("k: 7").at("k").is_number_integer());
}

BOOST_AUTO_TEST_CASE(finite_doubles_convert) {
    BOOST_CHECK_CLOSE(yamlconfig::parse("k: 3.14").at("k").get<double>(), 3.14,
                      1e-12);
    BOOST_CHECK_CLOSE(yamlconfig::parse("k: -2.5e-3").at("k").get<double>(),
                      -2.5e-3, 1e-12);
    // Integral-looking floats keep their point in the text: yaml-cpp decodes
    // `1.0` as double, and so do we.
    BOOST_CHECK(yamlconfig::parse("k: 1.0").at("k").is_number_float());
}

BOOST_AUTO_TEST_CASE(quoted_scalars_stay_strings) {
    BOOST_CHECK_EQUAL(yamlconfig::parse("k: \"42\"").at("k"), "42");
    BOOST_CHECK_EQUAL(yamlconfig::parse("k: 'true'").at("k"), "true");
    BOOST_CHECK_EQUAL(yamlconfig::parse("k: \"3.14\"").at("k"), "3.14");
    // Untagged non-numbers are strings anyway.
    BOOST_CHECK_EQUAL(yamlconfig::parse("k: hello world").at("k"), "hello world");
}

BOOST_AUTO_TEST_CASE(empty_document_is_null) {
    BOOST_CHECK(yamlconfig::parse("").is_null());
    BOOST_CHECK(yamlconfig::parse("# only a comment\n").is_null());
}

// ---------------------------------------------------------------------------
// Structures
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(nested_maps_and_sequences) {
    const auto j = yamlconfig::parse(R"yaml(
server:
  host: localhost
  ports: [8080, 8443]
  tls:
    enabled: true
    cert: /etc/cert.pem
clients:
  - name: alpha
    weight: 10
  - name: beta
    weight: 20
)yaml");
    BOOST_CHECK_EQUAL(j.at("server").at("host"), "localhost");
    BOOST_REQUIRE_EQUAL(j.at("server").at("ports").size(), 2u);
    BOOST_CHECK_EQUAL(j.at("server").at("ports")[0], 8080);
    BOOST_CHECK_EQUAL(j.at("server").at("ports")[1], 8443);
    BOOST_CHECK_EQUAL(j.at("server").at("tls").at("enabled"), true);
    BOOST_REQUIRE_EQUAL(j.at("clients").size(), 2u);
    BOOST_CHECK_EQUAL(j.at("clients")[1].at("name"), "beta");
    BOOST_CHECK_EQUAL(j.at("clients")[1].at("weight"), 20);
}

BOOST_AUTO_TEST_CASE(empty_containers_and_empty_values) {
    BOOST_CHECK(yamlconfig::parse("k: {}").at("k").empty());
    BOOST_CHECK(yamlconfig::parse("k: []").at("k").empty());
    BOOST_CHECK(yamlconfig::parse("k: {}").at("k").is_object());
    BOOST_CHECK(yamlconfig::parse("k: []").at("k").is_array());
}

BOOST_AUTO_TEST_CASE(scalar_keys_are_textualised) {
    const auto j = yamlconfig::parse(R"yaml(
8080: http
true: yes
3.14: pi
)yaml");
    BOOST_CHECK_EQUAL(j.at("8080"), "http");
    BOOST_CHECK_EQUAL(j.at("true"), true);      // key "true", value boolean
    BOOST_CHECK_EQUAL(j.at("3.14"), "pi");
    // All keys are JSON strings, whatever their YAML spelling.
    BOOST_CHECK(j.contains("8080") && j.contains("true") && j.contains("3.14"));
}

// ---------------------------------------------------------------------------
// Anchors, aliases, merge keys
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(anchors_and_aliases_are_copied) {
    const auto j = yamlconfig::parse(R"yaml(
defaults: &base
  retries: 3
  timeout: 30
derived:
  <<: *base
explicit:
  retries: 9
plain_alias: *base
)yaml");
    // The alias itself: a plain copy of the anchored mapping.
    BOOST_CHECK_EQUAL(j.at("plain_alias").at("retries"), 3);
    BOOST_CHECK_EQUAL(j.at("plain_alias").at("timeout"), 30);
    // `derived` sees the same values through the merge key (checked in
    // depth below); `defaults` keeps its own.
    BOOST_CHECK_EQUAL(j.at("defaults").at("retries"), 3);
    BOOST_CHECK_EQUAL(j.at("explicit").at("retries"), 9);
}

BOOST_AUTO_TEST_CASE(merge_key_map_value) {
    const auto j = yamlconfig::parse(R"yaml(
base: &b
  a: 1
  b: 2
child:
  <<: *b
  b: 20
  c: 30
)yaml");
    // Explicit keys win over merged defaults; missing keys come from base.
    BOOST_CHECK_EQUAL(j.at("child").at("a"), 1);
    BOOST_CHECK_EQUAL(j.at("child").at("b"), 20);
    BOOST_CHECK_EQUAL(j.at("child").at("c"), 30);
    BOOST_CHECK_EQUAL(j.at("child").size(), 3u);
}

BOOST_AUTO_TEST_CASE(merge_key_sequence_of_maps_earlier_wins) {
    const auto j = yamlconfig::parse(R"yaml(
first: &f
  a: 1
  shared: from_first
second: &s
  b: 2
  shared: from_second
child:
  <<: [*f, *s]
  own: 3
)yaml");
    // YAML 1.1 merge semantics: earlier maps in the sequence win over later
    // on shared keys; later maps fill the gaps nobody before them provided.
    BOOST_CHECK_EQUAL(j.at("child").at("a"), 1);
    BOOST_CHECK_EQUAL(j.at("child").at("b"), 2);
    BOOST_CHECK_EQUAL(j.at("child").at("shared"), "from_first");
    BOOST_CHECK_EQUAL(j.at("child").at("own"), 3);
    BOOST_CHECK_EQUAL(j.at("child").size(), 4u);
}

BOOST_AUTO_TEST_CASE(merge_value_must_be_map_or_seq_of_maps) {
    BOOST_CHECK_THROW(yamlconfig::parse("a:\n  <<: 42"),
                      yamlconfig::YamlConfigError);
    BOOST_CHECK_THROW(
        yamlconfig::parse("a: &x {k: 1}\nb:\n  <<: [*x, plain_string]"),
        yamlconfig::YamlConfigError);
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(non_finite_numbers_are_rejected) {
    for (const char* bad : {".nan", ".NaN", ".inf", "-.Inf", ".inf"}) {
        BOOST_CHECK_THROW(yamlconfig::parse(std::string("k: ") + bad),
                          yamlconfig::YamlConfigError);
    }
    // ...including one nested deep, with the in-document path in the message.
    try {
        (void)yamlconfig::parse("server:\n  limits:\n    ratio: .nan\n");
        BOOST_FAIL("expected YamlConfigError");
    } catch (const yamlconfig::YamlConfigError& e) {
        const std::string msg = e.what();
        BOOST_CHECK(msg.find("/server/limits/ratio") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(non_scalar_keys_are_rejected) {
    BOOST_CHECK_THROW(yamlconfig::parse("? [a, b]\n: value"),
                      yamlconfig::YamlConfigError);
    BOOST_CHECK_THROW(yamlconfig::parse("? {k: v}\n: value"),
                      yamlconfig::YamlConfigError);
    try {
        (void)yamlconfig::parse("outer:\n  ? [1, 2]\n  : v\n");
        BOOST_FAIL("expected YamlConfigError");
    } catch (const yamlconfig::YamlConfigError& e) {
        const std::string msg = e.what();
        BOOST_CHECK(msg.find("/outer") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(multiple_documents_are_rejected) {
    BOOST_CHECK_THROW(yamlconfig::parse("a: 1\n---\nb: 2\n"),
                      yamlconfig::YamlConfigError);
}

BOOST_AUTO_TEST_CASE(syntax_errors_carry_marks) {
    try {
        (void)yamlconfig::parse("a: 1\n  b: [unclosed\n");
        BOOST_FAIL("expected YamlConfigError");
    } catch (const yamlconfig::YamlConfigError& e) {
        const std::string msg = e.what();
        BOOST_CHECK(msg.find("line") != std::string::npos);
        BOOST_CHECK(msg.find("column") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(cyclic_aliases_hit_the_depth_guard) {
    // A self-referential alias: legal YAML (a recursive tree), impossible
    // JSON — the depth guard turns it into an error rather than a hang.
    BOOST_CHECK_THROW(yamlconfig::parse("a: &loop\n  next: *loop\n"),
                      yamlconfig::YamlConfigError);
}

BOOST_AUTO_TEST_CASE(missing_file_is_wrapped) {
    const auto nowhere = std::filesystem::temp_directory_path()
                       / "simplex_yamlconfig_does_not_exist_9f3.yaml";
    BOOST_CHECK_THROW(yamlconfig::load_file(nowhere),
                      yamlconfig::YamlConfigError);
}

// ---------------------------------------------------------------------------
// File entry point
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(load_file_round_trips_parse) {
    const std::string text = "model: deepseek-v4-flash\ntools: [clock, fs]\n";
    const scoped_yaml_file file{text};
    BOOST_CHECK_EQUAL(yamlconfig::load_file(file.p), yamlconfig::parse(text));
}

// ---------------------------------------------------------------------------
// A realistic config, end to end
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(llm_style_config_matches_expected_json) {
    const auto from_yaml = yamlconfig::parse(R"yaml(
# The provider entry this host configures.
provider: deepseek
model: deepseek-v4-flash
temperature: 0.7
stream: yes
endpoint:
  base_url: https://api.deepseek.com
  request_path: /chat/completions
  auth:
    scheme: bearer
    api_key: ${DEEPSEEK_API_KEY}   # substituted by the host, not us
tools:
  - name: get_current_time
    description: Current UTC time, ISO-8601.
    parameters: {}
retry:
  attempts: 3
  backoff_s: [0.5, 2.0]
)yaml");

    const nlohmann::json expected = nlohmann::json::parse(R"json({
      "provider": "deepseek",
      "model": "deepseek-v4-flash",
      "temperature": 0.7,
      "stream": true,
      "endpoint": {
        "base_url": "https://api.deepseek.com",
        "request_path": "/chat/completions",
        "auth": {
          "scheme": "bearer",
          "api_key": "${DEEPSEEK_API_KEY}"
        }
      },
      "tools": [
        {
          "name": "get_current_time",
          "description": "Current UTC time, ISO-8601.",
          "parameters": {}
        }
      ],
      "retry": {
        "attempts": 3,
        "backoff_s": [0.5, 2.0]
      }
    })json");

    BOOST_CHECK(from_yaml == expected);
}

BOOST_AUTO_TEST_CASE(result_is_legal_json) {
    // The legality round-trip: whatever we produce must survive
    // dump()/parse through nlohmann itself.
    const auto j = yamlconfig::parse(R"yaml(
a: &x [1, 2.5, text]
b:
  <<: {deep: {deeper: true}}
  own: null
)yaml");
    const nlohmann::json reparsed = nlohmann::json::parse(j.dump());
    BOOST_CHECK(reparsed == j);
}

BOOST_AUTO_TEST_SUITE_END()
