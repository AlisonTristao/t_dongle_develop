#pragma once

#include <cstdint>

/**
 * @brief "sudo" module: password-based permission elevation, analogous to
 * Linux sudo. See SudoManager for the identity/elevation model.
 */
namespace SudoCommands {

/**
 * @brief Creates the "sudo" module and registers its commands.
 */
uint8_t registerAll();

} // namespace SudoCommands
