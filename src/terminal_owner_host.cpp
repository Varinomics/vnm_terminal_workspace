#include "vnm_terminal_workspace/terminal_owner_host.h"

#include "terminal_hosted_owner.h"

#include <QString>

#include <utility>

namespace vnm::terminal_workspace {
namespace {

Terminal_owner_custody_state custody_state(
    detail::Terminal_custody_state value)
{
    switch (value)
    {
    case detail::Terminal_custody_state::STARTING:
        return Terminal_owner_custody_state::STARTING;
    case detail::Terminal_custody_state::RUNNING:
        return Terminal_owner_custody_state::RUNNING;
    case detail::Terminal_custody_state::CLOSING:
        return Terminal_owner_custody_state::CLOSING;
    }
    return Terminal_owner_custody_state::CLOSING;
}

Terminal_owner_close_cause close_cause(detail::Terminal_close_cause value)
{
    switch (value)
    {
    case detail::Terminal_close_cause::START_FAILED:
        return Terminal_owner_close_cause::START_FAILED;
    case detail::Terminal_close_cause::START_INDETERMINATE:
        return Terminal_owner_close_cause::START_INDETERMINATE;
    case detail::Terminal_close_cause::CHILD_EXIT:
        return Terminal_owner_close_cause::CHILD_EXIT;
    case detail::Terminal_close_cause::WORKER_CRASH:
        return Terminal_owner_close_cause::WORKER_CRASH;
    case detail::Terminal_close_cause::CANCELLED:
        return Terminal_owner_close_cause::CANCELLED;
    case detail::Terminal_close_cause::EXPLICIT_CLOSE:
        return Terminal_owner_close_cause::EXPLICIT_CLOSE;
    case detail::Terminal_close_cause::VIEWER_DEPARTURE:
        return Terminal_owner_close_cause::VIEWER_DEPARTURE;
    }
    return Terminal_owner_close_cause::WORKER_CRASH;
}

Terminal_owner_cleanup_disposition cleanup_disposition(
    detail::Terminal_cleanup_disposition value)
{
    switch (value)
    {
    case detail::Terminal_cleanup_disposition::NOT_REQUIRED:
        return Terminal_owner_cleanup_disposition::NOT_REQUIRED;
    case detail::Terminal_cleanup_disposition::COMPLETE:
        return Terminal_owner_cleanup_disposition::COMPLETE;
    case detail::Terminal_cleanup_disposition::RETAINED:
        return Terminal_owner_cleanup_disposition::RETAINED;
    }
    return Terminal_owner_cleanup_disposition::RETAINED;
}

Terminal_owner_launch_outcome launch_outcome(
    detail::Terminal_hosted_launch_outcome value)
{
    switch (value)
    {
    case detail::Terminal_hosted_launch_outcome::ADMITTED:
        return Terminal_owner_launch_outcome::ADMITTED;
    case detail::Terminal_hosted_launch_outcome::INVALID_REQUEST:
        return Terminal_owner_launch_outcome::INVALID_REQUEST;
    case detail::Terminal_hosted_launch_outcome::CANCELLED:
        return Terminal_owner_launch_outcome::CANCELLED;
    case detail::Terminal_hosted_launch_outcome::ALREADY_OWNED:
        return Terminal_owner_launch_outcome::ALREADY_OWNED;
    case detail::Terminal_hosted_launch_outcome::HOST_START_REJECTED:
        return Terminal_owner_launch_outcome::HOST_START_REJECTED;
    case detail::Terminal_hosted_launch_outcome::GENERATION_NOT_ALLOCATED:
        return Terminal_owner_launch_outcome::GENERATION_NOT_ALLOCATED;
    }
    return Terminal_owner_launch_outcome::HOST_START_REJECTED;
}

Terminal_owner_update_outcome update_outcome(
    detail::Terminal_owner_update_result value)
{
    switch (value)
    {
    case detail::Terminal_owner_update_result::APPLIED:
        return Terminal_owner_update_outcome::APPLIED;
    case detail::Terminal_owner_update_result::ALREADY_CURRENT:
        return Terminal_owner_update_outcome::ALREADY_CURRENT;
    case detail::Terminal_owner_update_result::STALE_GENERATION:
        return Terminal_owner_update_outcome::STALE_GENERATION;
    case detail::Terminal_owner_update_result::REJECTED:
        return Terminal_owner_update_outcome::REJECTED;
    }
    return Terminal_owner_update_outcome::REJECTED;
}

Terminal_owner_proxy_outcome proxy_outcome(
    detail::Terminal_proxy_gate_outcome value)
{
    switch (value)
    {
    case detail::Terminal_proxy_gate_outcome::ADMITTED:
        return Terminal_owner_proxy_outcome::ADMITTED;
    case detail::Terminal_proxy_gate_outcome::AUTHORITY_REJECTED:
        return Terminal_owner_proxy_outcome::AUTHORITY_REJECTED;
    case detail::Terminal_proxy_gate_outcome::NO_CUSTODY:
        return Terminal_owner_proxy_outcome::NO_CUSTODY;
    case detail::Terminal_proxy_gate_outcome::STALE_GENERATION:
        return Terminal_owner_proxy_outcome::STALE_GENERATION;
    case detail::Terminal_proxy_gate_outcome::STALE_ATTACHMENT:
        return Terminal_owner_proxy_outcome::STALE_ATTACHMENT;
    case detail::Terminal_proxy_gate_outcome::ATTACHMENT_UNAVAILABLE:
        return Terminal_owner_proxy_outcome::ATTACHMENT_UNAVAILABLE;
    case detail::Terminal_proxy_gate_outcome::INVALID_MESSAGE:
        return Terminal_owner_proxy_outcome::INVALID_MESSAGE;
    }
    return Terminal_owner_proxy_outcome::AUTHORITY_REJECTED;
}

Terminal_owner_viewer_bind_outcome bind_outcome(VNM_viewer_bind_outcome value)
{
    switch (value)
    {
    case VNM_viewer_bind_outcome::BOUND:
        return Terminal_owner_viewer_bind_outcome::BOUND;
    case VNM_viewer_bind_outcome::INVALID_IDENTITY:
        return Terminal_owner_viewer_bind_outcome::INVALID_IDENTITY;
    case VNM_viewer_bind_outcome::ALREADY_BOUND:
        return Terminal_owner_viewer_bind_outcome::ALREADY_BOUND;
    case VNM_viewer_bind_outcome::NOT_DRAINED:
        return Terminal_owner_viewer_bind_outcome::NOT_DRAINED;
    case VNM_viewer_bind_outcome::CLOSING:
        return Terminal_owner_viewer_bind_outcome::CLOSING;
    }
    return Terminal_owner_viewer_bind_outcome::CLOSING;
}

Terminal_owner_viewer_transport_departure_outcome departure_outcome(
    VNM_viewer_transport_departure_outcome value)
{
    switch (value)
    {
    case VNM_viewer_transport_departure_outcome::RECORDED:
        return Terminal_owner_viewer_transport_departure_outcome::RECORDED;
    case VNM_viewer_transport_departure_outcome::ALREADY_RECORDED:
        return Terminal_owner_viewer_transport_departure_outcome::ALREADY_RECORDED;
    case VNM_viewer_transport_departure_outcome::IDENTITY_MISMATCH:
        return Terminal_owner_viewer_transport_departure_outcome::IDENTITY_MISMATCH;
    }
    return Terminal_owner_viewer_transport_departure_outcome::IDENTITY_MISMATCH;
}

Terminal_owner_viewer_process_death_outcome process_death_outcome(
    VNM_viewer_process_death_outcome value)
{
    switch (value)
    {
    case VNM_viewer_process_death_outcome::REVOKED:
        return Terminal_owner_viewer_process_death_outcome::REVOKED;
    case VNM_viewer_process_death_outcome::ALREADY_REVOKED:
        return Terminal_owner_viewer_process_death_outcome::ALREADY_REVOKED;
    case VNM_viewer_process_death_outcome::IDENTITY_MISMATCH:
        return Terminal_owner_viewer_process_death_outcome::IDENTITY_MISMATCH;
    case VNM_viewer_process_death_outcome::CLOSE_COMMITTED:
        return Terminal_owner_viewer_process_death_outcome::CLOSE_COMMITTED;
    }
    return Terminal_owner_viewer_process_death_outcome::IDENTITY_MISMATCH;
}

Terminal_owner_viewer_drain_outcome drain_outcome(
    VNM_viewer_drain_outcome value)
{
    return value == VNM_viewer_drain_outcome::DRAINED
        ? Terminal_owner_viewer_drain_outcome::DRAINED
        : Terminal_owner_viewer_drain_outcome::NOT_REVOKED;
}

Terminal_owner_viewer_rebind_outcome rebind_outcome(
    VNM_viewer_rebind_transition_outcome value)
{
    switch (value)
    {
    case VNM_viewer_rebind_transition_outcome::COMPLETED:
        return Terminal_owner_viewer_rebind_outcome::COMPLETED;
    case VNM_viewer_rebind_transition_outcome::NOT_IN_ADMISSION:
        return Terminal_owner_viewer_rebind_outcome::NOT_IN_ADMISSION;
    case VNM_viewer_rebind_transition_outcome::IDENTITY_MISMATCH:
        return Terminal_owner_viewer_rebind_outcome::IDENTITY_MISMATCH;
    }
    return Terminal_owner_viewer_rebind_outcome::IDENTITY_MISMATCH;
}

Terminal_owner_custody_snapshot custody_snapshot(
    const detail::Terminal_custody_snapshot& value)
{
    Terminal_owner_custody_snapshot result;
    result.session_identity = value.session_identity;
    result.launch_request_identity = value.launch_request_identity;
    result.generation = value.generation;
    result.state = custody_state(value.state);
    if (value.first_close_cause) {
        result.first_close_cause = close_cause(*value.first_close_cause);
    }
    result.framework_ready = value.framework_ready;
    result.current_child_fact = value.current_child_fact;
    result.attachment.revision = value.attachment_revision;
    result.attachment.live = value.attachment_live;
    result.attachment.producer_process_id =
        value.attachment_producer_process_id;
    result.attachment.framebuffer_path =
        value.attachment_framebuffer_path;
    result.attachment.store_generation =
        value.attachment_store_generation;
    return result;
}

Terminal_owner_settlement settlement(
    const detail::Terminal_owner_settlement& value)
{
    Terminal_owner_settlement result;
    result.session_identity = value.session_identity;
    result.generation = value.generation;
    if (value.first_close_cause) {
        result.first_close_cause = close_cause(*value.first_close_cause);
    }
    result.cleanup_disposition = cleanup_disposition(
        value.cleanup_disposition);
    result.exit_code = value.exit_code;
    return result;
}

class Lifetime_capability_adapter final :
    public detail::Terminal_lifetime_capability
{
public:
    explicit Lifetime_capability_adapter(
        std::shared_ptr<Terminal_owner_lifetime_capability> capability)
    :
        m_capability(std::move(capability))
    {}

    detail::Terminal_viewer_departure_action note_viewer_departure(
        detail::Terminal_viewer_departure_kind kind,
        const detail::Terminal_custody_snapshot& custody) override
    {
        const Terminal_owner_viewer_departure_action action =
            m_capability->note_viewer_departure(
                kind == detail::Terminal_viewer_departure_kind::
                    TRANSPORT_DEPARTURE
                    ? Terminal_owner_viewer_departure_kind::TRANSPORT_DEPARTURE
                    : Terminal_owner_viewer_departure_kind::EXACT_PROCESS_DEATH,
                custody_snapshot(custody));
        return action == Terminal_owner_viewer_departure_action::KEEP_ATTACHABLE
            ? detail::Terminal_viewer_departure_action::KEEP_ATTACHABLE
            : detail::Terminal_viewer_departure_action::CLOSE;
    }

    void accept_settlement(
        const detail::Terminal_owner_settlement& value) override
    {
        m_capability->accept_settlement(settlement(value));
    }

private:
    std::shared_ptr<Terminal_owner_lifetime_capability> m_capability;
};

VNM_viewer_identity viewer_identity(
    const Terminal_owner_viewer_identity& value)
{
    return {
        value.transport_process_id,
        value.native_process_id,
        value.native_process_creation_identity,
    };
}

Terminal_owner_viewer_identity viewer_identity(
    const VNM_viewer_identity& value)
{
    return {
        value.transport_process_id,
        value.native_process_id,
        value.native_process_creation_identity,
    };
}

} // namespace

struct Terminal_owner_host::Impl
{
    explicit Impl(Terminal_owner_host_configuration configuration)
    :
        owner({
            QString::fromStdString(
                configuration.hosted_worker_host_executable_path),
            QString::fromStdString(
                configuration.terminal_worker_library_path),
            QString::fromStdString(configuration.provider_namespace),
        })
    {}

