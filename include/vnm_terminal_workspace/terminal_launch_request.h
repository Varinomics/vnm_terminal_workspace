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
inline constexpr std::uint32_t k_terminal_capability_contribution_version = 1U;

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

struct Terminal_capability_contribution_limits
{
    static constexpr std::size_t maximum_payload_bytes = 256U * 1024U;
    static constexpr std::size_t maximum_identity_bytes = 256U;
    static constexpr std::size_t maximum_allowed_names = 128U;
    static constexpr std::size_t maximum_environment_entries = 128U;
    static constexpr std::size_t maximum_environment_bytes = 128U * 1024U;
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
    Working_directory_advisory working_directory_advisory =
        Working_directory_advisory::NOT_PROBED;
    std::optional<Terminal_launch_request> request;
    std::vector<std::uint8_t> serialized_request;
};

Launch_request_result prepare_terminal_launch_request(
    Terminal_launch_request request,
    Launch_platform platform,
    std::span<const std::string_view> additional_reserved_names = {},
    Working_directory_probe working_directory_probe = {});

Launch_request_result decode_terminal_launch_request(
    std::span<const std::uint8_t> serialized_request,
    Launch_platform platform,
    std::span<const std::string_view> additional_reserved_names = {},
    Working_directory_probe working_directory_probe = {});

enum class Capability_contribution_status
{
    ABSENT,
    AVAILABLE,
    DEGRADED,
    REJECTED,
    CANCELLED,
};

enum class Capability_contribution_error
{
    NONE,
    CANCELLED,
    MISSING_REQUIRED_CONTRIBUTION,
    MALFORMED_PAYLOAD,
    UNSUPPORTED_VERSION,
    TRAILING_DATA,
    QUOTA_EXCEEDED,
    EMPTY_IDENTITY,
    IDENTITY_CONTAINS_NUL,
    BINDING_MISMATCH,
    INVALID_LIFETIME,
    INVALID_ALLOWED_NAME,
    DUPLICATE_ALLOWED_NAME,
    NONCANONICAL_ALLOWED_NAME_ORDER,
    INVALID_ENVIRONMENT_NAME,
    RESERVED_ENVIRONMENT_NAME,
    TERMINAL_OWNED_ENVIRONMENT_NAME,
    LOOKUP_SENSITIVE_ENVIRONMENT_NAME,
    ENVIRONMENT_NAME_NOT_ALLOWED,
    DUPLICATE_ENVIRONMENT_NAME,
    ENVIRONMENT_VALUE_CONTAINS_NUL,
};

enum class Capability_contribution_field
{
    NONE,
    CAPABILITY_ID,
    ISSUER,
    INTENDED_CONSUMER,
    PRODUCT_SCOPE,
    SESSION_SCOPE,
    LIFETIME_ID,
    ALLOWED_ENVIRONMENT_NAMES,
    ENVIRONMENT,
    PAYLOAD,
};

struct Terminal_capability_contribution
{
    std::uint32_t version = k_terminal_capability_contribution_version;
    std::string capability_id;
    std::string issuer;
    std::string intended_consumer;
    std::string product_scope;
    std::string session_scope;
    std::string lifetime_id;
    std::uint64_t issued_at_unix_ms = 0U;
    std::uint64_t expires_at_unix_ms = 0U;
    // The codec emits ascending unsigned UTF-8 byte order after rejecting
    // platform-equivalent duplicates and case collisions.
    std::vector<std::string> allowed_environment_names;
    std::vector<environment_policy::Environment_entry> environment;

    friend bool operator==(
        const Terminal_capability_contribution&,
        const Terminal_capability_contribution&) = default;
};

struct Terminal_capability_contribution_envelope
{
    bool present = false;
    Terminal_capability_contribution contribution;
};

struct Terminal_capability_binding_policy
{
    bool required = false;
    std::string capability_id;
    std::string expected_issuer;
    std::string intended_consumer;
    std::string product_scope;
    std::string session_scope;
    std::string lifetime_id;
    std::uint64_t evaluation_time_unix_ms = 0U;
    std::vector<std::string> allowed_environment_names;
    std::vector<std::string> additional_reserved_input_names;
};

struct Capability_contribution_result
{
    Capability_contribution_status status =
        Capability_contribution_status::REJECTED;
    Capability_contribution_error error =
        Capability_contribution_error::NONE;
    Capability_contribution_field field =
        Capability_contribution_field::NONE;
    std::size_t index = 0U;
    std::optional<Terminal_capability_contribution> contribution;
    std::vector<std::uint8_t> serialized_contribution;
};

Capability_contribution_result serialize_terminal_capability_contribution(
    Terminal_capability_contribution_envelope envelope,
    Launch_platform platform,
    const Terminal_capability_binding_policy& policy,
    const Pre_custody_cancellation& cancellation = {});

Capability_contribution_result decode_terminal_capability_contribution(
    std::span<const std::uint8_t> serialized_contribution,
    Launch_platform platform,
    const Terminal_capability_binding_policy& policy,
    const Pre_custody_cancellation& cancellation = {});

} // namespace vnm::terminal_workspace
