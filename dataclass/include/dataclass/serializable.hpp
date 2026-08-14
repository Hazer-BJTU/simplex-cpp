#pragma once

//
// serializable.hpp — JSON-serializable base interface
// ====================================================
//
// The base interface every dataclass record derives from. It declares the
// serialization contract (virtual to_json/from_json) plus the wire PROTOCOL
// every implementation follows, so (de)serialization travels with the type
// itself — there is no separate serialization module.
//
// Record bodies never hand-roll JSON member access: every field is written and
// read through the uniform field codec family (to_json / from_json /
// to_json_array / from_json_array below), with the `dataclass::optional` token
// marking optional fields. The plain and optional flavours of each function
// are generated from a single implementation template parameterised on a bool
// (detail::*_impl<IsOptional> below).
//
// ----------------------------------------------------------------------------
// SERIALIZATION PROTOCOL
// ----------------------------------------------------------------------------
//  1. Each record serialises to a JSON object.
//  2. Object keys are snake_case and match the C++ field names.
//  3. Optional fields are OMITTED when empty and present when set (never
//     emitted as JSON null); a missing key on read yields std::nullopt.
//  4. Enumerations serialise as lowercase snake_case STRING names
//     (e.g. ContentType::ExternalRef -> "external_ref"), never integers. An
//     unrecognised value on read falls back to the first listed mapping, so
//     types should list their safest/neutral value first.
//  5. nlohmann::json fields (e.g. arguments, extras) embed inline as-is.
//  6. Unknown keys are ignored on read (forward-compatible). A MISSING key
//     keeps the member's default (plain fields) or yields std::nullopt
//     (optional fields) — members therefore carry meaningful default
//     initialisers.
//  7. Round-trip invariant: from_json(to_json(x)) reproduces x.
//

#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace dataclass {

// Token selecting the optional flavour of a field codec function, e.g.
//     dataclass::to_json(j, "reasoning", reasoning, dataclass::optional);
struct optional_t {
    explicit optional_t() = default;
};
inline constexpr optional_t optional{};

// Base interface for JSON-serializable dataclass records.
class Serializable {
public:
    virtual ~Serializable() = default;

    virtual nlohmann::json to_json() const = 0;
    virtual void from_json(const nlohmann::json& j) = 0;

    // Deserialize in place: `record = json` is the same as
    // `record.from_json(json)`. NOTE: a derived class's implicitly-declared
    // copy assignment hides base-class overloads, so every record re-exposes
    // this operator with `using dataclass::Serializable::operator=;`.
    Serializable& operator=(const nlohmann::json& j) {
        from_json(j);
        return *this;
    }

    // Convenience: serialise to / from a JSON string.
    std::string to_json_string() const {
        return to_json().dump();
    }
    void from_json_string(std::string_view s) {
        from_json(nlohmann::json::parse(s)); // throws on malformed input
    }
};

// Constructing *from* JSON cannot live in the base: during base-class
// construction a virtual from_json call would dispatch to the pure base
// implementation, not the override. Each concrete record therefore declares
// its own one-liner next to the using-declaration:
//
//     X() = default;
//     explicit X(const nlohmann::json& j) { from_json(j); }
//     using dataclass::Serializable::operator=;
//
// giving every record `X x{json}` construction and `x = json` assignment on
// top of the interface.

namespace detail {

// Convert one value to JSON: records via their to_json(), anything nlohmann
// can already store (primitives, enums via NLOHMANN_JSON_SERIALIZE_ENUM,
// nlohmann::json) as-is.
template <std::derived_from<Serializable> T>
nlohmann::json jsonify(const T& v) {
    return v.to_json();
}
template <class T>
    requires(!std::derived_from<T, Serializable> &&
             requires(nlohmann::json x, const T& v) { x = v; })
nlohmann::json jsonify(const T& v) {
    return v;
}

// Convert JSON into one value: records via their from_json(), anything
// nlohmann can already read as-is.
template <std::derived_from<Serializable> T>
void unjsonify(const nlohmann::json& jv, T& out) {
    out.from_json(jv);
}
template <class T>
    requires(!std::derived_from<T, Serializable> &&
             requires(const nlohmann::json& x) { x.template get<T>(); })
void unjsonify(const nlohmann::json& jv, T& out) {
    out = jv.get<T>();
}

template <class T>
nlohmann::json jsonify_array(const std::vector<T>& v) {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& e : v) {
        a.push_back(jsonify(e));
    }
    return a;
}

