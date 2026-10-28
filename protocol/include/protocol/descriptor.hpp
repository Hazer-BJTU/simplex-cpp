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
    integer,
    boolean,
    any,
    enumeration,
    record,
    records,
    texts,
};

/** Whether a record tolerates fields it does not describe. */
enum class UnknownFields { ignore, reject };

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
};

/** A described record: a wire name, an explanation and its field views. */
struct RecordDescriptor {
    std::string_view name;
    std::string_view summary;
    std::span<const FieldView> (*fields)() = nullptr;
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
};

template <class M>
struct is_optional : std::false_type {};
template <class M>
struct is_optional<std::optional<M>> : std::true_type {};
template <class M>
inline constexpr bool is_optional_v = is_optional<std::remove_cvref_t<M>>::value;

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

// ---- Record description -----------------------------------------------------

namespace detail {

template <class R, class FieldType>
void append_view(std::vector<FieldView>& views, const FieldType& field) {
    views.push_back(FieldView{field.name, field.kind, !is_optional_v<decltype(std::declval<R&>().*field.member)>,
                              field.nullable, field.summary, field.enumeration, field.record});
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

template <class R>
constexpr UnknownFields unknown_policy() {
    if constexpr (requires { wire_unknown_fields(std::declval<const R&>()); }) {
        return wire_unknown_fields(std::declval<const R&>());
    } else {
        return UnknownFields::ignore;
    }
}

} // namespace detail

template <class R>
const RecordDescriptor& describe_record() {
    static const R instance{};
    static const std::vector<FieldView> views = detail::field_views<R>();
    static const RecordDescriptor descriptor{wire_name(instance), wire_summary(instance), [] {
                                                 return std::span<const FieldView>(views);
                                             }};
    return descriptor;
}

} // namespace protocol
