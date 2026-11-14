/**
 * @file realm_database.cpp
 * @brief SQLite3 WAL-mode schema bootstrap and persistence operations for dx-eternity-realm.
 */
#include "db/realm_database.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sqlite3.h>

namespace devilution::realm {

namespace {

uint64_t CurrentUnixSeconds()
{
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::seconds>(
	        std::chrono::system_clock::now().time_since_epoch())
	        .count());
}

constexpr const char *kBootstrapSchemaSql = R"SQL(
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;
PRAGMA temp_store = MEMORY;

CREATE TABLE IF NOT EXISTS accounts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL UNIQUE COLLATE NOCASE,
    password_hash TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    last_login_at INTEGER NOT NULL,
    is_banned INTEGER NOT NULL DEFAULT 0,
    ban_reason TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS characters (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    hero_slot INTEGER NOT NULL,
    hero_name TEXT NOT NULL UNIQUE COLLATE NOCASE,
    hero_class INTEGER NOT NULL,
    hero_level INTEGER NOT NULL DEFAULT 1,
    diablo_kill_level INTEGER NOT NULL DEFAULT 0,
    strength INTEGER NOT NULL,
    magic INTEGER NOT NULL,
    dexterity INTEGER NOT NULL,
    vitality INTEGER NOT NULL,
    experience INTEGER NOT NULL DEFAULT 0,
    gold INTEGER NOT NULL DEFAULT 0,
    player_pack_blob BLOB NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE(account_id, hero_slot)
);

CREATE TABLE IF NOT EXISTS stashes (
    account_id INTEGER PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    gold_vault INTEGER NOT NULL DEFAULT 0,
    stash_blob BLOB NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS item_ledger (
    item_uid INTEGER PRIMARY KEY AUTOINCREMENT,
    game_id INTEGER NOT NULL,
    owner_char_id INTEGER NOT NULL DEFAULT 0,
    item_idx INTEGER NOT NULL,
    create_info INTEGER NOT NULL,
    item_seed INTEGER NOT NULL,
    item_buff INTEGER NOT NULL,
    source_type INTEGER NOT NULL,
    source_level INTEGER NOT NULL,
    state INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_item_ledger_owner
    ON item_ledger(owner_char_id, state);
CREATE INDEX IF NOT EXISTS idx_item_ledger_seed
    ON item_ledger(item_idx, create_info, item_seed, item_buff);

CREATE TABLE IF NOT EXISTS security_audit_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id INTEGER NOT NULL DEFAULT 0,
    char_id INTEGER NOT NULL DEFAULT 0,
    game_id INTEGER NOT NULL DEFAULT 0,
    severity TEXT NOT NULL,
    category TEXT NOT NULL,
    details TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
)SQL";

} // namespace

RealmDatabase::~RealmDatabase()
{
	Close();
}

bool RealmDatabase::Open(const std::string &dbPath, std::string &outError)
{
	Close();

	const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
	if (sqlite3_open_v2(dbPath.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
		outError = db_ != nullptr ? sqlite3_errmsg(db_) : "sqlite3_open_v2 allocation failure";
		Close();
		return false;
	}

	if (!ExecSql(kBootstrapSchemaSql, outError)) {
		Close();
		return false;
	}
	return true;
}

void RealmDatabase::Close()
{
	if (db_ != nullptr) {
		sqlite3_close_v2(db_);
		db_ = nullptr;
	}
}

bool RealmDatabase::ExecSql(const char *sql, std::string &outError)
{
	char *errMsg = nullptr;
	const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
	if (rc != SQLITE_OK) {
		outError = errMsg != nullptr ? errMsg : "SQLite execution error";
		if (errMsg != nullptr)
			sqlite3_free(errMsg);
		return false;
	}
	return true;
}

bool RealmDatabase::CreateAccount(const std::string &username, const std::string &passwordHash, int64_t &outAccountId, RealmStatus &outStatus)
{
	outAccountId = 0;
	if (db_ == nullptr) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	if (FindAccountByUsername(username).has_value()) {
		outStatus = RealmStatus::ERR_AUTH_ACCOUNT_EXISTS;
		return false;
	}

	const uint64_t now = CurrentUnixSeconds();
	constexpr const char *kInsertAccountSql = R"SQL(
		INSERT INTO accounts (username, password_hash, created_at, last_login_at, is_banned, ban_reason)
		VALUES (?1, ?2, ?3, ?3, 0, '');
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kInsertAccountSql, -1, &stmt, nullptr) != SQLITE_OK) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, passwordHash.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(now));

	const int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	if (rc != SQLITE_DONE) {
		outStatus = (rc == SQLITE_CONSTRAINT) ? RealmStatus::ERR_AUTH_ACCOUNT_EXISTS : RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	outAccountId = sqlite3_last_insert_rowid(db_);

	// Initialize an empty stash row for this account
	constexpr const char *kInsertStashSql = R"SQL(
		INSERT OR IGNORE INTO stashes (account_id, gold_vault, stash_blob, updated_at)
		VALUES (?1, 0, zeroblob(0), ?2);
	)SQL";
	sqlite3_stmt *stashStmt = nullptr;
	if (sqlite3_prepare_v2(db_, kInsertStashSql, -1, &stashStmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(stashStmt, 1, outAccountId);
		sqlite3_bind_int64(stashStmt, 2, static_cast<sqlite3_int64>(now));
		sqlite3_step(stashStmt);
		sqlite3_finalize(stashStmt);
	}

	outStatus = RealmStatus::OK;
	return true;
}

