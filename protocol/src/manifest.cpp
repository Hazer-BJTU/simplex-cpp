/**
 * @file protocol/manifest.cpp
 * @brief Registry walk, manifest document and canonical fingerprint.
 *
 * The fingerprint is computed over a document that contains no explanatory
 * prose, no build metadata, no paths and no project version. Two builds of the
 * same descriptors therefore agree, and a comment edit cannot look like a
 * contract change. Regenerating the committed artifact is a build step, never a
 * hand edit.
 */

#include "protocol/manifest.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace protocol {

namespace {

const char* kind_name(Kind kind) {
    switch (kind) {
        case Kind::text: return "text";
        case Kind::integer: return "integer";
        case Kind::boolean: return "boolean";
        case Kind::any: return "any";
        case Kind::enumeration: return "enumeration";
        case Kind::record: return "record";
        case Kind::records: return "records";
        case Kind::texts: return "texts";
        case Kind::text_or_record: return "text_or_record";
        case Kind::projected_batch: return "projected_batch";
    }
    return "any";
}

/** Structure of one field; explanations are deliberately absent. */
Json canonical_field(const FieldView& field) {
    Json entry = Json::object();
    entry["kind"] = kind_name(field.kind);
    entry["name"] = field.name;
    entry["required"] = field.required;
    if (field.nullable) {
        entry["nullable"] = true;
    }
    if (field.enumeration != nullptr) {
        entry["enumeration"] = field.enumeration->name;
    }
    if (field.record != nullptr) {
        entry["record"] = field.record->name;
    }
    if (field.alternative != nullptr) entry["alternative"] = field.alternative->name;
    const auto& constraints = field.constraints;
    if (constraints.min_length) entry["min_length"] = *constraints.min_length;
    if (constraints.max_length) entry["max_length"] = *constraints.max_length;
    if (constraints.minimum) entry["minimum"] = *constraints.minimum;
    if (constraints.maximum) entry["maximum"] = *constraints.maximum;
    if (constraints.object_only) entry["object_only"] = true;
    if (constraints.empty_object) entry["empty_object"] = true;
    if (constraints.non_empty) entry["non_empty"] = true;
    if (constraints.constant != nullptr) entry["constant"] = constraints.constant;
    if (constraints.schema != nullptr) entry["schema"] = Json::parse(constraints.schema);
    if (!field.default_json.empty()) entry["default"] = Json::parse(field.default_json);
    return entry;
}

/** Published field: the structural entry plus its English explanation. */
Json described_field(const FieldView& field) {
    Json entry = canonical_field(field);
    entry["summary"] = field.summary;
    return entry;
}

Json enum_entry(const EnumDescriptor& descriptor, bool described) {
    Json entry = Json::object();
    Json spellings = Json::array();
    for (const auto spelling : descriptor.spellings) {
        spellings.push_back(spelling);
    }
    entry["spellings"] = std::move(spellings);
    if (described) {
        entry["summary"] = "Closed vocabulary; every other spelling is rejected.";
    }
    return entry;
}

Json record_entry(const RecordDescriptor& descriptor, bool described) {
    Json entry = Json::object();
    Json fields = Json::array();
    for (const auto& field : descriptor.fields()) {
        fields.push_back(described ? described_field(field) : canonical_field(field));
    }
    entry["fields"] = std::move(fields);
    if (described) {
        entry["summary"] = descriptor.summary;
    }
    return entry;
}

/** Record-level acceptance policy, read from the descriptor itself. */
inline Json policy_entry(const RecordDescriptor& descriptor) {
    Json entry = Json::object();
    entry["unknown_fields"] =
        descriptor.unknown != nullptr && descriptor.unknown() == UnknownFields::reject
            ? "reject" : "ignore";
    Json forbidden = Json::array();
    for (const auto name : descriptor.forbidden()) {
        forbidden.push_back(name);
    }
    if (!forbidden.empty()) {
        entry["forbidden_fields"] = std::move(forbidden);
    }
    Json rules = Json::array();
    for (const auto& rule : descriptor.rules()) {
        Json required = Json::array();
        for (const auto name : rule.requires_fields) {
            required.push_back(name);
        }
        Json forbids = Json::array();
        for (const auto name : rule.forbids_fields) {
            forbids.push_back(name);
        }
        rules.push_back(Json{{"when", Json{{rule.when_field, rule.when_equals}}},
                             {"requires", std::move(required)},
                             {"forbids", std::move(forbids)},
                             {"closed", rule.closed},
                             {"ignores", Json(rule.ignores_fields)}});
    }
    if (!rules.empty()) {
        entry["rules"] = std::move(rules);
    }
    return entry;
}

std::string enum_name_of(EventName event) {
    return std::string(detail::enum_to(event, "event"));
}

Json event_entry(EventName event, const EventDataSpec& spec) {
    Json entry = Json::object();
    entry["data"] = spec.record->name;
    entry["array"] = spec.array;
    entry["name"] = enum_name_of(event);
    return entry;
}

/** Structural document without prose, used for the fingerprint. */
Json canonical_document(std::span<const RecordDescriptor* const> descriptors = record_registry()) {
    Json document = Json::object();
    document["manifest_format"] = manifest_format;
    document["protocol"] = protocol_name;

    Json records = Json::object();
    for (const auto* descriptor : descriptors) {
        records[std::string(descriptor->name)] = record_entry(*descriptor, false);
    }
    for (const auto* descriptor : descriptors) {
        records[std::string(descriptor->name)].update(policy_entry(*descriptor));
    }
    document["records"] = std::move(records);

    Json enums = Json::object();
    for (const auto* descriptor : enum_registry()) {
        enums[std::string(descriptor->name)] = enum_entry(*descriptor, false);
    }
    document["enumerations"] = std::move(enums);

    Json events = Json::object();
    for (const auto& spec : event_data_registry()) {
        events[enum_name_of(spec.event)] = event_entry(spec.event, spec);
    }
    document["events"] = std::move(events);

    Json messages = Json::array();
    for (const auto& spec : message_registry()) {
        messages.push_back(Json{{"family", spec.family}, {"record", spec.record->name}});
    }
    document["messages"] = std::move(messages);

    return document;
}

} // namespace

