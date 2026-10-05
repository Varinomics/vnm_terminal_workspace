#include "vnm_terminal_workspace/terminal_owner_host.h"

#include "terminal_hosted_owner.h"

#include "vnm_remote_runtime.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace workspace = vnm::terminal_workspace;
namespace detail = vnm::terminal_workspace::detail;
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

bool wait_until(const std::function<bool()>& predicate, int timeout_ms)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() <= timeout_ms) {
        QCoreApplication::processEvents();
        if (predicate()) {
            return true;
        }
        QThread::msleep(10);
    }
    return false;
}

workspace::Launch_request_result prepared_request(
    const QTemporaryDir& directory,
    std::string launch_request_identity = "hosted-owner-launch",
    std::string session_identity = "hosted-owner-session",
    std::optional<environment::Environment_entry> additional_base =
        std::nullopt)
{
    workspace::Terminal_launch_request request;
    request.launch_request_id = std::move(launch_request_identity);
    request.session_id = std::move(session_identity);
    request.argv = {
        QDir::toNativeSeparators(
            QStringLiteral("C:/Windows/System32/cmd.exe")).toStdString(),
        "/d",
        "/s",
        "/c",
        "ping -n 6 127.0.0.1 >nul",
    };
    request.working_directory = QDir::toNativeSeparators(
        directory.path()).toStdString();
    request.base_environment_complete = true;
    request.base_environment = {
        {"COMSPEC", "C:\\Windows\\System32\\cmd.exe"},
        {"Path", "C:\\Windows\\System32"},
        {"SystemRoot", "C:\\Windows"},
        {"TEMP", request.working_directory},
        {"TMP", request.working_directory},
    };
    if (additional_base) {
        request.base_environment.push_back(std::move(*additional_base));
    }
    request.cancellation.identity = "hosted-owner-cancellation";
    return workspace::prepare_terminal_launch_request(
        std::move(request),
        workspace::Launch_platform::WINDOWS);
}

class Keeping_capability final :
    public workspace::Terminal_owner_lifetime_capability
{
public:
    workspace::Terminal_owner_viewer_departure_action note_viewer_departure(
        workspace::Terminal_owner_viewer_departure_kind,
        const workspace::Terminal_owner_custody_snapshot&) override
    {
        return workspace::Terminal_owner_viewer_departure_action::
            KEEP_ATTACHABLE;
    }

    void accept_settlement(
        const workspace::Terminal_owner_settlement& value) override
    {
        settlement = value;
    }

    std::optional<workspace::Terminal_owner_settlement> settlement;
};

class Capturing_detail_capability final :
    public detail::Terminal_lifetime_capability
{
public:
    detail::Terminal_viewer_departure_action note_viewer_departure(
        detail::Terminal_viewer_departure_kind,
        const detail::Terminal_custody_snapshot&) override
    {
        return detail::Terminal_viewer_departure_action::CLOSE;
    }

    void accept_settlement(
        const detail::Terminal_owner_settlement& value) override
    {
        ++settlement_count;
        settlement = value;
    }

    int settlement_count = 0;
    std::optional<detail::Terminal_owner_settlement> settlement;
};

