#include "terminal_hosted_owner.h"

#include "vnm_terminal_workspace/terminal_worker_composition.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QVariantList>
#include <QVariantMap>

#include "remote_ui_common/vnm_remote_ui_protocol.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace vnm::terminal_workspace::detail {
namespace {

constexpr char k_fact_provider_class[] =
    "vnm_terminal_workspace.terminal_facts";
QString acknowledgement_name(Terminal_child_fact_acknowledgement value)
{
    switch (value)
    {
    case Terminal_child_fact_acknowledgement::ACCEPTED:
        return QStringLiteral("accepted");
    case Terminal_child_fact_acknowledgement::ALREADY_CURRENT:
        return QStringLiteral("already_current");
    case Terminal_child_fact_acknowledgement::STALE_GENERATION:
        return QStringLiteral("stale_generation");
    case Terminal_child_fact_acknowledgement::REJECTED:
        return QStringLiteral("rejected");
    }
    return QStringLiteral("rejected");
}

std::optional<Terminal_child_fact_kind> fact_kind(int value)
{
    if (value < static_cast<int>(Terminal_child_fact_kind::STARTED) ||
        value > static_cast<int>(Terminal_child_fact_kind::EXITED))
    {
        return std::nullopt;
    }
    return static_cast<Terminal_child_fact_kind>(value);
}

std::optional<Terminal_child_fact_error> fact_error(int value)
{
    if (value < static_cast<int>(Terminal_child_fact_error::NONE) ||
        value > static_cast<int>(
            Terminal_child_fact_error::RUNNING_OBSERVATION))
    {
        return std::nullopt;
    }
    return static_cast<Terminal_child_fact_error>(value);
}

std::optional<std::uint64_t> unsigned_value(const QVariant& value)
{
    bool ok = false;
    const qulonglong result = value.toULongLong(&ok);
    return ok ? std::optional<std::uint64_t>(result) : std::nullopt;
}

void clear_string(std::string& value)
{
    volatile char* bytes = value.data();
    for (std::size_t index = 0U; index < value.size(); ++index) {
        bytes[index] = '\0';
    }
    value.clear();
}

void clear_string(QString& value)
{
    value.fill(QChar{});
    value.clear();
}

void clear_environment_values(
    std::optional<std::vector<environment_policy::Environment_entry>>& entries)
{
    if (!entries) {
        return;
    }
    for (environment_policy::Environment_entry& entry : *entries) {
        clear_string(entry.value);
    }
    entries.reset();
}

void clear_bytes(std::vector<std::uint8_t>& bytes)
{
    volatile std::uint8_t* data = bytes.data();
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        data[index] = 0U;
    }
    bytes.clear();
}

environment_policy::Environment_platform environment_platform(
    Launch_platform platform)
{
    return platform == Launch_platform::WINDOWS
        ? environment_policy::Environment_platform::WINDOWS
        : environment_policy::Environment_platform::POSIX;
}

bool base_contains_bound_product_environment_name(
    const Terminal_launch_request& request,
    Launch_platform platform,
    std::span<const std::string> product_environment_names)
{
    const auto environment = environment_platform(platform);
    return std::any_of(
        request.base_environment.begin(),
        request.base_environment.end(),
        [environment, product_environment_names](const auto& entry) {
            return std::any_of(
                product_environment_names.begin(),
                product_environment_names.end(),
                [environment, &entry](const std::string& fixed_name) {
                    return environment_policy::environment_names_equal(
                        entry.name,
                        fixed_name,
                        environment);
                });
        });
}

Terminal_worker_message_submission_outcome message_outcome(
    VNM_Hosted_worker_message_outcome outcome)
{
    using Source = VNM_Hosted_worker_message_outcome;
    using Target = Terminal_worker_message_submission_outcome;
    switch (outcome)
    {
    case Source::ACCEPTED: return Target::ACCEPTED;
    case Source::INVALID_UTF8: return Target::INVALID_UTF8;
    case Source::INVALID_MESSAGE: return Target::INVALID_MESSAGE;
    case Source::EMPTY_MESSAGE: return Target::EMPTY_MESSAGE;
    case Source::MESSAGE_TOO_LARGE: return Target::MESSAGE_TOO_LARGE;
    case Source::NOT_RUNNING: return Target::NOT_RUNNING;
    case Source::CLOSING: return Target::CLOSING;
    case Source::CAPABILITY_MISSING: return Target::CAPABILITY_MISSING;
    case Source::STALE_GENERATION: return Target::STALE_GENERATION;
    case Source::BACKPRESSURE: return Target::BACKPRESSURE;
    case Source::QUEUE_LIMIT: return Target::QUEUE_LIMIT;
    case Source::BACKEND_REJECTED: return Target::BACKEND_REJECTED;
    case Source::WORKER_REJECTED: return Target::WORKER_REJECTED;
    case Source::DEADLINE_EXPIRED: return Target::DEADLINE_EXPIRED;
    case Source::INDETERMINATE: return Target::INDETERMINATE;
    }
    return Target::INDETERMINATE;
}

