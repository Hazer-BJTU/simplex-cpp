#include "tools/intrinsic/modality_assist/schemas.hpp"

#include <cstdlib>
#include <system_error>

#ifndef SIMPLEX_MODALITY_ASSIST_SCHEMA_DIR
#error "tools_intrinsic_modality_assist requires SIMPLEX_MODALITY_ASSIST_SCHEMA_DIR"
#endif

namespace tools::intrinsic::modality_assist {

std::filesystem::path schema_directory()
{
    if (const auto directory = std::getenv("SIMPLEX_MODALITY_ASSIST_SCHEMA_DIR");
        directory != nullptr && *directory != '\0') {
        return directory;
    }
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !executable.empty()) {
        const auto installed = executable.parent_path() / "schemas" / "modality_assist";
        if (std::filesystem::is_directory(installed, error)) {
            return installed;
        }
    }
    return SIMPLEX_MODALITY_ASSIST_SCHEMA_DIR;
}

} // namespace tools::intrinsic::modality_assist
