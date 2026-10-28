#pragma once

/**
 * @file protocol/codec.hpp
 * @brief Serialize, parse and validate described records from one field list.
 *
 * The codec is deliberately strict about shape and permissive only where the
 * existing worker protocol is permissive:
 *
 *   - a missing required field fails; the error names the full JSON path;
 *   - an explicit `null` fails unless the field declares null part of the
 *     language (for example `persisted.revision`);
 *   - an unknown field is ignored unless the record opts into rejection;
 *   - closed vocabularies reject unknown spellings instead of silently
 *     accepting them.
 *
 * Records may add `wire_validate()` for cross-field invariants (an operation
 * that requires `content`, a decision that must name one of two values). Those
 * run after structural parsing, so the diagnostic can talk about the
 * combination rather than the field.
 */

#include "protocol/descriptor.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace protocol {

namespace detail {

inline std::string join_path(const std::string& path, std::string_view name) {
    return path.empty() ? std::string(name) : path + "." + std::string(name);
}

template <class E>
std::string_view enum_to(E value, const std::string& path) {
    for (const auto& entry : EnumTraits<E>::values) {
        if (entry.value == value) {
            return entry.spelling;
        }
    }
    throw ValidationError(path, "unrecognized enumeration value");
}

template <class E>
E enum_from(std::string_view text, const std::string& path) {
    for (const auto& entry : EnumTraits<E>::values) {
        if (entry.spelling == text) {
            return entry.value;
        }
    }
    std::string accepted;
    for (const auto& entry : EnumTraits<E>::values) {
        if (!accepted.empty()) {
            accepted += ", ";
        }
        accepted += entry.spelling;
    }
    throw ValidationError(path, "expected one of " + accepted);
}

template <class M>
Json encode_one(const M& value);

template <class M>
void decode_one(M& target, const Json& input, const std::string& path);

template <class R, class FieldType>
void read_field(R& record, const Json& input, const FieldType& field, const std::string& path);

template <class FieldType>
void check_constraints(const FieldType& field, const Json& value, const std::string& path);

/** True when a rule's selector field currently holds its activating value. */
template <class R>
bool rule_matches(const R& record, const Rule& rule);

/** True when the named field is present (an optional member holds a value). */
template <class R>
bool field_present(const R& record, std::string_view name);

} // namespace detail

/** Serialize one described record, omitting absent optional fields. */
template <class R>
    requires DescribedRecord<R>
Json to_json(const R& record) {
    Json output = Json::object();
    std::apply(
        [&](const auto&... field) {
            const auto write = [&](const auto& descriptor) {
                using Member = std::remove_cvref_t<decltype(record.*(descriptor.member))>;
                const std::string name(descriptor.name);
                if constexpr (is_optional_v<Member>) {
                    if (record.*(descriptor.member)) {
                        output[name] = detail::encode_one(*(record.*(descriptor.member)));
                    } else if (is_required(descriptor) && descriptor.nullable) {
                        // Required and nullable: the key is always present, and
                        // the only way to say "no value" is an explicit null.
                        output[name] = nullptr;
                    }
                } else {
                    output[name] = detail::encode_one(record.*(descriptor.member));
                }
            };
            (write(field), ...);
        },
        wire_fields(record));
    return output;
}

/** Parse one described record, rejecting shapes the wire language excludes. */
template <class R>
    requires DescribedRecord<R>