class Sensitive_environment_guard
{
public:
    explicit Sensitive_environment_guard(
        std::optional<
            std::vector<environment_policy::Environment_entry>>& environment)
    :
        m_environment(environment)
    {}

    ~Sensitive_environment_guard()
    {
        clear_environment_values(m_environment);
    }

    Sensitive_environment_guard(const Sensitive_environment_guard&) = delete;
    Sensitive_environment_guard& operator=(
        const Sensitive_environment_guard&) = delete;

private:
    std::optional<std::vector<environment_policy::Environment_entry>>&
        m_environment;
};

class Sensitive_string_guard
{
public:
    explicit Sensitive_string_guard(std::string& value) : m_value(value) {}
    ~Sensitive_string_guard() { clear_string(m_value); }

    Sensitive_string_guard(const Sensitive_string_guard&) = delete;
    Sensitive_string_guard& operator=(const Sensitive_string_guard&) = delete;

private:
    std::string& m_value;
};

} // namespace

struct Terminal_hosted_owner::Live_session
{
    std::string session_identity;
    std::string launch_request_identity;
    std::uint64_t framework_session_id = 0U;
    std::uint64_t generation = 0U;
    std::shared_ptr<Terminal_lifetime_capability> lifetime_capability;
    std::unique_ptr<VNM_Hosted_worker_session> session;
    QMetaObject::Connection phase_connection;
    QMetaObject::Connection start_connection;
    QMetaObject::Connection close_connection;
    QMetaObject::Connection crash_settlement_connection;
    QMetaObject::Connection attachment_connection;
    std::optional<Terminal_close_cause> close_cause;
    bool close_dispatch_pending = false;
    bool close_admission_in_progress = false;
    bool close_started = false;
};

Terminal_hosted_owner::Terminal_hosted_owner(
    Terminal_hosted_owner_configuration configuration,
    Terminal_hosted_owner_test_hooks test_hooks)
:
    m_configuration(std::move(configuration)),
    m_test_hooks(std::move(test_hooks)),
    m_router(std::make_shared<VNM_control_router>()),
    m_proxy_gate(
        m_core,
        [this](const std::string& session_identity,
               std::uint64_t generation,
               Terminal_close_cause cause) {
            return request_close(
                session_identity,
                generation,
                cause);
        })
{
    if (!m_configuration.encode_parameters) {
        m_configuration.package_id =
            Neutral_terminal_worker_package_policy::package_id;
        m_configuration.family_id =
            Neutral_terminal_worker_package_policy::family_id;
        m_configuration.capabilities.assign(
            Neutral_terminal_worker_package_policy::capabilities.begin(),
            Neutral_terminal_worker_package_policy::capabilities.end());
        m_configuration.product_environment_names.clear();
        m_configuration.encode_parameters = [](
            const Terminal_worker_envelope& envelope,
            std::string_view configuration)
                -> std::optional<std::string>
        {
            const auto decoded =
                Neutral_terminal_worker_package_policy::
                    decode_configuration(configuration);
            if (!decoded) {
                return std::nullopt;
            }
            auto encoded = encode_terminal_worker_fixed_parameters<
                Neutral_terminal_worker_package_policy>(envelope, *decoded);
            return encoded.error ==
                    Terminal_worker_fixed_parameters_error::NONE
                ? std::optional<std::string>(
                    std::move(encoded.serialized_parameters))
                : std::nullopt;
        };
    }
    register_fact_provider();
}

Terminal_hosted_owner::~Terminal_hosted_owner()
{
    m_provider_group.clear();
    m_sessions.clear();
    m_core.purge_receipts_for_shutdown();
}

