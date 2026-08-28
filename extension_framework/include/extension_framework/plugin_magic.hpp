#pragma once

/**
 * @file extension_framework/plugin_magic.hpp
 * @brief Toolchain-fingerprint admission block for dynamic plugin modules.
 *
 * This project keeps *live-object* plugins: executors, coroutine frames and
 * STL types cross the dlopen boundary, which is sound only while every module
 * was produced by the same execution context (same compiler, same standard,
 * same Boost, same ABI switches, same build profile). The magic block turns
 * that assumption into a load-time fact, following PostgreSQL's
 * PG_MODULE_MAGIC pattern: each plugin DSO embeds one exported data symbol
 * describing the toolchain it was built with, and the loader refuses the
 * module — before any alias is resolved or any factory is called — when it
 * does not match the host's own.
 *
 * Timing honesty: the check runs AFTER dlopen() returns, so the module's ELF
 * constructors and relocations have already executed by then. What the gate
 * guarantees is that no plugin LOGIC runs — no factory, no model, no
 * dialect — not that zero instructions executed. That is the strongest a
 * dlopen-based gate can claim.
 *
 * The fingerprint itself is computed at compile time (consteval FNV-1a) from
 * values CMake injects as identity strings (compiler id+version, C++ standard,
 * Boost version — Boost pins the whole tree including Asio) plus in-TU facts
 * (`_GLIBCXX_USE_CXX11_ABI`, `sizeof(void*)`, the build profile). Host and
 * plugin compiled in the same context agree bit-for-bit; anything else fails
 * loudly instead of crashing intermittently the way a forked asio runtime
 * did (see utils/asio/src/asio_runtime.cpp for that post-mortem).
 *
 * It is an engineering safeguard, not a proof of ABI compatibility: the
 * hash covers the identity inputs named above, not every build input that
 * can change layout or codegen (absent, deliberately: vendored header-only
 * third parties such as nlohmann/json, exception/sanitizer flags, -march,
 * LTO settings). It catches the mismatch classes this project has actually
 * met, at load time instead of as intermittent crashes; completeness is
 * not claimed. The real guarantee remains the deployment model — host and
 * plugins from one build context — which this check polices.
 *
 * Usage — exactly one TU of every plugin module:
 *
 *     #include "extension_framework/plugin_magic.hpp"
 *     SIMPLEX_EXPORT_PLUGIN_MAGIC
 *
 * Modules without the block are rejected as legacy/broken: there is no
 * third-party plugin ecosystem to stay compatible with, and admitting an
 * unmarked module would silently reopen the mismatched-context failure class
 * this check exists to close. A build outside this CMake (no
 * SIMPLEX_PLUGIN_BUILD_PROFILE define) hashes profile 2 and never matches —
 * intentional loudness against hand-rolled contexts; build in the provided
 * context (docs/abi-context.md, Dockerfile.build-context) instead.
 *
 * This header deliberately does not include the domain contract
 * (extensions.hpp) and stays independent of the loading pipeline; the loader
 * includes it to run check_module_magic() at its single choke point
 * (get_library_ref).
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>

#include <boost/dll/shared_library.hpp>

#include "versioning/version.hpp"

namespace extension {

inline constexpr std::uint32_t PLUGIN_MAGIC_ID = 0x53'50'4C'58u; // "SPLX"
inline constexpr std::uint32_t PLUGIN_MAGIC_VERSION = 1;

/// Fixed-layout, trivially copyable so the loader can read it straight out of
/// the module's data segment with no cross-module code involved. `size` acts
/// as a forward-compatibility guard: a layout the host does not know is a
/// mismatch, never a misread.
struct PluginMagic {
    std::uint32_t magic;           // == PLUGIN_MAGIC_ID
    std::uint32_t struct_version;  // == PLUGIN_MAGIC_VERSION
    std::uint32_t size;            // == sizeof(PluginMagic)
    std::uint32_t pointer_size;    // == sizeof(void*)
    std::uint32_t cxx11_abi;       // _GLIBCXX_USE_CXX11_ABI (0/1; 2 = unknown)
    std::uint32_t build_profile;   // 0 = Debug, 1 = everything else, 2 = not this CMake
    std::uint32_t reserved;        // 0 — kills tail-padding ambiguity
    std::uint64_t fingerprint;     // plugin_toolchain_fingerprint()
    char toolchain[48];            // e.g. "GNU-14.3.0" (always NUL-terminated)
    char standard[16];             // e.g. "C++20"
    char boost_version[16];        // e.g. "109100"
};
static_assert(std::is_standard_layout_v<PluginMagic>);
static_assert(std::is_trivially_copyable_v<PluginMagic>);
static_assert(sizeof(PluginMagic) == 120, "layout is part of the contract");

// =============================================================================
// Fingerprint computation — all inputs are translation-unit constants, so the
// whole value folds at compile time and lands in the module's read-only data.
// =============================================================================

#ifndef SIMPLEX_PLUGIN_BUILD_PROFILE
#define SIMPLEX_PLUGIN_BUILD_PROFILE 2 // not built by this CMake — mismatch loudly
#endif

/// FNV-1a over a byte range; foldable across multiple calls via @p h.
consteval std::uint64_t fnv1a(std::string_view s, std::uint64_t h) {
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

consteval std::uint64_t plugin_cxx11_abi_value() {
#if defined(_GLIBCXX_USE_CXX11_ABI)
    return static_cast<std::uint64_t>(_GLIBCXX_USE_CXX11_ABI);
#else
    return 2; // unknown — never equal to a libstdc++ build's 0/1
#endif
}

/// The toolchain fingerprint: same execution context => same value on both
/// sides of a dlopen boundary. Boost version pins the entire Boost tree
/// (including the Asio the plugins still instantiate templates from).
consteval std::uint64_t plugin_toolchain_fingerprint() {
    std::uint64_t h = 0xcbf29ce484222325ull;
    h = fnv1a(simplex::PLUGIN_TOOLCHAIN_ID, h);
    h = fnv1a(simplex::PLUGIN_CXX_STANDARD, h);
    h = fnv1a(simplex::PLUGIN_BOOST_VERSION, h);
    h ^= plugin_cxx11_abi_value();
    h *= 0x100000001b3ull;
    h ^= sizeof(void*);
    h *= 0x100000001b3ull;
    h ^= static_cast<std::uint64_t>(SIMPLEX_PLUGIN_BUILD_PROFILE);
    h *= 0x100000001b3ull;
    return h;
}

/// Copy a string literal into a fixed NUL-padded char array (at most N-1
/// bytes are taken, so the result is always a valid C string).
template <std::size_t N>
consteval std::array<char, N> fill_chars(const char* s) {
    std::array<char, N> out{}; // zero-initialized => NUL-terminated
    for (std::size_t i = 0; i + 1 < N && s[i] != '\0'; ++i) {
        out[i] = s[i];
    }
    return out;
}

/// The host-side reference block. Compiling this header in the host and in a
/// same-context plugin yields identical values — which is exactly what the
/// loader asserts. Built by assignment (raw char-array members cannot be
/// initialized from a std::array via designated initializers).
consteval PluginMagic make_plugin_magic() {
    PluginMagic m{}; // zero-initialized => NUL-terminated string fields
    m.magic = PLUGIN_MAGIC_ID;
    m.struct_version = PLUGIN_MAGIC_VERSION;
    m.size = sizeof(PluginMagic);
    m.pointer_size = sizeof(void*);
    m.cxx11_abi = static_cast<std::uint32_t>(plugin_cxx11_abi_value());
    m.build_profile = static_cast<std::uint32_t>(SIMPLEX_PLUGIN_BUILD_PROFILE);
    m.reserved = 0;
    m.fingerprint = plugin_toolchain_fingerprint();
    constexpr auto toolchain = fill_chars<48>(simplex::PLUGIN_TOOLCHAIN_ID);
    constexpr auto standard = fill_chars<16>(simplex::PLUGIN_CXX_STANDARD);
    constexpr auto boost_version = fill_chars<16>(simplex::PLUGIN_BOOST_VERSION);
    for (std::size_t i = 0; i < toolchain.size(); ++i) m.toolchain[i] = toolchain[i];
    for (std::size_t i = 0; i < standard.size(); ++i) m.standard[i] = standard[i];
    for (std::size_t i = 0; i < boost_version.size(); ++i) {
        m.boost_version[i] = boost_version[i];
    }
    return m;
}

// =============================================================================
// Export macro — one per plugin DSO
// =============================================================================

#if defined(__GNUC__) || defined(__clang__)
#define SIMPLEX_PLUGIN_MAGIC_EXPORT __attribute__((visibility("default")))
#else
#define SIMPLEX_PLUGIN_MAGIC_EXPORT // repo pins g++ on Linux; out of scope elsewhere
#endif

/// Defines the module's admission block as the unmangled data symbol
/// `simplex_plugin_magic`. The explicit default-visibility attribute keeps
/// the symbol exported even if a module is ever built with hidden defaults.
#define SIMPLEX_EXPORT_PLUGIN_MAGIC                                       \
    extern "C" SIMPLEX_PLUGIN_MAGIC_EXPORT                                \
    ::extension::PluginMagic const simplex_plugin_magic =                 \
        ::extension::make_plugin_magic()

// =============================================================================
// Loader-side check — runs inside get_library_ref() before anything is
// resolved out of the module.
// =============================================================================

enum class MagicVerdict { Ok, Absent, Mismatch };

/// Compare a loaded module's magic block against this build's own.
/// @param lib          an open shared_library handle.
/// @param diagnostic   filled on Absent/Mismatch with a human-readable cause.
inline MagicVerdict check_module_magic(
    const boost::dll::shared_library& lib, std::string& diagnostic) noexcept {
    try {
        if (!lib.has("simplex_plugin_magic")) {
            diagnostic =
                "no plugin magic block — every module must define "
                "SIMPLEX_EXPORT_PLUGIN_MAGIC (extension_framework/"
                "plugin_magic.hpp) in exactly one TU";
            return MagicVerdict::Absent;
        }
        const PluginMagic& m = lib.get<const PluginMagic>("simplex_plugin_magic");
        constexpr PluginMagic host = make_plugin_magic();
        if (m.magic == host.magic && m.struct_version == host.struct_version
            && m.size == host.size && m.pointer_size == host.pointer_size
            && m.cxx11_abi == host.cxx11_abi
            && m.build_profile == host.build_profile
            && m.fingerprint == host.fingerprint) {
            return MagicVerdict::Ok;
        }
        diagnostic = std::format(
            "plugin toolchain mismatch (plugin {} / {} / boost {} / profile {}"
            "{:s} vs host {} / {} / boost {} / profile {}) — rebuild the plugin"
            " in the same build context",
            m.toolchain, m.standard, m.boost_version, m.build_profile,
            m.fingerprint != host.fingerprint
                ? std::string(" [fingerprint differs]")
                : std::string(),
            host.toolchain, host.standard, host.boost_version,
            host.build_profile);
        return MagicVerdict::Mismatch;
    } catch (...) {
        diagnostic = "plugin magic block could not be read";
        return MagicVerdict::Mismatch;
    }
}

} // namespace extension