    detail::Terminal_hosted_owner owner;
};

Terminal_owner_host::Terminal_owner_host(
    Terminal_owner_host_configuration configuration)
:
    m_impl(std::make_unique<Impl>(std::move(configuration)))
{}

Terminal_owner_host::~Terminal_owner_host() = default;

Terminal_owner_launch_result Terminal_owner_host::new_launch(
    std::span<const std::uint8_t> serialized_request,
    Launch_platform platform,
    std::span<const std::string_view> additional_reserved_names,
    std::optional<std::vector<environment_policy::Environment_entry>>
        authorized_environment,
    std::shared_ptr<Terminal_owner_lifetime_capability> lifetime_capability)
{
    std::shared_ptr<detail::Terminal_lifetime_capability> adapter;
    if (lifetime_capability) {
        adapter = std::make_shared<Lifetime_capability_adapter>(
            std::move(lifetime_capability));
    }
    const detail::Terminal_hosted_launch_result result = m_impl->owner.launch(
        serialized_request,
        platform,
        additional_reserved_names,
        std::move(authorized_environment),
        std::move(adapter));
    return {
        launch_outcome(result.outcome),
        result.session_identity,
        result.generation,
    };
}

Terminal_owner_update_outcome Terminal_owner_host::request_close(
    const std::string& session_identity,
    std::uint64_t generation)
{
    return update_outcome(m_impl->owner.request_close(
            session_identity,
            generation,
            detail::Terminal_close_cause::EXPLICIT_CLOSE));
}

std::optional<Terminal_owner_custody_snapshot> Terminal_owner_host::custody(
    const std::string& session_identity) const
{
    const auto value = m_impl->owner.custody(session_identity);
    return value
        ? std::optional<Terminal_owner_custody_snapshot>(
            custody_snapshot(*value))
        : std::nullopt;
}

std::vector<Terminal_owner_custody_snapshot> Terminal_owner_host::custodies()
    const
{
    std::vector<Terminal_owner_custody_snapshot> result;
    const auto values = m_impl->owner.custodies();
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(custody_snapshot(value));
    }
    return result;
}

