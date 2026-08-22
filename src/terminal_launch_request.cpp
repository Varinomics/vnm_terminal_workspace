#include "vnm_terminal_workspace/terminal_launch_request.h"
#include "terminal_launch_request_test_support.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace vnm::terminal_workspace {
namespace testing {
namespace {

thread_local Capability_secret_wipe_observer s_capability_secret_wipe_observer =
    nullptr;

} // namespace

void set_capability_secret_wipe_observer(
    Capability_secret_wipe_observer observer)
{
    s_capability_secret_wipe_observer = observer;
}

void observe_capability_secret_wipe(
    std::span<const std::uint8_t> wiped_bytes)
{
    if (s_capability_secret_wipe_observer) {
        s_capability_secret_wipe_observer(wiped_bytes);
    }
}

} // namespace testing

namespace {

using environment_policy::Environment_entry;
using environment_policy::Environment_issue_code;
using environment_policy::Environment_platform;
using environment_policy::Reserved_environment_class;

constexpr std::array<std::uint8_t, 8> k_request_magic{
    'V', 'N', 'M', 'T', 'R', 'Q', '0', '1',
};
constexpr std::array<std::uint8_t, 8> k_contribution_magic{
    'V', 'N', 'M', 'T', 'C', 'P', '0', '1',
};

struct Request_validation
{
    Launch_request_status status = Launch_request_status::REJECTED;
    Launch_request_error error = Launch_request_error::NONE;
    Launch_request_field field = Launch_request_field::NONE;
    std::size_t index = 0U;
    Working_directory_advisory working_directory_advisory =
        Working_directory_advisory::NOT_PROBED;
};

struct Contribution_validation
{
    Capability_contribution_error error =
        Capability_contribution_error::NONE;
    Capability_contribution_field field =
        Capability_contribution_field::NONE;
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

std::vector<std::string_view> string_views(
    const std::vector<std::string>& values)
{
    std::vector<std::string_view> views;
    views.reserve(values.size());
    for (const std::string& value : values) {
        views.push_back(value);
    }
    return views;
}

bool names_equal(
    std::string_view left,
    std::string_view right,
    Launch_platform platform)
{
    return environment_policy::environment_names_equal(
        left,
        right,
        environment_platform(platform));
}

bool contains_equivalent_name(
    std::span<const std::string> names,
    std::string_view candidate,
    Launch_platform platform)
{
    return std::any_of(
        names.begin(),
        names.end(),
        [candidate, platform](const std::string& name) {
            return names_equal(name, candidate, platform);
        });
}

bool unsigned_utf8_less(std::string_view left, std::string_view right)
{
    return std::lexicographical_compare(
        left.begin(),
        left.end(),
        right.begin(),
        right.end(),
        [](char left_character, char right_character) {
            return static_cast<unsigned char>(left_character) <
                static_cast<unsigned char>(right_character);
        });
}

void canonicalize_allowed_names(std::vector<std::string>& names)
{
    std::sort(names.begin(), names.end(), unsigned_utf8_less);
}

bool allowed_names_are_canonical(std::span<const std::string> names)
{
    return std::is_sorted(names.begin(), names.end(), unsigned_utf8_less);
}

void secure_clear_capability_secret(std::string& value)
{
    if (value.empty()) {
        return;
    }

    volatile auto* bytes = reinterpret_cast<volatile std::uint8_t*>(
        value.data());
    for (std::size_t index = 0U; index < value.size(); ++index) {
        bytes[index] = 0U;
    }
    testing::observe_capability_secret_wipe({
        reinterpret_cast<const std::uint8_t*>(value.data()),
        value.size(),
    });
    value.clear();
}

void secure_clear_capability_secrets(
    Terminal_capability_contribution& contribution)
{
    for (Environment_entry& entry : contribution.environment) {
        secure_clear_capability_secret(entry.value);
    }
}

class Scoped_capability_secret_wipe
{
public:
    explicit Scoped_capability_secret_wipe(
        Terminal_capability_contribution& contribution)
    :
        m_contribution(&contribution)
    {}

    ~Scoped_capability_secret_wipe()
    {
        if (m_contribution) {
            secure_clear_capability_secrets(*m_contribution);
        }
    }

    Scoped_capability_secret_wipe(const Scoped_capability_secret_wipe&) =
        delete;
    Scoped_capability_secret_wipe& operator=(
        const Scoped_capability_secret_wipe&) = delete;