Terminal_hosted_launch_result Terminal_hosted_owner::launch(
    std::span<const std::uint8_t> serialized_request,
    Launch_platform platform,
    std::optional<std::vector<environment_policy::Environment_entry>>
        authorized_environment,
    std::shared_ptr<Terminal_lifetime_capability> lifetime_capability,
    Terminal_worker_surface_configuration surface_configuration,
    std::optional<Terminal_worker_output_capture_configuration> output_capture,
    std::string canonical_product_configuration)
{
    Sensitive_environment_guard environment_guard(authorized_environment);
    Sensitive_string_guard configuration_guard(
        canonical_product_configuration);
    Launch_request_result decoded = decode_terminal_launch_request(
        serialized_request,
        platform);
    Sensitive_launch_request_guard request_guard(decoded);
    if (decoded.status == Launch_request_status::CANCELLED) {
        return {Terminal_hosted_launch_outcome::CANCELLED};
    }
    if (decoded.status != Launch_request_status::ACCEPTED || !decoded.request) {
        return {Terminal_hosted_launch_outcome::INVALID_REQUEST};
    }
    if (base_contains_bound_product_environment_name(
            *decoded.request,
            platform,
            m_configuration.product_environment_names))
    {
        return {Terminal_hosted_launch_outcome::INVALID_REQUEST};
    }

    const Terminal_launch_request& request = *decoded.request;
    const auto names_equal = [platform](
        std::string_view left,
        std::string_view right) {
        return environment_policy::environment_names_equal(
            left,
            right,
            environment_platform(platform));
    };
    if (authorized_environment) {
        for (std::size_t index = 0U;
             index < authorized_environment->size();
             ++index)
        {
            const std::string_view name = (*authorized_environment)[index].name;
            const bool fixed_name = std::any_of(
                m_configuration.product_environment_names.begin(),
                m_configuration.product_environment_names.end(),
                [name, &names_equal](const std::string& allowed) {
                    return names_equal(name, allowed);
                });
            const bool duplicate = std::any_of(
                authorized_environment->begin(),
                authorized_environment->begin() +
                    static_cast<std::ptrdiff_t>(index),
                [name, &names_equal](const auto& entry) {
                    return names_equal(name, entry.name);
                });
            if (!fixed_name || duplicate) {
                return {Terminal_hosted_launch_outcome::INVALID_REQUEST};
            }
        }
    }
    if (m_sessions.contains(request.session_id) ||
        m_core.custody(request.session_id))
    {
        return {
            Terminal_hosted_launch_outcome::ALREADY_OWNED,
            request.session_id,
        };
    }

    const std::uint64_t framework_session_id =
        numeric_session_identity(request.session_id);
    const auto existing =
        m_framework_session_identities.find(framework_session_id);
    if (framework_session_id == 0U ||
        (existing != m_framework_session_identities.end() &&
         existing->second != request.session_id))
    {
        return {Terminal_hosted_launch_outcome::INVALID_REQUEST};
    }

    auto live = std::make_unique<Live_session>();
    live->session_identity = request.session_id;
    live->launch_request_identity = request.launch_request_id;
    live->framework_session_id = framework_session_id;
    live->lifetime_capability = std::move(lifetime_capability);
    live->session = std::make_unique<VNM_Hosted_worker_session>();
    live->session->set_host_executable_path(
        m_configuration.host_executable_path);
    live->session->set_worker_dll_path(m_configuration.worker_dll_path);
    QString start_payload = worker_payload(
        serialized_request,
        platform,
        surface_configuration,
        output_capture,
        canonical_product_configuration,
        authorized_environment);
    if (start_payload.isEmpty()) {
        return {
            Terminal_hosted_launch_outcome::INVALID_REQUEST,
            request.session_id,
        };
    }
    live->session->set_worker_start_payload(start_payload);
    clear_string(start_payload);
    live->session->set_session_id(framework_session_id);
    live->session->set_auto_restart_enabled(false);
    live->session->set_max_auto_restarts(0);
    live->session->set_provider_router(m_router);
    live->session->set_provider_namespace_grants({
        m_configuration.provider_namespace,
    });

    Live_session* const live_ptr = live.get();
    m_framework_session_identities.emplace(
        framework_session_id,
        request.session_id);
    m_sessions.emplace(request.session_id, std::move(live));
    connect_session(*live_ptr);

    const bool start_admitted = m_test_hooks.start_async
        ? m_test_hooks.start_async(*live_ptr->session)
        : live_ptr->session->start_async();
    if (!start_admitted) {
        const std::string identity = live_ptr->session_identity;
        const std::uint64_t generation = live_ptr->generation;
        if (generation != 0U && m_core.custody(identity)) {
            reserve_first_close_cause(
                *live_ptr,
                Terminal_close_cause::START_FAILED);
            static_cast<void>(commit_first_close_cause(*live_ptr));
            static_cast<void>(m_core.settle({
                identity,
                generation,
                std::nullopt,
                Terminal_cleanup_disposition::NOT_REQUIRED,
                std::nullopt,
            }, Terminal_owner_core::Time_point::clock::now()));
        }
        erase_settled_session(identity);
        return {
            Terminal_hosted_launch_outcome::HOST_START_REJECTED,
            identity,
            generation,
        };
    }
    if (live_ptr->generation == 0U ||
        !m_core.custody(live_ptr->session_identity))
    {
        static_cast<void>(m_test_hooks.close_async
            ? m_test_hooks.close_async(*live_ptr->session)
            : live_ptr->session->close_async());
        erase_settled_session(live_ptr->session_identity);
        return {
            Terminal_hosted_launch_outcome::GENERATION_NOT_ALLOCATED,
            request.session_id,
        };
    }
    return {
        Terminal_hosted_launch_outcome::ADMITTED,
        request.session_id,
        live_ptr->generation,
    };
}