bool synchronous_starting_then_rejection_settles_exact_generation()
{
    QTemporaryDir directory;
    const workspace::Launch_request_result request = prepared_request(
        directory,
        "hosted-rejected-launch",
        "hosted-rejected-session");
    if (!check(
            directory.isValid() &&
                request.status == workspace::Launch_request_status::ACCEPTED,
            "the synchronous start-rejection request must prepare"))
    {
        return false;
    }

    std::uint64_t framework_generation = 7000U;
    detail::Terminal_hosted_owner_test_hooks hooks;
    hooks.start_async = [&framework_generation](
                            vnm::VNM_Hosted_worker_session& session) {
        ++framework_generation;
        session.phase_changed(
            vnm::VNM_Hosted_worker_session_phase::STARTING);
        return false;
    };
    hooks.start_generation = [&framework_generation](
                                 const vnm::VNM_Hosted_worker_session&) {
        return framework_generation;
    };
    detail::Terminal_hosted_owner owner(
        {
            {},
            {},
            QStringLiteral("vnm_terminal_workspace.start_rejection"),
        },
        std::move(hooks));

    bool ok = true;
    for (const std::uint64_t expected_generation : {7001U, 7002U}) {
        const detail::Terminal_hosted_launch_result launch = owner.launch(
            request.serialized_request,
            workspace::Launch_platform::WINDOWS);
        ok &= check(
            launch.outcome ==
                    detail::Terminal_hosted_launch_outcome::HOST_START_REJECTED &&
                launch.session_identity == "hosted-rejected-session" &&
                launch.generation == expected_generation &&
                owner.live_custody_count() == 0U &&
                owner.core().contains_unprotected_receipt(
                    {
                        "hosted-rejected-session",
                        expected_generation,
                    },
                    detail::Terminal_owner_core::Time_point::clock::now()),
            "STARTING followed by false must publish the typed exact-generation rejection receipt");
    }
    ok &= check(
        owner.core().unprotected_receipt_count(
            detail::Terminal_owner_core::Time_point::clock::now()) == 2U,
        "a rejected hosted start must release wrapper and custody for exact ID reuse");
    QCoreApplication::processEvents();
    return ok;
}

bool close_rejection_preserves_retryable_custody_until_admitted()
{
    QTemporaryDir directory;
    const workspace::Launch_request_result request = prepared_request(
        directory,
        "hosted-close-retry-launch",
        "hosted-close-retry-session");
    if (!check(
            directory.isValid() &&
                request.status == workspace::Launch_request_status::ACCEPTED,
            "the hosted close-retry request must prepare"))
    {
        return false;
    }

    constexpr std::uint64_t k_generation = 7101U;
    int close_attempt_count = 0;
    std::optional<detail::Terminal_owner_update_result> concurrent_result;
    vnm::VNM_Hosted_worker_session* hosted_session = nullptr;
    detail::Terminal_hosted_owner* owner_pointer = nullptr;
    detail::Terminal_hosted_owner_test_hooks hooks;
    hooks.start_async = [&hosted_session](
                            vnm::VNM_Hosted_worker_session& session) {
        hosted_session = &session;
        session.phase_changed(
            vnm::VNM_Hosted_worker_session_phase::STARTING);
        return true;
    };
    hooks.start_generation = [](const vnm::VNM_Hosted_worker_session&) {
        return k_generation;
    };
    hooks.close_async = [
        &close_attempt_count,
        &concurrent_result,
        &owner_pointer](vnm::VNM_Hosted_worker_session& session) {
        ++close_attempt_count;
        session.phase_changed(
            vnm::VNM_Hosted_worker_session_phase::STOPPING);
        if (close_attempt_count == 1) {
            concurrent_result = owner_pointer->request_close(
                "hosted-close-retry-session",
                k_generation,
                detail::Terminal_close_cause::EXPLICIT_CLOSE);
            return false;
        }
        return true;
    };
    detail::Terminal_hosted_owner owner(
        {
            {},
            {},
            QStringLiteral("vnm_terminal_workspace.close_retry"),
        },
        std::move(hooks));
    owner_pointer = &owner;

    const detail::Terminal_hosted_launch_result launch = owner.launch(
        request.serialized_request,
        workspace::Launch_platform::WINDOWS);
    bool ok = true;
    ok &= check(
        launch.outcome == detail::Terminal_hosted_launch_outcome::ADMITTED &&
            launch.generation == k_generation,
        "the close-retry fixture must admit synchronous STARTING custody");
    ok &= check(
        owner.request_close(
            launch.session_identity,
            launch.generation,
            detail::Terminal_close_cause::CHILD_EXIT) ==
                detail::Terminal_owner_update_result::REJECTED &&
            concurrent_result == detail::Terminal_owner_update_result::REJECTED,
        "a false lower close admission must reject both the owner and concurrent request");
    const auto after_rejection = owner.custody(launch.session_identity);
    ok &= check(
        after_rejection &&
            after_rejection->state == detail::Terminal_custody_state::STARTING &&
            !after_rejection->first_close_cause,
        "a false lower close admission must leave visible retryable custody out of CLOSING");
    ok &= check(
        owner.request_close(
            launch.session_identity,
            launch.generation,
            detail::Terminal_close_cause::EXPLICIT_CLOSE) ==
                detail::Terminal_owner_update_result::APPLIED,
        "the same custody must admit one later lower close retry");
    const auto after_admission = owner.custody(launch.session_identity);
    ok &= check(
        close_attempt_count == 2 && after_admission &&
            after_admission->state == detail::Terminal_custody_state::CLOSING &&
            after_admission->first_close_cause ==
                detail::Terminal_close_cause::CHILD_EXIT,
        "the first reserved cause must commit exactly once only after lower admission");

    vnm::VNM_Hosted_worker_close_result close_result;
    close_result.outcome = vnm::VNM_Hosted_worker_close_outcome::CLOSED;
    close_result.cleanup_disposition =
        vnm::VNM_Hosted_worker_cleanup_disposition::COMPLETE;
    hosted_session->close_finished(close_result);
    ok &= check(
        owner.live_custody_count() == 0U &&
            owner.core().contains_unprotected_receipt(
                {
                    launch.session_identity,
                    launch.generation,
                },
                detail::Terminal_owner_core::Time_point::clock::now()),
        "the admitted retry must settle once and release custody into its receipt");
    QCoreApplication::processEvents();
    return ok;
}

