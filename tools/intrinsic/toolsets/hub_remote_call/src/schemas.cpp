#include "tools/intrinsic/hub_remote_call/schemas.hpp"
#include "subprocess/platform.hpp"

#include <cstdlib>
#include <system_error>

#ifndef SIMPLEX_HUB_REMOTE_CALL_SCHEMA_DIR
#error "tools_intrinsic_hub_remote_call requires SIMPLEX_HUB_REMOTE_CALL_SCHEMA_DIR"
#endif

namespace tools::intrinsic::hub_remote_call {

std::filesystem::path schema_directory()
{
    if (const auto directory = std::getenv("SIMPLEX_HUB_REMOTE_CALL_SCHEMA_DIR");
        directory != nullptr && *directory != '\0') {
        return directory;
    }
    std::error_code error;
    // Locate the host executable, not the tool library. If lookup or the
    // installed directory fails, keep the build-time schema path as fallback.
    const auto executable = subprocess::executable_location(error);
    if (!error && !executable.empty()) {
        const auto installed = executable.parent_path() / "schemas" / "hub_remote_call";
        if (std::filesystem::is_directory(installed, error)) {
            return installed;
        }
    }
    return SIMPLEX_HUB_REMOTE_CALL_SCHEMA_DIR;
}

} // namespace tools::intrinsic::hub_remote_call
