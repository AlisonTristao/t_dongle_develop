#ifndef SHARED_MESSAGE_TYPES_H
#define SHARED_MESSAGE_TYPES_H

#include <stdint.h>

// sizes and limits for logger messages, ensuring they are manageable and do not exceed memory limits
// max espnow size = 250
// protocol overhead: 4 bytes for timestamp + 1 byte for type + 2 byte for packet number + 2 byte for total packets + 1 byte for checksum = 10 bytes
// + 4 byte para o length
// therefore, max message size is 250 - 10 = 240 bytes
#define MAX_PACKET_SIZE         250 // if we change the transport protocol, we can increase this value
#define PROTOCOL_OVERHEAD_SIZE  20
#define MAX_CONTENT_SIZE        ((MAX_PACKET_SIZE - PROTOCOL_OVERHEAD_SIZE) -1) // -1 to ensure we have space for the null terminator
#define MAX_PACKETS_IN_RAM      32   // limit for messages in memory - watch out for available ram limits
#define LOGGER_MUTEX_TIMEOUT_MS 100  // time in ms to wait for the logger to be available - used to avoid deleting messages during printing

// enum to define log types, categorizing sent messages
enum class logType : uint8_t {
    NONE = 0,       // default type, should not be used for actual log messages
    INFO,           // informational messages that indicate normal operation and important events
    WARN,           // warning messages that indicate potential issues or important notices that are not errors
    ERRO,           // error messages
    DEBG,           // debug messages
    CMDO,           // terminal commands received
};

// creates a struct union to convert the message text or sound data to a byte array, 
// this will be used to send the messages over the transport protocol
struct messageContent_t {
    size_t size; 
    union { 
        char    text[MAX_CONTENT_SIZE + 1]; // +1 for null terminator
        uint8_t byte[MAX_CONTENT_SIZE + 1];              
    }; 

    // contrutctor to initialize the size and clear the content
    messageContent_t() : size(0) {
        memset(byte, 0, MAX_CONTENT_SIZE + 1);
        // set the null terminator for the text content to ensure it is always a valid string
        text[MAX_CONTENT_SIZE] = '\0';
    }
};

// struct to store logger messages, including timestamp, type, and content
// packet number and total packets are used for messages that need fragmentation due to protocol size limits
typedef struct {
    // message overhead for control and integrity verification
    uint32_t    timer;          // timestamp in ms to indicate when the message was created
    logType     type;           // message type, using the logtype enum to categorize the message
    uint16_t    packet_number;  // packet number for fragmented messages
    uint16_t    total_packets;  // total packets for fragmented messages
    uint8_t     checksum;       // simple checksum to verify message integrity

    // actual message content, limited to max_message_size to ensure the full packet stays within limits
    messageContent_t content;
} message;

inline constexpr const char* logTypeToString(logType type) {
    switch (type) {
        case logType::INFO:         return "INFO";
        case logType::CMDO:         return "CMDO";
        case logType::WARN:         return "WARN";
        case logType::ERRO:         return "ERRO";
        case logType::DEBG:         return "DEBG";
        case logType::NONE:         return "NONE";
        // capotamo o corsa
        default:                    return "UNKN"; 
    }
};

#endif // SHARED_MESSAGE_TYPES_H