std::optional<AccountRecord> RealmDatabase::FindAccountByUsername(const std::string &username) const
{
	if (db_ == nullptr)
		return std::nullopt;

	constexpr const char *kSql = R"SQL(
		SELECT id, username, password_hash, created_at, last_login_at, is_banned, ban_reason
		FROM accounts
		WHERE username = ?1 COLLATE NOCASE
		LIMIT 1;
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK)
		return std::nullopt;

	sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);

	std::optional<AccountRecord> result;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		AccountRecord rec {};
		rec.id = sqlite3_column_int64(stmt, 0);
		const unsigned char *u = sqlite3_column_text(stmt, 1);
		const unsigned char *p = sqlite3_column_text(stmt, 2);
		const unsigned char *b = sqlite3_column_text(stmt, 6);
		rec.username = u != nullptr ? reinterpret_cast<const char *>(u) : "";
		rec.passwordHash = p != nullptr ? reinterpret_cast<const char *>(p) : "";
		rec.createdAt = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
		rec.lastLoginAt = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
		rec.isBanned = sqlite3_column_int(stmt, 5) != 0;
		rec.banReason = b != nullptr ? reinterpret_cast<const char *>(b) : "";
		result = std::move(rec);
	}
	sqlite3_finalize(stmt);
	return result;
}

std::optional<AccountRecord> RealmDatabase::FindAccountById(int64_t accountId) const
{
	if (db_ == nullptr)
		return std::nullopt;

	constexpr const char *kSql = R"SQL(
		SELECT id, username, password_hash, created_at, last_login_at, is_banned, ban_reason
		FROM accounts
		WHERE id = ?1
		LIMIT 1;
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK)
		return std::nullopt;

	sqlite3_bind_int64(stmt, 1, accountId);

	std::optional<AccountRecord> result;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		AccountRecord rec {};
		rec.id = sqlite3_column_int64(stmt, 0);
		const unsigned char *u = sqlite3_column_text(stmt, 1);
		const unsigned char *p = sqlite3_column_text(stmt, 2);
		const unsigned char *b = sqlite3_column_text(stmt, 6);
		rec.username = u != nullptr ? reinterpret_cast<const char *>(u) : "";
		rec.passwordHash = p != nullptr ? reinterpret_cast<const char *>(p) : "";
		rec.createdAt = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
		rec.lastLoginAt = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
		rec.isBanned = sqlite3_column_int(stmt, 5) != 0;
		rec.banReason = b != nullptr ? reinterpret_cast<const char *>(b) : "";
		result = std::move(rec);
	}
	sqlite3_finalize(stmt);
	return result;
}

bool RealmDatabase::UpdateLastLogin(int64_t accountId)
{
	if (db_ == nullptr)
		return false;

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, "UPDATE accounts SET last_login_at = ?1 WHERE id = ?2;", -1, &stmt, nullptr) != SQLITE_OK)
		return false;

	sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(CurrentUnixSeconds()));
	sqlite3_bind_int64(stmt, 2, accountId);
	const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
	return ok;
}

bool RealmDatabase::SetAccountBanStatus(int64_t accountId, bool banned, const std::string &reason)
{
	if (db_ == nullptr)
		return false;

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, "UPDATE accounts SET is_banned = ?1, ban_reason = ?2 WHERE id = ?3;", -1, &stmt, nullptr) != SQLITE_OK)
		return false;

	sqlite3_bind_int(stmt, 1, banned ? 1 : 0);
	sqlite3_bind_text(stmt, 2, reason.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, accountId);
	const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
	return ok;
}

