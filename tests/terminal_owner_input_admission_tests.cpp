#include "vnm_terminal_workspace/terminal_owner_host.h"
#include "remote_ui_common/vnm_remote_ui_protocol.h"

#include <QCoreApplication>

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

namespace workspace = vnm::terminal_workspace;

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        workspace::Terminal_owner_host_configuration configuration;
        configuration.provider_namespace = "input-admission-regression";
        workspace::Terminal_owner_host owner(configuration);
        const auto result = [&](const workspace::Terminal_remote_input_message& message) {
            // No process is started. Invalid payloads must fail before custody
            // lookup; a valid message must still fail the existing custody gate.
            return owner.forward_input(1, 1, "no-session", 1, 1, message);
        };
        const auto require = [](bool condition, const char* message) {
            if (!condition) { throw std::runtime_error(message); }
        };
        using Outcome = workspace::Terminal_owner_proxy_outcome;
        for (const auto type : {vnm::k_ui_input_key_press, vnm::k_ui_input_key_release,
                               vnm::k_ui_input_text}) {
            workspace::Terminal_remote_input_message message;
            message.event_type = type;
            message.text_utf8.assign(vnm::k_remote_ui_input_max_text_bytes, 'x');
            require(result(message) == Outcome::NO_CUSTODY,
                "valid maximum-sized input must still require custody");
            message.text_utf8.push_back('x');
            require(result(message) == Outcome::INVALID_MESSAGE,
                "public owner must reject oversized input before admission");
            for (const std::string& malformed : std::vector<std::string>{
                    "\xc3", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80"}) {
                message.text_utf8 = malformed;
                require(result(message) == Outcome::INVALID_MESSAGE,
                    "public owner must reject original malformed bytes");
            }
        }
        workspace::Terminal_remote_input_message message;
        message.event_type = vnm::k_ui_input_key_press;
        message.modifiers = 0x80000000U;
        require(result(message) == Outcome::INVALID_MESSAGE,
            "public owner must reject unsupported modifiers");
        message.modifiers = 0;
        message.key = UINT32_MAX;
        require(result(message) == Outcome::INVALID_MESSAGE,
            "public owner must reject unrepresentable keys");
        message.key = 0;
        message.scroll_dx = std::numeric_limits<float>::quiet_NaN();
        require(result(message) == Outcome::INVALID_MESSAGE,
            "public owner must reject non-finite scroll");
        require(owner.custodies().empty(), "negative tests must not create a session");
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    std::puts("workspace public input admission: passed");
    return 0;
}
