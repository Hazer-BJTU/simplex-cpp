/**
 * @file protocol/tools/export_manifest.cpp
 * @brief Print the compiled contract so the committed artifact stays generated.
 *
 * Usage: `protocol_manifest_export [--hash|--canonical|--schema]`.
 *
 * stdout carries only the requested document plus a final newline; diagnostics
 * go to stderr and a failure exits nonzero, so a CI step can diff the output
 * without filtering banners.
 */

#include "protocol/manifest.hpp"

#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? std::string_view(argv[1]) : std::string_view();
        if (argc > 2) {
            std::cerr << "protocol_manifest_export: at most one option is accepted\n";
            return 2;
        }
        if (mode.empty()) {
            std::cout << protocol::manifest().dump(2) << '\n';
        } else if (mode == "--hash") {
            std::cout << protocol::fingerprint() << '\n';
        } else if (mode == "--canonical") {
            std::cout << protocol::canonical_contract().dump(2) << '\n';
        } else if (mode == "--schema") {
            std::cout << protocol::json_schema().dump(2) << '\n';
        } else {
            std::cerr << "protocol_manifest_export: unknown option " << mode
                      << "; expected --hash, --canonical or --schema\n";
            return 2;
        }
        return std::cout ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "protocol_manifest_export: " << error.what() << '\n';
        return 1;
    }
}
