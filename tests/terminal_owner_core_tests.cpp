#include "terminal_owner_core.h"
#include "terminal_owner_proxy_gate.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string_view>
#include <utility>

namespace workspace = vnm::terminal_workspace;
namespace detail = vnm::terminal_workspace::detail;

namespace {

bool check(bool condition, std::string_view message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "FAIL: %.*s\n",
        static_cast<int>(message.size()), message.data());
    return false;
}

workspace::Terminal_child_fact fact(
    std::string session,
    std::uint64_t generation,
    std::uint64_t sequence,
    workspace::Terminal_child_fact_kind kind)
{
    workspace::Terminal_child_fact value;
    value.session_identity = std::move(session);
    value.generation = generation;
    value.fact_key = sequence;
    value.sequence = sequence;
    value.kind = kind;
    return value;
}

bool readiness_orders_converge_and_gate_input()
{
    bool ok = true;
    for (const bool ready_first : {false, true})
    {
        detail::Terminal_owner_core owner;
        ok &= check(
            owner.admit_custody("session-a", "launch-a", 41U) ==
                detail::Terminal_owner_admission_result::ADMITTED,
            "framework-supplied custody must admit once");
        if (ready_first) {
            owner.note_framework_ready("session-a", 41U);
        }
        ok &= check(
            owner.ingest_child_fact(fact(
                "session-a",
                41U,
                1U,
                workspace::Terminal_child_fact_kind::STARTED)) ==
                workspace::Terminal_child_fact_acknowledgement::ACCEPTED,
            "the current started fact must be accepted");
        if (!ready_first) {
            owner.note_framework_ready("session-a", 41U);
        }
        ok &= check(
            owner.apply_attachment("session-a", 41U, 7U, true) ==
                    detail::Terminal_owner_update_result::APPLIED &&
                owner.can_forward_input("session-a", 41U, 7U) &&
                !owner.can_forward_input("session-a", 41U, 6U),
            "input must require RUNNING and the exact live attachment revision");
    }
    return ok;
}

bool fact_dedup_and_first_close_cause_are_stable()
{
    detail::Terminal_owner_core owner;
    owner.admit_custody("session-a", "launch-a", 42U);
    const workspace::Terminal_child_fact started = fact(
        "session-a",
        42U,
        1U,
        workspace::Terminal_child_fact_kind::STARTED);
    bool ok = true;
    ok &= check(
        owner.ingest_child_fact(started) ==
                workspace::Terminal_child_fact_acknowledgement::ACCEPTED &&
            owner.ingest_child_fact(started) ==
                workspace::Terminal_child_fact_acknowledgement::ALREADY_CURRENT,
        "a lost reply must become already-current for the same stable fact");
    ok &= check(
        owner.ingest_child_fact(fact(
            "session-a",
            42U,
            2U,
            workspace::Terminal_child_fact_kind::EXITED)) ==
                workspace::Terminal_child_fact_acknowledgement::ACCEPTED &&
            owner.request_close(
                "session-a",
                42U,
                detail::Terminal_close_cause::CHILD_EXIT) ==
                detail::Terminal_owner_update_result::APPLIED &&
            owner.request_close(
                "session-a",
                42U,
                detail::Terminal_close_cause::EXPLICIT_CLOSE) ==
                detail::Terminal_owner_update_result::ALREADY_CURRENT,
        "an admitted child-exit close must remain the immutable first cause");
    const auto snapshot = owner.custody("session-a");
    ok &= check(
        snapshot && snapshot->state == detail::Terminal_custody_state::CLOSING &&
            snapshot->first_close_cause ==
                detail::Terminal_close_cause::CHILD_EXIT,
        "the first close cause must remain immutable");
    ok &= check(
        owner.ingest_child_fact(fact(
            "session-a",
            43U,
            1U,
            workspace::Terminal_child_fact_kind::STARTED)) ==
            workspace::Terminal_child_fact_acknowledgement::STALE_GENERATION,
        "a different generation must be acknowledged stale");
    return ok;
}