template <class T>
void unjsonify_array(const nlohmann::json& jv, std::vector<T>& out) {
    out.clear();
    out.reserve(jv.size());
    for (const auto& e : jv) {
        T t;
        unjsonify(e, t);
        out.push_back(std::move(t));
    }
}

// One implementation per codec function; the plain and optional flavours are
// both instantiated from these templates via the IsOptional non-type
// parameter (`T` is the field's storage type: plain value or std::optional).
// Plain: always written / missing key keeps the member's default.
// Optional: omitted when empty / std::nullopt when missing.
template <bool IsOptional, class T>
void to_json_impl(nlohmann::json& j, const char* key, const T& v) {
    if constexpr (IsOptional) {
        if (v.has_value()) {
            j[key] = jsonify(*v);
        }
    } else {
        j[key] = jsonify(v);
    }
}

template <bool IsOptional, class T>
void to_json_array_impl(nlohmann::json& j, const char* key, const T& v) {
    if constexpr (IsOptional) {
        if (v.has_value()) {
            j[key] = jsonify_array(*v);
        }
    } else {
        j[key] = jsonify_array(v);
    }
}

template <bool IsOptional, class T>
void from_json_impl(const nlohmann::json& j, const char* key, T& v) {
    if (!j.contains(key)) {
        if constexpr (IsOptional) {
            v.reset();
        }
        return; // plain field: missing key keeps the member's default
    }
    if constexpr (IsOptional) {
        v.emplace();
        unjsonify(j.at(key), *v);
    } else {
        unjsonify(j.at(key), v);
    }
}

template <bool IsOptional, class T>
void from_json_array_impl(const nlohmann::json& j, const char* key, T& v) {
    if (!j.contains(key)) {
        if constexpr (IsOptional) {
            v.reset();
        }
        return;
    }
    if constexpr (IsOptional) {
        v.emplace();
        unjsonify_array(j.at(key), *v);
    } else {
        unjsonify_array(j.at(key), v);
    }
}

} // namespace detail

// ---- field codec family (the uniform record-body style) ---------------------
//
//     dataclass::to_json(j, "role", role);                       // plain field
//     dataclass::to_json(j, "reasoning", reasoning, optional);   // optional
//     dataclass::to_json_array(j, "agent_loop_step", steps);
//     dataclass::to_json_array(j, "invokes", invokes, optional);
//     dataclass::from_json(j, "role", role);
//     dataclass::from_json(j, "reasoning", reasoning, optional);
//     dataclass::from_json_array(j, "agent_loop_step", steps);
//     dataclass::from_json_array(j, "invokes", invokes, optional);

template <class T>
void to_json(nlohmann::json& j, const char* key, const T& v) {
    detail::to_json_impl<false>(j, key, v);
}

template <class T>
void to_json(nlohmann::json& j, const char* key, const std::optional<T>& v,
             optional_t) {
    detail::to_json_impl<true>(j, key, v);
}

template <class T>
void to_json_array(nlohmann::json& j, const char* key,
                   const std::vector<T>& v) {
    detail::to_json_array_impl<false>(j, key, v);
}

template <class T>
void to_json_array(nlohmann::json& j, const char* key,
                   const std::optional<std::vector<T>>& v, optional_t) {
    detail::to_json_array_impl<true>(j, key, v);
}

template <class T>
void from_json(const nlohmann::json& j, const char* key, T& v) {
    detail::from_json_impl<false>(j, key, v);
}

template <class T>
void from_json(const nlohmann::json& j, const char* key, std::optional<T>& v,
               optional_t) {
    detail::from_json_impl<true>(j, key, v);
}

template <class T>
void from_json_array(const nlohmann::json& j, const char* key,
                     std::vector<T>& v) {
    detail::from_json_array_impl<false>(j, key, v);
}

template <class T>
void from_json_array(const nlohmann::json& j, const char* key,
                     std::optional<std::vector<T>>& v, optional_t) {
    detail::from_json_array_impl<true>(j, key, v);
}

} // namespace dataclass
