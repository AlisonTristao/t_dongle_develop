#include "HelpCommands.h"

#include "ShellAliases.h"
#include "ShellCommandSupport.h"
#include "error_codes.h"

#include <string>
#include <vector>

namespace {

using std::string;
using ShellCommandSupport::context;
using ShellCommandSupport::failWithCode;
using ShellCommandSupport::printLine;
using ShellCommandSupport::trimCopy;

uint8_t wrapper_help_h() {
    if (context().shell == nullptr) {
        return failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para help -h");
    }

    const std::vector<std::string> modules = context().shell->complete_line("", 64);
    if (modules.empty()) {
        printLine("[help] nenhum modulo registrado");
        return RESULT_OK;
    }

    printLine("[help] modulos disponiveis:");
    for (const auto& entry : modules) {
        const string name = trimCopy(entry);
        if (!name.empty()) {
            printLine("- " + name);
        }
    }
    return RESULT_OK;
}

uint8_t wrapper_help_l(string module = "") {
    if (context().shell == nullptr) {
        return failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para help -l");
    }

    const string moduleName = trimCopy(module);
    if (moduleName.empty()) {
        return wrapper_help_h();
    }

    bool moduleExists = false;
    const std::vector<std::string> moduleMatches = context().shell->complete_line(moduleName, 32);
    for (const auto& entry : moduleMatches) {
        if (trimCopy(entry) == moduleName) {
            moduleExists = true;
            break;
        }
    }

    if (!moduleExists) {
        printLine("[help] modulo nao encontrado");
        return RESULT_OK;
    }

    const string seed = moduleName + " -";
    const std::vector<std::string> commands = context().shell->complete_line(seed, 96);
    if (commands.empty()) {
        printLine("[help] modulo nao encontrado ou sem comandos");
        return RESULT_OK;
    }

    printLine("[help] comandos do modulo " + moduleName + ":");
    for (const auto& entry : commands) {
        string text = trimCopy(entry);
        if (text.rfind(moduleName, 0) == 0) {
            text = trimCopy(text.substr(moduleName.length()));
        }
        if (!text.empty() && text[0] == '-') {
            text = trimCopy(text.substr(1));
        }
        if (!text.empty()) {
            printLine("- " + text);
        }
    }
    return RESULT_OK;
}

uint8_t wrapper_help_e() {
    printLine("[help] comandos gerais");
    printLine("Uso: <module> -<command> [args]");
    printLine("Horario RTC local: dongle -clock");
    printLine("Ajustar RTC local: dongle -set_clock \"YYYY-MM-DD HH:MM:SS\"");
    printLine("Exemplo local LCD: dongle -lcd \"Ola dongle\"");
    printLine("Se LCD nao aparecer: dongle -lcd_bl 1 e depois dongle -lcd_reinit");
    printLine("Se LCD estiver invertido: dongle -lcd_rot 0..3");
    printLine("Exemplo local LED: dongle -led 255, 0, 0");
    printLine("Exemplo espnow unicast: espnow -send_to 1, \"dongle -clock\"");
    printLine("Exemplo espnow para todos: espnow -send_all \"dongle -clock\"");
    printLine("send_to/send_all so alcancam peer ja cadastrado do qual ja recebemos alguma mensagem (BTP precisa do boot_id dele)");
    printLine("Executam o comando remotamente e respondem com a saida (aparece aqui como [cmd_result])");
    printLine("Editar peer: espnow -update 1, \"nome novo\", \"descricao nova\"");
    printLine("Encadear comandos: dongle -ping; dongle -clock");
    printLine("Permissao (sudo): sudo -login <senha> eleva o usuario atual (serial, ou o peer ESP-NOW que mandou o comando)");
    printLine("Elevacao fica so na RAM e sempre reseta quando o dongle liga; sudo -logout revoga, sudo -status consulta");
    printLine("Comandos destrutivos exigem sudo -login antes: dongle -sd_wipe, database -rebuild, database -drop <tabela>, database -clear_logs");
    printLine("Cartao SD: dongle -sd_ls <path> | -sd_cat <arquivo> | -sd_write/-sd_append <arquivo>, <texto> | -sd_rm <arquivo> | -sd_mkdir <path>");
    printLine("Script (1 comando por linha no SD): dongle -run_script <arquivo>");
    printLine("Historico rapido de comandos: dongle -history 20");
    printLine("Sistema: dongle -info | dongle -reboot");
    printLine("Sessao BTP v1 nesta porta: dongle -btp_v1 (ou o cliente manda BTP/1 ENTER <16 hex> direto); volta ao console com SESSION_CLOSE ou por timeout");
    printLine("Banco sqlite no SD: database -status | database -tables | database -read peers, 20");
    printLine("Logs comando+saida: database -logs 20");
    printLine("Historico ESP-NOW RX/TX: database -espnow_history 30");
    printLine("Manutencao do banco: database -count <tabela> | -delete <tabela>, <condicao> | -vacuum | -export <tabela> | -clear_logs (requer sudo)");

    if (ShellAliases::count() > 0) {
        printLine("Alias de comando (primeira palavra digitada -> prefixo real):");
        for (size_t i = 0; i < ShellAliases::count(); ++i) {
            const ShellAliases::Entry& entry = ShellAliases::entries()[i];
            printLine(string("  ") + entry.alias + " = " + entry.expandsTo);
        }
    }

    return RESULT_OK;
}

} // namespace

namespace HelpCommands {

uint8_t registerAll() {
    if (context().shell == nullptr) {
        return failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para registrar modulo help");
    }

    context().shell->create_module("help", "help and information");

    context().shell->add(wrapper_help_h, "h", "list modules", "help");
    context().shell->add(wrapper_help_l, "l", "list functions in a module", "help");
    context().shell->add(wrapper_help_e, "e", "explain command usage", "help");

    return RESULT_OK;
}

} // namespace HelpCommands
