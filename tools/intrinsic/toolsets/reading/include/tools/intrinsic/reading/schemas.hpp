#pragma once

#include <filesystem>

namespace tools::intrinsic::reading {

/**
 * Resolve declarations per call: nonempty SIMPLEX_READING_SCHEMA_DIR environment
 * override, then <executable>/schemas/reading, then the compiled source path.
 * Overrides are authoritative, including missing directories. Schema failures
 * are reported by the shared declaration loader, not hidden by fallback.
 */
[[nodiscard]] std::filesystem::path schema_directory();

} // namespace tools::intrinsic::reading
