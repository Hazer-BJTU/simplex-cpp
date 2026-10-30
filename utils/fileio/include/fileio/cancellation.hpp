#pragma once

#include <stop_token>
#include <system_error>

namespace fileio {

/** Cooperative pre-publication checkpoint; never use after atomic rename. */
inline void check_cancelled(std::stop_token stop)
{
    if (stop.stop_requested()) {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled),
                                "file operation cancelled before completion/publication");
    }
}

} // namespace fileio
