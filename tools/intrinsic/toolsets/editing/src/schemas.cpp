#include "tools/intrinsic/editing/schemas.hpp"
#include "subprocess/platform.hpp"

#include <cstdlib>
#include <system_error>

#ifndef SIMPLEX_EDITING_SCHEMA_DIR
#error "tools_intrinsic_editing requires SIMPLEX_EDITING_SCHEMA_DIR"
#endif

namespace tools::intrinsic::editing {

std::filesystem::path schema_directory()
{
    if (const auto directory = std::getenv("SIMPLEX_EDITING_SCHEMA_DIR");
        directory != nullptr && *directory != '\0') {
        return directory;
    }
    std::error_code error;
    const auto executable = subprocess::executable_location(error);
    if (!error && !executable.empty()) {
        const auto installed = executable.parent_path() / "schemas" / "editing";
        if (std::filesystem::is_directory(installed, error)) return installed;
    }
    return SIMPLEX_EDITING_SCHEMA_DIR;
}

} // namespace tools::intrinsic::editing