Terminal_owner_update_result Terminal_hosted_owner::request_close(
    const std::string& session_identity,
    std::uint64_t generation,
    Terminal_close_cause cause)
{
    const auto custody = m_core.custody(session_identity);
    if (!custody || custody->generation != generation) {
        return Terminal_owner_update_result::STALE_GENERATION;
    }
    if (custody->first_close_cause) {
        return Terminal_owner_update_result::ALREADY_CURRENT;
    }
    const auto live = m_sessions.find(session_identity);
    if (live == m_sessions.end() || live->second->generation != generation) {
        return Terminal_owner_update_result::STALE_GENERATION;
    }
    if (live->second->close_started) {
        return Terminal_owner_update_result::ALREADY_CURRENT;
    }
    if (live->second->close_dispatch_pending ||
        live->second->close_admission_in_progress)
    {
        return Terminal_owner_update_result::REJECTED;
    }
    reserve_first_close_cause(*live->second, cause);
    return admit_reserved_close(*live->second);
}

Terminal_proxy_gate_outcome Terminal_hosted_owner::attach_existing(
    std::uint64_t caller_transport_process_id,
    VNM_viewer_authority_epoch expected_epoch,
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision)
{
    return m_proxy_gate.attach_existing(
        caller_transport_process_id,
        expected_epoch,
        session_identity,
        generation,
        attachment_revision);
}

Terminal_proxy_gate_outcome Terminal_hosted_owner::forward_input(
    std::uint64_t caller_transport_process_id,
    VNM_viewer_authority_epoch expected_epoch,
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision,
    const Terminal_remote_input_message& message)
{
    if (message.event_type < vnm::k_ui_input_mouse_move ||
        message.event_type > vnm::k_ui_input_mouse_double_click)
    {
        return Terminal_proxy_gate_outcome::INVALID_MESSAGE;
    }
    const auto live = m_sessions.find(session_identity);
    if (live == m_sessions.end() || live->second->generation != generation) {
        return Terminal_proxy_gate_outcome::NO_CUSTODY;
    }
    return m_proxy_gate.forward_input(
        caller_transport_process_id,
        expected_epoch,
        session_identity,
        generation,
        attachment_revision,
        [this, session = live->second.get(), message]() {
            send_input(*session, message);
        });
}

Terminal_proxy_gate_outcome Terminal_hosted_owner::forward_state(
    std::uint64_t caller_transport_process_id,
    VNM_viewer_authority_epoch expected_epoch,
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision,
    const Terminal_remote_state_message& message)
{
    if (message.state_type < vnm::k_ui_state_resize ||
        message.state_type > vnm::k_ui_state_dark_mode)
    {
        return Terminal_proxy_gate_outcome::INVALID_MESSAGE;
    }
    const auto live = m_sessions.find(session_identity);
    if (live == m_sessions.end() || live->second->generation != generation) {
        return Terminal_proxy_gate_outcome::NO_CUSTODY;
    }
    return m_proxy_gate.forward_input(
        caller_transport_process_id,
        expected_epoch,
        session_identity,
        generation,
        attachment_revision,
        [this, session = live->second.get(), message]() {
            send_state(*session, message);
        });
}

Terminal_hosted_message_submission_result Terminal_hosted_owner::submit_message(
    std::uint64_t caller_transport_process_id,
    VNM_viewer_authority_epoch expected_epoch,
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision,
    std::span<const std::uint8_t> message_utf8)
{
    Terminal_hosted_message_submission_result result;
    const auto live = m_sessions.find(session_identity);
    if (live == m_sessions.end() || live->second->generation != generation) {
        result.routing = Terminal_proxy_gate_outcome::NO_CUSTODY;
        return result;
    }
    result.routing = m_proxy_gate.submit_message(
        caller_transport_process_id,
        expected_epoch,
        session_identity,
        generation,
        attachment_revision,
        [this, &result, session = live->second.get(), message_utf8]() {
            result.submission = send_message(*session, message_utf8);
        });
    return result;
}

std::optional<Terminal_custody_snapshot> Terminal_hosted_owner::custody(
    const std::string& session_identity) const
{
    return m_core.custody(session_identity);
}

