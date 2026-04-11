#pragma once

#include <stdint.h>

namespace AppError {

enum class Code : uint16_t {
    NONE = 0,

    // Core / shell
    SHELL_NOT_READY = 1001,
    INVALID_CONTEXT = 1002,
    INVALID_ARGUMENT = 1003,

    // Dongle / peripherals
    PERIPHERALS_NOT_READY = 1101,
    RTC_READ_FAILED = 1102,
    CLOCK_FORMAT_INVALID = 1103,
    RTC_SET_FAILED = 1104,
    LCD_NOT_READY = 1105,
    LCD_REINIT_FAILED = 1106,
    SD_INIT_FAILED = 1107,
    SD_NOT_READY = 1108,
    SD_WIPE_FAILED = 1109,
    SD_REINIT_FAILED = 1110,
    SD_DB_RECREATE_FAILED = 1111,

    // ESP-NOW
    ESPNOW_NOT_READY = 1201,
    INVALID_MAC_FORMAT = 1202,
    PEER_ADD_FAILED = 1203,
    PEER_REMOVE_FAILED = 1204,
    PEER_NOT_FOUND = 1205,
    PEER_UPDATE_FAILED = 1206,
    INVALID_DEVICE_INDEX = 1207,
    SEND_STATUS_TIMEOUT = 1208,
    SEND_DELIVERY_FAILED = 1209,
    BROADCAST_QUEUE_FAILED = 1210,
    SEND_PARTIAL_DELIVERY = 1211,

    // Database
    DATABASE_NOT_READY = 1301,
    DATABASE_INIT_FAILED = 1302,
    DATABASE_QUERY_FAILED = 1303,
    DATABASE_DROP_FAILED = 1304,
    DATABASE_REBUILD_FAILED = 1305,
    DATABASE_EXEC_FAILED = 1306,
    DATABASE_PEER_NOT_PERSISTED = 1307,
    DATABASE_PEER_REMOVE_NOT_PERSISTED = 1308,
    DATABASE_PEER_UPDATE_NOT_PERSISTED = 1309,
    DATABASE_COMMAND_LOG_FAILED = 1310,
    DATABASE_PEER_SYNC_FAILED = 1311,
};

inline uint16_t value(Code code) {
    return static_cast<uint16_t>(code);
}

inline const char* name(Code code) {
    switch (code) {
    case Code::NONE:
        return "NONE";
    case Code::SHELL_NOT_READY:
        return "SHELL_NOT_READY";
    case Code::INVALID_CONTEXT:
        return "INVALID_CONTEXT";
    case Code::INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case Code::PERIPHERALS_NOT_READY:
        return "PERIPHERALS_NOT_READY";
    case Code::RTC_READ_FAILED:
        return "RTC_READ_FAILED";
    case Code::CLOCK_FORMAT_INVALID:
        return "CLOCK_FORMAT_INVALID";
    case Code::RTC_SET_FAILED:
        return "RTC_SET_FAILED";
    case Code::LCD_NOT_READY:
        return "LCD_NOT_READY";
    case Code::LCD_REINIT_FAILED:
        return "LCD_REINIT_FAILED";
    case Code::SD_INIT_FAILED:
        return "SD_INIT_FAILED";
    case Code::SD_NOT_READY:
        return "SD_NOT_READY";
    case Code::SD_WIPE_FAILED:
        return "SD_WIPE_FAILED";
    case Code::SD_REINIT_FAILED:
        return "SD_REINIT_FAILED";
    case Code::SD_DB_RECREATE_FAILED:
        return "SD_DB_RECREATE_FAILED";
    case Code::ESPNOW_NOT_READY:
        return "ESPNOW_NOT_READY";
    case Code::INVALID_MAC_FORMAT:
        return "INVALID_MAC_FORMAT";
    case Code::PEER_ADD_FAILED:
        return "PEER_ADD_FAILED";
    case Code::PEER_REMOVE_FAILED:
        return "PEER_REMOVE_FAILED";
    case Code::PEER_NOT_FOUND:
        return "PEER_NOT_FOUND";
    case Code::PEER_UPDATE_FAILED:
        return "PEER_UPDATE_FAILED";
    case Code::INVALID_DEVICE_INDEX:
        return "INVALID_DEVICE_INDEX";
    case Code::SEND_STATUS_TIMEOUT:
        return "SEND_STATUS_TIMEOUT";
    case Code::SEND_DELIVERY_FAILED:
        return "SEND_DELIVERY_FAILED";
    case Code::BROADCAST_QUEUE_FAILED:
        return "BROADCAST_QUEUE_FAILED";
    case Code::SEND_PARTIAL_DELIVERY:
        return "SEND_PARTIAL_DELIVERY";
    case Code::DATABASE_NOT_READY:
        return "DATABASE_NOT_READY";
    case Code::DATABASE_INIT_FAILED:
        return "DATABASE_INIT_FAILED";
    case Code::DATABASE_QUERY_FAILED:
        return "DATABASE_QUERY_FAILED";
    case Code::DATABASE_DROP_FAILED:
        return "DATABASE_DROP_FAILED";
    case Code::DATABASE_REBUILD_FAILED:
        return "DATABASE_REBUILD_FAILED";
    case Code::DATABASE_EXEC_FAILED:
        return "DATABASE_EXEC_FAILED";
    case Code::DATABASE_PEER_NOT_PERSISTED:
        return "DATABASE_PEER_NOT_PERSISTED";
    case Code::DATABASE_PEER_REMOVE_NOT_PERSISTED:
        return "DATABASE_PEER_REMOVE_NOT_PERSISTED";
    case Code::DATABASE_PEER_UPDATE_NOT_PERSISTED:
        return "DATABASE_PEER_UPDATE_NOT_PERSISTED";
    case Code::DATABASE_COMMAND_LOG_FAILED:
        return "DATABASE_COMMAND_LOG_FAILED";
    case Code::DATABASE_PEER_SYNC_FAILED:
        return "DATABASE_PEER_SYNC_FAILED";
    default:
        return "UNKNOWN";
    }
}

} // namespace AppError
