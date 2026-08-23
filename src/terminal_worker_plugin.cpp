#include "vnm_terminal_workspace/terminal_worker_runtime.h"
#include "vnm_terminal_workspace/terminal_worker_envelope.h"

#include "vnm_ls_remote_ui_adapter.h"
#include "vnm_plugin_contract.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QThread>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace workspace = vnm::terminal_workspace;

namespace {

constexpr char k_fact_provider_class[] =
    "vnm_terminal_workspace.terminal_facts";

void set_error(char* buffer, std::uint32_t capacity, const char* message)
{
    if (buffer == nullptr || capacity == 0U) {
        return;
    }
    const std::size_t size = std::min<std::size_t>(
        std::strlen(message),
        capacity - 1U);
    std::memcpy(buffer, message, size);
    buffer[size] = '\0';
}

class Worker_remote_sink final : public workspace::Terminal_worker_remote_sink
{
public:
    bool initialize(const ls_ui_callbacks_t* callbacks)
    {
        m_adapter.clear();
        if (!m_adapter.initialize(callbacks)) {
            m_sink = {};
            return false;
        }
        m_sink = m_adapter.surface_sink();
        return m_sink.create_surface != nullptr &&
            m_sink.begin_frame != nullptr &&
            m_sink.end_frame != nullptr &&
            m_sink.destroy_surface != nullptr;
    }

    void clear()
    {
        m_sink = {};
        m_adapter.clear();
    }

    std::optional<workspace::Terminal_remote_surface_identity> create_surface(
        const workspace::Terminal_remote_surface_descriptor& descriptor)
        override
    {
        vnm::vnm_remote_ui_surface_desc_t value{};
        value.width = descriptor.width;
        value.height = descriptor.height;
        value.pixel_format = descriptor.pixel_format;
        const vnm::vnm_remote_ui_surface_t surface =
            m_sink.create_surface(m_sink.context, &value);
        return surface == nullptr
            ? std::nullopt
            : std::optional<workspace::Terminal_remote_surface_identity>(
                reinterpret_cast<std::uintptr_t>(surface));
    }

    std::optional<workspace::Terminal_remote_frame_buffer> begin_frame(
        workspace::Terminal_remote_surface_identity surface,
        std::int32_t width,
        std::int32_t height) override
    {
        std::int32_t stride = 0;
        std::uint8_t* const bytes = m_sink.begin_frame(
            m_sink.context,
            reinterpret_cast<vnm::vnm_remote_ui_surface_t>(
                static_cast<std::uintptr_t>(surface)),
            width,
            height,
            &stride);
        if (bytes == nullptr || stride <= 0 || height <= 0) {
            return std::nullopt;
        }
        return workspace::Terminal_remote_frame_buffer{
            std::span<std::uint8_t>(
                bytes,
                static_cast<std::size_t>(stride) *
                    static_cast<std::size_t>(height)),
            stride,
        };
    }

    void end_frame(
        workspace::Terminal_remote_surface_identity surface,
        std::span<const workspace::Terminal_remote_damage_rectangle> damage,
        std::uint32_t flags) override
    {
        std::vector<std::array<std::int32_t, 4>> rectangles;
        rectangles.reserve(damage.size());
        for (const auto& rectangle : damage) {
            rectangles.push_back({
                rectangle.x,
                rectangle.y,
                rectangle.width,
                rectangle.height,
            });
        }
        m_sink.end_frame(
            m_sink.context,
            reinterpret_cast<vnm::vnm_remote_ui_surface_t>(
                static_cast<std::uintptr_t>(surface)),
            static_cast<std::uint32_t>(rectangles.size()),
            reinterpret_cast<const std::int32_t (*)[4]>(rectangles.data()),
            flags);
    }

    void destroy_surface(
        workspace::Terminal_remote_surface_identity surface) override
    {
        m_sink.destroy_surface(
            m_sink.context,
            reinterpret_cast<vnm::vnm_remote_ui_surface_t>(
                static_cast<std::uintptr_t>(surface)));
    }

    void set_cursor(
        workspace::Terminal_remote_surface_identity surface,
        std::int32_t cursor_shape) override
    {
        if (m_sink.set_cursor != nullptr) {
            m_sink.set_cursor(
                m_sink.context,
                reinterpret_cast<vnm::vnm_remote_ui_surface_t>(
                    static_cast<std::uintptr_t>(surface)),
                cursor_shape);
        }
    }

