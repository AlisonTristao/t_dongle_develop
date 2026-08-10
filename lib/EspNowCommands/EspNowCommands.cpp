#include "EspNowCommands.h"

#include "ShellCommandSupport.h"
#include "BtpTransport.h"
#include "error_codes.h"

#include <cstdio>
#include <cstring>

namespace {

using std::string;
using ShellCommandSupport::context;
using ShellCommandSupport::failWithCode;
using ShellCommandSupport::parseMacAddress;
using ShellCommandSupport::printLine;
using ShellCommandSupport::stripOuterQuotes;
using ShellCommandSupport::warnWithCode;

uint8_t wrapper_espnow_list() {
    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando list");
    }

    const size_t total = context().espNow->deviceCount();
    if (total == 0) {
        printLine("[espnow] nenhum peer real cadastrado");
        return RESULT_OK;
    }

    for (size_t i = 0; i < total; ++i) {
        EspNowManager::deviceInfo item = {};
        if (!context().espNow->deviceAt(i, item)) {
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

        uint32_t knownSourceId = 0;
        uint32_t knownBootId = 0;
        const bool bootKnown = BtpTransport::lookupPeer(item.mac, &knownSourceId, &knownBootId);

        char line[256] = {0};
        std::snprintf(
            line,
            sizeof(line),
            "[%03u] %s - %s - %s - boot_id=%s",
            static_cast<unsigned>(i + 1),
            item.name,
            item.description,
            macText,
            bootKnown ? "conhecido" : "desconhecido (send_to vai falhar ate chegar algo dele)"
        );
        printLine(line);
    }

    return RESULT_OK;
}

uint8_t wrapper_espnow_add(string macText, string name, string description) {
    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando add");
    }

    uint8_t mac[6] = {0};
    if (!parseMacAddress(macText, mac)) {
        return failWithCode(AppError::Code::INVALID_MAC_FORMAT, "MAC invalido. Use formato AA:BB:CC:DD:EE:FF");
    }

    const bool ok = context().espNow->addDevice(
        mac,
        stripOuterQuotes(name).c_str(),
        stripOuterQuotes(description).c_str()
    );

    if (!ok) {
        return failWithCode(AppError::Code::PEER_ADD_FAILED, "falha ao adicionar dispositivo");
    }

    if (context().database != nullptr && context().database->isReady()) {
        if (!context().database->upsertPeer(mac, stripOuterQuotes(name).c_str(), stripOuterQuotes(description).c_str())) {
            warnWithCode(AppError::Code::DATABASE_PEER_NOT_PERSISTED, "peer adicionado, mas nao persistido");
        }
    }

    printLine("[espnow] dispositivo adicionado");
    return RESULT_OK;
}

uint8_t wrapper_espnow_remove(int32_t deviceNumber) {
    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando remove");
    }

    if (deviceNumber <= 0) {
        return failWithCode(AppError::Code::INVALID_DEVICE_INDEX, "indice invalido. Use valores >= 1");
    }

    const size_t index = static_cast<size_t>(deviceNumber - 1);
    EspNowManager::deviceInfo removed = {};
    const bool hadDevice = context().espNow->deviceAt(index, removed);

    const bool ok = context().espNow->removeDeviceByIndex(index);
    if (!ok) {
        return failWithCode(AppError::Code::PEER_REMOVE_FAILED, "indice invalido");
    }

    if (hadDevice && context().database != nullptr && context().database->isReady()) {
        if (!context().database->removePeer(removed.mac)) {
            warnWithCode(AppError::Code::DATABASE_PEER_REMOVE_NOT_PERSISTED, "peer removido, mas persistencia nao atualizada");
        }
    }

    printLine("[espnow] dispositivo removido");
    return RESULT_OK;
}

uint8_t wrapper_espnow_remove_mac(string macText) {
    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando remove_mac");
    }

    uint8_t mac[6] = {0};
    if (!parseMacAddress(macText, mac)) {
        return failWithCode(AppError::Code::INVALID_MAC_FORMAT, "MAC invalido. Use formato AA:BB:CC:DD:EE:FF");
    }

    const bool ok = context().espNow->removeDeviceByMac(mac);
    if (!ok) {
        return failWithCode(AppError::Code::PEER_NOT_FOUND, "MAC nao encontrado");
    }

    if (context().database != nullptr && context().database->isReady()) {
        if (!context().database->removePeer(mac)) {
            warnWithCode(AppError::Code::DATABASE_PEER_REMOVE_NOT_PERSISTED, "peer removido, mas persistencia nao atualizada");
        }
    }

    printLine("[espnow] dispositivo removido por MAC");
    return RESULT_OK;
}

