/**
 * @file realm_session.hpp
 * @brief Closed Realm Game Session Registry, Difficulty Gates, Server-Oracle Loot Dispensing,
 *        and Atomic Ground-to-Inventory Item Ledger (Phase 5).
 */
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/realm_database.hpp"
#include "game/item_verifier.hpp"
#include "game/lcg.hpp"
#include "protocol/realm_protocol.hpp"

namespace devilution::realm {

/**
 * @brief Effective difficulty-scaled monster statistics (including DIFF_TORMENT 130% of Hell scaling).
 */
struct EffectiveMonsterStats {
	_monster_id baseType = MT_NZOMBIE;
	int32_t level = 1;
	int32_t minHp = 1;
	int32_t maxHp = 1;
	int32_t armorClass = 0;
	int32_t toHit = 0;
	int32_t minDamage = 1;
	int32_t maxDamage = 1;
	uint16_t treasureFlags = 0;
	uint32_t expReward = 0;
};

/**
 * @brief Active ground item tracked in a game session's server-authoritative Ground Ledger.
 */
struct GroundLedgerEntry {
	uint64_t itemUid = 0;
	uint8_t currlevel = 0;
	uint8_t tileX = 0;
	uint8_t tileY = 0;
	PkItemStruct packed {};
	TCmdGItem dropCmd {};
	uint64_t droppedAtMs = 0;
};

/**
 * @brief Per-player runtime state inside an active Closed Realm game session.
 */
struct GamePlayerRuntimeState {
	int64_t accountId = 0;
	int64_t charId = 0;
	uint8_t playerSlot = 0;
	uint8_t currentLevel = 0; // 0 = Tristram Town, 1..24 = Dungeon floors
	uint8_t lastTileX = 0;
	uint8_t lastTileY = 0;
	uint64_t lastKillTimestampMs = 0;
	uint32_t killsInRecentWindow = 0;
	uint64_t killWindowStartMs = 0;
	uint64_t verifiedExpGained = 0;
	uint64_t verifiedGoldDropped = 0;
	uint32_t highPingJitterMs = 350; // WAN TCP coalescing / high-ping jitter cushion (350ms default, 650ms in high-ping mode)
	bool hellDiabloCreditGranted = false;
	bool hellDiabloBoostBlocked = false;
	std::bitset<MaxDungeonLevel + 1> visitedFloors {};
	PlayerPack initialPack {};
	ComputedPlayerCombatStats combatStats {};
};

/**
 * @brief Per-floor server state tracking killed monsters and opened containers.
 */
struct GameFloorState {
	std::bitset<MaxFloorMonsters> killedMonsters {};
	std::bitset<MaxFloorObjects> openedObjects {};
};

/**
 * @brief Active Closed Realm game session.
 */
struct ActiveGameSession {
	uint64_t gameId = 0;
	std::string gameName;
	std::string password;
	_difficulty difficulty = DIFF_NORMAL;
	uint8_t maxPlayers = 4;
	uint8_t tickRate = DefaultClosedRealmTickRate; // Default/Max = 30 ('Fast'), Min = 20 ('Normal')
	bool isHellfire = true;
	uint32_t terrainInitSeed = 0;
	std::array<uint8_t, 32> realmSecretSalt {}; // Never leaves the server
	uint64_t createdAtMs = 0;

	std::vector<GamePlayerRuntimeState> players;
	std::array<GameFloorState, MaxDungeonLevel + 1> floors {};
	std::vector<GroundLedgerEntry> groundLedger;
};

/**
 * @brief Computes difficulty-scaled monster stats matching DX:Eternity (Normal, Nightmare, Hell, and Torment 130% of Hell).
 */
EffectiveMonsterStats CalculateEffectiveMonsterStats(
    int16_t monsterTypeOrUid,
    bool isUniqueMonster,
    _difficulty difficulty,
    bool isHellfire);

/**
 * @brief Checks if a dungeon floor transition `fromLevel -> toLevel` is topologically valid in DX:Eternity
 * (including Caves 12 <-> Hive 17 and Hell 14 <-> Crypt 21 branches).
 */
bool IsFloorTransitionValid(
    uint8_t fromLevel,
    uint8_t toLevel,
    const std::bitset<MaxDungeonLevel + 1> &visitedFloors);

class RealmSessionManager {
public:
	explicit RealmSessionManager(RealmDatabase &db);

	void SetHighPingTolerance(bool enabled)
	{
		highPingTolerance_ = enabled;
	}

	bool HighPingToleranceEnabled() const
	{
		return highPingTolerance_;
	}

	/**
	 * @brief Creates a new Closed Realm game session after enforcing difficulty & level gates.
	 */
	RealmStatus CreateGame(
	    int64_t accountId,
	    const GameCreateRequestPayload &req,
	    const std::string &remoteIp,
	    GameCreateOrJoinResponsePayload &outResp);

	/**
	 * @brief Joins an existing Closed Realm game session after enforcing difficulty, level, password, and capacity gates.
	 */
	RealmStatus JoinGame(
	    int64_t accountId,
	    const GameJoinRequestPayload &req,
	    const std::string &remoteIp,
	    GameCreateOrJoinResponsePayload &outResp);

	/**
	 * @brief Lists active public and password-protected games in the lobby, optionally filtered by
	 *        searchQuery (substring match on gameName or player heroName), difficulty, full status, or password status.
	 */
	RealmStatus ListGames(
	    GameListResponsePayload &outResp,
	    const GameListFilterRequestPayload *filter = nullptr) const;

