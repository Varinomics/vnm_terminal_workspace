#include "vnm_terminal_workspace/terminal_worker_envelope.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace vnm::terminal_workspace {
namespace {

constexpr std::array<std::uint8_t, 8> k_envelope_magic{
    'V', 'N', 'M', 'T', 'W', 'E', '0', '1',
};
constexpr std::string_view k_json_prefix =
    "{\"vnm_terminal_workspace_envelope@1\":\"";
constexpr std::string_view k_json_suffix = "\"}";
constexpr std::size_t k_maximum_provider_namespace_bytes = 256U;
constexpr std::size_t k_maximum_configuration_string_bytes = 4096U;
constexpr std::size_t k_maximum_binary_envelope_bytes =
    Terminal_launch_request_limits::maximum_payload_bytes +
    Terminal_launch_request_limits::maximum_environment_bytes +
    64U * 1024U;

void clear_bytes(std::vector<std::uint8_t>& bytes)
{
    volatile std::uint8_t* data = bytes.data();
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        data[index] = 0U;
    }
    bytes.clear();
}

void clear_string(std::string& value)
{
    volatile char* data = value.data();
    for (std::size_t index = 0U; index < value.size(); ++index) {
        data[index] = '\0';
    }
    value.clear();
}

void clear_envelope_values(Terminal_worker_envelope& envelope)
{
    clear_bytes(envelope.serialized_request);
    if (!envelope.authorized_environment) {
        return;
    }
    for (environment_policy::Environment_entry& entry :
         *envelope.authorized_environment)
    {
        clear_string(entry.value);
    }
    envelope.authorized_environment.reset();
}

class Sensitive_envelope_guard
{
public:
    explicit Sensitive_envelope_guard(Terminal_worker_envelope& envelope)
    :
        m_envelope(envelope)
    {}

    ~Sensitive_envelope_guard()
    {
        clear_envelope_values(m_envelope);
    }

    Sensitive_envelope_guard(const Sensitive_envelope_guard&) = delete;
    Sensitive_envelope_guard& operator=(const Sensitive_envelope_guard&) =
        delete;

private:
    Terminal_worker_envelope& m_envelope;
};

bool contains_nul(std::string_view value)
{
    return value.find('\0') != std::string_view::npos;
}

bool valid_utf8(std::string_view value)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t index = 0U;
    while (index < value.size()) {
        const unsigned char first = bytes[index];
        std::size_t length = 0U;
        std::uint32_t codepoint = 0U;
        if (first <= 0x7fU) {
            length = 1U;
            codepoint = first;
        }
        else if (first >= 0xc2U && first <= 0xdfU) {
            length = 2U;
            codepoint = first & 0x1fU;
        }
        else if (first >= 0xe0U && first <= 0xefU) {
            length = 3U;
            codepoint = first & 0x0fU;
        }
        else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4U;
            codepoint = first & 0x07U;
        }
        else {
            return false;
        }
        if (index + length > value.size()) {
            return false;
        }
        for (std::size_t offset = 1U; offset < length; ++offset) {
            const unsigned char continuation = bytes[index + offset];
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if ((length == 2U && codepoint < 0x80U) ||
            (length == 3U && codepoint < 0x800U) ||
            (length == 4U && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            codepoint > 0x10ffffU)
        {
            return false;
        }
        index += length;
    }
    return true;
}

class Byte_writer
{
public:
    void append(std::span<const std::uint8_t> bytes)
    {
        m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
    }

    void append_u8(std::uint8_t value)
    {
        m_bytes.push_back(value);
    }

