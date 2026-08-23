#pragma once

#include "terminal_owner_core.h"
#include "vnm_viewer_authority.h"

#include <cstdint>
#include <functional>
#include <string>

namespace vnm::terminal_workspace::detail {

enum class Terminal_proxy_gate_outcome
{
    ADMITTED,
    AUTHORITY_REJECTED,
    NO_CUSTODY,
    STALE_GENERATION,
    STALE_ATTACHMENT,
    ATTACHMENT_UNAVAILABLE,
    INVALID_MESSAGE,
};

class Terminal_owner_proxy_gate
{
public:
    using Close_request = std::function<Terminal_owner_update_result(
        const std::string& session_identity,
        std::uint64_t generation,
        Terminal_close_cause cause)>;

    explicit Terminal_owner_proxy_gate(
        Terminal_owner_core& owner,
        Close_request close_request = {});

    [[nodiscard]] VNM_viewer_bind_outcome bind_initial_viewer(
        const VNM_viewer_identity& identity);
    [[nodiscard]] VNM_viewer_authority_snapshot authority_snapshot() const;

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
        const std::function<void()>& send);

    [[nodiscard]] VNM_viewer_transport_departure_outcome
        note_transport_departure(const VNM_viewer_identity& identity);
    [[nodiscard]] VNM_viewer_process_death_result
        observe_exact_process_death(
            std::uint64_t native_process_id,
            std::uint64_t native_process_creation_identity);
    [[nodiscard]] VNM_viewer_drain_outcome wait_until_epoch_drained(
        VNM_viewer_authority_epoch revoked_epoch);
    [[nodiscard]] VNM_viewer_bind_outcome begin_rebind(
        const VNM_viewer_identity& identity);
    [[nodiscard]] VNM_viewer_rebind_transition_outcome commit_rebind(
        const VNM_viewer_identity& identity);
    [[nodiscard]] VNM_viewer_rebind_transition_outcome cancel_rebind(
        const VNM_viewer_identity& identity);

private:
    void route_departure(Terminal_viewer_departure_kind kind);

    Terminal_owner_core& m_owner;
    Close_request m_close_request;
    VNM_Viewer_authority m_authority;
};

} // namespace vnm::terminal_workspace::detail
