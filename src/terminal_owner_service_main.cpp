#include "vnm_terminal_workspace/terminal_worker_composition.h"

#include "terminal_owner_process_identity.h"
#include "terminal_owner_service_policy.h"
#include "terminal_owner_wire.h"
#include "terminal_remote_input.h"

#include "vnm_remote_runtime.h"

#include "remote_ui_common/vnm_remote_ui_protocol.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vnm::terminal_workspace {

namespace detail {

struct Terminal_owner_service_access
{
    static std::unique_ptr<Terminal_owner_host> make_host(
        Terminal_owner_host_configuration configuration,
        const Terminal_worker_fixed_package_binding& binding)
    {
        return std::unique_ptr<Terminal_owner_host>(
            new Terminal_owner_host(std::move(configuration), binding));
    }

    static Terminal_owner_launch_result new_launch(
        Terminal_owner_host& host,
        std::span<const std::uint8_t> serialized_request,
        Launch_platform platform,
        Terminal_worker_surface_configuration surface_configuration,
        std::optional<Terminal_worker_output_capture_configuration>
            output_capture,
        std::string canonical_product_configuration,
        std::optional<std::vector<environment_policy::Environment_entry>>
            authorized_environment)
    {
        return host.new_launch_for_fixed_package(
            serialized_request,
            platform,
            std::move(surface_configuration),
            std::move(output_capture),
            std::move(canonical_product_configuration),
            std::move(authorized_environment));
    }
};

} // namespace detail

