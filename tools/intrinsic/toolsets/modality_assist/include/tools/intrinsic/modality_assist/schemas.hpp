#pragma once

#include <filesystem>

namespace tools::intrinsic::modality_assist {

/**
 * Resolve declarations per call: nonempty SIMPLEX_MODALITY_ASSIST_SCHEMA_DIR environment
 * override, then <executable>/schemas/modality_assist, then the compiled source path.
 * Overrides are authoritative, including missing directories. Schema failures
 * are reported by the shared declaration loader, not hidden by fallback.
 */
[[nodiscard]] std::filesystem::path schema_directory();

} // namespace tools::intrinsic::modality_assist
