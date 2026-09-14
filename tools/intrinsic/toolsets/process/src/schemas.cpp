#include "tools/intrinsic/process/schemas.hpp"

#include <cstdlib>
#include <system_error>

// The build has to say where the declarations are — this package cannot guess,
// and a build without the definition would hand the loader an empty path and
// skip every tool. Fail the build, not the run.
#ifndef SIMPLEX_PROCESS_SCHEMA_DIR
#error "tools_intrinsic_process must be built with SIMPLEX_PROCESS_SCHEMA_DIR set (its CMakeLists.txt does)"
#endif

namespace tools::intrinsic {
namespace {

/// The environment override's name, deliberately the same spelling as the
/// macro it overrides: one is what a build bakes in, the other is what a
/// deployment points elsewhere, and they answer the same question.
constexpr const char* kSchemaDirectoryEnv = "SIMPLEX_PROCESS_SCHEMA_DIR";

/// Where an installed release keeps the declarations: `<exe_dir>/schemas/process`,
/// the same "beside the executable" convention the host uses for its plugins
/// (`<exe_dir>/plugins/llm`, cmake/SimplexRelease.cmake). The CMakeLists installs
/// them there, so a release needs no configuration to find them; the build tree
/// has no such directory and falls through to the path the build baked in.
///
/// Empty when the running executable cannot be located, which is the honest
/// answer rather than a guess: /proc is Linux, and a platform without it simply
/// uses the other two answers.
std::filesystem::path beside_executable()
{
    std::error_code failure;
    const std::filesystem::path self =
        std::filesystem::read_symlink("/proc/self/exe", failure);
    if (failure || self.empty()) {
        return {};
    }
    return self.parent_path() / "schemas" / "process";
}

bool directory_exists(const std::filesystem::path& directory)
{
    if (directory.empty()) return false;
    std::error_code failure;
    return std::filesystem::is_directory(directory, failure);
}

} // namespace

std::filesystem::path schema_directory()
{
    // Read per call rather than cached: a lookup against the environment and
    // one stat are cheaper than the file open that follows, and a test that
    // points a process at its own directory gets what it asked for.
    if (const char* override_directory = std::getenv(kSchemaDirectoryEnv);
        override_directory != nullptr && *override_directory != '\0') {
        return std::filesystem::path(override_directory);
    }
    // A directory shipped beside the executable WINS over the path the build
    // baked in: only one of the two ever exists in a given tree — a release
    // carries the first, a build tree the second — and taking the one that is
    // really there is what keeps "tested" and "shipped" the same layout.
    if (const std::filesystem::path installed = beside_executable();
        directory_exists(installed)) {
        return installed;
    }
    return std::filesystem::path(SIMPLEX_PROCESS_SCHEMA_DIR);
}

} // namespace tools::intrinsic