    void append_u32(std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8) {
            m_bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void append_u64(std::uint64_t value)
    {
        for (int shift = 0; shift < 64; shift += 8) {
            m_bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void append_string(std::string_view value)
    {
        append_u32(static_cast<std::uint32_t>(value.size()));
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
        append({bytes, value.size()});
    }

    std::vector<std::uint8_t> take()
    {
        return std::move(m_bytes);
    }

private:
    std::vector<std::uint8_t> m_bytes;
};

class Byte_reader
{
public:
    explicit Byte_reader(std::span<const std::uint8_t> bytes)
    :
        m_bytes(bytes)
    {}

    bool read_exact(std::span<const std::uint8_t> expected)
    {
        if (remaining() < expected.size()) {
            return false;
        }
        const auto candidate = m_bytes.subspan(m_offset, expected.size());
        if (!std::equal(candidate.begin(), candidate.end(), expected.begin())) {
            return false;
        }
        m_offset += expected.size();
        return true;
    }

    bool read_u8(std::uint8_t& value)
    {
        if (remaining() < 1U) {
            return false;
        }
        value = m_bytes[m_offset++];
        return true;
    }

    bool read_u32(std::uint32_t& value)
    {
        if (remaining() < 4U) {
            return false;
        }
        value = 0U;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(m_bytes[m_offset++]) << shift;
        }
        return true;
    }

    bool read_u64(std::uint64_t& value)
    {
        if (remaining() < 8U) {
            return false;
        }
        value = 0U;
        for (int shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(m_bytes[m_offset++]) << shift;
        }
        return true;
    }

    bool read_string(std::string& value, std::size_t maximum_bytes)
    {
        std::uint32_t size = 0U;
        if (!read_u32(size) || size > maximum_bytes || remaining() < size) {
            return false;
        }
        const char* data = reinterpret_cast<const char*>(
            m_bytes.data() + m_offset);
        value.assign(data, size);
        m_offset += size;
        return true;
    }

    bool read_bytes(std::vector<std::uint8_t>& value, std::size_t maximum_bytes)
    {
        std::uint32_t size = 0U;
        if (!read_u32(size) || size > maximum_bytes || remaining() < size) {
            return false;
        }
        const auto bytes = m_bytes.subspan(m_offset, size);
        value.assign(bytes.begin(), bytes.end());
        m_offset += size;
        return true;
    }

    [[nodiscard]] bool at_end() const
    {
        return m_offset == m_bytes.size();
    }

private:
    [[nodiscard]] std::size_t remaining() const
    {
        return m_bytes.size() - m_offset;
    }

    std::span<const std::uint8_t> m_bytes;
    std::size_t m_offset = 0U;
};

constexpr std::array<char, 64> k_base64_alphabet{
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3',
    '4', '5', '6', '7', '8', '9', '+', '/',
};

std::string base64_encode(std::span<const std::uint8_t> bytes)
{
    std::string result;
    result.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t index = 0U; index < bytes.size(); index += 3U) {
        const std::uint32_t first = bytes[index];
        const std::uint32_t second = index + 1U < bytes.size()
            ? bytes[index + 1U]
            : 0U;
        const std::uint32_t third = index + 2U < bytes.size()
            ? bytes[index + 2U]
            : 0U;
        const std::uint32_t group = (first << 16U) | (second << 8U) | third;
        result.push_back(k_base64_alphabet[(group >> 18U) & 0x3fU]);
        result.push_back(k_base64_alphabet[(group >> 12U) & 0x3fU]);
        result.push_back(index + 1U < bytes.size()
            ? k_base64_alphabet[(group >> 6U) & 0x3fU]
            : '=');
        result.push_back(index + 2U < bytes.size()
            ? k_base64_alphabet[group & 0x3fU]
            : '=');
    }
    return result;
}

int base64_value(char character)
{
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

std::optional<std::vector<std::uint8_t>> base64_decode(
    std::string_view encoded)
{
    if (encoded.empty() || encoded.size() % 4U != 0U) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> result;
    result.reserve((encoded.size() / 4U) * 3U);
    for (std::size_t index = 0U; index < encoded.size(); index += 4U) {
        const bool third_padding = encoded[index + 2U] == '=';
        const bool fourth_padding = encoded[index + 3U] == '=';
        if (third_padding && !fourth_padding) {
            clear_bytes(result);
            return std::nullopt;
        }
        if ((third_padding || fourth_padding) && index + 4U != encoded.size()) {
            clear_bytes(result);
            return std::nullopt;
        }
        const int first = base64_value(encoded[index]);
        const int second = base64_value(encoded[index + 1U]);
        const int third = third_padding ? 0 : base64_value(encoded[index + 2U]);
        const int fourth = fourth_padding ? 0 : base64_value(encoded[index + 3U]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0) {
            clear_bytes(result);
            return std::nullopt;
        }
        const std::uint32_t group =
            (static_cast<std::uint32_t>(first) << 18U) |
            (static_cast<std::uint32_t>(second) << 12U) |
            (static_cast<std::uint32_t>(third) << 6U) |
            static_cast<std::uint32_t>(fourth);
        result.push_back(static_cast<std::uint8_t>(group >> 16U));
        if (!third_padding) {
            result.push_back(static_cast<std::uint8_t>(group >> 8U));
        }
        if (!fourth_padding) {
            result.push_back(static_cast<std::uint8_t>(group));
        }
    }
    std::string canonical = base64_encode(result);
    const bool is_canonical = canonical == encoded;
    clear_string(canonical);
    if (!is_canonical) {
        clear_bytes(result);
        return std::nullopt;
    }
    return result;
}

Terminal_worker_envelope_result rejected(
    Terminal_worker_envelope_error error)
{
    Terminal_worker_envelope_result result;
    result.error = error;
    return result;
}

bool valid_configuration_string(std::string_view value)
{
    return value.size() <= k_maximum_configuration_string_bytes &&
        !contains_nul(value) && valid_utf8(value);
}

bool valid_surface_configuration(
    const Terminal_worker_surface_configuration& configuration,
    const std::optional<Terminal_worker_output_capture_configuration>& capture)
{
    if (configuration.logical_width <= 0 ||
        configuration.logical_height <= 0 ||
        configuration.maximum_physical_width <= 0 ||
        configuration.maximum_physical_height <= 0 ||
        configuration.logical_width > configuration.maximum_physical_width ||
        configuration.logical_height > configuration.maximum_physical_height ||
        !std::isfinite(configuration.scale_factor) ||
        configuration.scale_factor <= 0.0F ||
        !std::isfinite(configuration.settings.font_size) ||
        configuration.settings.font_size <= 0.0 ||
        !std::isfinite(configuration.scrollbar_width) ||
        configuration.scrollbar_width < 0.0 ||
        !valid_configuration_string(configuration.settings.color_scheme) ||
        !valid_configuration_string(configuration.settings.font_family) ||
        !valid_configuration_string(configuration.title) ||
        static_cast<unsigned>(configuration.settings.text_renderer_mode) >
            static_cast<unsigned>(Terminal_worker_text_renderer_mode::GLYPH) ||
        static_cast<unsigned>(configuration.settings.lcd_subpixel_order) >
            static_cast<unsigned>(Terminal_worker_lcd_subpixel_order::VBGR) ||
        static_cast<unsigned>(configuration.style) >
            static_cast<unsigned>(Terminal_worker_style::DARK) ||
        (configuration.settings.scrollback_limit &&
         *configuration.settings.scrollback_limit <= 0))
    {
        return false;
    }
    return !capture ||
        (!capture->base_path.empty() && capture->maximum_bytes > 0U &&
         valid_configuration_string(capture->base_path));
}

Terminal_worker_envelope_error validate_envelope(
    const Terminal_worker_envelope& envelope)
{
    if (!valid_launch_platform(envelope.platform)) {
        return Terminal_worker_envelope_error::MALFORMED_PAYLOAD;
    }
    if (envelope.provider_namespace.empty() ||
        envelope.provider_namespace.size() > k_maximum_provider_namespace_bytes ||
        contains_nul(envelope.provider_namespace) ||
        !valid_utf8(envelope.provider_namespace))
    {
        return Terminal_worker_envelope_error::INVALID_PROVIDER_NAMESPACE;
    }
    if (envelope.serialized_request.empty() ||
        envelope.serialized_request.size() >
            Terminal_launch_request_limits::maximum_payload_bytes)
    {
        return Terminal_worker_envelope_error::INVALID_REQUEST;
    }
    if (!valid_surface_configuration(
            envelope.surface_configuration,
            envelope.output_capture))
    {
        return Terminal_worker_envelope_error::MALFORMED_PAYLOAD;
    }
    if (!envelope.authorized_environment) {
        return Terminal_worker_envelope_error::NONE;
    }
    if (envelope.authorized_environment->size() >
        Terminal_launch_request_limits::maximum_environment_entries)
    {
        return Terminal_worker_envelope_error::QUOTA_EXCEEDED;
    }
    std::size_t bytes = 0U;
    for (const environment_policy::Environment_entry& entry :
         *envelope.authorized_environment)
    {
        if (entry.name.empty() || contains_nul(entry.name) ||
            entry.name.find('=') != std::string::npos ||
            contains_nul(entry.value) || !valid_utf8(entry.name) ||
            !valid_utf8(entry.value) ||
            entry.name.size() >
                Terminal_launch_request_limits::maximum_environment_bytes ||
            entry.value.size() >
                Terminal_launch_request_limits::maximum_environment_bytes ||
            entry.name.size() + entry.value.size() >
                Terminal_launch_request_limits::maximum_environment_bytes - bytes)
        {
            return Terminal_worker_envelope_error::
                INVALID_AUTHORIZED_ENVIRONMENT;
        }
        bytes += entry.name.size() + entry.value.size();
    }
    return Terminal_worker_envelope_error::NONE;
}

} // namespace

Terminal_worker_envelope_result encode_terminal_worker_envelope(
    const Terminal_worker_envelope& envelope)
{
    const Terminal_worker_envelope_error validation =
        validate_envelope(envelope);
    if (validation != Terminal_worker_envelope_error::NONE) {
        return rejected(validation);
    }

    Byte_writer writer;
    writer.append(k_envelope_magic);
    writer.append_u32(k_terminal_worker_envelope_schema_version);
    writer.append_u8(
        envelope.platform == Launch_platform::WINDOWS ? 0U : 1U);
    writer.append_string(envelope.provider_namespace);
    writer.append_u32(
        static_cast<std::uint32_t>(envelope.serialized_request.size()));
    writer.append(envelope.serialized_request);
    const Terminal_worker_surface_configuration& surface =
        envelope.surface_configuration;
    writer.append_u32(static_cast<std::uint32_t>(surface.logical_width));
    writer.append_u32(static_cast<std::uint32_t>(surface.logical_height));
    writer.append_u32(
        static_cast<std::uint32_t>(surface.maximum_physical_width));
    writer.append_u32(
        static_cast<std::uint32_t>(surface.maximum_physical_height));
    writer.append_u32(std::bit_cast<std::uint32_t>(surface.scale_factor));
    writer.append_string(surface.settings.color_scheme);
    writer.append_string(surface.settings.font_family);
    writer.append_u64(std::bit_cast<std::uint64_t>(
        surface.settings.font_size));
    writer.append_u8(static_cast<std::uint8_t>(
        surface.settings.text_renderer_mode));
    writer.append_u8(static_cast<std::uint8_t>(
        surface.settings.lcd_subpixel_order));
    writer.append_u8(surface.settings.row_timestamp_tooltip_enabled ? 1U : 0U);
    writer.append_u8(surface.settings.scrollback_limit ? 1U : 0U);
    writer.append_u32(static_cast<std::uint32_t>(
        surface.settings.scrollback_limit.value_or(0)));
    writer.append_string(surface.title);
    writer.append_u8(static_cast<std::uint8_t>(surface.style));
    writer.append_u64(std::bit_cast<std::uint64_t>(surface.scrollbar_width));
    writer.append_u8(envelope.output_capture ? 1U : 0U);
    if (envelope.output_capture) {
        writer.append_string(envelope.output_capture->base_path);
        writer.append_u64(static_cast<std::uint64_t>(
            envelope.output_capture->maximum_bytes));
    }
    writer.append_u8(envelope.authorized_environment ? 1U : 0U);
    writer.append_u32(static_cast<std::uint32_t>(
        envelope.authorized_environment
            ? envelope.authorized_environment->size()
            : 0U));
    if (envelope.authorized_environment) {
        for (const environment_policy::Environment_entry& entry :
             *envelope.authorized_environment)
        {
            writer.append_string(entry.name);
            writer.append_string(entry.value);
        }
    }
    std::vector<std::uint8_t> binary = writer.take();
    if (binary.size() > k_maximum_binary_envelope_bytes) {
        clear_bytes(binary);
        return rejected(Terminal_worker_envelope_error::QUOTA_EXCEEDED);
    }
    std::string encoded = base64_encode(binary);
    clear_bytes(binary);

    Terminal_worker_envelope_result result;
    result.error = Terminal_worker_envelope_error::NONE;
    result.serialized_envelope.reserve(
        k_json_prefix.size() + encoded.size() + k_json_suffix.size());
    result.serialized_envelope.append(k_json_prefix);
    result.serialized_envelope.append(encoded);
    result.serialized_envelope.append(k_json_suffix);
    clear_string(encoded);
    return result;
}

Terminal_worker_envelope_result decode_terminal_worker_envelope(
    std::string_view serialized_envelope)
{
    if (!serialized_envelope.starts_with(k_json_prefix) ||
        !serialized_envelope.ends_with(k_json_suffix) ||
        serialized_envelope.size() <=
            k_json_prefix.size() + k_json_suffix.size())
    {
        return rejected(Terminal_worker_envelope_error::MALFORMED_PAYLOAD);
    }
    const std::string_view encoded = serialized_envelope.substr(
        k_json_prefix.size(),
        serialized_envelope.size() -
            k_json_prefix.size() - k_json_suffix.size());
    std::optional<std::vector<std::uint8_t>> binary = base64_decode(encoded);
    if (!binary || binary->size() > k_maximum_binary_envelope_bytes) {
        if (binary) {
            clear_bytes(*binary);
        }
        return rejected(Terminal_worker_envelope_error::MALFORMED_PAYLOAD);
    }

    Byte_reader reader(*binary);
    if (!reader.read_exact(k_envelope_magic)) {
        clear_bytes(*binary);
        return rejected(Terminal_worker_envelope_error::MALFORMED_PAYLOAD);
    }
    std::uint32_t version = 0U;
    if (!reader.read_u32(version)) {
        clear_bytes(*binary);
        return rejected(Terminal_worker_envelope_error::MALFORMED_PAYLOAD);
    }
    if (version != k_terminal_worker_envelope_schema_version) {
        clear_bytes(*binary);
        return rejected(Terminal_worker_envelope_error::UNSUPPORTED_SCHEMA);
    }

    Terminal_worker_envelope envelope;
    Sensitive_envelope_guard envelope_guard(envelope);
    std::uint8_t platform = 0U;
    if (!reader.read_u8(platform) || platform > 1U ||
        !reader.read_string(
            envelope.provider_namespace,
            k_maximum_provider_namespace_bytes) ||
        !reader.read_bytes(
            envelope.serialized_request,
            Terminal_launch_request_limits::maximum_payload_bytes))
    {
        clear_bytes(*binary);
        return rejected(Terminal_worker_envelope_error::MALFORMED_PAYLOAD);
    }
    envelope.platform = platform == 0U
        ? Launch_platform::WINDOWS
        : Launch_platform::POSIX;

    std::uint32_t logical_width = 0U;
    std::uint32_t logical_height = 0U;
    std::uint32_t maximum_physical_width = 0U;
    std::uint32_t maximum_physical_height = 0U;
    std::uint32_t scale_factor = 0U;
    std::uint64_t font_size = 0U;
    std::uint8_t renderer = 0U;
    std::uint8_t subpixel = 0U;
    std::uint8_t timestamps = 0U;
    std::uint8_t scrollback_present = 0U;
    std::uint32_t scrollback_limit = 0U;
    std::uint8_t style = 0U;
    std::uint64_t scrollbar_width = 0U;
    std::uint8_t capture_present = 0U;
    if (!reader.read_u32(logical_width) ||
        !reader.read_u32(logical_height) ||
        !reader.read_u32(maximum_physical_width) ||
        !reader.read_u32(maximum_physical_height) ||
        !reader.read_u32(scale_factor) ||
        !reader.read_string(
            envelope.surface_configuration.settings.color_scheme,
            k_maximum_configuration_string_bytes) ||
        !reader.read_string(
            envelope.surface_configuration.settings.font_family,
            k_maximum_configuration_string_bytes) ||
        !reader.read_u64(font_size) || !reader.read_u8(renderer) ||
        !reader.read_u8(subpixel) || !reader.read_u8(timestamps) ||
        timestamps > 1U || !reader.read_u8(scrollback_present) ||
        scrollback_present > 1U || !reader.read_u32(scrollback_limit) ||
        !reader.read_string(
            envelope.surface_configuration.title,
            k_maximum_configuration_string_bytes) ||
        !reader.read_u8(style) || !reader.read_u64(scrollbar_width) ||
        !reader.read_u8(capture_present) || capture_present > 1U)
    {
        clear_bytes(*binary);
        return rejected(Terminal_worker_envelope_error::MALFORMED_PAYLOAD);
    }
    Terminal_worker_surface_configuration& surface =
        envelope.surface_configuration;
    surface.logical_width = static_cast<std::int32_t>(logical_width);
    surface.logical_height = static_cast<std::int32_t>(logical_height);
    surface.maximum_physical_width =
        static_cast<std::int32_t>(maximum_physical_width);
    surface.maximum_physical_height =
        static_cast<std::int32_t>(maximum_physical_height);
    surface.scale_factor = std::bit_cast<float>(scale_factor);
    surface.settings.font_size = std::bit_cast<double>(font_size);
    surface.settings.text_renderer_mode =
        static_cast<Terminal_worker_text_renderer_mode>(renderer);
    surface.settings.lcd_subpixel_order =
        static_cast<Terminal_worker_lcd_subpixel_order>(subpixel);
    surface.settings.row_timestamp_tooltip_enabled = timestamps != 0U;
    if (scrollback_present != 0U) {
        surface.settings.scrollback_limit =
            static_cast<std::int32_t>(scrollback_limit);
    }
    surface.style = static_cast<Terminal_worker_style>(style);
    surface.scrollbar_width = std::bit_cast<double>(scrollbar_width);
    if (capture_present != 0U) {
        Terminal_worker_output_capture_configuration capture;
        std::uint64_t maximum_bytes = 0U;
        if (!reader.read_string(
                capture.base_path,
                k_maximum_configuration_string_bytes) ||
            !reader.read_u64(maximum_bytes) ||
            maximum_bytes > std::numeric_limits<std::size_t>::max())
        {
            clear_bytes(*binary);
            return rejected(Terminal_worker_envelope_error::MALFORMED_PAYLOAD);
        }
        capture.maximum_bytes = static_cast<std::size_t>(maximum_bytes);
        envelope.output_capture = std::move(capture);
    }

    std::uint8_t environment_present = 0U;
    std::uint32_t environment_count = 0U;
    if (!reader.read_u8(environment_present) || environment_present > 1U ||
        !reader.read_u32(environment_count) ||
        environment_count >
            Terminal_launch_request_limits::maximum_environment_entries ||
        (environment_present == 0U && environment_count != 0U))
    {
        clear_bytes(*binary);
        return rejected(Terminal_worker_envelope_error::MALFORMED_PAYLOAD);
    }
    if (environment_present != 0U) {
        envelope.authorized_environment.emplace();
        envelope.authorized_environment->reserve(environment_count);
        for (std::uint32_t index = 0U; index < environment_count; ++index) {
            environment_policy::Environment_entry entry;
            if (!reader.read_string(
                    entry.name,
                    Terminal_launch_request_limits::maximum_environment_bytes) ||
                !reader.read_string(
                    entry.value,
                    Terminal_launch_request_limits::maximum_environment_bytes))
            {
                clear_bytes(*binary);
                return rejected(
                    Terminal_worker_envelope_error::MALFORMED_PAYLOAD);
            }
            envelope.authorized_environment->push_back(std::move(entry));
        }
    }
    if (!reader.at_end()) {
        clear_bytes(*binary);
        return rejected(Terminal_worker_envelope_error::TRAILING_DATA);
    }
    clear_bytes(*binary);

    const Terminal_worker_envelope_error validation =
        validate_envelope(envelope);
    if (validation != Terminal_worker_envelope_error::NONE) {
        return rejected(validation);
    }
    Terminal_worker_envelope_result result;
    result.error = Terminal_worker_envelope_error::NONE;
    result.envelope = std::move(envelope);
    return result;
}

} // namespace vnm::terminal_workspace
