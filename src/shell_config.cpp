#include "shell_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace {

using std::string;

ShellConfig::Context g_ctx = {nullptr, nullptr, nullptr};

string trimCopy(const string& text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });

    if (first == text.end()) {
        return "";
    }

    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();

    return string(first, last);
}

string stripOuterQuotes(const string& text) {
    string s = trimCopy(text);
    if (s.length() >= 2) {
        const bool doubleQuoted = (s.front() == '"' && s.back() == '"');
        const bool singleQuoted = (s.front() == '\'' && s.back() == '\'');
        if (doubleQuoted || singleQuoted) {
            s = s.substr(1, s.length() - 2);
        }
    }
    return s;
}

bool parseMacAddress(const string& text, uint8_t outMac[6]) {
    unsigned int bytes[6] = {0, 0, 0, 0, 0, 0};
    const string clean = stripOuterQuotes(text);

    if (std::sscanf(
            clean.c_str(),
            "%02x:%02x:%02x:%02x:%02x:%02x",
            &bytes[0], &bytes[1], &bytes[2],
            &bytes[3], &bytes[4], &bytes[5]
        ) != 6) {
        return false;
    }

    for (size_t i = 0; i < 6; ++i) {
        outMac[i] = static_cast<uint8_t>(bytes[i]);
    }
    return true;
}

void printLine(const string& text) {
    if (g_ctx.io != nullptr) {
        g_ctx.io->println(text.c_str());
    }
}

string normalizeCommand(const string& command) {
    const string input = trimCopy(command);
    const string prefix = "espnow -send_to ";

    if (input.rfind(prefix, 0) == 0) {
        const string args = trimCopy(input.substr(prefix.length()));
        if (!args.empty() && args.find(',') == string::npos) {
            return "espnow -send_all " + args;
        }
    }

    return input;
}

uint8_t wrapper_help_h() {
    if (g_ctx.shell == nullptr) {
        return RESULT_ERROR;
    }

    printLine(g_ctx.shell->get_help(""));
    return RESULT_OK;
}

uint8_t wrapper_help_l(string module = "") {
    if (g_ctx.shell == nullptr) {
        return RESULT_ERROR;
    }

    printLine(g_ctx.shell->get_help(module));
    return RESULT_OK;
}

uint8_t wrapper_help_e() {
    printLine("Uso: <module> -<command> [args]");
    printLine("Exemplo local: dongle -run \"status\"");
    printLine("Exemplo espnow unicast: espnow -send_to 1, \"dongle -run status\"");
    printLine("Exemplo espnow broadcast: espnow -send_to \"dongle -run status\"");
    return RESULT_OK;
}

uint8_t wrapper_dongle_run(string command) {
    const string localCommand = stripOuterQuotes(command);
    printLine("[dongle] comando local: " + localCommand);
    return RESULT_OK;
}

uint8_t wrapper_dongle_ping() {
    printLine("[dongle] pong");
    return RESULT_OK;
}

uint8_t wrapper_espnow_list() {
    if (g_ctx.espNow == nullptr) {
        return RESULT_ERROR;
    }

    const size_t total = g_ctx.espNow->deviceCount();
    if (total == 0) {
        printLine("[espnow] nenhum dispositivo cadastrado");
        return RESULT_OK;
    }

    for (size_t i = 0; i < total; ++i) {
        EspNowManager::deviceInfo item = {};
        if (!g_ctx.espNow->deviceAt(i, item)) {
            continue;
        }

        char macText[18] = {0};
        std::snprintf(
            macText,
            sizeof(macText),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            item.mac[0], item.mac[1], item.mac[2],
            item.mac[3], item.mac[4], item.mac[5]
        );

        char line[256] = {0};
        std::snprintf(
            line,
            sizeof(line),
            "[%u] %s - %s - %s",
            static_cast<unsigned>(i + 1),
            item.name,
            item.description,
            macText
        );
        printLine(line);
    }

    return RESULT_OK;
}

uint8_t wrapper_espnow_add(string macText, string name, string description) {
    if (g_ctx.espNow == nullptr) {
        return RESULT_ERROR;
    }

    uint8_t mac[6] = {0};
    if (!parseMacAddress(macText, mac)) {
        printLine("[espnow] MAC invalido. Use formato AA:BB:CC:DD:EE:FF");
        return RESULT_ERROR;
    }

    const bool ok = g_ctx.espNow->addDevice(
        mac,
        stripOuterQuotes(name).c_str(),
        stripOuterQuotes(description).c_str()
    );

    if (!ok) {
        printLine("[espnow] falha ao adicionar dispositivo");
        return RESULT_ERROR;
    }

    printLine("[espnow] dispositivo adicionado");
    return RESULT_OK;
}

