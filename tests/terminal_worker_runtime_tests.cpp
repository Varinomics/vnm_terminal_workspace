#include "terminal_worker_runtime_internal.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace workspace = vnm::terminal_workspace;
namespace runtime_detail = vnm::terminal_workspace::detail;
namespace environment = vnm::environment_policy;

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

workspace::Terminal_launch_request request(bool cancelled = false)
{
    workspace::Terminal_launch_request value;
    value.launch_request_id = "launch-a";
    value.session_id = "session-a";
    value.argv = {"/bin/shell", "argument"};
    value.working_directory = "/workspace";
    value.base_environment_complete = true;
    value.base_environment = {{"PATH", "/bin"}, {"ORDINARY", "value"}};
    value.cancellation.identity = "cancel-a";
    value.cancellation.requested = cancelled;
    return value;
}

std::vector<std::uint8_t> payload(bool cancelled = false)
{
    return workspace::prepare_terminal_launch_request(
        request(cancelled),
        workspace::Launch_platform::POSIX).serialized_request;
}

class Fake_adapter final : public runtime_detail::Terminal_worker_runtime_adapter
{
public:
    enum class Failed_stage
    {
        NONE,
        REMOTE_RUNTIME,
        ROOT,
        SETTINGS,
        SURFACE,
        SCROLLBAR,
        REMOTE_UI,
        TIMESTAMPS,
    };

    bool initialize_remote_runtime() override
    {
        ++remote_runtime_calls;
        return failed_stage != Failed_stage::REMOTE_RUNTIME;
    }

    bool initialize_root() override
    {
        ++root_calls;
        return failed_stage != Failed_stage::ROOT;
    }

    bool construct_terminal_settings() override
    {
        ++settings_calls;
        return failed_stage != Failed_stage::SETTINGS;
    }

    bool construct_terminal_surface() override
    {
        ++surface_calls;
        return failed_stage != Failed_stage::SURFACE;
    }

    bool construct_terminal_scrollbar() override
    {
        ++scrollbar_calls;
        return failed_stage != Failed_stage::SCROLLBAR;
    }

    bool connect_remote_ui() override
    {
        ++remote_ui_calls;
        return failed_stage != Failed_stage::REMOTE_UI;
    }

    bool connect_row_timestamps() override
    {
        ++timestamp_calls;
        return failed_stage != Failed_stage::TIMESTAMPS;
    }

    runtime_detail::Structured_start_result start_terminal(
        const runtime_detail::Terminal_worker_start_projection& value) override
    {
        ++start_calls;
        projection = value;
        return start_result;
    }

    runtime_detail::Terminal_worker_exit_observation observe_exit() override
    {
        ++exit_calls;
        return exit_observation;
    }

    bool observe_running() override
    {
        ++running_calls;
        return running_observed;
    }

    void terminate_hosted_worker() override
    {
        ++termination_calls;
    }

    Failed_stage failed_stage = Failed_stage::NONE;
    runtime_detail::Structured_start_result start_result{true, true};
    bool running_observed = true;
    runtime_detail::Terminal_worker_exit_observation exit_observation{
        true,
        17,
    };
    std::optional<runtime_detail::Terminal_worker_start_projection> projection;
    int remote_runtime_calls = 0;
    int root_calls = 0;
    int settings_calls = 0;
    int surface_calls = 0;
    int scrollbar_calls = 0;
    int remote_ui_calls = 0;
    int timestamp_calls = 0;
    int start_calls = 0;
    int running_calls = 0;
    int exit_calls = 0;
    int termination_calls = 0;
};

class Scripted_transport final : public workspace::Terminal_child_fact_transport
{
public:
    workspace::Terminal_child_fact_delivery_result deliver(
        const workspace::Terminal_child_fact& fact) override
    {
        delivered.push_back(fact);
        if (next_result < results.size()) {
            return results[next_result++];
        }
        return {
            workspace::Terminal_child_fact_delivery_status::DELIVERED,
            workspace::Terminal_child_fact_acknowledgement::ACCEPTED,
        };
    }

    std::vector<workspace::Terminal_child_fact_delivery_result> results;
    std::vector<workspace::Terminal_child_fact> delivered;
    std::size_t next_result = 0U;
};

