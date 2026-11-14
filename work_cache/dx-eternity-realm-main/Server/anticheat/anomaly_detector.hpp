/**
 * @file anomaly_detector.hpp
 * @brief Server-Side Anti-Cheat Anomaly Detection, Combat DPS/TTK Ceiling Calculator,
 *        Kill-Cadence Auditor, and Save Conservation & Item Provenance Engine (Phase 6).
 */
#pragma once

#include <cstdint>
#include <string>

#include "db/realm_database.hpp"
#include "game/item_verifier.hpp"
#include "protocol/realm_protocol.hpp"
#include "session/realm_session.hpp"

namespace devilution::realm {

/**
 * @brief Server-computed upper bounds on a character's physical, elemental, and spell damage output
 *        plus movement/attack cadence limits derived from their authoritative `PlayerPack`.
 */
struct AuthoritativeCombatCeiling {
	ComputedPlayerCombatStats equipped {};
	int32_t maxPhysicalSingleHit = 25;
	int32_t maxSpellSingleHit = 25;
	int32_t effectiveMaxSingleHit = 25;
	int32_t maxDpsCeiling = 100;
	uint32_t minAttackIntervalMs = 150;
	uint32_t maxTilesPerSecond = 12;
	bool hasTeleportAccess = false;
};

/**
 * @brief Computes a character's authoritative single-hit, spell, and DPS ceilings from their `PlayerPack`.
 */
AuthoritativeCombatCeiling CalculateAuthoritativeCombatCeiling(const PlayerPack &pack);

/**
 * @brief Validates an incoming `OP_MONSTER_DROP_REQ` against the character's equipped DPS/single-hit ceiling,
 *        multi-hit Time-To-Kill (TTK) floor, and tile travel-distance / kill-frequency cadence.
 *
 * @return `RealmStatus::OK` if legitimate, or `ERR_ANOMALY_TTK_VIOLATION` / `ERR_ANOMALY_CADENCE_VIOLATION`.
 */
RealmStatus AuditMonsterKillAnomaly(
    const AuthoritativeCombatCeiling &ceiling,
    const EffectiveMonsterStats &monsterStats,
    const MonsterDropRequestPayload &req,
    uint64_t nowMs,
    GamePlayerRuntimeState &inOutPlayerState,
    std::string &outReason);

/**
 * @brief Audits an incoming `OP_HERO_SAVE_REQ` (`incomingPack`) against `existingRecord`,
 *        active session XP/Gold conservation (`activePlayerSession` if in-game), and 100% SQLite `item_ledger`
 *        provenance + anti-clone checks.
 */
bool AuditCharacterSaveConservation(
    const RealmDatabase &db,
    const CharacterRecord &existingRecord,
    const PlayerPack &incomingPack,
    const GamePlayerRuntimeState *activePlayerSession,
    std::string &outReason);

/**
 * @brief Generates a randomized Eternity Warden memory-table challenge (`OP_WARDEN_CHALLENGE_REQ`)
 *        targeting a specific entry in `AllItemsList`, `UniqueItems`, `SpellsData`, or `ServerMonstersData`.
 */
WardenChallengePayload GenerateWardenChallenge(uint64_t challengeId);

/**
 * @brief Computes the canonical cross-platform BLAKE2b-256 slice digest (`key = GetDefaultAttestationKey()`)
 *        over `wardenNonce[16]` + deterministic non-pointer gameplay fields of the requested table entry.
 */
bool ComputeWardenSliceDigest(
    uint8_t tableId,
    uint16_t sliceIndex,
    const uint8_t wardenNonce[16],
    std::array<uint8_t, 32> &outDigest);

/**
 * @brief Verifies a client's `OP_WARDEN_CHALLENGE_RESP` against `challenge`:
 *        1. Verifies `challengeId` matches the pending challenge.
 *        2. Verifies `resp.anomalyFlags == WARDEN_FLAG_NONE` (blocks Cheat Engine DLLs, DR0..DR3 hardware breakpoints,
 *           writable `.text` pages, and Rolling-XOR canary mismatches).
 *        3. Verifies `resp.sliceBlake2b` in constant time (`sodium_memcmp`) against `ComputeWardenSliceDigest`.
 *        4. Audits monotonic clock skew `(clientDeltaMs * 1000) / serverDeltaMs <= 1200` to catch `Speedhack.dll`.
 */
RealmStatus VerifyWardenResponse(
    const WardenChallengePayload &challenge,
    uint64_t baselineServerMs,
    uint64_t baselineClientMs,
    uint64_t nowServerMs,
    const WardenResponsePayload &resp,
    uint32_t &outDriftPermille,
    std::string &outReason);

} // namespace devilution::realm

