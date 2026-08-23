#pragma once

#include "vnm_terminal_workspace/terminal_launch_request.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vnm::terminal_workspace {

enum class Terminal_child_fact_kind
{
    STARTED,
    START_FAILED,
    START_INDETERMINATE,
    EXITED,
};

enum class Terminal_child_fact_error
{
    NONE,
    CANCELLED,
    REMOTE_RUNTIME_INITIALIZATION,
    ROOT_INITIALIZATION,
    SETTINGS_CONSTRUCTION,
    SURFACE_CONSTRUCTION,
    SCROLLBAR_CONSTRUCTION,
    REMOTE_UI_CONNECTION,
    TIMESTAMP_CONNECTION,
    STRUCTURED_START,
    RUNNING_OBSERVATION,
};

struct Terminal_child_fact
{
    std::string session_identity;
    std::uint64_t generation = 0U;
    std::uint64_t fact_key = 0U;
    std::uint64_t sequence = 0U;
    Terminal_child_fact_kind kind = Terminal_child_fact_kind::START_FAILED;
    Terminal_child_fact_error error = Terminal_child_fact_error::NONE;
    bool native_dispatch_occurred = false;
    std::optional<int> exit_code;

    friend bool operator==(
        const Terminal_child_fact&,
        const Terminal_child_fact&) = default;
};

enum class Terminal_child_fact_acknowledgement
{
    ACCEPTED,
    ALREADY_CURRENT,
    STALE_GENERATION,
    REJECTED,
};

enum class Terminal_child_fact_delivery_status
{
    DELIVERED,
    HANDLER_UNAVAILABLE,
    BACKPRESSURE,
    REPLY_LOST,
    INDETERMINATE,
};

struct Terminal_child_fact_delivery_result
{
    Terminal_child_fact_delivery_status status =
        Terminal_child_fact_delivery_status::INDETERMINATE;
    std::optional<Terminal_child_fact_acknowledgement> acknowledgement;
};

class Terminal_child_fact_transport
{
public:
    virtual ~Terminal_child_fact_transport() = default;

    virtual Terminal_child_fact_delivery_result deliver(
        const Terminal_child_fact& fact) = 0;
};

enum class Terminal_gui_dispatch_kind
{
    QUEUED,
    BLOCKING,
};

class Terminal_worker_gui_dispatcher
{
public:
    virtual ~Terminal_worker_gui_dispatcher() = default;

    virtual bool dispatch(
        Terminal_gui_dispatch_kind kind,
        const std::function<void()>& function) = 0;
};

struct Terminal_remote_surface_descriptor
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t pixel_format = 0U;
};

struct Terminal_remote_frame_buffer
{
    std::span<std::uint8_t> bytes;
    std::int32_t stride = 0;
};

struct Terminal_remote_damage_rectangle
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct Terminal_remote_initial_state
{
    std::optional<std::int32_t> logical_width;
    std::optional<std::int32_t> logical_height;
    std::optional<float> scale_factor;
};

using Terminal_remote_surface_identity = std::uint64_t;

class Terminal_worker_remote_sink
{
public:
    virtual ~Terminal_worker_remote_sink() = default;

    virtual std::optional<Terminal_remote_surface_identity> create_surface(
        const Terminal_remote_surface_descriptor& descriptor) = 0;
    virtual std::optional<Terminal_remote_frame_buffer> begin_frame(
        Terminal_remote_surface_identity surface,
        std::int32_t width,
        std::int32_t height) = 0;
    virtual void end_frame(
        Terminal_remote_surface_identity surface,
        std::span<const Terminal_remote_damage_rectangle> damage,
        std::uint32_t flags) = 0;
    virtual void destroy_surface(
        Terminal_remote_surface_identity surface) = 0;
    virtual void set_cursor(
        Terminal_remote_surface_identity surface,
        std::int32_t cursor_shape) = 0;
    virtual Terminal_remote_initial_state initial_state() = 0;
};

