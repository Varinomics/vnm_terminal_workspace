#include "terminal_worker_runtime_internal.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace vnm::terminal_workspace::detail {
namespace {

constexpr std::size_t k_maximum_unacknowledged_facts = 8U;
constexpr std::size_t k_maximum_delivery_attempts = 3U;

void clear_string(std::string& value)
{
    volatile char* bytes = value.data();
    for (std::size_t index = 0U; index < value.size(); ++index) {
        bytes[index] = '\0';
    }
    value.clear();
}

void clear_projection_values(Terminal_worker_start_projection& projection)
{
    for (std::string& argument : projection.argv) {
        clear_string(argument);
    }
    for (environment_policy::Environment_entry& entry :
         projection.base_environment)
    {
        clear_string(entry.value);
    }
    if (!projection.authorized_environment) {
        return;
    }
    for (environment_policy::Environment_entry& entry :
         *projection.authorized_environment)
    {
        clear_string(entry.value);
    }
}

void clear_request_values(Terminal_launch_request& request)
{
    for (std::string& argument : request.argv) {
        clear_string(argument);
    }
    for (environment_policy::Environment_entry& entry :
         request.base_environment)
    {
        clear_string(entry.value);
    }
}

void clear_authorized_environment_values(
    std::optional<std::vector<environment_policy::Environment_entry>>& entries)
{
    if (!entries) {
        return;
    }
    for (environment_policy::Environment_entry& entry : *entries) {
        clear_string(entry.value);
    }
}

class Sensitive_request_guard
{
public:
    explicit Sensitive_request_guard(Terminal_launch_request& request)
    :
        m_request(request)
    {}

    ~Sensitive_request_guard()
    {
        clear_request_values(m_request);
    }

    Sensitive_request_guard(const Sensitive_request_guard&) = delete;
    Sensitive_request_guard& operator=(const Sensitive_request_guard&) = delete;

private:
    Terminal_launch_request& m_request;
};

class Sensitive_environment_guard
{
public:
    explicit Sensitive_environment_guard(
        std::optional<std::vector<environment_policy::Environment_entry>>& entries)
    :
        m_entries(entries)
    {}

    ~Sensitive_environment_guard()
    {
        clear_authorized_environment_values(m_entries);
    }

    Sensitive_environment_guard(const Sensitive_environment_guard&) = delete;
    Sensitive_environment_guard& operator=(
        const Sensitive_environment_guard&) = delete;

private:
    std::optional<std::vector<environment_policy::Environment_entry>>& m_entries;
};

} // namespace

Terminal_worker_coordinator::Terminal_worker_coordinator(
    Terminal_worker_runtime_adapter& adapter,
    Terminal_child_fact_transport& transport)
:
    m_adapter(adapter),
    m_transport(transport)
{}

Terminal_worker_run_result Terminal_worker_coordinator::run(
    std::span<const std::uint8_t> serialized_request,
    Launch_platform platform,
    std::uint64_t hosted_generation,
    std::span<const std::string_view> additional_reserved_names,
    std::optional<std::vector<environment_policy::Environment_entry>>
        authorized_environment)
{
    return run_impl(
        serialized_request,
        platform,
        hosted_generation,
        additional_reserved_names,
        std::move(authorized_environment),
        true);
}

Terminal_worker_run_result Terminal_worker_coordinator::run_preinitialized(
    std::span<const std::uint8_t> serialized_request,
    Launch_platform platform,
    std::uint64_t hosted_generation,
    std::span<const std::string_view> additional_reserved_names,
    std::optional<std::vector<environment_policy::Environment_entry>>
        authorized_environment)
{
    return run_impl(
        serialized_request,
        platform,
        hosted_generation,
        additional_reserved_names,
        std::move(authorized_environment),
        false);
}