    workspace::Terminal_remote_initial_state initial_state() override
    {
        workspace::Terminal_remote_initial_state result;
        if (m_sink.get_initial_state == nullptr) {
            return result;
        }
        vnm::vnm_ui_initial_state_t state{};
        if (m_sink.get_initial_state(m_sink.context, &state) == 0) {
            return result;
        }
        if ((state.flags & vnm::k_ui_initial_state_logical_size) != 0U) {
            result.logical_width = state.logical_width;
            result.logical_height = state.logical_height;
        }
        if ((state.flags & vnm::k_ui_initial_state_scale_factor) != 0U) {
            result.scale_factor = state.scale_factor;
        }
        return result;
    }

private:
    vnm::Ls_remote_ui_adapter m_adapter;
    vnm::vnm_remote_ui_surface_sink_t m_sink{};
};

class Worker_gui_dispatcher final : public workspace::Terminal_worker_gui_dispatcher
{
public:
    bool dispatch(
        workspace::Terminal_gui_dispatch_kind kind,
        const std::function<void()>& function) override
    {
        QCoreApplication* const application = QCoreApplication::instance();
        if (application == nullptr || !function) {
            return false;
        }
        if (QThread::currentThread() == application->thread()) {
            function();
            return true;
        }
        return QMetaObject::invokeMethod(
            application,
            function,
            kind == workspace::Terminal_gui_dispatch_kind::BLOCKING
                ? Qt::BlockingQueuedConnection
                : Qt::QueuedConnection);
    }
};

class Worker_termination final : public workspace::Terminal_worker_termination
{
public:
    void terminate_hosted_worker() override
    {
        if (QCoreApplication* const application = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(
                application,
                []() {
                    QCoreApplication::exit(70);
                },
                Qt::QueuedConnection);
        }
    }
};

class Worker_fact_transport final : public workspace::Terminal_child_fact_transport
{
public:
    Worker_fact_transport(
        const vnm_host_callbacks_t& callbacks,
        std::string provider_namespace)
    :
        m_callbacks(callbacks),
        m_provider_namespace(std::move(provider_namespace))
    {}

    std::optional<std::pair<std::string, std::uint64_t>> worker_context()
    {
        const std::optional<QJsonObject> payload = call(
            VNM_PROVIDER_REQUEST_KIND_STATE,
            "worker_context",
            {});
        if (!payload) {
            return std::nullopt;
        }
        const QJsonValue identity =
            payload->value(QStringLiteral("session_identity"));
        const QJsonValue generation =
            payload->value(QStringLiteral("generation"));
        if (!identity.isString() || !generation.isString()) {
            return std::nullopt;
        }
        bool ok = false;
        const std::uint64_t value = generation.toString().toULongLong(&ok);
        if (!ok || value == 0U) {
            return std::nullopt;
        }
        return std::pair{
            identity.toString().toStdString(),
            value,
        };
    }

    workspace::Terminal_child_fact_delivery_result deliver(
        const workspace::Terminal_child_fact& fact) override
    {
        QJsonObject arguments{
            {QStringLiteral("fact_key"), QString::number(fact.fact_key)},
            {QStringLiteral("sequence"), QString::number(fact.sequence)},
            {QStringLiteral("kind"), static_cast<int>(fact.kind)},
            {QStringLiteral("error"), static_cast<int>(fact.error)},
            {QStringLiteral("native_dispatch_occurred"),
                fact.native_dispatch_occurred},
        };
        if (fact.exit_code) {
            arguments.insert(QStringLiteral("exit_code"), *fact.exit_code);
        }
        m_last_host_result = VNM_HOST_TRANSPORT_ERROR;
        const std::optional<QJsonObject> payload = call(
            VNM_PROVIDER_REQUEST_KIND_ACTION,
            "ingest_fact",
            arguments);
        if (!payload) {
            return {delivery_status(m_last_host_result), std::nullopt};
        }
        const QString acknowledgement = payload->value(
            QStringLiteral("acknowledgement")).toString();
        if (acknowledgement == QStringLiteral("accepted")) {
            return {
                workspace::Terminal_child_fact_delivery_status::DELIVERED,
                workspace::Terminal_child_fact_acknowledgement::ACCEPTED,
            };
        }
        if (acknowledgement == QStringLiteral("already_current")) {
            return {
                workspace::Terminal_child_fact_delivery_status::DELIVERED,
                workspace::Terminal_child_fact_acknowledgement::ALREADY_CURRENT,
            };
        }
        if (acknowledgement == QStringLiteral("stale_generation")) {
            return {
                workspace::Terminal_child_fact_delivery_status::DELIVERED,
                workspace::Terminal_child_fact_acknowledgement::STALE_GENERATION,
            };
        }
        return {
            workspace::Terminal_child_fact_delivery_status::DELIVERED,
            workspace::Terminal_child_fact_acknowledgement::REJECTED,
        };
    }

private:
    static workspace::Terminal_child_fact_delivery_status delivery_status(
        std::int32_t host_result)
    {
        switch (host_result)
        {
        case VNM_HOST_UNAVAILABLE:
        case VNM_HOST_CAPABILITY_MISSING:
            return workspace::Terminal_child_fact_delivery_status::
                HANDLER_UNAVAILABLE;
        case VNM_HOST_BACKPRESSURE:
            return workspace::Terminal_child_fact_delivery_status::BACKPRESSURE;
        case VNM_HOST_TRANSPORT_ERROR:
            return workspace::Terminal_child_fact_delivery_status::INDETERMINATE;
        default:
            return workspace::Terminal_child_fact_delivery_status::INDETERMINATE;
        }
    }