    void release()
    {
        m_contribution = nullptr;
    }

private:
    Terminal_capability_contribution* m_contribution;
};

bool is_terminal_owned_environment_name(
    std::string_view name,
    Launch_platform platform)
{
    constexpr std::array<std::string_view, 3> k_names{
        "TERM",
        "COLORTERM",
        "NO_COLOR",
    };
    return std::any_of(
        k_names.begin(),
        k_names.end(),
        [name, platform](std::string_view candidate) {
            return names_equal(name, candidate, platform);
        });
}

bool is_lookup_sensitive_environment_name(
    std::string_view name,
    Launch_platform platform)
{
    if (names_equal(name, "PATH", platform)) {
        return true;
    }
    if (platform != Launch_platform::WINDOWS) {
        return false;
    }
    constexpr std::array<std::string_view, 3> k_windows_names{
        "PATHEXT",
        "SystemRoot",
        "WINDIR",
    };
    return std::any_of(
        k_windows_names.begin(),
        k_windows_names.end(),
        [name, platform](std::string_view candidate) {
            return names_equal(name, candidate, platform);
        });
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
    Launch_platform platform,
    std::span<const std::string_view> additional_reserved_names,
    const Working_directory_probe& working_directory_probe)
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
            environment_platform(platform),
            additional_reserved_names);
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
    if (working_directory_probe) {
        result.working_directory_advisory =
            working_directory_probe(request.working_directory);
    }
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