Terminal_owner_atomic_snapshot Terminal_owner_host::atomic_snapshot(
    std::chrono::steady_clock::time_point now)
{
    const detail::Terminal_owner_atomic_snapshot value =
        m_impl->owner.core().atomic_snapshot(now);
    Terminal_owner_atomic_snapshot result;
    result.revision = value.revision;
    result.custodies.reserve(value.custodies.size());
    for (const auto& custody : value.custodies) {
        result.custodies.push_back(custody_snapshot(custody));
    }
    result.unprotected_receipts.reserve(
        value.unprotected_receipts.size());
    for (const auto& receipt : value.unprotected_receipts) {
        result.unprotected_receipts.push_back({
            receipt.session_identity,
            receipt.generation,
        });
    }
    return result;
}

Terminal_owner_viewer_bind_outcome Terminal_owner_host::bind_initial_viewer(
    const Terminal_owner_viewer_identity& identity)
{
    return bind_outcome(m_impl->owner.proxy_gate().bind_initial_viewer(
            viewer_identity(identity)));
}

Terminal_owner_viewer_authority_snapshot
Terminal_owner_host::viewer_authority_snapshot() const
{
    const VNM_viewer_authority_snapshot value =
        m_impl->owner.proxy_gate().authority_snapshot();
    return {
        viewer_identity(value.identity),
        value.epoch,
        value.last_revoked_epoch,
        value.last_drained_epoch,
        value.active_operations,
        value.disposition == VNM_viewer_runtime_disposition::CLOSING,
        value.lease == VNM_viewer_lease_state::LEASED,
        value.lease == VNM_viewer_lease_state::ADMISSION,
        value.transport_departed,
    };
}

