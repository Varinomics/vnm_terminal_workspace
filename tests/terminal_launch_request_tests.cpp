#include "vnm_terminal_workspace/terminal_launch_request.h"
#include "vnm_terminal_workspace/terminal_worker_envelope.h"
#include "vnm_terminal_workspace/terminal_worker_composition.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace workspace = vnm::terminal_workspace;
namespace environment = vnm::environment_policy;

namespace {

struct Closed_product_configuration
{
    std::string control_token;
};

struct Closed_product_policy
{
    using Configuration = Closed_product_configuration;
    inline static constexpr std::string_view package_id = "test.worker";
    inline static constexpr std::string_view family_id = "test";
    inline static constexpr std::string_view configuration_schema =
        "Closed_product_configuration@1";
    inline static constexpr std::array capabilities{
        workspace::Terminal_worker_package_capability::REMOTE_UI,
        workspace::Terminal_worker_package_capability::WHOLE_MESSAGE_INPUT,
    };
    inline static constexpr std::array<std::string_view, 1>
        product_environment_names{"TEST_CONTROL_TOKEN"};
    inline static bool observed_clear = false;

    static std::optional<std::string> encode_configuration(
        const Configuration& value) noexcept
    {
        return value.control_token == "secret"
            ? std::optional<std::string>(
                "{\"control_token\":\"secret\"}")
            : std::nullopt;
    }

    static std::optional<Configuration> decode_configuration(
        std::string_view value) noexcept
    {
        return value == "{\"control_token\":\"secret\"}"
            ? std::optional<Configuration>(Configuration{"secret"})
            : std::nullopt;
    }

    static void clear_configuration(Configuration& value) noexcept
    {
        std::fill(value.control_token.begin(), value.control_token.end(), '\0');
        observed_clear = std::all_of(
            value.control_token.begin(),
            value.control_token.end(),
            [](char byte) { return byte == '\0'; });
        value.control_token.clear();
    }
};

struct Invalid_fixed_policy : Closed_product_policy
{
    inline static constexpr std::array<std::string_view, 1>
        product_environment_names{"VNM_CONTROL_TOKEN"};
};

bool check(bool condition, std::string_view message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "FAIL: %.*s\n",
        static_cast<int>(message.size()), message.data());
    return false;
}

workspace::Terminal_launch_request windows_request()
{
    workspace::Terminal_launch_request request;
    request.launch_request_id = "launch-request-a";
    request.session_id = "session-a";
    request.argv = {
        "C:\\Tools\\shell.exe",
        "",
        "argument with spaces",
        "\"quoted\" & data",
    };
    request.working_directory = "C:\\Workspace";
    request.base_environment_complete = true;
    request.base_environment = {
        {"=C:", "C:\\Workspace"},
        {"Path", "C:\\Tools"},
        {"ALLOWED_EMPTY", ""},
    };
    request.cancellation.identity = "cancel-a";
    return request;
}

workspace::Terminal_launch_request posix_request()
{
    workspace::Terminal_launch_request request = windows_request();
    request.argv.front() = "/usr/bin/shell";
    request.working_directory = "/workspace";
    request.base_environment.erase(request.base_environment.begin());
    return request;
}

bool working_directory_advisory_is_separate()
{
    std::size_t probe_calls = 0U;
    std::string observed_directory;
    const workspace::Working_directory_advisory advisory =
        workspace::probe_terminal_working_directory(
            "C:\\Workspace",
            [&probe_calls, &observed_directory](std::string_view directory) {
                ++probe_calls;
                observed_directory = directory;
                return workspace::Working_directory_advisory::MISSING;
            });

    bool ok = true;
    ok &= check(
        advisory == workspace::Working_directory_advisory::MISSING &&
            probe_calls == 1U &&
            observed_directory == "C:\\Workspace",
        "cwd advisory must invoke only the explicitly supplied probe");
    ok &= check(
        workspace::probe_terminal_working_directory("C:\\Workspace", {}) ==
            workspace::Working_directory_advisory::NOT_PROBED,
        "cwd advisory without a probe must remain explicitly unobserved");
    return ok;
}

