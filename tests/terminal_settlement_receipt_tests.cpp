#include "terminal_settlement_receipt_inbox.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace workspace = vnm::terminal_workspace;

namespace {

using namespace std::chrono_literals;

bool check(bool condition, std::string_view message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "FAIL: %.*s\n",
        static_cast<int>(message.size()), message.data());
    return false;
}

workspace::Terminal_settlement_receipt_key key(
    std::string session_identity,
    std::uint64_t generation = 1U)
{
    return {std::move(session_identity), generation};
}

bool production_capacity_and_retention_are_fixed()
{
    workspace::Terminal_settlement_receipt_inbox inbox;
    const auto settled_at = workspace::Terminal_settlement_receipt_inbox::
        Time_point{};

    bool ok = true;
    for (std::size_t index = 0U; index < 32U; ++index) {
        ok &= check(
            inbox.ingest(
                key("session-" + std::to_string(index)),
                settled_at) ==
                workspace::Settlement_receipt_ingest_result::RECORDED,
            "the production inbox must accept its fixed 32 receipts");
    }
    ok &= check(
        inbox.size(settled_at) == 32U,
        "the production inbox must retain exactly 32 unique receipts");

    ok &= check(
        inbox.ingest(key("session-32"), settled_at + 1min) ==
                workspace::Settlement_receipt_ingest_result::RECORDED &&
            !inbox.contains(key("session-0"), settled_at + 1min) &&
            inbox.contains(key("session-1"), settled_at + 1min),
        "capacity must evict the deterministic oldest-settled receipt");

    ok &= check(
        inbox.size(settled_at + 2min) == 1U &&
            inbox.contains(key("session-32"), settled_at + 2min),
        "production receipts must expire two monotonic minutes after settlement");
    ok &= check(
        inbox.size(settled_at + 3min) == 0U,
        "the later replacement receipt must retain its own first-settlement expiry");
    return ok;
}

bool duplicate_and_read_do_not_refresh_expiry()
{
    auto inbox = workspace::Terminal_settlement_receipt_inbox::
        create_for_testing(2U, 10min);
    const auto settled_at = workspace::Terminal_settlement_receipt_inbox::
        Time_point{};
    const auto receipt = key("session-a");

    bool ok = true;
    ok &= check(
        inbox.ingest(receipt, settled_at) ==
            workspace::Settlement_receipt_ingest_result::RECORDED,
        "first settlement must record one receipt");
    ok &= check(
        inbox.contains(receipt, settled_at + 9min),
        "reading a live receipt must find it without changing it");
    ok &= check(
        inbox.ingest(receipt, settled_at + 9min) ==
            workspace::Settlement_receipt_ingest_result::DUPLICATE,
        "duplicate settlement must coalesce by exact session and generation");
    ok &= check(
        !inbox.contains(receipt, settled_at + 10min),
        "reads and duplicate ingest must not refresh first-settlement expiry");
    return ok;
}

bool equal_time_eviction_uses_ingest_order()
{
    auto inbox = workspace::Terminal_settlement_receipt_inbox::
        create_for_testing(2U, 10min);
    const auto settled_at = workspace::Terminal_settlement_receipt_inbox::
        Time_point{};

    inbox.ingest(key("session-b"), settled_at);
    inbox.ingest(key("session-a"), settled_at);
    inbox.ingest(key("session-c"), settled_at);
    return check(
        !inbox.contains(key("session-b"), settled_at) &&
            inbox.contains(key("session-a"), settled_at) &&
            inbox.contains(key("session-c"), settled_at),
        "equal-time capacity eviction must remove the earliest ingest");
}

bool acknowledgement_is_exact_and_shutdown_purges()
{
    auto inbox = workspace::Terminal_settlement_receipt_inbox::
        create_for_testing(4U, 10min);
    const auto settled_at = workspace::Terminal_settlement_receipt_inbox::
        Time_point{};
    const auto first_generation = key("session-a", 1U);
    const auto second_generation = key("session-a", 2U);
    inbox.ingest(first_generation, settled_at);
    inbox.ingest(second_generation, settled_at);

    bool ok = true;
    ok &= check(
        inbox.acknowledge(first_generation, settled_at) &&
            !inbox.contains(first_generation, settled_at) &&
            inbox.contains(second_generation, settled_at),
        "acknowledgement must remove only the exact session and generation");
    ok &= check(
        !inbox.acknowledge(first_generation, settled_at),
        "acknowledgement must be idempotent after exact removal");
    inbox.purge_for_shutdown();
    ok &= check(
        inbox.size(settled_at) == 0U,
        "owner shutdown must purge every transient receipt");
    return ok;
}

bool invalid_identity_and_zero_generation_reject()
{
    auto inbox = workspace::Terminal_settlement_receipt_inbox::
        create_for_testing(2U, 10min);
    const auto settled_at = workspace::Terminal_settlement_receipt_inbox::
        Time_point{};

    bool ok = true;
    ok &= check(
        inbox.ingest(key(""), settled_at) ==
            workspace::Settlement_receipt_ingest_result::INVALID_SESSION_IDENTITY,
        "a receipt requires a stable session identity");
    ok &= check(
        inbox.ingest(key("session-a", 0U), settled_at) ==
            workspace::Settlement_receipt_ingest_result::INVALID_GENERATION,
        "a receipt requires a nonzero hosted generation");
    ok &= check(
        workspace::Terminal_settlement_receipt_inbox::create_for_testing(
            0U,
            10min).ingest(key("session-a"), settled_at) ==
            workspace::Settlement_receipt_ingest_result::CAPACITY_UNAVAILABLE,
        "a zero-capacity test inbox must retain no receipt");
    return ok;
}

bool owner_instances_are_isolated()
{
    auto first_owner = workspace::Terminal_settlement_receipt_inbox::
        create_for_testing(1U, 10min);
    auto second_owner = workspace::Terminal_settlement_receipt_inbox::
        create_for_testing(1U, 10min);
    const auto settled_at = workspace::Terminal_settlement_receipt_inbox::
        Time_point{};
    const auto receipt = key("shared-session");
    first_owner.ingest(receipt, settled_at);

    return check(
        first_owner.contains(receipt, settled_at) &&
            !second_owner.contains(receipt, settled_at),
        "each runtime owner must retain an independent receipt inbox");
}

} // namespace

int main()
{
    bool ok = true;
    ok &= production_capacity_and_retention_are_fixed();
    ok &= duplicate_and_read_do_not_refresh_expiry();
    ok &= equal_time_eviction_uses_ingest_order();
    ok &= acknowledgement_is_exact_and_shutdown_purges();
    ok &= invalid_identity_and_zero_generation_reject();
    ok &= owner_instances_are_isolated();
    return ok ? 0 : 1;
}
