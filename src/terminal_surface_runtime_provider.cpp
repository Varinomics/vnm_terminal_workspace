#include "terminal_worker_runtime_internal.h"

#include "vnm_remote_ui_surface_runtime.h"
#include "vnm_terminal/app_support/app_shortcuts.h"
#include "vnm_terminal/app_support/app_settings.h"
#include "vnm_terminal/app_support/terminal_search_bar.h"
#include "vnm_terminal/app_support/terminal_scrollbar.h"
#include "vnm_terminal/app_support/terminal_settings_controller.h"
#include "vnm_terminal/app_support/terminal_settings_window.h"
#include "vnm_terminal/backend_output_capture.h"
#include "vnm_terminal/diagnostics/metrics_json.h"
#include "vnm_terminal/terminal_message_submission.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <QByteArray>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QResource>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_set>
#include <utility>

static void initialize_worker_qml_resource()
{
    Q_INIT_RESOURCE(terminal_worker);
}

namespace vnm::terminal_workspace::detail {
namespace {

using vnm_terminal::Terminal_environment_entry;
using vnm_terminal::Terminal_process_start_request;

constexpr std::chrono::seconds k_exit_observation_timeout{30};
constexpr std::chrono::seconds k_termination_observation_timeout{5};
constexpr qint64 k_terminal_output_activity_throttle_ms = 250;
constexpr qsizetype k_backend_diagnostic_max_characters = 1024;
constexpr qsizetype k_backend_diagnostic_scan_characters = 4096;
constexpr char k_worker_qml_url[] =
    "qrc:/vnm_terminal_workspace/TerminalWorkerRoot.qml";

std::optional<QString> strict_utf8(std::string_view value)
{
    const QByteArray bytes(value.data(), static_cast<qsizetype>(value.size()));
    const QString decoded = QString::fromUtf8(bytes);
    if (decoded.toUtf8() != bytes) {
        return std::nullopt;
    }
    return decoded;
}

std::optional<std::vector<Terminal_environment_entry>> terminal_environment(
    const std::vector<environment_policy::Environment_entry>& entries)
{
    std::vector<Terminal_environment_entry> result;
    result.reserve(entries.size());
    for (const environment_policy::Environment_entry& entry : entries) {
        std::optional<QString> name = strict_utf8(entry.name);
        std::optional<QString> value = strict_utf8(entry.value);
        if (!name || !value) {
            return std::nullopt;
        }
        result.push_back({std::move(*name), std::move(*value)});
    }
    return result;
}

std::optional<Terminal_process_start_request> terminal_start_request(
    const Terminal_worker_start_projection& projection)
{
    Terminal_process_start_request request;
    request.argv.reserve(static_cast<qsizetype>(projection.argv.size()));
    for (const std::string& argument : projection.argv) {
        std::optional<QString> decoded = strict_utf8(argument);
        if (!decoded) {
            return std::nullopt;
        }
        request.argv.push_back(std::move(*decoded));
    }
    std::optional<QString> working_directory =
        strict_utf8(projection.working_directory);
    std::optional<std::vector<Terminal_environment_entry>> base_environment =
        terminal_environment(projection.base_environment);
    if (!working_directory || !base_environment) {
        return std::nullopt;
    }
    request.working_directory = std::move(*working_directory);
    request.base_environment = std::move(*base_environment);
    if (projection.authorized_environment) {
        std::optional<std::vector<Terminal_environment_entry>> authorized =
            terminal_environment(*projection.authorized_environment);
        if (!authorized) {
            return std::nullopt;
        }
        request.capability_environment = std::move(*authorized);
    }
    return request;
}

Structured_start_determinacy workspace_determinacy(
    vnm_terminal::Terminal_process_start_determinacy determinacy)
{
    return determinacy ==
            vnm_terminal::Terminal_process_start_determinacy::DETERMINATE
        ? Structured_start_determinacy::DETERMINATE
        : Structured_start_determinacy::INDETERMINATE;
}

bool valid_configuration(
    const Terminal_worker_surface_configuration& configuration)
{
    return configuration.logical_width > 0 &&
        configuration.logical_height > 0 &&
        configuration.maximum_physical_width > 0 &&
        configuration.maximum_physical_height > 0 &&
        std::isfinite(configuration.scale_factor) &&
        configuration.scale_factor > 0.0F &&
        std::isfinite(configuration.scrollbar_width) &&
        configuration.scrollbar_width >= 0.0 &&
        std::isfinite(configuration.settings.font_size) &&
        configuration.settings.font_size > 0.0 &&
        (!configuration.settings.scrollback_limit ||
         *configuration.settings.scrollback_limit >= 0) &&
        strict_utf8(configuration.settings.color_scheme).has_value() &&
        strict_utf8(configuration.settings.font_family).has_value() &&
        strict_utf8(configuration.title).has_value();
}

bool valid_output_capture(
    const std::optional<Terminal_worker_output_capture_configuration>& capture)
{
    return !capture ||
        (!capture->base_path.empty() && capture->maximum_bytes > 0U &&
         strict_utf8(capture->base_path).has_value());
}

vnm_terminal::terminal_app::Terminal_settings_snapshot terminal_settings(
    const Terminal_worker_settings& settings)
{
    vnm_terminal::terminal_app::Terminal_settings_snapshot snapshot;
    snapshot.color_scheme = strict_utf8(settings.color_scheme).value();
    if (!settings.font_family.empty()) {
        snapshot.font_family = strict_utf8(settings.font_family).value();
    }
    snapshot.font_size = settings.font_size;
    snapshot.text_renderer_mode = static_cast<int>(settings.text_renderer_mode);
    snapshot.lcd_subpixel_order = static_cast<int>(settings.lcd_subpixel_order);
    snapshot.row_timestamp_tooltip_enabled =
        settings.row_timestamp_tooltip_enabled;
    return snapshot;
}

Terminal_worker_settings worker_settings(
    const vnm_terminal::terminal_app::Terminal_settings_snapshot& snapshot,
    const VNM_TerminalSurface& surface)
{
    Terminal_worker_settings settings;
    settings.color_scheme = snapshot.color_scheme.toStdString();
    settings.font_family = snapshot.font_family.toStdString();
    settings.font_size = snapshot.font_size;
    settings.text_renderer_mode =
        static_cast<Terminal_worker_text_renderer_mode>(
            snapshot.text_renderer_mode);
    settings.lcd_subpixel_order =
        static_cast<Terminal_worker_lcd_subpixel_order>(
            snapshot.lcd_subpixel_order);
    settings.row_timestamp_tooltip_enabled =
        snapshot.row_timestamp_tooltip_enabled;
    // The app-support snapshot carries only the MiB buffer budget; the
    // workspace contract is the surface's line limit, read back live.
    settings.scrollback_limit =
        surface.scrollback_limit() == std::numeric_limits<int>::max()
            ? std::nullopt
            : std::optional<int>(surface.scrollback_limit());
    return settings;
}

QString bounded_backend_diagnostic(QString diagnostic)
{
    diagnostic.truncate(k_backend_diagnostic_scan_characters);
    for (qsizetype index = 0; index < diagnostic.size(); ++index) {
        const ushort character = diagnostic.at(index).unicode();
        if (character < 0x20 || character == 0x7f) {
            diagnostic[index] = QLatin1Char(' ');
        }
    }
    diagnostic = diagnostic.simplified();
    diagnostic.truncate(k_backend_diagnostic_max_characters);
    return diagnostic;
}

std::optional<std::uint64_t> diagnostics_counter(
    const QJsonObject& object,
    const char* key)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (!value.isString()) {
        return std::nullopt;
    }
    bool valid = false;
    const qulonglong counter = value.toString().toULongLong(&valid, 10);
    return valid ? std::optional<std::uint64_t>{counter} : std::nullopt;
}

Terminal_worker_message_submission_outcome workspace_message_outcome(
    vnm_terminal::Terminal_message_submission_outcome outcome)
{
    using Surface_outcome =
        vnm_terminal::Terminal_message_submission_outcome;
    switch (outcome) {
        case Surface_outcome::ACCEPTED:
            return Terminal_worker_message_submission_outcome::ACCEPTED;
        case Surface_outcome::INVALID_UTF8:
            return Terminal_worker_message_submission_outcome::INVALID_UTF8;
        case Surface_outcome::INVALID_MESSAGE:
            return Terminal_worker_message_submission_outcome::INVALID_MESSAGE;
        case Surface_outcome::EMPTY_MESSAGE:
            return Terminal_worker_message_submission_outcome::EMPTY_MESSAGE;
        case Surface_outcome::MESSAGE_TOO_LARGE:
            return Terminal_worker_message_submission_outcome::MESSAGE_TOO_LARGE;
        case Surface_outcome::NOT_RUNNING:
            return Terminal_worker_message_submission_outcome::NOT_RUNNING;
        case Surface_outcome::QUEUE_LIMIT:
            return Terminal_worker_message_submission_outcome::QUEUE_LIMIT;
        case Surface_outcome::BACKEND_REJECTED:
            return Terminal_worker_message_submission_outcome::BACKEND_REJECTED;
    }
    return Terminal_worker_message_submission_outcome::BACKEND_REJECTED;
}

class Remote_sink_bridge
{
public:
    explicit Remote_sink_bridge(Terminal_worker_remote_sink& sink)
    :
        m_sink(sink)
    {}

