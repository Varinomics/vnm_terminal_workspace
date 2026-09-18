#include "vnm_terminal_workspace/terminal_owner_client.h"

#include "terminal_owner_process_identity.h"
#include "terminal_owner_wire.h"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QRandomGenerator>
#include <QString>
#include <QStringList>

#include <array>
#include <limits>
#include <utility>

namespace vnm::terminal_workspace {
namespace {

using detail::Terminal_owner_wire_operation;
using detail::Terminal_owner_wire_status;

QString scope_identity(const Terminal_owner_service_scope& scope)
{
    QByteArray source(scope.product_identity.data(),
                      static_cast<qsizetype>(scope.product_identity.size()));
    source.append('\0');
    source.append(
        scope.application_instance_identity.data(),
        static_cast<qsizetype>(scope.application_instance_identity.size()));
    return QString::fromLatin1(
        QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex());
}

QString endpoint_for_scope(const Terminal_owner_service_scope& scope)
{
    return QStringLiteral("vnm-terminal-workspace-owner-v1-") +
        scope_identity(scope).left(32);
}

QString lock_path_for_scope(const Terminal_owner_service_scope& scope)
{
    return QDir::temp().absoluteFilePath(
        QStringLiteral("vnm-terminal-workspace-owner-v1-") +
        scope_identity(scope).left(32) + QStringLiteral(".lock"));
}

QString invitation_token()
{
    std::array<quint32, 8U> words{};
    for (quint32& word : words) {
        word = QRandomGenerator::system()->generate();
    }
    QByteArray bytes(
        reinterpret_cast<const char*>(words.data()),
        static_cast<qsizetype>(sizeof(words)));
    return QString::fromLatin1(bytes.toHex());
}

int remaining_timeout(QDeadlineTimer& deadline)
{
    const qint64 remaining = deadline.remainingTime();
    return remaining < 0
        ? 0
        : static_cast<int>(std::min<qint64>(
            remaining,
            std::numeric_limits<int>::max()));
}

void set_connect_result(
    Terminal_owner_client_connect_outcome* output,
    Terminal_owner_client_connect_outcome value,
    std::string* diagnostic,
    std::string_view message)
{
    if (output) {
        *output = value;
    }
    if (diagnostic) {
        diagnostic->assign(message);
    }
}

void clear_string(std::string& value)
{
    volatile char* bytes = value.data();
    for (std::size_t index = 0U; index < value.size(); ++index) {
        bytes[index] = '\0';
    }
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

void clear_bytes(QByteArray& bytes)
{
    volatile char* data = bytes.data();
    for (qsizetype index = 0; index < bytes.size(); ++index) {
        data[index] = '\0';
    }
    bytes.clear();
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

bool configuration_valid(
    const Terminal_owner_client_configuration& configuration)
{
    return
        !configuration.scope.product_identity.empty() &&
        !configuration.scope.application_instance_identity.empty() &&
        configuration.connect_timeout.count() > 0 &&
        QFileInfo(QString::fromStdString(
            configuration.owner_executable_path)).isAbsolute() &&
        QFileInfo(QString::fromStdString(
            configuration.owner.hosted_worker_host_executable_path)).
                isAbsolute() &&
        QFileInfo(QString::fromStdString(
            configuration.owner.terminal_worker_library_path)).isAbsolute() &&
        !configuration.owner.provider_namespace.empty();
}

void write_request_prefix(
    QDataStream& stream,
    Terminal_owner_wire_operation operation)
{
    stream
        << static_cast<quint32>(detail::k_terminal_owner_wire_version)
        << static_cast<quint32>(operation);
}

bool response_ok(QDataStream& stream)
{
    quint32 version = 0U;
    quint32 status = 0U;
    stream >> version >> status;
    return
        stream.status() == QDataStream::Ok &&
        version == detail::k_terminal_owner_wire_version &&
        status == static_cast<quint32>(Terminal_owner_wire_status::OK);
}

} // namespace

struct Terminal_owner_client::Impl
{
    std::optional<QByteArray> transact(QByteArray payload)
    {
        Sensitive_byte_array_guard payload_guard(payload);
        if (socket.state() != QLocalSocket::ConnectedState) {
            return std::nullopt;
        }
        QDeadlineTimer deadline(timeout);
        QByteArray framed = detail::frame_terminal_owner_message(payload);
        Sensitive_byte_array_guard framed_guard(framed);
        if (socket.write(framed) != framed.size()) {
            return std::nullopt;
        }
        while (socket.bytesToWrite() != 0) {
            if (!socket.waitForBytesWritten(remaining_timeout(deadline))) {
                return std::nullopt;
            }
        }
        for (;;) {
            QByteArray response;
            if (detail::take_terminal_owner_frame(read_buffer, response)) {
                return response.isEmpty()
                    ? std::nullopt
                    : std::optional<QByteArray>(std::move(response));
            }
            if (!socket.waitForReadyRead(remaining_timeout(deadline))) {
                return std::nullopt;
            }
            read_buffer.append(socket.readAll());
        }
    }

    QLocalSocket socket;
    QByteArray read_buffer;
    int timeout = 10000;
    Terminal_owner_viewer_epoch epoch = 0U;
};

Terminal_owner_client::Terminal_owner_client(std::unique_ptr<Impl> impl)
:
    m_impl(std::move(impl))
{}

Terminal_owner_client::~Terminal_owner_client()
{
    if (m_impl->socket.state() == QLocalSocket::ConnectedState) {
        m_impl->socket.disconnectFromServer();
    }
}

std::unique_ptr<Terminal_owner_client> Terminal_owner_client::connect(
    const Terminal_owner_client_configuration& configuration,
    Terminal_owner_client_connect_outcome* outcome,
    std::string* diagnostic)
{
    if (!configuration_valid(configuration)) {
        set_connect_result(
            outcome,
            Terminal_owner_client_connect_outcome::INVALID_CONFIGURATION,
            diagnostic,
            "The terminal owner client configuration is invalid");
        return {};
    }
    const auto identity = detail::current_terminal_owner_process_identity();
    if (!identity) {
        set_connect_result(
            outcome,
            Terminal_owner_client_connect_outcome::INVALID_CONFIGURATION,
            diagnostic,
            "The viewer process identity is unavailable");
        return {};
    }

    const QString endpoint = endpoint_for_scope(configuration.scope);
    const QString token = invitation_token();
    const QString owner_executable = QString::fromStdString(
        configuration.owner_executable_path);
    const QStringList arguments{
        QStringLiteral("--vnm-terminal-owner"),
        QStringLiteral("--endpoint=") + endpoint,
        QStringLiteral("--lock-path=") + lock_path_for_scope(configuration.scope),
        QStringLiteral("--invitation-token=") + token,
        QStringLiteral("--viewer-pid=") +
            QString::number(identity->native_process_id),
        QStringLiteral("--viewer-creation=") +
            QString::number(identity->native_process_creation_identity),
        QStringLiteral("--host-executable=") + QString::fromStdString(
            configuration.owner.hosted_worker_host_executable_path),
        QStringLiteral("--worker-library=") + QString::fromStdString(
            configuration.owner.terminal_worker_library_path),
        QStringLiteral("--provider-namespace=") + QString::fromStdString(
            configuration.owner.provider_namespace),
    };

    QDeadlineTimer deadline(configuration.connect_timeout);
    auto impl = std::make_unique<Impl>();
    impl->timeout = static_cast<int>(std::min<std::int64_t>(
        configuration.connect_timeout.count(),
        std::numeric_limits<int>::max()));
    impl->socket.connectToServer(endpoint);
    if (!impl->socket.waitForConnected(std::min(100, remaining_timeout(deadline)))) {
        impl->socket.abort();
        qint64 owner_process_id = 0;
        if (!QProcess::startDetached(
                owner_executable,
                arguments,
                QFileInfo(owner_executable).absolutePath(),
                &owner_process_id))
        {
            set_connect_result(
                outcome,
                Terminal_owner_client_connect_outcome::OWNER_LAUNCH_FAILED,
                diagnostic,
                "The terminal owner process could not be launched");
            return {};
        }
        while (!deadline.hasExpired()) {
            impl->socket.connectToServer(endpoint);
            if (impl->socket.waitForConnected(
                    std::min(100, remaining_timeout(deadline))))
            {
                break;
            }
            impl->socket.abort();
        }
    }
    if (impl->socket.state() != QLocalSocket::ConnectedState) {
        set_connect_result(
            outcome,
            Terminal_owner_client_connect_outcome::OWNER_UNAVAILABLE,
            diagnostic,
            "The terminal owner endpoint did not become available");
        return {};
    }

    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    write_request_prefix(writer, Terminal_owner_wire_operation::HANDSHAKE);
    detail::write_string(writer, token.toStdString());
    writer
        << static_cast<quint64>(identity->native_process_id)
        << static_cast<quint64>(identity->native_process_creation_identity);
    const auto response = impl->transact(std::move(request));
    if (!response) {
        set_connect_result(
            outcome,
            Terminal_owner_client_connect_outcome::OWNER_UNAVAILABLE,
            diagnostic,
            "The terminal owner did not answer the invitation");
        return {};
    }
    QByteArray response_bytes = *response;
    auto reader = detail::make_terminal_owner_reader(response_bytes);
    quint32 version = 0U;
    quint32 status = 0U;
    quint64 epoch = 0U;
    reader >> version >> status;
    if (version != detail::k_terminal_owner_wire_version) {
        set_connect_result(
            outcome,
            Terminal_owner_client_connect_outcome::INCOMPATIBLE_OWNER,
            diagnostic,
            "The terminal owner protocol is incompatible");
        return {};
    }
    if (status != static_cast<quint32>(Terminal_owner_wire_status::OK)) {
        quint32 rejection_category = 0U;
        reader >> rejection_category;
        set_connect_result(
            outcome,
            Terminal_owner_client_connect_outcome::AUTHORIZATION_REJECTED,
            diagnostic,
            std::string("The terminal owner rejected invitation category ") +
                std::to_string(rejection_category));
        return {};
    }
    reader >> epoch;
    if (reader.status() != QDataStream::Ok || epoch == 0U) {
        set_connect_result(
            outcome,
            Terminal_owner_client_connect_outcome::INCOMPATIBLE_OWNER,
            diagnostic,
            "The terminal owner returned an invalid authority epoch");
        return {};
    }
    impl->epoch = epoch;
    set_connect_result(
        outcome,
        Terminal_owner_client_connect_outcome::CONNECTED,
        diagnostic,
        {});
    return std::unique_ptr<Terminal_owner_client>(
        new Terminal_owner_client(std::move(impl)));
}

Terminal_owner_viewer_epoch Terminal_owner_client::viewer_epoch() const noexcept
{
    return m_impl->epoch;
}

Terminal_owner_launch_result Terminal_owner_client::new_launch(
    std::span<const std::uint8_t> serialized_request,
    Launch_platform platform,
    std::optional<std::vector<environment_policy::Environment_entry>>
        authorized_environment)
{
    return new_launch_for_fixed_package(
        serialized_request,
        platform,
        {},
        std::nullopt,
        {},
        std::move(authorized_environment));
}

Terminal_owner_launch_result
Terminal_owner_client::new_launch_for_fixed_package(
    std::span<const std::uint8_t> serialized_request,
    Launch_platform platform,
    Terminal_worker_surface_configuration surface_configuration,
    std::optional<Terminal_worker_output_capture_configuration> output_capture,
    std::string canonical_product_configuration,
    std::optional<std::vector<environment_policy::Environment_entry>>
        authorized_environment)
{
    Sensitive_environment_guard environment_guard(authorized_environment);
    Sensitive_string_guard configuration_guard(
        canonical_product_configuration);
    if (!valid_launch_platform(platform)) {
        return {Terminal_owner_launch_outcome::INVALID_REQUEST, {}, 0U};
    }
    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    write_request_prefix(writer, Terminal_owner_wire_operation::NEW_LAUNCH);
    writer << QByteArray(
        reinterpret_cast<const char*>(serialized_request.data()),
        static_cast<qsizetype>(serialized_request.size()));
    writer << static_cast<quint32>(platform);
    detail::write_launch_configuration(
        writer,
        surface_configuration,
        output_capture,
        canonical_product_configuration);
    writer << authorized_environment.has_value();
    if (authorized_environment) {
        writer << static_cast<quint32>(authorized_environment->size());
        for (const auto& entry : *authorized_environment) {
            detail::write_string(writer, entry.name);
            detail::write_string(writer, entry.value);
        }
    }
    const auto response = m_impl->transact(std::move(request));
    if (!response) {
        return {Terminal_owner_launch_outcome::HOST_START_REJECTED, {}, 0U};
    }
    QByteArray response_bytes = *response;
    auto reader = detail::make_terminal_owner_reader(response_bytes);
    Terminal_owner_launch_result result;
    if (!response_ok(reader) ||
        !detail::read_launch_result(reader, result))
    {
        return {Terminal_owner_launch_outcome::HOST_START_REJECTED, {}, 0U};
    }
    return result;
}

Terminal_owner_update_outcome Terminal_owner_client::request_close(
    const std::string& session_identity,
    std::uint64_t generation)
{
    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    write_request_prefix(writer, Terminal_owner_wire_operation::REQUEST_CLOSE);
    detail::write_string(writer, session_identity);
    writer << static_cast<quint64>(generation);
    const auto response = m_impl->transact(std::move(request));
    if (!response) {
        return Terminal_owner_update_outcome::REJECTED;
    }
    QByteArray response_bytes = *response;
    auto reader = detail::make_terminal_owner_reader(response_bytes);
    quint32 result = 0U;
    if (!response_ok(reader)) {
        return Terminal_owner_update_outcome::REJECTED;
    }
    reader >> result;
    return reader.status() == QDataStream::Ok
        ? static_cast<Terminal_owner_update_outcome>(result)
        : Terminal_owner_update_outcome::REJECTED;
}

std::optional<Terminal_owner_custody_snapshot> Terminal_owner_client::custody(
    const std::string& session_identity)
{
    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    write_request_prefix(writer, Terminal_owner_wire_operation::CUSTODY);
    detail::write_string(writer, session_identity);
    const auto response = m_impl->transact(std::move(request));
    if (!response) {
        return std::nullopt;
    }
    QByteArray response_bytes = *response;
    auto reader = detail::make_terminal_owner_reader(response_bytes);
    bool present = false;
    if (!response_ok(reader)) {
        return std::nullopt;
    }
    reader >> present;
    if (!present) {
        return std::nullopt;
    }
    Terminal_owner_custody_snapshot value;
    return detail::read_custody_snapshot(reader, value)
        ? std::optional<Terminal_owner_custody_snapshot>(std::move(value))
        : std::nullopt;
}

std::vector<Terminal_owner_custody_snapshot> Terminal_owner_client::custodies()
{
    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    write_request_prefix(writer, Terminal_owner_wire_operation::CUSTODIES);
    const auto response = m_impl->transact(std::move(request));
    if (!response) {
        return {};
    }
    QByteArray response_bytes = *response;
    auto reader = detail::make_terminal_owner_reader(response_bytes);
    quint32 count = 0U;
    if (!response_ok(reader)) {
        return {};
    }
    reader >> count;
    if (count > 4096U) {
        return {};
    }
    std::vector<Terminal_owner_custody_snapshot> result;
    result.reserve(count);
    for (quint32 index = 0U; index < count; ++index) {
        Terminal_owner_custody_snapshot value;
        if (!detail::read_custody_snapshot(reader, value)) {
            return {};
        }
        result.push_back(std::move(value));
    }
    return result;
}

Terminal_owner_atomic_snapshot Terminal_owner_client::atomic_snapshot()
{
    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    write_request_prefix(writer, Terminal_owner_wire_operation::ATOMIC_SNAPSHOT);
    const auto response = m_impl->transact(std::move(request));
    if (!response) {
        return {};
    }
    QByteArray response_bytes = *response;
    auto reader = detail::make_terminal_owner_reader(response_bytes);
    quint64 revision = 0U;
    quint32 custody_count = 0U;
    if (!response_ok(reader)) {
        return {};
    }
    reader >> revision >> custody_count;
    if (reader.status() != QDataStream::Ok || custody_count > 4096U) {
        return {};
    }
    Terminal_owner_atomic_snapshot result;
    result.revision = revision;
    result.custodies.reserve(custody_count);
    for (quint32 index = 0U; index < custody_count; ++index) {
        Terminal_owner_custody_snapshot value;
        if (!detail::read_custody_snapshot(reader, value)) {
            return {};
        }
        result.custodies.push_back(std::move(value));
    }
    quint32 receipt_count = 0U;
    reader >> receipt_count;
    if (reader.status() != QDataStream::Ok || receipt_count > 32U) {
        return {};
    }
    result.unprotected_receipts.reserve(receipt_count);
    for (quint32 index = 0U; index < receipt_count; ++index) {
        Terminal_owner_settlement_receipt_key key;
        quint64 generation = 0U;
        if (!detail::read_string(reader, key.session_identity)) {
            return {};
        }
        reader >> generation;
        key.generation = generation;
        result.unprotected_receipts.push_back(std::move(key));
    }
    return reader.status() == QDataStream::Ok
        ? result
        : Terminal_owner_atomic_snapshot{};
}

Terminal_owner_proxy_outcome Terminal_owner_client::attach_existing(
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision)
{
    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    write_request_prefix(writer, Terminal_owner_wire_operation::ATTACH_EXISTING);
    detail::write_string(writer, session_identity);
    writer
        << static_cast<quint64>(generation)
        << static_cast<quint64>(attachment_revision);
    const auto response = m_impl->transact(std::move(request));
    if (!response) {
        return Terminal_owner_proxy_outcome::AUTHORITY_REJECTED;
    }
    QByteArray response_bytes = *response;
    auto reader = detail::make_terminal_owner_reader(response_bytes);
    quint32 result = 0U;
    if (!response_ok(reader)) {
        return Terminal_owner_proxy_outcome::AUTHORITY_REJECTED;
    }
    reader >> result;
    return reader.status() == QDataStream::Ok
        ? static_cast<Terminal_owner_proxy_outcome>(result)
        : Terminal_owner_proxy_outcome::AUTHORITY_REJECTED;
}

Terminal_owner_proxy_outcome Terminal_owner_client::forward_input(
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision,
    const Terminal_remote_input_message& message)
{
    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    write_request_prefix(writer, Terminal_owner_wire_operation::FORWARD_INPUT);
    detail::write_string(writer, session_identity);
    writer
        << static_cast<quint64>(generation)
        << static_cast<quint64>(attachment_revision)
        << static_cast<quint32>(message.event_type)
        << static_cast<quint32>(message.modifiers)
        << static_cast<qint32>(message.x)
        << static_cast<qint32>(message.y)
        << static_cast<quint32>(message.button)
        << static_cast<quint32>(message.buttons)
        << static_cast<quint32>(message.key)
        << message.scroll_dx
        << message.scroll_dy;
    detail::write_string(writer, message.text_utf8);
    writer
        << static_cast<quint64>(message.timestamp)
        << static_cast<quint32>(message.native_scan_code)
        << static_cast<quint32>(message.native_virtual_key)
        << static_cast<quint32>(message.native_modifiers)
        << static_cast<quint16>(message.count)
        << message.auto_repeat;
    const auto response = m_impl->transact(std::move(request));
    if (!response) {
        return Terminal_owner_proxy_outcome::AUTHORITY_REJECTED;
    }
    QByteArray response_bytes = *response;
    auto reader = detail::make_terminal_owner_reader(response_bytes);
    quint32 result = 0U;
    if (!response_ok(reader)) {
        return Terminal_owner_proxy_outcome::AUTHORITY_REJECTED;
    }
    reader >> result;
    return reader.status() == QDataStream::Ok
        ? static_cast<Terminal_owner_proxy_outcome>(result)
        : Terminal_owner_proxy_outcome::AUTHORITY_REJECTED;
}

Terminal_owner_proxy_outcome Terminal_owner_client::forward_state(
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision,
    const Terminal_remote_state_message& message)
{
    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    write_request_prefix(writer, Terminal_owner_wire_operation::FORWARD_STATE);
    detail::write_string(writer, session_identity);
    writer
        << static_cast<quint64>(generation)
        << static_cast<quint64>(attachment_revision)
        << static_cast<quint32>(message.state_type)
        << static_cast<qint32>(message.width)
        << static_cast<qint32>(message.height)
        << message.scale_factor
        << static_cast<quint8>(message.value);
    const auto response = m_impl->transact(std::move(request));
    if (!response) {
        return Terminal_owner_proxy_outcome::AUTHORITY_REJECTED;
    }
    QByteArray response_bytes = *response;
    auto reader = detail::make_terminal_owner_reader(response_bytes);
    quint32 result = 0U;
    if (!response_ok(reader)) {
        return Terminal_owner_proxy_outcome::AUTHORITY_REJECTED;
    }
    reader >> result;
    return reader.status() == QDataStream::Ok
        ? static_cast<Terminal_owner_proxy_outcome>(result)
        : Terminal_owner_proxy_outcome::AUTHORITY_REJECTED;
}

Terminal_owner_message_submission_result Terminal_owner_client::submit_message(
    const std::string& session_identity,
    std::uint64_t generation,
    std::uint64_t attachment_revision,
    std::span<const std::uint8_t> message_utf8)
{
    if (message_utf8.size() > detail::k_terminal_owner_maximum_frame_bytes) {
        return {};
    }
    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    write_request_prefix(writer, Terminal_owner_wire_operation::SUBMIT_MESSAGE);
    detail::write_string(writer, session_identity);
    writer
        << static_cast<quint64>(generation)
        << static_cast<quint64>(attachment_revision)
        << QByteArray(
            reinterpret_cast<const char*>(message_utf8.data()),
            static_cast<qsizetype>(message_utf8.size()));
    const auto response = m_impl->transact(std::move(request));
    if (!response) {
        return {};
    }
    QByteArray response_bytes = *response;
    auto reader = detail::make_terminal_owner_reader(response_bytes);
    quint32 routing = 0U;
    bool has_submission = false;
    if (!response_ok(reader)) {
        return {};
    }
    reader >> routing >> has_submission;
    if (reader.status() != QDataStream::Ok ||
        routing > static_cast<quint32>(
            Terminal_owner_proxy_outcome::INVALID_MESSAGE))
    {
        return {};
    }
    Terminal_owner_message_submission_result result;
    result.routing = static_cast<Terminal_owner_proxy_outcome>(routing);
    if (has_submission) {
        quint32 outcome = 0U;
        std::string error;
        reader >> outcome;
        if (!detail::read_string(reader, error) ||
            outcome > static_cast<quint32>(
                Terminal_worker_message_submission_outcome::INDETERMINATE))
        {
            return {};
        }
        result.submission = Terminal_worker_message_submission_result{
            static_cast<Terminal_worker_message_submission_outcome>(outcome),
            std::move(error),
        };
    }
    return reader.status() == QDataStream::Ok
        ? result
        : Terminal_owner_message_submission_result{};
}

bool Terminal_owner_client::contains_unprotected_settlement(
    const std::string& session_identity,
    std::uint64_t generation)
{
    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    write_request_prefix(
        writer,
        Terminal_owner_wire_operation::CONTAINS_SETTLEMENT);
    detail::write_string(writer, session_identity);
    writer << static_cast<quint64>(generation);
    const auto response = m_impl->transact(std::move(request));
    if (!response) {
        return false;
    }
    QByteArray response_bytes = *response;
    auto reader = detail::make_terminal_owner_reader(response_bytes);
    bool result = false;
    if (!response_ok(reader)) {
        return false;
    }
    reader >> result;
    return reader.status() == QDataStream::Ok && result;
}

bool Terminal_owner_client::acknowledge_unprotected_settlement(
    const std::string& session_identity,
    std::uint64_t generation)
{
    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    write_request_prefix(
        writer,
        Terminal_owner_wire_operation::ACKNOWLEDGE_SETTLEMENT);
    detail::write_string(writer, session_identity);
    writer << static_cast<quint64>(generation);
    const auto response = m_impl->transact(std::move(request));
    if (!response) {
        return false;
    }
    QByteArray response_bytes = *response;
    auto reader = detail::make_terminal_owner_reader(response_bytes);
    bool result = false;
    if (!response_ok(reader)) {
        return false;
    }
    reader >> result;
    return reader.status() == QDataStream::Ok && result;
}

} // namespace vnm::terminal_workspace
