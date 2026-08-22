#include "vnm_terminal_workspace/terminal_launch_request.h"
#include "terminal_launch_request_test_support.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace workspace = vnm::terminal_workspace;
namespace environment = vnm::environment_policy;

namespace {

struct Wipe_observation
{
    std::size_t calls = 0U;
    std::size_t bytes = 0U;
    bool all_zero = true;
};

thread_local Wipe_observation* s_wipe_observation = nullptr;

void observe_capability_secret_wipe(
    std::span<const std::uint8_t> wiped_bytes)
{
    ++s_wipe_observation->calls;
    s_wipe_observation->bytes += wiped_bytes.size();
    s_wipe_observation->all_zero &= std::all_of(
        wiped_bytes.begin(),
        wiped_bytes.end(),
        [](std::uint8_t byte) { return byte == 0U; });
}

void reset_wipe_observation(Wipe_observation& observation)
{
    observation = {};
}

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

workspace::Terminal_capability_binding_policy contribution_policy(bool required)
{
    workspace::Terminal_capability_binding_policy policy;
    policy.required = required;
    policy.capability_id = "terminal-child-environment@1";
    policy.expected_issuer = "product-adapter-a";
    policy.intended_consumer = "terminal-child";
    policy.product_scope = "product-a";
    policy.session_scope = "session-a";
    policy.lifetime_id = "lifetime-a";
    policy.evaluation_time_unix_ms = 20'000U;
    policy.allowed_environment_names = {
        "PRODUCT_CAPABILITY_ENDPOINT",
        "PRODUCT_CAPABILITY_TOKEN",
        "PRODUCT_TRACE",
    };
    policy.additional_reserved_input_names = {
        "PRODUCT_CAPABILITY_ENDPOINT",
        "PRODUCT_CAPABILITY_TOKEN",
        "PRODUCT_PRIVATE",
    };
    return policy;
}

workspace::Terminal_capability_contribution contribution()
{
    workspace::Terminal_capability_contribution value;
    value.capability_id = "terminal-child-environment@1";
    value.issuer = "product-adapter-a";
    value.intended_consumer = "terminal-child";
    value.product_scope = "product-a";
    value.session_scope = "session-a";
    value.lifetime_id = "lifetime-a";
    value.issued_at_unix_ms = 10'000U;
    value.expires_at_unix_ms = 30'000U;
    value.allowed_environment_names = {
        "PRODUCT_TRACE",
        "PRODUCT_CAPABILITY_TOKEN",
        "PRODUCT_CAPABILITY_ENDPOINT",
    };
    value.environment = {
        {"PRODUCT_CAPABILITY_ENDPOINT", "endpoint-secret"},
        {"PRODUCT_CAPABILITY_TOKEN", "token-secret"},
        {"PRODUCT_TRACE", "enabled"},
    };
    return value;
}

bool request_round_trip_preserves_exact_data()
{
    const workspace::Terminal_launch_request original = windows_request();
    const workspace::Launch_request_result prepared =
        workspace::prepare_terminal_launch_request(
            original,
            workspace::Launch_platform::WINDOWS,
            {},
            [](std::string_view) {
                return workspace::Working_directory_advisory::MISSING;
            });

    bool ok = true;
    ok &= check(
        prepared.status == workspace::Launch_request_status::ACCEPTED,
        "valid Windows request must prepare");
    ok &= check(
        prepared.working_directory_advisory ==
            workspace::Working_directory_advisory::MISSING,
        "advisory cwd failure must be reported without rejecting preparation");
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
        "PRODUCT_CAPABILITY_TOKEN",
        "secret",
    });
    constexpr std::array<std::string_view, 3> k_product_reserved{
        "PRODUCT_CAPABILITY_ENDPOINT",
        "PRODUCT_CAPABILITY_TOKEN",
        "PRODUCT_PRIVATE",
    };
    const std::array<environment::Environment_entry, 2> unsanitized_base{{
        {"PRODUCT_CAPABILITY_TOKEN", "secret"},
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
        "class-(c) product name must be stripped from the explicit base");

    workspace::Launch_request_result result =
        workspace::prepare_terminal_launch_request(
            request,
            workspace::Launch_platform::WINDOWS,
            k_product_reserved);
    ok &= check(
        result.error ==
            workspace::Launch_request_error::BASE_ENVIRONMENT_RESERVED_NAME,
        "class-(c) product name must remain reserved from the explicit base");

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

    workspace::Terminal_capability_contribution_envelope envelope;
    envelope.present = true;
    envelope.contribution = contribution();
    const workspace::Capability_contribution_result capability =
        workspace::serialize_terminal_capability_contribution(
            std::move(envelope),
            workspace::Launch_platform::WINDOWS,
            contribution_policy(false),
            request.cancellation);
    ok &= check(
        capability.status ==
            workspace::Capability_contribution_status::CANCELLED,
        "contribution handoff must stop on the same pre-custody cancellation");
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
    constexpr auto k_golden_absent_contribution =
        std::to_array<std::uint8_t>({
            'V', 'N', 'M', 'T', 'C', 'P', '0', '1',
            1U, 0U, 0U, 0U,
            0U,
        });
    constexpr auto k_golden_present_contribution =
        std::to_array<std::uint8_t>({
            'V', 'N', 'M', 'T', 'C', 'P', '0', '1',
            1U, 0U, 0U, 0U,
            1U,
            1U, 0U, 0U, 0U, 'c',
            1U, 0U, 0U, 0U, 'i',
            1U, 0U, 0U, 0U, 'u',
            1U, 0U, 0U, 0U, 'p',
            1U, 0U, 0U, 0U, 's',
            1U, 0U, 0U, 0U, 'l',
            1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
            3U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
            2U, 0U, 0U, 0U,
            1U, 0U, 0U, 0U, 'A',
            1U, 0U, 0U, 0U, 'B',
            2U, 0U, 0U, 0U,
            1U, 0U, 0U, 0U, 'B',
            1U, 0U, 0U, 0U, '2',
            1U, 0U, 0U, 0U, 'A',
            1U, 0U, 0U, 0U, '1',
        });

    bool ok = true;
    ok &= check(
        k_golden_request.size() == 62U &&
            k_golden_absent_contribution.size() == 13U &&
            k_golden_present_contribution.size() == 97U,
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

    workspace::Terminal_capability_contribution_envelope absent;
    const workspace::Capability_contribution_result encoded_absent =
        workspace::serialize_terminal_capability_contribution(
            absent,
            workspace::Launch_platform::POSIX,
            {});
    ok &= check(
        encoded_absent.serialized_contribution == std::vector<std::uint8_t>(
            k_golden_absent_contribution.begin(),
            k_golden_absent_contribution.end()),
        "absent contribution serializer must match its independent payload");
    const workspace::Capability_contribution_result decoded_absent =
        workspace::decode_terminal_capability_contribution(
            k_golden_absent_contribution,
            workspace::Launch_platform::POSIX,
            {});
    ok &= check(
        decoded_absent.status ==
            workspace::Capability_contribution_status::ABSENT,
        "strict decoder must accept the canonical absent contribution");

    workspace::Terminal_capability_binding_policy policy;
    policy.capability_id = "c";
    policy.expected_issuer = "i";
    policy.intended_consumer = "u";
    policy.product_scope = "p";
    policy.session_scope = "s";
    policy.lifetime_id = "l";
    policy.evaluation_time_unix_ms = 2U;
    policy.allowed_environment_names = {"B", "A"};

    workspace::Terminal_capability_contribution present;
    present.capability_id = "c";
    present.issuer = "i";
    present.intended_consumer = "u";
    present.product_scope = "p";
    present.session_scope = "s";
    present.lifetime_id = "l";
    present.issued_at_unix_ms = 1U;
    present.expires_at_unix_ms = 3U;
    present.allowed_environment_names = {"B", "A"};
    present.environment = {{"B", "2"}, {"A", "1"}};
    workspace::Terminal_capability_contribution_envelope present_envelope;
    present_envelope.present = true;
    present_envelope.contribution = std::move(present);
    const workspace::Capability_contribution_result encoded_present =
        workspace::serialize_terminal_capability_contribution(
            std::move(present_envelope),
            workspace::Launch_platform::POSIX,
            policy);
    ok &= check(
        encoded_present.status ==
                workspace::Capability_contribution_status::AVAILABLE &&
            encoded_present.serialized_contribution ==
                std::vector<std::uint8_t>(
                    k_golden_present_contribution.begin(),
                    k_golden_present_contribution.end()) &&
            encoded_present.contribution.has_value() &&
            encoded_present.contribution->allowed_environment_names ==
                std::vector<std::string>({"A", "B"}),
        "present serializer must canonicalize names and match golden bytes");
    const workspace::Capability_contribution_result decoded_present =
        workspace::decode_terminal_capability_contribution(
            k_golden_present_contribution,
            workspace::Launch_platform::POSIX,
            policy);
    ok &= check(
        decoded_present.status ==
                workspace::Capability_contribution_status::AVAILABLE &&
            decoded_present.contribution.has_value() &&
            decoded_present.contribution->allowed_environment_names ==
                std::vector<std::string>({"A", "B"}) &&
            decoded_present.contribution->environment ==
                std::vector<environment::Environment_entry>(
                    {{"B", "2"}, {"A", "1"}}),
        "strict decoder must accept the independent canonical contribution");

    std::vector<std::uint8_t> noncanonical(
        k_golden_present_contribution.begin(),
        k_golden_present_contribution.end());
    std::swap(noncanonical[67], noncanonical[72]);
    const workspace::Capability_contribution_result rejected_order =
        workspace::decode_terminal_capability_contribution(
            noncanonical,
            workspace::Launch_platform::POSIX,
            policy);
    ok &= check(
        rejected_order.error == workspace::Capability_contribution_error::
            NONCANONICAL_ALLOWED_NAME_ORDER,
        "strict decoder must reject a noncanonical allowed-name wire order");
    return ok;
}

bool contribution_round_trip_and_binding()
{
    workspace::Terminal_capability_contribution_envelope envelope;
    envelope.present = true;
    envelope.contribution = contribution();
    workspace::Terminal_capability_contribution canonical =
        envelope.contribution;
    canonical.allowed_environment_names = {
        "PRODUCT_CAPABILITY_ENDPOINT",
        "PRODUCT_CAPABILITY_TOKEN",
        "PRODUCT_TRACE",
    };
    const workspace::Terminal_capability_binding_policy policy =
        contribution_policy(false);

    const workspace::Capability_contribution_result encoded =
        workspace::serialize_terminal_capability_contribution(
            std::move(envelope),
            workspace::Launch_platform::WINDOWS,
            policy);
    bool ok = true;
    ok &= check(
        encoded.status ==
            workspace::Capability_contribution_status::AVAILABLE,
        "valid exact class-(c) contribution must serialize");
    ok &= check(
        encoded.contribution.has_value() &&
            *encoded.contribution == canonical,
        "serialization must canonicalize names and preserve remaining fields");

    const workspace::Capability_contribution_result decoded =
        workspace::decode_terminal_capability_contribution(
            encoded.serialized_contribution,
            workspace::Launch_platform::WINDOWS,
            policy);
    ok &= check(
        decoded.status ==
            workspace::Capability_contribution_status::AVAILABLE &&
            decoded.contribution.has_value() &&
            *decoded.contribution == canonical,
        "valid contribution must bind to exact issuer/consumer/product/session/lifetime");

    workspace::Terminal_capability_binding_policy wrong_consumer = policy;
    wrong_consumer.intended_consumer = "hook-broker";
    const workspace::Capability_contribution_result wrong_binding =
        workspace::decode_terminal_capability_contribution(
            encoded.serialized_contribution,
            workspace::Launch_platform::WINDOWS,
            wrong_consumer);
    ok &= check(
        wrong_binding.status ==
            workspace::Capability_contribution_status::DEGRADED &&
            wrong_binding.error ==
                workspace::Capability_contribution_error::BINDING_MISMATCH,
        "optional contribution must not cross its intended consumer boundary");

    workspace::Terminal_capability_binding_policy expired = policy;
    expired.evaluation_time_unix_ms = 30'000U;
    const workspace::Capability_contribution_result expired_result =
        workspace::decode_terminal_capability_contribution(
            encoded.serialized_contribution,
            workspace::Launch_platform::WINDOWS,
            expired);
    ok &= check(
        expired_result.error ==
            workspace::Capability_contribution_error::INVALID_LIFETIME,
        "expired contribution must not bind");
    return ok;
}

bool optional_and_required_contribution_outcomes()
{
    workspace::Terminal_capability_contribution_envelope absent;
    const workspace::Capability_contribution_result absent_encoded =
        workspace::serialize_terminal_capability_contribution(
            absent,
            workspace::Launch_platform::WINDOWS,
            contribution_policy(false));

    bool ok = true;
    ok &= check(
        absent_encoded.status ==
            workspace::Capability_contribution_status::ABSENT,
        "absent optional contribution must be an explicit local absence");
    const workspace::Capability_contribution_result absent_decoded =
        workspace::decode_terminal_capability_contribution(
            absent_encoded.serialized_contribution,
            workspace::Launch_platform::WINDOWS,
            contribution_policy(false));
    ok &= check(
        absent_decoded.status ==
            workspace::Capability_contribution_status::ABSENT,
        "absent optional contribution must decode independently from the base");
    const workspace::Capability_contribution_result missing_required =
        workspace::decode_terminal_capability_contribution(
            absent_encoded.serialized_contribution,
            workspace::Launch_platform::WINDOWS,
            contribution_policy(true));
    ok &= check(
        missing_required.status ==
            workspace::Capability_contribution_status::REJECTED &&
            missing_required.error == workspace::Capability_contribution_error::
                MISSING_REQUIRED_CONTRIBUTION,
        "explicitly required absent contribution must reject");

    std::vector<std::uint8_t> unknown = absent_encoded.serialized_contribution;
    unknown[8] = 2U;
    const workspace::Capability_contribution_result unknown_optional =
        workspace::decode_terminal_capability_contribution(
            unknown,
            workspace::Launch_platform::WINDOWS,
            contribution_policy(false));
    ok &= check(
        unknown_optional.status ==
            workspace::Capability_contribution_status::DEGRADED &&
            unknown_optional.error ==
                workspace::Capability_contribution_error::UNSUPPORTED_VERSION,
        "unknown optional version must degrade locally");
    const workspace::Capability_contribution_result unknown_required =
        workspace::decode_terminal_capability_contribution(
            unknown,
            workspace::Launch_platform::WINDOWS,
            contribution_policy(true));
    ok &= check(
        unknown_required.status ==
            workspace::Capability_contribution_status::REJECTED,
        "unknown required version must reject");

    workspace::Terminal_capability_contribution_envelope wrong_version;
    wrong_version.present = true;
    wrong_version.contribution = contribution();
    wrong_version.contribution.version = 2U;
    const workspace::Capability_contribution_result typed_unknown_optional =
        workspace::serialize_terminal_capability_contribution(
            wrong_version,
            workspace::Launch_platform::WINDOWS,
            contribution_policy(false));
    ok &= check(
        typed_unknown_optional.status ==
                workspace::Capability_contribution_status::DEGRADED &&
            typed_unknown_optional.error ==
                workspace::Capability_contribution_error::UNSUPPORTED_VERSION,
        "unknown typed optional version must degrade before serialization");
    const workspace::Capability_contribution_result typed_unknown_required =
        workspace::serialize_terminal_capability_contribution(
            std::move(wrong_version),
            workspace::Launch_platform::WINDOWS,
            contribution_policy(true));
    ok &= check(
        typed_unknown_required.status ==
            workspace::Capability_contribution_status::REJECTED,
        "unknown typed required version must reject before serialization");

    std::vector<std::uint8_t> malformed = absent_encoded.serialized_contribution;
    malformed.resize(5U);
    const workspace::Capability_contribution_result malformed_optional =
        workspace::decode_terminal_capability_contribution(
            malformed,
            workspace::Launch_platform::WINDOWS,
            contribution_policy(false));
    ok &= check(
        malformed_optional.status ==
            workspace::Capability_contribution_status::DEGRADED &&
            malformed_optional.error ==
                workspace::Capability_contribution_error::MALFORMED_PAYLOAD,
        "malformed optional contribution must degrade locally");
    const workspace::Capability_contribution_result malformed_required =
        workspace::decode_terminal_capability_contribution(
            malformed,
            workspace::Launch_platform::WINDOWS,
            contribution_policy(true));
    ok &= check(
        malformed_required.status ==
            workspace::Capability_contribution_status::REJECTED,
        "malformed required contribution must reject");

    const workspace::Launch_request_result base =
        workspace::prepare_terminal_launch_request(
            windows_request(),
            workspace::Launch_platform::WINDOWS);
    ok &= check(
        base.status == workspace::Launch_request_status::ACCEPTED,
        "optional contribution failure must not invalidate the independent base request");
    return ok;
}

bool contribution_environment_policy_is_exact()
{
    bool ok = true;
    const workspace::Terminal_capability_binding_policy policy =
        contribution_policy(false);

    const auto encode_with_name = [&policy](std::string name) {
        workspace::Terminal_capability_contribution value = contribution();
        value.allowed_environment_names = {name};
        value.environment = {{std::move(name), "sensitive-value"}};
        workspace::Terminal_capability_contribution_envelope envelope;
        envelope.present = true;
        envelope.contribution = std::move(value);

        workspace::Terminal_capability_binding_policy matching_policy = policy;
        matching_policy.allowed_environment_names =
            envelope.contribution.allowed_environment_names;
        return workspace::serialize_terminal_capability_contribution(
            std::move(envelope),
            workspace::Launch_platform::WINDOWS,
            matching_policy);
    };

    workspace::Terminal_capability_contribution_envelope product_overlap;
    product_overlap.present = true;
    product_overlap.contribution = contribution();
    const workspace::Capability_contribution_result overlap_result =
        workspace::serialize_terminal_capability_contribution(
            std::move(product_overlap),
            workspace::Launch_platform::WINDOWS,
            policy);
    ok &= check(
        overlap_result.status ==
                workspace::Capability_contribution_status::AVAILABLE &&
            overlap_result.contribution.has_value() &&
            overlap_result.contribution->environment.front().name ==
                "PRODUCT_CAPABILITY_ENDPOINT",
        "exact class-(a)/class-(c) product intersection must be accepted");

    constexpr std::array<std::string_view, 2> k_framework_class_a_names{
        "VNM_CONTROL_ENDPOINT",
        "VNM_CONTROL_TOKEN",
    };
    for (const std::string_view framework_name : k_framework_class_a_names) {
        const workspace::Capability_contribution_result result =
            encode_with_name(std::string(framework_name));
        ok &= check(
            result.error == workspace::Capability_contribution_error::
                RESERVED_ENVIRONMENT_NAME,
            "framework class-(a) name must not become product contribution data");
    }

    constexpr std::array<std::string_view, 10> k_class_b_credentials{
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
    for (const std::string_view infrastructure_name : k_class_b_credentials) {
        const workspace::Capability_contribution_result result =
            encode_with_name(std::string(infrastructure_name));
        ok &= check(
            result.error == workspace::Capability_contribution_error::
                RESERVED_ENVIRONMENT_NAME,
            "class-(b) infrastructure credential must never be exportable");
    }

    constexpr std::array<std::string_view, 11> k_forbidden_terminal_names{
        "TERM",
        "term",
        "COLORTERM",
        "NO_COLOR",
        "PATH",
        "path",
        "PATHEXT",
        "SystemRoot",
        "systemroot",
        "WINDIR",
        "=C:",
    };
    for (std::string_view name : k_forbidden_terminal_names) {
        const workspace::Capability_contribution_result result =
            encode_with_name(std::string(name));
        ok &= check(
            result.status == workspace::Capability_contribution_status::DEGRADED,
            "invalid optional names must degrade without becoming available");
    }

    workspace::Terminal_capability_contribution value = contribution();
    value.environment.push_back({"NOT_ALLOWED", "sensitive-value"});
    workspace::Terminal_capability_contribution_envelope envelope;
    envelope.present = true;
    envelope.contribution = std::move(value);
    workspace::Capability_contribution_result result =
        workspace::serialize_terminal_capability_contribution(
            std::move(envelope),
            workspace::Launch_platform::WINDOWS,
            policy);
    ok &= check(
        result.error == workspace::Capability_contribution_error::
            ENVIRONMENT_NAME_NOT_ALLOWED,
        "contribution entry must be in the exact recorded allowlist");

    value = contribution();
    value.environment.push_back({"PRODUCT_PRIVATE", "sensitive-value"});
    envelope.present = true;
    envelope.contribution = std::move(value);
    result = workspace::serialize_terminal_capability_contribution(
        std::move(envelope),
        workspace::Launch_platform::WINDOWS,
        policy);
    ok &= check(
        result.error == workspace::Capability_contribution_error::
            ENVIRONMENT_NAME_NOT_ALLOWED,
        "product reservation outside the exact allowlist must remain unavailable");

    value = contribution();
    value.allowed_environment_names.push_back("product_trace");
    envelope.present = true;
    envelope.contribution = std::move(value);
    result = workspace::serialize_terminal_capability_contribution(
        std::move(envelope),
        workspace::Launch_platform::WINDOWS,
        policy);
    ok &= check(
        result.error ==
            workspace::Capability_contribution_error::DUPLICATE_ALLOWED_NAME,
        "Windows case-fold duplicate allowlist names must reject");

    value = contribution();
    value.environment.push_back({"PRODUCT_TRACE", "duplicate"});
    envelope.present = true;
    envelope.contribution = std::move(value);
    result = workspace::serialize_terminal_capability_contribution(
        std::move(envelope),
        workspace::Launch_platform::WINDOWS,
        policy);
    ok &= check(
        result.error == workspace::Capability_contribution_error::
            DUPLICATE_ENVIRONMENT_NAME,
        "duplicate contribution entries must reject");

    value = contribution();
    value.environment[0].value = std::string("secret\0tail", 11U);
    envelope.present = true;
    envelope.contribution = std::move(value);
    result = workspace::serialize_terminal_capability_contribution(
        std::move(envelope),
        workspace::Launch_platform::WINDOWS,
        policy);
    ok &= check(
        result.error == workspace::Capability_contribution_error::
            ENVIRONMENT_VALUE_CONTAINS_NUL &&
            !result.contribution.has_value() &&
            result.serialized_contribution.empty(),
        "rejected secret value must not be retained in result diagnostics or payload");
    return ok;
}

bool contribution_binding_quota_and_owned_lifetime()
{
    bool ok = true;
    const workspace::Terminal_capability_binding_policy policy =
        contribution_policy(false);

    workspace::Terminal_capability_contribution_envelope envelope;
    envelope.present = true;
    envelope.contribution = contribution();
    workspace::Capability_contribution_result encoded =
        workspace::serialize_terminal_capability_contribution(
            envelope,
            workspace::Launch_platform::WINDOWS,
            policy);
    std::vector<std::uint8_t> payload = encoded.serialized_contribution;
    const workspace::Capability_contribution_result decoded =
        workspace::decode_terminal_capability_contribution(
            payload,
            workspace::Launch_platform::WINDOWS,
            policy);
    std::fill(payload.begin(), payload.end(), 0U);
    envelope.contribution.environment.front().value = "changed";
    ok &= check(
        decoded.contribution.has_value() &&
            decoded.contribution->environment.front().value ==
                "endpoint-secret",
        "decoded contribution must own values after source buffers change");

    std::vector<workspace::Terminal_capability_binding_policy> mismatches;
    workspace::Terminal_capability_binding_policy mismatch = policy;
    mismatch.capability_id = "other-capability";
    mismatches.push_back(mismatch);
    mismatch = policy;
    mismatch.expected_issuer = "other-issuer";
    mismatches.push_back(mismatch);
    mismatch = policy;
    mismatch.intended_consumer = "other-consumer";
    mismatches.push_back(mismatch);
    mismatch = policy;
    mismatch.product_scope = "other-product";
    mismatches.push_back(mismatch);
    mismatch = policy;
    mismatch.session_scope = "other-session";
    mismatches.push_back(mismatch);
    mismatch = policy;
    mismatch.lifetime_id = "other-lifetime";
    mismatches.push_back(mismatch);
    for (const auto& wrong_policy : mismatches) {
        const workspace::Capability_contribution_result result =
            workspace::decode_terminal_capability_contribution(
                encoded.serialized_contribution,
                workspace::Launch_platform::WINDOWS,
                wrong_policy);
        ok &= check(
            result.status ==
                    workspace::Capability_contribution_status::DEGRADED &&
                result.error == workspace::Capability_contribution_error::
                    BINDING_MISMATCH,
            "every contribution identity dimension must bind exactly");
    }

    workspace::Terminal_capability_contribution invalid = contribution();
    invalid.allowed_environment_names.pop_back();
    envelope.present = true;
    envelope.contribution = invalid;
    workspace::Capability_contribution_result result =
        workspace::serialize_terminal_capability_contribution(
            envelope,
            workspace::Launch_platform::WINDOWS,
            policy);
    ok &= check(
        result.status == workspace::Capability_contribution_status::DEGRADED &&
            result.error == workspace::Capability_contribution_error::
                BINDING_MISMATCH,
        "invalid optional exact-name binding must degrade locally");
    workspace::Terminal_capability_binding_policy required_policy = policy;
    required_policy.required = true;
    result = workspace::serialize_terminal_capability_contribution(
        envelope,
        workspace::Launch_platform::WINDOWS,
        required_policy);
    ok &= check(
        result.status == workspace::Capability_contribution_status::REJECTED,
        "the same invalid required contribution must reject");

    invalid = contribution();
    invalid.allowed_environment_names.clear();
    for (std::size_t index = 0U;
         index <= workspace::Terminal_capability_contribution_limits::
             maximum_allowed_names;
         ++index)
    {
        invalid.allowed_environment_names.push_back(
            "PRODUCT_VALUE_" + std::to_string(index));
    }
    envelope.contribution = std::move(invalid);
    result = workspace::serialize_terminal_capability_contribution(
        envelope,
        workspace::Launch_platform::WINDOWS,
        policy);
    ok &= check(
        result.status == workspace::Capability_contribution_status::DEGRADED &&
            result.error ==
                workspace::Capability_contribution_error::QUOTA_EXCEEDED,
        "oversized optional contribution allowlist must degrade locally");

    std::vector<std::uint8_t> oversized(
        workspace::Terminal_capability_contribution_limits::
                maximum_payload_bytes +
            1U,
        0U);
    result = workspace::decode_terminal_capability_contribution(
        oversized,
        workspace::Launch_platform::WINDOWS,
        policy);
    ok &= check(
        result.status == workspace::Capability_contribution_status::DEGRADED &&
            result.error ==
                workspace::Capability_contribution_error::QUOTA_EXCEEDED,
        "oversized optional contribution payload must degrade locally");

    workspace::Terminal_capability_binding_policy posix_policy = policy;
    posix_policy.allowed_environment_names = {"Path"};
    invalid = contribution();
    invalid.allowed_environment_names = {"Path"};
    invalid.environment = {{"Path", "/tools"}};
    envelope.contribution = std::move(invalid);
    result = workspace::serialize_terminal_capability_contribution(
        envelope,
        workspace::Launch_platform::POSIX,
        posix_policy);
    ok &= check(
        result.status == workspace::Capability_contribution_status::AVAILABLE,
        "POSIX case-distinct Path contribution must not be treated as PATH");
    return ok;
}

bool capability_secrets_are_wiped_on_failure()
{
    workspace::Terminal_capability_contribution_envelope encoded_envelope;
    encoded_envelope.present = true;
    encoded_envelope.contribution = contribution();
    const workspace::Capability_contribution_result encoded =
        workspace::serialize_terminal_capability_contribution(
            std::move(encoded_envelope),
            workspace::Launch_platform::WINDOWS,
            contribution_policy(false));

    Wipe_observation observation;
    s_wipe_observation = &observation;
    workspace::testing::set_capability_secret_wipe_observer(
        observe_capability_secret_wipe);
    bool ok = true;

    workspace::Terminal_capability_contribution_envelope envelope;
    envelope.present = true;
    envelope.contribution = contribution();
    workspace::Pre_custody_cancellation cancellation;
    cancellation.identity = "cancel-secret-handoff";
    cancellation.requested = true;
    workspace::Capability_contribution_result result =
        workspace::serialize_terminal_capability_contribution(
            std::move(envelope),
            workspace::Launch_platform::WINDOWS,
            contribution_policy(false),
            cancellation);
    ok &= check(
        result.status == workspace::Capability_contribution_status::CANCELLED &&
            observation.calls == 3U &&
            observation.bytes > 0U &&
            observation.all_zero,
        "cancellation must zero every owned contribution value before return");

    reset_wipe_observation(observation);
    envelope.present = true;
    envelope.contribution = contribution();
    envelope.contribution.issuer = "wrong-issuer";
    result = workspace::serialize_terminal_capability_contribution(
        std::move(envelope),
        workspace::Launch_platform::WINDOWS,
        contribution_policy(false));
    ok &= check(
        result.error ==
                workspace::Capability_contribution_error::BINDING_MISMATCH &&
            observation.calls == 3U &&
            observation.all_zero,
        "binding rejection must zero every owned contribution value");

    reset_wipe_observation(observation);
    envelope.present = false;
    envelope.contribution.environment = {{"IGNORED", "secret-value"}};
    result = workspace::serialize_terminal_capability_contribution(
        std::move(envelope),
        workspace::Launch_platform::WINDOWS,
        contribution_policy(true));
    ok &= check(
        result.error == workspace::Capability_contribution_error::
                MISSING_REQUIRED_CONTRIBUTION &&
            observation.calls == 1U &&
            observation.all_zero,
        "absent required rejection must clear ignored owned secret state");

    reset_wipe_observation(observation);
    workspace::Terminal_capability_binding_policy wrong_policy =
        contribution_policy(false);
    wrong_policy.expected_issuer = "wrong-issuer";
    result = workspace::decode_terminal_capability_contribution(
        encoded.serialized_contribution,
        workspace::Launch_platform::WINDOWS,
        wrong_policy);
    ok &= check(
        result.error ==
                workspace::Capability_contribution_error::BINDING_MISMATCH &&
            observation.calls == 3U &&
            observation.all_zero,
        "fully decoded rejection must zero all decoded secret values");

    reset_wipe_observation(observation);
    std::vector<std::uint8_t> truncated = encoded.serialized_contribution;
    truncated.pop_back();
    result = workspace::decode_terminal_capability_contribution(
        truncated,
        workspace::Launch_platform::WINDOWS,
        contribution_policy(false));
    ok &= check(
        result.error ==
                workspace::Capability_contribution_error::MALFORMED_PAYLOAD &&
            observation.calls == 2U &&
            observation.all_zero,
        "partial decode failure must zero every completed secret value");

    workspace::testing::set_capability_secret_wipe_observer(nullptr);
    s_wipe_observation = nullptr;
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= request_round_trip_preserves_exact_data();
    ok &= request_structure_and_direct_exec_policy();
    ok &= base_environment_is_defensively_sanitized();
    ok &= cancellation_is_preserved_at_pure_handoffs();
    ok &= request_malformed_quota_and_owned_lifetime();
    ok &= codec_golden_payloads_are_stable();
    ok &= contribution_round_trip_and_binding();
    ok &= optional_and_required_contribution_outcomes();
    ok &= contribution_environment_policy_is_exact();
    ok &= contribution_binding_quota_and_owned_lifetime();
    ok &= capability_secrets_are_wiped_on_failure();
    return ok ? 0 : 1;
}
