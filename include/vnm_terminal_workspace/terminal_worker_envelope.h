#pragma once

#include "vnm_terminal_workspace/terminal_launch_request.h"
#include "vnm_terminal_workspace/terminal_worker_runtime.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vnm::terminal_workspace {

enum class Terminal_worker_package_capability
{
    REMOTE_UI,
    WHOLE_MESSAGE_INPUT,
};

inline constexpr std::uint32_t k_terminal_worker_envelope_schema_version = 1U;

struct Terminal_worker_envelope
{
    std::string provider_namespace;
    std::vector<std::uint8_t> serialized_request;
    Launch_platform platform = Launch_platform::WINDOWS;
    Terminal_worker_surface_configuration surface_configuration;
    std::optional<Terminal_worker_output_capture_configuration> output_capture;
    std::optional<std::vector<environment_policy::Environment_entry>>
        authorized_environment;

    friend bool operator==(
        const Terminal_worker_envelope&,
        const Terminal_worker_envelope&) = default;
};

enum class Terminal_worker_envelope_error
{
    NONE,
    MALFORMED_PAYLOAD,
    UNSUPPORTED_SCHEMA,
    TRAILING_DATA,
    QUOTA_EXCEEDED,
    INVALID_PROVIDER_NAMESPACE,
    INVALID_REQUEST,
    INVALID_AUTHORIZED_ENVIRONMENT,
};

struct Terminal_worker_envelope_result
{
    Terminal_worker_envelope_error error =
        Terminal_worker_envelope_error::MALFORMED_PAYLOAD;
    std::optional<Terminal_worker_envelope> envelope;
    std::string serialized_envelope;
};

Terminal_worker_envelope_result encode_terminal_worker_envelope(
    const Terminal_worker_envelope& envelope);

Terminal_worker_envelope_result decode_terminal_worker_envelope(
    std::string_view serialized_envelope);

} // namespace vnm::terminal_workspace
