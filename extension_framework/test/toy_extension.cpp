/**
 * @file toy_extension.cpp
 * @brief A toy extension module (libtoyextension.so) for the dynamic-load test.
 *
 * Builds into a dynamically-loaded library that exports `create_toy_extension`
 * via BOOST_DLL_ALIAS. The returned object is a concrete ExtensionContext whose
 * vtable + destructor live in this .so — exactly the lifetime situation the
 * framework's library-binding contract is designed to keep safe.
 */

#include "toy_extension_spec.hpp"

#include "extension_framework/extensions.hpp"

#include <boost/dll/alias.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace ext_test {

namespace {

class ToyExtension final : public extension::ExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override {
        return TOY_EXTENSION_ABI_VERSION;
    }

    std::string_view name() const noexcept override {
        return TOY_EXTENSION_NAME;
    }

    long priority() const noexcept override {
        return TOY_EXTENSION_PRIORITY;
    }
};

} // namespace

// Factory imported by the host via import_alias<unique_ptr<ExtensionContext>()>.
// Return type matches the import_alias signature exactly (no reinterpretation
// across the boundary).
std::unique_ptr<extension::ExtensionContext> create_toy_extension() {
    return std::make_unique<ToyExtension>();
}

} // namespace ext_test

BOOST_DLL_ALIAS(ext_test::create_toy_extension, create_toy_extension)