	/**
	 * @brief Posts a message into a static lobby chat channel (`#General`, `#Trade`, `#LFG`, `#Torment`)
	 *        or processes `/w <target> <msg>`, `/who`, and `/games [query]` slash commands.
	 */
	RealmStatus SendChatMessage(
	    int64_t accountId,
	    int64_t charId,
	    const ChatSendRequestPayload &req,
	    const std::string &remoteIp,
	    ChatSendResponsePayload &outResp);

	/**
	 * @brief Polls recent messages for `channel` (merged with any private whispers addressed to `accountId`)
	 *        with `messageId > afterMessageId`.
	 */
	RealmStatus PollChatChannel(
	    int64_t accountId,
	    uint8_t rawChannelId,
	    uint64_t afterMessageId,
	    uint16_t channelUsers,
	    ChatChannelFeedResponsePayload &outResp) const;

	/**
	 * @brief Posts an official `[REALM]` / `[ADMIN]` system announcement into `channel`.
	 */
	uint64_t PostSystemChatMessage(
	    RealmChatChannel channel,
	    const std::string &senderBadge,
	    const std::string &text,
	    int64_t privateTargetAccountId = 0);

	/**
	 * @brief Removes a character/account from their active game session (and cleans up empty games).
	 */
	void LeaveGame(uint64_t gameId, int64_t charId);

	/**
	 * @brief Validates and records a player's staircase/portal/warp floor transition (`0..24`).
	 */
	RealmStatus HandleFloorTransition(
	    int64_t accountId,
	    int64_t charId,
	    const FloorTransitionRequestPayload &req,
	    const std::string &remoteIp,
	    FloorTransitionResponsePayload &outResp);

	/**
	 * @brief Server-Oracle Monster Kill & Loot Dispensing RPC (`OP_MONSTER_DROP_REQ`).
	 */
	RealmStatus HandleMonsterDrop(
	    int64_t accountId,
	    int64_t charId,
	    const MonsterDropRequestPayload &req,
	    const std::string &remoteIp,
	    MonsterOrChestDropResponsePayload &outResp);

	/**
	 * @brief Server-Oracle Chest / Container Loot Dispensing RPC (`OP_CHEST_DROP_REQ`).
	 */
	RealmStatus HandleChestDrop(
	    int64_t accountId,
	    int64_t charId,
	    const ChestDropRequestPayload &req,
	    const std::string &remoteIp,
	    MonsterOrChestDropResponsePayload &outResp);

	/**
	 * @brief Atomic Ground-to-Inventory Item Ledger Transfer RPC (`OP_ITEM_PICKUP_REQ`).
	 */
	RealmStatus HandleItemPickup(
	    int64_t accountId,
	    int64_t charId,
	    const ItemPickupRequestPayload &req,
	    const std::string &remoteIp,
	    ItemPickupResponsePayload &outResp);

	/**
	 * @brief Atomic Inventory-to-Ground Item Ledger Transfer RPC (`OP_ITEM_DROP_REQ`).
	 */
	RealmStatus HandleItemDrop(
	    int64_t accountId,
	    int64_t charId,
	    const ItemDropRequestPayload &req,
	    const std::string &remoteIp,
	    ItemDropResponsePayload &outResp);

	/**
	 * @brief Returns read-only pointer to an active game session (for Phase 6 save auditing & Phase 7 admin telemetry).
	 */
	const ActiveGameSession *FindGame(uint64_t gameId) const;

	/**
	 * @brief Returns read-only pointer to a player's runtime session state in `gameId`.
	 */
	const GamePlayerRuntimeState *FindPlayerInGame(uint64_t gameId, int64_t charId) const;

	size_t ActiveGameCount() const
	{
		return games_.size();
	}

private:
	RealmStatus CheckDifficultyEntryGate(
	    const CharacterRecord &charRecord,
	    _difficulty difficulty,
	    uint8_t maxPlayers,
	    const std::string &remoteIp) const;

	uint32_t DeriveOracleDropSeed(
	    const ActiveGameSession &game,
	    uint8_t currlevel,
	    uint16_t entityIndex,
	    uint8_t entityTag) const;

	void RollOracleDungeonItem(
	    LcgEngine &lcg,
	    const ServerPlayerContext &playerCtx,
	    int itemLevel,
	    int uper,
	    bool onlyGood,
	    ServerItem &outItem) const;

	bool RecordDispensedGroundItem(
	    ActiveGameSession &game,
	    uint8_t currlevel,
	    uint8_t tileX,
	    uint8_t tileY,
	    uint8_t playerSlot,
	    LedgerItemSource sourceType,
	    const ServerItem &serverItem,
	    OracleDroppedItemEntry &outEntry);

	RealmDatabase &db_;
	bool highPingTolerance_ = true;
	uint64_t nextGameId_ = 1;
	uint64_t nextChatMessageId_ = 1;
	std::unordered_map<uint64_t, ActiveGameSession> games_;
	std::array<std::vector<ChatMessageEntry>, static_cast<size_t>(RealmChatChannel::COUNT)> channelHistory_ {};
	std::unordered_map<int64_t, std::vector<ChatMessageEntry>> accountWhisperInbox_;
};

} // namespace devilution::realm
