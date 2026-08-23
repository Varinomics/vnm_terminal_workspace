#pragma once

#include "vnm_terminal_workspace/terminal_owner_host.h"

#include <optional>

class QLocalSocket;

namespace vnm::terminal_workspace::detail {

std::optional<Terminal_owner_viewer_identity>
    current_terminal_owner_process_identity();
std::optional<Terminal_owner_viewer_identity>
    terminal_owner_local_socket_peer_identity(QLocalSocket& socket);

} // namespace vnm::terminal_workspace::detail
