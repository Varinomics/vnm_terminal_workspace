#pragma once

#include "vnm_terminal_workspace/terminal_worker_runtime.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vnm::terminal_workspace {

namespace detail {
struct Terminal_owner_service_access;
struct Terminal_worker_fixed_package_binding;
}

enum class Terminal_owner_custody_state
{
    STARTING,
    RUNNING,
    CLOSING,
};

enum class Terminal_owner_close_cause
{
    START_FAILED,
    START_INDETERMINATE,
    CHILD_EXIT,
    WORKER_CRASH,
    CANCELLED,
    EXPLICIT_CLOSE,
    VIEWER_DEPARTURE,
};

enum class Terminal_owner_cleanup_disposition
{
    NOT_REQUIRED,
    COMPLETE,
    RETAINED,
};

struct Terminal_owner_attachment_snapshot
{
    std::uint64_t revision = 0U;
    bool live = false;
    std::uint64_t producer_process_id = 0U;
    std::string framebuffer_path;
    std::uint64_t store_generation = 0U;
};

struct Terminal_owner_custody_snapshot
{
    std::string session_identity;
    std::string launch_request_identity;
    std::uint64_t generation = 0U;
    Terminal_owner_custody_state state =
        Terminal_owner_custody_state::STARTING;
    std::optional<Terminal_owner_close_cause> first_close_cause;
    bool framework_ready = false;
    std::optional<Terminal_child_fact> current_child_fact;
    Terminal_owner_attachment_snapshot attachment;
};

struct Terminal_owner_settlement
{
    std::string session_identity;
    std::uint64_t generation = 0U;
    std::optional<Terminal_owner_close_cause> first_close_cause;
    Terminal_owner_cleanup_disposition cleanup_disposition =
        Terminal_owner_cleanup_disposition::NOT_REQUIRED;
    std::optional<int> exit_code;
};

struct Terminal_owner_settlement_receipt_key
{
    std::string session_identity;
    std::uint64_t generation = 0U;

    friend bool operator==(
        const Terminal_owner_settlement_receipt_key&,
        const Terminal_owner_settlement_receipt_key&) = default;
};

struct Terminal_owner_atomic_snapshot
{
    std::uint64_t revision = 0U;
    std::vector<Terminal_owner_custody_snapshot> custodies;
    std::vector<Terminal_owner_settlement_receipt_key>
        unprotected_receipts;
};

enum class Terminal_owner_viewer_departure_kind
{
    TRANSPORT_DEPARTURE,
    EXACT_PROCESS_DEATH,
};

enum class Terminal_owner_viewer_departure_action
{
    CLOSE,
    KEEP_ATTACHABLE,
};

class Terminal_owner_lifetime_capability
{
public:
    virtual ~Terminal_owner_lifetime_capability() = default;

    virtual Terminal_owner_viewer_departure_action note_viewer_departure(
        Terminal_owner_viewer_departure_kind kind,
        const Terminal_owner_custody_snapshot& custody) = 0;
    virtual void accept_settlement(
        const Terminal_owner_settlement& settlement) = 0;
};

enum class Terminal_owner_launch_outcome
{
    ADMITTED,
    INVALID_REQUEST,
    CANCELLED,
    ALREADY_OWNED,
    HOST_START_REJECTED,
    GENERATION_NOT_ALLOCATED,
};

struct Terminal_owner_launch_result
{
    Terminal_owner_launch_outcome outcome =
        Terminal_owner_launch_outcome::INVALID_REQUEST;
    std::string session_identity;
    std::uint64_t generation = 0U;
};

enum class Terminal_owner_update_outcome
{
    APPLIED,
    ALREADY_CURRENT,
    STALE_GENERATION,
    REJECTED,
};

enum class Terminal_owner_proxy_outcome
{
    ADMITTED,
    AUTHORITY_REJECTED,
    NO_CUSTODY,
    STALE_GENERATION,
    STALE_ATTACHMENT,
    ATTACHMENT_UNAVAILABLE,
    INVALID_MESSAGE,
};

struct Terminal_owner_message_submission_result
{
    Terminal_owner_proxy_outcome routing =
        Terminal_owner_proxy_outcome::AUTHORITY_REJECTED;
    std::optional<Terminal_worker_message_submission_result> submission;
};

struct Terminal_owner_viewer_identity
{
    std::uint64_t transport_process_id = 0U;
    std::uint64_t native_process_id = 0U;
    std::uint64_t native_process_creation_identity = 0U;
};

using Terminal_owner_viewer_epoch = std::uint64_t;

enum class Terminal_owner_viewer_bind_outcome
{
    BOUND,
    INVALID_IDENTITY,
    ALREADY_BOUND,
    NOT_DRAINED,
    CLOSING,
};

enum class Terminal_owner_viewer_transport_departure_outcome
{
    RECORDED,
    ALREADY_RECORDED,
    IDENTITY_MISMATCH,
};

enum class Terminal_owner_viewer_process_death_outcome
{
    REVOKED,
    ALREADY_REVOKED,
    IDENTITY_MISMATCH,
    CLOSE_COMMITTED,
};