    std::optional<QJsonObject> call(
        std::uint32_t request_kind,
        const char* operation,
        const QJsonObject& arguments)
    {
        if (m_callbacks.resolve_provider_lease == nullptr ||
            m_callbacks.get_provider_lease == nullptr ||
            m_callbacks.call_provider_action == nullptr ||
            m_callbacks.read_provider_state == nullptr)
        {
            m_last_host_result = VNM_HOST_UNAVAILABLE;
            return std::nullopt;
        }

        std::uint64_t lease_handle = 0U;
        m_last_host_result = m_callbacks.resolve_provider_lease(
            m_callbacks.host_user_data,
            m_provider_namespace.c_str(),
            k_fact_provider_class,
            "",
            &lease_handle);
        if (m_last_host_result != VNM_HOST_OK || lease_handle == 0U) {
            return std::nullopt;
        }
        vnm_provider_lease_t lease{};
        lease.struct_size = sizeof(lease);
        m_last_host_result = m_callbacks.get_provider_lease(
            m_callbacks.host_user_data,
            lease_handle,
            &lease);
        if (m_last_host_result != VNM_HOST_OK) {
            return std::nullopt;
        }

        const QByteArray payload =
            QJsonDocument(arguments).toJson(QJsonDocument::Compact);
        vnm_provider_request_t request{};
        request.struct_size = sizeof(request);
        request.provider_lease_handle = lease_handle;
        request.lease_generation = lease.lease_generation;
        request.provider_namespace = m_provider_namespace.c_str();
        request.provider_class = k_fact_provider_class;
        request.provider_instance = "";
        request.operation_name = operation;
        request.payload_json = reinterpret_cast<const std::uint8_t*>(
            payload.constData());
        request.payload_json_len = static_cast<std::uint32_t>(payload.size());
        request.deadline_ns_from_now = 5'000'000'000ULL;
        request.product_correlation_id = "terminal-fact";
        request.request_kind = request_kind;
        vnm_provider_response_t response{};
        response.struct_size = sizeof(response);
        m_last_host_result = request_kind == VNM_PROVIDER_REQUEST_KIND_STATE
            ? m_callbacks.read_provider_state(
                m_callbacks.host_user_data,
                &request,
                &response)
            : m_callbacks.call_provider_action(
                m_callbacks.host_user_data,
                &request,
                &response);
        if (m_last_host_result != VNM_HOST_OK || response.result_code != 0 ||
            response.payload_json == nullptr)
        {
            return std::nullopt;
        }
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(
            QByteArray(
                reinterpret_cast<const char*>(response.payload_json),
                static_cast<qsizetype>(response.payload_json_len)),
            &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            return std::nullopt;
        }
        return document.object();
    }

