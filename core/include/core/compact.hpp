#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "dataclass/model_io.hpp"

namespace core {

/** Replacement-context budget in UTF-8 bytes, independent of provider tokens. */
inline constexpr std::uint64_t compact_context_max_bytes = 64 * 1024;

/** Fixed retained prompt and exact memory wrapper planned before any disk write.
 * History is never copied into this plan. The raw summary allowance is bounded
 * by both the remaining replacement budget and the 32 KiB summary ceiling.
 */
struct CompactPlan {
    model_io::PromptTemplate prompt;
    std::string memory_prefix;
    std::string memory_suffix;
    std::uint64_t fixed_bytes = 0;
    std::size_t summary_bytes = 0;
};

/** Deterministic byte measure; not a tokenizer or provider capacity estimate.
 * Adds rendered prompt, JSON tool declarations and one serialized turn at a time.
 */
std::uint64_t compact_context_bytes(const model_io::AgentInputState& state);

/** Derive the summary allowance from actual fixed overhead, including paths.
 * Throws an actionable preflight diagnostic if no summary can fit. The supplied
 * archive pathname need not exist; the caller reserves it only after preflight.
 */
CompactPlan plan_compact(
    const model_io::AgentInputState& state,
    const std::filesystem::path& memory_directory,
    const std::filesystem::path& archive_file);

/** Validate original UTF-8 summary bytes, then fill the planned memory section.
 * Does not truncate the summary. Callers also validate the completed replacement
 * and the existing 10% reduction rule before publishing a snapshot.
 */
model_io::PromptTemplate compact_prompt(CompactPlan plan, const std::string& summary);

/** Find absolute archive-directory references in retained state string values.
 * Covers host memory and explicit references in prompts, tools, extras and turns.
 * Produces only direct children of the selected root; retention still validates
 * recognized on-disk archives. Relative paths and arbitrary prose are not resolved.
 */
std::vector<std::filesystem::path> compact_archive_references(
    const model_io::AgentInputState& state,
    const std::filesystem::path& directory);

} // namespace core
