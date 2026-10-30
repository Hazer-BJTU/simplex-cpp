#pragma once

/**
 * @file protocol/descriptor.hpp
 * @brief One explicit field list per wire record, shared by codec and schema.
 *
 * A record declares its wire fields exactly once, in `wire_fields()`. That
 * single declaration feeds three consumers:
 *
 *   - the codec (`protocol::to_json` / `protocol::from_json`),
 *   - the generated manifest and JSON Schema descriptions,
 *   - the canonical structural fingerprint.
 *
 * There is deliberately no reflection and no schema text to keep in sync: a
 * field that is not in `wire_fields()` does not exist on the wire, and a field
 * that is there is described automatically. Presence is inferred from the
 * member type (`std::optional<M>` is optional, anything else is required), so
 * the two can never disagree.
 *
 * Open extension points stay open on purpose. `opaque` fields carry
 * `nlohmann::json` and are documented as provider-, tool- or plugin-owned; they
 * are *described* by the manifest, not constrained by it.
 */

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace protocol {

using Json = nlohmann::json;

class ValidationError : public std::runtime_error {
public:
    ValidationError(std::string path, std::string message)
        : std::runtime_error(path.empty() ? message : path + ": " + message) {}
};

/** How a described value is spelled on the wire. */
enum class Kind {
    text,
    integer,  // Unsigned 64-bit count/cursor, independent of application quotas.
    boolean,
    any,
    enumeration,
    enumerations,
    record,
    records,
    texts,
    text_or_record,
    projected_batch,
};

/** Whether a record tolerates fields it does not describe. */
enum class UnknownFields { ignore, reject };

/**
 * Presence of a field, independent of how its value is stored. Most fields
 * infer presence from `std::optional`, but the wire language needs one more
 * combination: `persisted.revision` is always present and may still be null.
 */
enum class Presence { inferred, required, optional };

/** Stateless acceptance rules that do not need application state. */
struct Constraints {
    std::optional<std::uint64_t> min_length;
    std::optional<std::uint64_t> max_length;
    std::optional<std::uint64_t> minimum;
    std::optional<std::uint64_t> maximum;
    /** The value must be a JSON object (an open one unless more is declared). */
    bool object_only = false;
    /** The value must be an empty JSON object. */
    bool empty_object = false;
    /** The value must be a nonempty array. */
    bool non_empty = false;
    /** An enumeration member pinned to one spelling, e.g. an envelope type. */
    const char* constant = nullptr;
    /** Extra JSON Schema fragment published for an open value. */
    const char* schema = nullptr;
};

/** One conditional requirement attached to a record. */
struct Rule {
    /** Field whose value selects the rule. */
    std::string_view when_field;
    /** Spelling that activates it. */
    std::string_view when_equals;
    std::span<const std::string_view> requires_fields;
    std::span<const std::string_view> forbids_fields;
    /** True when this scope rejects every field it does not name. */
    bool closed = false;
    /** Fields accepted without decoding for this selector value. */
    std::span<const std::string_view> ignores_fields;
};

struct RecordDescriptor;
struct EnumDescriptor;

/** One field as the manifest and schema generator see it. */
struct FieldView {
    std::string_view name;
    Kind kind = Kind::any;
    bool required = true;
    /** True when an explicit JSON null is part of the accepted language. */
    bool nullable = false;
    std::string_view summary;
    const EnumDescriptor* enumeration = nullptr;
    const RecordDescriptor* record = nullptr;
    Constraints constraints{};
    /** Published default as JSON text; empty when the field has none. */
    std::string_view default_json;
    const RecordDescriptor* alternative = nullptr;
};

/** A described record: a wire name, an explanation and its field views. */
struct RecordDescriptor {
    std::string_view name;
    std::string_view summary;
    std::span<const FieldView> (*fields)() = nullptr;
    /** Effective acceptance policy, published and fingerprinted per record. */
    UnknownFields (*unknown)() = nullptr;
    std::span<const std::string_view> (*forbidden)() = nullptr;
    std::span<const Rule> (*rules)() = nullptr;
};

/** A closed vocabulary with its exact wire spellings. */
struct EnumDescriptor {
    std::string_view name;
    std::span<const std::string_view> spellings;
};

/** One (value, spelling) pair of a closed vocabulary. */
template <class E>
struct EnumValue {
    E value;
    std::string_view spelling;
};

/** Per-enum description: specialize once beside the enum itself. */
template <class E>
struct EnumTraits;

template <class E>
inline constexpr bool is_described_enum = requires { EnumTraits<E>::name; };

/** Describe an enum from its trait specialization, building the view once. */
template <class E>
    requires is_described_enum<E>
const EnumDescriptor& describe_enum() {
    static const std::vector<std::string_view> spellings = [] {
        std::vector<std::string_view> result;
        result.reserve(std::size(EnumTraits<E>::values));
        for (const auto& entry : EnumTraits<E>::values) {
            result.push_back(entry.spelling);
        }
        return result;
    }();
    static const EnumDescriptor descriptor{EnumTraits<E>::name, spellings};
    return descriptor;
}

template <class R>
const RecordDescriptor& describe_record();

/** A field bound to one member of its record. */
template <class T, class M>
struct Field {
    std::string_view name;
    M T::*member = nullptr;
    Kind kind = Kind::any;
    std::string_view summary;
    bool nullable = false;
    const EnumDescriptor* enumeration = nullptr;
    const RecordDescriptor* record = nullptr;
    Presence presence = Presence::inferred;
    Constraints constraints{};
    const char* default_json = "";
    const RecordDescriptor* alternative = nullptr;
    void (*validate_json)(const Json&, const std::string&) = nullptr;

    Field& required() & { presence = Presence::required; return *this; }
    Field&& required() && { presence = Presence::required; return std::move(*this); }
    Field& nullable_value() & { nullable = true; return *this; }
    Field&& nullable_value() && { nullable = true; return std::move(*this); }
    Field& min_length(std::uint64_t value) & { constraints.min_length = value; return *this; }
    Field&& min_length(std::uint64_t value) && { constraints.min_length = value; return std::move(*this); }
    Field& max_length(std::uint64_t value) & { constraints.max_length = value; return *this; }
    Field&& max_length(std::uint64_t value) && { constraints.max_length = value; return std::move(*this); }
    Field& minimum(std::uint64_t value) & { constraints.minimum = value; return *this; }
    Field&& minimum(std::uint64_t value) && { constraints.minimum = value; return std::move(*this); }
    Field& maximum(std::uint64_t value) & { constraints.maximum = value; return *this; }
    Field&& maximum(std::uint64_t value) && { constraints.maximum = value; return std::move(*this); }
    Field& object_only() & { constraints.object_only = true; return *this; }
    Field&& object_only() && { constraints.object_only = true; return std::move(*this); }
    Field& empty_object() & { constraints.empty_object = true; return *this; }
    Field&& empty_object() && { constraints.empty_object = true; return std::move(*this); }
    Field& non_empty() & { constraints.non_empty = true; return *this; }
    Field&& non_empty() && { constraints.non_empty = true; return std::move(*this); }
    Field& constant(const char* spelling) & { constraints.constant = spelling; return *this; }
    Field&& constant(const char* spelling) && { constraints.constant = spelling; return std::move(*this); }
    Field& default_value(const char* text) & { default_json = text; return *this; }
    Field&& default_value(const char* text) && { default_json = text; return std::move(*this); }
};

template <class M>
struct is_optional : std::false_type {};
template <class M>
struct is_optional<std::optional<M>> : std::true_type {};
template <class M>
inline constexpr bool is_optional_v = is_optional<std::remove_cvref_t<M>>::value;

/** Presence after inference: `std::optional<M>` is optional unless overridden. */
template <class T, class M>
bool is_required(const Field<T, M>& field) {
    if (field.presence == Presence::required) return true;
    if (field.presence == Presence::optional) return false;
    return !is_optional_v<M>;
}

template <class M>
inline constexpr bool is_json_v = std::is_same_v<std::remove_cvref_t<M>, Json>;

template <class M>
struct is_record_vector : std::false_type {};
template <class M>
struct is_record_vector<std::vector<M>> : std::true_type {};
template <class M>
inline constexpr bool is_record_vector_v = is_record_vector<std::remove_cvref_t<M>>::value;

/**
 * A record is anything that can describe itself through `wire_fields()`.
 * The free functions are found by ADL, so a record stays a plain aggregate and
 * nothing has to be registered by hand.
 */
template <class R>
concept DescribedRecord = std::default_initializable<R> && requires(const R& record) {
    wire_fields(record);
    wire_name(record);
};

template <class M>
inline constexpr bool always_false = false;

// ---- Field builders ---------------------------------------------------------
// Each builder binds a member to its wire name, shape, and English explanation.
// The summary is what an operator reads in the generated manifest, so it states
// the rule that actually holds, not a restatement of the field name.

template <class T, class M>
Field<T, M> text(std::string_view name, M T::*member, std::string_view summary,
                 bool nullable = false) {
    return {name, member, Kind::text, summary, nullable};
}

template <class T, class M>
Field<T, M> integer(std::string_view name, M T::*member, std::string_view summary,
                    bool nullable = false) {
    return {name, member, Kind::integer, summary, nullable};
}

template <class T, class M>
Field<T, M> boolean(std::string_view name, M T::*member, std::string_view summary,
                    bool nullable = false) {
    return {name, member, Kind::boolean, summary, nullable};
}

/** An open JSON value whose permitted shape is documented, not constrained. */
template <class T, class M>
Field<T, M> opaque(std::string_view name, M T::*member, std::string_view summary,
                   bool nullable = false) {
    return {name, member, Kind::any, summary, nullable};
}

template <class E, class T, class M>
    requires is_described_enum<E>
Field<T, M> enumeration(std::string_view name, M T::*member, std::string_view summary,
                        bool nullable = false) {
    return {name, member, Kind::enumeration, summary, nullable, &describe_enum<E>(), nullptr};
}

template <class R, class T, class M>
    requires DescribedRecord<R>
Field<T, M> record(std::string_view name, M T::*member, std::string_view summary,
                   bool nullable = false) {
    return {name, member, Kind::record, summary, nullable, nullptr, &describe_record<R>()};
}

template <class R, class T, class M>
    requires DescribedRecord<R>
Field<T, M> records(std::string_view name, M T::*member, std::string_view summary,
                    bool nullable = false) {
    return {name, member, Kind::records, summary, nullable, nullptr, &describe_record<R>()};
}

template <class T, class M>
Field<T, M> texts(std::string_view name, M T::*member, std::string_view summary,
                  bool nullable = false) {
    return {name, member, Kind::texts, summary, nullable};
}

/**
 * A closed vocabulary spelled as a JSON array of strings.
 *
 * Use texts() instead for extensible feature names that consumers must preserve.
 */
template <class E, class T, class M>
    requires is_described_enum<E>
Field<T, M> enumerations(std::string_view name, M T::*member, std::string_view summary,
                         bool nullable = false) {
    return {name, member, Kind::enumerations, summary, nullable, &describe_enum<E>(), nullptr};
}

namespace detail {
template <class R>
void validate_text_or_record(const Json&, const std::string&);
template <class R, class Marker>
void validate_projected_batch(const Json&, const std::string&);
}

/** A worker-owned union; unlike opaque JSON, both alternatives have fixed shapes. */
template <class R, class T, class M>
Field<T, M> text_or_record(std::string_view name, M T::*member, std::string_view summary) {
    Field<T, M> field{name, member, Kind::text_or_record, summary};
    field.record = &describe_record<R>();
    field.validate_json = &detail::validate_text_or_record<R>;
    return field;
}

/** A homogeneous projected array with at most one trailing omission notice. */
template <class R, class Marker, class T, class M>
Field<T, M> projected_batch(std::string_view name, M T::*member, std::string_view summary) {
    Field<T, M> field{name, member, Kind::projected_batch, summary};
    field.record = &describe_record<R>();
    field.alternative = &describe_record<Marker>();
    field.validate_json = &detail::validate_projected_batch<R, Marker>;
    return field;
}

// ---- Record description -----------------------------------------------------

namespace detail {

template <class R, class FieldType>
void append_view(std::vector<FieldView>& views, const FieldType& field) {
    views.push_back(FieldView{field.name, field.kind, is_required(field), field.nullable,
                              field.summary, field.enumeration, field.record,
                              field.constraints, field.default_json, field.alternative});
}

template <class R>
std::vector<FieldView> field_views() {
    // A default instance exists because from_json() already requires one; using
    // it keeps ADL lookup in an evaluated context, where declval() is illegal.
    const R instance{};
    std::vector<FieldView> views;
    std::apply([&](const auto&... field) { (append_view<R>(views, field), ...); },
               wire_fields(instance));
    return views;
}

} // namespace detail

template <class R>
UnknownFields unknown_policy() {
    if constexpr (requires(const R& record) { wire_unknown_fields(record); }) {
        const R instance{};
        return wire_unknown_fields(instance);
    } else {
        return UnknownFields::ignore;
    }
}

/** Fields a record rejects by name, even though it ignores other unknowns. */
template <class R>
std::span<const std::string_view> forbidden_fields() {
    if constexpr (requires(const R& record) { wire_forbidden_fields(record); }) {
        const R instance{};
        return wire_forbidden_fields(instance);
    } else {
        return {};
    }
}

/** Conditional requirements attached to a record, if any. */
template <class R>
std::span<const Rule> rules() {
    if constexpr (requires(const R& record) { wire_rules(record); }) {
        const R instance{};
        return wire_rules(instance);
    } else {
        return {};
    }
}


template <class R>
const RecordDescriptor& describe_record() {
    static const R instance{};
    static const std::vector<FieldView> views = detail::field_views<R>();
    static const RecordDescriptor descriptor{
        wire_name(instance), wire_summary(instance),
        [] { return std::span<const FieldView>(views); },
        [] { return unknown_policy<R>(); },
        [] { return forbidden_fields<R>(); },
        [] { return rules<R>(); }};
    return descriptor;
}

} // namespace protocol
