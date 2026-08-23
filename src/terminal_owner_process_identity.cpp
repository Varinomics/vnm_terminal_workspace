#include "terminal_owner_process_identity.h"

#include <QCoreApplication>
#include <QLocalSocket>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace vnm::terminal_workspace::detail {
namespace {

#if defined(Q_OS_WIN)
std::uint64_t file_time_value(const FILETIME& value)
{
    ULARGE_INTEGER encoded{};
    encoded.LowPart = value.dwLowDateTime;
    encoded.HighPart = value.dwHighDateTime;
    return encoded.QuadPart;
}

std::optional<Terminal_owner_viewer_identity> identity_for_process(
    DWORD process_id)
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
    return Terminal_owner_viewer_identity{
        process_id,
        process_id,
        file_time_value(creation),
    };
}
#endif

} // namespace

std::optional<Terminal_owner_viewer_identity>
current_terminal_owner_process_identity()
{
#if defined(Q_OS_WIN)
    return identity_for_process(GetCurrentProcessId());
#else
    const qint64 process_id = QCoreApplication::applicationPid();
    if (process_id <= 0) {
        return std::nullopt;
    }
    return Terminal_owner_viewer_identity{
        static_cast<std::uint64_t>(process_id),
        static_cast<std::uint64_t>(process_id),
        1U,
    };
#endif
}

std::optional<Terminal_owner_viewer_identity>
terminal_owner_local_socket_peer_identity(QLocalSocket& socket)
{
#if defined(Q_OS_WIN)
    ULONG process_id = 0U;
    const HANDLE pipe = reinterpret_cast<HANDLE>(socket.socketDescriptor());
    if (pipe == INVALID_HANDLE_VALUE ||
        !GetNamedPipeClientProcessId(pipe, &process_id))
    {
        return std::nullopt;
    }
    return identity_for_process(process_id);
#else
    Q_UNUSED(socket);
    return std::nullopt;
#endif
}

} // namespace vnm::terminal_workspace::detail
