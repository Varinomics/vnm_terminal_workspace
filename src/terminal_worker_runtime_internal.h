#pragma once

#include "vnm_terminal_workspace/terminal_worker_runtime.h"

#include <functional>
#include <optional>
#include <vector>

namespace vnm_terminal::terminal_app {
class Terminal_scrollbar;
}

namespace vnm::terminal_workspace::detail {

enum class Structured_start_determinacy
{
    DETERMINATE,
    INDETERMINATE,
};

struct Structured_start_result
{
    bool accepted = false;
    bool native_dispatch_occurred = false;
    Structured_start_determinacy determinacy =
        Structured_start_determinacy::DETERMINATE;
    bool cancelled = false;
};

struct Terminal_worker_start_projection
{
    std::vector<std::string> argv;
    std::string working_directory;
    std::vector<environment_policy::Environment_entry> base_environment;
    std::optional<std::vector<environment_policy::Environment_entry>>
        authorized_environment;
};

struct Terminal_worker_exit_observation
{
    bool observed = false;
    int exit_code = 0;
};

class Terminal_worker_runtime_adapter
{
public:
    virtual ~Terminal_worker_runtime_adapter() = default;

    virtual bool initialize_remote_runtime() = 0;
    virtual bool initialize_root() = 0;
    virtual bool construct_terminal_settings() = 0;
    virtual bool construct_terminal_surface() = 0;
    virtual bool construct_terminal_scrollbar() = 0;
    virtual bool connect_remote_ui() = 0;
    virtual bool connect_row_timestamps() = 0;
    virtual Structured_start_result start_terminal(
        const Terminal_worker_start_projection& projection) = 0;
    virtual bool observe_running() = 0;
    virtual Terminal_worker_exit_observation observe_exit() = 0;
    virtual void terminate_hosted_worker() = 0;
};

class Terminal_worker_coordinator
{
public:
    Terminal_worker_coordinator(
        Terminal_worker_runtime_adapter& adapter,
        Terminal_child_fact_transport& transport);

    Terminal_worker_run_result run(
        std::span<const std::uint8_t> serialized_request,
        Launch_platform platform,
        std::uint64_t hosted_generation,
        std::span<const std::string_view> additional_reserved_names = {},
        std::optional<std::vector<environment_policy::Environment_entry>>
            authorized_environment = std::nullopt);
    Terminal_worker_run_result run_preinitialized(
        std::span<const std::uint8_t> serialized_request,
        Launch_platform platform,
        std::uint64_t hosted_generation,
        std::span<const std::string_view> additional_reserved_names = {},
        std::optional<std::vector<environment_policy::Environment_entry>>
            authorized_environment = std::nullopt);

    std::optional<Terminal_child_fact> current_fact() const;
    std::vector<Terminal_child_fact> unacknowledged_facts() const;
    bool replay_unacknowledged();

private:
    Terminal_worker_run_result run_impl(
        std::span<const std::uint8_t> serialized_request,
        Launch_platform platform,
        std::uint64_t hosted_generation,
        std::span<const std::string_view> additional_reserved_names,
        std::optional<std::vector<environment_policy::Environment_entry>>
            authorized_environment,
        bool initialize_adapter);
    bool publish_fact(
        Terminal_child_fact_kind kind,
        Terminal_child_fact_error error,
        bool native_dispatch_occurred,
        std::optional<int> exit_code = std::nullopt);
    bool reconcile(const Terminal_child_fact& fact);
    void terminate_once();

    Terminal_worker_runtime_adapter& m_adapter;
    Terminal_child_fact_transport& m_transport;
    std::string m_session_identity;
    std::uint64_t m_generation = 0U;
    std::uint64_t m_next_sequence = 1U;
    bool m_run_started = false;
    bool m_terminated = false;
    std::optional<Terminal_child_fact> m_current_fact;
    std::vector<Terminal_child_fact> m_unacknowledged;
};

enum class Terminal_native_start_admission
{
    RELEASE,
    CANCEL,
};

using Terminal_native_start_admission_hook =
    std::function<Terminal_native_start_admission()>;

class Terminal_surface_runtime_adapter final :
    public Terminal_worker_runtime_adapter
{
public:
    Terminal_surface_runtime_adapter(
        Terminal_worker_surface_configuration configuration,
        Terminal_worker_remote_sink& remote_sink,
        Terminal_worker_gui_dispatcher& gui_dispatcher,
        Terminal_worker_termination& termination,
        Terminal_native_start_admission_hook admission_hook = {});
    ~Terminal_surface_runtime_adapter() override;

    Terminal_surface_runtime_adapter(
        const Terminal_surface_runtime_adapter&) = delete;
    Terminal_surface_runtime_adapter& operator=(
        const Terminal_surface_runtime_adapter&) = delete;

    Terminal_worker_initialization_result initialize();
    bool forward_input(const Terminal_remote_input_message& message);
    bool forward_state(const Terminal_remote_state_message& message);
    bool request_present();
    void shutdown();

    bool initialize_remote_runtime() override;
    bool initialize_root() override;
    bool construct_terminal_settings() override;
    bool construct_terminal_surface() override;
    bool construct_terminal_scrollbar() override;
    bool connect_remote_ui() override;
    bool connect_row_timestamps() override;
    Structured_start_result start_terminal(
        const Terminal_worker_start_projection& projection) override;
    bool observe_running() override;
    Terminal_worker_exit_observation observe_exit() override;
    void terminate_hosted_worker() override;

    struct Test_observation
    {
        std::size_t surface_construction_count = 0U;
        std::size_t structured_start_call_count = 0U;
        bool surface_alive = false;
        bool scrollbar_alive = false;
        bool remote_runtime_initialized = false;
        bool timestamp_visible = false;
        bool surface_has_focus = false;
        double surface_width = 0.0;
        double surface_height = 0.0;
        double scrollbar_width = 0.0;
        std::size_t private_teardown_order = 0U;
        std::size_t remote_shutdown_order = 0U;
        int process_state = 0;
    };

    bool test_inject_timestamp_request();
    Test_observation test_observation() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vnm::terminal_workspace::detail
