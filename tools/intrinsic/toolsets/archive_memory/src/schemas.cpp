#include "tools/intrinsic/archive_memory/schemas.hpp"
#include "subprocess/platform.hpp"

#include <cstdlib>
#include <system_error>

#ifndef SIMPLEX_ARCHIVE_MEMORY_SCHEMA_DIR
#error "tools_intrinsic_archive_memory requires SIMPLEX_ARCHIVE_MEMORY_SCHEMA_DIR"
#endif

namespace tools::intrinsic::archive_memory {

std::filesystem::path schema_directory()
{
    if (const auto directory = std::getenv("SIMPLEX_ARCHIVE_MEMORY_SCHEMA_DIR");
        directory != nullptr && *directory != '\0') {
        return directory;
    }
    std::error_code error;
    // Locate the host executable, not the tool library. If lookup or the
    // installed directory fails, keep the build-time schema path as fallback.
    const auto executable = subprocess::executable_location(error);
    if (!error && !executable.empty()) {
        const auto installed = executable.parent_path() / "schemas" / "archive_memory";
        if (std::filesystem::is_directory(installed, error)) {
            return installed;
        }
    }
    return SIMPLEX_ARCHIVE_MEMORY_SCHEMA_DIR;
}

} // namespace tools::intrinsic::archive_memory