    ~Remote_sink_bridge()
    {
        clear();
    }

    vnm::vnm_remote_ui_surface_sink_t surface_sink()
    {
        return {
            this,
            &Remote_sink_bridge::create_surface,
            &Remote_sink_bridge::begin_frame,
            &Remote_sink_bridge::end_frame,
            &Remote_sink_bridge::destroy_surface,
            &Remote_sink_bridge::set_cursor,
            &Remote_sink_bridge::get_initial_state,
        };
    }

    void clear()
    {
        std::vector<Surface_handle*> remaining;
        {
            std::lock_guard lock(m_mutex);
            remaining.assign(m_handles.begin(), m_handles.end());
            m_handles.clear();
        }
        for (Surface_handle* handle : remaining) {
            m_sink.destroy_surface(handle->identity);
            delete handle;
        }
    }

private:
    struct Surface_handle
    {
        Terminal_remote_surface_identity identity = 0U;
        std::optional<Terminal_remote_frame_buffer> current_frame;
    };

    static Remote_sink_bridge* self(void* context)
    {
        return static_cast<Remote_sink_bridge*>(context);
    }

    static vnm::vnm_remote_ui_surface_t create_surface(
        void* context,
        const vnm::vnm_remote_ui_surface_desc_t* descriptor)
    {
        Remote_sink_bridge* bridge = self(context);
        if (bridge == nullptr || descriptor == nullptr) {
            return nullptr;
        }
        const std::optional<Terminal_remote_surface_identity> identity =
            bridge->m_sink.create_surface({
                descriptor->width,
                descriptor->height,
                descriptor->pixel_format,
            });
        if (!identity) {
            return nullptr;
        }
        auto handle = std::make_unique<Surface_handle>();
        handle->identity = *identity;
        Surface_handle* const result = handle.release();
        {
            std::lock_guard lock(bridge->m_mutex);
            bridge->m_handles.insert(result);
        }
        return result;
    }

    static std::uint8_t* begin_frame(
        void* context,
        vnm::vnm_remote_ui_surface_t surface,
        std::int32_t width,
        std::int32_t height,
        std::int32_t* output_stride)
    {
        Remote_sink_bridge* bridge = self(context);
        auto* handle = static_cast<Surface_handle*>(surface);
        if (bridge == nullptr || handle == nullptr || output_stride == nullptr ||
            width <= 0 || height <= 0)
        {
            return nullptr;
        }
        handle->current_frame = bridge->m_sink.begin_frame(
            handle->identity,
            width,
            height);
        if (!handle->current_frame || handle->current_frame->stride <= 0) {
            handle->current_frame.reset();
            return nullptr;
        }
        const std::uint64_t required =
            static_cast<std::uint64_t>(handle->current_frame->stride) *
            static_cast<std::uint64_t>(height);
        if (required > handle->current_frame->bytes.size()) {
            handle->current_frame.reset();
            return nullptr;
        }
        *output_stride = handle->current_frame->stride;
        return handle->current_frame->bytes.data();
    }

