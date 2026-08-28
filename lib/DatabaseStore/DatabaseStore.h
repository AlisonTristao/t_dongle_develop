#pragma once

#include <Arduino.h>
#include <EspNowManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <btp/codec.hpp>

struct sqlite3;

/**
 * @brief Persists runtime data in SQLite database stored on SD card.
 *
 * Stored entities:
 * - ESP-NOW peers (MAC/name/description)
 * - shell command history (command/source/timestamp)
 */
class DatabaseStore final {
public:
    DatabaseStore();

    /**
     * @brief Opens database and applies bootstrap SQL/migrations.
     *
     * Deliberately takes no EspNowManager: this runs before espNowManager's
     * begin() in AppRuntime, while the internal heap is least fragmented by
     * the WiFi/ESP-NOW driver, so SQLite's own allocations get first pick.
     * Call loadPeers() separately once ESP-NOW is up.
     */
    bool begin(Stream* io = nullptr);

    /**
     * @brief Loads persisted peers into EspNowManager. Requires ESP-NOW to
     * already be initialized (esp_now_add_peer needs it); call after
     * espNowManager.begin(), once begin() above has returned true.
     */
    bool loadPeers(EspNowManager& espNow);

    /**
     * @brief Deletes current DB file and recreates schema from bootstrap script.
     */
    bool rebuild(EspNowManager& espNow);

    /**
     * @brief Creates a timestamped database snapshot under /database/backups.
     */
    bool backup(String& outText);

    /**
     * @brief Closes active SQLite handle.
     */
    void end();

    /**
     * @brief Returns true when DB is open and ready.
     */
    bool isReady() const;

    /**
     * @brief Inserts or updates one peer by MAC.
     */
    bool upsertPeer(const uint8_t mac[6], const char* name, const char* description);

    /**
     * @brief Removes one peer by MAC.
     */
    bool removePeer(const uint8_t mac[6]);

    /**
     * @brief Updates peer metadata by MAC.
     */
    bool updatePeerMetadata(const uint8_t mac[6], const char* name, const char* description);

    /**
     * @brief Logs one executed command in command_log table.
     */
    bool logCommand(const char* command, const char* source = "serial");

    /**
     * @brief Logs command and stores output in related table.
     */
    bool logCommandWithOutput(const char* command, const char* output, const char* source = "serial");

    /**
     * @brief Stores one outgoing ESP-NOW payload with delivery status. type
     * is stored as its raw btp::MessageType numeric value; payload is a
     * human-readable preview (see payloadPreviewText in the .cpp), not the
     * wire envelope.
     */
    bool logOutgoingEspNow(const uint8_t mac[6], btp::MessageType type, const uint8_t* payload, size_t payloadSize, bool delivered);

    /**
     * @brief Stores one boot event timestamp.
     */
    bool logBootEvent(const char* reason = "power_on");

    /**
     * @brief Persists all peers currently registered in EspNowManager.
     */
    bool syncPeersFromManager(const EspNowManager& espNow);

    /**
     * @brief Returns human-readable database status summary.
     */
    bool getStatus(String& outText);

    /**
     * @brief Lists user tables present in sqlite_master.
     */
    bool listTables(String& outText);

    /**
     * @brief Reads rows from one table with LIMIT.
     */
    bool readTable(const String& tableName, size_t limit, String& outText);

    /**
     * @brief Reads shell command logs joined with their textual outputs.
     *
     * Output fields include formatted date/time for readability.
     */
    bool readCommandLogsWithOutput(size_t limit, String& outText);

    /**
     * @brief Reads recent serial commands for shell history restoration.
     *
     * Returns one command per line in chronological order (oldest -> newest).
     */
    bool readRecentCommands(size_t limit, String& outText);

    /**
     * @brief Reads unified ESP-NOW history (RX and TX) with delivery status.
     *
     * Output fields include formatted date/time and TX success/failure.
     */
    bool readEspNowHistory(size_t limit, String& outText);

    /**
     * @brief Drops one table when identifier is valid.
     */
    bool dropTable(const String& tableName);

    /**
     * @brief Executes any SQL statement and returns query output text when applicable.
     */
    bool executeSql(const String& sql, String& outText);

    /**
     * @brief Counts rows in one table.
     */
    bool countRows(const String& tableName, int32_t& outCount);

    /**
     * @brief Deletes rows matching a WHERE clause. The clause is required
     * (no accidental full-table delete) and is not escaped, same trust model
     * as executeSql().
     */
    bool deleteRows(const String& tableName, const String& whereClause, int32_t& outDeletedCount);

    /**
     * @brief Runs SQLite VACUUM to reclaim space and defragment the file.
     */
    bool vacuum(String& outText);

    /**
     * @brief Dumps one table to /database/exports/<table>.csv on the SD card.
     */
    bool exportTableToCsv(const String& tableName, String& outText);

    /**
     * @brief Clears command_log, command_log_output and espnow_outgoing_log
     * without dropping the tables (keeps peers/boot_events/kv_store intact).
     */
    bool clearLogs(String& outText);

    /**
     * @brief Starts an explicit SQLite transaction.
     */
    bool beginTransaction();

    /**
     * @brief Commits current SQLite transaction.
     */
    bool commitTransaction();

    /**
     * @brief Rolls back current SQLite transaction.
     */
    bool rollbackTransaction();

private:
    sqlite3* db_;
    Stream* io_;
    bool ready_;
    SemaphoreHandle_t dbMutex_;

    static constexpr const char* kFsDatabaseDir = "/database";
    static constexpr const char* kFsBackupDir = "/database/backups";
    static constexpr const char* kFsBootstrapPath = "/database/bootstrap.sql";
    static constexpr const char* kFsDatabasePath = "/database/dongle.db";
    static constexpr const char* kSqliteDatabasePath = "/sdcard/database/dongle.db";

    bool openDatabase();
    void closeDatabase();

    bool ensureBootstrapAssets();
    bool applyBootstrapScript();
    bool applyRuntimeMigrations();
    bool ensureDefaultBroadcastPeer();
    bool loadPeersFromDatabase(EspNowManager& espNow);
    bool ensurePeerExistsWithDefaults(const uint8_t mac[6], int32_t& outPeerId);
    bool peerIdByMac(const uint8_t mac[6], int32_t& outPeerId);

    bool executeNoResult(const String& sql);
    bool querySingleInt(const String& sql, int32_t& outValue);
    bool queryToText(const String& sql, size_t maxRows, String& outText);

    bool lockDb(uint32_t timeoutMs = 1000);
    void unlockDb();

    static int64_t currentEpochSeconds();

    void logLine(const String& text) const;

    static bool isSafeIdentifier(const String& value);
    static String escapeSqlText(const String& value);
    static String macToText(const uint8_t mac[6]);
};