std::vector<CharacterRecord> RealmDatabase::ListCharactersForAccount(int64_t accountId) const
{
	std::vector<CharacterRecord> list;
	if (db_ == nullptr)
		return list;

	constexpr const char *kSql = R"SQL(
		SELECT id, account_id, hero_slot, hero_name, hero_class, hero_level, diablo_kill_level,
		       strength, magic, dexterity, vitality, experience, gold, player_pack_blob, updated_at
		FROM characters
		WHERE account_id = ?1
		ORDER BY hero_slot ASC;
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK)
		return list;

	sqlite3_bind_int64(stmt, 1, accountId);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		CharacterRecord rec {};
		rec.id = sqlite3_column_int64(stmt, 0);
		rec.accountId = sqlite3_column_int64(stmt, 1);
		rec.heroSlot = sqlite3_column_int(stmt, 2);
		const unsigned char *namePtr = sqlite3_column_text(stmt, 3);
		rec.heroName = namePtr != nullptr ? reinterpret_cast<const char *>(namePtr) : "";
		rec.heroClass = static_cast<uint8_t>(sqlite3_column_int(stmt, 4));
		rec.heroLevel = static_cast<uint8_t>(sqlite3_column_int(stmt, 5));
		rec.diabloKillLevel = static_cast<uint8_t>(sqlite3_column_int(stmt, 6));
		rec.strength = static_cast<uint8_t>(sqlite3_column_int(stmt, 7));
		rec.magic = static_cast<uint8_t>(sqlite3_column_int(stmt, 8));
		rec.dexterity = static_cast<uint8_t>(sqlite3_column_int(stmt, 9));
		rec.vitality = static_cast<uint8_t>(sqlite3_column_int(stmt, 10));
		rec.experience = sqlite3_column_int(stmt, 11);
		rec.gold = sqlite3_column_int(stmt, 12);

		const void *blobPtr = sqlite3_column_blob(stmt, 13);
		const int blobBytes = sqlite3_column_bytes(stmt, 13);
		if (blobPtr != nullptr && blobBytes == static_cast<int>(sizeof(PlayerPack))) {
			std::memcpy(&rec.pack, blobPtr, sizeof(PlayerPack));
		}
		rec.updatedAt = static_cast<uint64_t>(sqlite3_column_int64(stmt, 14));
		list.push_back(std::move(rec));
	}
	sqlite3_finalize(stmt);
	return list;
}

bool RealmDatabase::CreateCharacter(
    int64_t accountId,
    const std::string &heroName,
    uint8_t heroClass,
    const PlayerPack &initialPack,
    int64_t &outCharId,
    RealmStatus &outStatus)
{
	outCharId = 0;
	if (db_ == nullptr) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	// Check global case-insensitive hero name uniqueness
	{
		sqlite3_stmt *nameStmt = nullptr;
		if (sqlite3_prepare_v2(db_, "SELECT id FROM characters WHERE hero_name = ?1 COLLATE NOCASE LIMIT 1;", -1, &nameStmt, nullptr) != SQLITE_OK) {
			outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
			return false;
		}
		sqlite3_bind_text(nameStmt, 1, heroName.c_str(), -1, SQLITE_TRANSIENT);
		const bool exists = (sqlite3_step(nameStmt) == SQLITE_ROW);
		sqlite3_finalize(nameStmt);
		if (exists) {
			outStatus = RealmStatus::ERR_HERO_NAME_TAKEN;
			return false;
		}
	}

	const std::vector<CharacterRecord> existing = ListCharactersForAccount(accountId);
	if (existing.size() >= MaxHeroesPerAccount) {
		outStatus = RealmStatus::ERR_HERO_LIMIT_REACHED;
		return false;
	}

	int32_t freeSlot = 0;
	for (; freeSlot < static_cast<int32_t>(MaxHeroesPerAccount); freeSlot++) {
		bool used = false;
		for (const auto &c : existing) {
			if (c.heroSlot == freeSlot) {
				used = true;
				break;
			}
		}
		if (!used)
			break;
	}

	constexpr const char *kInsertSql = R"SQL(
		INSERT INTO characters
		(account_id, hero_slot, hero_name, hero_class, hero_level, diablo_kill_level,
		 strength, magic, dexterity, vitality, experience, gold, player_pack_blob, updated_at)
		VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14);
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kInsertSql, -1, &stmt, nullptr) != SQLITE_OK) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	sqlite3_bind_int64(stmt, 1, accountId);
	sqlite3_bind_int(stmt, 2, freeSlot);
	sqlite3_bind_text(stmt, 3, heroName.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 4, heroClass);
	sqlite3_bind_int(stmt, 5, initialPack.pLevel);
	sqlite3_bind_int(stmt, 6, static_cast<int>(initialPack.pDiabloKillLevel));
	sqlite3_bind_int(stmt, 7, initialPack.pBaseStr);
	sqlite3_bind_int(stmt, 8, initialPack.pBaseMag);
	sqlite3_bind_int(stmt, 9, initialPack.pBaseDex);
	sqlite3_bind_int(stmt, 10, initialPack.pBaseVit);
	sqlite3_bind_int(stmt, 11, initialPack.pExperience);
	sqlite3_bind_int(stmt, 12, initialPack.pGold);
	sqlite3_bind_blob(stmt, 13, &initialPack, static_cast<int>(sizeof(PlayerPack)), SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 14, static_cast<sqlite3_int64>(CurrentUnixSeconds()));

	const int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	if (rc != SQLITE_DONE) {
		outStatus = (rc == SQLITE_CONSTRAINT) ? RealmStatus::ERR_HERO_NAME_TAKEN : RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	outCharId = sqlite3_last_insert_rowid(db_);
	outStatus = RealmStatus::OK;
	return true;
}

