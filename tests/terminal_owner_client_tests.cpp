#include "vnm_terminal_workspace/terminal_owner_client.h"

#include "terminal_owner_process_identity.h"
#include "terminal_owner_service_policy.h"
#include "terminal_owner_wire.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <qt_windows.h>

#include <TlHelp32.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
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
    std::fprintf(
        stderr,
        "FAIL: %.*s\n",
        static_cast<int>(message.size()),
        message.data());
    return false;
}

bool rejected_lower_close_is_a_bounded_service_failure()
{
    const std::array draining{
        workspace::Terminal_owner_update_outcome::APPLIED,
        workspace::Terminal_owner_update_outcome::ALREADY_CURRENT,
    };
    const std::array rejected{
        workspace::Terminal_owner_update_outcome::REJECTED,
    };
    return check(
        detail::owner_shutdown_disposition({}, true) ==
                detail::Terminal_owner_shutdown_disposition::COMPLETE &&
            detail::owner_shutdown_disposition(draining, false) ==
                detail::Terminal_owner_shutdown_disposition::DRAINING &&
            detail::owner_shutdown_disposition(rejected, false) ==
                detail::Terminal_owner_shutdown_disposition::FAILED,
        "a lower close rejection must fail the owner instead of polling forever");
}

bool wait_until(const std::function<bool()>& predicate, int timeout_ms)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() <= timeout_ms) {
        QCoreApplication::processEvents();
        if (predicate()) {
            return true;
        }
        QThread::msleep(10);
    }
    return false;
}

workspace::Terminal_owner_client_configuration configuration(
    std::string product,
    std::string instance)
{
    return {
        {std::move(product), std::move(instance)},
        VNM_TW_TEST_OWNER_PATH,
        {
            VNM_TW_TEST_HOST_PATH,
            VNM_TW_TEST_WORKER_PATH,
            "vnm_terminal_workspace.client_test",
        },
        std::chrono::seconds(15),
    };
}

workspace::Launch_request_result prepared_request(
    const QTemporaryDir& directory,
    std::string launch_request_identity = "client-owner-launch",
    std::string session_identity = "client-owner-session",
    int ping_count = 10)
{
    workspace::Terminal_launch_request request;
    request.launch_request_id = std::move(launch_request_identity);
    request.session_id = std::move(session_identity);
    request.argv = {
        QDir::toNativeSeparators(
            QStringLiteral("C:/Windows/System32/cmd.exe")).toStdString(),
        "/d",
        "/s",
        "/c",
        "ping -n " + std::to_string(ping_count) + " 127.0.0.1 >nul",
    };
    request.working_directory = QDir::toNativeSeparators(
        directory.path()).toStdString();
    request.base_environment_complete = true;
    request.base_environment = {
        {"COMSPEC", "C:\\Windows\\System32\\cmd.exe"},
        {"Path", "C:\\Windows\\System32"},
        {"SystemRoot", "C:\\Windows"},
        {"TEMP", request.working_directory},
        {"TMP", request.working_directory},
    };
    request.cancellation.identity = "client-owner-cancellation";
    return workspace::prepare_terminal_launch_request(
        std::move(request),
        workspace::Launch_platform::WINDOWS);
}

struct Exact_process_identity
{
    DWORD process_id = 0U;
    std::uint64_t creation_identity = 0U;
};

std::optional<Exact_process_identity> process_identity(DWORD process_id)
{
    const HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE,
        process_id);
    if (!process) {
        return std::nullopt;
    }
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    const bool queried = GetProcessTimes(
        process,
        &creation,
        &exit,
        &kernel,
        &user) != FALSE;
    CloseHandle(process);
    if (!queried) {
        return std::nullopt;
    }
    ULARGE_INTEGER encoded{};
    encoded.LowPart = creation.dwLowDateTime;
    encoded.HighPart = creation.dwHighDateTime;
    return Exact_process_identity{process_id, encoded.QuadPart};
}