class Terminal_worker_termination
{
public:
    virtual ~Terminal_worker_termination() = default;

    virtual void terminate_hosted_worker() = 0;
};

enum class Terminal_worker_text_renderer_mode
{
    AUTO,
    MSDF,
    GLYPH,
};

enum class Terminal_worker_lcd_subpixel_order
{
    AUTO,
    NONE,
    RGB,
    BGR,
    VRGB,
    VBGR,
};

enum class Terminal_worker_style
{
    LIGHT,
    DARK,
};

struct Terminal_worker_settings
{
    std::string color_scheme = "Classic";
    std::string font_family;
    double font_size = 13.0;
    Terminal_worker_text_renderer_mode text_renderer_mode =
        Terminal_worker_text_renderer_mode::AUTO;
    Terminal_worker_lcd_subpixel_order lcd_subpixel_order =
        Terminal_worker_lcd_subpixel_order::AUTO;
    bool row_timestamp_tooltip_enabled = true;
    std::optional<int> scrollback_limit;

    friend bool operator==(
        const Terminal_worker_settings&,
        const Terminal_worker_settings&) = default;
};

struct Terminal_worker_surface_configuration
{
    std::int32_t logical_width = 800;
    std::int32_t logical_height = 600;
    std::int32_t maximum_physical_width = 8192;
    std::int32_t maximum_physical_height = 8192;
    float scale_factor = 1.0F;
    Terminal_worker_settings settings;
    std::string title;
    Terminal_worker_style style = Terminal_worker_style::DARK;
    double scrollbar_width = 12.0;

    friend bool operator==(
        const Terminal_worker_surface_configuration&,
        const Terminal_worker_surface_configuration&) = default;
};

struct Terminal_worker_output_capture_configuration
{
    std::string base_path;
    std::size_t maximum_bytes = 0U;

    friend bool operator==(
        const Terminal_worker_output_capture_configuration&,
        const Terminal_worker_output_capture_configuration&) = default;
};

class Terminal_worker_complete_settings_sink
{
public:
    virtual ~Terminal_worker_complete_settings_sink() = default;

    virtual void accept_complete_settings(
        const Terminal_worker_settings& settings) = 0;
};

class Terminal_worker_noop_complete_settings_sink final :
    public Terminal_worker_complete_settings_sink
{
public:
    void accept_complete_settings(
        const Terminal_worker_settings&) override;
};

struct Terminal_worker_first_text_frame_observation
{
    int rows = 0;
    int columns = 0;
    bool backend_ready = false;
    bool backend_geometry_in_sync = false;
    std::uint64_t rendered_snapshot_sequence = 0U;
    std::uint64_t rendered_publication_generation = 0U;
    bool drew = false;
    std::uint64_t glyph_draw_calls = 0U;
    std::uint64_t msdf_text_draw_calls = 0U;
    bool cursor_valid = false;
    int cursor_row = 0;
    int cursor_column = 0;

    friend bool operator==(
        const Terminal_worker_first_text_frame_observation&,
        const Terminal_worker_first_text_frame_observation&) = default;
};

struct Terminal_worker_backend_error_observation
{
    int code = 0;
    std::string message;

    friend bool operator==(
        const Terminal_worker_backend_error_observation&,
        const Terminal_worker_backend_error_observation&) = default;
};

class Terminal_worker_diagnostic_observation_sink
{
public:
    virtual ~Terminal_worker_diagnostic_observation_sink() = default;

    virtual void terminal_output_activity() = 0;
    virtual void terminal_first_text_frame_produced(
        const Terminal_worker_first_text_frame_observation& observation) = 0;
    virtual void terminal_backend_error(
        const Terminal_worker_backend_error_observation& observation) = 0;
};

class Terminal_worker_noop_diagnostic_observation_sink final :
    public Terminal_worker_diagnostic_observation_sink
{
public:
    void terminal_output_activity() override;
    void terminal_first_text_frame_produced(
        const Terminal_worker_first_text_frame_observation&) override;
    void terminal_backend_error(
        const Terminal_worker_backend_error_observation&) override;
};

