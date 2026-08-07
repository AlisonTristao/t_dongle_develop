#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ShellSerial.h>
#include <EspNowManager.h>
#include <DonglePeripherals.h>
#include <LcdDashboard.h>
#include <DatabaseStore.h>
#include <TinyShell.h>

class AppRuntime final {
public:
    void begin();
    void tick();

private:
    static void espNowRxWorkerTask(void* param);
    static void espNowRxDbWorkerTask(void* param);
    static void espNowHeartbeatWorkerTask(void* param);

    void restoreShellHistoryFromDatabase();
    BaseType_t selectEspNowWorkerCore() const;
    void startEspNowWorkers(bool asyncRxEnabled);
    void startHeartbeatWorker();
    void processAsyncWarnings(bool& needPromptRefresh);
    void flushPendingEspNowOutput(bool& needPromptRefresh);
    void handleShellInput();

    ShellSerial serialShell_;
    EspNowManager espNowManager_;
    DonglePeripherals donglePeripherals_;
    LcdDashboard lcdDashboard_;
    DatabaseStore databaseStore_;
    TinyShell tinyShell_;

    TaskHandle_t espNowRxTaskHandle_ = nullptr;
    TaskHandle_t espNowRxDbTaskHandle_ = nullptr;
    TaskHandle_t espNowHeartbeatTaskHandle_ = nullptr;
};