    static void end_frame(
        void* context,
        vnm::vnm_remote_ui_surface_t surface,
        std::uint32_t damage_count,
        const std::int32_t (*damage_rectangles)[4],
        std::uint32_t flags)
    {
        Remote_sink_bridge* bridge = self(context);
        auto* handle = static_cast<Surface_handle*>(surface);
        if (bridge == nullptr || handle == nullptr) {
            return;
        }
        std::vector<Terminal_remote_damage_rectangle> damage;
        damage.reserve(damage_count);
        for (std::uint32_t index = 0U;
             index < damage_count && damage_rectangles != nullptr;
             ++index)
        {
            damage.push_back({
                damage_rectangles[index][0],
                damage_rectangles[index][1],
                damage_rectangles[index][2],
                damage_rectangles[index][3],
            });
        }
        bridge->m_sink.end_frame(handle->identity, damage, flags);
        handle->current_frame.reset();
    }

    static void destroy_surface(
        void* context,
        vnm::vnm_remote_ui_surface_t surface)
    {
        Remote_sink_bridge* bridge = self(context);
        auto* handle = static_cast<Surface_handle*>(surface);
        if (bridge == nullptr || handle == nullptr) {
            return;
        }
        {
            std::lock_guard lock(bridge->m_mutex);
            bridge->m_handles.erase(handle);
        }
        bridge->m_sink.destroy_surface(handle->identity);
        delete handle;
    }

    static void set_cursor(
        void* context,
        vnm::vnm_remote_ui_surface_t surface,
        std::int32_t cursor_shape)
    {
        Remote_sink_bridge* bridge = self(context);
        auto* handle = static_cast<Surface_handle*>(surface);
        if (bridge != nullptr && handle != nullptr) {
            bridge->m_sink.set_cursor(handle->identity, cursor_shape);
        }
    }

    static std::int32_t get_initial_state(
        void* context,
        vnm::vnm_ui_initial_state_t* state)
    {
        Remote_sink_bridge* bridge = self(context);
        if (bridge == nullptr || state == nullptr) {
            return 0;
        }
        const Terminal_remote_initial_state initial =
            bridge->m_sink.initial_state();
        if (initial.logical_width && initial.logical_height) {
            state->flags |= vnm::k_ui_initial_state_logical_size;
            state->logical_width = *initial.logical_width;
            state->logical_height = *initial.logical_height;
        }
        if (initial.scale_factor) {
            state->flags |= vnm::k_ui_initial_state_scale_factor;
            state->scale_factor = *initial.scale_factor;
        }
        return state->flags == 0U ? 0 : 1;
    }

    Terminal_worker_remote_sink& m_sink;
    std::mutex m_mutex;
    std::unordered_set<Surface_handle*> m_handles;
};

vnm::vnm_ui_input_message_t remote_input(
    const Terminal_remote_input_message& message)
{
    vnm::vnm_ui_input_message_t result{};
    result.event_type         = message.event_type;
    result.modifiers          = message.modifiers;
    result.x                  = message.x;
    result.y                  = message.y;
    result.button             = message.button;
    result.buttons            = message.buttons;
    result.key                = message.key;
    result.scroll_dx          = message.scroll_dx;
    result.scroll_dy          = message.scroll_dy;
    result.text_utf8          = message.text_utf8;
    result.timestamp          = message.timestamp;
    result.native_scan_code   = message.native_scan_code;
    result.native_virtual_key = message.native_virtual_key;
    result.native_modifiers   = message.native_modifiers;
    result.auto_repeat        = message.auto_repeat;
    result.count              = message.count;
    return result;
}

vnm::vnm_ui_state_message_t remote_state(
    const Terminal_remote_state_message& message)
{
    return {
        message.state_type,
        message.width,
        message.height,
        message.scale_factor,
        message.value,
    };
}

} // namespace

struct Terminal_surface_runtime_adapter::Impl
{
    Impl(
        Terminal_worker_surface_configuration worker_configuration,
        std::optional<Terminal_worker_output_capture_configuration>
            worker_output_capture,
        Terminal_worker_remote_sink& worker_remote_sink,
        Terminal_worker_gui_dispatcher& worker_gui_dispatcher,
        Terminal_worker_termination& worker_termination,
        Terminal_worker_complete_settings_sink& worker_complete_settings_sink,
        Terminal_worker_diagnostic_observation_sink& worker_diagnostic_sink,
        Terminal_native_start_admission_hook native_start_admission)
    :
        configuration(std::move(worker_configuration)),
        output_capture(std::move(worker_output_capture)),
        remote_sink(worker_remote_sink),
        gui_dispatcher(worker_gui_dispatcher),
        termination(worker_termination),
        complete_settings_sink(worker_complete_settings_sink),
        diagnostic_sink(worker_diagnostic_sink),
        admission_hook(std::move(native_start_admission)),
        sink_bridge(remote_sink)
    {}

    bool dispatch(
        Terminal_gui_dispatch_kind kind,
        const std::function<void()>& function)
    {
        return gui_dispatcher.dispatch(kind, function);
    }

    void apply_layout()
    {
        if (root_item == nullptr || surface == nullptr || scrollbar == nullptr) {
            return;
        }
        const qreal width = std::max<qreal>(0.0, root_item->width());
        const qreal height = std::max<qreal>(0.0, root_item->height());
        const qreal scrollbar_width = std::clamp<qreal>(
            configuration.scrollbar_width,
            0.0,
            width);
        surface->setPosition(QPointF(0.0, 0.0));
        surface->setSize(QSizeF(width - scrollbar_width, height));
        scrollbar->setPosition(QPointF(width - scrollbar_width, 0.0));
        scrollbar->setSize(QSizeF(scrollbar_width, height));
    }

    void publish_complete_settings()
    {
        if (surface == nullptr) {
            return;
        }
        complete_settings_sink.accept_complete_settings(worker_settings(
            vnm_terminal::terminal_app::terminal_settings_snapshot(*surface),
            *surface));
    }