bool successful_run_projects_once_and_publishes_monotonic_facts()
{
    Fake_adapter adapter;
    Scripted_transport transport;
    runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
    std::optional<std::vector<environment::Environment_entry>> authorized =
        std::vector<environment::Environment_entry>{{"PRODUCT_ALLOWED", "secret"}};

    const workspace::Terminal_worker_run_result result = runtime.run(
        payload(),
        workspace::Launch_platform::POSIX,
        41U,
        {},
        authorized);

    bool ok = true;
    ok &= check(
        result == workspace::Terminal_worker_run_result::COMPLETED &&
            adapter.start_calls == 1 && adapter.running_calls == 1 &&
            adapter.exit_calls == 1 &&
            adapter.termination_calls == 1,
        "a successful generation must start once, observe exit, and terminate host");
    ok &= check(
        adapter.remote_runtime_calls == 1 && adapter.root_calls == 1 &&
            adapter.settings_calls == 1 && adapter.surface_calls == 1 &&
            adapter.scrollbar_calls == 1 && adapter.remote_ui_calls == 1 &&
            adapter.timestamp_calls == 1,
        "the neutral runtime must drive every explicit construction adapter once");
    ok &= check(
        adapter.projection && adapter.projection->argv == request().argv &&
            adapter.projection->working_directory == request().working_directory &&
            adapter.projection->base_environment == request().base_environment &&
            adapter.projection->authorized_environment == authorized,
        "surface projection must preserve the base and separate authorized entries");
    ok &= check(
        transport.delivered.size() == 2U &&
            transport.delivered[0].kind ==
                workspace::Terminal_child_fact_kind::STARTED &&
            transport.delivered[1].kind ==
                workspace::Terminal_child_fact_kind::EXITED &&
            transport.delivered[0].generation == 41U &&
            transport.delivered[0].sequence == 1U &&
            transport.delivered[1].sequence == 2U &&
            transport.delivered[1].exit_code == 17,
        "started and exited facts must carry one hosted generation and monotonic keys");
    ok &= check(
        runtime.current_fact() == transport.delivered.back() &&
            runtime.unacknowledged_facts().empty(),
        "accepted ingest acknowledgements must retain only the current fact");
    return ok;
}

bool reply_loss_replays_same_fact_without_native_retry()
{
    Fake_adapter adapter;
    Scripted_transport transport;
    transport.results = {
        {workspace::Terminal_child_fact_delivery_status::REPLY_LOST, std::nullopt},
        {workspace::Terminal_child_fact_delivery_status::DELIVERED,
            workspace::Terminal_child_fact_acknowledgement::ALREADY_CURRENT},
    };
    runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
    const auto result = runtime.run(
        payload(), workspace::Launch_platform::POSIX, 42U);

    return check(
        result == workspace::Terminal_worker_run_result::COMPLETED &&
            adapter.start_calls == 1 && transport.delivered.size() == 3U &&
            transport.delivered[0] == transport.delivered[1],
        "lost reply must replay the stable fact without another structured start");
}

bool cancellation_and_invalid_envelopes_never_construct_surface()
{
    bool ok = true;
    {
        Fake_adapter adapter;
        Scripted_transport transport;
        runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
        ok &= check(
            runtime.run(payload(true), workspace::Launch_platform::POSIX, 43U) ==
                    workspace::Terminal_worker_run_result::TERMINATED &&
                adapter.remote_runtime_calls == 0 && adapter.start_calls == 0 &&
                adapter.termination_calls == 1 &&
                transport.delivered.size() == 1U &&
                transport.delivered.front().error ==
                    workspace::Terminal_child_fact_error::CANCELLED,
            "pre-custody cancellation must totalize without construction");
    }
    {
        Fake_adapter adapter;
        Scripted_transport transport;
        runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
        const std::vector<std::uint8_t> malformed{1U, 2U, 3U};
        ok &= check(
            runtime.run(malformed, workspace::Launch_platform::POSIX, 44U) ==
                    workspace::Terminal_worker_run_result::TERMINATED &&
                adapter.start_calls == 0 && adapter.termination_calls == 1 &&
                transport.delivered.empty(),
            "an undecodable envelope must terminate without inventing a session fact");
    }
    return ok;
}

bool worker_revalidates_product_reserved_base_names()
{
    workspace::Terminal_launch_request value = request();
    value.base_environment.push_back({"PRODUCT_RESERVED", "must-not-pass"});
    const workspace::Launch_request_result prepared =
        workspace::prepare_terminal_launch_request(
            std::move(value),
            workspace::Launch_platform::POSIX);
    Fake_adapter adapter;
    Scripted_transport transport;
    runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
    const std::string_view additional_reserved[] = {"PRODUCT_RESERVED"};

    const auto result = runtime.run(
        prepared.serialized_request,
        workspace::Launch_platform::POSIX,
        55U,
        additional_reserved);

    return check(
        prepared.status == workspace::Launch_request_status::ACCEPTED &&
            result == workspace::Terminal_worker_run_result::TERMINATED &&
            adapter.remote_runtime_calls == 0 && adapter.start_calls == 0 &&
            adapter.termination_calls == 1 && transport.delivered.empty(),
        "the worker must revalidate the decoded base with product reservations");
}

