#pragma once

//
// schemas.hpp — where the process tools' declarations live
// ========================================================
//
// Every tool in this package is declared in a YAML file under this package's
// schemas/ directory — one file per tool, named after the tool — and
// ProcessToolBase loads its own when the tool is built. The file states the
// tool's name, the description a model reads and the JSON Schema of its
// arguments; the tool's behaviour stays in src/tools.cpp. See
// tools/intrinsic/tool_declaration.hpp for the format and for what is
// deliberately not loaded from it.
//
// The directory carries one more document that is not a tool: skill.yaml, the
// set's skill — how the six are used TOGETHER, which no per-tool description
// can say. It is loaded by the set rather than by a tool
// (ProcessToolSet's constructor, tools/intrinsic/toolset_base.hpp's
// load_skill()), and its format is tools/intrinsic/skill_declaration.hpp.
//
// This header is that location, decided ONCE. The tools name a file
// ("spawn_process.yaml"); where the directory is, and how a deployment moves
// it, is answered here and nowhere else:
//
//   1. SIMPLEX_PROCESS_SCHEMA_DIR, when set and non-empty — a deployment that
//      keeps the declarations somewhere of its own choosing points this at its
//      copy, with no rebuild;
//   2. otherwise <exe_dir>/schemas/process, when that directory exists — where
//      a release installs them (the CMakeLists has the install rule), following
//      the same "beside the executable" convention as the host's plugins;
//   3. otherwise the directory this build was configured with, baked in by
//      CMake from the package's source tree (SIMPLEX_PROCESS_SCHEMA_DIR) —
//      which is what the dev tree, the test suite and any build that keeps the
//      sources around get.
//
// A path that does not exist is not an error here: the loader reports the file
// it could not read and the tool is skipped, which is the same handling every
// other broken declaration gets (tool_declaration.hpp). For skill.yaml the
// answer is milder still — the set keeps every tool and carries no guidance
// (skill_declaration.hpp).
//

#include <filesystem>

namespace tools::intrinsic {

/// The directory the process toolset's declaration files live in. See the file
/// header for how it is resolved; the answer is the same within one process.
[[nodiscard]] std::filesystem::path schema_directory();

} // namespace tools::intrinsic
