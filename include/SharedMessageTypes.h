#pragma once

#include <stdint.h>

// Keep this file synchronized with bally_software/include/SharedMessageTypes.h.
enum class logType : uint8_t {
    NONE = 0,
    INFO,
    CMDO,
    TELE,
    ERRO,
    DEBG,
    PAKG // complete packet message -> the message is more biger than 250 bytes
};

// Packet metadata packed in one byte:
// bit 7     : last packet flag
// bits 0..6 : packet index (0..127)
static constexpr uint8_t MESSAGE_PACKET_LAST_FLAG = 0x80;
static constexpr uint8_t MESSAGE_PACKET_INDEX_MASK = 0x7F;
static constexpr uint8_t MESSAGE_PACKET_MAX_INDEX = 127;

inline constexpr uint8_t makePacketInfo(uint8_t packetIndex, bool isLastPacket) {
    return (packetIndex & MESSAGE_PACKET_INDEX_MASK) |
           (isLastPacket ? MESSAGE_PACKET_LAST_FLAG : 0);
}

inline constexpr uint8_t getPacketIndex(uint8_t packetInfo) {
    return packetInfo & MESSAGE_PACKET_INDEX_MASK;
}

inline constexpr bool isLastPacket(uint8_t packetInfo) {
    return (packetInfo & MESSAGE_PACKET_LAST_FLAG) != 0;
}