bool request_round_trip_preserves_exact_data()
{
    const workspace::Terminal_launch_request original = windows_request();
    const workspace::Launch_request_result prepared =
        workspace::prepare_terminal_launch_request(
            original,
            workspace::Launch_platform::WINDOWS);

    bool ok = true;
    ok &= check(
        prepared.status == workspace::Launch_request_status::ACCEPTED,
        "valid Windows request must prepare");
    ok &= check(
        prepared.request.has_value() && *prepared.request == original,
        "preparation must preserve every request byte and empty argument");
    ok &= check(
        !prepared.serialized_request.empty(),
        "preparation must produce a canonical payload");

    const workspace::Launch_request_result decoded =
        workspace::decode_terminal_launch_request(
            prepared.serialized_request,
            workspace::Launch_platform::WINDOWS);
    ok &= check(
        decoded.status == workspace::Launch_request_status::ACCEPTED,
        "canonical request must decode");
    ok &= check(
        decoded.request.has_value() && *decoded.request == original,
        "request decode must preserve argv, cwd, identities, and environment");

    std::vector<std::uint8_t> with_trailing = prepared.serialized_request;
    with_trailing.push_back(0U);
    const workspace::Launch_request_result trailing =
        workspace::decode_terminal_launch_request(
            with_trailing,
            workspace::Launch_platform::WINDOWS);
    ok &= check(
        trailing.error == workspace::Launch_request_error::TRAILING_DATA,
        "strict request decode must reject trailing bytes");
    return ok;
}

bool request_structure_and_direct_exec_policy()
{
    bool ok = true;

    workspace::Terminal_launch_request request = windows_request();
    request.argv.front() = "relative\\shell.exe";
    workspace::Launch_request_result result =
        workspace::prepare_terminal_launch_request(
            request,
            workspace::Launch_platform::WINDOWS);
    ok &= check(
        result.error == workspace::Launch_request_error::RELATIVE_EXECUTABLE_PATH,
        "Windows relative executable path must be rejected");

    request = windows_request();
    request.argv.front() = "C:shell.exe";
    result = workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::WINDOWS);
    ok &= check(
        result.error == workspace::Launch_request_error::RELATIVE_EXECUTABLE_PATH,
        "Windows drive-relative executable must be rejected");

    request = windows_request();
    request.argv.front() = "shell.exe";
    result = workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::WINDOWS);
    ok &= check(
        result.status == workspace::Launch_request_status::ACCEPTED,
        "bare executable token must remain unresolved and accepted");

    request = posix_request();
    request.argv.front() = "relative/shell";
    result = workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::POSIX);
    ok &= check(
        result.error == workspace::Launch_request_error::RELATIVE_EXECUTABLE_PATH,
        "POSIX relative executable path must be rejected");

    request = posix_request();
    request.working_directory = "relative";
    result = workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::POSIX);
    ok &= check(
        result.error == workspace::Launch_request_error::WORKING_DIRECTORY_NOT_ABSOLUTE,
        "relative cwd must be rejected without probing the filesystem");

    request = posix_request();
    request.argv.clear();
    result = workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::POSIX);
    ok &= check(
        result.error == workspace::Launch_request_error::ARGV_EMPTY,
        "empty argv must be rejected");

    request = posix_request();
    request.argv.push_back(std::string("bad\0argument", 12U));
    result = workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::POSIX);
    ok &= check(
        result.error == workspace::Launch_request_error::ARGUMENT_CONTAINS_NUL,
        "argument NUL must be rejected");

    request = posix_request();
    request.base_environment_complete = false;
    result = workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::POSIX);
    ok &= check(
        result.error == workspace::Launch_request_error::BASE_ENVIRONMENT_INCOMPLETE,
        "incomplete explicit environment must be rejected");

    request = posix_request();
    request.argv.assign(
        workspace::Terminal_launch_request_limits::maximum_argument_count + 1U,
        "x");
    result = workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::POSIX);
    ok &= check(
        result.error == workspace::Launch_request_error::QUOTA_EXCEEDED,
        "argument count quota must be enforced before serialization");
    return ok;
}

