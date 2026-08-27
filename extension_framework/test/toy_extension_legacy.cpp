/**
 * @file toy_extension_legacy.cpp
 * @brief Admission-gate fixture: a plausible extension module with NO toolchain
 *        magic block at all.
 *
 * The "legacy / hand-rolled" shape — a module that simply does not define
 * SIMPLEX_EXPORT_PLUGIN_MAGIC. Under the same-execution-context strategy there
 * is no such thing as an acceptable unmarked module (host and plugins are
 * always rebuilt together), so the loader must reject it with the diagnostic
 * that names the missing macro, not admit it on trust.
 */

#include "toy_extension_spec.hpp"

#include "extension_framework/extensions.hpp"

#include <boost/dll/alias.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace ext_test_legacy {

namespace {

class LegacyExtension final : public extension::ExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override {
        return ext_test::TOY_EXTENSION_ABI_VERSION;
    }

    std::string_view name() const noexcept override {
        return "toy_legacy";
    }
};

} // namespace

std::unique_ptr<extension::ExtensionContext> create_toy_extension() {
    return std::make_unique<LegacyExtension>();
}

} // namespace ext_test_legacy

BOOST_DLL_ALIAS(ext_test_legacy::create_toy_extension, create_toy_extension)
