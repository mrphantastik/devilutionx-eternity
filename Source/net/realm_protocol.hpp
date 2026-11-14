/**
 * @file realm_protocol.hpp
 * @brief Binary protocol framing, opcodes, and wire structures for DX:Eternity Closed Realm (dx-eternity-realm).
 *        Matches dx-eternity-realm Server/protocol/realm_protocol.hpp 1:1.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace devilution {
namespace realm {

constexpr uint32_t ProtocolMagic = 0x52455844U; // "DXER" in little-endian
constexpr uint16_t ProtocolVersion = 1U;
constexpr size_t PacketMacSize = 16U;           // 128-bit BLAKE2b authentication tag
constexpr uint32_t MaxPayloadSize = 65536U;     // 64 KB max packet payload
constexpr uint16_t DefaultRealmPort = 6112U;
constexpr uint32_t ClientBuildVersion = 0x00000704U; // Beta v0.7d (0.7.4)

constexpr size_t MaxHeroesPerAccount = 12U;
constexpr size_t MaxCharactersPerAccount = MaxHeroesPerAccount;
constexpr size_t MaxStashItems = 400U;
constexpr size_t MaxPlayersPerGame = 4U;
constexpr size_t MaxListedGames = 32U;
constexpr size_t MaxDropsPerKill = 4U;
constexpr size_t MaxFloorMonsters = 256U;
constexpr size_t MaxFloorObjects = 128U;
constexpr size_t MaxChatMessageLength = 160U;
constexpr size_t MaxChatHistoryBatch = 16U;

enum class RealmOpcode : uint16_t {
	// System / Transport (0x00xx)
	OP_PING = 0x0001,
	OP_PONG = 0x0002,
	OP_ERROR_RESPONSE = 0x00FF,

	// Phase 2: Authentication & Binary Attestation (0x01xx)
	OP_AUTH_CHALLENGE_REQ = 0x0101,
	OP_AUTH_CHALLENGE_RESP = 0x0102,
	OP_AUTH_REGISTER_REQ = 0x0103,
	OP_AUTH_LOGIN_REQ = 0x0104,
	OP_AUTH_RESULT = 0x0105,
	OP_ATTEST_HANDSHAKE_REQ = 0x0106,
	OP_ATTEST_HANDSHAKE_RESP = 0x0107,
	OP_AUTH_RESUME_REQ = 0x0108,

	// Phase 3: Server-Vault Hero & Stash Persistence (0x02xx)
	OP_HERO_LIST_REQ = 0x0201,
	OP_HERO_LIST_RESP = 0x0202,
	OP_HERO_CREATE_REQ = 0x0203,
	OP_HERO_CREATE_RESP = 0x0204,
	OP_HERO_DELETE_REQ = 0x0205,
	OP_HERO_DELETE_RESP = 0x0206,
	OP_HERO_LOAD_REQ = 0x0207,
	OP_HERO_LOAD_RESP = 0x0208,
	OP_HERO_SAVE_REQ = 0x0209,
	OP_HERO_SAVE_RESP = 0x020A,
	OP_STASH_LOAD_REQ = 0x020B,
	OP_STASH_LOAD_RESP = 0x020C,
	OP_STASH_SAVE_REQ = 0x020D,
	OP_STASH_SAVE_RESP = 0x020E,

	// Phase 5: Game Session Registry & Server-Oracle Loot Dispensing (0x03xx)
	OP_GAME_CREATE_REQ = 0x0301,
	OP_GAME_CREATE_RESP = 0x0302,
	OP_GAME_JOIN_REQ = 0x0303,
	OP_GAME_JOIN_RESP = 0x0304,
	OP_GAME_LIST_REQ = 0x0305,
	OP_GAME_LIST_RESP = 0x0306,
	OP_FLOOR_TRANSITION_REQ = 0x0307,
	OP_FLOOR_TRANSITION_RESP = 0x0308,
	OP_MONSTER_DROP_REQ = 0x0310,
	OP_MONSTER_DROP_RESP = 0x0311,
	OP_CHEST_DROP_REQ = 0x0312,
	OP_CHEST_DROP_RESP = 0x0313,
	OP_ITEM_PICKUP_REQ = 0x0314,
	OP_ITEM_PICKUP_RESP = 0x0315,
	OP_ITEM_DROP_REQ = 0x0316,
	OP_ITEM_DROP_RESP = 0x0317,

	// Lobby Static Chat Channels (General / Trade / LFG / Torment) & Whispers (0x04xx)
	OP_CHAT_JOIN_CHANNEL_REQ = 0x0401,
	OP_CHAT_JOIN_CHANNEL_RESP = 0x0402,
	OP_CHAT_SEND_REQ = 0x0403,
	OP_CHAT_SEND_RESP = 0x0404,
	OP_CHAT_POLL_REQ = 0x0405,
	OP_CHAT_POLL_RESP = 0x0406,

	// Phase 6.5: Eternity Warden Runtime Memory, Module & Speedhack Attestation (0x05xx)
	OP_WARDEN_CHALLENGE_REQ = 0x0501,
	OP_WARDEN_CHALLENGE_RESP = 0x0502,
	OP_WARDEN_VERDICT_RESP = 0x0503,
};

enum class RealmStatus : uint16_t {
	OK = 0,
	ERR_MALFORMED_PACKET = 1,
	ERR_MAC_VERIFICATION_FAILED = 2,
	ERR_REPLAY_DETECTED = 3,
	ERR_ATTESTATION_FAILED = 4,
	ERR_AUTH_INVALID_CREDENTIALS = 5,
	ERR_AUTH_ACCOUNT_EXISTS = 6,
	ERR_AUTH_ACCOUNT_BANNED = 7,
	ERR_UNAUTHORIZED = 8,
	ERR_HERO_LIMIT_REACHED = 9,
	ERR_HERO_NAME_TAKEN = 10,
	ERR_HERO_NOT_FOUND = 11,
	ERR_TORMENT_LOCKED_HELL_REQUIRED = 12,
	ERR_TORMENT_LEVEL_35_REQUIRED = 13,
	ERR_DUPLICATE_KILL_OR_CHEST = 14,
	ERR_ITEM_NOT_ON_GROUND = 15,
	ERR_ANOMALY_TTK_VIOLATION = 16,
	ERR_ANOMALY_CADENCE_VIOLATION = 17,
	ERR_ANOMALY_FLOOR_GATE_VIOLATION = 18,
	ERR_SAVE_AUDIT_FAILED = 19,
	ERR_INTERNAL_DATABASE = 20,
	ERR_GAME_NOT_FOUND = 21,
	ERR_GAME_FULL = 22,
	ERR_GAME_PASSWORD_INVALID = 23,
	ERR_GAME_NAME_TAKEN = 24,
	ERR_DIFFICULTY_LEVEL_LOCKED = 25,
	ERR_CHAT_RATE_LIMITED = 26,
	ERR_CHAT_INVALID_CHANNEL = 27,
	ERR_CHAT_EMPTY_MESSAGE = 28,
	ERR_WARDEN_INTEGRITY_VIOLATION = 29,
	ERR_WARDEN_SPEEDHACK_DETECTED = 30,
};

enum class WardenMemoryTableId : uint8_t {
	AllItemsList = 0,
	UniqueItems = 1,
	SpellsData = 2,
	MonstersData = 3,
	COUNT = 4,
};

enum WardenClientAnomalyFlags : uint32_t {
	WARDEN_FLAG_CLEAN = 0,
	WARDEN_FLAG_DEBUGGER_PRESENT = 1U << 0,
	WARDEN_FLAG_HARDWARE_BREAKPOINT = 1U << 1,
	WARDEN_FLAG_SUSPICIOUS_DLL = 1U << 2,
	WARDEN_FLAG_XOR_CANARY_MISMATCH = 1U << 3,
	WARDEN_FLAG_TEXT_SECTION_MODIFIED = 1U << 4,
};

enum class RealmChatChannel : uint8_t {
	General = 0,
	Trade = 1,
	LFG = 2,
	Torment = 3,
	COUNT = 4,
};

#pragma pack(push, 1)

struct PkItemStruct {
	uint32_t iSeed;
	uint16_t iCreateInfo;
	uint16_t idx;
	uint8_t bId;
	uint8_t bDur;
	uint8_t bMDur;
	uint8_t bCh;
	uint8_t bMCh;
	uint16_t wValue;
	uint32_t dwBuff;
};
static_assert(sizeof(PkItemStruct) == 19, "PkItemStruct must be exactly 19 bytes");

struct RealmPlayerPack {
	uint32_t dwLowDateTime;
	uint32_t dwHighDateTime;
	int8_t destAction;
	int8_t destParam1;
	int8_t destParam2;
	uint8_t plrlevel;
	uint8_t px;
	uint8_t py;
	uint8_t targx;
	uint8_t targy;
	char pName[32];
	uint8_t pClass;
	uint8_t pBaseStr;
	uint8_t pBaseMag;
	uint8_t pBaseDex;
	uint8_t pBaseVit;
	int8_t pLevel;
	uint8_t pStatPts;
	int32_t pExperience;
	int32_t pGold;
	int32_t pHPBase;
	int32_t pMaxHPBase;
	int32_t pManaBase;
	int32_t pMaxManaBase;
	int8_t pSplLvl[37];
	uint64_t pMemSpells;
	PkItemStruct InvBody[7];
	PkItemStruct InvList[40];
	int8_t _pNumInv;
	int8_t InvGrid[40];
	PkItemStruct SpdList[8];
	int8_t pTownWarps;
	int8_t pDungMsgs;
	int8_t pLvlLoad;
	uint8_t pBattleNet;
	uint8_t pManaShield;
	int8_t pDungMsgs2;
	uint8_t bIsHellfire;
	uint8_t bReserved;
	uint16_t wReflections;
	int16_t wReserved2;
	int8_t pSplLvl2[10];
	int16_t wReserved8;
	uint32_t pDiabloKillLevel;
	uint32_t pDifficulty;
	uint32_t pDamAcFlags;
	uint8_t bReserved2[16];
};
static_assert(sizeof(RealmPlayerPack) == 1262, "RealmPlayerPack must be 1262 bytes");

struct RealmCmdGItem {
	uint8_t bCmd;
	uint8_t bMaster;
	uint8_t bPnum;
	uint8_t bCursitem;
	uint8_t bLevel;
	uint8_t x;
	uint8_t y;
	uint16_t wIndx;
	uint16_t wCI;
	uint32_t dwSeed;
	uint8_t bId;
	uint8_t bDur;
	uint8_t bMDur;
	uint8_t bCh;
	uint8_t bMCh;
	uint16_t wValue;
	uint32_t dwBuff;
	uint32_t dwTime;
};
static_assert(sizeof(RealmCmdGItem) == 30, "RealmCmdGItem must be 30 bytes");

struct RealmPacketHeader {
	uint32_t magic;
	uint16_t version;
	uint16_t opcode;
	uint32_t payloadLength;
	uint32_t sequenceNumber;
	uint8_t mac[PacketMacSize];
};
static_assert(sizeof(RealmPacketHeader) == 32, "RealmPacketHeader must be 32 bytes");

struct PingPongPayload {
	uint64_t clientTimestampMs;
	uint64_t serverTimestampMs;
};
static_assert(sizeof(PingPongPayload) == 16, "PingPongPayload must be 16 bytes");

struct ErrorResponsePayload {
	uint16_t requestOpcode;
	uint16_t statusCode;
	char message[124];
};
static_assert(sizeof(ErrorResponsePayload) == 128, "ErrorResponsePayload must be 128 bytes");

struct AuthChallengeResponsePayload {
	uint8_t serverNonce[32];
	uint32_t serverProtocolVersion;
};
static_assert(sizeof(AuthChallengeResponsePayload) == 36, "AuthChallengeResponsePayload must be 36 bytes");

struct AttestHandshakeRequestPayload {
	uint8_t clientNonce[32];
	uint8_t clientTextSectionHash[32];
	uint32_t clientBuildVersion;
	uint8_t attestationHmac[32];
};
static_assert(sizeof(AttestHandshakeRequestPayload) == 100, "AttestHandshakeRequestPayload must be 100 bytes");

struct AttestHandshakeResponsePayload {
	uint16_t statusCode;
	uint16_t reserved;
};
static_assert(sizeof(AttestHandshakeResponsePayload) == 4, "AttestHandshakeResponsePayload must be 4 bytes");

struct AuthCredentialsRequestPayload {
	char username[32];
	char password[64];
};
static_assert(sizeof(AuthCredentialsRequestPayload) == 96, "AuthCredentialsRequestPayload must be 96 bytes");

struct AuthResumeRequestPayload {
	uint64_t accountId;
	uint8_t sessionToken[32];
};
static_assert(sizeof(AuthResumeRequestPayload) == 40, "AuthResumeRequestPayload must be 40 bytes");

struct AuthResultPayload {
	uint16_t statusCode;
	uint16_t reserved;
	uint64_t accountId;
	uint8_t sessionToken[32];
	char message[64];
};
static_assert(sizeof(AuthResultPayload) == 108, "AuthResultPayload must be 108 bytes");

struct HeroSummaryInfo {
	uint64_t charId;
	char name[32];
	uint8_t heroClass;
	uint8_t level;
	uint8_t diabloKillLevel;
	uint8_t bIsHellfire;
	uint8_t baseStr;
	uint8_t baseMag;
	uint8_t baseDex;
	uint8_t baseVit;
};
static_assert(sizeof(HeroSummaryInfo) == 48, "HeroSummaryInfo must be 48 bytes");

struct HeroListResponsePayload {
	uint16_t statusCode;
	uint16_t heroCount;
	HeroSummaryInfo heroes[MaxHeroesPerAccount];
};
static_assert(sizeof(HeroListResponsePayload) == 4 + 48 * MaxHeroesPerAccount, "HeroListResponsePayload size check");

struct HeroCreateRequestPayload {
	char name[32];
	uint8_t heroClass;
	uint8_t bIsHellfire;
	uint16_t reserved;
};
static_assert(sizeof(HeroCreateRequestPayload) == 36, "HeroCreateRequestPayload must be 36 bytes");

struct HeroCreateResponsePayload {
	uint16_t statusCode;
	uint16_t reserved;
	HeroSummaryInfo summary;
	RealmPlayerPack pack;
};
static_assert(sizeof(HeroCreateResponsePayload) == 4 + 48 + 1262, "HeroCreateResponsePayload must be 1314 bytes");

struct HeroDeleteRequestPayload {
	uint64_t charId;
};
static_assert(sizeof(HeroDeleteRequestPayload) == 8, "HeroDeleteRequestPayload must be 8 bytes");

struct HeroDeleteResponsePayload {
	uint16_t statusCode;
	uint16_t reserved;
	uint64_t charId;
};
static_assert(sizeof(HeroDeleteResponsePayload) == 12, "HeroDeleteResponsePayload must be 12 bytes");

struct HeroLoadRequestPayload {
	uint64_t charId;
};
static_assert(sizeof(HeroLoadRequestPayload) == 8, "HeroLoadRequestPayload must be 8 bytes");

struct HeroLoadResponsePayload {
	uint16_t statusCode;
	uint16_t reserved;
	uint64_t charId;
	RealmPlayerPack pack;
};
static_assert(sizeof(HeroLoadResponsePayload) == 12 + 1262, "HeroLoadResponsePayload must be 1274 bytes");

struct HeroSaveRequestPayload {
	uint64_t charId;
	RealmPlayerPack pack;
};
static_assert(sizeof(HeroSaveRequestPayload) == 8 + 1262, "HeroSaveRequestPayload must be 1270 bytes");

struct HeroSaveResponsePayload {
	uint16_t statusCode;
	uint16_t reserved;
	uint64_t charId;
};
static_assert(sizeof(HeroSaveResponsePayload) == 12, "HeroSaveResponsePayload must be 12 bytes");

struct StashDataHeader {
	uint16_t statusCode;
	uint16_t itemCount;
	uint32_t stashGold;
};
static_assert(sizeof(StashDataHeader) == 8, "StashDataHeader must be 8 bytes");

struct StashSaveResponsePayload {
	uint16_t statusCode;
	uint16_t savedItemCount;
	uint32_t stashGold;
};
static_assert(sizeof(StashSaveResponsePayload) == 8, "StashSaveResponsePayload must be 8 bytes");

struct GameCreateRequestPayload {
	uint64_t charId;
	char gameName[32];
	char password[32];
	uint8_t difficulty;
	uint8_t maxPlayers;
	uint8_t bIsHellfire;
	uint8_t reserved;
};
static_assert(sizeof(GameCreateRequestPayload) == 76, "GameCreateRequestPayload must be 76 bytes");

struct GameJoinRequestPayload {
	uint64_t charId;
	char gameName[32];
	char password[32];
};
static_assert(sizeof(GameJoinRequestPayload) == 72, "GameJoinRequestPayload must be 72 bytes");

struct GameCreateOrJoinResponsePayload {
	uint16_t statusCode;
	uint8_t difficulty;
	uint8_t playerSlot;
	uint64_t gameId;
	uint32_t terrainInitSeed;
	uint8_t bIsHellfire;
	uint8_t maxPlayers;
	uint16_t reserved;
};
static_assert(sizeof(GameCreateOrJoinResponsePayload) == 20, "GameCreateOrJoinResponsePayload must be 20 bytes");

struct GameListFilterRequestPayload {
	char searchQuery[32];
	uint8_t difficultyFilter;
	uint8_t hideFullGames;
	uint8_t hidePasswordedGames;
	uint8_t reserved;
};
static_assert(sizeof(GameListFilterRequestPayload) == 36, "GameListFilterRequestPayload must be 36 bytes");

struct GameLobbyInfo {
	uint64_t gameId;
	char gameName[32];
	uint8_t difficulty;
	uint8_t currentPlayers;
	uint8_t maxPlayers;
	uint8_t passwordProtected;
};
static_assert(sizeof(GameLobbyInfo) == 44, "GameLobbyInfo must be 44 bytes");

struct GameListResponsePayload {
	uint16_t statusCode;
	uint16_t gameCount;
	GameLobbyInfo games[MaxListedGames];
};
static_assert(sizeof(GameListResponsePayload) == 4 + 44 * MaxListedGames, "GameListResponsePayload size check");

struct FloorTransitionRequestPayload {
	uint64_t gameId;
	uint8_t fromLevel;
	uint8_t toLevel;
	uint8_t tileX;
	uint8_t tileY;
};
static_assert(sizeof(FloorTransitionRequestPayload) == 12, "FloorTransitionRequestPayload must be 12 bytes");

struct FloorTransitionResponsePayload {
	uint16_t statusCode;
	uint8_t currentLevel;
	uint8_t reserved;
};
static_assert(sizeof(FloorTransitionResponsePayload) == 4, "FloorTransitionResponsePayload must be 4 bytes");

struct MonsterDropRequestPayload {
	uint64_t gameId;
	uint8_t currlevel;
	uint8_t tileX;
	uint8_t tileY;
	uint8_t isUniqueMonster;
	uint16_t monsterIndex;
	int16_t monsterTypeOrUid;
	int32_t totalDamageDealt;
	int32_t maxSingleHitDealt;
	uint32_t combatDurationMs;
};
static_assert(sizeof(MonsterDropRequestPayload) == 28, "MonsterDropRequestPayload must be 28 bytes");

struct ChestDropRequestPayload {
	uint64_t gameId;
	uint8_t currlevel;
	uint8_t tileX;
	uint8_t tileY;
	uint8_t chestQuality;
	uint16_t objectIndex;
	uint16_t reserved;
};
static_assert(sizeof(ChestDropRequestPayload) == 16, "ChestDropRequestPayload must be 16 bytes");

struct OracleDroppedItemEntry {
	uint64_t itemUid;
	RealmCmdGItem dropCmd;
	PkItemStruct packed;
	uint8_t reserved;
};
static_assert(sizeof(OracleDroppedItemEntry) == 58, "OracleDroppedItemEntry must be 58 bytes");

struct MonsterOrChestDropResponsePayload {
	uint16_t statusCode;
	uint8_t dropCount;
	uint8_t currlevel;
	uint32_t awardedExp;
	OracleDroppedItemEntry drops[MaxDropsPerKill];
};
static_assert(sizeof(MonsterOrChestDropResponsePayload) == 8 + 58 * MaxDropsPerKill, "MonsterOrChestDropResponsePayload must be 240 bytes");

struct ItemPickupRequestPayload {
	uint64_t gameId;
	uint64_t itemUid;
	uint8_t currlevel;
	uint8_t tileX;
	uint8_t tileY;
	uint8_t reserved;
	uint32_t iSeed;
	uint16_t idx;
	uint16_t iCreateInfo;
};
static_assert(sizeof(ItemPickupRequestPayload) == 28, "ItemPickupRequestPayload must be 28 bytes");

struct ItemPickupResponsePayload {
	uint16_t statusCode;
	uint16_t reserved;
	uint64_t itemUid;
	PkItemStruct item;
};
static_assert(sizeof(ItemPickupResponsePayload) == 31, "ItemPickupResponsePayload must be 31 bytes");

struct ItemDropRequestPayload {
	uint64_t gameId;
	uint8_t currlevel;
	uint8_t tileX;
	uint8_t tileY;
	uint8_t reserved;
	PkItemStruct item;
};
static_assert(sizeof(ItemDropRequestPayload) == 31, "ItemDropRequestPayload must be 31 bytes");

struct ItemDropResponsePayload {
	uint16_t statusCode;
	uint16_t reserved;
	OracleDroppedItemEntry dropped;
};
static_assert(sizeof(ItemDropResponsePayload) == 62, "ItemDropResponsePayload must be 62 bytes");

struct WardenChallengePayload {
	uint64_t challengeId;
	uint8_t wardenNonce[16];
	uint8_t tableId;
	uint8_t reserved;
	uint16_t sliceIndex;
};
static_assert(sizeof(WardenChallengePayload) == 28, "WardenChallengePayload must be 28 bytes");

struct WardenResponsePayload {
	uint64_t challengeId;
	uint64_t clientMonotonicMs;
	uint32_t anomalyFlags;
	uint8_t sliceBlake2b[32];
};
static_assert(sizeof(WardenResponsePayload) == 52, "WardenResponsePayload must be 52 bytes");

struct WardenVerdictPayload {
	uint16_t statusCode;
	uint16_t reserved;
	uint32_t driftPermille;
	uint64_t challengeId;
};
static_assert(sizeof(WardenVerdictPayload) == 16, "WardenVerdictPayload must be 16 bytes");

#pragma pack(pop)

inline const char *StatusToString(RealmStatus status) noexcept
{
	switch (status) {
	case RealmStatus::OK:
		return "OK";
	case RealmStatus::ERR_MALFORMED_PACKET:
		return "Malformed packet payload";
	case RealmStatus::ERR_MAC_VERIFICATION_FAILED:
		return "Packet MAC verification failed";
	case RealmStatus::ERR_REPLAY_DETECTED:
		return "Packet replay or out-of-order sequence detected";
	case RealmStatus::ERR_ATTESTATION_FAILED:
		return "Client binary attestation failed";
	case RealmStatus::ERR_AUTH_INVALID_CREDENTIALS:
		return "Invalid username or password";
	case RealmStatus::ERR_AUTH_ACCOUNT_EXISTS:
		return "Account username already registered";
	case RealmStatus::ERR_AUTH_ACCOUNT_BANNED:
		return "Account is suspended";
	case RealmStatus::ERR_UNAUTHORIZED:
		return "Session not authenticated";
	case RealmStatus::ERR_HERO_LIMIT_REACHED:
		return "Maximum characters per account reached (12)";
	case RealmStatus::ERR_HERO_NAME_TAKEN:
		return "Character name is already taken on this realm";
	case RealmStatus::ERR_HERO_NOT_FOUND:
		return "Character not found in Server-Vault";
	case RealmStatus::ERR_TORMENT_LOCKED_HELL_REQUIRED:
		return "Torment difficulty requires defeating Diablo on Hell difficulty";
	case RealmStatus::ERR_TORMENT_LEVEL_35_REQUIRED:
		return "Multiplayer Torment difficulty requires Character Level 35+";
	case RealmStatus::ERR_DUPLICATE_KILL_OR_CHEST:
		return "Monster or chest loot already dispensed on this floor";
	case RealmStatus::ERR_ITEM_NOT_ON_GROUND:
		return "Item is no longer on the ground";
	case RealmStatus::ERR_ANOMALY_TTK_VIOLATION:
		return "Combat damage or TTK exceeded character ceiling";
	case RealmStatus::ERR_ANOMALY_CADENCE_VIOLATION:
		return "Kill frequency or travel distance exceeded movement ceiling";
	case RealmStatus::ERR_ANOMALY_FLOOR_GATE_VIOLATION:
		return "Floor progression gate violation";
	case RealmStatus::ERR_SAVE_AUDIT_FAILED:
		return "Character save rejected by Server-Vault anti-cheat audit";
	case RealmStatus::ERR_INTERNAL_DATABASE:
		return "Internal realm database error";
	case RealmStatus::ERR_GAME_NOT_FOUND:
		return "Game not found or has already closed";
	case RealmStatus::ERR_GAME_FULL:
		return "Game is full (4/4 players)";
	case RealmStatus::ERR_GAME_PASSWORD_INVALID:
		return "Incorrect game password";
	case RealmStatus::ERR_GAME_NAME_TAKEN:
		return "A game with that name already exists";
	case RealmStatus::ERR_DIFFICULTY_LEVEL_LOCKED:
		return "Character does not meet minimum level for this difficulty";
	case RealmStatus::ERR_CHAT_RATE_LIMITED:
		return "Chat rate limit exceeded";
	case RealmStatus::ERR_CHAT_INVALID_CHANNEL:
		return "Invalid lobby chat channel";
	case RealmStatus::ERR_CHAT_EMPTY_MESSAGE:
		return "Chat message cannot be empty";
	case RealmStatus::ERR_WARDEN_INTEGRITY_VIOLATION:
		return "Warden runtime memory or module integrity violation";
	case RealmStatus::ERR_WARDEN_SPEEDHACK_DETECTED:
		return "Warden Speedhack clock-skew violation";
	default:
		return "Unknown realm error";
	}
}

} // namespace realm
} // namespace devilution