Terminal_owner_proxy_outcome Terminal_owner_host::attach_existing(
    std::uint64_t caller_transport_process_id,
    Terminal_owner_viewer_epoch expected_epoch,
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision)
{
    return proxy_outcome(m_impl->owner.attach_existing(
            caller_transport_process_id,
            expected_epoch,
            session_identity,
            generation,
            attachment_revision));
}

Terminal_owner_proxy_outcome Terminal_owner_host::forward_input(
    std::uint64_t caller_transport_process_id,
    Terminal_owner_viewer_epoch expected_epoch,
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision,
    const Terminal_remote_input_message& message)
{
    return proxy_outcome(m_impl->owner.forward_input(
            caller_transport_process_id,
            expected_epoch,
            session_identity,
            generation,
            attachment_revision,
            message));
}

Terminal_owner_proxy_outcome Terminal_owner_host::forward_state(
    std::uint64_t caller_transport_process_id,
    Terminal_owner_viewer_epoch expected_epoch,
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision,
    const Terminal_remote_state_message& message)
{
    return proxy_outcome(m_impl->owner.forward_state(
            caller_transport_process_id,
            expected_epoch,
            session_identity,
            generation,
            attachment_revision,
            message));
}

Terminal_owner_viewer_transport_departure_outcome
Terminal_owner_host::note_viewer_transport_departure(
    const Terminal_owner_viewer_identity& identity)
{
    return departure_outcome(m_impl->owner.proxy_gate().note_transport_departure(
            viewer_identity(identity)));
}

