#include "EspNowCommands.h"

#include "ShellCommandSupport.h"
#include "error_codes.h"

#include <cstdio>
#include <cstring>

namespace {

using std::string;
using ShellCommandSupport::context;
using ShellCommandSupport::failWithCode;
using ShellCommandSupport::parseMacAddress;
using ShellCommandSupport::printLine;
using ShellCommandSupport::resolveDefaultBroadcastMac;
using ShellCommandSupport::stripOuterQuotes;
using ShellCommandSupport::warnWithCode;

uint8_t wrapper_espnow_list() {
    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando list");
    }

    printLine("[000] todos - FF:FF:FF:FF:FF:FF (alias broadcast)");

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

        char line[256] = {0};
        std::snprintf(
            line,
            sizeof(line),
            "[%03u] %s - %s - %s",
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

uint8_t wrapper_espnow_send_to(int32_t deviceNumber, string command) {
    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando send_to");
    }

    if (deviceNumber < 0) {
        return failWithCode(AppError::Code::INVALID_DEVICE_INDEX, "indice invalido. Use 000 ou valores >= 1");
    }

    EspNowManager::message outgoing = {};
    outgoing.timer = millis();
    // CMDO tells a registered receiving peer "run this as a shell command and
    // reply with the output" (see EspNowConfig::processRxMessageInternal).
    outgoing.type = EspNowManager::logType::CMDO;
    outgoing.packet_number = 0;
    outgoing.total_packets = 1;
    outgoing.checksum = 0;

    const string msg = stripOuterQuotes(command);
    const size_t maxLen = EspNowManager::MESSAGE_TEXT_SIZE;
    const size_t copyLen = (msg.size() < maxLen) ? msg.size() : maxLen;
    if (copyLen > 0) {
        std::memcpy(outgoing.content.text, msg.c_str(), copyLen);
    }
    outgoing.content.text[copyLen] = '\0';
    outgoing.content.size = copyLen;

    if (deviceNumber == 0) {
        // Peer virtual 000: route to default broadcast MAC (stored in DB, with FF fallback).
        uint8_t broadcastMac[6] = {0, 0, 0, 0, 0, 0};
        resolveDefaultBroadcastMac(broadcastMac);

        const bool queued = context().espNow->sendToMac(broadcastMac, outgoing);

        if (context().database != nullptr && context().database->isReady()) {
            context().database->logOutgoingEspNow(broadcastMac, outgoing, queued);
        }

        if (context().lcdDashboard != nullptr) {
            context().lcdDashboard->notifyTx(queued);
        }

        if (!queued) {
            return failWithCode(AppError::Code::BROADCAST_QUEUE_FAILED, "000 status=false");
        }

        printLine("[espnow] 000 status=true");
        return RESULT_OK;
    }

    const size_t index = static_cast<size_t>(deviceNumber - 1);

    bool delivered = false;
    const bool gotStatus = context().espNow->sendToDeviceWithStatus(index, outgoing, delivered, 700);

    EspNowManager::deviceInfo target = {};
    if (context().database != nullptr && context().database->isReady() && context().espNow->deviceAt(index, target)) {
        context().database->logOutgoingEspNow(target.mac, outgoing, gotStatus && delivered);
    }

    if (context().lcdDashboard != nullptr) {
        context().lcdDashboard->notifyTx(gotStatus && delivered);
    }

    if (!gotStatus) {
        return failWithCode(AppError::Code::SEND_STATUS_TIMEOUT, "status=false (sem callback/timeout)");
    }

    if (!delivered) {
        return failWithCode(AppError::Code::SEND_DELIVERY_FAILED, "status=false");
    }

    printLine("[espnow] status=true");
    return RESULT_OK;
}

uint8_t wrapper_espnow_send_all(string command) {
    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando send_all");
    }

    EspNowManager::message outgoing = {};
    outgoing.timer = millis();
    // CMDO tells a registered receiving peer "run this as a shell command and
    // reply with the output" (see EspNowConfig::processRxMessageInternal).
    outgoing.type = EspNowManager::logType::CMDO;
    outgoing.packet_number = 0;
    outgoing.total_packets = 1;
    outgoing.checksum = 0;

    const string msg = stripOuterQuotes(command);
    const size_t maxLen = EspNowManager::MESSAGE_TEXT_SIZE;
    const size_t copyLen = (msg.size() < maxLen) ? msg.size() : maxLen;
    if (copyLen > 0) {
        std::memcpy(outgoing.content.text, msg.c_str(), copyLen);
    }
    outgoing.content.text[copyLen] = '\0';
    outgoing.content.size = copyLen;

    size_t deliveredCount = 0;
    size_t triedCount = 0;
    const bool attempted = context().espNow->sendToAllWithStatus(outgoing, deliveredCount, triedCount, 700);

    if (!attempted || triedCount == 0) {
        uint8_t broadcastMac[6] = {0, 0, 0, 0, 0, 0};
        resolveDefaultBroadcastMac(broadcastMac);

        const bool queued = context().espNow->sendToMac(broadcastMac, outgoing);
        if (context().database != nullptr && context().database->isReady()) {
            context().database->logOutgoingEspNow(broadcastMac, outgoing, queued);
        }

        if (context().lcdDashboard != nullptr) {
            context().lcdDashboard->notifyTx(queued);
        }

        if (!queued) {
            return failWithCode(AppError::Code::BROADCAST_QUEUE_FAILED, "status=false (nenhum peer e broadcast falhou)");
        }

        printLine("[espnow] status=true (broadcast 000)");
        return RESULT_OK;
    }

    char line[120] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[espnow] status=%s delivered=%u/%u",
        (deliveredCount == triedCount) ? "true" : "false",
        static_cast<unsigned>(deliveredCount),
        static_cast<unsigned>(triedCount)
    );
    printLine(line);

    if (context().lcdDashboard != nullptr) {
        context().lcdDashboard->notifyTx(deliveredCount == triedCount);
    }

    if (deliveredCount != triedCount) {
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
    context().shell->add(wrapper_espnow_send_to, "send_to", "send to index: <number|000>, <command> (000=all)", "espnow");
    context().shell->add(wrapper_espnow_send_all, "send_all", "send to all: <command>", "espnow");

    return RESULT_OK;
}

} // namespace EspNowCommands
