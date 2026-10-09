#pragma once
#include <cstddef>

namespace intercom {
/** Default aggregate encoded write reservations, including the active message. */
inline constexpr std::size_t default_write_byte_capacity = 16 * 1024 * 1024;
/** Confirmation replies are correlation/decision metadata, not arbitrary output. */
inline constexpr std::size_t confirmation_reply_max_bytes = 64 * 1024;
} // namespace intercom
