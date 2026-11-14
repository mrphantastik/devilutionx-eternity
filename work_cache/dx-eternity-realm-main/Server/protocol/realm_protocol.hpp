/**
 * @file realm_protocol.hpp
 * @brief Binary protocol framing, opcodes, and shared DX:Eternity structures for dx-eternity-realm.
 *
 * Maintains 1:1 binary layout parity with DevilutionX: Eternity (Beta v0.7d+, 1.5.4 fork).
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace devilution {

/**
 * @brief DX:Eternity difficulty tiers (matches Source/levels/gendung.h 1:1).
 */
enum _difficulty : uint8_t {
	DIFF_NORMAL = 0,
	DIFF_NIGHTMARE = 1,
	DIFF_HELL = 2,
	DIFF_TORMENT = 3,
	DIFF_LAST = DIFF_TORMENT,
};

/**
 * @brief DX:Eternity playable hero classes (matches Source/player.h 1:1, including Classic Female Barbarian).
 */
enum class HeroClass : uint8_t {
	Warrior = 0,
	Rogue = 1,
	Sorcerer = 2,
	Monk = 3,
	Bard = 4,
	Barbarian = 5,
	LAST = Barbarian,
};

/**
 * @brief DX:Eternity item creation flags stored in Item::dwBuff / PkItemStruct::dwBuff (matches Source/items.h 1:1).
 */
enum icreateinfo_flag2 : uint32_t {
	CF_HELLFIRE = 1U << 0,
	CF_TORMENT = 1U << 1, // Item generated on DIFF_TORMENT: rolls maximum possible stats in RndPL, iMaxAC, sStaffMax, _iMaxDur
};

/**
 * @brief Dungeon floor range (0 = Tristram Town, 1..4 = Cathedral, 5..8 = Catacombs,
 * 9..12 = Caves, 13..16 = Hell, 17..20 = Hive, 21..24 = Crypt).
 */
constexpr uint8_t MaxDungeonLevel = 24;

/**
 * @brief Minimumablo kill level (`pDiabloKillLevel`) and multiplayer character level required to enter DIFF_TORMENT.
 */
constexpr uint8_t TormentRequiredDiabloKillLevel = 3; // Must have slain Diablo on Hell difficulty
constexpr uint8_t TormentRequiredMultiplayerLevel = 35;

#pragma pack(push, 1)

/**
 * @brief Packed 19-byte item network/save structure (matches DevilutionX PkItemStruct in Source/pack.h 1:1).
 */
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
static_assert(sizeof(PkItemStruct) == 19, "PkItemStruct must be exactly 19 bytes for 1:1 client parity");

/**
 * @brief Packed 1262-byte character save/network structure (matches DevilutionX PlayerPack in Source/pack.h 1:1).
 */
