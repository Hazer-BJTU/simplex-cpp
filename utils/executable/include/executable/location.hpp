#pragma once

#include <cstddef>
#include <filesystem>
#include <system_error>
#include <boost/dll/runtime_symbol_info.hpp>

#if defined(__linux__)
#include <link.h>
#endif

namespace executable {

/**
 * Locate the executable whose code is running, rather than its ELF loader.
 *
 * With explicit musl loader invocation, /proc/self/exe (and Boost.DLL) points
 * to ld-musl. musl's first program-header entry instead names the application.
 * The portable launcher supplies an absolute ELF path, so this remains valid
 * even after a working-directory change. On glibc the main entry is normally
 * unnamed; on other platforms retain Boost.DLL's native lookup.
 *
 * No environment override is consulted. Return an empty path and an error
 * on lookup failure. As with filesystem's error-code overloads, allocation
 * errors may still throw.
 */
inline std::filesystem::path location(std::error_code& error)
{
#if defined(__linux__)
    const char* main_name = nullptr;
    dl_iterate_phdr(
        [](dl_phdr_info* entry, std::size_t, void* output) {
            // Do not allocate or throw across the C callback boundary. The
            // main executable's name stays alive for the process lifetime.
            *static_cast<const char**>(output) = entry->dlpi_name;
            return 1;
        },
        &main_name);
    if (main_name && *main_name) {
        // A relative name is not reliable after chdir(). Ordinary execution
        // can still use the native lookup; our loader always gets an absolute
        // name, avoiding that ambiguity for relocatable installations.
        const std::filesystem::path main_path(main_name);
        if (main_path.is_absolute()) {
            return std::filesystem::canonical(main_path, error);
        }
    }
#endif
    const auto path = boost::dll::program_location(error);
    return error ? std::filesystem::path{} : std::filesystem::path(path.string());
}

} // namespace executable