std::vector<Terminal_custody_snapshot> Terminal_hosted_owner::custodies() const
{
    return m_core.custodies();
}

std::size_t Terminal_hosted_owner::live_custody_count() const
{
    return m_core.live_custody_count();
}

Terminal_owner_core& Terminal_hosted_owner::core()
{
    return m_core;
}

Terminal_owner_proxy_gate& Terminal_hosted_owner::proxy_gate()
{
    return m_proxy_gate;
}

std::uint64_t Terminal_hosted_owner::numeric_session_identity(
    std::string_view identity)
{
    const QByteArray digest = QCryptographicHash::hash(
        QByteArray(identity.data(), static_cast<qsizetype>(identity.size())),
        QCryptographicHash::Sha256);
    if (digest.size() < static_cast<qsizetype>(sizeof(std::uint64_t))) {
        return 0U;
    }
    std::uint64_t result = 0U;
    for (int index = 0; index < static_cast<int>(sizeof(result)); ++index) {
        result = (result << 8U) |
            static_cast<unsigned char>(digest.at(index));
    }
    return result == 0U ? 1U : result;
}

Terminal_cleanup_disposition Terminal_hosted_owner::cleanup_disposition(
    VNM_Hosted_worker_cleanup_disposition disposition)
{
    switch (disposition)
    {
    case VNM_Hosted_worker_cleanup_disposition::NOT_REQUIRED:
        return Terminal_cleanup_disposition::NOT_REQUIRED;
    case VNM_Hosted_worker_cleanup_disposition::COMPLETE:
        return Terminal_cleanup_disposition::COMPLETE;
    case VNM_Hosted_worker_cleanup_disposition::RETAINED:
        return Terminal_cleanup_disposition::RETAINED;
    }
    return Terminal_cleanup_disposition::RETAINED;
}

QString Terminal_hosted_owner::worker_payload(
    std::span<const std::uint8_t> serialized_request,
    Launch_platform platform,
    const Terminal_worker_surface_configuration& surface_configuration,
    const std::optional<Terminal_worker_output_capture_configuration>&
        output_capture,
    std::string_view canonical_product_configuration,
    const std::optional<std::vector<environment_policy::Environment_entry>>&
        authorized_environment) const
{
    Terminal_worker_envelope envelope;
    envelope.provider_namespace =
        m_configuration.provider_namespace.toStdString();
    envelope.serialized_request.assign(
        serialized_request.begin(),
        serialized_request.end());
    envelope.platform = platform;
    envelope.surface_configuration = surface_configuration;
    envelope.output_capture = output_capture;
    envelope.authorized_environment = authorized_environment;
    std::optional<std::string> encoded = m_configuration.encode_parameters
        ? m_configuration.encode_parameters(
            envelope,
            canonical_product_configuration)
        : std::nullopt;
    clear_bytes(envelope.serialized_request);
    clear_environment_values(envelope.authorized_environment);
    if (!encoded) {
        return {};
    }
    QString result = QString::fromStdString(*encoded);
    clear_string(*encoded);
    return result;
}

void Terminal_hosted_owner::register_fact_provider()
{
    m_provider_group =
        m_router->make_provider_group(m_configuration.provider_namespace);
    auto provider = m_provider_group.provider_class(
        QString::fromLatin1(k_fact_provider_class));
    provider
        .label(QStringLiteral("Terminal workspace fact owner"))
        .description(QStringLiteral(
            "Generation-qualified neutral terminal worker fact owner"))
        .class_state(
            QStringLiteral("worker_context"),
            QStringLiteral("Read the authenticated worker generation"),
            [this](const VNM_control_call& call) {
                return worker_context(call);
            })
        .class_action(
            QStringLiteral("ingest_fact"),
            QStringLiteral("Apply one stable generation-qualified child fact"),
            [this](const VNM_control_call& call) {
                return ingest_worker_fact(call);
            });
    const VNM_control_registration_result result =
        m_provider_group.registration_result();
    if (!result.ok()) {
        m_provider_group.clear();
    }
}