bool base_environment_is_defensively_sanitized()
{
    constexpr std::array<std::string_view, 12> k_framework_canaries{
        "VNM_CONTROL_ENDPOINT",
        "VNM_CONTROL_TOKEN",
        "VNM_OWNER_ENDPOINT",
        "VNM_OWNER_TOKEN",
        "VNM_RELAY_ENDPOINT",
        "VNM_RELAY_TOKEN",
        "VNM_BOOTSTRAP_ENDPOINT",
        "VNM_BOOTSTRAP_TOKEN",
        "VNM_INVITATION_TOKEN",
        "VNM_AUTHORIZATION_TOKEN",
        "VNM_WORKER_CONTROL_ENDPOINT",
        "VNM_WORKER_CONTROL_TOKEN",
    };

    bool ok = true;
    for (std::string_view canary : k_framework_canaries) {
        workspace::Terminal_launch_request request = windows_request();
        request.base_environment.push_back({std::string(canary), "secret"});
        const workspace::Launch_request_result result =
            workspace::prepare_terminal_launch_request(
                request,
                workspace::Launch_platform::WINDOWS);
        ok &= check(
            result.error ==
                workspace::Launch_request_error::BASE_ENVIRONMENT_RESERVED_NAME,
            "every framework class-(a)/(b) canary must be rejected from the base");
    }

    workspace::Terminal_launch_request request = windows_request();
    request.base_environment.push_back({"PATH", "second"});
    workspace::Launch_request_result result =
        workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::WINDOWS);
    ok &= check(
        result.error == workspace::Launch_request_error::BASE_ENVIRONMENT_INVALID,
        "Windows case collision must be rejected");

    request = posix_request();
    request.base_environment.push_back({"PATH", "/other"});
    result = workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::POSIX);
    ok &= check(
        result.status == workspace::Launch_request_status::ACCEPTED,
        "POSIX case-distinct environment names must remain distinct");

    request = windows_request();
    request.base_environment.push_back({"BAD=NAME", "value"});
    result = workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::WINDOWS);
    ok &= check(
        result.error == workspace::Launch_request_error::BASE_ENVIRONMENT_INVALID,
        "ordinary environment names containing equals must be rejected");

    request = windows_request();
    request.base_environment.push_back({"GOOD", std::string("bad\0value", 9U)});
    result = workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::WINDOWS);
    ok &= check(
        result.error == workspace::Launch_request_error::BASE_ENVIRONMENT_INVALID,
        "environment value NUL must be rejected without exposing the value");
    return ok;
}

bool cancellation_is_preserved_at_pure_handoffs()
{
    workspace::Terminal_launch_request request = windows_request();
    request.cancellation.requested = true;
    const workspace::Launch_request_result prepared =
        workspace::prepare_terminal_launch_request(
            request,
            workspace::Launch_platform::WINDOWS);

    bool ok = true;
    ok &= check(
        prepared.status == workspace::Launch_request_status::CANCELLED &&
            prepared.error == workspace::Launch_request_error::CANCELLED,
        "preparation must return typed pre-custody cancellation");
    ok &= check(
        !prepared.serialized_request.empty(),
        "cancelled pure request must preserve its cancellation handoff state");

    const workspace::Launch_request_result decoded =
        workspace::decode_terminal_launch_request(
            prepared.serialized_request,
            workspace::Launch_platform::WINDOWS);
    ok &= check(
        decoded.status == workspace::Launch_request_status::CANCELLED &&
            decoded.request.has_value() &&
            decoded.request->cancellation == request.cancellation,
        "decode handoff must preserve typed cancellation identity and state");

    return ok;
}