std::span<const RecordDescriptor* const> record_registry() {
    static const RecordDescriptor* const records[] = {
        &describe_record<Content>(),
        &describe_record<Usage>(),
        &describe_record<AnswerSource>(),
        &describe_record<ExecutionIdentity>(),
        &describe_record<Call>(),
        &describe_record<Result>(),
        &describe_record<RequestContent>(),
        &describe_record<ResponseProjection>(),
        &describe_record<ToolResultMessage>(),
        &describe_record<OmissionSummary>(),
        &describe_record<OptionChoice>(),
        &describe_record<OptionCategory>(),
        &describe_record<OptionsData>(),
        &describe_record<EventQueue>(),
        &describe_record<MemoryRetention>(),
        &describe_record<AutoCompact>(),
        &describe_record<LoopProgress>(),
        &describe_record<StatusData>(),
        &describe_record<PayloadRequest>(),
        &describe_record<SignalRequest>(),
        &describe_record<ConfirmationRequest>(),
        &describe_record<ConfirmationResponse>(),
        &describe_record<ToolError>(),
        &describe_record<ToolRequest>(),
        &describe_record<ToolResponse>(),
        &describe_record<HistoryStep>(),
        &describe_record<HistoryTurn>(),
        &describe_record<HistoryPage>(),
        &describe_record<AnswerPage>(),
        &describe_record<EmptyData>(),
        &describe_record<QueryErrorData>(),
        &describe_record<InputAdmittedData>(),
        &describe_record<InputRejectedData>(),
        &describe_record<ArchiveRef>(),
        &describe_record<ArchiveCleanup>(),
        &describe_record<CompactFinishedData>(),
        &describe_record<PersistedData>(),
        &describe_record<ExportErrorData>(),
        &describe_record<FailureData>(),
        &describe_record<RunOutcomeData>(),
        &describe_record<ErrorData>(),
        &describe_record<OmissionMarker>(),
        &describe_record<EventEnvelope>(),
        &describe_record<PayloadEnvelope>(),
        &describe_record<SignalEnvelope>(),
        &describe_record<ConfirmationRequestEnvelope>(),
        &describe_record<ConfirmationResponseEnvelope>(),
        &describe_record<ToolRequestEnvelope>(),
        &describe_record<ToolResponseEnvelope>(),
    };
    return records;
}

std::span<const EnumDescriptor* const> enum_registry() {
    static const EnumDescriptor* const enums[] = {
        &describe_enum<EnvelopeType>(),
        &describe_enum<InputOperation>(),
        &describe_enum<SignalOperation>(),
        &describe_enum<ContentType>(),
        &describe_enum<Modality>(),
        &describe_enum<MessageType>(),
        &describe_enum<CallScheduling>(),
        &describe_enum<CallSecurity>(),
        &describe_enum<ConfirmationDecision>(),
        &describe_enum<EventName>(),
        &describe_enum<LoopStatus>(),
        &describe_enum<LoopPhase>(),
        &describe_enum<RunStatus>(),
        &describe_enum<PersistBoundary>(),
        &describe_enum<StorageKind>(),
        &describe_enum<RejectionCode>(),
        &describe_enum<FailureStage>(),
        &describe_enum<ToolStatus>(),
    };
    return enums;
}

