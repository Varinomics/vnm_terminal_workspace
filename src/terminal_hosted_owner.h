#pragma once

#include "terminal_owner_proxy_gate.h"

#include "vnm_terminal_workspace/terminal_worker_envelope.h"

#include "vnm_control_router.h"
#include "vnm_hosted_worker_session.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vnm::terminal_workspace::detail {

enum class Terminal_hosted_launch_outcome
{
    ADMITTED,
    INVALID_REQUEST,
    CANCELLED,
    ALREADY_OWNED,
    HOST_START_REJECTED,
    GENERATION_NOT_ALLOCATED,
};

struct Terminal_hosted_launch_result
{
    Terminal_hosted_launch_outcome outcome =
        Terminal_hosted_launch_outcome::INVALID_REQUEST;
    std::string session_identity;
    std::uint64_t generation = 0U;
};

struct Terminal_hosted_owner_configuration
{
    QString host_executable_path;
    QString worker_dll_path;
    QString provider_namespace;
    std::string package_id;
    std::string family_id;
    std::vector<Terminal_worker_package_capability> capabilities;
    std::vector<std::string> product_environment_names;
    std::function<std::optional<std::string>(
        const Terminal_worker_envelope&,
        std::string_view)> encode_parameters;
};

struct Terminal_hosted_message_submission_result
{
    Terminal_proxy_gate_outcome routing =
        Terminal_proxy_gate_outcome::AUTHORITY_REJECTED;
    std::optional<Terminal_worker_message_submission_result> submission;
};

struct Terminal_hosted_owner_test_hooks
{
    std::function<bool(VNM_Hosted_worker_session&)> start_async;
    std::function<std::uint64_t(const VNM_Hosted_worker_session&)>
        start_generation;
    std::function<bool(VNM_Hosted_worker_session&)> close_async;
};

class Terminal_hosted_owner
{
public:
    explicit Terminal_hosted_owner(
        Terminal_hosted_owner_configuration configuration,
        Terminal_hosted_owner_test_hooks test_hooks = {});
    ~Terminal_hosted_owner();

    Terminal_hosted_owner(const Terminal_hosted_owner&) = delete;
    Terminal_hosted_owner& operator=(const Terminal_hosted_owner&) = delete;

    Terminal_hosted_launch_result launch(
        std::span<const std::uint8_t> serialized_request,
        Launch_platform platform,
        std::optional<std::vector<environment_policy::Environment_entry>>
            authorized_environment = std::nullopt,
        std::shared_ptr<Terminal_lifetime_capability> lifetime_capability = {},
        Terminal_worker_surface_configuration surface_configuration = {},
        std::optional<Terminal_worker_output_capture_configuration>
            output_capture = std::nullopt,
        std::string canonical_product_configuration = {});
    Terminal_owner_update_result request_close(
        const std::string& session_identity,
        std::uint64_t generation,
        Terminal_close_cause cause);
    [[nodiscard]] Terminal_proxy_gate_outcome attach_existing(
        std::uint64_t caller_transport_process_id,
        VNM_viewer_authority_epoch expected_epoch,
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision);
    [[nodiscard]] Terminal_proxy_gate_outcome forward_input(
        std::uint64_t caller_transport_process_id,
        VNM_viewer_authority_epoch expected_epoch,
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision,
        const Terminal_remote_input_message& message);
    [[nodiscard]] Terminal_proxy_gate_outcome forward_state(
        std::uint64_t caller_transport_process_id,
        VNM_viewer_authority_epoch expected_epoch,
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision,
        const Terminal_remote_state_message& message);
    [[nodiscard]] Terminal_hosted_message_submission_result submit_message(
        std::uint64_t caller_transport_process_id,
        VNM_viewer_authority_epoch expected_epoch,
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision,
        std::span<const std::uint8_t> message_utf8);

    [[nodiscard]] std::optional<Terminal_custody_snapshot> custody(
        const std::string& session_identity) const;
    [[nodiscard]] std::vector<Terminal_custody_snapshot> custodies() const;
    [[nodiscard]] std::size_t live_custody_count() const;
    [[nodiscard]] Terminal_owner_core& core();
    [[nodiscard]] Terminal_owner_proxy_gate& proxy_gate();

private:
    struct Live_session;

    static std::uint64_t numeric_session_identity(std::string_view identity);
    static Terminal_cleanup_disposition cleanup_disposition(
        VNM_Hosted_worker_cleanup_disposition disposition);
    QString worker_payload(
        std::span<const std::uint8_t> serialized_request,
        Launch_platform platform,
        const Terminal_worker_surface_configuration& surface_configuration,
        const std::optional<Terminal_worker_output_capture_configuration>&
            output_capture,
        std::string_view canonical_product_configuration,
        const std::optional<
            std::vector<environment_policy::Environment_entry>>&
                authorized_environment) const;
    void register_fact_provider();
    void connect_session(Live_session& live);
    void settle_start_failure(
        Live_session& live,
        const VNM_Hosted_worker_start_result& result);
    void settle_close(
        Live_session& live,
        const VNM_Hosted_worker_close_result& result);
    void settle_running_crash(Live_session& live, int exit_code);
    void reserve_first_close_cause(
        Live_session& live,
        Terminal_close_cause cause);
    Terminal_owner_update_result commit_first_close_cause(
        Live_session& live);
    Terminal_owner_update_result admit_reserved_close(Live_session& live);
    void schedule_hosted_close(
        const std::string& session_identity,
        std::uint64_t generation,
        Terminal_close_cause cause);
    VNM_control_result worker_context(const VNM_control_call& call);
    VNM_control_result ingest_worker_fact(const VNM_control_call& call);
    Live_session* live_for_framework_caller(const VNM_control_call& call);
    void erase_settled_session(std::string_view session_identity);
    void send_input(
        Live_session& live,
        const Terminal_remote_input_message& message);
    void send_state(
        Live_session& live,
        const Terminal_remote_state_message& message);
    Terminal_worker_message_submission_result send_message(
        Live_session& live,
        std::span<const std::uint8_t> message_utf8);

    Terminal_hosted_owner_configuration m_configuration;
    Terminal_hosted_owner_test_hooks m_test_hooks;
    std::shared_ptr<VNM_control_router> m_router;
    VNM_control_provider_group m_provider_group;
    Terminal_owner_core m_core;
    Terminal_owner_proxy_gate m_proxy_gate;
    std::map<std::string, std::unique_ptr<Live_session>, std::less<>>
        m_sessions;
    std::map<std::uint64_t, std::string> m_framework_session_identities;
};

} // namespace vnm::terminal_workspace::detail
