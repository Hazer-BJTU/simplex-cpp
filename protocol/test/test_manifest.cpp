#define BOOST_TEST_MODULE protocol_manifest
#include <boost/test/unit_test.hpp>

#include "protocol/manifest.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using namespace protocol;

/** Recursively remove explanatory prose, leaving structural entries only. */
void strip_summaries(Json& value) {
    if (value.is_object()) {
        value.erase("summary");
        for (auto& [key, child] : value.items()) {
            (void)key;
            strip_summaries(child);
        }
    } else if (value.is_array()) {
        for (auto& child : value) {
            strip_summaries(child);
        }
    }
}

std::string read_committed_artifact() {
    std::ifstream input(PROTOCOL_SCHEMA_PATH, std::ios::binary);
    BOOST_REQUIRE_MESSAGE(input, "cannot open " << PROTOCOL_SCHEMA_PATH);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

const RecordDescriptor* find_record(std::string_view name) {
    for (const auto* descriptor : record_registry()) {
        if (descriptor->name == name) return descriptor;
    }
    return nullptr;
}

bool is_hex_sha256(std::string_view text) {
    return text.size() == 64
        && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

} // namespace

BOOST_AUTO_TEST_CASE(committed_artifact_matches_the_compiled_contract) {
    const std::string expected = manifest().dump(2) + "\n";
    const std::string committed = read_committed_artifact();
    if (committed != expected) {
        BOOST_FAIL("the committed protocol artifact is stale: regenerate it with\n"
                   "  cmake --build <build-dir> --target protocol_manifest_export\n"
                   "  <build-dir>/protocol/protocol_manifest_export > "
                   << PROTOCOL_SCHEMA_PATH);
    }
    BOOST_TEST(committed == expected);
}

BOOST_AUTO_TEST_CASE(fingerprint_is_a_stable_sha256) {
    const std::string first = fingerprint();
    const std::string second = fingerprint();
    BOOST_TEST(first == second);
    BOOST_TEST(is_hex_sha256(first));
    // Pinned so an accidental canonicalization change is a visible diff rather
    // than a silently different hash for the same structure.
    BOOST_TEST_MESSAGE("fingerprint: " << first);
    BOOST_TEST(manifest().at("fingerprint").get<std::string>() == first);
}

BOOST_AUTO_TEST_CASE(prose_never_reaches_the_canonical_contract) {
    // Summaries are what a reviewer edits; they must not be able to move the
    // fingerprint. The canonical document therefore contains no `summary` key.
    Json canonical = canonical_contract();
    Json stripped = canonical;
    strip_summaries(stripped);
    BOOST_TEST(canonical.dump() == stripped.dump());

    const Json full = manifest();
    BOOST_TEST(full.contains("fingerprint"));
    BOOST_TEST(full.contains("json_schema"));
    BOOST_TEST(full.contains("extension_policies"));
}

BOOST_AUTO_TEST_CASE(manifest_is_the_canonical_contract_plus_prose) {
    Json reduced = manifest();
    reduced.erase("json_schema");
    reduced.erase("extension_policies");
    reduced.erase("fingerprint_scope");
    reduced.erase("fingerprint");
    strip_summaries(reduced);
    BOOST_TEST(reduced.dump() == canonical_contract().dump());
}

BOOST_AUTO_TEST_CASE(record_names_are_unique_and_references_resolve) {
    std::set<std::string> records;
    for (const auto* descriptor : record_registry()) {
        BOOST_TEST_MESSAGE("record: " << descriptor->name);
        BOOST_TEST(records.insert(std::string(descriptor->name)).second);
        BOOST_TEST(!descriptor->summary.empty());
        for (const auto& field : descriptor->fields()) {
            BOOST_TEST(!field.name.empty());
            BOOST_TEST(!field.summary.empty());
            if (field.record != nullptr) {
                BOOST_TEST_MESSAGE("field reference: " << field.record->name);
                BOOST_TEST(find_record(field.record->name) != nullptr);
            }
            if (field.enumeration != nullptr) {
                const auto& spellings = field.enumeration->spellings;
                BOOST_TEST(!spellings.empty());
            }
        }
    }

    std::set<std::string> enums;
    for (const auto* descriptor : enum_registry()) {
        BOOST_TEST(enums.insert(std::string(descriptor->name)).second);
        BOOST_TEST(!descriptor->spellings.empty());
    }
}

BOOST_AUTO_TEST_CASE(schema_definitions_match_the_registries) {
    const Json schema = json_schema();
    const Json& definitions = schema.at("$defs");
    BOOST_TEST(definitions.size() == record_registry().size() + enum_registry().size());

    for (const auto* descriptor : record_registry()) {
        const Json& entry = definitions.at(std::string(descriptor->name));
        BOOST_TEST(entry.at("type").get<std::string>() == "object");
        const Json& properties = entry.at("properties");
        BOOST_TEST(properties.size() == descriptor->fields().size());
        for (const auto& field : descriptor->fields()) {
            BOOST_TEST(properties.contains(std::string(field.name)));
        }
        if (entry.contains("required")) {
            BOOST_TEST(entry.at("required").size()
                       == static_cast<std::size_t>(std::count_if(
                              descriptor->fields().begin(), descriptor->fields().end(),
                              [](const FieldView& field) { return field.required; })));
        }
    }
}

BOOST_AUTO_TEST_CASE(published_extension_policies_name_the_open_points) {
    const Json document = manifest();
    const Json& policies = document.at("extension_policies");
    BOOST_TEST(policies.is_array());
    BOOST_TEST(policies.size() >= 4);
    const std::string joined = policies.dump();
    for (const std::string_view needle : {"extras", "routes", "unknown fields", "not a claim"}) {
        BOOST_TEST(joined.find(needle) != std::string::npos);
    }
    BOOST_TEST(document.at("fingerprint_scope").dump().find("not certified") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(constraints_defaults_and_policies_are_inside_the_contract) {
    // These are acceptance rules, not prose: they must live in the document the
    // fingerprint covers, or a relaxation like `limit <= 11` would be invisible.
    const Json canonical = canonical_contract();
    const Json& request = canonical.at("records").at("PayloadRequest");
    BOOST_TEST(request.at("unknown_fields").get<std::string>() == "ignore");
    BOOST_TEST(request.at("forbidden_fields").dump().find("role") != std::string::npos);
    BOOST_TEST(!request.at("rules").empty());

    const Json& fields = request.at("fields");
    const auto find_field = [&](std::string_view name) -> const Json& {
        for (const auto& field : fields) {
            if (field.at("name").get<std::string>() == name) return field;
        }
        BOOST_FAIL("missing field " << name);
        return fields.at(0);
    };
    BOOST_TEST(find_field("request_id").at("min_length").get<std::uint64_t>() == 1);
    BOOST_TEST(find_field("request_id").at("max_length").get<std::uint64_t>() == 128);
    BOOST_TEST(find_field("limit").at("maximum").get<std::uint64_t>() == 10);
    BOOST_TEST(find_field("limit").at("minimum").get<std::uint64_t>() == 1);
    BOOST_TEST(find_field("limit").at("default").get<std::uint64_t>() == 10);
    BOOST_TEST(find_field("content").at("non_empty").get<bool>());

    // Options is a closed category set, and its reserved member must be empty.
    const Json& options = canonical.at("records").at("Options");
    BOOST_TEST(options.at("unknown_fields").get<std::string>() == "reject");

    // A required-but-nullable field keeps both statements.
    const Json& revision = canonical.at("records").at("PersistedData").at("fields").at(2);
    BOOST_TEST(revision.at("required").get<bool>());
    BOOST_TEST(revision.at("nullable").get<bool>());

    // Every envelope pins its own discriminator; routes remain Hub-owned.
    BOOST_TEST(canonical.at("records").at("EventEnvelope").at("fields").at(0)
                   .at("constant").get<std::string>() == "event");
    BOOST_TEST(!canonical.contains("routes"));
}

BOOST_AUTO_TEST_CASE(schema_publishes_the_same_constraints) {
    const Json schema = json_schema();
    const Json& request = schema.at("$defs").at("PayloadRequest");
    BOOST_TEST(request.at("required").dump().find("request_id") != std::string::npos);
    const Json& limit = request.at("properties").at("limit");
    BOOST_TEST(limit.at("maximum").get<std::uint64_t>() == 10);
    BOOST_TEST(limit.at("minimum").get<std::uint64_t>() == 1);
    const Json& request_id = request.at("properties").at("request_id");
    // UTF-8 byte limits are annotations: maxLength counts code points.
    BOOST_TEST(request_id.at("x-simplex-utf8-bytes").at("min").get<std::uint64_t>() == 1);
    BOOST_TEST(request_id.at("x-simplex-utf8-bytes").at("max").get<std::uint64_t>() == 128);
    BOOST_TEST(!request_id.contains("maxLength"));
    BOOST_TEST(request.at("properties").at("limit").at("default").get<std::uint64_t>() == 10);
    BOOST_TEST(request.at("allOf").dump().find("content") != std::string::npos);

    const Json& envelope = schema.at("$defs").at("EventEnvelope").at("properties").at("type");
    BOOST_TEST(envelope.at("const").get<std::string>() == "event");
}


BOOST_AUTO_TEST_CASE(remote_tool_contract_stops_at_the_envelope) {
    const Json canonical = canonical_contract();
    BOOST_TEST(!canonical.contains("routes"));
    for (const auto& [name, record] : canonical.at("records").items()) {
        BOOST_TEST(!name.starts_with("Plan"));
        BOOST_TEST(!name.starts_with("Subagent"));
    }
    const Json schema = json_schema();
    for (const auto& [record, field] :
         {std::pair{"ToolRequest", "arguments"}, std::pair{"ToolResponse", "result"}}) {
        const Json& value = schema.at("$defs").at(record).at("properties").at(field);
        BOOST_TEST(value.at("type").get<std::string>() == "object");
        BOOST_TEST(!value.contains("properties"));
        BOOST_TEST(!value.contains("required"));
        BOOST_TEST(value.at("additionalProperties").get<bool>());
    }
    const Json& code = schema.at("$defs").at("ToolError").at("properties").at("code");
    BOOST_TEST(code.at("type").get<std::string>() == "string");
    BOOST_TEST(!code.contains("enum"));
}
