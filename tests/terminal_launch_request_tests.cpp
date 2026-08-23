#include "vnm_terminal_workspace/terminal_launch_request.h"

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
    request.base_environment.push_back({
        "PRODUCT_PRIVATE_TOKEN",
        "secret",
    });
    constexpr std::array<std::string_view, 3> k_product_reserved{
        "PRODUCT_PRIVATE_ENDPOINT",
        "PRODUCT_PRIVATE_TOKEN",
        "PRODUCT_INTERNAL",
    };
    const std::array<environment::Environment_entry, 2> unsanitized_base{{
        {"PRODUCT_PRIVATE_TOKEN", "secret"},
        {"PRODUCT_VISIBLE", "ordinary"},
    }};
    const environment::Environment_sanitization_result sanitized =
        environment::sanitize_explicit_base_environment(
            unsanitized_base,
            environment::Environment_platform::WINDOWS,
            k_product_reserved);
    ok &= check(
        sanitized.accepted &&
            sanitized.entries ==
                std::vector<environment::Environment_entry>(
                    {{"PRODUCT_VISIBLE", "ordinary"}}) &&
            sanitized.issues.size() == 1U &&
            sanitized.issues.front().reserved_class ==
                environment::Reserved_environment_class::
                    PRODUCT_RESERVED_INPUT,
        "product-owned name must be stripped from the explicit base");

    workspace::Launch_request_result result =
        workspace::prepare_terminal_launch_request(
            request,
            workspace::Launch_platform::WINDOWS,
            k_product_reserved);
    ok &= check(
        result.error ==
            workspace::Launch_request_error::BASE_ENVIRONMENT_RESERVED_NAME,
        "product-owned name must remain reserved from the explicit base");

    request = windows_request();
    request.base_environment.push_back({"PATH", "second"});
    result = workspace::prepare_terminal_launch_request(
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
    return ok ? 0 : 1;
}
