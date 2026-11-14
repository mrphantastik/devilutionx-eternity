/**
 * @file realm_database.hpp
 * @brief Embedded SQLite3 WAL-mode persistence engine for dx-eternity-realm.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "protocol/realm_protocol.hpp"

struct sqlite3;

namespace devilution::realm {

struct AccountRecord {
	int64_t id = 0;
	std::string username;
	std::string passwordHash;
	uint64_t createdAt = 0;
	uint64_t lastLoginAt = 0;
	bool isBanned = false;
	std::string banReason;
};

struct CharacterRecord {
	int64_t id = 0;
	int64_t accountId = 0;
	int32_t heroSlot = 0;
	std::string heroName;
	uint8_t heroClass = 0;
	uint8_t heroLevel = 1;
	uint8_t diabloKillLevel = 0;
	uint8_t strength = 0;
	uint8_t magic = 0;
	uint8_t dexterity = 0;
	uint8_t vitality = 0;
	int32_t experience = 0;
	int32_t gold = 0;
	PlayerPack pack {};
	uint64_t updatedAt = 0;
};

enum class LedgerItemSource : int32_t {
	StartingEquipment = 0,
	MonsterDrop = 1,
	ChestDrop = 2,
	VendorOrReward = 3,
};

enum class LedgerItemState : int32_t {
	OnGround = 0,
	InInventoryOrStash = 1,
	ConsumedOrSold = 2,
};

struct AuditLogEntry {
	int64_t id = 0;
	int64_t accountId = 0;
	int64_t charId = 0;
	uint64_t gameId = 0;
	std::string severity;
	std::string category;
	std::string details;
	uint64_t createdAt = 0;
};

class RealmDatabase {
public:
	RealmDatabase() = default;
	~RealmDatabase();

	RealmDatabase(const RealmDatabase &) = delete;
	RealmDatabase &operator=(const RealmDatabase &) = delete;

	/**
	 * @brief Opens (or creates) the SQLite3 database at `dbPath`, enables WAL mode,
	 * and initializes all Closed Realm tables and indices.
	 */
	bool Open(const std::string &dbPath, std::string &outError);

	/**
	 * @brief Closes the database connection cleanly.
	 */
	void Close();

	bool IsOpen() const
	{
		return db_ != nullptr;
	}

	/**
	 * @brief Creates a new account row with an Argon2id `passwordHash` and initializes an empty stash row.
	 */
	bool CreateAccount(const std::string &username, const std::string &passwordHash, int64_t &outAccountId, RealmStatus &outStatus);

	/**
	 * @brief Looks up an account by case-insensitive username.
	 */
	std::optional<AccountRecord> FindAccountByUsername(const std::string &username) const;

	/**
	 * @brief Looks up an account by primary key `accountId`.
	 */
	std::optional<AccountRecord> FindAccountById(int64_t accountId) const;

	/**
	 * @brief Updates `last_login_at` timestamp for `accountId`.
	 */
	bool UpdateLastLogin(int64_t accountId);

	/**
	 * @brief Sets or clears the ban status of `accountId`.
	 */
	bool SetAccountBanStatus(int64_t accountId, bool banned, const std::string &reason);

	/**
	 * @brief Lists all characters owned by `accountId` ordered by `hero_slot`.
	 */
	std::vector<CharacterRecord> ListCharactersForAccount(int64_t accountId) const;

	/**
	 * @brief Creates a new character row for `accountId` in the next available `hero_slot` (0..MaxHeroesPerAccount-1).
	 */
	bool CreateCharacter(
	    int64_t accountId,
	    const std::string &heroName,
	    uint8_t heroClass,
	    const PlayerPack &initialPack,
	    int64_t &outCharId,
	    RealmStatus &outStatus);

	/**
	 * @brief Deletes a character owned by `accountId` and removes its `item_ledger` entries.
	 */
	bool DeleteCharacter(int64_t accountId, int64_t charId, RealmStatus &outStatus);

	/**
	 * @brief Loads a character owned by `accountId` (`charId`).
	 */
	bool LoadCharacter(int64_t accountId, int64_t charId, CharacterRecord &outRecord, RealmStatus &outStatus) const;

	/**
	 * @brief Saves an updated `PlayerPack` snapshot for `(accountId, charId)`.
	 */
	bool SaveCharacter(int64_t accountId, int64_t charId, const PlayerPack &pack, RealmStatus &outStatus);

	/**
	 * @brief Loads the account's shared Server-Vault stash (`stashGold` and packed `PkItemStruct` items).
	 */
	bool LoadStash(int64_t accountId, uint32_t &outStashGold, std::vector<PkItemStruct> &outItems, RealmStatus &outStatus) const;

	/**
	 * @brief Saves the account's shared Server-Vault stash (`stashGold` and packed `PkItemStruct` items).
	 */
	bool SaveStash(int64_t accountId, uint32_t stashGold, const std::vector<PkItemStruct> &items, RealmStatus &outStatus);

	/**
	 * @brief Appends a verified item into `item_ledger` for provenance tracking.
	 */
	bool RecordLedgerItem(
	    uint64_t gameId,
	    int64_t ownerCharId,
	    const PkItemStruct &item,
	    LedgerItemSource sourceType,
	    uint8_t sourceLevel,
	    LedgerItemState state,
	    int64_t *outItemUid = nullptr);

	/**
	 * @brief Atomically transitions an `item_ledger` row (`itemUid`, `gameId`) from `expectedState` to `newState`
	 * and assigns `newOwnerCharId`. Prevents race-condition multi-pickup duping.
	 */
	bool TransitionLedgerItemState(
	    int64_t itemUid,
	    uint64_t gameId,
	    int64_t newOwnerCharId,
	    LedgerItemState expectedState,
	    LedgerItemState newState);

	/**
	 * @brief Atomically transitions one `InInventoryOrStash` ledger entry owned by `ownerCharId`
	 * matching `item` back to `OnGround` in `gameId`, returning its `outItemUid`.
	 */
	bool TransitionOwnedItemToGround(
	    int64_t ownerCharId,
	    uint64_t gameId,
	    const PkItemStruct &item,
	    int64_t &outItemUid);

	/**
	 * @brief Checks if `charId` holds an active `InInventoryOrStash` entry in `item_ledger` matching `item`.
	 */
	bool HasCharacterLedgerProvenance(int64_t charId, const PkItemStruct &item) const;

	/**
	 * @brief Returns how many active `InInventoryOrStash` copies of `(idx, iCreateInfo, iSeed, dwBuff)` `charId` owns in `item_ledger`.
	 */
	int64_t CountCharacterLedgerCopies(int64_t charId, const PkItemStruct &item) const;

	/**
	 * @brief Returns how many active `InInventoryOrStash` copies of `(idx, iCreateInfo, iSeed, dwBuff)` are owned across all characters of `accountId`.
	 */
	int64_t CountAccountLedgerCopies(int64_t accountId, const PkItemStruct &item) const;

	/**
	 * @brief Returns the number of `item_ledger` entries currently owned by `charId`.
	 */
	int64_t CountLedgerItemsForCharacter(int64_t charId) const;

	/**
	 * @brief Removes all `item_ledger` entries currently owned by `charId` (used during Admin Hero Re-Import).
	 */
	bool ClearCharacterLedgerItems(int64_t charId);

	/**
	 * @brief Configures an optional plain-text diagnostic log file (e.g. `dx-eternity-realm.log`)
	 * where all `LogSecurityEvent` entries are appended in real time with timestamps.
	 */
	void SetLogFilePath(const std::string &logFilePath)
	{
		logFilePath_ = logFilePath;
	}

	/**
	 * @brief Appends a structured security, diagnostic, or anomaly event to `security_audit_log`
	 * and mirrors it to `logFilePath_` (`dx-eternity-realm.log`) if configured.
	 */
	bool LogSecurityEvent(
	    int64_t accountId,
	    int64_t charId,
	    uint64_t gameId,
	    const std::string &severity,
	    const std::string &category,
	    const std::string &details);

	/**
	 * @brief Returns total count of rows in `security_audit_log` (used by self-test & monitoring).
	 */
	int64_t GetSecurityAuditCount() const;

	int64_t GetTotalAccountsCount() const;
	int64_t GetTotalCharactersCount() const;
	int64_t GetTotalLedgerItemsCount() const;

	std::vector<AccountRecord> ListAccounts(size_t limit = 50) const;
	std::vector<CharacterRecord> ListAllCharacters(size_t limit = 50) const;
	std::vector<AuditLogEntry> ListRecentAuditLogs(size_t limit = 100) const;

	sqlite3 *RawHandle() const
	{
		return db_;
	}

private:
	bool ExecSql(const char *sql, std::string &outError);

	sqlite3 *db_ = nullptr;
	std::string logFilePath_;
};

} // namespace devilution::realm
