#pragma once

#include <cstdint>
#include <string>

/**
 * @brief "database" module: SQLite on SD (status, reading and maintenance).
 */
namespace DatabaseCommands {

/**
 * @brief Creates the "database" module and registers its commands.
 */
uint8_t registerAll();

/**
 * @brief Executes raw SQL and logs it in command_log. Also reachable as "database -exec".
 *
 * Exposed directly because ShellConfig::runLine bypasses the TinyShell tokenizer
 * for this command, so SQL can contain commas/quotes safely.
 */
uint8_t exec(std::string sql);

/**
 * @brief Same as exec(), but without persisting the command to command_log.
 * Also reachable as "database -exec_nolog".
 */
uint8_t execNoLog(std::string sql);

} // namespace DatabaseCommands