class Keeping_lifetime_capability final :
    public detail::Terminal_lifetime_capability
{
public:
    detail::Terminal_viewer_departure_action note_viewer_departure(
        detail::Terminal_viewer_departure_kind kind,
        const detail::Terminal_custody_snapshot&) override
    {
        ++departure_count;
        last_departure_kind = kind;
        return detail::Terminal_viewer_departure_action::KEEP_ATTACHABLE;
    }

    void accept_settlement(
        const detail::Terminal_owner_settlement& value) override
    {
        ++settlement_count;
        settlement = value;
    }

    int departure_count = 0;
    int settlement_count = 0;
    detail::Terminal_viewer_departure_kind last_departure_kind =
        detail::Terminal_viewer_departure_kind::TRANSPORT_DEPARTURE;
    std::optional<detail::Terminal_owner_settlement> settlement;
};

bool lifetime_hook_is_orthogonal_to_custody()
{
    detail::Terminal_owner_core owner;
    const auto capability = std::make_shared<Keeping_lifetime_capability>();
    owner.admit_custody("protected", "launch-protected", 51U, capability);
    bool ok = true;
    ok &= check(
        owner.note_viewer_departure(
            "protected",
            51U,
            detail::Terminal_viewer_departure_kind::EXACT_PROCESS_DEATH) ==
                detail::Terminal_viewer_departure_result::KEEP_ATTACHABLE &&
            owner.custody("protected")->state ==
                detail::Terminal_custody_state::STARTING &&
            capability->departure_count == 1 &&
            capability->last_departure_kind ==
                detail::Terminal_viewer_departure_kind::EXACT_PROCESS_DEATH,
        "a bound lifetime capability may keep the same custody attachable");
    const detail::Terminal_owner_settlement settlement{
        "protected",
        51U,
        std::nullopt,
        detail::Terminal_cleanup_disposition::COMPLETE,
        0,
    };
    ok &= check(
        owner.settle(settlement, detail::Terminal_owner_core::Time_point{}) ==
                detail::Terminal_owner_settlement_result::
                    HANDED_TO_LIFETIME_CAPABILITY &&
            owner.live_custody_count() == 0U &&
            capability->settlement_count == 1 &&
            capability->settlement == settlement,
        "protected settlement must remove live custody and use only the hook");
    return ok;
}

bool unprotected_departure_settlement_and_receipt_are_bounded()
{
    using namespace std::chrono_literals;
    detail::Terminal_owner_core owner(
        workspace::Terminal_settlement_receipt_inbox::create_for_testing(
            2U,
            2min));
    const auto origin = detail::Terminal_owner_core::Time_point{};
    owner.admit_custody("session-a", "launch-a", 61U);
    bool ok = true;
    ok &= check(
        owner.note_viewer_departure(
            "session-a",
            61U,
            detail::Terminal_viewer_departure_kind::TRANSPORT_DEPARTURE) ==
                detail::Terminal_viewer_departure_result::CLOSE &&
            owner.request_close(
                "session-a",
                61U,
                detail::Terminal_close_cause::VIEWER_DEPARTURE) ==
                detail::Terminal_owner_update_result::APPLIED &&
            owner.custody("session-a")->first_close_cause ==
                detail::Terminal_close_cause::VIEWER_DEPARTURE,
        "an unprotected viewer departure must select the close path");
    const detail::Terminal_owner_settlement settlement{
        "session-a",
        61U,
        std::nullopt,
        detail::Terminal_cleanup_disposition::NOT_REQUIRED,
        17,
    };
    const workspace::Terminal_settlement_receipt_key key{"session-a", 61U};
    ok &= check(
        owner.settle(settlement, origin) ==
                detail::Terminal_owner_settlement_result::RECORDED_UNPROTECTED &&
            owner.live_custody_count() == 0U &&
            owner.contains_unprotected_receipt(key, origin + 1min),
        "unprotected settlement must atomically remove custody and add one receipt");
    ok &= check(
        owner.settle(settlement, origin + 1min) ==
            detail::Terminal_owner_settlement_result::DUPLICATE_UNPROTECTED,
        "duplicate settlement must coalesce by session and generation");
    const detail::Terminal_owner_atomic_snapshot settled_snapshot =
        owner.atomic_snapshot(origin + 1min);
    ok &= check(
        !owner.contains_unprotected_receipt(key, origin + 2min) &&
            owner.atomic_snapshot(origin + 2min).revision >
                settled_snapshot.revision,
        "duplicate settlement and reads must not refresh first-settlement expiry");
    owner.purge_receipts_for_shutdown();
    ok &= check(
        owner.unprotected_receipt_count(origin + 2min) == 0U,
        "shutdown must purge the neutral receipt inbox");
    return ok;
}