bool rejected_causes_survive_typed_start_failure_and_identity_reuse()
{
    QTemporaryDir directory;
    const workspace::Launch_request_result request = prepared_request(
        directory,
        "hosted-start-settlement-launch",
        "hosted-start-settlement-session");
    if (!check(
            directory.isValid() &&
                request.status == workspace::Launch_request_status::ACCEPTED,
            "the typed start-settlement request must prepare"))
    {
        return false;
    }

    std::uint64_t generation = 7200U;
    vnm::VNM_Hosted_worker_session* hosted_session = nullptr;
    detail::Terminal_hosted_owner_test_hooks hooks;
    hooks.start_async = [&generation, &hosted_session](
                            vnm::VNM_Hosted_worker_session& session) {
        ++generation;
        hosted_session = &session;
        session.phase_changed(
            vnm::VNM_Hosted_worker_session_phase::STARTING);
        return true;
    };
    hooks.start_generation = [&generation](
                                 const vnm::VNM_Hosted_worker_session&) {
        return generation;
    };
    hooks.close_async = [](vnm::VNM_Hosted_worker_session& session) {
        session.phase_changed(
            vnm::VNM_Hosted_worker_session_phase::STOPPING);
        return false;
    };
    detail::Terminal_hosted_owner owner(
        {
            {},
            {},
            QStringLiteral("vnm_terminal_workspace.start_settlement"),
        },
        std::move(hooks));
    const std::array causes{
        detail::Terminal_close_cause::EXPLICIT_CLOSE,
        detail::Terminal_close_cause::CHILD_EXIT,
        detail::Terminal_close_cause::VIEWER_DEPARTURE,
    };

    bool ok = true;
    for (std::size_t index = 0U; index < causes.size(); ++index) {
        const auto capability =
            std::make_shared<Capturing_detail_capability>();
        const detail::Terminal_hosted_launch_result launch = owner.launch(
            request.serialized_request,
            workspace::Launch_platform::WINDOWS,
            std::nullopt,
            capability);
        ok &= check(
            launch.outcome == detail::Terminal_hosted_launch_outcome::ADMITTED &&
                launch.generation == 7201U + index,
            "the typed start-settlement fixture must reuse the exact session identity");
        ok &= check(
            owner.request_close(
                launch.session_identity,
                launch.generation,
                causes[index]) ==
                detail::Terminal_owner_update_result::REJECTED,
            "the fixture lower close must reject before core CLOSING commits");

        vnm::VNM_Hosted_worker_start_result start_result;
        start_result.outcome = index % 2U == 0U
            ? vnm::VNM_Hosted_worker_start_outcome::CANCELLED
            : vnm::VNM_Hosted_worker_start_outcome::FAILED;
        hosted_session->start_finished(start_result);
        hosted_session->start_finished(start_result);
        ok &= check(
            capability->settlement_count == 1 && capability->settlement &&
                capability->settlement->session_identity ==
                    "hosted-start-settlement-session" &&
                capability->settlement->generation == launch.generation &&
                capability->settlement->first_close_cause == causes[index] &&
                owner.live_custody_count() == 0U,
            "typed start failure must settle once with the earliest rejected close cause");
    }
    QCoreApplication::processEvents();
    return ok;
}

