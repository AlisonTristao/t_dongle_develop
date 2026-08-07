#pragma once

#include <Arduino.h>
#include <TinyShell.h>
#include <EspNowManager.h>
#include <DonglePeripherals.h>
#include <LcdDashboard.h>
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
	LcdDashboard* lcdDashboard;
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
 * - sudo     -> SudoCommands
 *
 * @return RESULT_OK on success.
 */
uint8_t registerDefaultModules();

/**
 * @brief Runs one or more shell command lines.
 *
 * Special handling:
 * - Multiple commands separated by ';' (outside quotes) run in sequence,
 *   each with its own alias/persistence handling, e.g. "dongle -ping; dongle -clock".
 * - Command aliases are expanded first (see ShellAliases).
 * - "espnow -send_to <texto>" is mapped to broadcast send.
 * - "espnow -send_to <indice>, <texto>" sends to one device.
 * - A command identical to the immediately previous one still runs, but is
 *   not written to command_log again (avoids flooding the SD history when
 *   the same line is resent back-to-back).
 *
 * @param command Input line formatted as <module> -<command> [args].
 * @param source Tag stored in command_log ("serial", "espnow", ...).
 * @param outFullText When non-null, receives the full text a wrapper printed
 * (via printLine) plus any TinyShell response for this command — the same
 * text persisted to command_log. Used by callers that need to relay a
 * command's real output (e.g. replying to a remote ESP-NOW command), since
 * the returned std::string is usually empty (most wrappers write directly to
 * serial/LCD instead of returning text, to avoid double-printing here).
 * @param userId Identity running this command, e.g. "serial" or
 * "espnow:AA:BB:CC:DD:EE:FF" (one identity per registered peer). Read by
 * SudoManager-gated commands via ShellCommandSupport::currentUserId(). When
 * empty (the default), falls back to source.
 * @return TinyShell response text (rarely non-empty; see outFullText).
 */
std::string runLine(const std::string& command, const std::string& source = "serial", std::string* outFullText = nullptr, const std::string& userId = "");

} // namespace ShellConfig