    void observe_first_output()
    {
        if (surface == nullptr || terminal_output_observed) {
            return;
        }
        QJsonObject invalidation;
        vnm_terminal::diagnostics::append_render_invalidation_metrics_json(
            *surface,
            invalidation);
        first_output_rendered_snapshot_sequence = diagnostics_counter(
            invalidation,
            "last_rendered_snapshot_sequence");
        terminal_output_observed = true;
    }

    void maybe_publish_first_text_frame()
    {
        if (surface == nullptr || terminal_first_text_frame_produced ||
            !process_started_observed || !terminal_output_observed ||
            !first_output_rendered_snapshot_sequence)
        {
            return;
        }

        QJsonObject atlas;
        vnm_terminal::diagnostics::append_atlas_metrics_json(*surface, atlas);
        QJsonObject invalidation;
        vnm_terminal::diagnostics::append_render_invalidation_metrics_json(
            *surface,
            invalidation);
        const std::optional<std::uint64_t> captured_snapshot_sequence =
            diagnostics_counter(atlas, "captured_snapshot_sequence");
        const std::optional<std::uint64_t> rendered_snapshot_sequence =
            diagnostics_counter(invalidation, "last_rendered_snapshot_sequence");
        const std::optional<std::uint64_t> rendered_publication_generation =
            diagnostics_counter(
                invalidation,
                "last_rendered_publication_generation");
        const QJsonValue drew = atlas.value(QStringLiteral("drew"));
        const QJsonObject buffer_upload =
            atlas.value(QStringLiteral("buffer_upload")).toObject();
        const std::optional<std::uint64_t> glyph_draw_calls =
            diagnostics_counter(buffer_upload, "glyph_draw_calls");
        const std::optional<std::uint64_t> msdf_text_draw_calls =
            diagnostics_counter(buffer_upload, "msdf_text_draw_calls");
        const QJsonObject cursor =
            atlas.value(QStringLiteral("captured_render_cursor")).toObject();
        if (!captured_snapshot_sequence || !rendered_snapshot_sequence ||
            !rendered_publication_generation ||
            *rendered_snapshot_sequence <=
                *first_output_rendered_snapshot_sequence ||
            *captured_snapshot_sequence != *rendered_snapshot_sequence ||
            *rendered_publication_generation == 0U || !drew.isBool() ||
            !drew.toBool() || !glyph_draw_calls || !msdf_text_draw_calls ||
            (*glyph_draw_calls == 0U && *msdf_text_draw_calls == 0U) ||
            !cursor.value(QStringLiteral("valid")).isBool() ||
            !cursor.value(QStringLiteral("row")).isDouble() ||
            !cursor.value(QStringLiteral("column")).isDouble())
        {
            return;
        }

        terminal_first_text_frame_produced = true;
        QObject::disconnect(first_text_frame_connection);
        diagnostic_sink.terminal_first_text_frame_produced({
            surface->rows(),
            surface->columns(),
            surface->backend_ready(),
            surface->backend_geometry_in_sync(),
            *rendered_snapshot_sequence,
            *rendered_publication_generation,
            true,
            *glyph_draw_calls,
            *msdf_text_draw_calls,
            cursor.value(QStringLiteral("valid")).toBool(),
            cursor.value(QStringLiteral("row")).toInt(),
            cursor.value(QStringLiteral("column")).toInt(),
        });
    }

    void bind_first_text_frame_window(QQuickWindow* window)
    {
        QObject::disconnect(first_text_frame_connection);
        if (window == nullptr || surface == nullptr ||
            terminal_first_text_frame_produced)
        {
            return;
        }
        first_text_frame_connection = QObject::connect(
            window,
            &QQuickWindow::afterFrameEnd,
            surface.data(),
            [this] { maybe_publish_first_text_frame(); },
            Qt::QueuedConnection);
    }

