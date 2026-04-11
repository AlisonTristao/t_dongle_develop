#include <WiFi.h>
#include "config.h"
#include <ShellSerial.h>
#include <EspNowManager.h>
#include <TinyShell.h>
#include "shell_config.h"

// 80:B5:4E:C6:D8:C8
static constexpr uint8_t PEER_MAC[] = {0x80, 0xB5, 0x4E, 0xC6, 0xD8, 0xC8};

EspNowManager::message myData = {};
ShellSerial serialShell;
EspNowManager espNowManager;
TinyShell tinyShell;

// callback for incoming esp-now data
void OnDataRecv(const uint8_t *mac, const EspNowManager::message& incomingData) {
    myData = incomingData;
    serialShell.addLog(String(myData.msg));
    Serial.printf("%s\n", myData.msg);
}

// callback for esp-now send status
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    // no print, could handle errors if needed
}

void setup() {
    BoardConfig::initBoardPins(false);

    serialShell.begin(Serial, 921600);
    serialShell.setPrompt("$ ");
    while (!Serial); // wait for serial to be ready

    // print mac address
    Serial.print("mac: ");
    Serial.println(WiFi.macAddress());

    espNowManager.setReceiveCallback(OnDataRecv);
    espNowManager.setSendCallback(OnDataSent);

    if (!espNowManager.begin(0, false)) {
        Serial.println("esp-now init failed");
        return;
    }

    espNowManager.addDevice(PEER_MAC, "peer-main", "peer principal para mensagens esp-now");

    const bool shellBound = ShellConfig::bind({&tinyShell, &espNowManager, &Serial});
    if (!shellBound) {
        Serial.println("shell bind failed");
        return;
    }

    if (ShellConfig::registerDefaultModules() != RESULT_OK) {
        Serial.println("shell module registration failed");
        return;
    }

    Serial.println("shell ready: <module> -<command> [args]");
    Serial.println("use: help -e");
}

void loop() {
    String command;
    if (serialShell.readInputLine(command)) {
        const std::string response = ShellConfig::runLine(std::string(command.c_str()));
        if (!response.empty()) {
            Serial.println(response.c_str());
            serialShell.addLog(String(response.c_str()));
        }
    }

    delay(5); // small delay to prevent blocking
}
