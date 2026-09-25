#pragma once

#include <filesystem>

namespace tools::intrinsic::editing {

/** Environment override, installed executable-relative directory, source path. */
[[nodiscard]] std::filesystem::path schema_directory();

} // namespace tools::intrinsic::editing