    void construct_private_root(QObject* loaded_root)
    {
        root_callback_observed = true;
        if (root_item != nullptr || surface != nullptr || scrollbar != nullptr) {
            return;
        }
        root_item = qobject_cast<QQuickItem*>(loaded_root);
        if (root_item == nullptr ||
            root_item->thread() != QThread::currentThread())
        {
            return;
        }

        const std::optional<QString> title = strict_utf8(configuration.title);
        if (!title) {
            return;
        }
        root_item->setProperty("terminalTitle", *title);
        root_item->setProperty(
            "terminalStyle",
            configuration.style == Terminal_worker_style::DARK ? 1 : 0);

        settings = terminal_settings(configuration.settings);
        surface = new VNM_TerminalSurface(root_item.data());
        ++surface_construction_count;
        if (output_capture) {
            const std::optional<QString> base_path =
                strict_utf8(output_capture->base_path);
            if (!base_path || base_path->isEmpty() ||
                output_capture->maximum_bytes == 0U)
            {
                return;
            }
            surface->set_backend_output_capture_config(
                vnm_terminal::Backend_output_capture_config{
                    *base_path,
                    output_capture->maximum_bytes,
                });
            output_capture_configured = true;
        }
        vnm_terminal::terminal_app::apply_terminal_settings_snapshot(
            *settings,
            *surface);
        if (configuration.settings.scrollback_limit) {
            surface->set_scrollback_limit(*configuration.settings.scrollback_limit);
        }
        scrollbar = new vnm_terminal::terminal_app::Terminal_scrollbar(
            root_item.data());
        scrollbar->set_surface(surface.data());
        apply_layout();

        QQmlEngine* const engine = qmlEngine(root_item.data());
        QQuickWindow* const window = root_item->window();
        if (engine == nullptr || window == nullptr) {
            return;
        }
        search_bar = new vnm_terminal::terminal_app::Terminal_search_bar(
            *engine,
            *window,
            *surface);
        if (!search_bar->is_valid()) {
            return;
        }
        search_bar->setParent(surface.data());
        settings_controller =
            new vnm_terminal::terminal_app::Terminal_settings_controller(
                surface.data());
        settings_window =
            new vnm_terminal::terminal_app::Terminal_settings_window(
                *engine,
                *surface,
                *settings_controller,
                false,
                surface.data());
        if (!settings_window->is_valid()) {
            return;
        }
        settings_window->set_transient_parent(window);
        shortcut_filter =
            new vnm_terminal::terminal_app::Terminal_shortcut_filter(
                surface.data());
        window->installEventFilter(shortcut_filter.data());
        shortcut_filter->set_search_ui_root(search_bar->root_item());
        QObject::connect(
            shortcut_filter.data(),
            &vnm_terminal::terminal_app::Terminal_shortcut_filter::
                search_requested,
            search_bar.data(),
            &vnm_terminal::terminal_app::Terminal_search_bar::show_search);
        QObject::connect(
            shortcut_filter.data(),
            &vnm_terminal::terminal_app::Terminal_shortcut_filter::
                search_next_requested,
            surface.data(),
            [this] { static_cast<void>(surface->search_next()); });
        QObject::connect(
            shortcut_filter.data(),
            &vnm_terminal::terminal_app::Terminal_shortcut_filter::
                search_previous_requested,
            surface.data(),
            [this] { static_cast<void>(surface->search_previous()); });
        QObject::connect(
            shortcut_filter.data(),
            &vnm_terminal::terminal_app::Terminal_shortcut_filter::
                search_dismiss_requested,
            search_bar.data(),
            &vnm_terminal::terminal_app::Terminal_search_bar::dismiss_search);
        QObject::connect(
            shortcut_filter.data(),
            &vnm_terminal::terminal_app::Terminal_shortcut_filter::
                settings_requested,
            settings_window.data(),
            &vnm_terminal::terminal_app::Terminal_settings_window::show_window);
        QObject::connect(
            search_bar.data(),
            &vnm_terminal::terminal_app::Terminal_search_bar::visibility_changed,
            shortcut_filter.data(),
            &vnm_terminal::terminal_app::Terminal_shortcut_filter::
                set_search_ui_visible);

        QObject::connect(
            root_item,
            &QQuickItem::widthChanged,
            root_item,
            [this] { apply_layout(); });
        QObject::connect(
            root_item,
            &QQuickItem::heightChanged,
            root_item,
            [this] { apply_layout(); });
        QObject::connect(
            root_item,
            &QQuickItem::activeFocusChanged,
            root_item,
            [this] {
                if (root_item != nullptr && root_item->hasActiveFocus() &&
                    surface != nullptr)
                {
                    surface->forceActiveFocus();
                }
            });
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::terminal_title_changed,
            root_item,
            [this] {
                if (root_item != nullptr && surface != nullptr) {
                    root_item->setProperty(
                        "terminalTitle",
                        surface->terminal_title());
                }
            });
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::row_timestamp_tooltip_requested,
            root_item,
            [this](const QRectF& row_rect, const QDateTime& timestamp) {
                if (root_item == nullptr) {
                    return;
                }
                root_item->setProperty("timestampX", row_rect.x());
                root_item->setProperty("timestampY", row_rect.bottom());
                root_item->setProperty(
                    "timestampText",
                    timestamp.toString(Qt::ISODateWithMs));
                root_item->setProperty("timestampVisible", true);
            });
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::row_timestamp_tooltip_dismissed,
            root_item,
            [this] {
                if (root_item != nullptr) {
                    root_item->setProperty("timestampVisible", false);
                }
            });
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::process_started,
            surface.data(),
            [this] {
                process_started_observed = true;
            });
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::backend_error,
            surface.data(),
            [this](
                VNM_TerminalSurface::Backend_error_code code,
                const QString& message)
            {
                diagnostic_sink.terminal_backend_error({
                    static_cast<int>(code),
                    bounded_backend_diagnostic(message).toStdString(),
                });
            });
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::windowChanged,
            surface.data(),
            [this](QQuickWindow* changed_window) {
                bind_first_text_frame_window(changed_window);
            });
        bind_first_text_frame_window(surface->window());
        auto last_output_activity_ms = std::make_shared<qint64>(-1);
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::output_activity,
            surface.data(),
            [this, last_output_activity_ms] {
                observe_first_output();
                const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
                if (*last_output_activity_ms >= 0 &&
                    now_ms >= *last_output_activity_ms &&
                    now_ms - *last_output_activity_ms <
                        k_terminal_output_activity_throttle_ms)
                {
                    return;
                }
                *last_output_activity_ms = now_ms;
                diagnostic_sink.terminal_output_activity();
            });
        const auto publish_settings = [this] { publish_complete_settings(); };
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::font_size_changed,
            surface.data(),
            publish_settings);
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::color_scheme_changed,
            surface.data(),
            publish_settings);
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::font_family_changed,
            surface.data(),
            publish_settings);
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::text_renderer_mode_changed,
            surface.data(),
            publish_settings);
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::lcd_subpixel_order_changed,
            surface.data(),
            publish_settings);
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::row_timestamp_tooltip_enabled_changed,
            surface.data(),
            publish_settings);
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::scrollback_limit_changed,
            surface.data(),
            publish_settings);
        QObject::connect(
            surface.data(),
            &VNM_TerminalSurface::process_exited,
            surface.data(),
            [this](VNM_TerminalSurface::Exit_reason, int exit_code) {
                exit_observation = Terminal_worker_exit_observation{
                    true,
                    exit_code,
                };
            });
        surface->forceActiveFocus();
        private_root_ready = true;
    }

    void destroy_private_objects()
    {
        QObject::disconnect(first_text_frame_connection);
        delete settings_window.data();
        settings_window.clear();
        delete settings_controller.data();
        settings_controller.clear();
        delete search_bar.data();
        search_bar.clear();
        delete shortcut_filter.data();
        shortcut_filter.clear();
        delete scrollbar.data();
        scrollbar.clear();
        delete surface.data();
        surface.clear();
        settings.reset();
        root_item.clear();
        private_root_ready = false;
    }

    Terminal_worker_surface_configuration configuration;
    std::optional<Terminal_worker_output_capture_configuration> output_capture;
    Terminal_worker_remote_sink& remote_sink;
    Terminal_worker_gui_dispatcher& gui_dispatcher;
    Terminal_worker_termination& termination;
    Terminal_worker_complete_settings_sink& complete_settings_sink;
    Terminal_worker_diagnostic_observation_sink& diagnostic_sink;
    Terminal_native_start_admission_hook admission_hook;
    Remote_sink_bridge sink_bridge;
    vnm::Remote_ui_surface_runtime remote_runtime;
    QPointer<QQuickItem> root_item;
    std::optional<vnm_terminal::terminal_app::Terminal_settings_snapshot>
        settings;
    QPointer<VNM_TerminalSurface> surface;
    QPointer<vnm_terminal::terminal_app::Terminal_scrollbar> scrollbar;
    QPointer<vnm_terminal::terminal_app::Terminal_search_bar> search_bar;
    QPointer<vnm_terminal::terminal_app::Terminal_settings_controller>
        settings_controller;
    QPointer<vnm_terminal::terminal_app::Terminal_settings_window>
        settings_window;
    QPointer<vnm_terminal::terminal_app::Terminal_shortcut_filter>
        shortcut_filter;
    QMetaObject::Connection first_text_frame_connection;
    std::optional<Terminal_worker_exit_observation> exit_observation;
    std::optional<std::uint64_t> first_output_rendered_snapshot_sequence;
    std::size_t surface_construction_count = 0U;
    std::size_t structured_start_call_count = 0U;
    bool root_callback_observed = false;
    bool private_root_ready = false;
    bool process_started_observed = false;
    bool terminal_output_observed = false;
    bool terminal_first_text_frame_produced = false;
    bool output_capture_configured = false;
    bool start_invoked = false;
    bool hosted_worker_terminated = false;
    bool initialized = false;
    std::size_t lifecycle_sequence = 0U;
    std::size_t private_teardown_order = 0U;
    std::size_t remote_shutdown_order = 0U;
};

