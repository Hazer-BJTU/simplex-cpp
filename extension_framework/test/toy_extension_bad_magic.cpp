/**
 * @file toy_extension_bad_magic.cpp
 * @brief Admission-gate fixture: a plausible extension module whose toolchain
 *        magic block carries a WRONG fingerprint.
 *
 * Simulates a plugin built in a different execution context (another compiler
 * version, another Boost, another profile — the exact cause is folded into the
 * fingerprint). The loader must reject it inside get_library_ref(), before any
 * alias is resolved or any code in this module runs.
 *
 * The block is defined manually instead of via SIMPLEX_EXPORT_PLUGIN_MAGIC so
 * the fingerprint can be perturbed; everything else matches a real module.
 */

#include "extension_framework/plugin_magic.hpp"
#include "toy_extension_spec.hpp"

#include "extension_framework/extensions.hpp"

#include <boost/dll/alias.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace ext_test_badmagic {

namespace {

class BadMagicExtension final : public extension::ExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override {
        return ext_test::TOY_EXTENSION_ABI_VERSION;
    }

    std::string_view name() const noexcept override {
        return "toy_bad_magic";
    }
};

/// The host's own block with flipped fingerprint bits (evaluated at compile
/// time, so the consteval make_plugin_magic() call stays constant).
constexpr extension::PluginMagic corrupted_magic() {
    auto m = extension::make_plugin_magic();
    m.fingerprint ^= 0xdead'beef'cafe'f00dull;
    return m;
}

} // namespace

std::unique_ptr<extension::ExtensionContext> create_toy_extension() {
    return std::make_unique<BadMagicExtension>();
}

} // namespace ext_test_badmagic

BOOST_DLL_ALIAS(ext_test_badmagic::create_toy_extension, create_toy_extension)

// The deliberately-wrong admission block: same layout, same identity strings,
// flipped fingerprint bits.
extern "C" __attribute__((visibility("default")))
extension::PluginMagic const simplex_plugin_magic =
    ext_test_badmagic::corrupted_magic();
