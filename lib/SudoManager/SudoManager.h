#pragma once

#include <cstddef>
#include <string>

/**
 * @brief Tracks which shell "users" currently have elevated (sudo) permission,
 * analogous to Linux sudo: an identity is elevated after supplying the right
 * password, and that grant lives only in RAM -- it is always cleared when the
 * dongle reboots.
 *
 * A "user" is just a free-form identity string built by the caller: "serial"
 * for the local UART/USB console, "espnow:<MAC>" for one specific registered
 * peer, and so on for any future transport (e.g. "mqtt:<client_id>").
 * SudoManager does not know or care where an identity comes from -- it only
 * tracks elevation per identity string, so adding a new transport later just
 * means picking a new identity prefix for it.
 */
namespace SudoManager {

/**
 * @brief Checks password and, on match, elevates userId.
 * @return true when the password matched (and userId is now elevated).
 */
bool elevate(const std::string& userId, const std::string& password);

/**
 * @brief Revokes elevation for one identity ("sudo -k" equivalent).
 */
void revoke(const std::string& userId);

/**
 * @brief Whether userId currently has elevated permission.
 */
bool isElevated(const std::string& userId);

/**
 * @brief Number of identities currently elevated (for status/info output).
 */
size_t elevatedCount();

} // namespace SudoManager
