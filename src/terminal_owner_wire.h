#pragma once

#include "vnm_terminal_workspace/terminal_owner_host.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vnm::terminal_workspace::detail {

inline constexpr std::uint32_t k_terminal_owner_wire_version = 4U;
inline constexpr std::uint32_t k_terminal_owner_maximum_frame_bytes =
    4U * 1024U * 1024U;

enum class Terminal_owner_wire_operation : std::uint32_t
{
    HANDSHAKE = 1U,
    NEW_LAUNCH = 2U,
    REQUEST_CLOSE = 3U,
    CUSTODY = 4U,
    CUSTODIES = 5U,
    ATTACH_EXISTING = 6U,
    FORWARD_INPUT = 7U,
    FORWARD_STATE = 8U,
    CONTAINS_SETTLEMENT = 9U,
    ACKNOWLEDGE_SETTLEMENT = 10U,
    ATOMIC_SNAPSHOT = 11U,
    SUBMIT_MESSAGE = 12U,
};

enum class Terminal_owner_wire_status : std::uint32_t
{
    OK = 0U,
    MALFORMED = 1U,
    UNAUTHORIZED = 2U,
    INCOMPATIBLE = 3U,
    FAILED = 4U,
};

class Terminal_owner_data_stream final : public QDataStream
{
public:
    Terminal_owner_data_stream(QByteArray& bytes, QIODeviceBase::OpenMode mode)
    :
        QDataStream(&bytes, mode)
    {
        setVersion(QDataStream::Qt_6_0);
        setByteOrder(QDataStream::BigEndian);
    }
};

inline Terminal_owner_data_stream make_terminal_owner_reader(QByteArray& bytes)
{
    return Terminal_owner_data_stream(bytes, QIODevice::ReadOnly);
}

inline Terminal_owner_data_stream make_terminal_owner_writer(QByteArray& bytes)
{
    return Terminal_owner_data_stream(bytes, QIODevice::WriteOnly);
}

void write_string(QDataStream& stream, const std::string& value);
bool read_string(QDataStream& stream, std::string& value);

void write_custody_snapshot(
    QDataStream& stream,
    const Terminal_owner_custody_snapshot& value);
bool read_custody_snapshot(
    QDataStream& stream,
    Terminal_owner_custody_snapshot& value);

void write_launch_result(
    QDataStream& stream,
    const Terminal_owner_launch_result& value);
bool read_launch_result(
    QDataStream& stream,
    Terminal_owner_launch_result& value);

void write_launch_configuration(
    QDataStream& stream,
    const Terminal_worker_surface_configuration& surface,
    const std::optional<Terminal_worker_output_capture_configuration>& capture,
    const std::string& canonical_product_configuration);
bool read_launch_configuration(
    QDataStream& stream,
    Terminal_worker_surface_configuration& surface,
    std::optional<Terminal_worker_output_capture_configuration>& capture,
    std::string& canonical_product_configuration);

QByteArray frame_terminal_owner_message(const QByteArray& payload);
bool take_terminal_owner_frame(QByteArray& buffer, QByteArray& payload);

} // namespace vnm::terminal_workspace::detail