bool RealmDatabase::DeleteCharacter(int64_t accountId, int64_t charId, RealmStatus &outStatus)
{
	if (db_ == nullptr) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, "DELETE FROM characters WHERE id = ?1 AND account_id = ?2;", -1, &stmt, nullptr) != SQLITE_OK) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}
	sqlite3_bind_int64(stmt, 1, charId);
	sqlite3_bind_int64(stmt, 2, accountId);
	const int rc = sqlite3_step(stmt);
	const int changes = sqlite3_changes(db_);
	sqlite3_finalize(stmt);

	if (rc != SQLITE_DONE || changes == 0) {
		outStatus = RealmStatus::ERR_HERO_NOT_FOUND;
		return false;
	}

	sqlite3_stmt *ledgerStmt = nullptr;
	if (sqlite3_prepare_v2(db_, "DELETE FROM item_ledger WHERE owner_char_id = ?1;", -1, &ledgerStmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int64(ledgerStmt, 1, charId);
		sqlite3_step(ledgerStmt);
		sqlite3_finalize(ledgerStmt);
	}

	outStatus = RealmStatus::OK;
	return true;
}

bool RealmDatabase::LoadCharacter(int64_t accountId, int64_t charId, CharacterRecord &outRecord, RealmStatus &outStatus) const
{
	if (db_ == nullptr) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	constexpr const char *kSql = R"SQL(
		SELECT id, account_id, hero_slot, hero_name, hero_class, hero_level, diablo_kill_level,
		       strength, magic, dexterity, vitality, experience, gold, player_pack_blob, updated_at
		FROM characters
		WHERE id = ?1 AND account_id = ?2
		LIMIT 1;
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	sqlite3_bind_int64(stmt, 1, charId);
	sqlite3_bind_int64(stmt, 2, accountId);

	bool found = false;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		outRecord = {};
		outRecord.id = sqlite3_column_int64(stmt, 0);
		outRecord.accountId = sqlite3_column_int64(stmt, 1);
		outRecord.heroSlot = sqlite3_column_int(stmt, 2);
		const unsigned char *namePtr = sqlite3_column_text(stmt, 3);
		outRecord.heroName = namePtr != nullptr ? reinterpret_cast<const char *>(namePtr) : "";
		outRecord.heroClass = static_cast<uint8_t>(sqlite3_column_int(stmt, 4));
		outRecord.heroLevel = static_cast<uint8_t>(sqlite3_column_int(stmt, 5));
		outRecord.diabloKillLevel = static_cast<uint8_t>(sqlite3_column_int(stmt, 6));
		outRecord.strength = static_cast<uint8_t>(sqlite3_column_int(stmt, 7));
		outRecord.magic = static_cast<uint8_t>(sqlite3_column_int(stmt, 8));
		outRecord.dexterity = static_cast<uint8_t>(sqlite3_column_int(stmt, 9));
		outRecord.vitality = static_cast<uint8_t>(sqlite3_column_int(stmt, 10));
		outRecord.experience = sqlite3_column_int(stmt, 11);
		outRecord.gold = sqlite3_column_int(stmt, 12);

		const void *blobPtr = sqlite3_column_blob(stmt, 13);
		const int blobBytes = sqlite3_column_bytes(stmt, 13);
		if (blobPtr != nullptr && blobBytes == static_cast<int>(sizeof(PlayerPack))) {
			std::memcpy(&outRecord.pack, blobPtr, sizeof(PlayerPack));
			found = true;
		}
		outRecord.updatedAt = static_cast<uint64_t>(sqlite3_column_int64(stmt, 14));
	}
	sqlite3_finalize(stmt);

	if (!found) {
		outStatus = RealmStatus::ERR_HERO_NOT_FOUND;
		return false;
	}
	outStatus = RealmStatus::OK;
	return true;
}

bool RealmDatabase::SaveCharacter(int64_t accountId, int64_t charId, const PlayerPack &pack, RealmStatus &outStatus)
{
	if (db_ == nullptr) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	constexpr const char *kUpdateSql = R"SQL(
		UPDATE characters
		SET hero_level = ?1,
		    diablo_kill_level = ?2,
		    strength = ?3,
		    magic = ?4,
		    dexterity = ?5,
		    vitality = ?6,
		    experience = ?7,
		    gold = ?8,
		    player_pack_blob = ?9,
		    updated_at = ?10
		WHERE id = ?11 AND account_id = ?12;
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kUpdateSql, -1, &stmt, nullptr) != SQLITE_OK) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	sqlite3_bind_int(stmt, 1, pack.pLevel);
	sqlite3_bind_int(stmt, 2, static_cast<int>(pack.pDiabloKillLevel));
	sqlite3_bind_int(stmt, 3, pack.pBaseStr);
	sqlite3_bind_int(stmt, 4, pack.pBaseMag);
	sqlite3_bind_int(stmt, 5, pack.pBaseDex);
	sqlite3_bind_int(stmt, 6, pack.pBaseVit);
	sqlite3_bind_int(stmt, 7, pack.pExperience);
	sqlite3_bind_int(stmt, 8, pack.pGold);
	sqlite3_bind_blob(stmt, 9, &pack, static_cast<int>(sizeof(PlayerPack)), SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 10, static_cast<sqlite3_int64>(CurrentUnixSeconds()));
	sqlite3_bind_int64(stmt, 11, charId);
	sqlite3_bind_int64(stmt, 12, accountId);

	const int rc = sqlite3_step(stmt);
	const int changes = sqlite3_changes(db_);
	sqlite3_finalize(stmt);

	if (rc != SQLITE_DONE || changes == 0) {
		outStatus = RealmStatus::ERR_HERO_NOT_FOUND;
		return false;
	}

	outStatus = RealmStatus::OK;
	return true;
}