bool request_malformed_quota_and_owned_lifetime()
{
    bool ok = true;

    workspace::Terminal_launch_request request = posix_request();
    request.launch_request_id = std::string("bad\0identity", 12U);
    workspace::Launch_request_result result =
        workspace::prepare_terminal_launch_request(
            request,
            workspace::Launch_platform::POSIX);
    ok &= check(
        result.error == workspace::Launch_request_error::IDENTITY_CONTAINS_NUL,
        "request identity NUL must produce a typed structural error");

    request = posix_request();
    request.base_environment.assign(
        workspace::Terminal_launch_request_limits::maximum_environment_entries +
            1U,
        {"ORDINARY", "value"});
    result = workspace::prepare_terminal_launch_request(
        request,
        workspace::Launch_platform::POSIX);
    ok &= check(
        result.error == workspace::Launch_request_error::QUOTA_EXCEEDED,
        "base environment entry quota must be enforced");

    const workspace::Launch_request_result prepared =
        workspace::prepare_terminal_launch_request(
            posix_request(),
            workspace::Launch_platform::POSIX);
    std::vector<std::uint8_t> payload = prepared.serialized_request;
    const workspace::Launch_request_result decoded =
        workspace::decode_terminal_launch_request(
            payload,
            workspace::Launch_platform::POSIX);
    std::fill(payload.begin(), payload.end(), 0U);
    ok &= check(
        decoded.request.has_value() &&
            decoded.request->launch_request_id == "launch-request-a" &&
            decoded.request->base_environment.front().name == "Path",
        "decoded request must own its data after the transport buffer changes");

    std::vector<std::uint8_t> oversized(
        workspace::Terminal_launch_request_limits::maximum_payload_bytes + 1U,
        0U);
    result = workspace::decode_terminal_launch_request(
        oversized,
        workspace::Launch_platform::POSIX);
    ok &= check(
        result.error == workspace::Launch_request_error::QUOTA_EXCEEDED,
        "oversized request payload must reject before decoding");
    return ok;
}

bool codec_golden_payloads_are_stable()
{
    constexpr auto k_golden_request = std::to_array<std::uint8_t>({
        'V', 'N', 'M', 'T', 'R', 'Q', '0', '1',
        1U, 0U, 0U, 0U,
        1U, 0U, 0U, 0U, 'r',
        1U, 0U, 0U, 0U, 's',
        1U, 0U, 0U, 0U, 'c',
        0U,
        2U, 0U, 0U, 0U,
        2U, 0U, 0U, 0U, 's', 'h',
        0U, 0U, 0U, 0U,
        1U, 0U, 0U, 0U, '/',
        1U,
        1U, 0U, 0U, 0U,
        1U, 0U, 0U, 0U, 'A',
        1U, 0U, 0U, 0U, 'B',
    });

    bool ok = true;
    ok &= check(
        k_golden_request.size() == 62U,
        "hand-constructed codec golden sizes must remain explicit");

    workspace::Terminal_launch_request request;
    request.launch_request_id = "r";
    request.session_id = "s";
    request.argv = {"sh", ""};
    request.working_directory = "/";
    request.base_environment_complete = true;
    request.base_environment = {{"A", "B"}};
    request.cancellation.identity = "c";
    const workspace::Launch_request_result encoded_request =
        workspace::prepare_terminal_launch_request(
            request,
            workspace::Launch_platform::POSIX);
    ok &= check(
        encoded_request.serialized_request == std::vector<std::uint8_t>(
            k_golden_request.begin(), k_golden_request.end()),
        "request serializer must match the independent canonical payload");
    const workspace::Launch_request_result decoded_request =
        workspace::decode_terminal_launch_request(
            k_golden_request,
            workspace::Launch_platform::POSIX);
    ok &= check(
        decoded_request.status == workspace::Launch_request_status::ACCEPTED &&
            decoded_request.request.has_value() &&
            *decoded_request.request == request,
        "strict request decoder must accept the independent canonical payload");

    return ok;
}