R from_json(const Json& input, const std::string& path = {}) {
    if (!input.is_object()) {
        throw ValidationError(path, "expected a JSON object");
    }
    R record{};
    std::apply(
        [&](const auto&... field) { (detail::read_field(record, input, field, path), ...); },
        wire_fields(std::as_const(record)));

    for (const auto forbidden : detail::forbidden_fields<R>()) {
        if (input.contains(std::string(forbidden))) {
            throw ValidationError(detail::join_path(path, forbidden),
                                  "this record does not accept that field");
        }
    }

    if (detail::unknown_policy<R>() == UnknownFields::reject) {
        const auto& descriptor = describe_record<R>();
        for (const auto& [key, value] : input.items()) {
            const auto known = std::ranges::any_of(descriptor.fields(), [&](const FieldView& view) {
                return view.name == key;
            });
            if (!known) {
                throw ValidationError(detail::join_path(path, key), "unknown field");
            }
        }
    }

    // Conditional requirements are data, not code: a rule names the field that
    // selects it and the fields it requires or forbids, so the same table is
    // published in the manifest and enforced here.
    for (const auto& rule : detail::rules<R>()) {
        if (!detail::rule_matches(record, rule)) continue;
        for (const auto present : rule.requires_fields) {
            if (!detail::field_present(record, present)) {
                throw ValidationError(detail::join_path(path, present),
                                      std::string("required when ") + std::string(rule.when_field)
                                          + " is " + std::string(rule.when_equals));
            }
        }
        for (const auto forbidden : rule.forbids_fields) {
            if (detail::field_present(record, forbidden)) {
                throw ValidationError(detail::join_path(path, forbidden),
                                      std::string("not accepted when ") + std::string(rule.when_field)
                                          + " is " + std::string(rule.when_equals));
            }
        }
    }

    if constexpr (requires(const R& value) { wire_validate(value); }) {
        wire_validate(record);
    }
    return record;
}