bool RealmDatabase::LoadStash(int64_t accountId, uint32_t &outStashGold, std::vector<PkItemStruct> &outItems, RealmStatus &outStatus) const
{
	outStashGold = 0;
	outItems.clear();
	if (db_ == nullptr) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, "SELECT gold_vault, stash_blob FROM stashes WHERE account_id = ?1 LIMIT 1;", -1, &stmt, nullptr) != SQLITE_OK) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}
	sqlite3_bind_int64(stmt, 1, accountId);

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		outStashGold = static_cast<uint32_t>(sqlite3_column_int64(stmt, 0));
		const void *blobPtr = sqlite3_column_blob(stmt, 1);
		const int blobBytes = sqlite3_column_bytes(stmt, 1);
		if (blobPtr != nullptr && blobBytes > 0 && (blobBytes % static_cast<int>(sizeof(PkItemStruct)) == 0)) {
			const size_t count = std::min<size_t>(static_cast<size_t>(blobBytes) / sizeof(PkItemStruct), MaxStashItems);
			outItems.resize(count);
			std::memcpy(outItems.data(), blobPtr, count * sizeof(PkItemStruct));
		}
	}
	sqlite3_finalize(stmt);
	outStatus = RealmStatus::OK;
	return true;
}

bool RealmDatabase::SaveStash(int64_t accountId, uint32_t stashGold, const std::vector<PkItemStruct> &items, RealmStatus &outStatus)
{
	if (db_ == nullptr || items.size() > MaxStashItems) {
		outStatus = items.size() > MaxStashItems ? RealmStatus::ERR_MALFORMED_PACKET : RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	constexpr const char *kUpsertSql = R"SQL(
		INSERT INTO stashes (account_id, gold_vault, stash_blob, updated_at)
		VALUES (?1, ?2, ?3, ?4)
		ON CONFLICT(account_id) DO UPDATE SET
		    gold_vault = excluded.gold_vault,
		    stash_blob = excluded.stash_blob,
		    updated_at = excluded.updated_at;
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kUpsertSql, -1, &stmt, nullptr) != SQLITE_OK) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}

	sqlite3_bind_int64(stmt, 1, accountId);
	sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(stashGold));
	if (items.empty()) {
		sqlite3_bind_zeroblob(stmt, 3, 0);
	} else {
		sqlite3_bind_blob(stmt, 3, items.data(), static_cast<int>(items.size() * sizeof(PkItemStruct)), SQLITE_TRANSIENT);
	}
	sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(CurrentUnixSeconds()));

	const int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	if (rc != SQLITE_DONE) {
		outStatus = RealmStatus::ERR_INTERNAL_DATABASE;
		return false;
	}
	outStatus = RealmStatus::OK;
	return true;
}

bool RealmDatabase::RecordLedgerItem(
    uint64_t gameId,
    int64_t ownerCharId,
    const PkItemStruct &item,
    LedgerItemSource sourceType,
    uint8_t sourceLevel,
    LedgerItemState state,
    int64_t *outItemUid)
{
	if (db_ == nullptr)
		return false;

	constexpr const char *kInsertSql = R"SQL(
		INSERT INTO item_ledger
		(game_id, owner_char_id, item_idx, create_info, item_seed, item_buff, source_type, source_level, state, created_at)
		VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kInsertSql, -1, &stmt, nullptr) != SQLITE_OK)
		return false;

	sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(gameId));
	sqlite3_bind_int64(stmt, 2, ownerCharId);
	sqlite3_bind_int(stmt, 3, item.idx);
	sqlite3_bind_int(stmt, 4, item.iCreateInfo);
	sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(item.iSeed));
	sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(item.dwBuff));
	sqlite3_bind_int(stmt, 7, static_cast<int>(sourceType));
	sqlite3_bind_int(stmt, 8, sourceLevel);
	sqlite3_bind_int(stmt, 9, static_cast<int>(state));
	sqlite3_bind_int64(stmt, 10, static_cast<sqlite3_int64>(CurrentUnixSeconds()));

	const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
	if (ok && outItemUid != nullptr) {
		*outItemUid = sqlite3_last_insert_rowid(db_);
	}
	sqlite3_finalize(stmt);
	return ok;
}