namespace {

using detail::Terminal_owner_wire_operation;
using detail::Terminal_owner_wire_status;

void clear_string(std::string& value)
{
    volatile char* bytes = value.data();
    for (std::size_t index = 0U; index < value.size(); ++index) {
        bytes[index] = '\0';
    }
    value.clear();
}

void clear_bytes(QByteArray& bytes)
{
    volatile char* data = bytes.data();
    for (qsizetype index = 0; index < bytes.size(); ++index) {
        data[index] = '\0';
    }
    bytes.clear();
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

class Sensitive_byte_array_guard
{
public:
    explicit Sensitive_byte_array_guard(QByteArray& bytes)
    :
        m_bytes(bytes)
    {}

    ~Sensitive_byte_array_guard()
    {
        clear_bytes(m_bytes);
    }

    Sensitive_byte_array_guard(const Sensitive_byte_array_guard&) = delete;
    Sensitive_byte_array_guard& operator=(
        const Sensitive_byte_array_guard&) = delete;

private:
    QByteArray& m_bytes;
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

struct Owner_arguments
{
    QString endpoint;
    QString lock_path;
    QString invitation_token;
    Terminal_owner_viewer_identity viewer_identity;
    Terminal_owner_host_configuration host;

    [[nodiscard]] bool valid() const
    {
        return
            !endpoint.isEmpty() &&
            !lock_path.isEmpty() &&
            !invitation_token.isEmpty() &&
            viewer_identity.native_process_id != 0U &&
            viewer_identity.native_process_creation_identity != 0U &&
            QFileInfo(QString::fromStdString(
                host.hosted_worker_host_executable_path)).isAbsolute() &&
            QFileInfo(QString::fromStdString(
                host.terminal_worker_library_path)).isAbsolute() &&
            !host.provider_namespace.empty();
    }
};

std::optional<quint64> unsigned_argument(QStringView value)
{
    bool ok = false;
    const quint64 result = value.toULongLong(&ok);
    return ok && result != 0U
        ? std::optional<quint64>(result)
        : std::nullopt;
}

Owner_arguments parse_arguments(const QStringList& arguments)
{
    Owner_arguments result;
    const auto value = [&arguments](QStringView prefix) -> QString {
        for (const QString& argument : arguments) {
            if (QStringView(argument).startsWith(prefix)) {
                return argument.sliced(prefix.size());
            }
        }
        return {};
    };
    result.endpoint = value(u"--endpoint=");
    result.lock_path = value(u"--lock-path=");
    result.invitation_token = value(u"--invitation-token=");
    const auto viewer_pid = unsigned_argument(value(u"--viewer-pid="));
    const auto viewer_creation = unsigned_argument(
        value(u"--viewer-creation="));
    if (viewer_pid && viewer_creation) {
        result.viewer_identity = {
            *viewer_pid,
            *viewer_pid,
            *viewer_creation,
        };
    }
    result.host.hosted_worker_host_executable_path =
        value(u"--host-executable=").toStdString();
    result.host.terminal_worker_library_path =
        value(u"--worker-library=").toStdString();
    result.host.provider_namespace =
        value(u"--provider-namespace=").toStdString();
    return result;
}

void write_response_prefix(
    QDataStream& stream,
    Terminal_owner_wire_status status)
{
    stream
        << static_cast<quint32>(detail::k_terminal_owner_wire_version)
        << static_cast<quint32>(status);
}

bool tokens_equal(QStringView expected, std::string_view supplied)
{
    const QByteArray expected_bytes = expected.toLatin1();
    unsigned char difference = static_cast<unsigned char>(
        expected_bytes.size() ^ static_cast<qsizetype>(supplied.size()));
    const qsizetype maximum = std::max(
        expected_bytes.size(),
        static_cast<qsizetype>(supplied.size()));
    for (qsizetype index = 0; index < maximum; ++index) {
        const unsigned char expected_byte = index < expected_bytes.size()
            ? static_cast<unsigned char>(expected_bytes[index])
            : 0U;
        const unsigned char supplied_byte = index <
                static_cast<qsizetype>(supplied.size())
            ? static_cast<unsigned char>(supplied[static_cast<std::size_t>(index)])
            : 0U;
        difference = static_cast<unsigned char>(
            difference | (expected_byte ^ supplied_byte));
    }
    return difference == 0U;
}

class Terminal_owner_service
{
public:
    Terminal_owner_service(
        QCoreApplication& application,
        Owner_arguments arguments,
        detail::Terminal_worker_fixed_package_binding binding)
    :
        m_application(application),
        m_arguments(std::move(arguments)),
        m_lock(m_arguments.lock_path),
        m_host(detail::Terminal_owner_service_access::make_host(
            m_arguments.host,
            binding))
    {
        m_invitation_timeout.setSingleShot(true);
        m_invitation_timeout.setInterval(10000);
        QObject::connect(
            &m_invitation_timeout,
            &QTimer::timeout,
            &m_application,
            [this]() {
                if (!m_authorized) {
                    m_application.exit(EXIT_FAILURE);
                }
            });
        m_settlement_poll.setInterval(10);
        QObject::connect(
            &m_settlement_poll,
            &QTimer::timeout,
            &m_application,
            [this]() {
                if (!m_host->custodies().empty()) {
                    return;
                }
                m_host->purge_unprotected_settlements_for_shutdown();
                m_settlement_poll.stop();
                m_application.exit(EXIT_SUCCESS);
            });
    }

    ~Terminal_owner_service()
    {
        clear_bytes(m_read_buffer);
    }

    bool listen()
    {
        m_lock.setStaleLockTime(30000);
        if (!m_lock.tryLock(0)) {
            return false;
        }
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        QLocalServer::removeServer(m_arguments.endpoint);
        QObject::connect(
            &m_server,
            &QLocalServer::newConnection,
            &m_application,
            [this]() { accept_pending_connections(); });
        if (!m_server.listen(m_arguments.endpoint)) {
            return false;
        }
        if (m_server.hasPendingConnections()) {
            accept_pending_connections();
        }
        m_invitation_timeout.start();
        return true;
    }

private:
    void accept_pending_connections()
    {
        while (QLocalSocket* socket = m_server.nextPendingConnection()) {
            if (m_socket) {
                socket->disconnectFromServer();
                socket->deleteLater();
                continue;
            }
            m_socket = socket;
            QObject::connect(
                socket,
                &QLocalSocket::readyRead,
                &m_application,
                [this, socket]() {
                    if (socket == m_socket) {
                        read_socket();
                    }
                });
            QObject::connect(
                socket,
                &QLocalSocket::disconnected,
                &m_application,
                [this, socket]() {
                    if (socket == m_socket) {
                        viewer_disconnected();
                    }
                    socket->deleteLater();
                });
        }
    }

    void read_socket()
    {
        QByteArray incoming = m_socket->readAll();
        Sensitive_byte_array_guard incoming_guard(incoming);
        m_read_buffer.append(incoming);
        for (;;) {
            QByteArray request;
            if (!detail::take_terminal_owner_frame(m_read_buffer, request)) {
                return;
            }
            if (request.isEmpty()) {
                send_status(Terminal_owner_wire_status::MALFORMED);
                m_socket->disconnectFromServer();
                return;
            }
            dispatch(request);
            if (!m_socket ||
                m_socket->state() == QLocalSocket::UnconnectedState)
            {
                return;
            }
        }
    }

    void dispatch(QByteArray& request)
    {
        Sensitive_byte_array_guard request_guard(request);
        auto reader = detail::make_terminal_owner_reader(request);
        quint32 version = 0U;
        quint32 operation_value = 0U;
        reader >> version >> operation_value;
        if (reader.status() != QDataStream::Ok) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        if (version != detail::k_terminal_owner_wire_version) {
            send_status(Terminal_owner_wire_status::INCOMPATIBLE);
            return;
        }
        const auto operation = static_cast<Terminal_owner_wire_operation>(
            operation_value);
        if (!m_authorized) {
            if (operation != Terminal_owner_wire_operation::HANDSHAKE) {
                send_status(Terminal_owner_wire_status::UNAUTHORIZED);
                return;
            }
            handshake(reader);
            return;
        }
        switch (operation)
        {
        case Terminal_owner_wire_operation::NEW_LAUNCH:
            new_launch(reader);
            break;
        case Terminal_owner_wire_operation::REQUEST_CLOSE:
            request_close(reader);
            break;
        case Terminal_owner_wire_operation::CUSTODY:
            custody(reader);
            break;
        case Terminal_owner_wire_operation::CUSTODIES:
            custodies(reader);
            break;
        case Terminal_owner_wire_operation::ATTACH_EXISTING:
            attach_existing(reader);
            break;
        case Terminal_owner_wire_operation::FORWARD_INPUT:
            forward_input(reader);
            break;
        case Terminal_owner_wire_operation::FORWARD_STATE:
            forward_state(reader);
            break;
        case Terminal_owner_wire_operation::CONTAINS_SETTLEMENT:
            settlement(reader, false);
            break;
        case Terminal_owner_wire_operation::ACKNOWLEDGE_SETTLEMENT:
            settlement(reader, true);
            break;
        case Terminal_owner_wire_operation::ATOMIC_SNAPSHOT:
            atomic_snapshot(reader);
            break;
        case Terminal_owner_wire_operation::SUBMIT_MESSAGE:
            submit_message(reader);
            break;
        case Terminal_owner_wire_operation::HANDSHAKE:
        default:
            send_status(Terminal_owner_wire_status::MALFORMED);
            break;
        }
    }

    void handshake(QDataStream& reader)
    {
        std::string token;
        quint64 claimed_pid = 0U;
        quint64 claimed_creation = 0U;
        if (!detail::read_string(reader, token)) {
            reject_handshake(Terminal_owner_wire_status::MALFORMED, 1U);
            return;
        }
        reader >> claimed_pid >> claimed_creation;
        const auto peer = detail::terminal_owner_local_socket_peer_identity(
            *m_socket);
        if (reader.status() != QDataStream::Ok) {
            reject_handshake(Terminal_owner_wire_status::MALFORMED, 2U);
            return;
        }
        if (!tokens_equal(m_arguments.invitation_token, token)) {
            reject_handshake(Terminal_owner_wire_status::UNAUTHORIZED, 3U);
            return;
        }
        if (!peer) {
            reject_handshake(Terminal_owner_wire_status::UNAUTHORIZED, 4U);
            return;
        }
        if (claimed_pid != m_arguments.viewer_identity.native_process_id ||
            claimed_creation !=
                m_arguments.viewer_identity.native_process_creation_identity)
        {
            reject_handshake(Terminal_owner_wire_status::UNAUTHORIZED, 5U);
            return;
        }
        if (peer->native_process_id != claimed_pid ||
            peer->native_process_creation_identity != claimed_creation)
        {
            reject_handshake(Terminal_owner_wire_status::UNAUTHORIZED, 6U);
            return;
        }
        if (m_host->bind_initial_viewer(*peer) !=
            Terminal_owner_viewer_bind_outcome::BOUND)
        {
            reject_handshake(Terminal_owner_wire_status::UNAUTHORIZED, 7U);
            return;
        }
        m_arguments.viewer_identity = *peer;
        m_epoch = m_host->viewer_authority_snapshot().epoch;
        if (m_epoch == 0U) {
            reject_handshake(Terminal_owner_wire_status::FAILED, 8U);
            return;
        }
        m_authorized = true;
        m_invitation_timeout.stop();
        m_server.close();
        QByteArray response;
        auto writer = detail::make_terminal_owner_writer(response);
        write_response_prefix(writer, Terminal_owner_wire_status::OK);
        writer << static_cast<quint64>(m_epoch);
        send_response(response);
    }

    void reject_handshake(
        Terminal_owner_wire_status status,
        quint32 category)
    {
        QByteArray response;
        auto writer = detail::make_terminal_owner_writer(response);
        write_response_prefix(writer, status);
        writer << category;
        send_response(response);
        QLocalSocket* const rejected = m_socket;
        QTimer::singleShot(
            100,
            rejected,
            [rejected]() { rejected->disconnectFromServer(); });
    }

    void new_launch(QDataStream& reader)
    {
        QByteArray serialized_request;
        Sensitive_byte_array_guard request_guard(serialized_request);
        quint32 platform_value = 0U;
        bool has_environment = false;
        Terminal_worker_surface_configuration surface_configuration;
        std::optional<Terminal_worker_output_capture_configuration>
            output_capture;
        std::string product_configuration;
        Sensitive_string_guard product_guard(product_configuration);
        reader >> serialized_request >> platform_value;
        if (!detail::read_launch_configuration(
                reader,
                surface_configuration,
                output_capture,
                product_configuration))
        {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        reader >> has_environment;
        if (reader.status() != QDataStream::Ok ||
            serialized_request.size() >
                static_cast<qsizetype>(detail::k_terminal_owner_maximum_frame_bytes) ||
            platform_value >
                static_cast<quint32>(Launch_platform::POSIX))
        {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        std::optional<std::vector<environment_policy::Environment_entry>>
            environment;
        Sensitive_environment_guard environment_guard(environment);
        if (has_environment) {
            quint32 environment_count = 0U;
            reader >> environment_count;
            if (environment_count > 4096U) {
                send_status(Terminal_owner_wire_status::MALFORMED);
                return;
            }
            environment.emplace();
            environment->reserve(environment_count);
            for (quint32 index = 0U; index < environment_count; ++index) {
                environment_policy::Environment_entry entry;
                if (!detail::read_string(reader, entry.name) ||
                    !detail::read_string(reader, entry.value))
                {
                    send_status(Terminal_owner_wire_status::MALFORMED);
                    return;
                }
                environment->push_back(std::move(entry));
            }
        }
        if (reader.status() != QDataStream::Ok) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        const Terminal_owner_launch_result result =
            detail::Terminal_owner_service_access::new_launch(
            *m_host,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(serialized_request.data()),
                static_cast<std::size_t>(serialized_request.size())),
            static_cast<Launch_platform>(platform_value),
            std::move(surface_configuration),
            std::move(output_capture),
            std::move(product_configuration),
            std::move(environment));
        QByteArray response;
        auto writer = detail::make_terminal_owner_writer(response);
        write_response_prefix(writer, Terminal_owner_wire_status::OK);
        detail::write_launch_result(writer, result);
        send_response(response);
    }

    void request_close(QDataStream& reader)
    {
        std::string session_identity;
        quint64 generation = 0U;
        if (!detail::read_string(reader, session_identity)) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        reader >> generation;
        if (reader.status() != QDataStream::Ok) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        send_enum(m_host->request_close(session_identity, generation));
    }

    void custody(QDataStream& reader)
    {
        std::string session_identity;
        if (!detail::read_string(reader, session_identity)) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        const auto value = m_host->custody(session_identity);
        QByteArray response;
        auto writer = detail::make_terminal_owner_writer(response);
        write_response_prefix(writer, Terminal_owner_wire_status::OK);
        writer << value.has_value();
        if (value) {
            detail::write_custody_snapshot(writer, *value);
        }
        send_response(response);
    }

    void custodies(QDataStream& reader)
    {
        if (reader.status() != QDataStream::Ok || !reader.atEnd()) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        const auto values = m_host->custodies();
        QByteArray response;
        auto writer = detail::make_terminal_owner_writer(response);
        write_response_prefix(writer, Terminal_owner_wire_status::OK);
        writer << static_cast<quint32>(values.size());
        for (const auto& value : values) {
            detail::write_custody_snapshot(writer, value);
        }
        send_response(response);
    }

    void attach_existing(QDataStream& reader)
    {
        std::string session_identity;
        quint64 generation = 0U;
        quint64 revision = 0U;
        if (!detail::read_string(reader, session_identity)) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        reader >> generation >> revision;
        if (reader.status() != QDataStream::Ok) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        send_enum(m_host->attach_existing(
            m_arguments.viewer_identity.transport_process_id,
            m_epoch,
            session_identity,
            generation,
            revision));
    }

    void atomic_snapshot(QDataStream& reader)
    {
        if (reader.status() != QDataStream::Ok || !reader.atEnd()) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        const Terminal_owner_atomic_snapshot value = m_host->atomic_snapshot(
            std::chrono::steady_clock::now());
        QByteArray response;
        auto writer = detail::make_terminal_owner_writer(response);
        write_response_prefix(writer, Terminal_owner_wire_status::OK);
        writer
            << static_cast<quint64>(value.revision)
            << static_cast<quint32>(value.custodies.size());
        for (const auto& custody : value.custodies) {
            detail::write_custody_snapshot(writer, custody);
        }
        writer << static_cast<quint32>(value.unprotected_receipts.size());
        for (const auto& receipt : value.unprotected_receipts) {
            detail::write_string(writer, receipt.session_identity);
            writer << static_cast<quint64>(receipt.generation);
        }
        send_response(response);
    }

    void forward_input(QDataStream& reader)
    {
        std::string session_identity;
        quint64 generation = 0U;
        quint64 revision = 0U;
        quint32 event_type = 0U;
        quint32 modifiers = 0U;
        qint32 x = 0;
        qint32 y = 0;
        quint32 button = 0U;
        quint32 buttons = 0U;
        quint32 key = 0U;
        float scroll_dx = 0.0F;
        float scroll_dy = 0.0F;
        std::string text;
        quint64 timestamp = 0U;
        quint32 native_scan_code = 0U;
        quint32 native_virtual_key = 0U;
        quint32 native_modifiers = 0U;
        quint16 count = 0U;
        bool auto_repeat = false;
        if (!detail::read_string(reader, session_identity)) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        reader
            >> generation >> revision >> event_type >> modifiers >> x >> y
            >> button >> buttons >> key >> scroll_dx >> scroll_dy;
        if (reader.status() != QDataStream::Ok ||
            !detail::read_string(reader, text))
        {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        reader
            >> timestamp >> native_scan_code >> native_virtual_key
            >> native_modifiers >> count >> auto_repeat;
        if (reader.status() != QDataStream::Ok ||
            text.size() > vnm::k_remote_ui_input_max_text_bytes)
        {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        Terminal_remote_input_message message;
        message.event_type = event_type;
        message.modifiers = modifiers;
        message.x = x;
        message.y = y;
        message.button = button;
        message.buttons = buttons;
        message.key = key;
        message.scroll_dx = scroll_dx;
        message.scroll_dy = scroll_dy;
        message.text_utf8 = std::move(text);
        message.timestamp = timestamp;
        message.native_scan_code = native_scan_code;
        message.native_virtual_key = native_virtual_key;
        message.native_modifiers = native_modifiers;
        message.auto_repeat = auto_repeat;
        message.count = count;
        if (detail::validate_terminal_remote_input(message) !=
            vnm::Remote_ui_input_decode_result::DECODED)
        {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        send_enum(m_host->forward_input(
            m_arguments.viewer_identity.transport_process_id,
            m_epoch,
            session_identity,
            generation,
            revision,
            message));
    }

    void forward_state(QDataStream& reader)
    {
        std::string session_identity;
        quint64 generation = 0U;
        quint64 revision = 0U;
        quint32 state_type = 0U;
        qint32 width = 0;
        qint32 height = 0;
        float scale_factor = 1.0F;
        quint8 value = 0U;
        if (!detail::read_string(reader, session_identity)) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        reader
            >> generation >> revision >> state_type >> width >> height
            >> scale_factor >> value;
        if (reader.status() != QDataStream::Ok) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        send_enum(m_host->forward_state(
            m_arguments.viewer_identity.transport_process_id,
            m_epoch,
            session_identity,
            generation,
            revision,
            {state_type, width, height, scale_factor, value}));
    }

    void submit_message(QDataStream& reader)
    {
        std::string session_identity;
        quint64 generation = 0U;
        quint64 revision = 0U;
        QByteArray message;
        Sensitive_byte_array_guard message_guard(message);
        if (!detail::read_string(reader, session_identity)) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        reader >> generation >> revision >> message;
        if (reader.status() != QDataStream::Ok ||
            message.size() > static_cast<qsizetype>(
                detail::k_terminal_owner_maximum_frame_bytes))
        {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        const Terminal_owner_message_submission_result result =
            m_host->submit_message(
                m_arguments.viewer_identity.transport_process_id,
                m_epoch,
                session_identity,
                generation,
                revision,
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(message.data()),
                    static_cast<std::size_t>(message.size())));
        QByteArray response;
        auto writer = detail::make_terminal_owner_writer(response);
        write_response_prefix(writer, Terminal_owner_wire_status::OK);
        writer
            << static_cast<quint32>(result.routing)
            << result.submission.has_value();
        if (result.submission) {
            writer << static_cast<quint32>(result.submission->outcome);
            detail::write_string(writer, result.submission->error);
        }
        send_response(response);
    }

    void settlement(QDataStream& reader, bool acknowledge)
    {
        std::string session_identity;
        quint64 generation = 0U;
        if (!detail::read_string(reader, session_identity)) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        reader >> generation;
        if (reader.status() != QDataStream::Ok) {
            send_status(Terminal_owner_wire_status::MALFORMED);
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool result = acknowledge
            ? m_host->acknowledge_unprotected_settlement(
                session_identity,
                generation,
                now)
            : m_host->contains_unprotected_settlement(
                session_identity,
                generation,
                now);
        QByteArray response;
        auto writer = detail::make_terminal_owner_writer(response);
        write_response_prefix(writer, Terminal_owner_wire_status::OK);
        writer << result;
        send_response(response);
    }

    template<typename Enum>
    void send_enum(Enum value)
    {
        QByteArray response;
        auto writer = detail::make_terminal_owner_writer(response);
        write_response_prefix(writer, Terminal_owner_wire_status::OK);
        writer << static_cast<quint32>(value);
        send_response(response);
    }

    void send_status(Terminal_owner_wire_status status)
    {
        QByteArray response;
        auto writer = detail::make_terminal_owner_writer(response);
        write_response_prefix(writer, status);
        send_response(response);
    }

    void send_response(const QByteArray& response)
    {
        if (!m_socket ||
            m_socket->state() != QLocalSocket::ConnectedState)
        {
            return;
        }
        m_socket->write(detail::frame_terminal_owner_message(response));
        m_socket->flush();
    }

    void viewer_disconnected()
    {
        m_socket = nullptr;
        clear_bytes(m_read_buffer);
        if (!m_authorized) {
            m_application.exit(EXIT_FAILURE);
            return;
        }
        static_cast<void>(m_host->note_viewer_transport_departure(
            m_arguments.viewer_identity));
        std::vector<Terminal_owner_update_outcome> close_results;
        for (const auto& custody : m_host->custodies()) {
            close_results.push_back(m_host->request_close(
                custody.session_identity,
                custody.generation));
        }
        const detail::Terminal_owner_shutdown_disposition disposition =
            detail::owner_shutdown_disposition(
                close_results,
                m_host->custodies().empty());
        if (disposition ==
            detail::Terminal_owner_shutdown_disposition::COMPLETE)
        {
            m_host->purge_unprotected_settlements_for_shutdown();
            m_application.exit(EXIT_SUCCESS);
            return;
        }
        if (disposition ==
            detail::Terminal_owner_shutdown_disposition::FAILED)
        {
            m_application.exit(EXIT_FAILURE);
            return;
        }
        m_settlement_poll.start();
    }

    QCoreApplication& m_application;
    Owner_arguments m_arguments;
    QLockFile m_lock;
    std::unique_ptr<Terminal_owner_host> m_host;
    QLocalServer m_server;
    QLocalSocket* m_socket = nullptr;
    QByteArray m_read_buffer;
    QTimer m_invitation_timeout;
    QTimer m_settlement_poll;
    Terminal_owner_viewer_epoch m_epoch = 0U;
    bool m_authorized = false;
};

bool complete_remote_runtime_shutdown()
{
    return VNM_RemoteRuntime::shutdown_with_completion() ==
        VNM_RemoteRuntime::Shutdown_completion::COMPLETE;
}

} // namespace

int detail::run_terminal_owner_service_program(
    int argc,
    char** argv,
    detail::Terminal_worker_fixed_package_binding binding)
{
    QCoreApplication application(argc, argv);
    const auto arguments = parse_arguments(
        application.arguments());
    if (!arguments.valid()) {
        return EXIT_FAILURE;
    }
    if (!vnm::VNM_RemoteRuntime::initialize(
            argc,
            argv,
            "vnm-terminal-workspace-owner"))
    {
        return EXIT_FAILURE;
    }
    int result = EXIT_FAILURE;
    {
        Terminal_owner_service service(
            application,
            arguments,
            std::move(binding));
        if (service.listen()) {
            result = application.exec();
        }
    }
    if (!complete_remote_runtime_shutdown()) {
        result = EXIT_FAILURE;
    }
    return result;
}

} // namespace vnm::terminal_workspace