uint8_t wrapper_espnow_remove(int32_t deviceNumber) {
    if (g_ctx.espNow == nullptr || deviceNumber <= 0) {
        return RESULT_ERROR;
    }

    const size_t index = static_cast<size_t>(deviceNumber - 1);
    const bool ok = g_ctx.espNow->removeDeviceByIndex(index);
    if (!ok) {
        printLine("[espnow] indice invalido");
        return RESULT_ERROR;
    }

    printLine("[espnow] dispositivo removido");
    return RESULT_OK;
}

uint8_t wrapper_espnow_remove_mac(string macText) {
    if (g_ctx.espNow == nullptr) {
        return RESULT_ERROR;
    }

    uint8_t mac[6] = {0};
    if (!parseMacAddress(macText, mac)) {
        printLine("[espnow] MAC invalido. Use formato AA:BB:CC:DD:EE:FF");
        return RESULT_ERROR;
    }

    const bool ok = g_ctx.espNow->removeDeviceByMac(mac);
    if (!ok) {
        printLine("[espnow] MAC nao encontrado");
        return RESULT_ERROR;
    }

    printLine("[espnow] dispositivo removido por MAC");
    return RESULT_OK;
}

uint8_t wrapper_espnow_send_to(int32_t deviceNumber, string command) {
    if (g_ctx.espNow == nullptr || deviceNumber <= 0) {
        return RESULT_ERROR;
    }

    const size_t index = static_cast<size_t>(deviceNumber - 1);

    EspNowManager::message outgoing = {};
    outgoing.timer = millis();
    outgoing.type = EspNowManager::logType::INFO;

    const string msg = stripOuterQuotes(command);
    std::strncpy(outgoing.msg, msg.c_str(), sizeof(outgoing.msg) - 1);
    outgoing.msg[sizeof(outgoing.msg) - 1] = '\0';

    const bool ok = g_ctx.espNow->sendToDevice(index, outgoing);
    if (!ok) {
        printLine("[espnow] falha ao enviar para dispositivo especifico");
        return RESULT_ERROR;
    }

    printLine("[espnow] mensagem enviada para dispositivo especifico");
    return RESULT_OK;
}

uint8_t wrapper_espnow_send_all(string command) {
    if (g_ctx.espNow == nullptr) {
        return RESULT_ERROR;
    }

    EspNowManager::message outgoing = {};
    outgoing.timer = millis();
    outgoing.type = EspNowManager::logType::INFO;

    const string msg = stripOuterQuotes(command);
    std::strncpy(outgoing.msg, msg.c_str(), sizeof(outgoing.msg) - 1);
    outgoing.msg[sizeof(outgoing.msg) - 1] = '\0';

    const bool ok = g_ctx.espNow->sendToAll(outgoing);
    if (!ok) {
        printLine("[espnow] falha ao enviar em broadcast");
        return RESULT_ERROR;
    }

    printLine("[espnow] mensagem enviada para todos os dispositivos");
    return RESULT_OK;
}

} // namespace

namespace ShellConfig {

bool bind(const Context& context) {
    if (context.shell == nullptr || context.espNow == nullptr || context.io == nullptr) {
        return false;
    }

    g_ctx = context;
    return true;
}

uint8_t registerDefaultModules() {
    if (g_ctx.shell == nullptr) {
        return RESULT_ERROR;
    }

    g_ctx.shell->create_module("help", "ajuda e informacoes");
    g_ctx.shell->create_module("dongle", "comandos executados localmente nesta ESP");
    g_ctx.shell->create_module("espnow", "gerenciamento de peers e envio de mensagens esp-now");

    g_ctx.shell->add(wrapper_help_h, "h", "lista os modulos", "help");
    g_ctx.shell->add(wrapper_help_l, "l", "lista as funcoes de um modulo", "help");
    g_ctx.shell->add(wrapper_help_e, "e", "explica o uso dos comandos", "help");

    g_ctx.shell->add(wrapper_dongle_ping, "ping", "teste rapido local", "dongle");
    g_ctx.shell->add(wrapper_dongle_run, "run", "executa comando local (placeholder)", "dongle");

    g_ctx.shell->add(wrapper_espnow_list, "list", "lista dispositivos cadastrados", "espnow");
    g_ctx.shell->add(wrapper_espnow_add, "add", "adiciona peer: <mac>, <nome>, <descricao>", "espnow");
    g_ctx.shell->add(wrapper_espnow_remove, "remove", "remove peer por indice: <numero>", "espnow");
    g_ctx.shell->add(wrapper_espnow_remove_mac, "remove_mac", "remove peer por mac: <mac>", "espnow");
    g_ctx.shell->add(wrapper_espnow_send_to, "send_to", "envia para indice: <numero>, <comando>", "espnow");
    g_ctx.shell->add(wrapper_espnow_send_all, "send_all", "envia para todos: <comando>", "espnow");

    return RESULT_OK;
}

std::string runLine(const std::string& command) {
    if (g_ctx.shell == nullptr) {
        return "Shell nao configurada.";
    }

    return g_ctx.shell->run_line_command(normalizeCommand(command));
}

} // namespace ShellConfig