Json schema_type(const FieldView& field) {
    const auto& constraints = field.constraints;
    Json type = Json::object();
    switch (field.kind) {
        case Kind::text_or_record:
            type = Json{{"anyOf", Json::array({Json{{"type", "string"}},
                Json{{"$ref", "#/$defs/" + std::string(field.record->name)}}})}};
            break;
        case Kind::projected_batch: {
            const Json marker{{"type", "object"}, {"required", Json::array({"display_omitted"})}};
            type = Json{{"type", "array"},
                {"items", Json{{"if", marker},
                    {"then", Json{{"$ref", "#/$defs/" + std::string(field.alternative->name)}}},
                    {"else", Json{{"$ref", "#/$defs/" + std::string(field.record->name)}}}}},
                {"contains", marker}, {"minContains", 0}, {"maxContains", 1},
                {"x-simplex-trailing-marker", "display_omitted"}};
            break;
        }

        case Kind::text:
            type = Json{{"type", "string"}};
            break;
        case Kind::integer:
            type = Json{{"type", "integer"}, {"minimum", 0}};
            break;
        case Kind::boolean:
            type = Json{{"type", "boolean"}};
            break;
        case Kind::any:
            // An open extension point. Constraints that do pin its container
            // kind still apply, and the schema has to say so.
            type = (constraints.object_only || constraints.empty_object)
                ? Json{{"type", "object"}}
                : Json::object();
            break;
        case Kind::enumeration:
            type = Json{{"$ref", std::string("#/$defs/") + std::string(field.enumeration->name)}};
            break;
        case Kind::record:
            type = Json{{"$ref", std::string("#/$defs/") + std::string(field.record->name)}};
            break;
        case Kind::records:
            type = Json{{"type", "array"},
                        {"items", Json{{"$ref", std::string("#/$defs/")
                                                   + std::string(field.record->name)}}}};
            break;
        case Kind::texts:
            type = Json{{"type", "array"}, {"items", Json{{"type", "string"}}}};
            break;
    }
    if (constraints.constant != nullptr) {
        type["const"] = constraints.constant;
    }
    // Lengths are UTF-8 bytes, not characters: JSON Schema `maxLength` counts
    // code points, so publishing it here would accept a 129-byte identifier of
    // 43 multibyte characters. The byte limits travel as annotations instead.
    if (constraints.min_length || constraints.max_length) {
        Json bytes = Json::object();
        if (constraints.min_length) bytes["min"] = *constraints.min_length;
        if (constraints.max_length) bytes["max"] = *constraints.max_length;
        type["x-simplex-utf8-bytes"] = std::move(bytes);
    }
    if (constraints.minimum) type["minimum"] = *constraints.minimum;
    if (constraints.maximum) type["maximum"] = *constraints.maximum;
    if (constraints.non_empty) type["minItems"] = 1;
    if (constraints.empty_object) {
        type["maxProperties"] = 0;
        type["additionalProperties"] = false;
    } else if (constraints.object_only) {
        type["additionalProperties"] = true;
    }
    if (field.nullable) {
        return Json{{"anyOf", Json::array({type, Json{{"type", "null"}}})}};
    }
    return type;
}

Json canonical_contract(std::span<const RecordDescriptor* const> descriptors) {
    return canonical_document(descriptors);
}

