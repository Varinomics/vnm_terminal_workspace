#pragma once

#include "vnm_terminal_workspace/terminal_owner_host.h"

#include <span>

namespace vnm::terminal_workspace::detail {

enum class Terminal_owner_shutdown_disposition
{
    COMPLETE,
    DRAINING,
    FAILED,
};

inline Terminal_owner_shutdown_disposition owner_shutdown_disposition(
    std::span<const Terminal_owner_update_outcome> close_results,
    bool custody_empty)
{
    if (custody_empty) {
        return Terminal_owner_shutdown_disposition::COMPLETE;
    }
    for (const Terminal_owner_update_outcome result : close_results) {
        if (result == Terminal_owner_update_outcome::REJECTED) {
            return Terminal_owner_shutdown_disposition::FAILED;
        }
    }
    return Terminal_owner_shutdown_disposition::DRAINING;
}

} // namespace vnm::terminal_workspace::detail