    vnm_host_callbacks_t m_callbacks{};
    std::string m_provider_namespace;
    std::int32_t m_last_host_result = VNM_HOST_TRANSPORT_ERROR;
};

struct Worker_state
{
    vnm_host_callbacks_t callbacks{};
    QByteArray request;
    workspace::Launch_platform platform = workspace::Launch_platform::WINDOWS;
    workspace::Terminal_worker_surface_configuration surface_configuration;
    std::optional<workspace::Terminal_worker_output_capture_configuration>
        output_capture;
    std::optional<std::vector<vnm::environment_policy::Environment_entry>>
        authorized_environment;
    std::unique_ptr<Worker_fact_transport> fact_transport;
    Worker_remote_sink remote_sink;
    Worker_gui_dispatcher gui_dispatcher;
    Worker_termination termination;
    workspace::Terminal_worker_noop_complete_settings_sink settings_sink;
    workspace::Terminal_worker_noop_diagnostic_observation_sink diagnostic_sink;
    std::unique_ptr<workspace::Terminal_worker_runtime> runtime;
    bool run_scheduled = false;
};

void clear_string(std::string& value)
{
    volatile char* bytes = value.data();
    for (std::size_t index = 0U; index < value.size(); ++index) {
        bytes[index] = '\0';
    }
    value.clear();
}

void clear_bytes(std::vector<std::uint8_t>& value)
{
    volatile std::uint8_t* bytes = value.data();
    for (std::size_t index = 0U; index < value.size(); ++index) {
        bytes[index] = 0U;
    }
    value.clear();
}

void clear_environment(
    std::optional<std::vector<vnm::environment_policy::Environment_entry>>&
        environment)
{
    if (!environment) {
        return;
    }
    for (vnm::environment_policy::Environment_entry& entry : *environment) {
        clear_string(entry.value);
    }
    environment.reset();
}

bool initialize_state(
    Worker_state& state,
    const vnm_plugin_create_context_t* context)
{
    if (context == nullptr ||
        context->struct_size < sizeof(vnm_plugin_create_context_t) ||
        context->host_callbacks == nullptr ||
        context->host_callbacks->struct_size < sizeof(vnm_host_callbacks_t) ||
        context->params_json == nullptr)
    {
        return false;
    }
    workspace::Terminal_worker_envelope_result decoded =
        workspace::decode_terminal_worker_envelope(context->params_json);
    if (!decoded.envelope) {
        return false;
    }
    workspace::Terminal_worker_envelope& envelope = *decoded.envelope;
    // The neutral package policy has no product environment names. A product
    // wrapper must validate its own fixed compile-time allowlist before it
    // constructs the shared runtime.
    if (envelope.authorized_environment &&
        !envelope.authorized_environment->empty())
    {
        clear_bytes(envelope.serialized_request);
        clear_environment(envelope.authorized_environment);
        return false;
    }
    state.request = QByteArray(
        reinterpret_cast<const char*>(envelope.serialized_request.data()),
        static_cast<qsizetype>(envelope.serialized_request.size()));
    clear_bytes(envelope.serialized_request);
    state.platform = envelope.platform;
    state.surface_configuration = std::move(envelope.surface_configuration);
    state.output_capture = std::move(envelope.output_capture);
    state.authorized_environment =
        std::move(envelope.authorized_environment);
    state.callbacks = *context->host_callbacks;
    state.fact_transport = std::make_unique<Worker_fact_transport>(
        state.callbacks,
        std::move(envelope.provider_namespace));
    return true;
}

workspace::Terminal_remote_input_message input_message(
    const ls_remote_ui_input_event_t& event)
{
    workspace::Terminal_remote_input_message message;
    message.event_type = event.event_type;
    message.modifiers = event.modifiers;
    message.x = event.x;
    message.y = event.y;
    message.button = event.button;
    message.buttons = event.buttons;
    message.key = event.key;
    message.scroll_dx = event.scroll_dx;
    message.scroll_dy = event.scroll_dy;
    std::memcpy(message.text_utf8.data(), event.text_utf8, message.text_utf8.size());
    message.timestamp = event.timestamp;
    return message;
}

workspace::Terminal_remote_state_message state_message(
    const ls_remote_ui_state_event_t& event)
{
    return {
        event.state_type,
        event.width,
        event.height,
        event.scale_factor,
        event.value,
    };
}

} // namespace