struct PlayerPack {
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
	uint8_t pDungMsgs2;
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
static_assert(sizeof(PlayerPack) == 1262, "PlayerPack must be exactly 1262 bytes for 1:1 client parity");

/**
 * @brief Ground item drop command structure (matches DevilutionX TCmdGItem in Source/msg.h 1:1).
 */
struct TCmdGItem {
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
static_assert(sizeof(TCmdGItem) == 30, "TCmdGItem must be exactly 30 bytes for 1:1 client parity");

#pragma pack(pop)

namespace realm {

constexpr uint32_t ProtocolMagic = 0x52455844U; // "DXER" in little-endian
constexpr uint16_t ProtocolVersion = 1U;
constexpr size_t PacketMacSize = 16U;           // 128-bit BLAKE2b authentication tag
constexpr uint32_t MaxPayloadSize = 65536U;     // 64 KB max packet payload
constexpr uint16_t DefaultRealmPort = 6112U;

/**
 * @brief Closed Realm Wire Opcodes.
 */
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

/**
 * @brief Standardized realm error status codes.
 */
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

/**
 * @brief Target read-only engine data table challenged by Eternity Warden (`MEM_CHECK`).
 */
enum class WardenMemoryTableId : uint8_t {
	AllItemsList = 0, // `AllItemsList[0..169]` item base stats, prices & requirements
	UniqueItems = 1,  // `UniqueItems[0..112]` unique item powers & parameters
	SpellsData = 2,   // `SpellsData[0..51]` spell mana costs, damage bounds & flags
	MonstersData = 3, // `MonstersData[0..137]` monster HP, AC, damage & XP tables
	COUNT = 4,
};

/**
 * @brief Client-reported user-mode process anomaly flags in `WardenResponsePayload`.
 */
enum WardenClientAnomalyFlags : uint32_t {
	WARDEN_FLAG_CLEAN = 0,
	WARDEN_FLAG_DEBUGGER_PRESENT = 1U << 0,      // `IsDebuggerPresent` / `CheckRemoteDebuggerPresent`
	WARDEN_FLAG_HARDWARE_BREAKPOINT = 1U << 1,   // CPU `CONTEXT::Dr0..Dr3 != 0`
	WARDEN_FLAG_SUSPICIOUS_DLL = 1U << 2,        // `speedhack-x86_64.dll`, `vehdebug-x86_64.dll`, `cehook64.dll`
	WARDEN_FLAG_XOR_CANARY_MISMATCH = 1U << 3,   // Rolling-XOR shadow memory canary mismatch (`_pGold`, `_pBaseStr`, etc.)
	WARDEN_FLAG_TEXT_SECTION_MODIFIED = 1U << 4, // In-memory PE `.text` code section hash modified
};

/**
 * @brief Static Closed-Realm Lobby Chat Channels.
 */
enum class RealmChatChannel : uint8_t {
	General = 0, // #General — Global Realm discussion
	Trade = 1,   // #Trade   — Item trading & price checks
	LFG = 2,     // #LFG     — Looking For Group / Dungeon & Diablo runs
	Torment = 3, // #Torment — High-level Endgame / Torment discussion
	COUNT = 4,
};

/**
 * @brief Bitflags attached to a `ChatMessageEntry`.
 */
enum RealmChatMessageFlags : uint8_t {
	CHAT_FLAG_NONE = 0,
	CHAT_FLAG_WHISPER = 1U << 0,       // Direct private `/w <target>` message
	CHAT_FLAG_SYSTEM = 1U << 1,        // Server / Admin announcement or `/who` `/games` reply
	CHAT_FLAG_TORMENT_CROWN = 1U << 2, // Server-verified `diabloKillLevel >= 3` prestige badge
};

constexpr size_t MaxHeroesPerAccount = 12U;
constexpr size_t MaxStashItems = 100U;               // 1 single 10x10 stash page (max 100 1x1 items)
constexpr uint16_t MaxGoldStackValue = 5000U;        // Traditional physical gold stack cap (GOLD_MAX_LIMIT)
constexpr int32_t MaxHeroInventoryGold = 200000;     // 40 inventory slots * 5,000g physical stacks
constexpr uint32_t MaxSinglePageStashGold = 300000U; // Hard limit: 300,000 gold in 1-page stash (max 60 x 5,000g stacks)
constexpr uint8_t MinClosedRealmTickRate = 20U;      // 'Normal' DX multiplayer speed (20 ticks/sec)
constexpr uint8_t DefaultClosedRealmTickRate = 30U;  // 'Fast' DX multiplayer speed (30 ticks/sec, default)
constexpr uint8_t MaxClosedRealmTickRate = 30U;      // 'Fast' DX multiplayer speed (30 ticks/sec, hard cap; Faster=40/Fastest=50 forbidden)
constexpr size_t MaxPlayersPerGame = 4U;
constexpr size_t MaxListedGames = 32U;
constexpr size_t MaxDropsPerKill = 4U; // 1 normal drop + up to 3 Hell/Torment Diablo exclusive uniques
constexpr size_t MaxFloorMonsters = 256U;
constexpr size_t MaxFloorObjects = 128U;
constexpr size_t MaxChatMessageLength = 160U;
constexpr size_t MaxChatHistoryBatch = 16U;

#pragma pack(push, 1)

/**
 * @brief 32-byte wire header prefixing every Closed Realm TCP frame.
 */
struct RealmPacketHeader {
	uint32_t magic;            // ProtocolMagic (0x52455844 "DXER")
	uint16_t version;          // ProtocolVersion (1)
	uint16_t opcode;           // RealmOpcode
	uint32_t payloadLength;    // 0 .. MaxPayloadSize
	uint32_t sequenceNumber;   // Monotonic per-direction counter (1, 2, 3...)
	uint8_t mac[PacketMacSize]; // Keyed BLAKE2b-128 tag over header(with mac=0) + payload
};
static_assert(sizeof(RealmPacketHeader) == 32, "RealmPacketHeader must be exactly 32 bytes");

/**
 * @brief Payload for OP_PING / OP_PONG.
 */
struct PingPongPayload {
	uint64_t clientTimestampMs;
	uint64_t serverTimestampMs;
};
static_assert(sizeof(PingPongPayload) == 16, "PingPongPayload must be 16 bytes");

/**
 * @brief Payload for OP_ERROR_RESPONSE.
 */
struct ErrorResponsePayload {
	uint16_t requestOpcode;
	uint16_t statusCode; // RealmStatus
	char message[124];
};
static_assert(sizeof(ErrorResponsePayload) == 128, "ErrorResponsePayload must be 128 bytes");

/**
 * @brief Payload for OP_AUTH_CHALLENGE_RESP issued by the server on connection.
 */
struct AuthChallengeResponsePayload {
	uint8_t serverNonce[32];
	uint32_t serverProtocolVersion;
};
static_assert(sizeof(AuthChallengeResponsePayload) == 36, "AuthChallengeResponsePayload must be 36 bytes");

/**
 * @brief Payload for OP_ATTEST_HANDSHAKE_REQ sent by the client to prove official binary integrity.
 * `attestationHmac` = BLAKE2b-256(key = REALM_ATTESTATION_KEY,
 *                     msg = serverNonce[32] || clientNonce[32] || clientTextHash[32] || clientBuildVersionLE[4])
 */
struct AttestHandshakeRequestPayload {
	uint8_t clientNonce[32];
	uint8_t clientTextSectionHash[32];
	uint32_t clientBuildVersion;
	uint8_t attestationHmac[32];
};
static_assert(sizeof(AttestHandshakeRequestPayload) == 100, "AttestHandshakeRequestPayload must be 100 bytes");

/**
 * @brief Payload for OP_ATTEST_HANDSHAKE_RESP returned by the server.
 * Note: Once OP_ATTEST_HANDSHAKE_RESP is sent (using the initial handshake key), both client
 * and server immediately switch their packet MAC trafficKey to:
 * `DeriveSessionTrafficKey(REALM_ATTESTATION_KEY, serverNonce, clientNonce)`.
 */
struct AttestHandshakeResponsePayload {
	uint16_t statusCode; // RealmStatus::OK or ERR_ATTESTATION_FAILED
	uint16_t reserved;
};
static_assert(sizeof(AttestHandshakeResponsePayload) == 4, "AttestHandshakeResponsePayload must be 4 bytes");

/**
 * @brief Payload for OP_AUTH_REGISTER_REQ and OP_AUTH_LOGIN_REQ.
 */
struct AuthCredentialsRequestPayload {
	char username[32]; // Null-terminated ASCII [A-Za-z0-9_-], 3..24 chars
	char password[64]; // Null-terminated UTF-8 password, 8..63 chars
};
static_assert(sizeof(AuthCredentialsRequestPayload) == 96, "AuthCredentialsRequestPayload must be 96 bytes");

/**
 * @brief Payload for OP_AUTH_RESUME_REQ using an active 256-bit session token.
 */
struct AuthResumeRequestPayload {
	uint64_t accountId;
	uint8_t sessionToken[32];
};
static_assert(sizeof(AuthResumeRequestPayload) == 40, "AuthResumeRequestPayload must be 40 bytes");

/**
 * @brief Payload for OP_AUTH_RESULT returned on registration, login, or token resume.
 */
struct AuthResultPayload {
	uint16_t statusCode; // RealmStatus
	uint16_t reserved;
	uint64_t accountId;
	uint8_t sessionToken[32];
	char message[64];
};
static_assert(sizeof(AuthResultPayload) == 108, "AuthResultPayload must be 108 bytes");

/**
 * @brief 48-byte hero roster summary entry (maps directly to DevilutionX `_uiheroinfo` for `selhero.cpp`).
 * `diabloKillLevel` (`herorank` 0..4) drives the 4-skull difficulty badge and DIFF_TORMENT unlock gate.
 */
struct HeroSummaryInfo {
	uint64_t charId;
	char name[32];
	uint8_t heroClass;       // HeroClass (0..5, including Barbarian = 5)
	uint8_t level;           // 1..50
	uint8_t diabloKillLevel; // 0..4 (3+ required to enter DIFF_TORMENT)
	uint8_t bIsHellfire;     // 1 in DX:Eternity
	uint8_t baseStr;
	uint8_t baseMag;
	uint8_t baseDex;
	uint8_t baseVit;
};
static_assert(sizeof(HeroSummaryInfo) == 48, "HeroSummaryInfo must be 48 bytes");

/**
 * @brief Payload for OP_HERO_LIST_RESP returning all heroes owned by the authenticated account.
 */
struct HeroListResponsePayload {
	uint16_t statusCode; // RealmStatus
	uint16_t heroCount;  // 0 .. MaxHeroesPerAccount
	HeroSummaryInfo heroes[MaxHeroesPerAccount];
};
static_assert(sizeof(HeroListResponsePayload) == 4 + 48 * MaxHeroesPerAccount, "HeroListResponsePayload size check");

/**
 * @brief Payload for OP_HERO_CREATE_REQ.
 */
struct HeroCreateRequestPayload {
	char name[32];
	uint8_t heroClass;   // HeroClass (0..5)
	uint8_t bIsHellfire; // 1 for DX:Eternity
	uint16_t reserved;
};
static_assert(sizeof(HeroCreateRequestPayload) == 36, "HeroCreateRequestPayload must be 36 bytes");

/**
 * @brief Payload for OP_HERO_CREATE_RESP containing the newly created hero summary and server-generated PlayerPack.
 */
struct HeroCreateResponsePayload {
	uint16_t statusCode; // RealmStatus
	uint16_t reserved;
	HeroSummaryInfo summary;
	PlayerPack pack;
};
static_assert(sizeof(HeroCreateResponsePayload) == 4 + 48 + 1262, "HeroCreateResponsePayload must be 1314 bytes");

/**
 * @brief Payload for OP_HERO_DELETE_REQ.
 */
struct HeroDeleteRequestPayload {
	uint64_t charId;
};
static_assert(sizeof(HeroDeleteRequestPayload) == 8, "HeroDeleteRequestPayload must be 8 bytes");

/**
 * @brief Payload for OP_HERO_DELETE_RESP.
 */
struct HeroDeleteResponsePayload {
	uint16_t statusCode; // RealmStatus
	uint16_t reserved;
	uint64_t charId;
};
static_assert(sizeof(HeroDeleteResponsePayload) == 12, "HeroDeleteResponsePayload must be 12 bytes");

/**
 * @brief Payload for OP_HERO_LOAD_REQ.
 */
struct HeroLoadRequestPayload {
	uint64_t charId;
};
static_assert(sizeof(HeroLoadRequestPayload) == 8, "HeroLoadRequestPayload must be 8 bytes");

/**
 * @brief Payload for OP_HERO_LOAD_RESP returning the authoritative 1262-byte PlayerPack from the Server-Vault.
 */
struct HeroLoadResponsePayload {
	uint16_t statusCode; // RealmStatus
	uint16_t reserved;
	uint64_t charId;
	PlayerPack pack;
};
static_assert(sizeof(HeroLoadResponsePayload) == 12 + 1262, "HeroLoadResponsePayload must be 1274 bytes");

/**
 * @brief Payload for OP_HERO_SAVE_REQ sent by the client to persist a character snapshot to the Server-Vault.
 */
struct HeroSaveRequestPayload {
	uint64_t charId;
	PlayerPack pack;
};
static_assert(sizeof(HeroSaveRequestPayload) == 8 + 1262, "HeroSaveRequestPayload must be 1270 bytes");

/**
 * @brief Payload for OP_HERO_SAVE_RESP.
 */
struct HeroSaveResponsePayload {
	uint16_t statusCode; // RealmStatus
	uint16_t reserved;
	uint64_t charId;
};
static_assert(sizeof(HeroSaveResponsePayload) == 12, "HeroSaveResponsePayload must be 12 bytes");

/**
 * @brief Header for OP_STASH_LOAD_RESP and OP_STASH_SAVE_REQ followed by `itemCount * sizeof(PkItemStruct)` bytes.
 */
struct StashDataHeader {
	uint16_t statusCode; // RealmStatus (0 on request)
	uint16_t itemCount;  // 0 .. MaxStashItems
	uint32_t stashGold;
};
static_assert(sizeof(StashDataHeader) == 8, "StashDataHeader must be 8 bytes");

/**
 * @brief Payload for OP_STASH_SAVE_RESP.
 */
struct StashSaveResponsePayload {
	uint16_t statusCode; // RealmStatus
	uint16_t savedItemCount;
	uint32_t stashGold;
};
static_assert(sizeof(StashSaveResponsePayload) == 8, "StashSaveResponsePayload must be 8 bytes");

// =========================================================================
// Phase 5: Game Session Registry & Server-Oracle Loot Dispensing Payloads
// =========================================================================

/**
 * @brief Payload for OP_GAME_CREATE_REQ.
 */
struct GameCreateRequestPayload {
	uint64_t charId;
	char gameName[32];
	char password[32];
	uint8_t difficulty;  // _difficulty (0=Normal, 1=Nightmare, 2=Hell, 3=Torment)
	uint8_t maxPlayers;  // 1..4
	uint8_t bIsHellfire; // 1 for DX:Eternity
	uint8_t tickRate;    // 0 = Default 'Fast' (30), or 20..30 ('Normal'=20..'Fast'=30; >30 Faster/Fastest rejected)
};
static_assert(sizeof(GameCreateRequestPayload) == 76, "GameCreateRequestPayload must be 76 bytes");

/**
 * @brief Payload for OP_GAME_JOIN_REQ.
 */
struct GameJoinRequestPayload {
	uint64_t charId;
	char gameName[32];
	char password[32];
};
static_assert(sizeof(GameJoinRequestPayload) == 72, "GameJoinRequestPayload must be 72 bytes");

/**
 * @brief Payload for OP_GAME_CREATE_RESP and OP_GAME_JOIN_RESP.
 * Note: `terrainInitSeed` is the public seed for dungeon geometry layout ONLY;
 * the 256-bit `RealmSecretSalt` used for monster/chest loot rolling never leaves the server.
 */
struct GameCreateOrJoinResponsePayload {
	uint16_t statusCode; // RealmStatus
	uint8_t difficulty;  // _difficulty (0..3)
	uint8_t playerSlot;  // 0..3
	uint64_t gameId;
	uint32_t terrainInitSeed;
	uint8_t bIsHellfire;
	uint8_t maxPlayers;
	uint8_t tickRate;    // Authoritative game tick rate (default/max = 30 'Fast', min = 20 'Normal')
	uint8_t reserved;
};
static_assert(sizeof(GameCreateOrJoinResponsePayload) == 20, "GameCreateOrJoinResponsePayload must be 20 bytes");

/**
 * @brief Optional filter payload for OP_GAME_LIST_REQ (0-byte payload still lists all games).
 */
struct GameListFilterRequestPayload {
	char searchQuery[32];        // Case-insensitive substring match on gameName or host/player heroName (empty = all)
	uint8_t difficultyFilter;    // 0..3 for specific _difficulty, or 0xFF for Any difficulty
	uint8_t hideFullGames;       // 1 = only return games where currentPlayers < maxPlayers
	uint8_t hidePasswordedGames; // 1 = only return public games without a password
	uint8_t reserved;
};
static_assert(sizeof(GameListFilterRequestPayload) == 36, "GameListFilterRequestPayload must be 36 bytes");

/**
 * @brief Public game entry returned in OP_GAME_LIST_RESP.
 */
struct GameLobbyInfo {
	uint64_t gameId;
	char gameName[32];
	uint8_t difficulty;      // _difficulty (0..3)
	uint8_t currentPlayers;  // 1..4
	uint8_t maxPlayers;      // 1..4
	uint8_t passwordProtected;
};
static_assert(sizeof(GameLobbyInfo) == 44, "GameLobbyInfo must be 44 bytes");

/**
 * @brief Payload for OP_GAME_LIST_RESP.
 */
struct GameListResponsePayload {
	uint16_t statusCode; // RealmStatus
	uint16_t gameCount;  // 0 .. MaxListedGames
	GameLobbyInfo games[MaxListedGames];
};
static_assert(sizeof(GameListResponsePayload) == 4 + 44 * MaxListedGames, "GameListResponsePayload size check");

/**
 * @brief Payload for OP_FLOOR_TRANSITION_REQ sent when a player traverses stairs, town portals, or warps.
 */
struct FloorTransitionRequestPayload {
	uint64_t gameId;
	uint8_t fromLevel; // 0..24
	uint8_t toLevel;   // 0..24
	uint8_t tileX;
	uint8_t tileY;
};
static_assert(sizeof(FloorTransitionRequestPayload) == 12, "FloorTransitionRequestPayload must be 12 bytes");

/**
 * @brief Payload for OP_FLOOR_TRANSITION_RESP.
 */
struct FloorTransitionResponsePayload {
	uint16_t statusCode; // RealmStatus
	uint8_t currentLevel;
	uint8_t reserved;
};
static_assert(sizeof(FloorTransitionResponsePayload) == 4, "FloorTransitionResponsePayload must be 4 bytes");

/**
 * @brief Payload for OP_MONSTER_DROP_REQ ("Kill-Receipt RPC").
 */
struct MonsterDropRequestPayload {
	uint64_t gameId;
	uint8_t currlevel;        // 1..24
	uint8_t tileX;            // Monster death tile X
	uint8_t tileY;            // Monster death tile Y
	uint8_t isUniqueMonster;  // 1 if unique boss (`_uniqtype` > 0), 0 for standard monster
	uint16_t monsterIndex;    // Map monster slot index (0 .. MaxFloorMonsters-1)
	int16_t monsterTypeOrUid; // `_monster_id` (standard) or `_uniq_monsterid` (if isUniqueMonster == 1)
	int32_t totalDamageDealt; // Total damage dealt to monster (for Phase 6 TTK/DPS ceiling audit)
	int32_t maxSingleHitDealt; // Highest single hit dealt to monster
	uint32_t combatDurationMs; // Elapsed combat time from first hit to kill
};
static_assert(sizeof(MonsterDropRequestPayload) == 28, "MonsterDropRequestPayload must be 28 bytes");

/**
 * @brief Payload for OP_CHEST_DROP_REQ.
 */
struct ChestDropRequestPayload {
	uint64_t gameId;
	uint8_t currlevel;    // 1..24
	uint8_t tileX;        // Chest/object tile X
	uint8_t tileY;        // Chest/object tile Y
	uint8_t chestQuality; // 0=Small, 1=Medium, 2=Large/Special
	uint16_t objectIndex; // Map object slot index (0 .. MaxFloorObjects-1)
	uint16_t reserved;
};
static_assert(sizeof(ChestDropRequestPayload) == 16, "ChestDropRequestPayload must be 16 bytes");

/**
 * @brief Single server-dispensed ground item entry returned in OP_MONSTER_DROP_RESP / OP_CHEST_DROP_RESP.
 */
struct OracleDroppedItemEntry {
	uint64_t itemUid;   // Server `item_ledger` primary key for atomic pickup verification
	TCmdGItem dropCmd;  // Exact 30-byte DevilutionX ground item network command
	PkItemStruct packed; // Exact 19-byte packed representation
	uint8_t reserved;
};
static_assert(sizeof(OracleDroppedItemEntry) == 58, "OracleDroppedItemEntry must be 58 bytes");

/**
 * @brief Payload for OP_MONSTER_DROP_RESP and OP_CHEST_DROP_RESP.
 * Supports up to `MaxDropsPerKill` (4) items so Hell/Torment Diablo can drop standard loot
 * plus up to 3 independent 2% exclusive uniques (`UITEM_KADISFURY`, `UITEM_APOSTATESCROOK`, `UITEM_LOXLEYSBEND`).
 */
struct MonsterOrChestDropResponsePayload {
	uint16_t statusCode;       // RealmStatus
	uint8_t dropCount;         // 0 .. MaxDropsPerKill
	uint8_t currlevel;
	uint32_t awardedExp;       // Server-verified XP awarded for this kill (0 for chests)
	OracleDroppedItemEntry drops[MaxDropsPerKill];
};
static_assert(sizeof(MonsterOrChestDropResponsePayload) == 8 + 58 * MaxDropsPerKill, "MonsterOrChestDropResponsePayload must be 240 bytes");

/**
 * @brief Payload for OP_ITEM_PICKUP_REQ ("Atomic Ground-to-Inventory Ledger Transfer").
 */
struct ItemPickupRequestPayload {
	uint64_t gameId;
	uint64_t itemUid; // Server ledger ID from OracleDroppedItemEntry (or 0 to match by seed+idx+ci on floor)
	uint8_t currlevel;
	uint8_t tileX;
	uint8_t tileY;
	uint8_t reserved;
	uint32_t iSeed;
	uint16_t idx;
	uint16_t iCreateInfo;
};
static_assert(sizeof(ItemPickupRequestPayload) == 28, "ItemPickupRequestPayload must be 28 bytes");

/**
 * @brief Payload for OP_ITEM_PICKUP_RESP.
 */
struct ItemPickupResponsePayload {
	uint16_t statusCode; // RealmStatus::OK or ERR_ITEM_NOT_ON_GROUND
	uint16_t reserved;
	uint64_t itemUid;
	PkItemStruct item;
};
static_assert(sizeof(ItemPickupResponsePayload) == 31, "ItemPickupResponsePayload must be 31 bytes");

/**
 * @brief Payload for OP_ITEM_DROP_REQ when a player drops an inventory item onto the dungeon/town floor.
 */
struct ItemDropRequestPayload {
	uint64_t gameId;
	uint8_t currlevel;
	uint8_t tileX;
	uint8_t tileY;
	uint8_t reserved;
	PkItemStruct item;
};
static_assert(sizeof(ItemDropRequestPayload) == 31, "ItemDropRequestPayload must be 31 bytes");

/**
 * @brief Payload for OP_ITEM_DROP_RESP.
 */
struct ItemDropResponsePayload {
	uint16_t statusCode; // RealmStatus
	uint16_t reserved;
	OracleDroppedItemEntry dropped;
};
static_assert(sizeof(ItemDropResponsePayload) == 62, "ItemDropResponsePayload must be 62 bytes");

// =========================================================================
// Static Lobby Chat Channels (General / Trade / LFG / Torment) & Whispers
// =========================================================================

/**
 * @brief Single server-stamped chat message entry.
 */
struct ChatMessageEntry {
	uint64_t messageId;
	uint64_t timestampSec;
	uint8_t channelId;       // RealmChatChannel (0=General, 1=Trade, 2=LFG, 3=Torment)
	uint8_t senderClass;     // HeroClass (0..5)
	uint8_t senderLevel;     // 1..50 (server-verified from character row)
	uint8_t senderKillLevel; // 0..4 (server-verified pDiabloKillLevel)
	uint8_t flags;           // RealmChatMessageFlags
	uint8_t reserved[3];
	char senderName[32];     // Server-verified heroName (or account username if no hero selected)
	char text[MaxChatMessageLength];
};
static_assert(sizeof(ChatMessageEntry) == 216, "ChatMessageEntry must be 216 bytes");

/**
 * @brief Payload for OP_CHAT_JOIN_CHANNEL_REQ.
 */
struct ChatJoinChannelRequestPayload {
	uint64_t charId;     // Active hero ID for server-verified badge (or 0 to use account username)
	uint8_t channelId;   // RealmChatChannel (0=General, 1=Trade, 2=LFG, 3=Torment)
	uint8_t reserved[7];
};
static_assert(sizeof(ChatJoinChannelRequestPayload) == 16, "ChatJoinChannelRequestPayload must be 16 bytes");

/**
 * @brief Payload for OP_CHAT_SEND_REQ.
 */
struct ChatSendRequestPayload {
	uint8_t channelId;   // RealmChatChannel (0=General, 1=Trade, 2=LFG, 3=Torment)
	uint8_t reserved[7];
	char text[MaxChatMessageLength]; // Plain text or "/w <target> <msg>", "/who", "/games [query]"
};
static_assert(sizeof(ChatSendRequestPayload) == 168, "ChatSendRequestPayload must be 168 bytes");

/**
 * @brief Payload for OP_CHAT_SEND_RESP.
 */
struct ChatSendResponsePayload {
	uint16_t statusCode; // RealmStatus
	uint8_t channelId;
	uint8_t reserved[5];
	uint64_t messageId;
};
static_assert(sizeof(ChatSendResponsePayload) == 16, "ChatSendResponsePayload must be 16 bytes");

/**
 * @brief Payload for OP_CHAT_POLL_REQ.
 */
struct ChatPollRequestPayload {
	uint64_t afterMessageId; // Return messages with messageId > afterMessageId (0 = recent history)
	uint8_t channelId;       // RealmChatChannel (0=General, 1=Trade, 2=LFG, 3=Torment)
	uint8_t reserved[7];
};
static_assert(sizeof(ChatPollRequestPayload) == 16, "ChatPollRequestPayload must be 16 bytes");

/**
 * @brief Payload for OP_CHAT_JOIN_CHANNEL_RESP and OP_CHAT_POLL_RESP.
 */
struct ChatChannelFeedResponsePayload {
	uint16_t statusCode;      // RealmStatus
	uint8_t channelId;        // RealmChatChannel (0..3)
	uint8_t messageCount;     // 0 .. MaxChatHistoryBatch
	uint16_t channelUsers;    // Number of online sessions currently in this channel
	uint16_t reserved;
	uint64_t latestMessageId; // Highest messageId returned
	ChatMessageEntry messages[MaxChatHistoryBatch];
};
static_assert(sizeof(ChatChannelFeedResponsePayload) == 16 + 216 * MaxChatHistoryBatch, "ChatChannelFeedResponsePayload size check");

// =========================================================================
// Eternity Warden Runtime Memory, Module & Speedhack Attestation (0x05xx)
// =========================================================================

/**
 * @brief Payload for OP_WARDEN_CHALLENGE_REQ (issued by Server to Client).
 */
struct WardenChallengePayload {
	uint64_t challengeId;    // Monotonic server challenge ID
	uint8_t wardenNonce[16]; // 128-bit random challenge salt
	uint8_t tableId;         // WardenMemoryTableId (0..3)
	uint8_t reserved;
	uint16_t sliceIndex;     // Element index within the target table
};
static_assert(sizeof(WardenChallengePayload) == 28, "WardenChallengePayload must be 28 bytes");

/**
 * @brief Payload for OP_WARDEN_CHALLENGE_RESP (sent by Client to Server).
 */
struct WardenResponsePayload {
	uint64_t challengeId;       // Must match issued `WardenChallengePayload::challengeId`
	uint64_t clientMonotonicMs; // Client `GetTickCount64()` / monotonic clock ms (for Speedhack clock-skew detection)
	uint32_t anomalyFlags;      // `WardenClientAnomalyFlags` bitmask
	uint8_t sliceBlake2b[32];   // Keyed BLAKE2b-256 over `(wardenNonce || canonical_table_slice_bytes)`
};
static_assert(sizeof(WardenResponsePayload) == 52, "WardenResponsePayload must be 52 bytes");

/**
 * @brief Payload for OP_WARDEN_VERDICT_RESP (Server acknowledgement of Warden check).
 */
struct WardenVerdictPayload {
	uint16_t statusCode;    // RealmStatus::OK, ERR_WARDEN_INTEGRITY_VIOLATION, or ERR_WARDEN_SPEEDHACK_DETECTED
	uint16_t reserved;
	uint32_t driftPermille; // Measured client/server clock ratio (1000 = 1.000x exact)
	uint64_t challengeId;
};
static_assert(sizeof(WardenVerdictPayload) == 16, "WardenVerdictPayload must be 16 bytes");

#pragma pack(pop)

} // namespace realm
} // namespace devilution