uint8_t wrapper_espnow_update(int32_t deviceNumber, string name, string description) {
    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando update");
    }

    if (deviceNumber <= 0) {
        return failWithCode(AppError::Code::INVALID_DEVICE_INDEX, "indice invalido. Use valores >= 1");
    }

    const size_t index = static_cast<size_t>(deviceNumber - 1);
    const string safeName = stripOuterQuotes(name);
    const string safeDescription = stripOuterQuotes(description);

    const bool ok = context().espNow->updateDeviceByIndex(index, safeName.c_str(), safeDescription.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::PEER_UPDATE_FAILED, "indice invalido para update");
    }

    EspNowManager::deviceInfo updated = {};
    if (context().espNow->deviceAt(index, updated) && context().database != nullptr && context().database->isReady()) {
        if (!context().database->updatePeerMetadata(updated.mac, safeName.c_str(), safeDescription.c_str())) {
            warnWithCode(AppError::Code::DATABASE_PEER_UPDATE_NOT_PERSISTED, "peer atualizado em memoria, mas nao persistido");
        }
    }

    printLine("[espnow] peer atualizado");
    return RESULT_OK;
}

// Sends one BTP COMMAND_REQUEST (shell action) to a MAC whose boot_id we've
// already learned from received traffic (see BtpTransport::rememberPeer --
// there is no HELLO/MANIFEST handshake yet, topico 16). A peer we've never
// heard from cannot be addressed: unlike the old CMDO, a COMMAND_REQUEST
// needs a real target_boot_id, not just a MAC.
bool sendWithStatusViaManager(void* rawContext, const uint8_t mac[6], const uint8_t* data, size_t size,
                              bool* outDelivered, uint32_t timeoutMs) {
    return static_cast<EspNowManager*>(rawContext)->sendToMacWithStatus(mac, data, size, *outDelivered, timeoutMs);
}

bool sendShellCommandRequest(const uint8_t mac[6], const string& commandText, bool& outDelivered) {
    uint32_t targetSourceId = 0;
    uint32_t targetBootId = 0;
    if (!BtpTransport::lookupPeer(mac, &targetSourceId, &targetBootId)) {
        return false;
    }

    uint8_t payload[BtpTransport::btp_command::kRequestPrefixSize + BtpTransport::btp_command::kMaxShellCommandSize];
    const size_t textLen = (commandText.size() < BtpTransport::btp_command::kMaxShellCommandSize)
        ? commandText.size()
        : BtpTransport::btp_command::kMaxShellCommandSize;

    payload[0] = static_cast<uint8_t>(targetSourceId);
    payload[1] = static_cast<uint8_t>(targetSourceId >> 8);
    payload[2] = static_cast<uint8_t>(targetSourceId >> 16);
    payload[3] = static_cast<uint8_t>(targetSourceId >> 24);
    payload[4] = static_cast<uint8_t>(targetBootId);
    payload[5] = static_cast<uint8_t>(targetBootId >> 8);
    payload[6] = static_cast<uint8_t>(targetBootId >> 16);
    payload[7] = static_cast<uint8_t>(targetBootId >> 24);
    payload[8] = static_cast<uint8_t>(BtpTransport::btp_command::kShellActionId);
    payload[9] = static_cast<uint8_t>(BtpTransport::btp_command::kShellActionId >> 8);
    payload[10] = static_cast<uint8_t>(BtpTransport::btp_command::kShellActionVersion);
    payload[11] = static_cast<uint8_t>(BtpTransport::btp_command::kShellActionVersion >> 8);
    payload[12] = 0; payload[13] = 0; // flags, zero in v1
    payload[14] = 0; payload[15] = 0; // reserved
    payload[16] = static_cast<uint8_t>(textLen);
    payload[17] = static_cast<uint8_t>(textLen >> 8);
    payload[18] = static_cast<uint8_t>(textLen >> 16);
    payload[19] = static_cast<uint8_t>(textLen >> 24);
    std::memcpy(payload + BtpTransport::btp_command::kRequestPrefixSize, commandText.data(), textLen);

    const size_t payloadSize = BtpTransport::btp_command::kRequestPrefixSize + textLen;
    const uint64_t timestampUs = static_cast<uint64_t>(millis()) * 1000ULL;
    return BtpTransport::sendLogicalWithStatus(
        sendWithStatusViaManager, context().espNow, mac, btp::MessageType::Command,
        BtpTransport::btp_command::kCommandRequestObjectId,
        payload, payloadSize, timestampUs, outDelivered, 700);
}