namespace detail {

template <class M>
Json encode_one(const M& value) {
    using Type = std::remove_cvref_t<M>;
    if constexpr (is_json_v<Type>) {
        return value;
    } else if constexpr (std::is_same_v<Type, std::string>) {
        return Json(value);
    } else if constexpr (std::is_same_v<Type, bool>) {
        return Json(value);
    } else if constexpr (std::is_integral_v<Type>) {
        return Json(value);
    } else if constexpr (is_described_enum<Type>) {
        return Json(std::string(enum_to<Type>(value, {})));
    } else if constexpr (DescribedRecord<Type>) {
        return protocol::to_json(value);
    } else if constexpr (is_record_vector_v<Type>) {
        Json output = Json::array();
        for (const auto& item : value) {
            output.push_back(encode_one(item));
        }
        return output;
    } else {
        static_assert(always_false<Type>, "unsupported wire member type");
    }
}

template <class M>
void decode_one(M& target, const Json& input, const std::string& path) {
    using Type = std::remove_cvref_t<M>;
    if constexpr (is_json_v<Type>) {
        target = input;
    } else if constexpr (std::is_same_v<Type, std::string>) {
        if (!input.is_string()) {
            throw ValidationError(path, "expected a string");
        }
        target = input.get<std::string>();
    } else if constexpr (std::is_same_v<Type, bool>) {
        if (!input.is_boolean()) {
            throw ValidationError(path, "expected a boolean");
        }
        target = input.get<bool>();
    } else if constexpr (std::is_integral_v<Type>) {
        if (!input.is_number_integer() && !input.is_number_unsigned()) {
            throw ValidationError(path, "expected an integer");
        }
        if constexpr (std::is_signed_v<Type>) {
            if (input.is_number_unsigned() && input.get<std::uint64_t>() > static_cast<std::uint64_t>(
                                                                       std::numeric_limits<Type>::max())) {
                throw ValidationError(path, "integer is out of range");
            }
        } else {
            if (input.is_number_integer() && input.get<std::int64_t>() < 0) {
                throw ValidationError(path, "expected a nonnegative integer");
            }
        }
        target = input.get<Type>();
    } else if constexpr (is_described_enum<Type>) {
        if (!input.is_string()) {
            throw ValidationError(path, "expected an enumeration spelling");
        }
        target = enum_from<Type>(input.get<std::string>(), path);
    } else if constexpr (DescribedRecord<Type>) {
        target = protocol::from_json<Type>(input, path);
    } else if constexpr (is_record_vector_v<Type>) {
        if (!input.is_array()) {
            throw ValidationError(path, "expected an array");
        }
        target.clear();
        target.reserve(input.size());
        for (std::size_t index = 0; index < input.size(); ++index) {
            using Element = typename Type::value_type;
            if constexpr (DescribedRecord<Element>) {
                target.push_back(protocol::from_json<Element>(input.at(index), path + "[" + std::to_string(index) + "]"));
            } else {
                Element element{};
                decode_one(element, input.at(index), path + "[" + std::to_string(index) + "]");
                target.push_back(std::move(element));
            }
        }
    } else {
        static_assert(always_false<Type>, "unsupported wire member type");
    }
}

template <class R, class FieldType>
void read_field(R& record, const Json& input, const FieldType& field, const std::string& path) {
    using Member = std::remove_cvref_t<decltype(record.*(field.member))>;
    const std::string name(field.name);
    const std::string here = detail::join_path(path, field.name);
    const auto found = input.find(name);
    if (found == input.end()) {
        if (is_required(field)) {
            throw ValidationError(here, "missing required field");
        }
        return;
    }
    if (found->is_null()) {
        if (!field.nullable) {
            throw ValidationError(here, "explicit null is not accepted");
        }
        // The key exists and is null: leave an optional member unset, and keep
        // a JSON member's null (some fields carry null as a value).
        if constexpr (!is_optional_v<Member>) {
            if constexpr (is_json_v<Member>) {
                record.*(field.member) = Json(nullptr);
            }
        }
        return;
    }
    if constexpr (is_optional_v<Member>) {
        using Value = typename Member::value_type;
        Value value{};
        decode_one(value, *found, here);
        check_constraints(field, *found, here);
        record.*(field.member) = std::move(value);
    } else {
        decode_one(record.*(field.member), *found, here);
        check_constraints(field, *found, here);
    }
}

template <class FieldType>
void check_constraints(const FieldType& field, const Json& value, const std::string& path) {
    const auto& constraints = field.constraints;
    if ((constraints.object_only || constraints.empty_object) && !value.is_object()) {
        throw ValidationError(path, "expected a JSON object");
    }
    if (constraints.empty_object && !value.empty()) {
        throw ValidationError(path, "must be an empty object");
    }
    if (constraints.non_empty && value.is_array() && value.empty()) {
        throw ValidationError(path, "must be a nonempty array");
    }
    if ((constraints.min_length || constraints.max_length) && value.is_string()) {
        const auto size = value.get_ref<const std::string&>().size();
        if (constraints.min_length && size < *constraints.min_length) {
            throw ValidationError(path, "value is shorter than the minimum length");
        }
        if (constraints.max_length && size > *constraints.max_length) {
            throw ValidationError(path, "value is longer than the maximum length");
        }
    }
    if ((constraints.minimum || constraints.maximum)
        && (value.is_number_unsigned() || value.is_number_integer())) {
        const auto number = value.get<std::int64_t>();
        if (constraints.minimum && number < *constraints.minimum) {
            throw ValidationError(path, "value is below the minimum");
        }
        if (constraints.maximum && number > *constraints.maximum) {
            throw ValidationError(path, "value is above the maximum");
        }
    }
    if (constraints.constant != nullptr && value.is_string()
        && value.get_ref<const std::string&>() != constraints.constant) {
        throw ValidationError(path, std::string("must be \"") + constraints.constant + "\"");
    }
}

template <class R>
bool rule_matches(const R& record, const Rule& rule) {
    bool matched = false;
    std::apply(
        [&](const auto&... field) {
            (([&] {
                 if (std::string_view(field.name) != rule.when_field) {
                     return;
                 }
                 const auto& value = record.*(field.member);
                 using Member = std::remove_cvref_t<decltype(value)>;
                 if constexpr (is_described_enum<Member>) {
                     matched = enum_to(value, {}) == rule.when_equals;
                 } else if constexpr (std::is_same_v<Member, std::string>) {
                     matched = value == rule.when_equals;
                 }
             }()),
             ...);
        },
        wire_fields(record));
    return matched;
}

template <class R>
bool field_present(const R& record, std::string_view name) {
    bool present = false;
    std::apply(
        [&](const auto&... field) {
            (([&] {
                 if (std::string_view(field.name) != name) {
                     return;
                 }
                 const auto& value = record.*(field.member);
                 using Member = std::remove_cvref_t<decltype(value)>;
                 if constexpr (is_optional_v<Member>) {
                     present = value.has_value();
                 } else {
                     present = true;
                 }
             }()),
             ...);
        },
        wire_fields(record));
    return present;
}

} // namespace detail

} // namespace protocol
