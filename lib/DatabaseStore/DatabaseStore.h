#pragma once

#include <Arduino.h>
#include <EspNowManager.h>

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
     * @brief Opens database, applies bootstrap SQL, and loads peers into EspNowManager.
     */
    bool begin(EspNowManager& espNow, Stream* io = nullptr);

    /**
     * @brief Deletes current DB file and recreates schema from bootstrap script.
     */
    bool rebuild(EspNowManager& espNow);

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
     * @brief Stores one incoming ESP-NOW payload and relates it to peers table.
     */
    bool logIncomingEspNow(const uint8_t mac[6], const EspNowManager::message& incoming);

    /**
     * @brief Stores one outgoing ESP-NOW payload with delivery status.
     */
    bool logOutgoingEspNow(const uint8_t mac[6], const EspNowManager::message& outgoing, bool delivered);

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
     * @brief Drops one table when identifier is valid.
     */
    bool dropTable(const String& tableName);

    /**
     * @brief Executes any SQL statement and returns query output text when applicable.
     */
    bool executeSql(const String& sql, String& outText);

private:
    sqlite3* db_;
    Stream* io_;
    bool ready_;

    static constexpr const char* kFsDatabaseDir = "/database";
    static constexpr const char* kFsBootstrapPath = "/database/bootstrap.sql";
    static constexpr const char* kFsDatabasePath = "/database/dongle.db";
    static constexpr const char* kSqliteDatabasePath = "/sdcard/database/dongle.db";

    bool openDatabase();
    void closeDatabase();

    bool ensureBootstrapAssets();
    bool applyBootstrapScript();
    bool applyRuntimeMigrations();
    bool loadPeersFromDatabase(EspNowManager& espNow);
    bool ensurePeerExistsWithDefaults(const uint8_t mac[6], int32_t& outPeerId);
    bool peerIdByMac(const uint8_t mac[6], int32_t& outPeerId);

    bool executeNoResult(const String& sql);
    bool querySingleInt(const String& sql, int32_t& outValue);
    bool queryToText(const String& sql, size_t maxRows, String& outText);

    static int64_t currentEpochSeconds();

    void logLine(const String& text) const;

    static bool isSafeIdentifier(const String& value);
    static String escapeSqlText(const String& value);
    static String macToText(const uint8_t mac[6]);
};