bool every_construction_failure_totalizes()
{
    bool ok = true;
    const std::vector<std::pair<
        Fake_adapter::Failed_stage,
        workspace::Terminal_child_fact_error>> cases{
        {Fake_adapter::Failed_stage::REMOTE_RUNTIME,
            workspace::Terminal_child_fact_error::REMOTE_RUNTIME_INITIALIZATION},
        {Fake_adapter::Failed_stage::ROOT,
            workspace::Terminal_child_fact_error::ROOT_INITIALIZATION},
        {Fake_adapter::Failed_stage::SETTINGS,
            workspace::Terminal_child_fact_error::SETTINGS_CONSTRUCTION},
        {Fake_adapter::Failed_stage::SURFACE,
            workspace::Terminal_child_fact_error::SURFACE_CONSTRUCTION},
        {Fake_adapter::Failed_stage::SCROLLBAR,
            workspace::Terminal_child_fact_error::SCROLLBAR_CONSTRUCTION},
        {Fake_adapter::Failed_stage::REMOTE_UI,
            workspace::Terminal_child_fact_error::REMOTE_UI_CONNECTION},
        {Fake_adapter::Failed_stage::TIMESTAMPS,
            workspace::Terminal_child_fact_error::TIMESTAMP_CONNECTION},
    };
    for (const auto& [stage, expected_error] : cases)
    {
        Fake_adapter adapter;
        adapter.failed_stage = stage;
        Scripted_transport transport;
        runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
        const auto result = runtime.run(
            payload(), workspace::Launch_platform::POSIX, 45U);
        ok &= check(
            result == workspace::Terminal_worker_run_result::TERMINATED &&
                adapter.start_calls == 0 && adapter.termination_calls == 1 &&
                transport.delivered.size() == 1U &&
                transport.delivered.front().kind ==
                    workspace::Terminal_child_fact_kind::START_FAILED &&
                transport.delivered.front().error == expected_error,
            "each construction failure must publish once and terminate");
    }
    return ok;
}

bool structured_failure_and_indeterminacy_never_retry()
{
    bool ok = true;
    for (const runtime_detail::Structured_start_result start_result : {
             runtime_detail::Structured_start_result{false, false,
                 runtime_detail::Structured_start_determinacy::DETERMINATE},
             runtime_detail::Structured_start_result{false, true,
                 runtime_detail::Structured_start_determinacy::INDETERMINATE},
         })
    {
        Fake_adapter adapter;
        adapter.start_result = start_result;
        Scripted_transport transport;
        runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
        runtime.run(payload(), workspace::Launch_platform::POSIX, 46U);
        ok &= check(
            adapter.start_calls == 1 && adapter.exit_calls == 0 &&
                adapter.termination_calls == 1 && transport.delivered.size() == 1U &&
                transport.delivered.front().native_dispatch_occurred ==
                    start_result.native_dispatch_occurred,
            "structured failure or indeterminacy must publish without retry");
    }
    return ok;
}

bool running_failure_totalizes_without_exit_wait()
{
    Fake_adapter adapter;
    adapter.running_observed = false;
    Scripted_transport transport;
    runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);

    const auto result = runtime.run(
        payload(), workspace::Launch_platform::POSIX, 52U);

    return check(
        result == workspace::Terminal_worker_run_result::TERMINATED &&
            adapter.start_calls == 1 && adapter.running_calls == 1 &&
            adapter.exit_calls == 0 && adapter.termination_calls == 1 &&
            transport.delivered.size() == 1U &&
            transport.delivered.front().kind ==
                workspace::Terminal_child_fact_kind::START_FAILED &&
            transport.delivered.front().error ==
                workspace::Terminal_child_fact_error::RUNNING_OBSERVATION,
        "an unobserved running transition must publish a typed failure and terminate");
}

bool unavailable_or_rejected_delivery_terminates_boundedly()
{
    bool ok = true;
    {
        Fake_adapter adapter;
        Scripted_transport transport;
        transport.results.assign(3U, {
            workspace::Terminal_child_fact_delivery_status::BACKPRESSURE,
            std::nullopt,
        });
        runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
        runtime.run(payload(), workspace::Launch_platform::POSIX, 47U);
        const bool replayed = runtime.replay_unacknowledged();
        ok &= check(
            replayed && transport.delivered.size() == 4U &&
                transport.delivered[0] == transport.delivered[3] &&
                adapter.start_calls == 1 &&
                adapter.exit_calls == 0 && adapter.termination_calls == 1 &&
                runtime.current_fact() == transport.delivered.front() &&
                runtime.unacknowledged_facts().empty(),
            "bounded backpressure must retain and replay one stable fact");
    }
    {
        Fake_adapter adapter;
        Scripted_transport transport;
        transport.results = {{
            workspace::Terminal_child_fact_delivery_status::DELIVERED,
            workspace::Terminal_child_fact_acknowledgement::REJECTED,
        }};
        runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
        runtime.run(payload(), workspace::Launch_platform::POSIX, 48U);
        ok &= check(
            transport.delivered.size() == 1U && adapter.termination_calls == 1 &&
                runtime.unacknowledged_facts().empty(),
            "typed rejection must settle the pending delivery and terminate");
    }
    return ok;
}

