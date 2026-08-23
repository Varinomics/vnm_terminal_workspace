#pragma once

#include "terminal_settlement_receipt_inbox.h"
#include "vnm_terminal_workspace/terminal_worker_runtime.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vnm::terminal_workspace::detail {

enum class Terminal_custody_state
{
    STARTING,
    RUNNING,
    CLOSING,
};

enum class Terminal_close_cause
{
    START_FAILED,
    START_INDETERMINATE,
    CHILD_EXIT,
    WORKER_CRASH,
    CANCELLED,
    EXPLICIT_CLOSE,
    VIEWER_DEPARTURE,
};

enum class Terminal_owner_admission_result
{
    ADMITTED,
    INVALID_SESSION_IDENTITY,
    INVALID_LAUNCH_REQUEST_IDENTITY,
    INVALID_GENERATION,
    ALREADY_OWNED,
};

enum class Terminal_owner_update_result
{
    APPLIED,
    ALREADY_CURRENT,
    STALE_GENERATION,
    REJECTED,
};

enum class Terminal_cleanup_disposition
{
    NOT_REQUIRED,
    COMPLETE,
    RETAINED,
};

struct Terminal_owner_settlement
{
    std::string session_identity;
    std::uint64_t generation = 0U;
    std::optional<Terminal_close_cause> first_close_cause;
    Terminal_cleanup_disposition cleanup_disposition =
        Terminal_cleanup_disposition::NOT_REQUIRED;
    std::optional<int> exit_code;

    friend bool operator==(
        const Terminal_owner_settlement&,
        const Terminal_owner_settlement&) = default;
};

enum class Terminal_owner_settlement_result
{
    RECORDED_UNPROTECTED,
    DUPLICATE_UNPROTECTED,
    HANDED_TO_LIFETIME_CAPABILITY,
    STALE_GENERATION,
    REJECTED,
};

enum class Terminal_viewer_departure_action
{
    CLOSE,
    KEEP_ATTACHABLE,
};

enum class Terminal_viewer_departure_kind
{
    TRANSPORT_DEPARTURE,
    EXACT_PROCESS_DEATH,
};

enum class Terminal_viewer_departure_result
{
    CLOSE,
    KEEP_ATTACHABLE,
    STALE_GENERATION,
};

struct Terminal_custody_snapshot
{
    std::string session_identity;
    std::string launch_request_identity;
    std::uint64_t generation = 0U;
    Terminal_custody_state state = Terminal_custody_state::STARTING;
    std::optional<Terminal_close_cause> first_close_cause;
    bool framework_ready = false;
    std::optional<Terminal_child_fact> current_child_fact;
    std::uint64_t attachment_revision = 0U;
    bool attachment_live = false;
    std::uint64_t attachment_producer_process_id = 0U;
    std::string attachment_framebuffer_path;
    std::uint64_t attachment_store_generation = 0U;
};

struct Terminal_owner_atomic_snapshot
{
    std::uint64_t revision = 0U;
    std::vector<Terminal_custody_snapshot> custodies;
    std::vector<Terminal_settlement_receipt_key> unprotected_receipts;
};

class Terminal_lifetime_capability
{
public:
    virtual ~Terminal_lifetime_capability() = default;

    virtual Terminal_viewer_departure_action note_viewer_departure(
        Terminal_viewer_departure_kind kind,
        const Terminal_custody_snapshot& custody) = 0;
    virtual void accept_settlement(
        const Terminal_owner_settlement& settlement) = 0;
};

class Terminal_owner_core
{
public:
    using Time_point = Terminal_settlement_receipt_inbox::Time_point;

    Terminal_owner_core();
    explicit Terminal_owner_core(
        Terminal_settlement_receipt_inbox receipt_inbox);

    Terminal_owner_admission_result admit_custody(
        std::string session_identity,
        std::string launch_request_identity,
        std::uint64_t framework_generation,
        std::shared_ptr<Terminal_lifetime_capability> lifetime_capability = {});
    Terminal_owner_update_result note_framework_ready(
        const std::string& session_identity,
        std::uint64_t generation);
    Terminal_child_fact_acknowledgement ingest_child_fact(
        const Terminal_child_fact& fact);
    Terminal_owner_update_result request_close(
        const std::string& session_identity,
        std::uint64_t generation,
        Terminal_close_cause cause);
    Terminal_viewer_departure_result note_viewer_departure(
        const std::string& session_identity,
        std::uint64_t generation,
        Terminal_viewer_departure_kind kind);
    Terminal_owner_update_result apply_attachment(
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision,
        bool live,
        std::uint64_t producer_process_id = 0U,
        std::string framebuffer_path = {},
        std::uint64_t store_generation = 0U);
    [[nodiscard]] bool can_forward_input(
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision) const;
    Terminal_owner_settlement_result settle(
        Terminal_owner_settlement settlement,
        Time_point settled_at);

    [[nodiscard]] std::optional<Terminal_custody_snapshot> custody(
        const std::string& session_identity) const;
    [[nodiscard]] std::size_t live_custody_count() const;
    [[nodiscard]] std::vector<Terminal_custody_snapshot> custodies() const;
    [[nodiscard]] Terminal_owner_atomic_snapshot atomic_snapshot(
        Time_point now);

    bool contains_unprotected_receipt(
        const Terminal_settlement_receipt_key& key,
        Time_point now);
    bool acknowledge_unprotected_receipt(
        const Terminal_settlement_receipt_key& key,
        Time_point now);
    std::size_t unprotected_receipt_count(Time_point now);
    void purge_receipts_for_shutdown();

private:
    struct Live_custody
    {
        Terminal_custody_snapshot snapshot;
        std::shared_ptr<Terminal_lifetime_capability> lifetime_capability;
    };

    using Custody_map = std::map<std::string, Live_custody, std::less<>>;

    Custody_map::iterator find_current(
        const std::string& session_identity,
        std::uint64_t generation);
    Custody_map::const_iterator find_current(
        const std::string& session_identity,
        std::uint64_t generation) const;
    static void update_running_state(Live_custody& custody);
    static void propose_close(
        Live_custody& custody,
        Terminal_close_cause cause);
    void synchronize_receipt_revision();

    Custody_map m_custodies;
    Terminal_settlement_receipt_inbox m_receipt_inbox;
    std::uint64_t m_observed_receipt_revision = 0U;
    std::uint64_t m_snapshot_revision = 1U;
};

} // namespace vnm::terminal_workspace::detail
