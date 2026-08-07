#include "SudoCommands.h"

#include "ShellCommandSupport.h"
#include "SudoManager.h"
#include "error_codes.h"

#include <cstdio>

namespace {

using std::string;
using ShellCommandSupport::context;
using ShellCommandSupport::currentUserId;
using ShellCommandSupport::failWithCode;
using ShellCommandSupport::printLine;
using ShellCommandSupport::stripOuterQuotes;

uint8_t wrapper_sudo_login(string password = "") {
    const string userId = currentUserId();
    if (!SudoManager::elevate(userId, stripOuterQuotes(password))) {
        return failWithCode(AppError::Code::SUDO_WRONG_PASSWORD, "senha incorreta. Uso: sudo -login <senha>");
    }

    printLine("[sudo] acesso elevado concedido para '" + userId + "' ate o proximo reboot");
    return RESULT_OK;
}

uint8_t wrapper_sudo_logout() {
    const string userId = currentUserId();
    SudoManager::revoke(userId);
    printLine("[sudo] acesso elevado revogado para '" + userId + "'");
    return RESULT_OK;
}

uint8_t wrapper_sudo_status() {
    const string userId = currentUserId();
    const bool elevated = SudoManager::isElevated(userId);

    char line[128] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[sudo] usuario atual: %s (%s) | %u usuario(s) elevado(s) no total",
        userId.c_str(),
        elevated ? "elevado" : "sem privilegios",
        static_cast<unsigned>(SudoManager::elevatedCount())
    );
    printLine(line);
    return RESULT_OK;
}

} // namespace

namespace SudoCommands {

uint8_t registerAll() {
    if (context().shell == nullptr) {
        return failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para registrar modulo sudo");
    }

    context().shell->create_module("sudo", "password-based permission elevation for the current user");

    context().shell->add(wrapper_sudo_login, "login", "elevate current user: <senha>", "sudo");
    context().shell->add(wrapper_sudo_logout, "logout", "revoke elevation for current user", "sudo");
    context().shell->add(wrapper_sudo_status, "status", "show elevation status of current user", "sudo");

    return RESULT_OK;
}

} // namespace SudoCommands