bool worker_envelope_is_strict_and_canonical()
{
    constexpr std::string_view k_absent_golden =
        "{\"vnm_terminal_workspace_envelope@1\":\""
        "Vk5NVFdFMDEBAAAAAQEAAABwAgAAAAECIAMAAFgCAAAAIAAAACAAAAAAgD8HAAAA"
        "Q2xhc3NpYwAAAAAAAAAAAAAqQAAAAQAAAAAAAAAAAAEAAAAAAAAoQAAAAAAAAA==\"}";
    constexpr std::string_view k_present_golden =
        "{\"vnm_terminal_workspace_envelope@1\":\""
        "Vk5NVFdFMDEBAAAAAQEAAABwAgAAAAECIAMAAFgCAAAAIAAAACAAAAAAgD8HAAAA"
        "Q2xhc3NpYwAAAAAAAAAAAAAqQAAAAQAAAAAAAAAAAAEAAAAAAAAoQAABAQAAAAEAAA"
        "BBAQAAAEI=\"}";

    workspace::Terminal_worker_envelope absent;
    absent.provider_namespace = "p";
    absent.serialized_request = {1U, 2U};
    absent.platform = workspace::Launch_platform::POSIX;
    const workspace::Terminal_worker_envelope_result encoded_absent =
        workspace::encode_terminal_worker_envelope(absent);
    const workspace::Terminal_worker_envelope_result decoded_absent =
        workspace::decode_terminal_worker_envelope(k_absent_golden);

    workspace::Terminal_worker_envelope present = absent;
    present.authorized_environment =
        std::vector<environment::Environment_entry>{{"A", "B"}};
    const workspace::Terminal_worker_envelope_result encoded_present =
        workspace::encode_terminal_worker_envelope(present);
    const workspace::Terminal_worker_envelope_result decoded_present =
        workspace::decode_terminal_worker_envelope(k_present_golden);

    bool ok = true;
    ok &= check(
        encoded_absent.error ==
                workspace::Terminal_worker_envelope_error::NONE &&
            encoded_absent.serialized_envelope == k_absent_golden &&
            decoded_absent.envelope == absent,
        "the absent-environment envelope must match the independent golden");
    ok &= check(
        encoded_present.error ==
                workspace::Terminal_worker_envelope_error::NONE &&
            encoded_present.serialized_envelope == k_present_golden &&
            decoded_present.envelope == present,
        "the present-environment envelope must match the independent golden");

    constexpr std::array<std::string_view, 5> k_rejected_json{
        "{}",
        "{\"unknown\":\"Vk5N\"}",
        "{\"vnm_terminal_workspace_envelope@1\":\"Vk5N\","
            "\"vnm_terminal_workspace_envelope@1\":\"Vk5N\"}",
        " {\"vnm_terminal_workspace_envelope@1\":\"Vk5N\"}",
        "{\"vnm_terminal_workspace_envelope@1\":\"Vk5N\"} ",
    };
    for (const std::string_view rejected : k_rejected_json) {
        const workspace::Terminal_worker_envelope_result decoded =
            workspace::decode_terminal_worker_envelope(rejected);
        ok &= check(
            !decoded.envelope &&
                decoded.error ==
                    workspace::Terminal_worker_envelope_error::
                        MALFORMED_PAYLOAD,
            "unknown, duplicate, missing, or noncanonical envelope fields must reject");
    }
    return ok;
}

bool invalid_platform_values_are_rejected_before_encoding()
{
    constexpr workspace::Launch_platform invalid =
        static_cast<workspace::Launch_platform>(91);
    workspace::Terminal_launch_request request;
    request.launch_request_id = "invalid-platform-request";
    request.session_id = "invalid-platform-session";
    request.argv = {"tool"};
    request.working_directory = "/";
    request.base_environment_complete = true;
    request.cancellation.identity = "invalid-platform-cancellation";

    workspace::Terminal_worker_envelope envelope;
    envelope.provider_namespace = "invalid-platform";
    envelope.serialized_request = {1U};
    envelope.platform = invalid;
    return check(
        workspace::prepare_terminal_launch_request(request, invalid).error ==
                workspace::Launch_request_error::MALFORMED_PAYLOAD &&
            workspace::decode_terminal_launch_request(
                std::span<const std::uint8_t>{}, invalid).error ==
                workspace::Launch_request_error::MALFORMED_PAYLOAD &&
            workspace::encode_terminal_worker_envelope(envelope).error ==
                workspace::Terminal_worker_envelope_error::MALFORMED_PAYLOAD,
        "invalid platform values must reject before serialization or decoding");
}

