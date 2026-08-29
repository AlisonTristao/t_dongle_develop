#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>

#include <ShellLineEditor.h>
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
    static void espNowHeartbeatWorkerTask(void* param);

    void restoreShellHistoryFromDatabase();
    // Opens SQLite off the boot path (topico 35 C.3): called every tick(),
    // a no-op once the DB is up or after the attempt budget is spent. The
    // first attempt waits a beat so begin()'s big allocations have settled.
    void maybeInitDatabase();
    BaseType_t selectEspNowWorkerCore() const;
    void startEspNowWorkers(bool asyncRxEnabled);
    void startHeartbeatWorker();
    void processAsyncWarnings(bool& needPromptRefresh);
    void flushPendingEspNowOutput(bool& needPromptRefresh);
    void handleShellInput();
    // Drain whatever the line editor echoed into `editorOut_` to the USB
    // console. No-op while SerialMux owns the port. Clears the buffer.
    void flushEditorOutput();

    // The USB console's line editor. Reads bytes fed from Serial, appends
    // everything it would echo to editorOut_ (drained by flushEditorOutput);
    // ShellLineEditor is the shared, framework-free port of the old
    // lib/ShellSerial (now in the TinyShell package).
    ShellLineEditor serialShell_;
    std::string editorOut_;
    EspNowManager espNowManager_;
    DonglePeripherals donglePeripherals_;
    LcdDashboard lcdDashboard_;
    DatabaseStore databaseStore_;
    TinyShell tinyShell_;

    TaskHandle_t espNowRxTaskHandle_ = nullptr;
    TaskHandle_t espNowHeartbeatTaskHandle_ = nullptr;

    // Deferred SQLite open (topico 35 C.3).
    bool databaseReady_ = false;
    uint8_t databaseInitAttempts_ = 0;
    uint32_t lastDatabaseInitMs_ = 0;
};
