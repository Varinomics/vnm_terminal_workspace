#include "terminal_owner_wire.h"

#include <QBuffer>

#include <limits>

namespace vnm::terminal_workspace::detail {
namespace {

constexpr qsizetype k_maximum_string_bytes = 1024 * 1024;

void clear_bytes(QByteArray& bytes)
{
    volatile char* data = bytes.data();
    for (qsizetype index = 0; index < bytes.size(); ++index) {
        data[index] = '\0';
    }
    bytes.clear();
}

template<typename Enum>
void write_enum(QDataStream& stream, Enum value)
{
    stream << static_cast<quint32>(value);
}

template<typename Enum>
bool read_enum(QDataStream& stream, Enum& value)
{
    quint32 encoded = 0U;
    stream >> encoded;
    value = static_cast<Enum>(encoded);
    return stream.status() == QDataStream::Ok;
}

void write_child_fact(QDataStream& stream, const Terminal_child_fact& value)
{
    write_string(stream, value.session_identity);
    stream
        << static_cast<quint64>(value.generation)
        << static_cast<quint64>(value.fact_key)
        << static_cast<quint64>(value.sequence);
    write_enum(stream, value.kind);
    write_enum(stream, value.error);
    stream << value.native_dispatch_occurred;
    stream << value.exit_code.has_value();
    if (value.exit_code) {
        stream << static_cast<qint32>(*value.exit_code);
    }
}

bool read_child_fact(QDataStream& stream, Terminal_child_fact& value)
{
    quint64 generation = 0U;
    quint64 fact_key = 0U;
    quint64 sequence = 0U;
    bool has_exit_code = false;
    qint32 exit_code = 0;
    if (!read_string(stream, value.session_identity)) {
        return false;
    }
    stream >> generation >> fact_key >> sequence;
    if (!read_enum(stream, value.kind) ||
        !read_enum(stream, value.error))
    {
        return false;
    }
    stream >> value.native_dispatch_occurred >> has_exit_code;
    if (has_exit_code) {
        stream >> exit_code;
        value.exit_code = static_cast<int>(exit_code);
    }
    else {
        value.exit_code.reset();
    }
    value.generation = generation;
    value.fact_key = fact_key;
    value.sequence = sequence;
    return stream.status() == QDataStream::Ok;
}

} // namespace

void write_string(QDataStream& stream, const std::string& value)
{
    stream << QByteArray(value.data(), static_cast<qsizetype>(value.size()));
}

bool read_string(QDataStream& stream, std::string& value)
{
    QByteArray bytes;
    stream >> bytes;
    if (stream.status() != QDataStream::Ok ||
        bytes.size() > k_maximum_string_bytes)
    {
        clear_bytes(bytes);
        return false;
    }
    value.assign(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    clear_bytes(bytes);
    return true;
}

void write_custody_snapshot(
    QDataStream& stream,
    const Terminal_owner_custody_snapshot& value)
{
    write_string(stream, value.session_identity);
    write_string(stream, value.launch_request_identity);
    stream << static_cast<quint64>(value.generation);
    write_enum(stream, value.state);
    stream << value.first_close_cause.has_value();
    if (value.first_close_cause) {
        write_enum(stream, *value.first_close_cause);
    }
    stream << value.framework_ready;
    stream << value.current_child_fact.has_value();
    if (value.current_child_fact) {
        write_child_fact(stream, *value.current_child_fact);
    }
    stream
        << static_cast<quint64>(value.attachment.revision)
        << value.attachment.live
        << static_cast<quint64>(value.attachment.producer_process_id);
    write_string(stream, value.attachment.framebuffer_path);
    stream << static_cast<quint64>(value.attachment.store_generation);
}

bool read_custody_snapshot(
    QDataStream& stream,
    Terminal_owner_custody_snapshot& value)
{
    quint64 generation = 0U;
    bool has_close_cause = false;
    bool has_child_fact = false;
    quint64 attachment_revision = 0U;
    quint64 attachment_process = 0U;
    quint64 attachment_store_generation = 0U;
    if (!read_string(stream, value.session_identity) ||
        !read_string(stream, value.launch_request_identity))
    {
        return false;
    }
    stream >> generation;
    if (!read_enum(stream, value.state)) {
        return false;
    }
    stream >> has_close_cause;
    if (has_close_cause) {
        Terminal_owner_close_cause cause{};
        if (!read_enum(stream, cause)) {
            return false;
        }
        value.first_close_cause = cause;
    }
    else {
        value.first_close_cause.reset();
    }
    stream >> value.framework_ready >> has_child_fact;
    if (has_child_fact) {
        Terminal_child_fact fact;
        if (!read_child_fact(stream, fact)) {
            return false;
        }
        value.current_child_fact = std::move(fact);
    }
    else {
        value.current_child_fact.reset();
    }
    stream
        >> attachment_revision
        >> value.attachment.live
        >> attachment_process;
    if (!read_string(stream, value.attachment.framebuffer_path)) {
        return false;
    }
    stream >> attachment_store_generation;
    value.generation = generation;
    value.attachment.revision = attachment_revision;
    value.attachment.producer_process_id = attachment_process;
    value.attachment.store_generation = attachment_store_generation;
    return stream.status() == QDataStream::Ok;
}

void write_launch_result(
    QDataStream& stream,
    const Terminal_owner_launch_result& value)
{
    write_enum(stream, value.outcome);
    write_string(stream, value.session_identity);
    stream << static_cast<quint64>(value.generation);
}

bool read_launch_result(
    QDataStream& stream,
    Terminal_owner_launch_result& value)
{
    quint64 generation = 0U;
    if (!read_enum(stream, value.outcome) ||
        !read_string(stream, value.session_identity))
    {
        return false;
    }
    stream >> generation;
    value.generation = generation;
    return stream.status() == QDataStream::Ok;
}

void write_launch_configuration(
    QDataStream& stream,
    const Terminal_worker_surface_configuration& surface,
    const std::optional<Terminal_worker_output_capture_configuration>& capture,
    const std::string& canonical_product_configuration)
{
    stream
        << static_cast<qint32>(surface.logical_width)
        << static_cast<qint32>(surface.logical_height)
        << static_cast<qint32>(surface.maximum_physical_width)
        << static_cast<qint32>(surface.maximum_physical_height)
        << surface.scale_factor;
    write_string(stream, surface.settings.color_scheme);
    write_string(stream, surface.settings.font_family);
    stream << surface.settings.font_size;
    write_enum(stream, surface.settings.text_renderer_mode);
    write_enum(stream, surface.settings.lcd_subpixel_order);
    stream << surface.settings.row_timestamp_tooltip_enabled;
    stream << surface.settings.scrollback_limit.has_value();
    if (surface.settings.scrollback_limit) {
        stream << static_cast<qint32>(*surface.settings.scrollback_limit);
    }
    write_string(stream, surface.title);
    write_enum(stream, surface.style);
    stream << surface.scrollbar_width;
    stream << capture.has_value();
    if (capture) {
        write_string(stream, capture->base_path);
        stream << static_cast<quint64>(capture->maximum_bytes);
    }
    write_string(stream, canonical_product_configuration);
}

bool read_launch_configuration(
    QDataStream& stream,
    Terminal_worker_surface_configuration& surface,
    std::optional<Terminal_worker_output_capture_configuration>& capture,
    std::string& canonical_product_configuration)
{
    qint32 width = 0;
    qint32 height = 0;
    qint32 maximum_width = 0;
    qint32 maximum_height = 0;
    quint32 renderer = 0U;
    quint32 subpixel = 0U;
    bool has_scrollback = false;
    qint32 scrollback = 0;
    quint32 style = 0U;
    bool has_capture = false;
    stream
        >> width >> height >> maximum_width >> maximum_height
        >> surface.scale_factor;
    if (!read_string(stream, surface.settings.color_scheme) ||
        !read_string(stream, surface.settings.font_family))
    {
        return false;
    }
    stream
        >> surface.settings.font_size
        >> renderer
        >> subpixel
        >> surface.settings.row_timestamp_tooltip_enabled
        >> has_scrollback;
    if (has_scrollback) {
        stream >> scrollback;
        surface.settings.scrollback_limit = static_cast<int>(scrollback);
    }
    else {
        surface.settings.scrollback_limit.reset();
    }
    if (!read_string(stream, surface.title)) {
        return false;
    }
    stream >> style >> surface.scrollbar_width >> has_capture;
    if (has_capture) {
        Terminal_worker_output_capture_configuration value;
        quint64 maximum_bytes = 0U;
        if (!read_string(stream, value.base_path)) {
            return false;
        }
        stream >> maximum_bytes;
        value.maximum_bytes = static_cast<std::size_t>(maximum_bytes);
        capture = std::move(value);
    }
    else {
        capture.reset();
    }
    if (!read_string(stream, canonical_product_configuration) ||
        renderer > static_cast<quint32>(
            Terminal_worker_text_renderer_mode::GLYPH) ||
        subpixel > static_cast<quint32>(
            Terminal_worker_lcd_subpixel_order::VBGR) ||
        style > static_cast<quint32>(Terminal_worker_style::DARK))
    {
        return false;
    }
    surface.logical_width = width;
    surface.logical_height = height;
    surface.maximum_physical_width = maximum_width;
    surface.maximum_physical_height = maximum_height;
    surface.settings.text_renderer_mode =
        static_cast<Terminal_worker_text_renderer_mode>(renderer);
    surface.settings.lcd_subpixel_order =
        static_cast<Terminal_worker_lcd_subpixel_order>(subpixel);
    surface.style = static_cast<Terminal_worker_style>(style);
    return stream.status() == QDataStream::Ok;
}

QByteArray frame_terminal_owner_message(const QByteArray& payload)
{
    QByteArray framed;
    auto stream = make_terminal_owner_writer(framed);
    stream << static_cast<quint32>(payload.size());
    framed.append(payload);
    return framed;
}

bool take_terminal_owner_frame(QByteArray& buffer, QByteArray& payload)
{
    if (buffer.size() < static_cast<qsizetype>(sizeof(quint32))) {
        return false;
    }
    QByteArray prefix = buffer.first(static_cast<qsizetype>(sizeof(quint32)));
    auto stream = make_terminal_owner_reader(prefix);
    quint32 size = 0U;
    stream >> size;
    if (stream.status() != QDataStream::Ok ||
        size > k_terminal_owner_maximum_frame_bytes)
    {
        clear_bytes(buffer);
        payload.clear();
        return true;
    }
    const qsizetype total = static_cast<qsizetype>(sizeof(quint32)) +
        static_cast<qsizetype>(size);
    if (buffer.size() < total) {
        return false;
    }
    payload = buffer.mid(static_cast<qsizetype>(sizeof(quint32)), size);
    volatile char* data = buffer.data();
    for (qsizetype index = 0; index < total; ++index) {
        data[index] = '\0';
    }
    buffer.remove(0, total);
    return true;
}

} // namespace vnm::terminal_workspace::detail