bool RealmDatabase::TransitionLedgerItemState(
    int64_t itemUid,
    uint64_t gameId,
    int64_t newOwnerCharId,
    LedgerItemState expectedState,
    LedgerItemState newState)
{
	if (db_ == nullptr || itemUid <= 0)
		return false;

	constexpr const char *kUpdateSql = R"SQL(
		UPDATE item_ledger
		SET owner_char_id = ?1,
		    state = ?2
		WHERE item_uid = ?3 AND game_id = ?4 AND state = ?5;
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kUpdateSql, -1, &stmt, nullptr) != SQLITE_OK)
		return false;

	sqlite3_bind_int64(stmt, 1, newOwnerCharId);
	sqlite3_bind_int(stmt, 2, static_cast<int>(newState));
	sqlite3_bind_int64(stmt, 3, itemUid);
	sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(gameId));
	sqlite3_bind_int(stmt, 5, static_cast<int>(expectedState));

	const int rc = sqlite3_step(stmt);
	const int changes = sqlite3_changes(db_);
	sqlite3_finalize(stmt);

	return rc == SQLITE_DONE && changes == 1;
}

bool RealmDatabase::TransitionOwnedItemToGround(
    int64_t ownerCharId,
    uint64_t gameId,
    const PkItemStruct &item,
    int64_t &outItemUid)
{
	outItemUid = 0;
	if (db_ == nullptr || ownerCharId <= 0)
		return false;

	constexpr const char *kFindSql = R"SQL(
		SELECT item_uid
		FROM item_ledger
		WHERE owner_char_id = ?1
		  AND item_idx = ?2
		  AND create_info = ?3
		  AND item_seed = ?4
		  AND item_buff = ?5
		  AND state = ?6
		LIMIT 1;
	)SQL";

	sqlite3_stmt *findStmt = nullptr;
	if (sqlite3_prepare_v2(db_, kFindSql, -1, &findStmt, nullptr) != SQLITE_OK)
		return false;

	sqlite3_bind_int64(findStmt, 1, ownerCharId);
	sqlite3_bind_int(findStmt, 2, item.idx);
	sqlite3_bind_int(findStmt, 3, item.iCreateInfo);
	sqlite3_bind_int64(findStmt, 4, static_cast<sqlite3_int64>(item.iSeed));
	sqlite3_bind_int64(findStmt, 5, static_cast<sqlite3_int64>(item.dwBuff));
	sqlite3_bind_int(findStmt, 6, static_cast<int>(LedgerItemState::InInventoryOrStash));

	int64_t candidateUid = 0;
	if (sqlite3_step(findStmt) == SQLITE_ROW) {
		candidateUid = sqlite3_column_int64(findStmt, 0);
	}
	sqlite3_finalize(findStmt);

	if (candidateUid <= 0)
		return false;

	constexpr const char *kUpdateSql = R"SQL(
		UPDATE item_ledger
		SET game_id = ?1,
		    owner_char_id = 0,
		    state = ?2
		WHERE item_uid = ?3 AND owner_char_id = ?4 AND state = ?5;
	)SQL";

	sqlite3_stmt *updStmt = nullptr;
	if (sqlite3_prepare_v2(db_, kUpdateSql, -1, &updStmt, nullptr) != SQLITE_OK)
		return false;

	sqlite3_bind_int64(updStmt, 1, static_cast<sqlite3_int64>(gameId));
	sqlite3_bind_int(updStmt, 2, static_cast<int>(LedgerItemState::OnGround));
	sqlite3_bind_int64(updStmt, 3, candidateUid);
	sqlite3_bind_int64(updStmt, 4, ownerCharId);
	sqlite3_bind_int(updStmt, 5, static_cast<int>(LedgerItemState::InInventoryOrStash));

	const int rc = sqlite3_step(updStmt);
	const int changes = sqlite3_changes(db_);
	sqlite3_finalize(updStmt);

	if (rc == SQLITE_DONE && changes == 1) {
		outItemUid = candidateUid;
		return true;
	}
	return false;
}

bool RealmDatabase::HasCharacterLedgerProvenance(int64_t charId, const PkItemStruct &item) const
{
	return CountCharacterLedgerCopies(charId, item) > 0;
}