bool fixed_product_composition_is_typed_and_strict()
{
    const workspace::Launch_request_result prepared =
        workspace::prepare_terminal_launch_request(
            windows_request(),
            workspace::Launch_platform::WINDOWS);
    workspace::Terminal_worker_envelope envelope;
    envelope.provider_namespace = "test.provider";
    envelope.serialized_request = prepared.serialized_request;
    envelope.platform = workspace::Launch_platform::WINDOWS;
    const auto encoded =
        workspace::encode_terminal_worker_fixed_parameters<
            Closed_product_policy>(
                envelope,
                Closed_product_configuration{"secret"});
    auto decoded = workspace::decode_terminal_worker_fixed_parameters<
        Closed_product_policy>(encoded.serialized_parameters);
    bool ok = true;
    ok &= check(
        encoded.error ==
                workspace::Terminal_worker_fixed_parameters_error::NONE &&
            encoded.serialized_parameters.find(
                "\"Closed_product_configuration@1\":"
                "{\"control_token\":\"secret\"}") != std::string::npos &&
            decoded.error ==
                workspace::Terminal_worker_fixed_parameters_error::NONE &&
            decoded.parameters &&
            decoded.parameters->configuration.control_token == "secret",
        "fixed package composition must carry only its closed typed record");
    if (decoded.parameters) {
        Closed_product_policy::clear_configuration(
            decoded.parameters->configuration);
    }
    ok &= check(
        Closed_product_policy::observed_clear,
        "typed product configuration must expose causal owned-secret cleanup");
    const auto invalid_policy =
        workspace::encode_terminal_worker_fixed_parameters<
            Invalid_fixed_policy>(
                envelope,
                Closed_product_configuration{"secret"});
    ok &= check(
        invalid_policy.error ==
            workspace::Terminal_worker_fixed_parameters_error::INVALID_POLICY,
        "fixed product environment names must not overlap framework reservations");

    workspace::Terminal_launch_request base_injection = windows_request();
    base_injection.base_environment.push_back(
        {"TEST_CONTROL_TOKEN", "base-secret"});
    const workspace::Launch_request_result unrestricted =
        workspace::prepare_terminal_launch_request(
            base_injection,
            workspace::Launch_platform::WINDOWS);
    workspace::Terminal_worker_envelope injected_envelope;
    injected_envelope.provider_namespace = "test.provider";
    injected_envelope.serialized_request = unrestricted.serialized_request;
    injected_envelope.platform = workspace::Launch_platform::WINDOWS;
    const auto fixed_injection =
        workspace::encode_terminal_worker_fixed_parameters<
            Closed_product_policy>(
                injected_envelope,
                Closed_product_configuration{"secret"});
    const workspace::Terminal_worker_envelope_result common_injection =
        workspace::encode_terminal_worker_envelope(injected_envelope);
    std::string injected_parameters = common_injection.serialized_envelope;
    injected_parameters.pop_back();
    injected_parameters.append(
        ",\"Closed_product_configuration@1\":"
        "{\"control_token\":\"secret\"}}");
    const auto decoded_injection =
        workspace::decode_terminal_worker_fixed_parameters<
            Closed_product_policy>(injected_parameters);
    ok &= check(
        unrestricted.status == workspace::Launch_request_status::ACCEPTED &&
            !injected_envelope.authorized_environment &&
            fixed_injection.error ==
                workspace::Terminal_worker_fixed_parameters_error::
                    INVALID_COMMON_ENVELOPE &&
            decoded_injection.error ==
                workspace::Terminal_worker_fixed_parameters_error::
                    INVALID_COMMON_ENVELOPE &&
            !decoded_injection.parameters,
        "fixed product names must remain reserved in base input when the "
        "optional product contribution is absent");

    std::string malformed = encoded.serialized_parameters;
    malformed.insert(malformed.size() - 1U, ",\"unknown\":true");
    const auto rejected =
        workspace::decode_terminal_worker_fixed_parameters<
            Closed_product_policy>(malformed);
    ok &= check(
        rejected.error ==
                workspace::Terminal_worker_fixed_parameters_error::
                    INVALID_PRODUCT_CONFIGURATION &&
            !rejected.parameters,
        "fixed parameters must reject unknown product-level fields");
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= working_directory_advisory_is_separate();
    ok &= request_round_trip_preserves_exact_data();
    ok &= request_structure_and_direct_exec_policy();
    ok &= base_environment_is_defensively_sanitized();
    ok &= cancellation_is_preserved_at_pure_handoffs();
    ok &= request_malformed_quota_and_owned_lifetime();
    ok &= codec_golden_payloads_are_stable();
    ok &= worker_envelope_is_strict_and_canonical();
    ok &= invalid_platform_values_are_rejected_before_encoding();
    ok &= fixed_product_composition_is_typed_and_strict();
    return ok ? 0 : 1;
}
