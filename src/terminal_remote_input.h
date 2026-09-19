#pragma once

#include "vnm_terminal_workspace/terminal_worker_runtime.h"
#include "remote_ui_common/vnm_remote_ui_input_codec.h"

namespace vnm::terminal_workspace::detail {

inline vnm::vnm_ui_input_message_t remote_input_message(
    const Terminal_remote_input_message& message)
{
    vnm::vnm_ui_input_message_t input;
    input.event_type         = message.event_type;
    input.modifiers          = message.modifiers;
    input.x                  = message.x;
    input.y                  = message.y;
    input.button             = message.button;
    input.buttons            = message.buttons;
    input.key                = message.key;
    input.scroll_dx          = message.scroll_dx;
    input.scroll_dy          = message.scroll_dy;
    input.text_utf8          = message.text_utf8;
    input.timestamp          = message.timestamp;
    input.native_scan_code   = message.native_scan_code;
    input.native_virtual_key = message.native_virtual_key;
    input.native_modifiers   = message.native_modifiers;
    input.auto_repeat        = message.auto_repeat;
    input.count              = message.count;
    return input;
}

inline vnm::Remote_ui_input_decode_result validate_terminal_remote_input(
    const Terminal_remote_input_message& message)
{
    // Bound the copy before constructing the canonical owning representation.
    if (message.text_utf8.size() > vnm::k_remote_ui_input_max_text_bytes) {
        return vnm::Remote_ui_input_decode_result::TOO_LARGE;
    }
    return vnm::validate_remote_ui_input(remote_input_message(message));
}

} // namespace vnm::terminal_workspace::detail
