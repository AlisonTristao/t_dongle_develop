#pragma once

#include <cstdint>

/**
 * @brief "espnow" module: peer management and message sending over ESP-NOW.
 */
namespace EspNowCommands {

/**
 * @brief Creates the "espnow" module and registers its commands.
 */
uint8_t registerAll();

} // namespace EspNowCommands
