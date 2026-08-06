#pragma once

#include <cstdint>

/**
 * @brief "help" module: list modules/commands and explain usage.
 */
namespace HelpCommands {

/**
 * @brief Creates the "help" module and registers its commands.
 */
uint8_t registerAll();

} // namespace HelpCommands