Terminal_surface_runtime_adapter::Terminal_surface_runtime_adapter(
    Terminal_worker_surface_configuration configuration,
    std::optional<Terminal_worker_output_capture_configuration> output_capture,
    Terminal_worker_remote_sink& remote_sink,
    Terminal_worker_gui_dispatcher& gui_dispatcher,
    Terminal_worker_termination& termination,
    Terminal_worker_complete_settings_sink& complete_settings_sink,
    Terminal_worker_diagnostic_observation_sink& diagnostic_sink,
    Terminal_native_start_admission_hook admission_hook)
:
    m_impl(std::make_unique<Impl>(
        std::move(configuration),
        std::move(output_capture),
        remote_sink,
        gui_dispatcher,
        termination,
        complete_settings_sink,
        diagnostic_sink,
        std::move(admission_hook)))
{}

Terminal_surface_runtime_adapter::~Terminal_surface_runtime_adapter()
{
    shutdown();
}

Terminal_worker_initialization_result
Terminal_surface_runtime_adapter::initialize()
{
    if (m_impl->initialized) {
        return Terminal_worker_initialization_result::ALREADY_INITIALIZED;
    }
    if (!valid_configuration(m_impl->configuration) ||
        !valid_output_capture(m_impl->output_capture))
    {
        return Terminal_worker_initialization_result::INVALID_CONFIGURATION;
    }

    initialize_worker_qml_resource();

    vnm::Remote_ui_surface_runtime_config remote_configuration;
    remote_configuration.qml_url = QUrl(QString::fromLatin1(k_worker_qml_url));
    remote_configuration.logical_width = m_impl->configuration.logical_width;
    remote_configuration.logical_height = m_impl->configuration.logical_height;
    remote_configuration.max_width =
        m_impl->configuration.maximum_physical_width;
    remote_configuration.max_height =
        m_impl->configuration.maximum_physical_height;
    remote_configuration.scale_factor = m_impl->configuration.scale_factor;
    remote_configuration.invoke_on_gui_thread =
        [this](
            Qt::ConnectionType connection_type,
            const std::function<void()>& function)
        {
            const Terminal_gui_dispatch_kind kind =
                connection_type == Qt::BlockingQueuedConnection
                ? Terminal_gui_dispatch_kind::BLOCKING
                : Terminal_gui_dispatch_kind::QUEUED;
            return m_impl->dispatch(kind, function);
        };
    remote_configuration.after_load =
        [this](QObject* root) {
            m_impl->construct_private_root(root);
        };
    remote_configuration.after_event_batch =
        [this](QObject*) {
            if (m_impl->surface == nullptr ||
                (m_impl->search_bar != nullptr &&
                 m_impl->search_bar->is_visible()))
            {
                return m_impl->surface != nullptr;
            }
            m_impl->surface->forceActiveFocus(Qt::OtherFocusReason);
            return true;
        };

    if (!m_impl->remote_runtime.initialize(
            remote_configuration,
            m_impl->sink_bridge.surface_sink()))
    {
        m_impl->sink_bridge.clear();
        return Terminal_worker_initialization_result::REMOTE_RUNTIME_FAILED;
    }
    if (!m_impl->root_callback_observed || m_impl->root_item == nullptr) {
        shutdown();
        return Terminal_worker_initialization_result::PRIVATE_ROOT_FAILED;
    }
    if (!m_impl->private_root_ready || m_impl->surface == nullptr ||
        m_impl->scrollbar == nullptr ||
        m_impl->search_bar == nullptr ||
        m_impl->settings_window == nullptr ||
        m_impl->shortcut_filter == nullptr ||
        m_impl->surface_construction_count != 1U)
    {
        shutdown();
        return Terminal_worker_initialization_result::SURFACE_FAILED;
    }
    m_impl->initialized = true;
    return Terminal_worker_initialization_result::READY;
}

bool Terminal_surface_runtime_adapter::forward_input(
    const Terminal_remote_input_message& message)
{
    if (!m_impl->initialized) {
        return false;
    }
    m_impl->remote_runtime.enqueue_input(remote_input(message));
    return true;
}

bool Terminal_surface_runtime_adapter::forward_state(
    const Terminal_remote_state_message& message)
{
    if (!m_impl->initialized) {
        return false;
    }
    m_impl->remote_runtime.enqueue_state(remote_state(message));
    return true;
}

bool Terminal_surface_runtime_adapter::request_present()
{
    if (!m_impl->initialized) {
        return false;
    }
    m_impl->remote_runtime.request_present();
    return true;
}

