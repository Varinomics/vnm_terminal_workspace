#include "terminal_remote_input.h"

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

namespace workspace = vnm::terminal_workspace;
namespace detail = vnm::terminal_workspace::detail;

namespace {
std::size_t checks = 0;
void require(bool condition, const char* message)
{
    ++checks;
    if (!condition) { throw std::runtime_error(message); }
}

void run()
{
    using Result = vnm::Remote_ui_input_decode_result;
    workspace::Terminal_remote_input_message message;
    message.event_type = vnm::k_ui_input_key_press;
    message.modifiers = vnm::k_ui_mod_mask;
    message.x = -17;
    message.y = 29;
    message.button = vnm::k_ui_button_right;
    message.buttons = vnm::k_ui_button_left | vnm::k_ui_button_right;
    message.key = 'Q';
    message.scroll_dx = 1.25F;
    message.scroll_dy = -2.5F;
    message.text_utf8 = std::string("\xef\xbb\xbf\0\xf0\x9f\x99\x82", 8);
    message.timestamp = UINT64_MAX;
    message.native_scan_code = 0x1234;
    message.native_virtual_key = 0x51;
    message.native_modifiers = 0xfedcba98;
    message.auto_repeat = true;
    message.count = UINT16_MAX;
    const auto canonical = detail::remote_input_message(message);
    require(canonical.event_type == message.event_type &&
        canonical.modifiers == message.modifiers && canonical.x == -17 && canonical.y == 29 &&
        canonical.button == message.button && canonical.buttons == message.buttons &&
        canonical.key == 'Q' && canonical.scroll_dx == 1.25F && canonical.scroll_dy == -2.5F &&
        canonical.text_utf8 == message.text_utf8 && canonical.timestamp == UINT64_MAX &&
        canonical.native_scan_code == 0x1234 && canonical.native_virtual_key == 0x51 &&
        canonical.native_modifiers == 0xfedcba98 && canonical.auto_repeat &&
        canonical.count == UINT16_MAX, "all owner fields must map exactly");
    require(detail::validate_terminal_remote_input(message) == Result::DECODED,
        "native metadata, U+FEFF, NUL and supplementary UTF-8 are valid");
    vnm::vnm_ui_input_message_t decoded;
    require(vnm::decode_remote_ui_input(vnm::encode_remote_ui_input(canonical), decoded) ==
        Result::DECODED && decoded == canonical, "mapped owner input must round-trip exactly");

    const auto valid = message;
    for (const auto type : {vnm::k_ui_input_key_press, vnm::k_ui_input_key_release,
                           vnm::k_ui_input_text}) {
        message = valid;
        message.event_type = type;
        for (const std::size_t size : {std::size_t{0}, std::size_t{1},
                vnm::k_remote_ui_input_max_text_bytes}) {
            message.text_utf8.assign(size, 'x');
            require(detail::validate_terminal_remote_input(message) == Result::DECODED,
                "empty and boundary-sized text must remain valid");
        }
        message.text_utf8.push_back('x');
        require(detail::validate_terminal_remote_input(message) == Result::TOO_LARGE,
            "65,537 bytes must be rejected before mapping/copying");
        for (const std::string& malformed : std::vector<std::string>{
                "\xc3", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80"}) {
            message.text_utf8 = malformed;
            require(detail::validate_terminal_remote_input(message) == Result::MALFORMED,
                "malformed UTF-8 must not reach Qt conversion");
        }
    }
    message = valid; message.event_type = 0;
    require(detail::validate_terminal_remote_input(message) == Result::MALFORMED,
        "invalid event type must fail");
    message = valid; message.modifiers = 0x80000000U;
    require(detail::validate_terminal_remote_input(message) == Result::MALFORMED,
        "unsupported modifiers must fail");
    message = valid; message.key = UINT32_MAX;
    require(detail::validate_terminal_remote_input(message) == Result::MALFORMED,
        "unrepresentable Qt key must fail");
    message = valid; message.scroll_dx = std::numeric_limits<float>::infinity();
    require(detail::validate_terminal_remote_input(message) == Result::MALFORMED,
        "infinite scroll must fail");
    message = valid; message.scroll_dy = std::numeric_limits<float>::quiet_NaN();
    require(detail::validate_terminal_remote_input(message) == Result::MALFORMED,
        "NaN scroll must fail");
}
}

int main()
{
    try { run(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL after %zu checks: %s\n", checks, error.what());
        return 1;
    }
    std::printf("workspace canonical input: %zu checks passed\n", checks);
    return 0;
}
