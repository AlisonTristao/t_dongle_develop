#pragma once

#include "error_codes.h"

#include <Arduino.h>
#include <TinyShell.h>
#include <EspNowManager.h>
#include <DonglePeripherals.h>
#include <LcdTerminal.h>
#include <DatabaseStore.h>

#include <cstdint>
#include <ctime>
#include <string>

/**
 * @brief Shared runtime context and helpers used by every command module
 * (HelpCommands, DongleCommands, EspNowCommands, DatabaseCommands) that
 * registers wrappers into ShellConfig.
 *
 * This keeps one standard way to reach peripherals/espnow/database/io,
 * print output, and report errors, instead of each module inventing its own.
 * Mirrors ShellConfig::Context field-for-field; kept separate (instead of
 * shared) so command modules never need to include ShellConfig.h themselves.
 */
namespace ShellCommandSupport {

struct Context {
    TinyShell* shell;
    EspNowManager* espNow;
    DonglePeripherals* peripherals;
    LcdTerminal* lcdTerminal;
    DatabaseStore* database;
    Stream* io;
};

/**
 * @brief Stores the runtime context. Called once from ShellConfig::bind.
 */
void setContext(const Context& context);

/**
 * @brief Returns the currently bound runtime context.
 */
const Context& context();

/**
 * @brief Clears the per-command output buffers. Called at the start/end of ShellConfig::runLine.
 */
void resetBuffers();

/**
 * @brief Appends text coming from TinyShell's own output callback.
 */
void appendShellResponse(const std::string& text);

/**
 * @brief Text printed via printLine() during the current command.
 */
const std::string& commandOutputBuffer();

/**
 * @brief Text collected via appendShellResponse() during the current command.
 */
const std::string& shellResponseBuffer();

std::string trimCopy(const std::string& text);
std::string stripOuterQuotes(const std::string& text);
bool parseMacAddress(const std::string& text, uint8_t outMac[6]);
bool parseDateTimeText(const std::string& text, time_t& outEpoch);
uint8_t clampByte(int32_t value);

/**
 * @brief Resolves the broadcast MAC used by espnow send_to/send_all peer "000":
 * the default stored in the database, falling back to FF:FF:FF:FF:FF:FF.
 */
void resolveDefaultBroadcastMac(uint8_t outMac[6]);

/**
 * @brief Prints one line to serial/LCD and records it in the command output buffer.
 */
void printLine(const std::string& text);

/**
 * @brief Prints "erro(<code>/<name>) <detail>" and returns RESULT_ERROR.
 */
uint8_t failWithCode(AppError::Code code, const std::string& detail);

/**
 * @brief Prints "aviso(<code>/<name>) <detail>" without changing the command result.
 */
void warnWithCode(AppError::Code code, const std::string& detail);

} // namespace ShellCommandSupport
