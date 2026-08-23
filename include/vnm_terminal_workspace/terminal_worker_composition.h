#pragma once

#include "vnm_terminal_workspace/terminal_owner_client.h"
#include "vnm_terminal_workspace/terminal_worker_envelope.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vnm::terminal_workspace {

struct Terminal_worker_launch_configuration
{
    Terminal_worker_surface_configuration surface;
    std::optional<Terminal_worker_output_capture_configuration> output_capture;
};

struct Terminal_worker_neutral_configuration
{};

struct Neutral_terminal_worker_package_policy
{
    using Configuration = Terminal_worker_neutral_configuration;

    inline static constexpr std::string_view package_id =
        "vnm_terminal_workspace.terminal_worker";
    inline static constexpr std::string_view family_id =
        "vnm_terminal_workspace";
    inline static constexpr std::string_view configuration_schema = {};
    inline static constexpr std::array capabilities{
        Terminal_worker_package_capability::REMOTE_UI,
    };
    inline static constexpr std::array<std::string_view, 0>
        product_environment_names{};

    static std::optional<std::string> encode_configuration(
        const Configuration&) noexcept;
    static std::optional<Configuration> decode_configuration(
        std::string_view serialized) noexcept;
    static void clear_configuration(Configuration&) noexcept;
};

template<typename Policy>
concept Fixed_terminal_worker_package_policy = requires(
    const typename Policy::Configuration& configuration,
    typename Policy::Configuration& mutable_configuration,
    std::string_view serialized)
{
    { Policy::package_id } -> std::convertible_to<std::string_view>;
    { Policy::family_id } -> std::convertible_to<std::string_view>;
    { Policy::configuration_schema } -> std::convertible_to<std::string_view>;
    { std::span<const Terminal_worker_package_capability>(
        Policy::capabilities) };
    { std::span<const std::string_view>(Policy::product_environment_names) };
    { Policy::encode_configuration(configuration) } noexcept ->
        std::same_as<std::optional<std::string>>;
    { Policy::decode_configuration(serialized) } noexcept ->
        std::same_as<std::optional<typename Policy::Configuration>>;
    { Policy::clear_configuration(mutable_configuration) } noexcept;
};

enum class Terminal_worker_fixed_parameters_error
{
    NONE,
    INVALID_POLICY,
    INVALID_COMMON_ENVELOPE,
    INVALID_PRODUCT_CONFIGURATION,
    MALFORMED_PARAMETERS,
};

template<Fixed_terminal_worker_package_policy Policy>
struct Terminal_worker_fixed_parameters
{
    Terminal_worker_envelope envelope;
    typename Policy::Configuration configuration;
};

template<Fixed_terminal_worker_package_policy Policy>
struct Terminal_worker_fixed_parameters_result
{
    Terminal_worker_fixed_parameters_error error =
        Terminal_worker_fixed_parameters_error::MALFORMED_PARAMETERS;
    std::optional<Terminal_worker_fixed_parameters<Policy>> parameters;
    std::string serialized_parameters;
};

namespace detail {

inline constexpr std::string_view k_common_parameter_prefix =
    "{\"vnm_terminal_workspace_envelope@1\":\"";

inline void clear_sensitive_string(std::string& value) noexcept
{
    volatile char* bytes = value.data();
    for (std::size_t index = 0U; index < value.size(); ++index) {
        bytes[index] = '\0';
    }
    value.clear();
}

inline void clear_sensitive_bytes(
    std::vector<std::uint8_t>& value) noexcept
{
    volatile std::uint8_t* bytes = value.data();
    for (std::size_t index = 0U; index < value.size(); ++index) {
        bytes[index] = 0U;
    }
    value.clear();
}

inline void clear_sensitive_environment(
    std::optional<std::vector<environment_policy::Environment_entry>>&
        environment) noexcept
{
    if (!environment) {
        return;
    }
    for (environment_policy::Environment_entry& entry : *environment) {
        clear_sensitive_string(entry.name);
        clear_sensitive_string(entry.value);
    }
    environment.reset();
}

inline void clear_sensitive_launch_request(
    Launch_request_result& result) noexcept
{
    clear_sensitive_bytes(result.serialized_request);
    if (!result.request) {
        return;
    }
    Terminal_launch_request& request = *result.request;
    clear_sensitive_string(request.launch_request_id);
    clear_sensitive_string(request.session_id);
    for (std::string& argument : request.argv) {
        clear_sensitive_string(argument);
    }
    clear_sensitive_string(request.working_directory);
    for (environment_policy::Environment_entry& entry :
         request.base_environment)
    {
        clear_sensitive_string(entry.name);
        clear_sensitive_string(entry.value);
    }
    clear_sensitive_string(request.cancellation.identity);
    result.request.reset();
}

inline void clear_sensitive_envelope(
    Terminal_worker_envelope& envelope) noexcept
{
    clear_sensitive_bytes(envelope.serialized_request);
    clear_sensitive_environment(envelope.authorized_environment);
}

class Sensitive_launch_request_guard
{
public:
    explicit Sensitive_launch_request_guard(Launch_request_result& result)
    :
        m_result(result)
    {}

