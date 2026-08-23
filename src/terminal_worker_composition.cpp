#include "vnm_terminal_workspace/terminal_worker_composition.h"

namespace vnm::terminal_workspace {

std::optional<std::string>
Neutral_terminal_worker_package_policy::encode_configuration(
    const Configuration&) noexcept
{
    return std::string{};
}

std::optional<Terminal_worker_neutral_configuration>
Neutral_terminal_worker_package_policy::decode_configuration(
    std::string_view serialized) noexcept
{
    return serialized.empty()
        ? std::optional<Configuration>(Configuration{})
        : std::nullopt;
}

void Neutral_terminal_worker_package_policy::clear_configuration(
    Configuration&) noexcept
{}

} // namespace vnm::terminal_workspace