int64_t RealmDatabase::CountCharacterLedgerCopies(int64_t charId, const PkItemStruct &item) const
{
	if (db_ == nullptr || charId <= 0)
		return 0;

	constexpr const char *kQuerySql = R"SQL(
		SELECT COUNT(*)
		FROM item_ledger
		WHERE owner_char_id = ?1
		  AND item_idx = ?2
		  AND create_info = ?3
		  AND item_seed = ?4
		  AND state = ?5;
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kQuerySql, -1, &stmt, nullptr) != SQLITE_OK)
		return 0;

	sqlite3_bind_int64(stmt, 1, charId);
	sqlite3_bind_int(stmt, 2, item.idx);
	sqlite3_bind_int(stmt, 3, item.iCreateInfo);
	sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(item.iSeed));
	sqlite3_bind_int(stmt, 5, static_cast<int>(LedgerItemState::InInventoryOrStash));

	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	}
	sqlite3_finalize(stmt);
	return count;
}

int64_t RealmDatabase::CountAccountLedgerCopies(int64_t accountId, const PkItemStruct &item) const
{
	if (db_ == nullptr || accountId <= 0)
		return 0;

	constexpr const char *kQuerySql = R"SQL(
		SELECT COUNT(*)
		FROM item_ledger l
		INNER JOIN characters c ON c.id = l.owner_char_id
		WHERE c.account_id = ?1
		  AND l.item_idx = ?2
		  AND l.create_info = ?3
		  AND l.item_seed = ?4
		  AND l.state = ?5;
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kQuerySql, -1, &stmt, nullptr) != SQLITE_OK)
		return 0;

	sqlite3_bind_int64(stmt, 1, accountId);
	sqlite3_bind_int(stmt, 2, item.idx);
	sqlite3_bind_int(stmt, 3, item.iCreateInfo);
	sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(item.iSeed));
	sqlite3_bind_int(stmt, 5, static_cast<int>(LedgerItemState::InInventoryOrStash));

	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	}
	sqlite3_finalize(stmt);
	return count;
}

int64_t RealmDatabase::CountLedgerItemsForCharacter(int64_t charId) const
{
	if (db_ == nullptr)
		return 0;

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM item_ledger WHERE owner_char_id = ?1;", -1, &stmt, nullptr) != SQLITE_OK)
		return 0;

	sqlite3_bind_int64(stmt, 1, charId);
	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	}
	sqlite3_finalize(stmt);
	return count;
}

bool RealmDatabase::ClearCharacterLedgerItems(int64_t charId)
{
	if (db_ == nullptr || charId <= 0)
		return false;

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, "DELETE FROM item_ledger WHERE owner_char_id = ?1;", -1, &stmt, nullptr) != SQLITE_OK)
		return false;

	sqlite3_bind_int64(stmt, 1, charId);
	const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
	return ok;
}

bool RealmDatabase::LogSecurityEvent(
    int64_t accountId,
    int64_t charId,
    uint64_t gameId,
    const std::string &severity,
    const std::string &category,
    const std::string &details)
{
	if (db_ == nullptr)
		return false;

	constexpr const char *kInsertSql = R"SQL(
		INSERT INTO security_audit_log
		(account_id, char_id, game_id, severity, category, details, created_at)
		VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kInsertSql, -1, &stmt, nullptr) != SQLITE_OK)
		return false;

	const uint64_t nowSec = CurrentUnixSeconds();
	sqlite3_bind_int64(stmt, 1, accountId);
	sqlite3_bind_int64(stmt, 2, charId);
	sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(gameId));
	sqlite3_bind_text(stmt, 4, severity.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, category.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, details.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(nowSec));

	const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);

	if (!logFilePath_.empty()) {
		if (std::FILE *fp = std::fopen(logFilePath_.c_str(), "a")) {
			const auto rawTime = static_cast<std::time_t>(nowSec);
			std::tm tmBuf {};
#ifdef _WIN32
			gmtime_s(&tmBuf, &rawTime);
#else
			gmtime_r(&rawTime, &tmBuf);
#endif
			char ts[32] {};
			std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S UTC", &tmBuf);
			std::fprintf(fp, "[%s] [%s] [%s] (acct=%lld char=%lld game=%llu) %s\n",
			    ts,
			    severity.c_str(),
			    category.c_str(),
			    static_cast<long long>(accountId),
			    static_cast<long long>(charId),
			    static_cast<unsigned long long>(gameId),
			    details.c_str());
			std::fclose(fp);
		}
	}

	return ok;
}

int64_t RealmDatabase::GetSecurityAuditCount() const
{
	if (db_ == nullptr)
		return 0;

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM security_audit_log;", -1, &stmt, nullptr) != SQLITE_OK)
		return 0;

	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int64(stmt, 0);
	}
	sqlite3_finalize(stmt);
	return count;
}

