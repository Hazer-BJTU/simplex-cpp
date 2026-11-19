#pragma once

namespace model_io {

/// MessageItem::extras boolean set by the provider when building TokenCost.
/// True includes an explicitly reported zero. Absence means the provider
/// did not attest a cache measurement. Raw provider usage stays in "usage".
inline constexpr char cache_hit_measured[] = "simplex.cache_hit_measured";

} // namespace model_io