bool rejected_close_cause_survives_running_crash_without_reuse_leak()
{
    QTemporaryDir directory;
    const workspace::Launch_request_result request = prepared_request(
        directory,
        "hosted-crash-settlement-launch",
        "hosted-crash-settlement-session");
    if (!check(
            directory.isValid() &&
                request.status == workspace::Launch_request_status::ACCEPTED,
            "the typed crash-settlement request must prepare"))
    {
        return false;
    }

    std::uint64_t generation = 7300U;
    vnm::VNM_Hosted_worker_session* hosted_session = nullptr;
    detail::Terminal_hosted_owner_test_hooks hooks;
    hooks.start_async = [&generation, &hosted_session](
                            vnm::VNM_Hosted_worker_session& session) {
        ++generation;
        hosted_session = &session;
        session.phase_changed(
            vnm::VNM_Hosted_worker_session_phase::STARTING);
        return true;
    };
    hooks.start_generation = [&generation](
                                 const vnm::VNM_Hosted_worker_session&) {
        return generation;
    };
    hooks.close_async = [](vnm::VNM_Hosted_worker_session& session) {
        session.phase_changed(
            vnm::VNM_Hosted_worker_session_phase::STOPPING);
        return false;
    };
    detail::Terminal_hosted_owner owner(
        {
            {},
            {},
            QStringLiteral("vnm_terminal_workspace.crash_settlement"),
        },
        std::move(hooks));

    const auto first_capability =
        std::make_shared<Capturing_detail_capability>();
    const detail::Terminal_hosted_launch_result first_launch = owner.launch(
        request.serialized_request,
        workspace::Launch_platform::WINDOWS,
        std::nullopt,
        first_capability);
    bool ok = true;
    ok &= check(
        first_launch.outcome == detail::Terminal_hosted_launch_outcome::ADMITTED &&
            owner.request_close(
                first_launch.session_identity,
                first_launch.generation,
                detail::Terminal_close_cause::EXPLICIT_CLOSE) ==
                detail::Terminal_owner_update_result::REJECTED,
        "the crash fixture must reserve one rejected explicit close cause");
    hosted_session->terminal_crash({vnm::Process_exit_status_kind::EXITED, 19});
    hosted_session->terminal_crash({vnm::Process_exit_status_kind::EXITED, 19});
    ok &= check(
        first_capability->settlement_count == 1 &&
            first_capability->settlement &&
            first_capability->settlement->first_close_cause ==
                detail::Terminal_close_cause::EXPLICIT_CLOSE &&
            first_capability->settlement->exit_code == 19 &&
            owner.live_custody_count() == 0U,
        "typed running crash must settle once with the earliest rejected cause");

    const auto reused_capability =
        std::make_shared<Capturing_detail_capability>();
    const detail::Terminal_hosted_launch_result reused_launch = owner.launch(
        request.serialized_request,
        workspace::Launch_platform::WINDOWS,
        std::nullopt,
        reused_capability);
    hosted_session->terminal_crash({vnm::Process_exit_status_kind::EXITED, 23});
    ok &= check(
        reused_launch.outcome ==
                detail::Terminal_hosted_launch_outcome::ADMITTED &&
            reused_launch.generation == first_launch.generation + 1U &&
            reused_capability->settlement_count == 1 &&
            reused_capability->settlement &&
            reused_capability->settlement->first_close_cause ==
                detail::Terminal_close_cause::WORKER_CRASH &&
            reused_capability->settlement->exit_code == 23 &&
            owner.live_custody_count() == 0U,
        "session identity reuse must not inherit the prior wrapper's reserved cause");
    QCoreApplication::processEvents();
    return ok;
}