enum class Terminal_worker_message_submission_outcome
{
    ACCEPTED,
    INVALID_UTF8,
    INVALID_MESSAGE,
    EMPTY_MESSAGE,
    MESSAGE_TOO_LARGE,
    NOT_RUNNING,
    CLOSING,
    CAPABILITY_MISSING,
    STALE_GENERATION,
    BACKPRESSURE,
    QUEUE_LIMIT,
    BACKEND_REJECTED,
    WORKER_REJECTED,
    DEADLINE_EXPIRED,
    INDETERMINATE,
};

struct Terminal_worker_message_submission_result
{
    Terminal_worker_message_submission_outcome outcome =
        Terminal_worker_message_submission_outcome::BACKEND_REJECTED;
    std::string error;

    [[nodiscard]] bool accepted() const noexcept
    {
        return outcome == Terminal_worker_message_submission_outcome::ACCEPTED;
    }
};

struct Terminal_remote_input_message
{
    std::uint32_t event_type = 0U;
    std::uint32_t modifiers = 0U;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t button = 0U;
    std::uint32_t buttons = 0U;
    std::uint32_t key = 0U;
    float scroll_dx = 0.0F;
    float scroll_dy = 0.0F;
    std::array<char, 32U> text_utf8{};
    std::uint64_t timestamp = 0U;
};

struct Terminal_remote_state_message
{
    std::uint32_t state_type = 0U;
    std::int32_t width = 0;
    std::int32_t height = 0;
    float scale_factor = 1.0F;
    std::uint8_t value = 0U;
};

enum class Terminal_worker_initialization_result
{
    READY,
    ALREADY_INITIALIZED,
    INVALID_CONFIGURATION,
    REMOTE_RUNTIME_FAILED,
    PRIVATE_ROOT_FAILED,
    SURFACE_FAILED,
};

enum class Terminal_worker_run_result
{
    COMPLETED,
    TERMINATED,
    NOT_INITIALIZED,
    INVALID_GENERATION,
    ALREADY_STARTED,
};

// Production owns the concrete remote runtime, QML root, terminal surface,
// scrollbar, and structured start. The installed boundary contains only
// value messages and host-neutral control adapters.
class Terminal_worker_runtime
{
public:
    Terminal_worker_runtime(
        Terminal_worker_surface_configuration configuration,
        std::optional<Terminal_worker_output_capture_configuration>
            output_capture,
        Terminal_worker_remote_sink& remote_sink,
        Terminal_worker_gui_dispatcher& gui_dispatcher,
        Terminal_child_fact_transport& fact_transport,
        Terminal_worker_termination& termination,
        Terminal_worker_complete_settings_sink& complete_settings_sink,
        Terminal_worker_diagnostic_observation_sink& diagnostic_sink);
    ~Terminal_worker_runtime();

    Terminal_worker_runtime(const Terminal_worker_runtime&) = delete;
    Terminal_worker_runtime& operator=(const Terminal_worker_runtime&) = delete;

    Terminal_worker_initialization_result initialize();
    Terminal_worker_run_result run(
        std::span<const std::uint8_t> serialized_request,
        Launch_platform platform,
        std::uint64_t hosted_generation,
        std::optional<std::vector<environment_policy::Environment_entry>>
            authorized_environment = std::nullopt);

    Terminal_worker_message_submission_result submit_message(
        std::span<const std::uint8_t> message_utf8);
    bool forward_input(const Terminal_remote_input_message& message);
    bool forward_state(const Terminal_remote_state_message& message);
    bool request_present();
    void shutdown();

    std::optional<Terminal_child_fact> current_fact() const;
    std::vector<Terminal_child_fact> unacknowledged_facts() const;
    bool replay_unacknowledged();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vnm::terminal_workspace
