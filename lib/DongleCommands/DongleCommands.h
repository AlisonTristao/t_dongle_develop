#pragma once

#include <cstdint>

/**
 * @brief "dongle" module: commands executed locally on this ESP
 * (clock, LED, LCD, SD card).
 */
namespace DongleCommands {

/**
 * @brief Creates the "dongle" module and registers its commands.
 */
uint8_t registerAll();

} // namespace DongleCommands
