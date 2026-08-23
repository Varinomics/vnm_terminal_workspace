#include "terminal_owner_core.h"

#include <utility>

namespace vnm::terminal_workspace::detail {

Terminal_owner_core::Terminal_owner_core() = default;

Terminal_owner_core::Terminal_owner_core(
    Terminal_settlement_receipt_inbox receipt_inbox)
:
    m_receipt_inbox(std::move(receipt_inbox))
{}

Terminal_owner_admission_result Terminal_owner_core::admit_custody(
    std::string session_identity,
    std::string launch_request_identity,
    std::uint64_t framework_generation,
    std::shared_ptr<Terminal_lifetime_capability> lifetime_capability)
{
    if (session_identity.empty()) {
        return Terminal_owner_admission_result::INVALID_SESSION_IDENTITY;
    }
    if (launch_request_identity.empty()) {
        return Terminal_owner_admission_result::INVALID_LAUNCH_REQUEST_IDENTITY;
    }
    if (framework_generation == 0U) {
        return Terminal_owner_admission_result::INVALID_GENERATION;
    }
    if (m_custodies.contains(session_identity)) {
        return Terminal_owner_admission_result::ALREADY_OWNED;
    }

    Terminal_custody_snapshot snapshot;
    snapshot.session_identity = session_identity;
    snapshot.launch_request_identity = std::move(launch_request_identity);
    snapshot.generation = framework_generation;
    m_custodies.emplace(
        std::move(session_identity),
        Live_custody{
            std::move(snapshot),
            std::move(lifetime_capability),
        });
    ++m_snapshot_revision;
    return Terminal_owner_admission_result::ADMITTED;
}

Terminal_owner_update_result Terminal_owner_core::note_framework_ready(
    const std::string& session_identity,
    std::uint64_t generation)
{
    const auto custody = find_current(session_identity, generation);
    if (custody == m_custodies.end()) {
        return Terminal_owner_update_result::STALE_GENERATION;
    }
    if (custody->second.snapshot.framework_ready) {
        return Terminal_owner_update_result::ALREADY_CURRENT;
    }
    custody->second.snapshot.framework_ready = true;
    update_running_state(custody->second);
    ++m_snapshot_revision;
    return Terminal_owner_update_result::APPLIED;
}

Terminal_child_fact_acknowledgement Terminal_owner_core::ingest_child_fact(
    const Terminal_child_fact& fact)
{
    if (fact.session_identity.empty() || fact.generation == 0U ||
        fact.fact_key == 0U || fact.sequence == 0U)
    {
        return Terminal_child_fact_acknowledgement::REJECTED;
    }

    const auto custody = find_current(fact.session_identity, fact.generation);
    if (custody == m_custodies.end()) {
        return Terminal_child_fact_acknowledgement::STALE_GENERATION;
    }

    const auto& current = custody->second.snapshot.current_child_fact;
    if (current) {
        if (*current == fact) {
            return Terminal_child_fact_acknowledgement::ALREADY_CURRENT;
        }
        if (fact.sequence <= current->sequence ||
            fact.fact_key == current->fact_key)
        {
            return Terminal_child_fact_acknowledgement::REJECTED;
        }
    }

    custody->second.snapshot.current_child_fact = fact;
    switch (fact.kind)
    {
    case Terminal_child_fact_kind::STARTED:
        update_running_state(custody->second);
        break;
    case Terminal_child_fact_kind::START_FAILED:
    case Terminal_child_fact_kind::START_INDETERMINATE:
    case Terminal_child_fact_kind::EXITED:
        break;
    }
    ++m_snapshot_revision;
    return Terminal_child_fact_acknowledgement::ACCEPTED;
}

Terminal_owner_update_result Terminal_owner_core::request_close(
    const std::string& session_identity,
    std::uint64_t generation,
    Terminal_close_cause cause)
{
    const auto custody = find_current(session_identity, generation);
    if (custody == m_custodies.end()) {
        return Terminal_owner_update_result::STALE_GENERATION;
    }
    if (custody->second.snapshot.first_close_cause) {
        return Terminal_owner_update_result::ALREADY_CURRENT;
    }
    propose_close(custody->second, cause);
    ++m_snapshot_revision;
    return Terminal_owner_update_result::APPLIED;
}

Terminal_viewer_departure_result Terminal_owner_core::note_viewer_departure(
    const std::string& session_identity,
    std::uint64_t generation,
    Terminal_viewer_departure_kind kind)
{
    const auto custody = find_current(session_identity, generation);
    if (custody == m_custodies.end()) {
        return Terminal_viewer_departure_result::STALE_GENERATION;
    }
    if (custody->second.lifetime_capability &&
        custody->second.lifetime_capability->note_viewer_departure(
            kind,
            custody->second.snapshot) ==
            Terminal_viewer_departure_action::KEEP_ATTACHABLE)
    {
        return Terminal_viewer_departure_result::KEEP_ATTACHABLE;
    }
    return Terminal_viewer_departure_result::CLOSE;
}

Terminal_owner_update_result Terminal_owner_core::apply_attachment(
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision,
    bool live,
    std::uint64_t producer_process_id,
    std::string framebuffer_path,
    std::uint64_t store_generation)
{
    const auto custody = find_current(session_identity, generation);
    if (custody == m_custodies.end()) {
        return Terminal_owner_update_result::STALE_GENERATION;
    }
    if (attachment_revision == 0U) {
        return Terminal_owner_update_result::REJECTED;
    }
    auto& snapshot = custody->second.snapshot;
    if (attachment_revision < snapshot.attachment_revision) {
        return Terminal_owner_update_result::STALE_GENERATION;
    }
    if (attachment_revision == snapshot.attachment_revision) {
        return snapshot.attachment_live == live
            ? Terminal_owner_update_result::ALREADY_CURRENT
            : Terminal_owner_update_result::REJECTED;
    }
    snapshot.attachment_revision = attachment_revision;
    snapshot.attachment_live = live;
    snapshot.attachment_producer_process_id = producer_process_id;
    snapshot.attachment_framebuffer_path = std::move(framebuffer_path);
    snapshot.attachment_store_generation = store_generation;
    ++m_snapshot_revision;
    return Terminal_owner_update_result::APPLIED;
}

bool Terminal_owner_core::can_forward_input(
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision) const
{
    const auto custody = find_current(session_identity, generation);
    if (custody == m_custodies.end()) {
        return false;
    }
    const auto& snapshot = custody->second.snapshot;
    return snapshot.state == Terminal_custody_state::RUNNING &&
        snapshot.attachment_live && attachment_revision != 0U &&
        snapshot.attachment_revision == attachment_revision;
}

Terminal_owner_settlement_result Terminal_owner_core::settle(
    Terminal_owner_settlement settlement,
    Time_point settled_at)
{
    if (settlement.session_identity.empty() || settlement.generation == 0U) {
        return Terminal_owner_settlement_result::REJECTED;
    }

    const Terminal_settlement_receipt_key key{
        settlement.session_identity,
        settlement.generation,
    };
    const auto custody = find_current(
        settlement.session_identity,
        settlement.generation);
    if (custody == m_custodies.end()) {
        const bool contains = m_receipt_inbox.contains(key, settled_at);
        synchronize_receipt_revision();
        return contains
            ? Terminal_owner_settlement_result::DUPLICATE_UNPROTECTED
            : Terminal_owner_settlement_result::STALE_GENERATION;
    }

    const std::shared_ptr<Terminal_lifetime_capability> lifetime_capability =
        custody->second.lifetime_capability;
    settlement.first_close_cause =
        custody->second.snapshot.first_close_cause;
    m_custodies.erase(custody);
    if (lifetime_capability) {
        lifetime_capability->accept_settlement(settlement);
        ++m_snapshot_revision;
        return Terminal_owner_settlement_result::HANDED_TO_LIFETIME_CAPABILITY;
    }

    const Settlement_receipt_ingest_result ingest =
        m_receipt_inbox.ingest(key, settled_at);
    synchronize_receipt_revision();
    if (ingest == Settlement_receipt_ingest_result::RECORDED) {
        return Terminal_owner_settlement_result::RECORDED_UNPROTECTED;
    }
    if (ingest == Settlement_receipt_ingest_result::DUPLICATE) {
        return Terminal_owner_settlement_result::DUPLICATE_UNPROTECTED;
    }
    return Terminal_owner_settlement_result::REJECTED;
}

std::optional<Terminal_custody_snapshot> Terminal_owner_core::custody(
    const std::string& session_identity) const
{
    const auto custody = m_custodies.find(session_identity);
    return custody == m_custodies.end()
        ? std::nullopt
        : std::optional<Terminal_custody_snapshot>(custody->second.snapshot);
}

std::size_t Terminal_owner_core::live_custody_count() const
{
    return m_custodies.size();
}

std::vector<Terminal_custody_snapshot> Terminal_owner_core::custodies() const
{
    std::vector<Terminal_custody_snapshot> snapshots;
    snapshots.reserve(m_custodies.size());
    for (const auto& [session_identity, custody] : m_custodies) {
        static_cast<void>(session_identity);
        snapshots.push_back(custody.snapshot);
    }
    return snapshots;
}

Terminal_owner_atomic_snapshot Terminal_owner_core::atomic_snapshot(
    Time_point now)
{
    std::vector<Terminal_settlement_receipt_key> receipt_keys =
        m_receipt_inbox.keys(now);
    synchronize_receipt_revision();
    return {
        m_snapshot_revision,
        custodies(),
        std::move(receipt_keys),
    };
}

bool Terminal_owner_core::contains_unprotected_receipt(
    const Terminal_settlement_receipt_key& key,
    Time_point now)
{
    const bool contains = m_receipt_inbox.contains(key, now);
    synchronize_receipt_revision();
    return contains;
}

bool Terminal_owner_core::acknowledge_unprotected_receipt(
    const Terminal_settlement_receipt_key& key,
    Time_point now)
{
    const bool acknowledged = m_receipt_inbox.acknowledge(key, now);
    synchronize_receipt_revision();
    return acknowledged;
}

std::size_t Terminal_owner_core::unprotected_receipt_count(Time_point now)
{
    const std::size_t count = m_receipt_inbox.size(now);
    synchronize_receipt_revision();
    return count;
}

void Terminal_owner_core::purge_receipts_for_shutdown()
{
    m_receipt_inbox.purge_for_shutdown();
    synchronize_receipt_revision();
}

void Terminal_owner_core::synchronize_receipt_revision()
{
    const std::uint64_t receipt_revision =
        m_receipt_inbox.mutation_revision();
    if (receipt_revision == m_observed_receipt_revision) {
        return;
    }
    m_observed_receipt_revision = receipt_revision;
    ++m_snapshot_revision;
}

Terminal_owner_core::Custody_map::iterator Terminal_owner_core::find_current(
    const std::string& session_identity,
    std::uint64_t generation)
{
    const auto custody = m_custodies.find(session_identity);
    return custody != m_custodies.end() &&
        custody->second.snapshot.generation == generation
        ? custody
        : m_custodies.end();
}

Terminal_owner_core::Custody_map::const_iterator
Terminal_owner_core::find_current(
    const std::string& session_identity,
    std::uint64_t generation) const
{
    const auto custody = m_custodies.find(session_identity);
    return custody != m_custodies.end() &&
        custody->second.snapshot.generation == generation
        ? custody
        : m_custodies.end();
}

void Terminal_owner_core::update_running_state(Live_custody& custody)
{
    if (custody.snapshot.state != Terminal_custody_state::STARTING ||
        !custody.snapshot.framework_ready ||
        !custody.snapshot.current_child_fact ||
        custody.snapshot.current_child_fact->kind !=
            Terminal_child_fact_kind::STARTED)
    {
        return;
    }
    custody.snapshot.state = Terminal_custody_state::RUNNING;
}

void Terminal_owner_core::propose_close(
    Live_custody& custody,
    Terminal_close_cause cause)
{
    if (!custody.snapshot.first_close_cause) {
        custody.snapshot.first_close_cause = cause;
        custody.snapshot.state = Terminal_custody_state::CLOSING;
    }
}

} // namespace vnm::terminal_workspace::detail