Json json_schema() {
    Json definitions = Json::object();
    for (const auto* descriptor : record_registry()) {
        Json properties = Json::object();
        Json required = Json::array();
        for (const auto& field : descriptor->fields()) {
            properties[std::string(field.name)] = schema_type(field);
            if (!field.default_json.empty()) {
                properties[std::string(field.name)]["default"] =
                    Json::parse(field.default_json);
            }
            if (field.required) {
                required.push_back(field.name);
            }
        }
        const bool closed = descriptor->unknown != nullptr
            && descriptor->unknown() == UnknownFields::reject;
        Json entry = Json{{"type", "object"},
                          {"description", descriptor->summary},
                          {"additionalProperties", !closed},
                          {"properties", properties}};
        Json forbidden = Json::array();
        for (const auto name : descriptor->forbidden()) {
            forbidden.push_back(Json{{"required", Json::array({name})}});
        }
        if (!forbidden.empty()) {
            entry["not"] = Json{{"anyOf", std::move(forbidden)}};
        }
        // An ignored field must not be typed by the outer schema. Each selected
        // branch supplies the active types, with its own complete property scope.
        for (const auto& rule : descriptor->rules()) {
            for (const auto name : rule.ignores_fields) {
                entry["properties"][std::string(name)] = Json::object();
            }
        }
        Json conditions = Json::array();
        for (const auto& rule : descriptor->rules()) {
            Json branch_properties = properties;
            for (const auto name : rule.ignores_fields) {
                branch_properties[std::string(name)] = Json::object();
            }
            Json then{{"properties", std::move(branch_properties)}};
            if (!rule.requires_fields.empty()) {
                Json required = Json::array();
                for (const auto name : rule.requires_fields) required.push_back(name);
                then["required"] = std::move(required);
            }
            if (!rule.forbids_fields.empty()) {
                Json forbidden_here = Json::array();
                for (const auto name : rule.forbids_fields) {
                    forbidden_here.push_back(Json{{"required", Json::array({name})}});
                }
                then["not"] = Json{{"anyOf", std::move(forbidden_here)}};
            }
            if (rule.closed) {
                then["additionalProperties"] = false;
            }
            conditions.push_back(Json{
                {"if", Json{{"properties", Json{{std::string(rule.when_field),
                                                  Json{{"const", rule.when_equals}}}}},
                             {"required", Json::array({rule.when_field})}}},
                {"then", std::move(then)}});
        }
        if (!conditions.empty()) {
            entry["allOf"] = std::move(conditions);
        }
        if (!required.empty()) {
            entry["required"] = std::move(required);
        }
        definitions[std::string(descriptor->name)] = std::move(entry);
    }
    for (const auto* descriptor : enum_registry()) {
        Json spellings = Json::array();
        for (const auto spelling : descriptor->spellings) {
            spellings.push_back(spelling);
        }
        definitions[std::string(descriptor->name)] =
            Json{{"type", "string"}, {"enum", std::move(spellings)}};
    }
    return Json{{"$schema", "https://json-schema.org/draft/2020-12/schema"},
                {"$id", "urn:simplex:worker-protocol"},
                {"title", "Simplex worker protocol"},
                {"description",
                 "Generated from the compiled descriptors. Do not edit: regenerate with "
                 "the protocol_manifest_export target."},
                {"$defs", std::move(definitions)}};
}

Json manifest() {
    Json document = canonical_document();
    for (const auto* descriptor : record_registry()) {
        Json& record = document["records"][std::string(descriptor->name)];
        record["summary"] = descriptor->summary;
        const auto views = descriptor->fields();
        auto& fields = record["fields"];
        for (std::size_t index = 0; index < views.size() && index < fields.size(); ++index) {
            fields[index]["summary"] = views[index].summary;
        }
    }
    document["json_schema"] = json_schema();
    document["extension_policies"] = Json::array({
        "extras, tool arguments, payload options and route results are open JSON owned by "
        "their provider, tool or plugin; the manifest describes their policy rather than their shape",
        "unknown fields are ignored unless a record declares rejection",
        "remote-tool routes belong to the Hub: the connection path names the route and no body "
        "field overrides it. The Hub decides support, authorization and argument validity; neither "
        "route names nor tool-specific arguments/results are part of the worker contract",
        "an unrecognized event name is not an envelope failure; a hub must tolerate future events",
        "the manifest describes built-in shapes only; it is not a claim that a configured provider "
        "or plugin capability is enabled",
    });
    document["fingerprint_scope"] = Json::array({
        "certified: field names, kinds, presence, nullability, closed vocabularies, record "
        "references, the event registry and the message registry",
        "not certified: ordering and delivery guarantees, backpressure and admission policy, "
        "resource limits, accepted option categories/values, argument and reply size budgets, cancellation and shutdown behaviour, confirmation "
        "deadlines, authorization and security judgement, persistence atomicity, and every "
        "explanatory sentence",
    });
    document["fingerprint"] = fingerprint();
    return document;
}

std::string fingerprint(std::span<const RecordDescriptor* const> descriptors) {
    const std::string canonical = canonical_contract(descriptors).dump();

    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1
        || EVP_DigestUpdate(context.get(), canonical.data(), canonical.size()) != 1) {
        throw std::runtime_error("cannot initialize the protocol fingerprint");
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest, &size) != 1) {
        throw std::runtime_error("cannot finish the protocol fingerprint");
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (unsigned int index = 0; index < size; ++index) {
        text.push_back(hex[digest[index] >> 4]);
        text.push_back(hex[digest[index] & 0x0f]);
    }
    return text;
}

} // namespace protocol