bool real_hosted_worker_reconciles_and_settles()
{
    QTemporaryDir directory;
    if (!check(directory.isValid(), "the controlled child directory must exist")) {
        return false;
    }
    const workspace::Launch_request_result request = prepared_request(directory);
    if (!check(
            request.status == workspace::Launch_request_status::ACCEPTED,
            "the real hosted test request must prepare"))
    {
        return false;
    }

    workspace::Terminal_owner_host owner({
        VNM_TW_TEST_HOST_PATH,
        VNM_TW_TEST_WORKER_PATH,
        "vnm_terminal_workspace.test_owner",
    });
    const auto capability = std::make_shared<Keeping_capability>();
    const workspace::Terminal_owner_launch_result launch = owner.new_launch(
        request.serialized_request,
        workspace::Launch_platform::WINDOWS,
        std::nullopt,
        capability);
    bool ok = true;
    ok &= check(
        launch.outcome == workspace::Terminal_owner_launch_outcome::ADMITTED &&
            launch.generation != 0U,
        "new launch must atomically record one framework generation");
    if (!ok) {
        return false;
    }

    bool saw_running = false;
    const bool settled = wait_until(
        [&owner, &saw_running]() {
            const auto custody = owner.custody("hosted-owner-session");
            if (custody &&
                custody->state == workspace::Terminal_owner_custody_state::RUNNING)
            {
                saw_running = true;
            }
            return owner.custodies().empty();
        },
        30000);
    ok &= check(
        saw_running,
        "framework readiness and the started fact must converge to RUNNING");
    ok &= check(
        settled && capability->settlement.has_value() &&
            capability->settlement->first_close_cause ==
                workspace::Terminal_owner_close_cause::CHILD_EXIT,
        "child exit must admit hosted close, remove custody, and hand off settlement");
    ok &= check(
        owner.custodies().empty(),
        "the real hosted lifecycle must not strand custody");
    return ok;
}

bool neutral_package_rejects_product_environment()
{
    QTemporaryDir directory;
    const workspace::Launch_request_result request = prepared_request(
        directory,
        "neutral-policy-launch",
        "neutral-policy-session");
    if (!check(
            directory.isValid() &&
                request.status == workspace::Launch_request_status::ACCEPTED,
            "the neutral package-policy request must prepare"))
    {
        return false;
    }

    workspace::Terminal_owner_host owner({
        VNM_TW_TEST_HOST_PATH,
        VNM_TW_TEST_WORKER_PATH,
        "vnm_terminal_workspace.neutral_policy",
    });
    const auto capability = std::make_shared<Keeping_capability>();
    std::optional<std::vector<environment::Environment_entry>> environment =
        std::vector<environment::Environment_entry>{
            {"PRODUCT_ONLY_NAME", "sensitive-value"},
        };
    const workspace::Terminal_owner_launch_result launch = owner.new_launch(
        request.serialized_request,
        workspace::Launch_platform::WINDOWS,
        environment,
        capability);
    volatile char* secret = environment->front().value.data();
    for (std::size_t index = 0U;
         index < environment->front().value.size();
         ++index)
    {
        secret[index] = '\0';
    }
    environment.reset();

    return check(
        launch.outcome ==
                workspace::Terminal_owner_launch_outcome::INVALID_REQUEST &&
            launch.generation == 0U && owner.custodies().empty() &&
            !capability->settlement,
        "the fixed neutral package must reject every product environment name");
}