int64_t RealmDatabase::GetTotalAccountsCount() const
{
	if (db_ == nullptr)
		return 0;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM accounts;", -1, &stmt, nullptr) != SQLITE_OK)
		return 0;
	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

int64_t RealmDatabase::GetTotalCharactersCount() const
{
	if (db_ == nullptr)
		return 0;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM characters;", -1, &stmt, nullptr) != SQLITE_OK)
		return 0;
	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

int64_t RealmDatabase::GetTotalLedgerItemsCount() const
{
	if (db_ == nullptr)
		return 0;
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM item_ledger;", -1, &stmt, nullptr) != SQLITE_OK)
		return 0;
	int64_t count = 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

std::vector<AccountRecord> RealmDatabase::ListAccounts(size_t limit) const
{
	std::vector<AccountRecord> result;
	if (db_ == nullptr)
		return result;

	constexpr const char *kSql = R"SQL(
		SELECT id, username, password_hash, created_at, last_login_at, is_banned, COALESCE(ban_reason, '')
		FROM accounts
		ORDER BY id DESC
		LIMIT ?1;
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK)
		return result;

	sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(limit));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		AccountRecord rec {};
		rec.id = sqlite3_column_int64(stmt, 0);
		const unsigned char *uPtr = sqlite3_column_text(stmt, 1);
		rec.username = uPtr != nullptr ? reinterpret_cast<const char *>(uPtr) : "";
		const unsigned char *hPtr = sqlite3_column_text(stmt, 2);
		rec.passwordHash = hPtr != nullptr ? reinterpret_cast<const char *>(hPtr) : "";
		rec.createdAt = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
		rec.lastLoginAt = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
		rec.isBanned = sqlite3_column_int(stmt, 5) != 0;
		const unsigned char *bPtr = sqlite3_column_text(stmt, 6);
		rec.banReason = bPtr != nullptr ? reinterpret_cast<const char *>(bPtr) : "";
		result.push_back(std::move(rec));
	}
	sqlite3_finalize(stmt);
	return result;
}

std::vector<CharacterRecord> RealmDatabase::ListAllCharacters(size_t limit) const
{
	std::vector<CharacterRecord> result;
	if (db_ == nullptr)
		return result;

	constexpr const char *kSql = R"SQL(
		SELECT id, account_id, hero_slot, hero_name, hero_class, hero_level,
		       diablo_kill_level, strength, magic, dexterity, vitality,
		       experience, gold, updated_at
		FROM characters
		ORDER BY updated_at DESC, id DESC
		LIMIT ?1;
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK)
		return result;

	sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(limit));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		CharacterRecord rec {};
		rec.id = sqlite3_column_int64(stmt, 0);
		rec.accountId = sqlite3_column_int64(stmt, 1);
		rec.heroSlot = sqlite3_column_int(stmt, 2);
		const unsigned char *nPtr = sqlite3_column_text(stmt, 3);
		rec.heroName = nPtr != nullptr ? reinterpret_cast<const char *>(nPtr) : "";
		rec.heroClass = static_cast<uint8_t>(sqlite3_column_int(stmt, 4));
		rec.heroLevel = static_cast<uint8_t>(sqlite3_column_int(stmt, 5));
		rec.diabloKillLevel = static_cast<uint8_t>(sqlite3_column_int(stmt, 6));
		rec.strength = static_cast<uint8_t>(sqlite3_column_int(stmt, 7));
		rec.magic = static_cast<uint8_t>(sqlite3_column_int(stmt, 8));
		rec.dexterity = static_cast<uint8_t>(sqlite3_column_int(stmt, 9));
		rec.vitality = static_cast<uint8_t>(sqlite3_column_int(stmt, 10));
		rec.experience = sqlite3_column_int(stmt, 11);
		rec.gold = sqlite3_column_int(stmt, 12);
		rec.updatedAt = static_cast<uint64_t>(sqlite3_column_int64(stmt, 13));
		result.push_back(std::move(rec));
	}
	sqlite3_finalize(stmt);
	return result;
}

std::vector<AuditLogEntry> RealmDatabase::ListRecentAuditLogs(size_t limit) const
{
	std::vector<AuditLogEntry> result;
	if (db_ == nullptr)
		return result;

	constexpr const char *kSql = R"SQL(
		SELECT id, account_id, char_id, game_id, severity, category, details, created_at
		FROM security_audit_log
		ORDER BY id DESC
		LIMIT ?1;
	)SQL";

	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK)
		return result;

	sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(limit));
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		AuditLogEntry entry {};
		entry.id = sqlite3_column_int64(stmt, 0);
		entry.accountId = sqlite3_column_int64(stmt, 1);
		entry.charId = sqlite3_column_int64(stmt, 2);
		entry.gameId = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
		const unsigned char *sPtr = sqlite3_column_text(stmt, 4);
		entry.severity = sPtr != nullptr ? reinterpret_cast<const char *>(sPtr) : "";
		const unsigned char *cPtr = sqlite3_column_text(stmt, 5);
		entry.category = cPtr != nullptr ? reinterpret_cast<const char *>(cPtr) : "";
		const unsigned char *dPtr = sqlite3_column_text(stmt, 6);
		entry.details = dPtr != nullptr ? reinterpret_cast<const char *>(dPtr) : "";
		entry.createdAt = static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
		result.push_back(std::move(entry));
	}
	sqlite3_finalize(stmt);
	return result;
}

} // namespace devilution::realm
