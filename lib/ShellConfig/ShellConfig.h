#pragma once

#include <Arduino.h>
#include <TinyShell.h>
#include <EspNowManager.h>
#include <DonglePeripherals.h>
#include <LcdTerminal.h>
#include <DatabaseStore.h>

#include <string>

namespace ShellConfig {

/**
 * @brief Runtime dependencies used by shell wrappers.
 */
struct Context {
	TinyShell* shell;
	EspNowManager* espNow;
	DonglePeripherals* peripherals;
	LcdTerminal* lcdTerminal;
	DatabaseStore* database;
	Stream* io;
};

/**
 * @brief Binds TinyShell and runtime services used by wrappers.
 * @param context Runtime context with valid pointers.
 * @return true when all required pointers are valid.
 */
bool bind(const Context& context);

/**
 * @brief Creates and registers default modules and commands.
 *
 * Each module owns its own registration in its own library:
 * - help     -> HelpCommands
 * - dongle   -> DongleCommands
 * - espnow   -> EspNowCommands
 * - database -> DatabaseCommands
 *
 * @return RESULT_OK on success.
 */
uint8_t registerDefaultModules();

/**
 * @brief Runs one shell command line.
 *
 * Special handling:
 * - Command aliases are expanded first (see ShellAliases).
 * - "espnow -send_to <texto>" is mapped to broadcast send.
 * - "espnow -send_to <indice>, <texto>" sends to one device.
 *
 * @param command Input line formatted as <module> -<command> [args].
 * @return TinyShell response text.
 */
std::string runLine(const std::string& command);

} // namespace ShellConfig