Terminal_worker_message_submission_result
Terminal_surface_runtime_adapter::submit_message(
    std::span<const std::uint8_t> message_utf8)
{
    if (!m_impl->initialized || m_impl->surface == nullptr) {
        return {
            Terminal_worker_message_submission_outcome::NOT_RUNNING,
            {},
        };
    }
    const QByteArray message(
        reinterpret_cast<const char*>(message_utf8.data()),
        static_cast<qsizetype>(message_utf8.size()));
    const vnm_terminal::Terminal_message_submission_result result =
        m_impl->surface->submit_utf8_message(message);
    return {
        workspace_message_outcome(result.outcome),
        result.error.toStdString(),
    };
}

void Terminal_surface_runtime_adapter::shutdown()
{
    if (m_impl == nullptr) {
        return;
    }
    if (m_impl->surface != nullptr || m_impl->scrollbar != nullptr) {
        (void)m_impl->dispatch(
            Terminal_gui_dispatch_kind::BLOCKING,
            [this] {
                m_impl->destroy_private_objects();
                m_impl->private_teardown_order =
                    ++m_impl->lifecycle_sequence;
            });
    }
    m_impl->initialized = false;
    m_impl->remote_runtime.shutdown();
    m_impl->remote_shutdown_order = ++m_impl->lifecycle_sequence;
    m_impl->sink_bridge.clear();
}

bool Terminal_surface_runtime_adapter::initialize_remote_runtime()
{
    const Terminal_worker_initialization_result result = initialize();
    return result == Terminal_worker_initialization_result::READY ||
        result == Terminal_worker_initialization_result::ALREADY_INITIALIZED;
}

bool Terminal_surface_runtime_adapter::initialize_root()
{
    return m_impl->private_root_ready && m_impl->root_item != nullptr;
}

bool Terminal_surface_runtime_adapter::construct_terminal_settings()
{
    return m_impl->settings.has_value();
}

bool Terminal_surface_runtime_adapter::construct_terminal_surface()
{
    return m_impl->surface != nullptr &&
        m_impl->surface_construction_count == 1U;
}

bool Terminal_surface_runtime_adapter::construct_terminal_scrollbar()
{
    return m_impl->scrollbar != nullptr;
}

bool Terminal_surface_runtime_adapter::connect_remote_ui()
{
    return m_impl->remote_runtime.is_initialized();
}

bool Terminal_surface_runtime_adapter::connect_row_timestamps()
{
    return m_impl->private_root_ready;
}

Structured_start_result Terminal_surface_runtime_adapter::start_terminal(
    const Terminal_worker_start_projection& projection)
{
    if (m_impl->start_invoked || m_impl->surface == nullptr ||
        m_impl->surface->thread() != QThread::currentThread())
    {
        return {};
    }
    m_impl->start_invoked = true;
    ++m_impl->structured_start_call_count;
    if (m_impl->admission_hook &&
        m_impl->admission_hook() == Terminal_native_start_admission::CANCEL)
    {
        return {
            false,
            false,
            Structured_start_determinacy::DETERMINATE,
            true,
        };
    }
    std::optional<Terminal_process_start_request> request =
        terminal_start_request(projection);
    if (!request) {
        return {};
    }

    const vnm_terminal::Terminal_process_start_result result =
        m_impl->surface->start_terminal(std::move(*request));
    return {
        result.accepted,
        result.native_dispatch_occurred,
        workspace_determinacy(result.determinacy),
        false,
    };
}

bool Terminal_surface_runtime_adapter::observe_running()
{
    if (m_impl->surface == nullptr) {
        return false;
    }
    const VNM_TerminalSurface::Process_state state =
        m_impl->surface->process_state();
    return m_impl->process_started_observed ||
        state == VNM_TerminalSurface::Process_state::RUNNING ||
        state == VNM_TerminalSurface::Process_state::EXITED;
}