void Terminal_hosted_owner::connect_session(Live_session& live)
{
    VNM_Hosted_worker_session* const session = live.session.get();
    live.phase_connection = QObject::connect(
        session,
        &VNM_Hosted_worker_session::phase_changed,
        session,
        [this, &live](VNM_Hosted_worker_session_phase phase) {
            if (phase != VNM_Hosted_worker_session_phase::STARTING ||
                live.generation != 0U)
            {
                return;
            }
            const std::uint64_t generation =
                m_test_hooks.start_generation
                ? m_test_hooks.start_generation(*live.session)
                : live.session->start_generation();
            if (m_core.admit_custody(
                    live.session_identity,
                    live.launch_request_identity,
                    generation,
                    live.lifetime_capability) ==
                Terminal_owner_admission_result::ADMITTED)
            {
                live.generation = generation;
            }
        });
    live.start_connection = QObject::connect(
        session,
        &VNM_Hosted_worker_session::start_finished,
        session,
        [this, &live](const VNM_Hosted_worker_start_result& result) {
            if (result.outcome == VNM_Hosted_worker_start_outcome::READY) {
                static_cast<void>(m_core.note_framework_ready(
                    live.session_identity,
                    live.generation));
            }
            else {
                settle_start_failure(live, result);
            }
        });
    live.close_connection = QObject::connect(
        session,
        &VNM_Hosted_worker_session::close_finished,
        session,
        [this, &live](const VNM_Hosted_worker_close_result& result) {
            settle_close(live, result);
        });
    live.crash_settlement_connection = QObject::connect(
        session,
        &VNM_Hosted_worker_session::terminal_crash,
        session,
        [this, &live](int exit_code) {
            settle_running_crash(live, exit_code);
        });
    live.attachment_connection = QObject::connect(
        session,
        &VNM_Hosted_worker_session::attachment_event,
        session,
        [this, &live](const VNM_Remote_surface_attachment_event& event) {
            if (event.state.start_generation != live.generation ||
                event.state.attachment_revision == 0U)
            {
                return;
            }
            static_cast<void>(m_core.apply_attachment(
                live.session_identity,
                live.generation,
                event.state.attachment_revision,
                event.state.live(),
                event.state.producer_process_id,
                event.state.framebuffer_path.toStdString(),
                event.state.store_generation));
        });
}

void Terminal_hosted_owner::settle_start_failure(
    Live_session& live,
    const VNM_Hosted_worker_start_result& result)
{
    if (live.generation == 0U) {
        erase_settled_session(live.session_identity);
        return;
    }
    reserve_first_close_cause(
        live,
        result.outcome == VNM_Hosted_worker_start_outcome::CANCELLED
            ? Terminal_close_cause::CANCELLED
            : Terminal_close_cause::START_FAILED);
    static_cast<void>(commit_first_close_cause(live));
    const std::string identity = live.session_identity;
    static_cast<void>(m_core.settle({
        identity,
        live.generation,
        std::nullopt,
        cleanup_disposition(result.cleanup_disposition),
        result.process_exit_observed
            ? std::optional<int>(result.process_exit_code)
            : std::nullopt,
    }, Terminal_owner_core::Time_point::clock::now()));
    erase_settled_session(identity);
}

void Terminal_hosted_owner::settle_close(
    Live_session& live,
    const VNM_Hosted_worker_close_result& result)
{
    const std::string identity = live.session_identity;
    static_cast<void>(m_core.settle({
        identity,
        live.generation,
        std::nullopt,
        cleanup_disposition(result.cleanup_disposition),
        std::nullopt,
    }, Terminal_owner_core::Time_point::clock::now()));
    erase_settled_session(identity);
}

void Terminal_hosted_owner::settle_running_crash(
    Live_session& live,
    int exit_code)
{
    const std::string identity = live.session_identity;
    reserve_first_close_cause(live, Terminal_close_cause::WORKER_CRASH);
    static_cast<void>(commit_first_close_cause(live));
    const Terminal_cleanup_disposition disposition =
        live.session->phase() ==
            VNM_Hosted_worker_session_phase::CLEANUP_RETAINED
        ? Terminal_cleanup_disposition::RETAINED
        : Terminal_cleanup_disposition::COMPLETE;
    static_cast<void>(m_core.settle({
        identity,
        live.generation,
        std::nullopt,
        disposition,
        exit_code,
    }, Terminal_owner_core::Time_point::clock::now()));
    erase_settled_session(identity);
}

void Terminal_hosted_owner::reserve_first_close_cause(
    Live_session& live,
    Terminal_close_cause cause)
{
    if (!live.close_cause) {
        live.close_cause = cause;
    }
}

Terminal_owner_update_result Terminal_hosted_owner::commit_first_close_cause(
    Live_session& live)
{
    return m_core.request_close(
        live.session_identity,
        live.generation,
        *live.close_cause);
}

Terminal_owner_update_result Terminal_hosted_owner::admit_reserved_close(
    Live_session& live)
{
    live.close_admission_in_progress = true;
    const bool admitted = m_test_hooks.close_async
        ? m_test_hooks.close_async(*live.session)
        : live.session->close_async();
    live.close_admission_in_progress = false;
    if (!admitted) {
        return Terminal_owner_update_result::REJECTED;
    }
    live.close_started = true;
    return commit_first_close_cause(live);
}

