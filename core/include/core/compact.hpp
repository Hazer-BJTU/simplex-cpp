#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "dataclass/model_io.hpp"

namespace core {

/** Replacement-context budget in UTF-8 bytes, independent of provider tokens. */
inline constexpr std::uint64_t compact_context_max_bytes = 64 * 1024;

/** Shared tolerance above the nominal context/reduction target. */
inline constexpr std::uint64_t compact_context_slack_bytes = 4 * 1024;
/** Stable full-summary file under persistence.directory, for either backend. */
inline constexpr const char* compact_memory_filename = "memory.md";

/** Preflight measures retained prompt/tools, derives one permissive target, and
 * reserves pointer/marker bytes before admitting any model-text prefix. */
struct CompactPlan {
    model_io::PromptTemplate prompt;
    std::string memory_prefix;
    std::string memory_suffix;
    std::string summary_pointer;
    std::uint64_t fixed_bytes = 0;
    std::uint64_t hard_target = 0;
    std::size_t summary_bytes = 0;
    std::size_t body_bytes = 0;
};

/** Rendered prompt plus JSON tools and turns; not a provider token estimate. */
std::uint64_t compact_context_bytes(const model_io::AgentInputState& state);

/** Plan a bounded summary referencing the absolute stable summary file.
 * Rejects fixed overhead that leaves no useful body space. History is measured
 * but never copied; old memory is excluded from retained overhead. */
CompactPlan plan_compact(const model_io::AgentInputState& state,
    const std::filesystem::path& memory_file);

/** One bounded string shared by prompt injection and all compact deliveries. */
struct CompactPreview {
    model_io::PromptTemplate prompt;
    std::string summary;
    bool truncated = false;
    std::size_t injected_bytes = 0;
};

/** Prefer a nearby line boundary, otherwise cut at a UTF-8 code-point boundary.
 * Full model output is borrowed; only the bounded preview is copied. */
CompactPreview compact_prompt(CompactPlan plan, const std::string& summary);

/** Find absolute archive-directory references in retained state string values.
 * Covers host memory and explicit references in prompts, tools, extras and turns.
 * Produces only direct children of the selected root; retention still validates
 * recognized on-disk archives. Relative paths and arbitrary prose are not resolved.
 */
std::vector<std::filesystem::path> compact_archive_references(
    const model_io::AgentInputState& state,
    const std::filesystem::path& directory);

} // namespace core