struct Terminal_owner_viewer_process_death_result
{
    Terminal_owner_viewer_process_death_outcome outcome =
        Terminal_owner_viewer_process_death_outcome::IDENTITY_MISMATCH;
    Terminal_owner_viewer_epoch revoked_epoch = 0U;
};

enum class Terminal_owner_viewer_drain_outcome
{
    DRAINED,
    NOT_REVOKED,
};

enum class Terminal_owner_viewer_rebind_outcome
{
    COMPLETED,
    NOT_IN_ADMISSION,
    IDENTITY_MISMATCH,
};

struct Terminal_owner_viewer_authority_snapshot
{
    Terminal_owner_viewer_identity identity;
    Terminal_owner_viewer_epoch epoch = 1U;
    Terminal_owner_viewer_epoch last_revoked_epoch = 0U;
    Terminal_owner_viewer_epoch last_drained_epoch = 0U;
    std::size_t active_operations = 0U;
    bool closing = false;
    bool leased = false;
    bool admission = false;
    bool transport_departed = false;
};

struct Terminal_owner_host_configuration
{
    std::string hosted_worker_host_executable_path;
    std::string terminal_worker_library_path;
    std::string provider_namespace;
};

class Terminal_owner_host
{
public:
    explicit Terminal_owner_host(Terminal_owner_host_configuration configuration);
    ~Terminal_owner_host();

    Terminal_owner_host(const Terminal_owner_host&) = delete;
    Terminal_owner_host& operator=(const Terminal_owner_host&) = delete;

    Terminal_owner_launch_result new_launch(
        std::span<const std::uint8_t> serialized_request,
        Launch_platform platform,
        std::optional<std::vector<environment_policy::Environment_entry>>
            authorized_environment = std::nullopt,
        std::shared_ptr<Terminal_owner_lifetime_capability>
            lifetime_capability = {});
    Terminal_owner_update_outcome request_close(
        const std::string& session_identity,
        std::uint64_t generation);

    [[nodiscard]] std::optional<Terminal_owner_custody_snapshot> custody(
        const std::string& session_identity) const;
    [[nodiscard]] std::vector<Terminal_owner_custody_snapshot> custodies() const;
    [[nodiscard]] Terminal_owner_atomic_snapshot atomic_snapshot(
        std::chrono::steady_clock::time_point now);

    Terminal_owner_viewer_bind_outcome bind_initial_viewer(
        const Terminal_owner_viewer_identity& identity);
    [[nodiscard]] Terminal_owner_viewer_authority_snapshot
        viewer_authority_snapshot() const;
    Terminal_owner_proxy_outcome attach_existing(
        std::uint64_t caller_transport_process_id,
        Terminal_owner_viewer_epoch expected_epoch,
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision);
    Terminal_owner_proxy_outcome forward_input(
        std::uint64_t caller_transport_process_id,
        Terminal_owner_viewer_epoch expected_epoch,
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision,
        const Terminal_remote_input_message& message);
    Terminal_owner_proxy_outcome forward_state(
        std::uint64_t caller_transport_process_id,
        Terminal_owner_viewer_epoch expected_epoch,
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision,
        const Terminal_remote_state_message& message);
    Terminal_owner_message_submission_result submit_message(
        std::uint64_t caller_transport_process_id,
        Terminal_owner_viewer_epoch expected_epoch,
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision,
        std::span<const std::uint8_t> message_utf8);

    Terminal_owner_viewer_transport_departure_outcome
        note_viewer_transport_departure(
            const Terminal_owner_viewer_identity& identity);
    Terminal_owner_viewer_process_death_result observe_exact_viewer_death(
        std::uint64_t native_process_id,
        std::uint64_t native_process_creation_identity);
    Terminal_owner_viewer_drain_outcome wait_until_viewer_epoch_drained(
        Terminal_owner_viewer_epoch revoked_epoch,
        std::chrono::steady_clock::time_point deadline);
    Terminal_owner_viewer_bind_outcome begin_viewer_rebind(
        const Terminal_owner_viewer_identity& identity);
    Terminal_owner_viewer_rebind_outcome commit_viewer_rebind(
        const Terminal_owner_viewer_identity& identity);

    bool contains_unprotected_settlement(
        const std::string& session_identity,
        std::uint64_t generation,
        std::chrono::steady_clock::time_point now);
    bool acknowledge_unprotected_settlement(
        const std::string& session_identity,
        std::uint64_t generation,
        std::chrono::steady_clock::time_point now);
    void purge_unprotected_settlements_for_shutdown();

private:
    Terminal_owner_host(
        Terminal_owner_host_configuration configuration,
        const detail::Terminal_worker_fixed_package_binding& binding);
    Terminal_owner_launch_result new_launch_for_fixed_package(
        std::span<const std::uint8_t> serialized_request,
        Launch_platform platform,
        Terminal_worker_surface_configuration surface_configuration,
        std::optional<Terminal_worker_output_capture_configuration>
            output_capture,
        std::string canonical_product_configuration,
        std::optional<std::vector<environment_policy::Environment_entry>>
            authorized_environment,
        std::shared_ptr<Terminal_owner_lifetime_capability>
            lifetime_capability = {});

    friend struct detail::Terminal_owner_service_access;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vnm::terminal_workspace