Terminal_worker_run_result Terminal_worker_coordinator::run_impl(
    std::span<const std::uint8_t> serialized_request,
    Launch_platform platform,
    std::uint64_t hosted_generation,
    std::span<const std::string_view> additional_reserved_names,
    std::optional<std::vector<environment_policy::Environment_entry>>
        authorized_environment,
    bool initialize_adapter)
{
    Sensitive_environment_guard authorized_environment_guard(
        authorized_environment);
    if (m_run_started) {
        return Terminal_worker_run_result::ALREADY_STARTED;
    }
    m_run_started = true;
    if (hosted_generation == 0U) {
        terminate_once();
        return Terminal_worker_run_result::INVALID_GENERATION;
    }
    m_generation = hosted_generation;

    Launch_request_result decoded = decode_terminal_launch_request(
        serialized_request,
        platform,
        additional_reserved_names);
    if (!decoded.request) {
        terminate_once();
        return Terminal_worker_run_result::TERMINATED;
    }
    Sensitive_request_guard decoded_request_guard(*decoded.request);
    m_session_identity = decoded.request->session_id;
    if (decoded.status == Launch_request_status::CANCELLED) {
        publish_fact(
            Terminal_child_fact_kind::START_FAILED,
            Terminal_child_fact_error::CANCELLED,
            false);
        terminate_once();
        return Terminal_worker_run_result::TERMINATED;
    }

    const auto fail_before_start = [this](Terminal_child_fact_error error) {
        publish_fact(Terminal_child_fact_kind::START_FAILED, error, false);
        terminate_once();
        return Terminal_worker_run_result::TERMINATED;
    };
    if (initialize_adapter) {
        if (!m_adapter.initialize_remote_runtime()) {
            return fail_before_start(
                Terminal_child_fact_error::REMOTE_RUNTIME_INITIALIZATION);
        }
        if (!m_adapter.initialize_root()) {
            return fail_before_start(
                Terminal_child_fact_error::ROOT_INITIALIZATION);
        }
        if (!m_adapter.construct_terminal_settings()) {
            return fail_before_start(
                Terminal_child_fact_error::SETTINGS_CONSTRUCTION);
        }
        if (!m_adapter.construct_terminal_surface()) {
            return fail_before_start(
                Terminal_child_fact_error::SURFACE_CONSTRUCTION);
        }
        if (!m_adapter.construct_terminal_scrollbar()) {
            return fail_before_start(
                Terminal_child_fact_error::SCROLLBAR_CONSTRUCTION);
        }
        if (!m_adapter.connect_remote_ui()) {
            return fail_before_start(
                Terminal_child_fact_error::REMOTE_UI_CONNECTION);
        }
        if (!m_adapter.connect_row_timestamps()) {
            return fail_before_start(
                Terminal_child_fact_error::TIMESTAMP_CONNECTION);
        }
    }

    Terminal_worker_start_projection projection;
    projection.argv = std::move(decoded.request->argv);
    projection.working_directory = std::move(decoded.request->working_directory);
    projection.base_environment = std::move(decoded.request->base_environment);
    projection.authorized_environment = std::move(authorized_environment);
    const Structured_start_result started = m_adapter.start_terminal(projection);
    clear_projection_values(projection);
    if (!started.accepted) {
        const bool indeterminate =
            started.determinacy == Structured_start_determinacy::INDETERMINATE;
        publish_fact(
            indeterminate && !started.cancelled
                ? Terminal_child_fact_kind::START_INDETERMINATE
                : Terminal_child_fact_kind::START_FAILED,
            started.cancelled
                ? Terminal_child_fact_error::CANCELLED
                : Terminal_child_fact_error::STRUCTURED_START,
            started.native_dispatch_occurred);
        terminate_once();
        return Terminal_worker_run_result::TERMINATED;
    }
    if (!m_adapter.observe_running()) {
        publish_fact(
            Terminal_child_fact_kind::START_FAILED,
            Terminal_child_fact_error::RUNNING_OBSERVATION,
            started.native_dispatch_occurred);
        terminate_once();
        return Terminal_worker_run_result::TERMINATED;
    }

    if (!publish_fact(
            Terminal_child_fact_kind::STARTED,
            Terminal_child_fact_error::NONE,
            started.native_dispatch_occurred))
    {
        terminate_once();
        return Terminal_worker_run_result::TERMINATED;
    }

    const Terminal_worker_exit_observation exit = m_adapter.observe_exit();
    if (!exit.observed ||
        !publish_fact(
            Terminal_child_fact_kind::EXITED,
            Terminal_child_fact_error::NONE,
            true,
            exit.exit_code))
    {
        terminate_once();
        return Terminal_worker_run_result::TERMINATED;
    }
    terminate_once();
    return Terminal_worker_run_result::COMPLETED;
}

std::optional<Terminal_child_fact>
Terminal_worker_coordinator::current_fact() const
{
    return m_current_fact;
}

std::vector<Terminal_child_fact>
Terminal_worker_coordinator::unacknowledged_facts() const
{
    return m_unacknowledged;
}

bool Terminal_worker_coordinator::replay_unacknowledged()
{
    while (!m_unacknowledged.empty()) {
        const Terminal_child_fact fact = m_unacknowledged.front();
        if (!reconcile(fact)) {
            return false;
        }
    }
    return m_unacknowledged.empty();
}

bool Terminal_worker_coordinator::publish_fact(
    Terminal_child_fact_kind kind,
    Terminal_child_fact_error error,
    bool native_dispatch_occurred,
    std::optional<int> exit_code)
{
    if (m_unacknowledged.size() >= k_maximum_unacknowledged_facts) {
        return false;
    }
    Terminal_child_fact fact;
    fact.session_identity = m_session_identity;
    fact.generation = m_generation;
    fact.fact_key = m_next_sequence;
    fact.sequence = m_next_sequence++;
    fact.kind = kind;
    fact.error = error;
    fact.native_dispatch_occurred = native_dispatch_occurred;
    fact.exit_code = exit_code;
    m_current_fact = fact;
    m_unacknowledged.push_back(fact);
    return reconcile(fact);
}

bool Terminal_worker_coordinator::reconcile(const Terminal_child_fact& fact)
{
    for (std::size_t attempt = 0U;
         attempt < k_maximum_delivery_attempts;
         ++attempt)
    {
        const Terminal_child_fact_delivery_result delivered =
            m_transport.deliver(fact);
        if (delivered.status != Terminal_child_fact_delivery_status::DELIVERED ||
            !delivered.acknowledgement)
        {
            continue;
        }
        const Terminal_child_fact_acknowledgement acknowledgement =
            *delivered.acknowledgement;
        const std::uint64_t fact_key = fact.fact_key;
        if (acknowledgement == Terminal_child_fact_acknowledgement::ACCEPTED ||
            acknowledgement ==
                Terminal_child_fact_acknowledgement::ALREADY_CURRENT)
        {
            std::erase_if(
                m_unacknowledged,
                [fact_key](const Terminal_child_fact& candidate) {
                    return candidate.fact_key == fact_key;
                });
            return true;
        }
        std::erase_if(
            m_unacknowledged,
            [fact_key](const Terminal_child_fact& candidate) {
                return candidate.fact_key == fact_key;
            });
        terminate_once();
        return false;
    }
    terminate_once();
    return false;
}

void Terminal_worker_coordinator::terminate_once()
{
    if (m_terminated) {
        return;
    }
    m_terminated = true;
    m_adapter.terminate_hosted_worker();
}

} // namespace vnm::terminal_workspace::detail