bool every_transient_delivery_status_is_bounded()
{
    bool ok = true;
    for (const workspace::Terminal_child_fact_delivery_status status : {
             workspace::Terminal_child_fact_delivery_status::HANDLER_UNAVAILABLE,
             workspace::Terminal_child_fact_delivery_status::BACKPRESSURE,
             workspace::Terminal_child_fact_delivery_status::REPLY_LOST,
             workspace::Terminal_child_fact_delivery_status::INDETERMINATE,
         })
    {
        Fake_adapter adapter;
        Scripted_transport transport;
        transport.results.assign(3U, {status, std::nullopt});
        runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
        runtime.run(payload(), workspace::Launch_platform::POSIX, 50U);
        ok &= check(
            transport.delivered.size() == 3U && adapter.start_calls == 1 &&
                adapter.termination_calls == 1 &&
                runtime.unacknowledged_facts().size() == 1U,
            "every transient delivery outcome must exhaust the same bounded replay");
    }
    return ok;
}

bool stale_generation_acknowledges_and_stops_active_worker()
{
    Fake_adapter adapter;
    Scripted_transport transport;
    transport.results = {{
        workspace::Terminal_child_fact_delivery_status::DELIVERED,
        workspace::Terminal_child_fact_acknowledgement::STALE_GENERATION,
    }};
    runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
    runtime.run(payload(), workspace::Launch_platform::POSIX, 51U);

    return check(
        transport.delivered.size() == 1U && adapter.start_calls == 1 &&
            adapter.exit_calls == 0 && adapter.termination_calls == 1 &&
            runtime.unacknowledged_facts().empty(),
        "a stale-generation acknowledgement must stop the active worker once");
}

bool lost_exit_and_zero_generation_terminate_safely()
{
    bool ok = true;
    {
        Fake_adapter adapter;
        adapter.exit_observation.observed = false;
        Scripted_transport transport;
        runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
        ok &= check(
            runtime.run(payload(), workspace::Launch_platform::POSIX, 49U) ==
                    workspace::Terminal_worker_run_result::TERMINATED &&
                adapter.start_calls == 1 && adapter.exit_calls == 1 &&
                adapter.termination_calls == 1 && transport.delivered.size() == 1U,
            "lost exit must terminate rather than leave the hosted worker alive");
    }
    {
        Fake_adapter adapter;
        Scripted_transport transport;
        runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
        ok &= check(
            runtime.run(payload(), workspace::Launch_platform::POSIX, 0U) ==
                    workspace::Terminal_worker_run_result::INVALID_GENERATION &&
                adapter.termination_calls == 1 && adapter.start_calls == 0,
            "the runtime must accept only a nonzero host-supplied generation");
    }
    return ok;
}

bool one_runtime_instance_never_starts_twice()
{
    Fake_adapter adapter;
    Scripted_transport transport;
    runtime_detail::Terminal_worker_coordinator runtime(adapter, transport);
    const auto first = runtime.run(
        payload(), workspace::Launch_platform::POSIX, 53U);
    const auto second = runtime.run(
        payload(), workspace::Launch_platform::POSIX, 54U);

    return check(
        first == workspace::Terminal_worker_run_result::COMPLETED &&
            second == workspace::Terminal_worker_run_result::ALREADY_STARTED &&
            adapter.start_calls == 1 && adapter.termination_calls == 1 &&
            transport.delivered.size() == 2U,
        "one runtime instance must admit exactly one hosted generation");
}

} // namespace

int main()
{
    bool ok = true;
    ok &= successful_run_projects_once_and_publishes_monotonic_facts();
    ok &= reply_loss_replays_same_fact_without_native_retry();
    ok &= cancellation_and_invalid_envelopes_never_construct_surface();
    ok &= worker_revalidates_product_reserved_base_names();
    ok &= every_construction_failure_totalizes();
    ok &= structured_failure_and_indeterminacy_never_retry();
    ok &= running_failure_totalizes_without_exit_wait();
    ok &= unavailable_or_rejected_delivery_terminates_boundedly();
    ok &= every_transient_delivery_status_is_bounded();
    ok &= stale_generation_acknowledges_and_stops_active_worker();
    ok &= lost_exit_and_zero_generation_terminate_safely();
    ok &= one_runtime_instance_never_starts_twice();
    return ok ? 0 : 1;
}