uint8_t wrapper_espnow_send_to(int32_t deviceNumber, string command) {
    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando send_to");
    }

    if (deviceNumber <= 0) {
        return failWithCode(AppError::Code::INVALID_DEVICE_INDEX, "indice invalido. Use valores >= 1 (000/broadcast nao e mais enderecavel, ver help -e)");
    }

    const size_t index = static_cast<size_t>(deviceNumber - 1);
    EspNowManager::deviceInfo target = {};
    if (!context().espNow->deviceAt(index, target)) {
        return failWithCode(AppError::Code::INVALID_DEVICE_INDEX, "indice invalido");
    }

    const string msg = stripOuterQuotes(command);
    bool delivered = false;
    const bool gotStatus = sendShellCommandRequest(target.mac, msg, delivered);

    if (context().database != nullptr && context().database->isReady()) {
        context().database->logOutgoingEspNow(
            target.mac, btp::MessageType::Command,
            reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), gotStatus && delivered);
    }

    if (context().lcdDashboard != nullptr) {
        context().lcdDashboard->notifyTx(gotStatus && delivered);
    }

    if (!gotStatus) {
        return failWithCode(AppError::Code::PEER_BOOT_UNKNOWN,
                            "boot_id do peer ainda desconhecido (aguarde uma mensagem dele chegar)");
    }
    if (!delivered) {
        return failWithCode(AppError::Code::SEND_DELIVERY_FAILED, "status=false");
    }

    printLine("[espnow] status=true (resposta chega como cmd_result)");
    return RESULT_OK;
}

uint8_t wrapper_espnow_send_all(string command) {
    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando send_all");
    }

    const string msg = stripOuterQuotes(command);
    const size_t total = context().espNow->deviceCount();
    size_t attempted = 0;
    size_t delivered = 0;

    for (size_t i = 0; i < total; ++i) {
        EspNowManager::deviceInfo target = {};
        if (!context().espNow->deviceAt(i, target)) {
            continue;
        }

        bool oneDelivered = false;
        const bool gotStatus = sendShellCommandRequest(target.mac, msg, oneDelivered);
        if (!gotStatus) {
            continue; // boot_id ainda desconhecido para este peer
        }

        ++attempted;
        if (oneDelivered) {
            ++delivered;
        }

        if (context().database != nullptr && context().database->isReady()) {
            context().database->logOutgoingEspNow(
                target.mac, btp::MessageType::Command,
                reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), oneDelivered);
        }
    }

    if (attempted == 0) {
        return failWithCode(AppError::Code::PEER_BOOT_UNKNOWN,
                            "nenhum peer com boot_id conhecido (aguarde alguma mensagem chegar)");
    }

    char line[120] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[espnow] status=%s delivered=%u/%u (respostas chegam como cmd_result)",
        (delivered == attempted) ? "true" : "false",
        static_cast<unsigned>(delivered),
        static_cast<unsigned>(attempted)
    );
    printLine(line);

    if (context().lcdDashboard != nullptr) {
        context().lcdDashboard->notifyTx(delivered == attempted);
    }

    if (delivered != attempted) {
        return failWithCode(AppError::Code::SEND_PARTIAL_DELIVERY, "entrega parcial no envio para todos os peers");
    }

    return RESULT_OK;
}

} // namespace

namespace EspNowCommands {

uint8_t registerAll() {
    if (context().shell == nullptr) {
        return failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para registrar modulo espnow");
    }

    context().shell->create_module("espnow", "ESP-NOW peers management and message sending");

    context().shell->add(wrapper_espnow_list, "list", "list registered devices", "espnow");
    context().shell->add(wrapper_espnow_add, "add", "add peer: <mac>, <name>, <description>", "espnow");
    context().shell->add(wrapper_espnow_remove, "remove", "remove peer by index: <number>", "espnow");
    context().shell->add(wrapper_espnow_remove_mac, "remove_mac", "remove peer by MAC: <mac>", "espnow");
    context().shell->add(wrapper_espnow_update, "update", "update peer: <number>, <name>, <description>", "espnow");
    context().shell->add(wrapper_espnow_send_to, "send_to", "send to index: <number>, <command> (peer must have sent something first)", "espnow");
    context().shell->add(wrapper_espnow_send_all, "send_all", "send to all peers with known boot_id: <command>", "espnow");

    return RESULT_OK;
}

} // namespace EspNowCommands