extern "C" {

static const vnm_plugin_descriptor_t k_descriptor = {
    sizeof(vnm_plugin_descriptor_t),
    VNM_PLUGIN_ABI_REVISION,
    "vnm_terminal_workspace.terminal_worker",
    "vnm_terminal_workspace",
    "0.1.0",
    "Terminal workspace worker",
    "Neutral hosted terminal worker",
    VNM_PLUGIN_CAP_REMOTE_UI,
};

VNM_API const vnm_plugin_descriptor_t* VNM_CALL vnm_get_plugin_descriptor(void)
{
    return &k_descriptor;
}

VNM_API int32_t VNM_CALL vnm_create(
    const vnm_plugin_create_context_t* context,
    std::uint32_t error_capacity,
    char* error_buffer,
    void** instance)
{
    if (instance == nullptr) {
        set_error(error_buffer, error_capacity, "missing_instance_output");
        return VNM_CREATE_INVALID_CONTEXT;
    }
    *instance = nullptr;
    auto state = std::make_unique<Worker_state>();
    if (!initialize_state(*state, context)) {
        set_error(error_buffer, error_capacity, "invalid_worker_payload");
        return VNM_CREATE_INVALID_PAYLOAD;
    }
    *instance = state.release();
    return VNM_CREATE_OK;
}

VNM_API int32_t VNM_CALL vnm_on_stop(void* instance)
{
    auto* state = static_cast<Worker_state*>(instance);
    if (state != nullptr && state->runtime) {
        state->runtime->shutdown();
    }
    return VNM_HOST_OK;
}

VNM_API void VNM_CALL vnm_reset(void* instance)
{
    auto* state = static_cast<Worker_state*>(instance);
    if (state != nullptr && state->runtime) {
        static_cast<void>(state->runtime->request_present());
    }
}

VNM_API void VNM_CALL vnm_destroy(void* instance)
{
    auto* state = static_cast<Worker_state*>(instance);
    if (state == nullptr) {
        return;
    }
    if (state->runtime) {
        state->runtime->shutdown();
        state->runtime.reset();
    }
    state->request.fill('\0');
    state->request.clear();
    clear_environment(state->authorized_environment);
    state->remote_sink.clear();
    delete state;
}

LS_API void LS_CALL ls_remote_ui_init(
    void* instance,
    const ls_ui_callbacks_t* callbacks)
{
    auto* state = static_cast<Worker_state*>(instance);
    if (state == nullptr || state->run_scheduled ||
        !state->remote_sink.initialize(callbacks))
    {
        return;
    }
    const auto context = state->fact_transport->worker_context();
    if (!context) {
        state->termination.terminate_hosted_worker();
        return;
    }
    state->runtime = std::make_unique<workspace::Terminal_worker_runtime>(
        std::move(state->surface_configuration),
        std::move(state->output_capture),
        state->remote_sink,
        state->gui_dispatcher,
        *state->fact_transport,
        state->termination,
        state->settings_sink,
        state->diagnostic_sink);
    if (state->runtime->initialize() !=
        workspace::Terminal_worker_initialization_result::READY)
    {
        state->termination.terminate_hosted_worker();
        return;
    }
    state->run_scheduled = true;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [state, generation = context->second]() {
            if (state->runtime == nullptr) {
                return;
            }
            static_cast<void>(state->runtime->run(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(
                        state->request.constData()),
                    static_cast<std::size_t>(state->request.size())),
                state->platform,
                generation,
                std::move(state->authorized_environment)));
            state->request.fill('\0');
            state->request.clear();
        },
        Qt::QueuedConnection);
}

LS_API void LS_CALL ls_on_remote_ui_input(
    void* instance,
    const ls_remote_ui_input_event_t* event)
{
    auto* state = static_cast<Worker_state*>(instance);
    if (state != nullptr && state->runtime && event != nullptr &&
        event->struct_size >= sizeof(ls_remote_ui_input_event_t))
    {
        static_cast<void>(state->runtime->forward_input(input_message(*event)));
    }
}

LS_API void LS_CALL ls_on_remote_ui_state(
    void* instance,
    const ls_remote_ui_state_event_t* event)
{
    auto* state = static_cast<Worker_state*>(instance);
    if (state != nullptr && state->runtime && event != nullptr &&
        event->struct_size >= sizeof(ls_remote_ui_state_event_t))
    {
        static_cast<void>(state->runtime->forward_state(state_message(*event)));
    }
}

LS_API void LS_CALL ls_remote_ui_shutdown(void* instance)
{
    auto* state = static_cast<Worker_state*>(instance);
    if (state != nullptr && state->runtime) {
        state->runtime->shutdown();
    }
}

} // extern "C"
