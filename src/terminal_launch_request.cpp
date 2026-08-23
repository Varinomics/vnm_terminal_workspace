#include "vnm_terminal_workspace/terminal_launch_request.h"

#include <algorithm>
#include <array>
#include <utility>

namespace vnm::terminal_workspace {
namespace {

using environment_policy::Environment_entry;
using environment_policy::Environment_platform;

constexpr std::array<std::uint8_t, 8> k_request_magic{
    'V', 'N', 'M', 'T', 'R', 'Q', '0', '1',
};

struct Request_validation
{
    Launch_request_status status = Launch_request_status::REJECTED;
    Launch_request_error error = Launch_request_error::NONE;
    Launch_request_field field = Launch_request_field::NONE;
    std::size_t index = 0U;
};

Environment_platform environment_platform(Launch_platform platform)
{
    return platform == Launch_platform::WINDOWS
        ? Environment_platform::WINDOWS
        : Environment_platform::POSIX;
}

bool contains_nul(std::string_view value)
{
    return value.find('\0') != std::string_view::npos;
}

bool would_exceed(
    std::size_t accumulated,
    std::size_t addition,
    std::size_t limit)
{
    return addition > limit || accumulated > limit - addition;
}

bool is_windows_separator(char character)
{
    return character == '\\' || character == '/';
}

bool is_windows_absolute_path(std::string_view path)
{
    const bool drive_absolute =
        path.size() >= 3U &&
        ((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' &&
        is_windows_separator(path[2]);
    const bool network_absolute =
        path.size() >= 3U &&
        is_windows_separator(path[0]) &&
        is_windows_separator(path[1]) &&
        !is_windows_separator(path[2]);
    return drive_absolute || network_absolute;
}

bool is_absolute_path(std::string_view path, Launch_platform platform)
{
    if (platform == Launch_platform::WINDOWS) {
        return is_windows_absolute_path(path);
    }
    return !path.empty() && path.front() == '/';
}

bool contains_path_separator(
    std::string_view executable,
    Launch_platform platform)
{
    if (platform == Launch_platform::WINDOWS) {
        return std::any_of(
            executable.begin(),
            executable.end(),
            is_windows_separator);
    }
    return executable.find('/') != std::string_view::npos;
}

bool is_drive_relative_windows_path(std::string_view executable)
{
    return
        executable.size() >= 2U &&
        ((executable[0] >= 'A' && executable[0] <= 'Z') ||
         (executable[0] >= 'a' && executable[0] <= 'z')) &&
        executable[1] == ':' &&
        !is_windows_absolute_path(executable);
}

Request_validation request_rejection(
    Launch_request_error error,
    Launch_request_field field,
    std::size_t index = 0U)
{
    return {
        Launch_request_status::REJECTED,
        error,
        field,
        index,
    };
}

Request_validation validate_identity(
    std::string_view identity,
    Launch_request_field field)
{
    if (identity.empty()) {
        return request_rejection(
            Launch_request_error::EMPTY_IDENTITY,
            field);
    }
    if (contains_nul(identity)) {
        return request_rejection(
            Launch_request_error::IDENTITY_CONTAINS_NUL,
            field);
    }
    if (identity.size() >
        Terminal_launch_request_limits::maximum_identity_bytes)
    {
        return request_rejection(
            Launch_request_error::QUOTA_EXCEEDED,
            field);
    }
    return {
        Launch_request_status::ACCEPTED,
        Launch_request_error::NONE,
        Launch_request_field::NONE,
    };
}

Request_validation validate_request(
    const Terminal_launch_request& request,
    Launch_platform platform)
{
    Request_validation identity = validate_identity(
        request.launch_request_id,
        Launch_request_field::LAUNCH_REQUEST_ID);
    if (identity.status != Launch_request_status::ACCEPTED) {
        return identity;
    }
    identity = validate_identity(
        request.session_id,
        Launch_request_field::SESSION_ID);
    if (identity.status != Launch_request_status::ACCEPTED) {
        return identity;
    }
    identity = validate_identity(
        request.cancellation.identity,
        Launch_request_field::CANCELLATION_ID);
    if (identity.status != Launch_request_status::ACCEPTED) {
        return identity;
    }

    if (request.argv.empty()) {
        return request_rejection(
            Launch_request_error::ARGV_EMPTY,
            Launch_request_field::ARGV);
    }
    if (request.argv.size() >
        Terminal_launch_request_limits::maximum_argument_count)
    {
        return request_rejection(
            Launch_request_error::QUOTA_EXCEEDED,
            Launch_request_field::ARGV);
    }

    std::size_t argument_bytes = 0U;
    for (std::size_t index = 0U; index < request.argv.size(); ++index) {
        const std::string& argument = request.argv[index];
        if (index == 0U && argument.empty()) {
            return request_rejection(
                Launch_request_error::EXECUTABLE_EMPTY,
                Launch_request_field::ARGV,
                index);
        }
        if (contains_nul(argument)) {
            return request_rejection(
                Launch_request_error::ARGUMENT_CONTAINS_NUL,
                Launch_request_field::ARGV,
                index);
        }
        if (would_exceed(
                argument_bytes,
                argument.size(),
                Terminal_launch_request_limits::maximum_argument_bytes))
        {
            return request_rejection(
                Launch_request_error::QUOTA_EXCEEDED,
                Launch_request_field::ARGV,
                index);
        }
        argument_bytes += argument.size();
    }

    const std::string_view executable = request.argv.front();
    if ((contains_path_separator(executable, platform) &&
         !is_absolute_path(executable, platform)) ||
        (platform == Launch_platform::WINDOWS &&
         is_drive_relative_windows_path(executable)))
    {
        return request_rejection(
            Launch_request_error::RELATIVE_EXECUTABLE_PATH,
            Launch_request_field::ARGV);
    }

    if (request.working_directory.empty()) {
        return request_rejection(
            Launch_request_error::WORKING_DIRECTORY_EMPTY,
            Launch_request_field::WORKING_DIRECTORY);
    }
    if (contains_nul(request.working_directory)) {
        return request_rejection(
            Launch_request_error::WORKING_DIRECTORY_CONTAINS_NUL,
            Launch_request_field::WORKING_DIRECTORY);
    }
    if (request.working_directory.size() >
        Terminal_launch_request_limits::maximum_working_directory_bytes)
    {
        return request_rejection(
            Launch_request_error::QUOTA_EXCEEDED,
            Launch_request_field::WORKING_DIRECTORY);
    }
    if (!is_absolute_path(request.working_directory, platform)) {
        return request_rejection(
            Launch_request_error::WORKING_DIRECTORY_NOT_ABSOLUTE,
            Launch_request_field::WORKING_DIRECTORY);
    }

    if (!request.base_environment_complete) {
        return request_rejection(
            Launch_request_error::BASE_ENVIRONMENT_INCOMPLETE,
            Launch_request_field::BASE_ENVIRONMENT);
    }
    if (request.base_environment.size() >
        Terminal_launch_request_limits::maximum_environment_entries)
    {
        return request_rejection(
            Launch_request_error::QUOTA_EXCEEDED,
            Launch_request_field::BASE_ENVIRONMENT);
    }

    std::size_t environment_bytes = 0U;
    for (std::size_t index = 0U;
         index < request.base_environment.size();
         ++index)
    {
        const Environment_entry& entry = request.base_environment[index];
        if (would_exceed(
                environment_bytes,
                entry.name.size(),
                Terminal_launch_request_limits::maximum_environment_bytes) ||
            would_exceed(
                environment_bytes + entry.name.size(),
                entry.value.size(),
                Terminal_launch_request_limits::maximum_environment_bytes))
        {
            return request_rejection(
                Launch_request_error::QUOTA_EXCEEDED,
                Launch_request_field::BASE_ENVIRONMENT,
                index);
        }
        environment_bytes += entry.name.size() + entry.value.size();
    }

    const environment_policy::Environment_sanitization_result sanitized =
        environment_policy::sanitize_explicit_base_environment(
            request.base_environment,
            environment_platform(platform));
    if (!sanitized.accepted) {
        const std::size_t index = sanitized.issues.empty()
            ? 0U
            : sanitized.issues.front().index;
        return request_rejection(
            Launch_request_error::BASE_ENVIRONMENT_INVALID,
            Launch_request_field::BASE_ENVIRONMENT,
            index);
    }
    if (!sanitized.issues.empty() ||
        sanitized.entries != request.base_environment)
    {
        const std::size_t index = sanitized.issues.empty()
            ? 0U
            : sanitized.issues.front().index;
        return request_rejection(
            Launch_request_error::BASE_ENVIRONMENT_RESERVED_NAME,
            Launch_request_field::BASE_ENVIRONMENT,
            index);
    }

    Request_validation result;
    result.status = request.cancellation.requested
        ? Launch_request_status::CANCELLED
        : Launch_request_status::ACCEPTED;
    result.error = request.cancellation.requested
        ? Launch_request_error::CANCELLED
        : Launch_request_error::NONE;
    return result;
}

class Byte_writer
{
public:
    void append(std::span<const std::uint8_t> bytes)
    {
        m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
    }

    void append_u8(std::uint8_t value)
    {
        m_bytes.push_back(value);
    }

    void append_u32(std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8) {
            m_bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void append_string(std::string_view value)
    {
        append_u32(static_cast<std::uint32_t>(value.size()));
        const auto* data = reinterpret_cast<const std::uint8_t*>(value.data());
        append({data, value.size()});
    }

    std::vector<std::uint8_t> take()
    {
        return std::move(m_bytes);
    }

private:
    std::vector<std::uint8_t> m_bytes;
};

class Byte_reader
{
public:
    explicit Byte_reader(std::span<const std::uint8_t> bytes)
    :
        m_bytes(bytes)
    {}

    bool read_exact(std::span<const std::uint8_t> expected)
    {
        if (remaining() < expected.size()) {
            return false;
        }
        const auto candidate = m_bytes.subspan(m_offset, expected.size());
        if (!std::equal(candidate.begin(), candidate.end(), expected.begin())) {
            return false;
        }
        m_offset += expected.size();
        return true;
    }

    bool read_u8(std::uint8_t& value)
    {
        if (remaining() < 1U) {
            return false;
        }
        value = m_bytes[m_offset++];
        return true;
    }

    bool read_u32(std::uint32_t& value)
    {
        if (remaining() < 4U) {
            return false;
        }
        value = 0U;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(m_bytes[m_offset++]) << shift;
        }
        return true;
    }

    bool read_string(std::string& value, std::size_t maximum_bytes)
    {
        std::uint32_t size = 0U;
        if (!read_u32(size) || size > maximum_bytes || remaining() < size) {
            return false;
        }
        const char* data = reinterpret_cast<const char*>(
            m_bytes.data() + m_offset);
        value.assign(data, size);
        m_offset += size;
        return true;
    }

    bool at_end() const
    {
        return m_offset == m_bytes.size();
    }

private:
    std::size_t remaining() const
    {
        return m_bytes.size() - m_offset;
    }

    std::span<const std::uint8_t> m_bytes;
    std::size_t m_offset = 0U;
};

std::vector<std::uint8_t> serialize_request(
    const Terminal_launch_request& request)
{
    Byte_writer writer;
    writer.append(k_request_magic);
    writer.append_u32(k_terminal_launch_request_schema_version);
    writer.append_string(request.launch_request_id);
    writer.append_string(request.session_id);
    writer.append_string(request.cancellation.identity);
    writer.append_u8(request.cancellation.requested ? 1U : 0U);
    writer.append_u32(static_cast<std::uint32_t>(request.argv.size()));
    for (const std::string& argument : request.argv) {
        writer.append_string(argument);
    }
    writer.append_string(request.working_directory);
    writer.append_u8(request.base_environment_complete ? 1U : 0U);
    writer.append_u32(
        static_cast<std::uint32_t>(request.base_environment.size()));
    for (const Environment_entry& entry : request.base_environment) {
        writer.append_string(entry.name);
        writer.append_string(entry.value);
    }
    return writer.take();
}

Launch_request_result malformed_request_result(Launch_request_error error)
{
    Launch_request_result result;
    result.error = error;
    result.field = Launch_request_field::PAYLOAD;
    return result;
}

Launch_request_result finish_request_result(
    Terminal_launch_request request,
    Request_validation validation,
    std::vector<std::uint8_t> serialized_request = {})
{
    Launch_request_result result;
    result.status = validation.status;
    result.error = validation.error;
    result.field = validation.field;
    result.index = validation.index;
    if (validation.status != Launch_request_status::REJECTED) {
        result.request = std::move(request);
        result.serialized_request = std::move(serialized_request);
    }
    return result;
}

} // namespace

Launch_request_result prepare_terminal_launch_request(
    Terminal_launch_request request,
    Launch_platform platform)
{
    if (!valid_launch_platform(platform)) {
        return malformed_request_result(
            Launch_request_error::MALFORMED_PAYLOAD);
    }
    Request_validation validation = validate_request(request, platform);
    if (validation.status == Launch_request_status::REJECTED) {
        return finish_request_result(std::move(request), validation);
    }

    std::vector<std::uint8_t> serialized = serialize_request(request);
    if (serialized.size() >
        Terminal_launch_request_limits::maximum_payload_bytes)
    {
        return finish_request_result(
            std::move(request),
            request_rejection(
                Launch_request_error::QUOTA_EXCEEDED,
                Launch_request_field::PAYLOAD));
    }
    return finish_request_result(
        std::move(request),
        validation,
        std::move(serialized));
}

Launch_request_result decode_terminal_launch_request(
    std::span<const std::uint8_t> serialized_request,
    Launch_platform platform)
{
    if (!valid_launch_platform(platform)) {
        return malformed_request_result(
            Launch_request_error::MALFORMED_PAYLOAD);
    }
    if (serialized_request.size() >
        Terminal_launch_request_limits::maximum_payload_bytes)
    {
        return malformed_request_result(Launch_request_error::QUOTA_EXCEEDED);
    }

    Byte_reader reader(serialized_request);
    if (!reader.read_exact(k_request_magic)) {
        return malformed_request_result(
            Launch_request_error::MALFORMED_PAYLOAD);
    }
    std::uint32_t version = 0U;
    if (!reader.read_u32(version)) {
        return malformed_request_result(
            Launch_request_error::MALFORMED_PAYLOAD);
    }
    if (version != k_terminal_launch_request_schema_version) {
        return malformed_request_result(
            Launch_request_error::UNSUPPORTED_SCHEMA);
    }

    Terminal_launch_request request;
    if (!reader.read_string(
            request.launch_request_id,
            Terminal_launch_request_limits::maximum_identity_bytes) ||
        !reader.read_string(
            request.session_id,
            Terminal_launch_request_limits::maximum_identity_bytes) ||
        !reader.read_string(
            request.cancellation.identity,
            Terminal_launch_request_limits::maximum_identity_bytes))
    {
        return malformed_request_result(
            Launch_request_error::MALFORMED_PAYLOAD);
    }

    std::uint8_t cancellation_requested = 0U;
    std::uint32_t argument_count = 0U;
    if (!reader.read_u8(cancellation_requested) ||
        cancellation_requested > 1U ||
        !reader.read_u32(argument_count))
    {
        return malformed_request_result(
            Launch_request_error::MALFORMED_PAYLOAD);
    }
    if (argument_count >
        Terminal_launch_request_limits::maximum_argument_count)
    {
        return malformed_request_result(Launch_request_error::QUOTA_EXCEEDED);
    }
    request.cancellation.requested = cancellation_requested != 0U;
    request.argv.reserve(argument_count);
    for (std::uint32_t index = 0U; index < argument_count; ++index) {
        std::string argument;
        if (!reader.read_string(
                argument,
                Terminal_launch_request_limits::maximum_argument_bytes))
        {
            return malformed_request_result(
                Launch_request_error::MALFORMED_PAYLOAD);
        }
        request.argv.push_back(std::move(argument));
    }
    if (!reader.read_string(
            request.working_directory,
            Terminal_launch_request_limits::maximum_working_directory_bytes))
    {
        return malformed_request_result(
            Launch_request_error::MALFORMED_PAYLOAD);
    }

    std::uint8_t base_environment_complete = 0U;
    std::uint32_t environment_count = 0U;
    if (!reader.read_u8(base_environment_complete) ||
        base_environment_complete > 1U ||
        !reader.read_u32(environment_count))
    {
        return malformed_request_result(
            Launch_request_error::MALFORMED_PAYLOAD);
    }
    if (environment_count >
        Terminal_launch_request_limits::maximum_environment_entries)
    {
        return malformed_request_result(Launch_request_error::QUOTA_EXCEEDED);
    }
    request.base_environment_complete = base_environment_complete != 0U;
    request.base_environment.reserve(environment_count);
    for (std::uint32_t index = 0U; index < environment_count; ++index) {
        Environment_entry entry;
        if (!reader.read_string(
                entry.name,
                Terminal_launch_request_limits::maximum_environment_bytes) ||
            !reader.read_string(
                entry.value,
                Terminal_launch_request_limits::maximum_environment_bytes))
        {
            return malformed_request_result(
                Launch_request_error::MALFORMED_PAYLOAD);
        }
        request.base_environment.push_back(std::move(entry));
    }
    if (!reader.at_end()) {
        return malformed_request_result(Launch_request_error::TRAILING_DATA);
    }

    Request_validation validation = validate_request(request, platform);
    return finish_request_result(std::move(request), validation);
}

Working_directory_advisory probe_terminal_working_directory(
    std::string_view working_directory,
    const Working_directory_probe& working_directory_probe)
{
    if (!working_directory_probe) {
        return Working_directory_advisory::NOT_PROBED;
    }
    return working_directory_probe(working_directory);
}

} // namespace vnm::terminal_workspace
