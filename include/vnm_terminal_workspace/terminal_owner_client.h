#pragma once

#include "vnm_terminal_workspace/terminal_owner_host.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vnm::terminal_workspace {

struct Terminal_owner_service_scope
{
    std::string product_identity;
    std::string application_instance_identity;
};

struct Terminal_owner_client_configuration
{
    Terminal_owner_service_scope scope;
    std::string owner_executable_path;
    Terminal_owner_host_configuration owner;
    std::chrono::milliseconds connect_timeout = std::chrono::seconds(10);
};

enum class Terminal_owner_client_connect_outcome
{
    CONNECTED,
    INVALID_CONFIGURATION,
    OWNER_LAUNCH_FAILED,
    OWNER_UNAVAILABLE,
    AUTHORIZATION_REJECTED,
    INCOMPATIBLE_OWNER,
};

class Terminal_owner_client
{
public:
    static std::unique_ptr<Terminal_owner_client> connect(
        const Terminal_owner_client_configuration& configuration,
        Terminal_owner_client_connect_outcome* outcome = nullptr,
        std::string* diagnostic = nullptr);

    ~Terminal_owner_client();

    Terminal_owner_client(const Terminal_owner_client&) = delete;
    Terminal_owner_client& operator=(const Terminal_owner_client&) = delete;

    [[nodiscard]] Terminal_owner_viewer_epoch viewer_epoch() const noexcept;

    Terminal_owner_launch_result new_launch(
        std::span<const std::uint8_t> serialized_request,
        Launch_platform platform,
        std::span<const std::string_view> additional_reserved_names = {},
        std::optional<std::vector<environment_policy::Environment_entry>>
            authorized_environment = std::nullopt);
    Terminal_owner_update_outcome request_close(
        const std::string& session_identity,
        std::uint64_t generation);

    [[nodiscard]] std::optional<Terminal_owner_custody_snapshot> custody(
        const std::string& session_identity);
    [[nodiscard]] std::vector<Terminal_owner_custody_snapshot> custodies();
    [[nodiscard]] Terminal_owner_atomic_snapshot atomic_snapshot();

    Terminal_owner_proxy_outcome attach_existing(
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision);
    Terminal_owner_proxy_outcome forward_input(
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision,
        const Terminal_remote_input_message& message);
    Terminal_owner_proxy_outcome forward_state(
        const std::string& session_identity,
        std::uint64_t generation,
        std::uint64_t attachment_revision,
        const Terminal_remote_state_message& message);

    bool contains_unprotected_settlement(
        const std::string& session_identity,
        std::uint64_t generation);
    bool acknowledge_unprotected_settlement(
        const std::string& session_identity,
        std::uint64_t generation);

private:
    struct Impl;

    explicit Terminal_owner_client(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace vnm::terminal_workspace
