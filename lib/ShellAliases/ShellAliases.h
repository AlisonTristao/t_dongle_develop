#pragma once

#include <cstddef>
#include <string>

/**
 * @brief Single place to declare short-word aliases for shell commands.
 *
 * An alias replaces the first token of a typed line with a full command
 * prefix, so "es 1, \"dongle -run status\"" runs as
 * "espnow -send_to 1, \"dongle -run status\"".
 *
 * To add a new alias, add one line to kAliases in ShellAliases.cpp.
 */
namespace ShellAliases {

struct Entry {
    const char* alias;
    const char* expandsTo;
};

/**
 * @brief Replaces the first token of the line with its expansion when it matches a known alias.
 * @param line Already-trimmed command line.
 * @return Line with the alias expanded, or the original line unchanged.
 */
std::string resolve(const std::string& line);

/**
 * @brief Registered alias table, for listing (used by help -e).
 */
const Entry* entries();
size_t count();

} // namespace ShellAliases
