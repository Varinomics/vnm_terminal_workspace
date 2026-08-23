#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vnm::terminal_workspace {

struct Terminal_settlement_receipt_key
{
    std::string session_identity;
    std::uint64_t generation = 0U;

    friend bool operator==(
        const Terminal_settlement_receipt_key&,
        const Terminal_settlement_receipt_key&) = default;
};

enum class Settlement_receipt_ingest_result
{
    RECORDED,
    DUPLICATE,
    INVALID_SESSION_IDENTITY,
    INVALID_GENERATION,
    CAPACITY_UNAVAILABLE,
};

class Terminal_settlement_receipt_inbox
{
public:
    using Clock = std::chrono::steady_clock;
    using Time_point = Clock::time_point;
    using Duration = Clock::duration;

    Terminal_settlement_receipt_inbox();

    static Terminal_settlement_receipt_inbox create_for_testing(
        std::size_t maximum_receipts,
        Duration retention);

    Settlement_receipt_ingest_result ingest(
        Terminal_settlement_receipt_key key,
        Time_point settled_at);
    bool contains(
        const Terminal_settlement_receipt_key& key,
        Time_point now);
    bool acknowledge(
        const Terminal_settlement_receipt_key& key,
        Time_point now);
    std::vector<Terminal_settlement_receipt_key> keys(Time_point now);
    std::size_t size(Time_point now);
    void purge_for_shutdown();
    [[nodiscard]] std::uint64_t mutation_revision() const;

private:
    struct Entry
    {
        Terminal_settlement_receipt_key key;
        Time_point settled_at;
        std::uint64_t insertion_sequence = 0U;
    };

    Terminal_settlement_receipt_inbox(
        std::size_t maximum_receipts,
        Duration retention);

    void expire(Time_point now);

    std::size_t m_maximum_receipts;
    Duration m_retention;
    std::uint64_t m_next_insertion_sequence = 0U;
    std::uint64_t m_mutation_revision = 0U;
    std::vector<Entry> m_entries;
};

} // namespace vnm::terminal_workspace