bool multi_session_and_attachment_updates_are_isolated()
{
    detail::Terminal_owner_core owner;
    owner.admit_custody("session-a", "launch-a", 71U);
    owner.admit_custody("session-b", "launch-b", 72U);
    owner.note_framework_ready("session-a", 71U);
    owner.ingest_child_fact(fact(
        "session-a",
        71U,
        1U,
        workspace::Terminal_child_fact_kind::STARTED));
    owner.apply_attachment("session-a", 71U, 3U, true);

    bool ok = true;
    ok &= check(
        owner.live_custody_count() == 2U &&
            owner.can_forward_input("session-a", 71U, 3U) &&
            !owner.can_forward_input("session-b", 72U, 3U),
        "one session's readiness and attachment must not enable another");
    ok &= check(
        owner.apply_attachment("session-a", 71U, 2U, true) ==
                detail::Terminal_owner_update_result::STALE_GENERATION &&
            owner.apply_attachment("session-a", 71U, 3U, false) ==
                detail::Terminal_owner_update_result::REJECTED,
        "stale or contradictory attachment revisions must not replace current state");
    return ok;
}

bool framework_authority_gates_attach_existing_and_input()
{
    detail::Terminal_owner_core owner;
    const auto capability = std::make_shared<Keeping_lifetime_capability>();
    owner.admit_custody("protected", "launch-protected", 81U, capability);
    owner.note_framework_ready("protected", 81U);
    owner.ingest_child_fact(fact(
        "protected",
        81U,
        1U,
        workspace::Terminal_child_fact_kind::STARTED));
    owner.apply_attachment("protected", 81U, 9U, true);

    detail::Terminal_owner_proxy_gate proxy(owner);
    const vnm::VNM_viewer_identity first_viewer{101U, 201U, 301U};
    const vnm::VNM_viewer_identity second_viewer{102U, 202U, 302U};
    bool ok = true;
    ok &= check(
        proxy.bind_initial_viewer(first_viewer) ==
            vnm::VNM_viewer_bind_outcome::BOUND,
        "the proxy must use the framework viewer-authority bind");
    const vnm::VNM_viewer_authority_epoch first_epoch =
        proxy.authority_snapshot().epoch;
    int send_count = 0;
    ok &= check(
        proxy.attach_existing(
            first_viewer.transport_process_id,
            first_epoch,
            "protected",
            81U,
            9U) == detail::Terminal_proxy_gate_outcome::ADMITTED &&
            proxy.forward_input(
                first_viewer.transport_process_id,
                first_epoch,
                "protected",
                81U,
                9U,
                [&send_count]() {
                    ++send_count;
                }) == detail::Terminal_proxy_gate_outcome::ADMITTED &&
            send_count == 1,
        "attach-existing and input must require current authority and attachment");
    ok &= check(
        proxy.attach_existing(
            first_viewer.transport_process_id,
            first_epoch,
            "protected",
            81U,
            8U) == detail::Terminal_proxy_gate_outcome::STALE_ATTACHMENT,
        "attach-existing must reject a stale attachment revision");
    ok &= check(
        proxy.note_transport_departure(first_viewer) ==
                vnm::VNM_viewer_transport_departure_outcome::RECORDED &&
            owner.custody("protected")->state ==
                detail::Terminal_custody_state::RUNNING &&
            capability->last_departure_kind ==
                detail::Terminal_viewer_departure_kind::TRANSPORT_DEPARTURE,
        "transport departure must remain distinct and use the lifetime hook");
    const vnm::VNM_viewer_process_death_result death =
        proxy.observe_exact_process_death(
            first_viewer.native_process_id,
            first_viewer.native_process_creation_identity);
    ok &= check(
        death.outcome == vnm::VNM_viewer_process_death_outcome::REVOKED &&
            capability->last_departure_kind ==
                detail::Terminal_viewer_departure_kind::EXACT_PROCESS_DEATH &&
            proxy.forward_input(
                first_viewer.transport_process_id,
                first_epoch,
                "protected",
                81U,
                9U,
                []() {}) ==
                detail::Terminal_proxy_gate_outcome::AUTHORITY_REJECTED,
        "exact death must revoke the old epoch without closing protected custody");
    ok &= check(
        proxy.wait_until_epoch_drained(
                death.revoked_epoch,
                std::chrono::steady_clock::now() + std::chrono::seconds(5)) ==
                vnm::VNM_viewer_drain_outcome::DRAINED &&
            proxy.begin_rebind(second_viewer) ==
                vnm::VNM_viewer_bind_outcome::BOUND &&
            proxy.commit_rebind(second_viewer) ==
                vnm::VNM_viewer_rebind_transition_outcome::COMPLETED,
        "a successor may bind only after the exact old epoch drains");
    const auto rebound = proxy.authority_snapshot();
    ok &= check(
        rebound.epoch > first_epoch &&
            proxy.attach_existing(
                second_viewer.transport_process_id,
                rebound.epoch,
                "protected",
                81U,
                9U) == detail::Terminal_proxy_gate_outcome::ADMITTED &&
            owner.custody("protected")->generation == 81U &&
            owner.custody("protected")->attachment_revision == 9U,
        "rebound attach-existing must preserve generation and attachment identity");
    return ok;
}