bool fixed_product_name_is_reserved_without_optional_contribution()
{
    QTemporaryDir directory;
    const workspace::Launch_request_result injected = prepared_request(
        directory,
        "fixed-base-injection-launch",
        "fixed-base-injection-session",
        environment::Environment_entry{
            "PRODUCT_ONLY_NAME",
            "base-secret",
        });
    const workspace::Launch_request_result valid = prepared_request(
        directory,
        "fixed-contribution-launch",
        "fixed-contribution-session");
    detail::Terminal_hosted_owner_configuration configuration;
    configuration.provider_namespace =
        QStringLiteral("vnm_terminal_workspace.fixed_product_policy");
    configuration.package_id = "fixed.product.worker";
    configuration.family_id = "fixed.product";
    configuration.capabilities = {
        workspace::Terminal_worker_package_capability::REMOTE_UI,
    };
    configuration.product_environment_names = {"PRODUCT_ONLY_NAME"};
    configuration.encode_parameters = [](
        const workspace::Terminal_worker_envelope& envelope,
        std::string_view product_configuration)
            -> std::optional<std::string>
    {
        if (!product_configuration.empty()) {
            return std::nullopt;
        }
        workspace::Terminal_worker_envelope_result encoded =
            workspace::encode_terminal_worker_envelope(envelope);
        return encoded.error == workspace::Terminal_worker_envelope_error::NONE
            ? std::optional<std::string>(
                std::move(encoded.serialized_envelope))
            : std::nullopt;
    };
    detail::Terminal_hosted_owner_test_hooks hooks;
    hooks.start_async = [](vnm::VNM_Hosted_worker_session&) {
        return false;
    };
    detail::Terminal_hosted_owner owner(
        std::move(configuration),
        std::move(hooks));

    const detail::Terminal_hosted_launch_result rejected = owner.launch(
        injected.serialized_request,
        workspace::Launch_platform::WINDOWS);
    std::optional<std::vector<environment::Environment_entry>> contribution =
        std::vector<environment::Environment_entry>{
            {"PRODUCT_ONLY_NAME", "authorized-secret"},
        };
    const detail::Terminal_hosted_launch_result allowed = owner.launch(
        valid.serialized_request,
        workspace::Launch_platform::WINDOWS,
        std::move(contribution));
    return check(
        directory.isValid() &&
            injected.status == workspace::Launch_request_status::ACCEPTED &&
            rejected.outcome ==
                detail::Terminal_hosted_launch_outcome::INVALID_REQUEST &&
            rejected.generation == 0U &&
            allowed.outcome ==
                detail::Terminal_hosted_launch_outcome::HOST_START_REJECTED &&
            owner.custodies().empty(),
        "a fixed product name must reject from base without a contribution "
        "while remaining available to the exact authorized contribution");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (!vnm::VNM_RemoteRuntime::initialize(argc, argv)) {
        std::fprintf(stderr, "FAIL: framework remote runtime did not initialize\n");
        return 1;
    }
    bool ok = true;
    ok &= synchronous_starting_then_rejection_settles_exact_generation();
    ok &= close_rejection_preserves_retryable_custody_until_admitted();
    ok &= rejected_causes_survive_typed_start_failure_and_identity_reuse();
    ok &= rejected_close_cause_survives_running_crash_without_reuse_leak();
    ok &= real_hosted_worker_reconciles_and_settles();
    ok &= neutral_package_rejects_product_environment();
    ok &= fixed_product_name_is_reserved_without_optional_contribution();
    const auto completion =
        vnm::VNM_RemoteRuntime::shutdown_with_completion();
    return ok && completion ==
            vnm::VNM_RemoteRuntime::Shutdown_completion::COMPLETE
        ? 0
        : 1;
}
