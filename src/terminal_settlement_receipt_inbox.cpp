#include "terminal_settlement_receipt_inbox.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace vnm::terminal_workspace {
namespace {

constexpr std::size_t k_production_maximum_receipts = 32U;
constexpr auto k_production_retention = std::chrono::minutes(2);

} // namespace

Terminal_settlement_receipt_inbox::Terminal_settlement_receipt_inbox()
:
    Terminal_settlement_receipt_inbox(
        k_production_maximum_receipts,
        k_production_retention)
{}

Terminal_settlement_receipt_inbox
Terminal_settlement_receipt_inbox::create_for_testing(
    std::size_t maximum_receipts,
    Duration retention)
{
    return Terminal_settlement_receipt_inbox(maximum_receipts, retention);
}

Settlement_receipt_ingest_result Terminal_settlement_receipt_inbox::ingest(
    Terminal_settlement_receipt_key key,
    Time_point settled_at)
{
    expire(settled_at);
    if (key.session_identity.empty()) {
        return Settlement_receipt_ingest_result::INVALID_SESSION_IDENTITY;
    }
    if (key.generation == 0U) {
        return Settlement_receipt_ingest_result::INVALID_GENERATION;
    }
    if (m_maximum_receipts == 0U) {
        return Settlement_receipt_ingest_result::CAPACITY_UNAVAILABLE;
    }

    const auto duplicate = std::find_if(
        m_entries.begin(),
        m_entries.end(),
        [&key](const Entry& entry) {
            return entry.key == key;
        });
    if (duplicate != m_entries.end()) {
        return Settlement_receipt_ingest_result::DUPLICATE;
    }

    if (m_entries.size() == m_maximum_receipts) {
        m_entries.erase(std::min_element(
            m_entries.begin(),
            m_entries.end(),
            [](const Entry& left, const Entry& right) {
                if (left.settled_at != right.settled_at) {
                    return left.settled_at < right.settled_at;
                }
                return left.insertion_sequence < right.insertion_sequence;
            }));
    }
    m_entries.push_back({
        std::move(key),
        settled_at,
        m_next_insertion_sequence++,
    });
    return Settlement_receipt_ingest_result::RECORDED;
}

bool Terminal_settlement_receipt_inbox::contains(
    const Terminal_settlement_receipt_key& key,
    Time_point now)
{
    expire(now);
    return std::any_of(
        m_entries.begin(),
        m_entries.end(),
        [&key](const Entry& entry) {
            return entry.key == key;
        });
}

bool Terminal_settlement_receipt_inbox::acknowledge(
    const Terminal_settlement_receipt_key& key,
    Time_point now)
{
    expire(now);
    const auto entry = std::find_if(
        m_entries.begin(),
        m_entries.end(),
        [&key](const Entry& candidate) {
            return candidate.key == key;
        });
    if (entry == m_entries.end()) {
        return false;
    }
    m_entries.erase(entry);
    return true;
}

std::size_t Terminal_settlement_receipt_inbox::size(Time_point now)
{
    expire(now);
    return m_entries.size();
}

void Terminal_settlement_receipt_inbox::purge_for_shutdown()
{
    m_entries.clear();
}

Terminal_settlement_receipt_inbox::Terminal_settlement_receipt_inbox(
    std::size_t maximum_receipts,
    Duration retention)
:
    m_maximum_receipts(maximum_receipts),
    m_retention(retention)
{}

void Terminal_settlement_receipt_inbox::expire(Time_point now)
{
    std::erase_if(
        m_entries,
        [now, retention = m_retention](const Entry& entry) {
            return now >= entry.settled_at &&
                now - entry.settled_at >= retention;
        });
}

} // namespace vnm::terminal_workspace