Terminal_owner_viewer_process_death_result
Terminal_owner_host::observe_exact_viewer_death(
    std::uint64_t native_process_id,
    std::uint64_t native_process_creation_identity)
{
    const VNM_viewer_process_death_result result =
        m_impl->owner.proxy_gate().observe_exact_process_death(
            native_process_id,
            native_process_creation_identity);
    return {
        process_death_outcome(result.outcome),
        result.revoked_epoch,
    };
}

Terminal_owner_viewer_drain_outcome
Terminal_owner_host::wait_until_viewer_epoch_drained(
    Terminal_owner_viewer_epoch revoked_epoch)
{
    return drain_outcome(
        m_impl->owner.proxy_gate().wait_until_epoch_drained(revoked_epoch));
}

Terminal_owner_viewer_bind_outcome Terminal_owner_host::begin_viewer_rebind(
    const Terminal_owner_viewer_identity& identity)
{
    return bind_outcome(
        m_impl->owner.proxy_gate().begin_rebind(viewer_identity(identity)));
}

Terminal_owner_viewer_rebind_outcome Terminal_owner_host::commit_viewer_rebind(
    const Terminal_owner_viewer_identity& identity)
{
    return rebind_outcome(
        m_impl->owner.proxy_gate().commit_rebind(viewer_identity(identity)));
}

Terminal_owner_viewer_rebind_outcome Terminal_owner_host::cancel_viewer_rebind(
    const Terminal_owner_viewer_identity& identity)
{
    return rebind_outcome(
        m_impl->owner.proxy_gate().cancel_rebind(viewer_identity(identity)));
}

bool Terminal_owner_host::contains_unprotected_settlement(
    const std::string& session_identity,
    std::uint64_t generation,
    std::chrono::steady_clock::time_point now)
{
    return m_impl->owner.core().contains_unprotected_receipt(
        {session_identity, generation},
        now);
}

bool Terminal_owner_host::acknowledge_unprotected_settlement(
    const std::string& session_identity,
    std::uint64_t generation,
    std::chrono::steady_clock::time_point now)
{
    return m_impl->owner.core().acknowledge_unprotected_receipt(
        {session_identity, generation},
        now);
}

void Terminal_owner_host::purge_unprotected_settlements_for_shutdown()
{
    m_impl->owner.core().purge_receipts_for_shutdown();
}

} // namespace vnm::terminal_workspace