    void append_u64(std::uint64_t value)
    {
        for (int shift = 0; shift < 64; shift += 8) {
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

    bool read_u64(std::uint64_t& value)
    {
        if (remaining() < 8U) {
            return false;
        }
        value = 0U;
        for (int shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(m_bytes[m_offset++]) << shift;
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
    result.working_directory_advisory =
        validation.working_directory_advisory;
    if (validation.status != Launch_request_status::REJECTED) {
        result.request = std::move(request);
        result.serialized_request = std::move(serialized_request);
    }
    return result;
}

Contribution_validation contribution_rejection(
    Capability_contribution_error error,
    Capability_contribution_field field,
    std::size_t index = 0U)
{
    return {error, field, index};
}

Contribution_validation validate_contribution_identity(
    std::string_view identity,
    Capability_contribution_field field)
{
    if (identity.empty()) {
        return contribution_rejection(
            Capability_contribution_error::EMPTY_IDENTITY,
            field);
    }
    if (contains_nul(identity)) {
        return contribution_rejection(
            Capability_contribution_error::IDENTITY_CONTAINS_NUL,
            field);
    }
    if (identity.size() >
        Terminal_capability_contribution_limits::maximum_identity_bytes)
    {
        return contribution_rejection(
            Capability_contribution_error::QUOTA_EXCEEDED,
            field);
    }
    return {};
}

Contribution_validation validate_contribution_name(
    std::string_view name,
    Launch_platform platform,
    std::span<const std::string_view> additional_reserved_names,
    Capability_contribution_field field,
    std::size_t index)
{
    if (is_terminal_owned_environment_name(name, platform)) {
        return contribution_rejection(
            Capability_contribution_error::TERMINAL_OWNED_ENVIRONMENT_NAME,
            field,
            index);
    }
    if (is_lookup_sensitive_environment_name(name, platform)) {
        return contribution_rejection(
            Capability_contribution_error::LOOKUP_SENSITIVE_ENVIRONMENT_NAME,
            field,
            index);
    }

    const environment_policy::Environment_name_validation validation =
        environment_policy::validate_environment_edit_name(
            name,
            environment_platform(platform),
            additional_reserved_names);
    if (validation.accepted) {
        return {};
    }
    if (validation.code == Environment_issue_code::RESERVED_EDIT &&
        validation.reserved_class ==
            Reserved_environment_class::PRODUCT_RESERVED_INPUT)
    {
        return {};
    }
    if (validation.code == Environment_issue_code::RESERVED_EDIT) {
        return contribution_rejection(
            Capability_contribution_error::RESERVED_ENVIRONMENT_NAME,
            field,
            index);
    }
    return contribution_rejection(
        Capability_contribution_error::INVALID_ENVIRONMENT_NAME,
        field,
        index);
}

Contribution_validation validate_allowed_names(
    std::span<const std::string> names,
    Launch_platform platform,
    std::span<const std::string_view> additional_reserved_names)
{
    if (names.size() >
        Terminal_capability_contribution_limits::maximum_allowed_names)
    {
        return contribution_rejection(
            Capability_contribution_error::QUOTA_EXCEEDED,
            Capability_contribution_field::ALLOWED_ENVIRONMENT_NAMES);
    }
    for (std::size_t index = 0U; index < names.size(); ++index) {
        const std::string& name = names[index];
        if (name.size() >
            Terminal_capability_contribution_limits::maximum_identity_bytes)
        {
            return contribution_rejection(
                Capability_contribution_error::QUOTA_EXCEEDED,
                Capability_contribution_field::ALLOWED_ENVIRONMENT_NAMES,
                index);
        }
        Contribution_validation valid_name = validate_contribution_name(
            name,
            platform,
            additional_reserved_names,
            Capability_contribution_field::ALLOWED_ENVIRONMENT_NAMES,
            index);
        if (valid_name.error != Capability_contribution_error::NONE) {
            valid_name.error =
                valid_name.error ==
                    Capability_contribution_error::INVALID_ENVIRONMENT_NAME
                ? Capability_contribution_error::INVALID_ALLOWED_NAME
                : valid_name.error;
            return valid_name;
        }
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (names_equal(name, names[previous], platform)) {
                return contribution_rejection(
                    Capability_contribution_error::DUPLICATE_ALLOWED_NAME,
                    Capability_contribution_field::ALLOWED_ENVIRONMENT_NAMES,
                    index);
            }
        }
    }
    return {};
}

bool same_name_set(
    std::span<const std::string> left,
    std::span<const std::string> right,
    Launch_platform platform)
{
    if (left.size() != right.size()) {
        return false;
    }
    return std::all_of(
        left.begin(),
        left.end(),
        [right, platform](const std::string& name) {
            return contains_equivalent_name(right, name, platform);
        });
}

Contribution_validation validate_contribution(
    const Terminal_capability_contribution& contribution,
    Launch_platform platform,
    const Terminal_capability_binding_policy& policy)
{
    if (contribution.version !=
        k_terminal_capability_contribution_version)
    {
        return contribution_rejection(
            Capability_contribution_error::UNSUPPORTED_VERSION,
            Capability_contribution_field::PAYLOAD);
    }

    const std::array<std::pair<std::string_view, Capability_contribution_field>, 6>
        identities{{
            {contribution.capability_id,
             Capability_contribution_field::CAPABILITY_ID},
            {contribution.issuer,
             Capability_contribution_field::ISSUER},
            {contribution.intended_consumer,
             Capability_contribution_field::INTENDED_CONSUMER},
            {contribution.product_scope,
             Capability_contribution_field::PRODUCT_SCOPE},
            {contribution.session_scope,
             Capability_contribution_field::SESSION_SCOPE},
            {contribution.lifetime_id,
             Capability_contribution_field::LIFETIME_ID},
        }};
    for (const auto& [identity, field] : identities) {
        Contribution_validation validation =
            validate_contribution_identity(identity, field);
        if (validation.error != Capability_contribution_error::NONE) {
            return validation;
        }
    }

    if (contribution.capability_id != policy.capability_id ||
        contribution.issuer != policy.expected_issuer ||
        contribution.intended_consumer != policy.intended_consumer ||
        contribution.product_scope != policy.product_scope ||
        contribution.session_scope != policy.session_scope ||
        contribution.lifetime_id != policy.lifetime_id)
    {
        return contribution_rejection(
            Capability_contribution_error::BINDING_MISMATCH,
            Capability_contribution_field::PAYLOAD);
    }
    if (contribution.issued_at_unix_ms > policy.evaluation_time_unix_ms ||
        contribution.expires_at_unix_ms <= policy.evaluation_time_unix_ms ||
        contribution.expires_at_unix_ms <= contribution.issued_at_unix_ms)
    {
        return contribution_rejection(
            Capability_contribution_error::INVALID_LIFETIME,
            Capability_contribution_field::LIFETIME_ID);
    }

    const std::vector<std::string_view> additional_reserved_names =
        string_views(policy.additional_reserved_input_names);
    Contribution_validation policy_names = validate_allowed_names(
        policy.allowed_environment_names,
        platform,
        additional_reserved_names);
    if (policy_names.error != Capability_contribution_error::NONE) {
        return policy_names;
    }
    Contribution_validation contribution_names = validate_allowed_names(
        contribution.allowed_environment_names,
        platform,
        additional_reserved_names);
    if (contribution_names.error != Capability_contribution_error::NONE) {
        return contribution_names;
    }
    if (!same_name_set(
            contribution.allowed_environment_names,
            policy.allowed_environment_names,
            platform))
    {
        return contribution_rejection(
            Capability_contribution_error::BINDING_MISMATCH,
            Capability_contribution_field::ALLOWED_ENVIRONMENT_NAMES);
    }

    if (contribution.environment.size() >
        Terminal_capability_contribution_limits::maximum_environment_entries)
    {
        return contribution_rejection(
            Capability_contribution_error::QUOTA_EXCEEDED,
            Capability_contribution_field::ENVIRONMENT);
    }

    std::size_t environment_bytes = 0U;
    for (std::size_t index = 0U;
         index < contribution.environment.size();
         ++index)
    {
        const Environment_entry& entry = contribution.environment[index];
        if (would_exceed(
                environment_bytes,
                entry.name.size(),
                Terminal_capability_contribution_limits::
                    maximum_environment_bytes) ||
            would_exceed(
                environment_bytes + entry.name.size(),
                entry.value.size(),
                Terminal_capability_contribution_limits::
                    maximum_environment_bytes))
        {
            return contribution_rejection(
                Capability_contribution_error::QUOTA_EXCEEDED,
                Capability_contribution_field::ENVIRONMENT,
                index);
        }
        environment_bytes += entry.name.size() + entry.value.size();

        Contribution_validation name_validation =
            validate_contribution_name(
                entry.name,
                platform,
                additional_reserved_names,
                Capability_contribution_field::ENVIRONMENT,
                index);
        if (name_validation.error != Capability_contribution_error::NONE) {
            return name_validation;
        }
        if (!contains_equivalent_name(
                contribution.allowed_environment_names,
                entry.name,
                platform))
        {
            return contribution_rejection(
                Capability_contribution_error::ENVIRONMENT_NAME_NOT_ALLOWED,
                Capability_contribution_field::ENVIRONMENT,
                index);
        }
        if (contains_nul(entry.value)) {
            return contribution_rejection(
                Capability_contribution_error::ENVIRONMENT_VALUE_CONTAINS_NUL,
                Capability_contribution_field::ENVIRONMENT,
                index);
        }
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (names_equal(
                    entry.name,
                    contribution.environment[previous].name,
                    platform))
            {
                return contribution_rejection(
                    Capability_contribution_error::DUPLICATE_ENVIRONMENT_NAME,
                    Capability_contribution_field::ENVIRONMENT,
                    index);
            }
        }
    }
    return {};
}

std::vector<std::uint8_t> serialize_contribution(
    const Terminal_capability_contribution_envelope& envelope)
{
    Byte_writer writer;
    writer.append(k_contribution_magic);
    writer.append_u32(k_terminal_capability_contribution_version);
    writer.append_u8(envelope.present ? 1U : 0U);
    if (!envelope.present) {
        return writer.take();
    }

    const Terminal_capability_contribution& contribution =
        envelope.contribution;
    writer.append_string(contribution.capability_id);
    writer.append_string(contribution.issuer);
    writer.append_string(contribution.intended_consumer);
    writer.append_string(contribution.product_scope);
    writer.append_string(contribution.session_scope);
    writer.append_string(contribution.lifetime_id);
    writer.append_u64(contribution.issued_at_unix_ms);
    writer.append_u64(contribution.expires_at_unix_ms);
    writer.append_u32(static_cast<std::uint32_t>(
        contribution.allowed_environment_names.size()));
    for (const std::string& name : contribution.allowed_environment_names) {
        writer.append_string(name);
    }
    writer.append_u32(
        static_cast<std::uint32_t>(contribution.environment.size()));
    for (const Environment_entry& entry : contribution.environment) {
        writer.append_string(entry.name);
        writer.append_string(entry.value);
    }
    return writer.take();
}

Capability_contribution_result contribution_failure(
    Capability_contribution_status status,
    Capability_contribution_error error,
    Capability_contribution_field field,
    std::size_t index = 0U)
{
    Capability_contribution_result result;
    result.status = status;
    result.error = error;
    result.field = field;
    result.index = index;
    return result;
}

Capability_contribution_result decoded_contribution_failure(
    const Terminal_capability_binding_policy& policy,
    Capability_contribution_error error,
    Capability_contribution_field field,
    std::size_t index = 0U)
{
    return contribution_failure(
        policy.required
            ? Capability_contribution_status::REJECTED
            : Capability_contribution_status::DEGRADED,
        error,
        field,
        index);
}

} // namespace

Launch_request_result prepare_terminal_launch_request(
    Terminal_launch_request request,
    Launch_platform platform,
    std::span<const std::string_view> additional_reserved_names,
    Working_directory_probe working_directory_probe)
{
    Request_validation validation = validate_request(
        request,
        platform,
        additional_reserved_names,
        working_directory_probe);
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
    Launch_platform platform,
    std::span<const std::string_view> additional_reserved_names,
    Working_directory_probe working_directory_probe)
{
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

    Request_validation validation = validate_request(
        request,
        platform,
        additional_reserved_names,
        working_directory_probe);
    return finish_request_result(std::move(request), validation);
}

Capability_contribution_result serialize_terminal_capability_contribution(
    Terminal_capability_contribution_envelope envelope,
    Launch_platform platform,
    const Terminal_capability_binding_policy& policy,
    const Pre_custody_cancellation& cancellation)
{
    if (cancellation.requested) {
        secure_clear_capability_secrets(envelope.contribution);
        return contribution_failure(
            Capability_contribution_status::CANCELLED,
            Capability_contribution_error::CANCELLED,
            Capability_contribution_field::NONE);
    }
    if (!envelope.present) {
        secure_clear_capability_secrets(envelope.contribution);
        if (policy.required) {
            return contribution_failure(
                Capability_contribution_status::REJECTED,
                Capability_contribution_error::MISSING_REQUIRED_CONTRIBUTION,
                Capability_contribution_field::PAYLOAD);
        }
        Capability_contribution_result result;
        result.status = Capability_contribution_status::ABSENT;
        result.serialized_contribution = serialize_contribution(envelope);
        return result;
    }

    Contribution_validation validation = validate_contribution(
        envelope.contribution,
        platform,
        policy);
    if (validation.error != Capability_contribution_error::NONE) {
        secure_clear_capability_secrets(envelope.contribution);
        return contribution_failure(
            policy.required
                ? Capability_contribution_status::REJECTED
                : Capability_contribution_status::DEGRADED,
            validation.error,
            validation.field,
            validation.index);
    }

    canonicalize_allowed_names(
        envelope.contribution.allowed_environment_names);
    Capability_contribution_result result;
    result.status = Capability_contribution_status::AVAILABLE;
    result.serialized_contribution = serialize_contribution(envelope);
    result.contribution = std::move(envelope.contribution);
    return result;
}

Capability_contribution_result decode_terminal_capability_contribution(
    std::span<const std::uint8_t> serialized_contribution,
    Launch_platform platform,
    const Terminal_capability_binding_policy& policy,
    const Pre_custody_cancellation& cancellation)
{
    if (cancellation.requested) {
        return contribution_failure(
            Capability_contribution_status::CANCELLED,
            Capability_contribution_error::CANCELLED,
            Capability_contribution_field::NONE);
    }
    if (serialized_contribution.size() >
        Terminal_capability_contribution_limits::maximum_payload_bytes)
    {
        return decoded_contribution_failure(
            policy,
            Capability_contribution_error::QUOTA_EXCEEDED,
            Capability_contribution_field::PAYLOAD);
    }

    Byte_reader reader(serialized_contribution);
    if (!reader.read_exact(k_contribution_magic)) {
        return decoded_contribution_failure(
            policy,
            Capability_contribution_error::MALFORMED_PAYLOAD,
            Capability_contribution_field::PAYLOAD);
    }
    std::uint32_t version = 0U;
    if (!reader.read_u32(version)) {
        return decoded_contribution_failure(
            policy,
            Capability_contribution_error::MALFORMED_PAYLOAD,
            Capability_contribution_field::PAYLOAD);
    }
    if (version != k_terminal_capability_contribution_version) {
        return decoded_contribution_failure(
            policy,
            Capability_contribution_error::UNSUPPORTED_VERSION,
            Capability_contribution_field::PAYLOAD);
    }

    std::uint8_t present = 0U;
    if (!reader.read_u8(present) || present > 1U) {
        return decoded_contribution_failure(
            policy,
            Capability_contribution_error::MALFORMED_PAYLOAD,
            Capability_contribution_field::PAYLOAD);
    }
    if (present == 0U) {
        if (!reader.at_end()) {
            return decoded_contribution_failure(
                policy,
                Capability_contribution_error::TRAILING_DATA,
                Capability_contribution_field::PAYLOAD);
        }
        if (policy.required) {
            return contribution_failure(
                Capability_contribution_status::REJECTED,
                Capability_contribution_error::MISSING_REQUIRED_CONTRIBUTION,
                Capability_contribution_field::PAYLOAD);
        }
        Capability_contribution_result result;
        result.status = Capability_contribution_status::ABSENT;
        return result;
    }

    Terminal_capability_contribution contribution;
    Scoped_capability_secret_wipe secret_wipe(contribution);
    contribution.version = version;
    const std::size_t identity_limit =
        Terminal_capability_contribution_limits::maximum_identity_bytes;
    if (!reader.read_string(contribution.capability_id, identity_limit) ||
        !reader.read_string(contribution.issuer, identity_limit) ||
        !reader.read_string(contribution.intended_consumer, identity_limit) ||
        !reader.read_string(contribution.product_scope, identity_limit) ||
        !reader.read_string(contribution.session_scope, identity_limit) ||
        !reader.read_string(contribution.lifetime_id, identity_limit) ||
        !reader.read_u64(contribution.issued_at_unix_ms) ||
        !reader.read_u64(contribution.expires_at_unix_ms))
    {
        return decoded_contribution_failure(
            policy,
            Capability_contribution_error::MALFORMED_PAYLOAD,
            Capability_contribution_field::PAYLOAD);
    }

    std::uint32_t allowed_name_count = 0U;
    if (!reader.read_u32(allowed_name_count)) {
        return decoded_contribution_failure(
            policy,
            Capability_contribution_error::MALFORMED_PAYLOAD,
            Capability_contribution_field::PAYLOAD);
    }
    if (allowed_name_count >
        Terminal_capability_contribution_limits::maximum_allowed_names)
    {
        return decoded_contribution_failure(
            policy,
            Capability_contribution_error::QUOTA_EXCEEDED,
            Capability_contribution_field::ALLOWED_ENVIRONMENT_NAMES);
    }
    contribution.allowed_environment_names.reserve(allowed_name_count);
    for (std::uint32_t index = 0U; index < allowed_name_count; ++index) {
        std::string name;
        if (!reader.read_string(name, identity_limit)) {
            return decoded_contribution_failure(
                policy,
                Capability_contribution_error::MALFORMED_PAYLOAD,
                Capability_contribution_field::ALLOWED_ENVIRONMENT_NAMES,
                index);
        }
        contribution.allowed_environment_names.push_back(std::move(name));
    }

    std::uint32_t environment_count = 0U;
    if (!reader.read_u32(environment_count)) {
        return decoded_contribution_failure(
            policy,
            Capability_contribution_error::MALFORMED_PAYLOAD,
            Capability_contribution_field::PAYLOAD);
    }
    if (environment_count >
        Terminal_capability_contribution_limits::maximum_environment_entries)
    {
        return decoded_contribution_failure(
            policy,
            Capability_contribution_error::QUOTA_EXCEEDED,
            Capability_contribution_field::ENVIRONMENT);
    }
    contribution.environment.reserve(environment_count);
    for (std::uint32_t index = 0U; index < environment_count; ++index) {
        Environment_entry entry;
        const std::size_t environment_limit =
            Terminal_capability_contribution_limits::maximum_environment_bytes;
        if (!reader.read_string(entry.name, environment_limit) ||
            !reader.read_string(entry.value, environment_limit))
        {
            return decoded_contribution_failure(
                policy,
                Capability_contribution_error::MALFORMED_PAYLOAD,
                Capability_contribution_field::ENVIRONMENT,
                index);
        }
        contribution.environment.push_back(std::move(entry));
    }
    if (!reader.at_end()) {
        return decoded_contribution_failure(
            policy,
            Capability_contribution_error::TRAILING_DATA,
            Capability_contribution_field::PAYLOAD);
    }

    Contribution_validation validation = validate_contribution(
        contribution,
        platform,
        policy);
    if (validation.error != Capability_contribution_error::NONE) {
        return decoded_contribution_failure(
            policy,
            validation.error,
            validation.field,
            validation.index);
    }
    if (!allowed_names_are_canonical(
            contribution.allowed_environment_names))
    {
        return decoded_contribution_failure(
            policy,
            Capability_contribution_error::
                NONCANONICAL_ALLOWED_NAME_ORDER,
            Capability_contribution_field::ALLOWED_ENVIRONMENT_NAMES);
    }

    Capability_contribution_result result;
    result.status = Capability_contribution_status::AVAILABLE;
    result.contribution = std::move(contribution);
    secret_wipe.release();
    return result;
}

} // namespace vnm::terminal_workspace
