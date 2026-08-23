#pragma once

#include <environment_policy/vnm_environment_policy.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vnm::terminal_workspace {

inline constexpr std::uint32_t k_terminal_launch_request_schema_version = 1U;

struct Terminal_launch_request_limits
{
    static constexpr std::size_t maximum_payload_bytes = 4U * 1024U * 1024U;
    static constexpr std::size_t maximum_identity_bytes = 256U;
    static constexpr std::size_t maximum_argument_count = 256U;
    static constexpr std::size_t maximum_argument_bytes = 256U * 1024U;
    static constexpr std::size_t maximum_working_directory_bytes = 32U * 1024U;
    static constexpr std::size_t maximum_environment_entries = 4096U;
    static constexpr std::size_t maximum_environment_bytes = 2U * 1024U * 1024U;
};

enum class Launch_request_status
{
    ACCEPTED,
    REJECTED,
    CANCELLED,
};

enum class Launch_request_error
{
    NONE,
    CANCELLED,
    MALFORMED_PAYLOAD,
    UNSUPPORTED_SCHEMA,
    TRAILING_DATA,
    QUOTA_EXCEEDED,
    EMPTY_IDENTITY,
    IDENTITY_CONTAINS_NUL,
    ARGV_EMPTY,
    EXECUTABLE_EMPTY,
    ARGUMENT_CONTAINS_NUL,
    RELATIVE_EXECUTABLE_PATH,
    WORKING_DIRECTORY_EMPTY,
    WORKING_DIRECTORY_CONTAINS_NUL,
    WORKING_DIRECTORY_NOT_ABSOLUTE,
    BASE_ENVIRONMENT_INCOMPLETE,
    BASE_ENVIRONMENT_INVALID,
    BASE_ENVIRONMENT_RESERVED_NAME,
};

enum class Launch_request_field
{
    NONE,
    LAUNCH_REQUEST_ID,
    SESSION_ID,
    CANCELLATION_ID,
    ARGV,
    WORKING_DIRECTORY,
    BASE_ENVIRONMENT,
    PAYLOAD,
};

enum class Working_directory_advisory
{
    NOT_PROBED,
    AVAILABLE,
    MISSING,
    INACCESSIBLE,
};

enum class Launch_platform
{
    WINDOWS,
    POSIX,
};

[[nodiscard]] constexpr bool valid_launch_platform(
    Launch_platform platform) noexcept
{
    return platform == Launch_platform::WINDOWS ||
        platform == Launch_platform::POSIX;
}

struct Pre_custody_cancellation
{
    std::string identity;
    bool requested = false;

    friend bool operator==(
        const Pre_custody_cancellation&,
        const Pre_custody_cancellation&) = default;
};

struct Terminal_launch_request
{
    std::string launch_request_id;
    std::string session_id;
    std::vector<std::string> argv;
    std::string working_directory;
    bool base_environment_complete = false;
    std::vector<environment_policy::Environment_entry> base_environment;
    Pre_custody_cancellation cancellation;

    friend bool operator==(
        const Terminal_launch_request&,
        const Terminal_launch_request&) = default;
};

using Working_directory_probe =
    std::function<Working_directory_advisory(std::string_view)>;

struct Launch_request_result
{
    Launch_request_status status = Launch_request_status::REJECTED;
    Launch_request_error error = Launch_request_error::NONE;
    Launch_request_field field = Launch_request_field::NONE;
    std::size_t index = 0U;
    std::optional<Terminal_launch_request> request;
    std::vector<std::uint8_t> serialized_request;
};

Launch_request_result prepare_terminal_launch_request(
    Terminal_launch_request request,
    Launch_platform platform);

Launch_request_result decode_terminal_launch_request(
    std::span<const std::uint8_t> serialized_request,
    Launch_platform platform);

Working_directory_advisory probe_terminal_working_directory(
    std::string_view working_directory,
    const Working_directory_probe& working_directory_probe);

} // namespace vnm::terminal_workspace