bool process_is_alive(const Exact_process_identity& identity)
{
    const HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE,
        identity.process_id);
    if (!process) {
        return false;
    }
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    const bool queried = GetProcessTimes(
        process,
        &creation,
        &exit,
        &kernel,
        &user) != FALSE;
    ULARGE_INTEGER encoded{};
    encoded.LowPart = creation.dwLowDateTime;
    encoded.HighPart = creation.dwHighDateTime;
    const bool alive = queried && encoded.QuadPart == identity.creation_identity &&
        WaitForSingleObject(process, 0U) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

std::optional<Exact_process_identity> child_process_with_image(
    DWORD parent_process_id,
    const QString& expected_image)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::optional<Exact_process_identity> result;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID != parent_process_id) {
                continue;
            }
            const HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                entry.th32ProcessID);
            if (!process) {
                continue;
            }
            std::wstring image(32768U, L'\0');
            DWORD image_size = static_cast<DWORD>(image.size());
            const bool queried = QueryFullProcessImageNameW(
                process,
                0U,
                image.data(),
                &image_size) != FALSE;
            CloseHandle(process);
            if (!queried) {
                continue;
            }
            image.resize(image_size);
            const QString actual_image = QDir::cleanPath(
                QString::fromStdWString(image));
            if (actual_image.compare(
                    QDir::cleanPath(expected_image),
                    Qt::CaseInsensitive) == 0)
            {
                const auto identity = process_identity(entry.th32ProcessID);
                if (identity && (!result || identity->creation_identity >
                        result->creation_identity))
                {
                    result = identity;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool unauthorized_invitation_is_rejected()
{
    const auto viewer = detail::current_terminal_owner_process_identity();
    QTemporaryDir directory;
    if (!check(viewer && directory.isValid(),
            "the unauthorized-peer fixture must obtain an exact identity"))
    {
        return false;
    }

    const QString unique = QString::number(
        static_cast<qlonglong>(QCoreApplication::applicationPid()));
    const QString endpoint = QStringLiteral("vnm-tw-rejected-") + unique;
    const QString token = QStringLiteral("expected-invitation-") + unique;
    const QStringList arguments{
        QStringLiteral("--vnm-terminal-owner"),
        QStringLiteral("--endpoint=") + endpoint,
        QStringLiteral("--lock-path=") + directory.filePath("owner.lock"),
        QStringLiteral("--invitation-token=") + token,
        QStringLiteral("--viewer-pid=") +
            QString::number(viewer->native_process_id),
        QStringLiteral("--viewer-creation=") +
            QString::number(viewer->native_process_creation_identity),
        QStringLiteral("--host-executable=") +
            QStringLiteral(VNM_TW_TEST_HOST_PATH),
        QStringLiteral("--worker-library=") +
            QStringLiteral(VNM_TW_TEST_WORKER_PATH),
        QStringLiteral("--provider-namespace=client-test-unauthorized"),
    };
    qint64 owner_process_id = 0;
    if (!check(
            QProcess::startDetached(
                QStringLiteral(VNM_TW_TEST_OWNER_PATH),
                arguments,
                QFileInfo(QStringLiteral(VNM_TW_TEST_OWNER_PATH)).absolutePath(),
                &owner_process_id),
            "the unauthorized-peer fixture owner must launch"))
    {
        return false;
    }
    const auto owner = process_identity(static_cast<DWORD>(owner_process_id));

    QLocalSocket socket;
    QDeadlineTimer deadline(5000);
    while (!deadline.hasExpired() &&
           socket.state() != QLocalSocket::ConnectedState)
    {
        socket.connectToServer(endpoint);
        if (!socket.waitForConnected(100)) {
            socket.abort();
            QThread::msleep(10);
        }
    }
    bool ok = true;
    ok &= check(
        socket.state() == QLocalSocket::ConnectedState,
        "the unauthorized peer must reach the exact invitation endpoint");
    if (!ok) {
        return false;
    }

    QByteArray request;
    auto writer = detail::make_terminal_owner_writer(request);
    writer
        << static_cast<quint32>(detail::k_terminal_owner_wire_version)
        << static_cast<quint32>(detail::Terminal_owner_wire_operation::HANDSHAKE);
    detail::write_string(writer, "wrong-invitation");
    writer
        << static_cast<quint64>(viewer->native_process_id)
        << static_cast<quint64>(viewer->native_process_creation_identity);
    const QByteArray framed = detail::frame_terminal_owner_message(request);
    ok &= check(
        socket.write(framed) == framed.size() && socket.waitForBytesWritten(1000),
        "the unauthorized handshake request must be delivered");

    QByteArray response_buffer;
    QByteArray response;
    ok &= check(
        wait_until(
            [&socket, &response_buffer, &response]() {
                if (socket.bytesAvailable() == 0) {
                    (void)socket.waitForReadyRead(20);
                }
                response_buffer.append(socket.readAll());
                return detail::take_terminal_owner_frame(
                    response_buffer,
                    response);
            },
            3000),
        "the unauthorized handshake must receive a typed rejection");
    if (!response.isEmpty()) {
        auto reader = detail::make_terminal_owner_reader(response);
        quint32 version = 0U;
        quint32 status = 0U;
        quint32 category = 0U;
        reader >> version >> status >> category;
        ok &= check(
            reader.status() == QDataStream::Ok &&
                version == detail::k_terminal_owner_wire_version &&
                status == static_cast<quint32>(
                    detail::Terminal_owner_wire_status::UNAUTHORIZED) &&
                category == 3U,
            "a wrong one-time invitation must be rejected before authority bind");
    }
    socket.abort();
    ok &= check(
        owner && wait_until(
            [&owner]() { return !process_is_alive(*owner); },
            5000),
        "the rejected invitation owner must release its endpoint and lock");
    return ok;
}

bool scoped_owner_and_attach_existing()
{
    const std::string unique = std::to_string(
        static_cast<long long>(QCoreApplication::applicationPid()));
    auto primary_configuration = configuration(
        "client-test-product",
        "primary-" + unique);
    workspace::Terminal_owner_client_connect_outcome connect_outcome{};
    std::string diagnostic;
    auto client = workspace::Terminal_owner_client::connect(
        primary_configuration,
        &connect_outcome,
        &diagnostic);
    if (!client) {
        std::fprintf(
            stderr,
            "connect outcome=%d diagnostic=%s\n",
            static_cast<int>(connect_outcome),
            diagnostic.c_str());
    }
    bool ok = true;
    ok &= check(
        client &&
            connect_outcome ==
                workspace::Terminal_owner_client_connect_outcome::CONNECTED &&
            client->viewer_epoch() != 0U,
        "the scoped client must receive one authenticated viewer epoch");
    if (!client) {
        return false;
    }

    auto duplicate_configuration = primary_configuration;
    duplicate_configuration.connect_timeout = std::chrono::milliseconds(500);
    auto duplicate = workspace::Terminal_owner_client::connect(
        duplicate_configuration,
        &connect_outcome,
        &diagnostic);
    ok &= check(
        !duplicate,
        "the same product/application instance must not admit a second viewer");

    auto other_product_configuration = configuration(
        "other-client-test-product",
        "primary-" + unique);
    auto other_instance_configuration = configuration(
        "client-test-product",
        "secondary-" + unique);
    workspace::Terminal_owner_client_connect_outcome other_product_outcome{};
    workspace::Terminal_owner_client_connect_outcome other_instance_outcome{};
    std::string other_product_diagnostic;
    std::string other_instance_diagnostic;
    auto other_product = workspace::Terminal_owner_client::connect(
        other_product_configuration,
        &other_product_outcome,
        &other_product_diagnostic);
    auto other_instance = workspace::Terminal_owner_client::connect(
        other_instance_configuration,
        &other_instance_outcome,
        &other_instance_diagnostic);
    if (!other_product || !other_instance) {
        std::fprintf(
            stderr,
            "independent scopes: product outcome=%d diagnostic=%s; "
            "instance outcome=%d diagnostic=%s\n",
            static_cast<int>(other_product_outcome),
            other_product_diagnostic.c_str(),
            static_cast<int>(other_instance_outcome),
            other_instance_diagnostic.c_str());
    }
    ok &= check(
        other_product && other_instance,
        "different product and same-product/different-instance scopes must be independent");
    other_product.reset();
    other_instance.reset();

    QTemporaryDir directory;
    const workspace::Launch_request_result request = prepared_request(directory);
    ok &= check(
        directory.isValid() &&
            request.status == workspace::Launch_request_status::ACCEPTED,
        "the cross-process owner request must prepare");
    if (!ok) {
        return false;
    }

    const workspace::Terminal_owner_launch_result launch = client->new_launch(
        request.serialized_request,
        workspace::Launch_platform::WINDOWS);
    ok &= check(
        launch.outcome == workspace::Terminal_owner_launch_outcome::ADMITTED &&
            launch.generation != 0U,
        "new-launch must admit only the hosted worker and return its generation");
    if (!ok) {
        return false;
    }

    std::optional<workspace::Terminal_owner_custody_snapshot> running;
    ok &= check(
        wait_until(
            [&client, &running]() {
                running = client->custody("client-owner-session");
                return
                    running &&
                    running->state ==
                        workspace::Terminal_owner_custody_state::RUNNING &&
                    running->attachment.live &&
                    running->attachment.revision != 0U;
            },
            20000),
        "the shared owner must reconcile ready, child, and attachment facts");
    if (!ok || !running) {
        return false;
    }

    const auto before_attach = *running;
    ok &= check(
        client->attach_existing(
            before_attach.session_identity,
            before_attach.generation,
            before_attach.attachment.revision) ==
            workspace::Terminal_owner_proxy_outcome::ADMITTED,
        "attach-existing must admit the current live attachment");
    const auto after_attach = client->custody(before_attach.session_identity);
    ok &= check(
        after_attach &&
            after_attach->generation == before_attach.generation &&
            after_attach->attachment.revision ==
                before_attach.attachment.revision &&
            after_attach->attachment.producer_process_id ==
                before_attach.attachment.producer_process_id &&
            after_attach->attachment.framebuffer_path ==
                before_attach.attachment.framebuffer_path,
        "attach-existing must preserve generation, worker, and surface identity");
    ok &= check(
        client->attach_existing(
            before_attach.session_identity,
            before_attach.generation,
            before_attach.attachment.revision + 1U) ==
            workspace::Terminal_owner_proxy_outcome::STALE_ATTACHMENT,
        "attach-existing must fence a stale attachment revision");

    workspace::Terminal_remote_state_message resize;
    resize.state_type = 1U;
    resize.width = 800;
    resize.height = 600;
    ok &= check(
        client->forward_state(
            before_attach.session_identity,
            before_attach.generation,
            before_attach.attachment.revision,
            resize) == workspace::Terminal_owner_proxy_outcome::ADMITTED,
        "only the authenticated current attachment may forward state");

    ok &= check(
        client->request_close(
            before_attach.session_identity,
            before_attach.generation) ==
            workspace::Terminal_owner_update_outcome::APPLIED,
        "explicit close must enter the sole closing path");
    ok &= check(
        wait_until(
            [&client]() {
                return client->custodies().empty();
            },
            15000),
        "the shared owner must settle and remove custody");
    ok &= check(
        client->contains_unprotected_settlement(
            before_attach.session_identity,
            before_attach.generation),
        "unprotected settlement must enter the neutral receipt inbox");
    const auto settled_snapshot = client->atomic_snapshot();
    ok &= check(
        settled_snapshot.revision != 0U &&
            settled_snapshot.custodies.empty() &&
            settled_snapshot.unprotected_receipts.size() == 1U &&
            settled_snapshot.unprotected_receipts.front().session_identity ==
                before_attach.session_identity &&
            settled_snapshot.unprotected_receipts.front().generation ==
                before_attach.generation,
        "one atomic barrier must expose settled custody removal and its receipt");
    ok &= check(
        client->acknowledge_unprotected_settlement(
            before_attach.session_identity,
            before_attach.generation) &&
            !client->contains_unprotected_settlement(
                before_attach.session_identity,
                before_attach.generation),
        "exact receipt acknowledgement must remove the neutral receipt");
    const auto acknowledged_snapshot = client->atomic_snapshot();
    ok &= check(
        acknowledged_snapshot.revision > settled_snapshot.revision &&
            acknowledged_snapshot.custodies.empty() &&
            acknowledged_snapshot.unprotected_receipts.empty(),
        "receipt acknowledgement must advance the atomic barrier without history");
    client.reset();
    return ok;
}

bool unprotected_disconnect_closes_and_drains()
{
    const std::string unique = std::to_string(
        static_cast<long long>(QCoreApplication::applicationPid()));
    const auto owner_configuration = configuration(
        "client-disconnect-product",
        "primary-" + unique);
    auto client = workspace::Terminal_owner_client::connect(owner_configuration);
    bool ok = true;
    ok &= check(
        client != nullptr,
        "the unprotected-disconnect fixture must connect to its scoped owner");
    if (!client) {
        return false;
    }

    std::optional<Exact_process_identity> owner_process;
    ok &= check(
        wait_until(
            [&owner_process]() {
                owner_process = child_process_with_image(
                    GetCurrentProcessId(),
                    QStringLiteral(VNM_TW_TEST_OWNER_PATH));
                return owner_process.has_value();
            },
            3000),
        "the test must identify the exact scoped owner process");

    QTemporaryDir directory;
    const auto request = prepared_request(
        directory,
        "client-disconnect-launch",
        "client-disconnect-session",
        30);
    const auto launch = client->new_launch(
        request.serialized_request,
        workspace::Launch_platform::WINDOWS);
    std::optional<workspace::Terminal_owner_custody_snapshot> running;
    ok &= check(
        launch.outcome == workspace::Terminal_owner_launch_outcome::ADMITTED &&
            wait_until(
                [&client, &running]() {
                    running = client->custody("client-disconnect-session");
                    return running &&
                        running->state ==
                            workspace::Terminal_owner_custody_state::RUNNING &&
                        running->attachment.live;
                },
                20000),
        "the unprotected owner must reach RUNNING with a live hosted attachment");
    if (!ok || !owner_process || !running) {
        return false;
    }
    const auto hosted_worker_process = process_identity(
        static_cast<DWORD>(running->attachment.producer_process_id));
    ok &= check(
        hosted_worker_process && process_is_alive(*hosted_worker_process),
        "the running attachment must identify the exact hosted-worker process");

    client.reset();
    ok &= check(
        wait_until(
            [&owner_process, &hosted_worker_process]() {
                return !process_is_alive(*owner_process) &&
                    !process_is_alive(*hosted_worker_process);
            },
            20000),
        "unprotected disconnect must close, settle, drain, and exit owner and host");

    auto replacement = workspace::Terminal_owner_client::connect(
        owner_configuration);
    ok &= check(
        replacement != nullptr,
        "a drained unprotected owner must release its scoped lock and endpoint");
    std::optional<Exact_process_identity> replacement_owner;
    ok &= check(
        wait_until(
            [&replacement_owner]() {
                replacement_owner = child_process_with_image(
                    GetCurrentProcessId(),
                    QStringLiteral(VNM_TW_TEST_OWNER_PATH));
                return replacement_owner.has_value();
            },
            3000),
        "the replacement owner must have an exact process identity");
    replacement.reset();
    ok &= check(
        replacement_owner && wait_until(
            [&replacement_owner]() {
                return !process_is_alive(*replacement_owner);
            },
            5000),
        "the empty replacement owner must shut down without retained state");
    return ok;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    bool ok = true;
    ok &= rejected_lower_close_is_a_bounded_service_failure();
    ok &= unauthorized_invitation_is_rejected();
    ok &= scoped_owner_and_attach_existing();
    ok &= unprotected_disconnect_closes_and_drains();
    return ok ? 0 : 1;
}