void Terminal_hosted_owner::schedule_hosted_close(
    const std::string& session_identity,
    std::uint64_t generation,
    Terminal_close_cause cause)
{
    const auto live = m_sessions.find(session_identity);
    if (live == m_sessions.end() || live->second->generation != generation ||
        live->second->close_started || live->second->close_dispatch_pending ||
        live->second->close_admission_in_progress)
    {
        return;
    }
    const auto custody = m_core.custody(session_identity);
    if (!custody || custody->generation != generation ||
        custody->first_close_cause)
    {
        return;
    }
    reserve_first_close_cause(*live->second, cause);
    live->second->close_dispatch_pending = true;
    QMetaObject::invokeMethod(
        live->second->session.get(),
        [this, session_identity, generation]() {
            const auto current = m_sessions.find(session_identity);
            if (current == m_sessions.end() ||
                current->second->generation != generation)
            {
                return;
            }
            current->second->close_dispatch_pending = false;
            static_cast<void>(admit_reserved_close(*current->second));
        },
        Qt::QueuedConnection);
}

VNM_control_result Terminal_hosted_owner::worker_context(
    const VNM_control_call& call)
{
    Live_session* const live = live_for_framework_caller(call);
    if (live == nullptr) {
        return VNM_control_result::failure(
            VNM_control_error_code::STALE_TARGET,
            QStringLiteral("worker generation is not current"));
    }
    return VNM_control_result::success(QVariantMap{
        {QStringLiteral("session_identity"),
            QString::fromUtf8(
                live->session_identity.data(),
                static_cast<qsizetype>(live->session_identity.size()))},
        {QStringLiteral("generation"),
            QString::number(live->generation)},
    });
}

VNM_control_result Terminal_hosted_owner::ingest_worker_fact(
    const VNM_control_call& call)
{
    Live_session* const live = live_for_framework_caller(call);
    if (live == nullptr) {
        return VNM_control_result::failure(
            VNM_control_error_code::STALE_TARGET,
            QStringLiteral("worker generation is not current"));
    }
    const std::optional<std::uint64_t> fact_key =
        unsigned_value(call.arguments.value(QStringLiteral("fact_key")));
    const std::optional<std::uint64_t> sequence =
        unsigned_value(call.arguments.value(QStringLiteral("sequence")));
    const std::optional<Terminal_child_fact_kind> kind = fact_kind(
        call.arguments.value(QStringLiteral("kind")).toInt());
    const std::optional<Terminal_child_fact_error> error = fact_error(
        call.arguments.value(QStringLiteral("error")).toInt());
    if (!fact_key || !sequence || !kind || !error) {
        return VNM_control_result::failure(
            VNM_control_error_code::BAD_ARGUMENTS,
            QStringLiteral("terminal child fact is malformed"));
    }
    Terminal_child_fact fact;
    fact.session_identity = live->session_identity;
    fact.generation = live->generation;
    fact.fact_key = *fact_key;
    fact.sequence = *sequence;
    fact.kind = *kind;
    fact.error = *error;
    fact.native_dispatch_occurred = call.arguments.value(
        QStringLiteral("native_dispatch_occurred")).toBool();
    if (call.arguments.contains(QStringLiteral("exit_code"))) {
        fact.exit_code = call.arguments.value(
            QStringLiteral("exit_code")).toInt();
    }
    const Terminal_child_fact_acknowledgement acknowledgement =
        m_core.ingest_child_fact(fact);
    if (acknowledgement == Terminal_child_fact_acknowledgement::ACCEPTED ||
        acknowledgement == Terminal_child_fact_acknowledgement::ALREADY_CURRENT)
    {
        std::optional<Terminal_close_cause> close_cause;
        switch (fact.kind)
        {
        case Terminal_child_fact_kind::STARTED:
            break;
        case Terminal_child_fact_kind::START_FAILED:
            close_cause = Terminal_close_cause::START_FAILED;
            break;
        case Terminal_child_fact_kind::START_INDETERMINATE:
            close_cause = Terminal_close_cause::START_INDETERMINATE;
            break;
        case Terminal_child_fact_kind::EXITED:
            close_cause = Terminal_close_cause::CHILD_EXIT;
            break;
        }
        if (close_cause) {
            schedule_hosted_close(
                live->session_identity,
                live->generation,
                *close_cause);
        }
    }
    return VNM_control_result::success(QVariantMap{
        {QStringLiteral("acknowledgement"),
            acknowledgement_name(acknowledgement)},
    });
}