    ~Sensitive_launch_request_guard()
    {
        clear_sensitive_launch_request(m_result);
    }

    Sensitive_launch_request_guard(const Sensitive_launch_request_guard&) =
        delete;
    Sensitive_launch_request_guard& operator=(
        const Sensitive_launch_request_guard&) = delete;

private:
    Launch_request_result& m_result;
};

class Sensitive_envelope_guard
{
public:
    explicit Sensitive_envelope_guard(
        std::optional<Terminal_worker_envelope>& envelope)
    :
        m_envelope(&envelope)
    {}

    ~Sensitive_envelope_guard()
    {
        if (m_envelope != nullptr && *m_envelope) {
            clear_sensitive_envelope(**m_envelope);
            m_envelope->reset();
        }
    }

    Sensitive_envelope_guard(const Sensitive_envelope_guard&) = delete;
    Sensitive_envelope_guard& operator=(const Sensitive_envelope_guard&) =
        delete;

    void release() noexcept
    {
        m_envelope = nullptr;
    }

private:
    std::optional<Terminal_worker_envelope>* m_envelope;
};

inline bool valid_schema_name(std::string_view value)
{
    return !value.empty() && value.size() <= 128U &&
        std::all_of(value.begin(), value.end(), [](char character) {
            return (character >= 'A' && character <= 'Z') ||
                (character >= 'a' && character <= 'z') ||
                (character >= '0' && character <= '9') ||
                character == '_' || character == '-' || character == '.' ||
                character == '@';
        });
}

inline bool valid_fixed_policy_identity(
    std::string_view package_id,
    std::string_view family_id,
    std::span<const Terminal_worker_package_capability> capabilities,
    std::string_view configuration_schema)
{
    if (package_id.empty() || family_id.empty() ||
        package_id.size() > 256U || family_id.size() > 256U ||
        std::find(
            capabilities.begin(),
            capabilities.end(),
            Terminal_worker_package_capability::REMOTE_UI) ==
                capabilities.end())
    {
        return false;
    }
    const bool has_product_configuration = !configuration_schema.empty();
    const bool has_whole_message = std::find(
        capabilities.begin(),
        capabilities.end(),
        Terminal_worker_package_capability::WHOLE_MESSAGE_INPUT) !=
            capabilities.end();
    return (!has_product_configuration ||
            valid_schema_name(configuration_schema)) &&
        (!has_whole_message || has_product_configuration);
}

inline bool valid_fixed_environment_names(
    std::span<const std::string_view> names)
{
    for (std::size_t index = 0U; index < names.size(); ++index) {
        if (names[index].empty() || names[index].size() > 32767U) {
            return false;
        }
        for (const environment_policy::Environment_platform platform : {
                 environment_policy::Environment_platform::WINDOWS,
                 environment_policy::Environment_platform::POSIX})
        {
            if (environment_policy::classify_environment_name(
                    names[index],
                    platform) !=
                environment_policy::Reserved_environment_class::NONE)
            {
                return false;
            }
            for (std::size_t prior = 0U; prior < index; ++prior) {
                if (environment_policy::environment_names_equal(
                        names[index], names[prior], platform))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

template<Fixed_terminal_worker_package_policy Policy>
inline bool base_contains_fixed_environment_name(
    const Terminal_launch_request& request,
    Launch_platform platform)
{
    const environment_policy::Environment_platform environment_platform =
        platform == Launch_platform::WINDOWS
            ? environment_policy::Environment_platform::WINDOWS
            : environment_policy::Environment_platform::POSIX;
    const auto fixed_names =
        std::span<const std::string_view>(Policy::product_environment_names);
    return std::any_of(
        request.base_environment.begin(),
        request.base_environment.end(),
        [environment_platform, fixed_names](const auto& entry) {
            return std::any_of(
                fixed_names.begin(),
                fixed_names.end(),
                [environment_platform, &entry](std::string_view fixed_name) {
                    return environment_policy::environment_names_equal(
                        entry.name,
                        fixed_name,
                        environment_platform);
                });
        });
}

class Terminal_worker_fixed_package_binding
{
public:
    Terminal_worker_fixed_package_binding(
        Terminal_worker_fixed_package_binding&&) = default;
    Terminal_worker_fixed_package_binding& operator=(
        Terminal_worker_fixed_package_binding&&) = default;

private:
    Terminal_worker_fixed_package_binding() = default;

    std::string package_id;
    std::string family_id;
    std::vector<Terminal_worker_package_capability> capabilities;
    std::vector<std::string> product_environment_names;
    std::function<std::optional<std::string>(
        const Terminal_worker_envelope& envelope,
        std::string_view canonical_configuration)>
            encode_parameters;

    friend class ::vnm::terminal_workspace::Terminal_owner_host;
    friend struct Terminal_owner_service_access;

    template<Fixed_terminal_worker_package_policy Policy>
    friend Terminal_worker_fixed_package_binding
        terminal_worker_fixed_package_binding();
};

int run_terminal_owner_service_program(
    int argc,
    char** argv,
    Terminal_worker_fixed_package_binding binding);

} // namespace detail

template<Fixed_terminal_worker_package_policy Policy>
Terminal_worker_fixed_parameters_result<Policy>
encode_terminal_worker_fixed_parameters(
    const Terminal_worker_envelope& envelope,
    const typename Policy::Configuration& configuration)
{
    Terminal_worker_fixed_parameters_result<Policy> result;
    const auto capabilities =
        std::span<const Terminal_worker_package_capability>(
            Policy::capabilities);
    const std::string_view schema = Policy::configuration_schema;
    const auto environment_names =
        std::span<const std::string_view>(Policy::product_environment_names);
    if (!detail::valid_fixed_policy_identity(
            Policy::package_id,
            Policy::family_id,
            capabilities,
            schema) ||
        !detail::valid_fixed_environment_names(environment_names))
    {
        result.error = Terminal_worker_fixed_parameters_error::INVALID_POLICY;
        return result;
    }
    Launch_request_result validated_request =
        decode_terminal_launch_request(
            envelope.serialized_request,
            envelope.platform);
    detail::Sensitive_launch_request_guard request_guard(validated_request);
    if (validated_request.status == Launch_request_status::REJECTED ||
        (validated_request.request &&
         detail::base_contains_fixed_environment_name<Policy>(
             *validated_request.request,
             envelope.platform)))
    {
        result.error =
            Terminal_worker_fixed_parameters_error::INVALID_COMMON_ENVELOPE;
        return result;
    }
    Terminal_worker_envelope_result common =
        encode_terminal_worker_envelope(envelope);
    if (common.error != Terminal_worker_envelope_error::NONE) {
        result.error =
            Terminal_worker_fixed_parameters_error::INVALID_COMMON_ENVELOPE;
        return result;
    }
    std::optional<std::string> product =
        Policy::encode_configuration(configuration);
    if (!product || (!schema.empty() &&
        (product->size() < 2U || product->front() != '{' ||
         product->back() != '}')))
    {
        if (product) {
            detail::clear_sensitive_string(*product);
        }
        detail::clear_sensitive_string(common.serialized_envelope);
        result.error = Terminal_worker_fixed_parameters_error::
            INVALID_PRODUCT_CONFIGURATION;
        return result;
    }
    if (schema.empty()) {
        if (!product->empty()) {
            detail::clear_sensitive_string(*product);
            detail::clear_sensitive_string(common.serialized_envelope);
            result.error = Terminal_worker_fixed_parameters_error::
                INVALID_PRODUCT_CONFIGURATION;
            return result;
        }
        result.serialized_parameters =
            std::move(common.serialized_envelope);
    }
    else {
        result.serialized_parameters.reserve(
            common.serialized_envelope.size() + schema.size() +
            product->size() + 5U);
        result.serialized_parameters.assign(
            common.serialized_envelope.data(),
            common.serialized_envelope.size() - 1U);
        result.serialized_parameters.append(",\"");
        result.serialized_parameters.append(schema);
        result.serialized_parameters.append("\":");
        result.serialized_parameters.append(*product);
        result.serialized_parameters.push_back('}');
        detail::clear_sensitive_string(common.serialized_envelope);
    }
    detail::clear_sensitive_string(*product);
    result.error = Terminal_worker_fixed_parameters_error::NONE;
    return result;
}

template<Fixed_terminal_worker_package_policy Policy>
Terminal_worker_fixed_parameters_result<Policy>
decode_terminal_worker_fixed_parameters(std::string_view serialized_parameters)
{
    Terminal_worker_fixed_parameters_result<Policy> result;
    const auto capabilities =
        std::span<const Terminal_worker_package_capability>(
            Policy::capabilities);
    const std::string_view schema = Policy::configuration_schema;
    const auto environment_names =
        std::span<const std::string_view>(Policy::product_environment_names);
    if (!detail::valid_fixed_policy_identity(
            Policy::package_id,
            Policy::family_id,
            capabilities,
            schema) ||
        !detail::valid_fixed_environment_names(environment_names))
    {
        result.error = Terminal_worker_fixed_parameters_error::INVALID_POLICY;
        return result;
    }

    std::string common;
    std::string configuration;
    if (schema.empty()) {
        common.assign(serialized_parameters);
    }
    else {
        std::string separator = ",\"";
        separator.append(schema);
        separator.append("\":");
        const std::size_t separator_offset =
            serialized_parameters.rfind(separator);
        if (!serialized_parameters.starts_with(
                detail::k_common_parameter_prefix) ||
            !serialized_parameters.ends_with('}') ||
            separator_offset == std::string_view::npos ||
            serialized_parameters.find(separator) != separator_offset)
        {
            result.error =
                Terminal_worker_fixed_parameters_error::MALFORMED_PARAMETERS;
            return result;
        }
        common.assign(serialized_parameters.substr(0U, separator_offset));
        common.push_back('}');
        const std::size_t configuration_offset =
            separator_offset + separator.size();
        configuration.assign(serialized_parameters.substr(
            configuration_offset,
            serialized_parameters.size() - configuration_offset - 1U));
        detail::clear_sensitive_string(separator);
    }

    Terminal_worker_envelope_result decoded_common =
        decode_terminal_worker_envelope(common);
    detail::clear_sensitive_string(common);
    if (!decoded_common.envelope) {
        detail::clear_sensitive_string(configuration);
        result.error =
            Terminal_worker_fixed_parameters_error::INVALID_COMMON_ENVELOPE;
        return result;
    }
    detail::Sensitive_envelope_guard envelope_guard(decoded_common.envelope);
    Launch_request_result validated_request =
        decode_terminal_launch_request(
            decoded_common.envelope->serialized_request,
            decoded_common.envelope->platform);
    detail::Sensitive_launch_request_guard request_guard(validated_request);
    if (validated_request.status == Launch_request_status::REJECTED ||
        (validated_request.request &&
         detail::base_contains_fixed_environment_name<Policy>(
             *validated_request.request,
             decoded_common.envelope->platform)))
    {
        detail::clear_sensitive_string(configuration);
        result.error =
            Terminal_worker_fixed_parameters_error::INVALID_COMMON_ENVELOPE;
        return result;
    }
    std::optional<typename Policy::Configuration> decoded_configuration =
        Policy::decode_configuration(configuration);
    std::optional<std::string> canonical = decoded_configuration
        ? Policy::encode_configuration(*decoded_configuration)
        : std::nullopt;
    const bool valid_configuration = decoded_configuration && canonical &&
        *canonical == configuration;
    detail::clear_sensitive_string(configuration);
    if (canonical) {
        detail::clear_sensitive_string(*canonical);
    }
    if (!valid_configuration) {
        if (decoded_configuration) {
            Policy::clear_configuration(*decoded_configuration);
        }
        result.error = Terminal_worker_fixed_parameters_error::
            INVALID_PRODUCT_CONFIGURATION;
        return result;
    }
    result.parameters.emplace(Terminal_worker_fixed_parameters<Policy>{
        std::move(*decoded_common.envelope),
        std::move(*decoded_configuration),
    });
    envelope_guard.release();
    result.error = Terminal_worker_fixed_parameters_error::NONE;
    return result;
}

namespace detail {

template<Fixed_terminal_worker_package_policy Policy>
Terminal_worker_fixed_package_binding
terminal_worker_fixed_package_binding()
{
    Terminal_worker_fixed_package_binding binding;
    binding.package_id = Policy::package_id;
    binding.family_id = Policy::family_id;
    binding.capabilities.assign(
        Policy::capabilities.begin(),
        Policy::capabilities.end());
    for (const std::string_view name : Policy::product_environment_names) {
        binding.product_environment_names.emplace_back(name);
    }
    binding.encode_parameters = [](
        const Terminal_worker_envelope& envelope,
        std::string_view canonical_configuration)
            -> std::optional<std::string>
    {
        std::optional<typename Policy::Configuration> configuration =
            Policy::decode_configuration(canonical_configuration);
        if (!configuration) {
            return std::nullopt;
        }
        Terminal_worker_fixed_parameters_result<Policy> encoded =
            encode_terminal_worker_fixed_parameters<Policy>(
                envelope,
                *configuration);
        Policy::clear_configuration(*configuration);
        if (encoded.error != Terminal_worker_fixed_parameters_error::NONE) {
            clear_sensitive_string(encoded.serialized_parameters);
            return std::nullopt;
        }
        return std::move(encoded.serialized_parameters);
    };
    return binding;
}

} // namespace detail

template<Fixed_terminal_worker_package_policy Policy>
int run_terminal_owner_service(int argc, char** argv)
{
    return detail::run_terminal_owner_service_program(
        argc,
        argv,
        detail::terminal_worker_fixed_package_binding<Policy>());
}

template<typename Policy>
class Terminal_owner_package_client
{
public:
    static_assert(Fixed_terminal_worker_package_policy<Policy>);
    static std::unique_ptr<Terminal_owner_package_client> connect(
        const Terminal_owner_client_configuration& configuration,
        Terminal_owner_client_connect_outcome* outcome = nullptr,
        std::string* diagnostic = nullptr)
    {
        std::unique_ptr<Terminal_owner_client> client =
            Terminal_owner_client::connect(configuration, outcome, diagnostic);
        return client
            ? std::unique_ptr<Terminal_owner_package_client>(
                new Terminal_owner_package_client(std::move(client)))
            : nullptr;
    }

    Terminal_owner_launch_result new_launch(
        std::span<const std::uint8_t> serialized_request,
        Launch_platform platform,
        Terminal_worker_launch_configuration launch_configuration,
        const typename Policy::Configuration& product_configuration,
        std::optional<std::vector<environment_policy::Environment_entry>>
            authorized_environment = std::nullopt)
    {
        Launch_request_result validated_request =
            decode_terminal_launch_request(
                serialized_request,
                platform);
        detail::Sensitive_launch_request_guard request_guard(
            validated_request);
        if (validated_request.status == Launch_request_status::REJECTED ||
            (validated_request.request &&
             detail::base_contains_fixed_environment_name<Policy>(
                 *validated_request.request,
                 platform)))
        {
            return {};
        }
        std::optional<std::string> canonical_configuration =
            Policy::encode_configuration(product_configuration);
        if (!canonical_configuration) {
            return {};
        }
        Terminal_owner_launch_result result =
            m_client->new_launch_for_fixed_package(
                serialized_request,
                platform,
                std::move(launch_configuration.surface),
                std::move(launch_configuration.output_capture),
                std::move(*canonical_configuration),
                std::move(authorized_environment));
        detail::clear_sensitive_string(*canonical_configuration);
        return result;
    }

    [[nodiscard]] Terminal_owner_viewer_epoch viewer_epoch() const noexcept
    {
        return m_client->viewer_epoch();
    }

    Terminal_owner_update_outcome request_close(
        const std::string& session_identity,
        std::uint64_t generation)
    {
        return m_client->request_close(session_identity, generation);
    }

    [[nodiscard]] std::optional<Terminal_owner_custody_snapshot> custody(
        const std::string& session_identity)
    {
        return m_client->custody(session_identity);
    }

    [[nodiscard]] std::vector<Terminal_owner_custody_snapshot> custodies()
    {
        return m_client->custodies();
    }

    [[nodiscard]] Terminal_owner_atomic_snapshot atomic_snapshot()
    {
        return m_client->atomic_snapshot();
    }

    Terminal_owner_proxy_outcome attach_existing(
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision)
    {
        return m_client->attach_existing(
            session_identity,
            generation,
            attachment_revision);
    }

    Terminal_owner_proxy_outcome forward_input(
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision,
        const Terminal_remote_input_message& message)
    {
        return m_client->forward_input(
            session_identity,
            generation,
            attachment_revision,
            message);
    }

    Terminal_owner_proxy_outcome forward_state(
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision,
        const Terminal_remote_state_message& message)
    {
        return m_client->forward_state(
            session_identity,
            generation,
            attachment_revision,
            message);
    }

    Terminal_owner_message_submission_result submit_message(
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision,
        std::span<const std::uint8_t> message_utf8)
    {
        return m_client->submit_message(
            session_identity,
            generation,
            attachment_revision,
            message_utf8);
    }

private:
    explicit Terminal_owner_package_client(
        std::unique_ptr<Terminal_owner_client> client)
    :
        m_client(std::move(client))
    {}

    std::unique_ptr<Terminal_owner_client> m_client;
};

} // namespace vnm::terminal_workspace
