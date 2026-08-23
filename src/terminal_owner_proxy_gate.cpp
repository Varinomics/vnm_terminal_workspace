#include "terminal_owner_proxy_gate.h"

#include <utility>

namespace vnm::terminal_workspace::detail {

Terminal_owner_proxy_gate::Terminal_owner_proxy_gate(
    Terminal_owner_core& owner,
    Close_request close_request)
:
    m_owner(owner),
    m_close_request(std::move(close_request))
{}

VNM_viewer_bind_outcome Terminal_owner_proxy_gate::bind_initial_viewer(
    const VNM_viewer_identity& identity)
{
    return m_authority.bind_initial(identity);
}

VNM_viewer_authority_snapshot
Terminal_owner_proxy_gate::authority_snapshot() const
{
    return m_authority.snapshot();
}

Terminal_proxy_gate_outcome Terminal_owner_proxy_gate::attach_existing(
    std::uint64_t caller_transport_process_id,
    VNM_viewer_authority_epoch expected_epoch,
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision)
{
    VNM_Viewer_authority_operation operation =
        m_authority.admit_operation(
            caller_transport_process_id,
            expected_epoch);
    if (!operation) {
        return Terminal_proxy_gate_outcome::AUTHORITY_REJECTED;
    }

    const auto custody = m_owner.custody(session_identity);
    if (!custody) {
        return Terminal_proxy_gate_outcome::NO_CUSTODY;
    }
    if (custody->generation != generation) {
        return Terminal_proxy_gate_outcome::STALE_GENERATION;
    }
    if (custody->attachment_revision != attachment_revision) {
        return Terminal_proxy_gate_outcome::STALE_ATTACHMENT;
    }
    if (!custody->attachment_live) {
        return Terminal_proxy_gate_outcome::ATTACHMENT_UNAVAILABLE;
    }
    return Terminal_proxy_gate_outcome::ADMITTED;
}

Terminal_proxy_gate_outcome Terminal_owner_proxy_gate::forward_input(
    std::uint64_t caller_transport_process_id,
    VNM_viewer_authority_epoch expected_epoch,
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision,
    const std::function<void()>& send)
{
    VNM_Viewer_authority_operation operation =
        m_authority.admit_operation(
            caller_transport_process_id,
            expected_epoch);
    if (!operation) {
        return Terminal_proxy_gate_outcome::AUTHORITY_REJECTED;
    }
    const auto custody = m_owner.custody(session_identity);
    if (!custody) {
        return Terminal_proxy_gate_outcome::NO_CUSTODY;
    }
    if (custody->generation != generation) {
        return Terminal_proxy_gate_outcome::STALE_GENERATION;
    }
    if (custody->attachment_revision != attachment_revision) {
        return Terminal_proxy_gate_outcome::STALE_ATTACHMENT;
    }
    if (!m_owner.can_forward_input(
            session_identity,
            generation,
            attachment_revision))
    {
        return Terminal_proxy_gate_outcome::ATTACHMENT_UNAVAILABLE;
    }
    if (!send) {
        return Terminal_proxy_gate_outcome::AUTHORITY_REJECTED;
    }
    send();
    return Terminal_proxy_gate_outcome::ADMITTED;
}

Terminal_proxy_gate_outcome Terminal_owner_proxy_gate::submit_message(
    std::uint64_t caller_transport_process_id,
    VNM_viewer_authority_epoch expected_epoch,
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision,
    const std::function<void()>& send)
{
    return forward_input(
        caller_transport_process_id,
        expected_epoch,
        session_identity,
        generation,
        attachment_revision,
        send);
}

VNM_viewer_transport_departure_outcome
Terminal_owner_proxy_gate::note_transport_departure(
    const VNM_viewer_identity& identity)
{
    const VNM_viewer_transport_departure_outcome outcome =
        m_authority.note_transport_departure(identity);
    if (outcome == VNM_viewer_transport_departure_outcome::RECORDED) {
        route_departure(Terminal_viewer_departure_kind::TRANSPORT_DEPARTURE);
    }
    return outcome;
}

VNM_viewer_process_death_result
Terminal_owner_proxy_gate::observe_exact_process_death(
    std::uint64_t native_process_id,
    std::uint64_t native_process_creation_identity)
{
    const VNM_viewer_process_death_result result =
        m_authority.observe_authorized_process_death(
            native_process_id,
            native_process_creation_identity);
    if (result.outcome == VNM_viewer_process_death_outcome::REVOKED) {
        route_departure(Terminal_viewer_departure_kind::EXACT_PROCESS_DEATH);
    }
    return result;
}

VNM_viewer_drain_outcome Terminal_owner_proxy_gate::wait_until_epoch_drained(
    VNM_viewer_authority_epoch revoked_epoch)
{
    return m_authority.wait_until_epoch_drained(revoked_epoch);
}

VNM_viewer_bind_outcome Terminal_owner_proxy_gate::begin_rebind(
    const VNM_viewer_identity& identity)
{
    return m_authority.rebind(identity);
}

VNM_viewer_rebind_transition_outcome Terminal_owner_proxy_gate::commit_rebind(
    const VNM_viewer_identity& identity)
{
    return m_authority.commit_rebind(identity);
}

VNM_viewer_rebind_transition_outcome Terminal_owner_proxy_gate::cancel_rebind(
    const VNM_viewer_identity& identity)
{
    return m_authority.cancel_rebind(identity);
}

void Terminal_owner_proxy_gate::route_departure(
    Terminal_viewer_departure_kind kind)
{
    const auto custodies = m_owner.custodies();
    for (const Terminal_custody_snapshot& custody : custodies) {
        const Terminal_viewer_departure_result result =
            m_owner.note_viewer_departure(
                custody.session_identity,
                custody.generation,
                kind);
        if (result == Terminal_viewer_departure_result::CLOSE &&
            m_close_request)
        {
            static_cast<void>(m_close_request(
                custody.session_identity,
                custody.generation,
                Terminal_close_cause::VIEWER_DEPARTURE));
        }
    }
}

} // namespace vnm::terminal_workspace::detail