Terminal_hosted_owner::Live_session*
Terminal_hosted_owner::live_for_framework_caller(
    const VNM_control_call& call)
{
    if (!call.caller.has_caller_session() ||
        call.caller.caller_worker_generation == 0U ||
        call.caller.caller_package_id !=
            QString::fromStdString(m_configuration.package_id) ||
        call.caller.caller_family_id !=
            QString::fromStdString(m_configuration.family_id))
    {
        return nullptr;
    }
    bool parsed = false;
    const std::uint64_t framework_session_id =
        call.caller.caller_session_id.toULongLong(&parsed);
    if (!parsed) {
        return nullptr;
    }
    const auto identity =
        m_framework_session_identities.find(framework_session_id);
    if (identity == m_framework_session_identities.end()) {
        return nullptr;
    }
    const auto live = m_sessions.find(identity->second);
    if (live == m_sessions.end() ||
        live->second->generation != call.caller.caller_worker_generation)
    {
        return nullptr;
    }
    return live->second.get();
}

void Terminal_hosted_owner::erase_settled_session(
    std::string_view session_identity)
{
    const auto live = m_sessions.find(session_identity);
    if (live == m_sessions.end()) {
        return;
    }
    QObject::disconnect(live->second->phase_connection);
    QObject::disconnect(live->second->start_connection);
    QObject::disconnect(live->second->close_connection);
    QObject::disconnect(live->second->crash_settlement_connection);
    QObject::disconnect(live->second->attachment_connection);
    VNM_Hosted_worker_session* const session = live->second->session.release();
    session->deleteLater();
    m_framework_session_identities.erase(live->second->framework_session_id);
    m_sessions.erase(live);
}

void Terminal_hosted_owner::send_input(
    Live_session& live,
    const Terminal_remote_input_message& message)
{
    switch (message.event_type)
    {
    case vnm::k_ui_input_mouse_move:
        live.session->send_mouse_move(
            message.x, message.y, message.buttons,
            message.modifiers, message.timestamp);
        break;
    case vnm::k_ui_input_mouse_press:
        live.session->send_mouse_press(
            message.x, message.y, message.button, message.buttons,
            message.modifiers, message.timestamp);
        break;
    case vnm::k_ui_input_mouse_release:
        live.session->send_mouse_release(
            message.x, message.y, message.button, message.buttons,
            message.modifiers, message.timestamp);
        break;
    case vnm::k_ui_input_mouse_double_click:
        live.session->send_mouse_double_click(
            message.x, message.y, message.button, message.buttons,
            message.modifiers, message.timestamp);
        break;
    case vnm::k_ui_input_mouse_scroll:
        live.session->send_mouse_scroll(
            message.x, message.y, message.scroll_dx, message.scroll_dy,
            message.modifiers, message.timestamp);
        break;
    case vnm::k_ui_input_key_press:
        live.session->send_key_press(message.key, message.modifiers);
        break;
    case vnm::k_ui_input_key_release:
        live.session->send_key_release(message.key, message.modifiers);
        break;
    case vnm::k_ui_input_text:
    {
        const auto end = std::find(
            message.text_utf8.begin(),
            message.text_utf8.end(),
            '\0');
        live.session->send_text_input(QString::fromUtf8(
            message.text_utf8.data(),
            static_cast<qsizetype>(
                std::distance(message.text_utf8.begin(), end))));
        break;
    }
    case vnm::k_ui_input_enter:
        live.session->send_enter(message.x, message.y, message.timestamp);
        break;
    case vnm::k_ui_input_leave:
        live.session->send_leave(message.x, message.y, message.timestamp);
        break;
    }
}

void Terminal_hosted_owner::send_state(
    Live_session& live,
    const Terminal_remote_state_message& message)
{
    switch (message.state_type)
    {
    case vnm::k_ui_state_resize:
        live.session->send_resize(message.width, message.height);
        break;
    case vnm::k_ui_state_focus:
        live.session->send_focus(message.value != 0U);
        break;
    case vnm::k_ui_state_visible:
        live.session->send_visible(message.value != 0U);
        break;
    case vnm::k_ui_state_scale_factor:
        live.session->send_scale_factor(message.scale_factor);
        break;
    case vnm::k_ui_state_dark_mode:
        live.session->send_dark_mode(message.value != 0U);
        break;
    }
}

Terminal_worker_message_submission_result Terminal_hosted_owner::send_message(
    Live_session& live,
    std::span<const std::uint8_t> message_utf8)
{
    QByteArray message(
        reinterpret_cast<const char*>(message_utf8.data()),
        static_cast<qsizetype>(message_utf8.size()));
    const VNM_Hosted_worker_message_result result =
        live.session->submit_message(
            live.generation,
            std::move(message),
            QStringLiteral("terminal-workspace-owner-message"));
    return {
        message_outcome(result.outcome),
        result.error.toStdString(),
    };
}

} // namespace vnm::terminal_workspace::detail