bool unprotected_departure_admits_one_hosted_close()
{
    detail::Terminal_owner_core owner;
    int close_count = 0;
    detail::Terminal_owner_proxy_gate proxy(
        owner,
        [&close_count, &owner](const std::string& session_identity,
                              std::uint64_t generation,
                              detail::Terminal_close_cause cause) {
            if (session_identity == "plain" && generation == 91U) {
                ++close_count;
            }
            return cause == detail::Terminal_close_cause::VIEWER_DEPARTURE
                ? owner.request_close(session_identity, generation, cause)
                : detail::Terminal_owner_update_result::REJECTED;
        });
    owner.admit_custody("plain", "launch-plain", 91U);
    const vnm::VNM_viewer_identity viewer{111U, 211U, 311U};
    static_cast<void>(proxy.bind_initial_viewer(viewer));

    bool ok = true;
    ok &= check(
        proxy.note_transport_departure(viewer) ==
                vnm::VNM_viewer_transport_departure_outcome::RECORDED &&
            close_count == 1 &&
            owner.custody("plain")->first_close_cause ==
                detail::Terminal_close_cause::VIEWER_DEPARTURE,
        "unprotected departure must admit the hosted close exactly once");
    ok &= check(
        proxy.note_transport_departure(viewer) ==
                vnm::VNM_viewer_transport_departure_outcome::ALREADY_RECORDED &&
            close_count == 1,
        "duplicate departure must not admit another hosted close");
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= readiness_orders_converge_and_gate_input();
    ok &= fact_dedup_and_first_close_cause_are_stable();
    ok &= lifetime_hook_is_orthogonal_to_custody();
    ok &= unprotected_departure_settlement_and_receipt_are_bounded();
    ok &= multi_session_and_attachment_updates_are_isolated();
    ok &= framework_authority_gates_attach_existing_and_input();
    ok &= unprotected_departure_admits_one_hosted_close();
    return ok ? 0 : 1;
}
