#pragma once

#include <cstdint>
#include <span>

namespace vnm::terminal_workspace::testing {

using Capability_secret_wipe_observer =
    void (*)(std::span<const std::uint8_t> wiped_bytes);

void set_capability_secret_wipe_observer(
    Capability_secret_wipe_observer observer);

} // namespace vnm::terminal_workspace::testing
