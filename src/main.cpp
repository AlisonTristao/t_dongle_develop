#include <WiFi.h>
#include <esp_now.h>

#define PEER_MAC {0x48, 0x27, 0xE2, 0x14, 0x70, 0xFC} // mac of the other esp

uint8_t peerAddress[] = PEER_MAC;
String commandBuffer = "";

// callback for incoming esp-now data
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
    char buffer[250];
    memcpy(buffer, incomingData, len);
    buffer[len] = '\0';
    Serial.println(buffer); // print only the data
}

// callback for esp-now send status
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    // no print, could handle errors if needed
}

void setup() {
    Serial.begin(115200);
    while (!Serial); // wait for serial to be ready

    // print mac address
    Serial.print("mac: ");
    Serial.println(WiFi.macAddress());

    // set wifi to station mode
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(); // avoid trying to connect

    // init esp-now
    if (esp_now_init() != ESP_OK) return;

    // register callbacks
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_register_send_cb(OnDataSent);

    // add peer
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, peerAddress, 6);
    peerInfo.channel = 0; // same channel for both esp
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
}

void loop() {
    // read serial input and send via esp-now
    while (Serial.available()) {
        char c = Serial.read();

        // echo character to serial
        Serial.write(c);

        // handle backspace/delete
        if (c == 8 || c == 127) {
            if (!commandBuffer.isEmpty()) {
                commandBuffer.remove(commandBuffer.length() - 1);
            }
            continue;
        }

        if (c == '\n') {
            commandBuffer.trim();
            if (commandBuffer.length() > 0) {
                esp_now_send(peerAddress, (uint8_t *)commandBuffer.c_str(), commandBuffer.length());
            }
            commandBuffer = "";
        } else {
            commandBuffer += c;
        }
    }

    delay(5); // small delay to prevent blocking
}