Terminal_worker_exit_observation
Terminal_surface_runtime_adapter::observe_exit()
{
    if (m_impl->surface == nullptr) {
        return {};
    }
    if (m_impl->exit_observation) {
        return *m_impl->exit_observation;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    const QMetaObject::Connection exit_connection = QObject::connect(
        m_impl->surface.data(),
        &VNM_TerminalSurface::process_exited,
        &loop,
        &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(k_exit_observation_timeout);
    loop.exec();
    QObject::disconnect(exit_connection);
    return m_impl->exit_observation.value_or(
        Terminal_worker_exit_observation{});
}

void Terminal_surface_runtime_adapter::terminate_hosted_worker()
{
    if (m_impl->surface != nullptr &&
        (m_impl->surface->process_state() ==
             VNM_TerminalSurface::Process_state::STARTING ||
         m_impl->surface->process_state() ==
             VNM_TerminalSurface::Process_state::RUNNING))
    {
        m_impl->surface->terminate_process();
        if (!m_impl->exit_observation) {
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            const QMetaObject::Connection exit_connection = QObject::connect(
                m_impl->surface.data(),
                &VNM_TerminalSurface::process_exited,
                &loop,
                &QEventLoop::quit);
            QObject::connect(
                &timeout,
                &QTimer::timeout,
                &loop,
                &QEventLoop::quit);
            timeout.start(k_termination_observation_timeout);
            loop.exec();
            QObject::disconnect(exit_connection);
        }
    }
    if (!m_impl->hosted_worker_terminated) {
        m_impl->hosted_worker_terminated = true;
        m_impl->termination.terminate_hosted_worker();
    }
}

bool Terminal_surface_runtime_adapter::test_inject_timestamp_request()
{
    if (m_impl->surface == nullptr || m_impl->root_item == nullptr) {
        return false;
    }
    emit m_impl->surface->row_timestamp_tooltip_requested(
        QRectF(20.0, 30.0, 120.0, 14.0),
        QDateTime::fromMSecsSinceEpoch(1000, Qt::UTC));
    return m_impl->root_item->property("timestampVisible").toBool() &&
        !m_impl->root_item->property("timestampText").toString().isEmpty();
}

bool Terminal_surface_runtime_adapter::test_inject_output_activity()
{
    if (m_impl->surface == nullptr) {
        return false;
    }
    emit m_impl->surface->output_activity();
    return true;
}

bool Terminal_surface_runtime_adapter::test_inject_backend_error()
{
    if (m_impl->surface == nullptr) {
        return false;
    }
    emit m_impl->surface->backend_error(
        static_cast<VNM_TerminalSurface::Backend_error_code>(0),
        QStringLiteral("synthetic\nbackend diagnostic"));
    return true;
}

bool Terminal_surface_runtime_adapter::test_set_font_size(double font_size)
{
    if (m_impl->surface == nullptr || !std::isfinite(font_size) ||
        font_size <= 0.0)
    {
        return false;
    }
    m_impl->surface->set_font_size(font_size);
    return true;
}

Terminal_surface_runtime_adapter::Test_observation
Terminal_surface_runtime_adapter::test_observation() const
{
    return {
        m_impl->surface_construction_count,
        m_impl->structured_start_call_count,
        m_impl->surface != nullptr,
        m_impl->scrollbar != nullptr,
        m_impl->search_bar != nullptr,
        m_impl->settings_window != nullptr,
        m_impl->shortcut_filter != nullptr,
        m_impl->output_capture_configured,
        m_impl->remote_runtime.is_initialized(),
        m_impl->root_item != nullptr &&
            m_impl->root_item->property("timestampVisible").toBool(),
        m_impl->surface != nullptr && m_impl->surface->hasActiveFocus(),
        m_impl->surface == nullptr ? 0.0 : m_impl->surface->width(),
        m_impl->surface == nullptr ? 0.0 : m_impl->surface->height(),
        m_impl->scrollbar == nullptr ? 0.0 : m_impl->scrollbar->width(),
        m_impl->private_teardown_order,
        m_impl->remote_shutdown_order,
        m_impl->surface == nullptr
            ? 0
            : static_cast<int>(m_impl->surface->process_state()),
    };
}

} // namespace vnm::terminal_workspace::detail

namespace vnm::terminal_workspace {

struct Terminal_worker_runtime::Impl
{
    Impl(
        Terminal_worker_surface_configuration configuration,
        std::optional<Terminal_worker_output_capture_configuration>
            output_capture,
        Terminal_worker_remote_sink& remote_sink,
        Terminal_worker_gui_dispatcher& gui_dispatcher,
        Terminal_child_fact_transport& fact_transport,
        Terminal_worker_termination& termination,
        Terminal_worker_complete_settings_sink& complete_settings_sink,
        Terminal_worker_diagnostic_observation_sink& diagnostic_sink)
    :
        adapter(
            std::move(configuration),
            std::move(output_capture),
            remote_sink,
            gui_dispatcher,
            termination,
            complete_settings_sink,
            diagnostic_sink),
        coordinator(adapter, fact_transport)
    {}

    detail::Terminal_surface_runtime_adapter adapter;
    detail::Terminal_worker_coordinator coordinator;
    bool initialized = false;
};

Terminal_worker_runtime::Terminal_worker_runtime(
    Terminal_worker_surface_configuration configuration,
    std::optional<Terminal_worker_output_capture_configuration> output_capture,
    Terminal_worker_remote_sink& remote_sink,
    Terminal_worker_gui_dispatcher& gui_dispatcher,
    Terminal_child_fact_transport& fact_transport,
    Terminal_worker_termination& termination,
    Terminal_worker_complete_settings_sink& complete_settings_sink,
    Terminal_worker_diagnostic_observation_sink& diagnostic_sink)
:
    m_impl(std::make_unique<Impl>(
        std::move(configuration),
        std::move(output_capture),
        remote_sink,
        gui_dispatcher,
        fact_transport,
        termination,
        complete_settings_sink,
        diagnostic_sink))
{}

Terminal_worker_runtime::~Terminal_worker_runtime()
{
    shutdown();
}

Terminal_worker_initialization_result Terminal_worker_runtime::initialize()
{
    const Terminal_worker_initialization_result result =
        m_impl->adapter.initialize();
    if (result == Terminal_worker_initialization_result::READY ||
        result == Terminal_worker_initialization_result::ALREADY_INITIALIZED)
    {
        m_impl->initialized = true;
    }
    return result;
}

Terminal_worker_run_result Terminal_worker_runtime::run(
    std::span<const std::uint8_t> serialized_request,
    Launch_platform platform,
    std::uint64_t hosted_generation,
    std::optional<std::vector<environment_policy::Environment_entry>>
        authorized_environment)
{
    if (!m_impl->initialized) {
        return Terminal_worker_run_result::NOT_INITIALIZED;
    }
    return m_impl->coordinator.run_preinitialized(
        serialized_request,
        platform,
        hosted_generation,
        std::move(authorized_environment));
}

Terminal_worker_message_submission_result Terminal_worker_runtime::submit_message(
    std::span<const std::uint8_t> message_utf8)
{
    return m_impl->adapter.submit_message(message_utf8);
}

bool Terminal_worker_runtime::forward_input(
    const Terminal_remote_input_message& message)
{
    return m_impl->adapter.forward_input(message);
}

bool Terminal_worker_runtime::forward_state(
    const Terminal_remote_state_message& message)
{
    return m_impl->adapter.forward_state(message);
}

bool Terminal_worker_runtime::request_present()
{
    return m_impl->adapter.request_present();
}

void Terminal_worker_runtime::shutdown()
{
    if (m_impl == nullptr) {
        return;
    }
    m_impl->initialized = false;
    m_impl->adapter.shutdown();
}

std::optional<Terminal_child_fact> Terminal_worker_runtime::current_fact() const
{
    return m_impl->coordinator.current_fact();
}

std::vector<Terminal_child_fact>
Terminal_worker_runtime::unacknowledged_facts() const
{
    return m_impl->coordinator.unacknowledged_facts();
}

bool Terminal_worker_runtime::replay_unacknowledged()
{
    return m_impl->coordinator.replay_unacknowledged();
}

void Terminal_worker_noop_complete_settings_sink::accept_complete_settings(
    const Terminal_worker_settings&)
{}

void Terminal_worker_noop_diagnostic_observation_sink::
terminal_output_activity()
{}

void Terminal_worker_noop_diagnostic_observation_sink::
terminal_first_text_frame_produced(
    const Terminal_worker_first_text_frame_observation&)
{}

void Terminal_worker_noop_diagnostic_observation_sink::terminal_backend_error(
    const Terminal_worker_backend_error_observation&)
{}

} // namespace vnm::terminal_workspace
