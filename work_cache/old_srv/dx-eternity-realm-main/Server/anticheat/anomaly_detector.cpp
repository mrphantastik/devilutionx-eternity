/**
 * @file anomaly_detector.cpp
 * @brief Server-Side Anti-Cheat Anomaly Detection, Combat DPS/TTK Ceiling Calculator,
 *        Kill-Cadence Auditor, and Save Conservation & Item Provenance Engine (Phase 6).
 */
#include "anticheat/anomaly_detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <tuple>
#include <vector>

#include <sodium.h>

#include "auth/realm_auth.hpp"
#include "vault/realm_vault.hpp"

namespace devilution::realm {

namespace {

constexpr uint64_t kKillWindowDurationMs = 2000ULL;
constexpr uint32_t kMaxKillsPerWindow = 24U;
constexpr uint64_t kQuestAndShrineXpAllowance = 5000ULL;
constexpr uint64_t kVendorSaleAndStartGoldAllowance = 350000ULL;

using ItemTupleKey = std::tuple<uint16_t, uint16_t, uint32_t, uint32_t>;

ItemTupleKey MakeItemKey(const PkItemStruct &item)
{
	return { item.idx, item.iCreateInfo, item.iSeed, item.dwBuff };
}

bool IsEmptyPackedSlot(const PkItemStruct &item)
{
	return item.idx == 0xFFFFU;
}

bool RequiresDungeonLedgerProvenance(const ServerItem &recreated, const PkItemStruct &packed)
{
	if (recreated.isEmpty() || recreated._itype == ItemType::Gold)
		return false;

	// Town vendor items (Griswold, Wirt, Adria, Pepin) are recreated & price-checked via RecreateTownItem
	if ((packed.iCreateInfo & CF_TOWN) != 0)
		return false;

	// Consumables (potions, scrolls, elixirs, spellbooks) may be used/bought freely
	if (recreated.isUsable() || recreated._itype == ItemType::Misc)
		return false;

	return true;
}

int32_t EstimateMaxSpellHit(const PlayerPack &pack, int32_t spellLvlAdd)
{
	int8_t highestSpellLvl = 0;
	for (int i = 1; i < 37; ++i) {
		highestSpellLvl = std::max(highestSpellLvl, pack.pSplLvl[i]);
	}
	for (int i = 0; i < 10; ++i) {
		highestSpellLvl = std::max(highestSpellLvl, pack.pSplLvl2[i]);
	}

	if (highestSpellLvl <= 0 && pack.pMemSpells == 0ULL && pack.InvBody[4].bMCh == 0) {
		return 35; // Scroll / staff baseline fallback
	}

	const int32_t effectiveSpellLvl = std::clamp<int32_t>(
	    static_cast<int32_t>(highestSpellLvl) + std::max<int32_t>(spellLvlAdd, 0), 1, 20);
	const int32_t charLvl = std::clamp<int32_t>(pack.pLevel, 1, 50);

	// Upper bound across Fireball / Chain Lightning / Apocalypse / Bone Spirit / Holy Bolt scaling:
	// Base ~ (24 + 2 * charLvl) scaled by 1.15^effectiveSpellLvl, with multi-bolt overlap headroom.
	double scaled = static_cast<double>(30 + charLvl * 3);
	for (int32_t s = 1; s < effectiveSpellLvl; ++s) {
		scaled *= 1.16;
	}
	return std::max<int32_t>(static_cast<int32_t>(scaled * 2.0), 60);
}

} // namespace

AuthoritativeCombatCeiling CalculateAuthoritativeCombatCeiling(const PlayerPack &pack)
{
	AuthoritativeCombatCeiling out {};
	out.equipped = CalculatePlayerEquippedStats(pack);

	// 1. Physical + Elemental weapon single-hit ceiling
	// CalculatePlayerEquippedStats already applies 4x crit/Rage/shrine headroom into maxSingleHitCeiling
	out.maxPhysicalSingleHit = std::max<int32_t>(out.equipped.maxSingleHitCeiling * 4, 30);

	// 2. Spell single-hit ceiling
	out.maxSpellSingleHit = EstimateMaxSpellHit(pack, out.equipped.spellLvlAdd);

	// 3. Effective single-hit ceiling (max of physical and spell)
	out.effectiveMaxSingleHit = std::max(out.maxPhysicalSingleHit, out.maxSpellSingleHit);

	// 4. Attack interval & DPS ceiling
	if (HasAnyOf(out.equipped.flags, ItemSpecialEffect::FastestAttack | ItemSpecialEffect::FasterAttack)) {
		out.minAttackIntervalMs = 120;
	} else if (HasAnyOf(out.equipped.flags, ItemSpecialEffect::FastAttack | ItemSpecialEffect::QuickAttack)) {
		out.minAttackIntervalMs = 150;
	} else {
		out.minAttackIntervalMs = 180;
	}

	out.maxDpsCeiling = (out.effectiveMaxSingleHit * 1000) / static_cast<int32_t>(out.minAttackIntervalMs);

	// 5. Movement & Teleport capability
	const uint64_t teleportBit = 1ULL << (static_cast<int>(SpellID::Teleport) - 1);
	const uint64_t phasingBit = 1ULL << (static_cast<int>(SpellID::Phasing) - 1);
	out.hasTeleportAccess = ((pack.pMemSpells & (teleportBit | phasingBit)) != 0ULL)
	    || (pack.pSplLvl[static_cast<int>(SpellID::Teleport)] > 0)
	    || (pack.pSplLvl[static_cast<int>(SpellID::Phasing)] > 0);

	if (HasAnyOf(out.equipped.hfFlags, ItemSpecialEffectHf::FastWalk)) {
		out.maxTilesPerSecond = 18;
	} else {
		out.maxTilesPerSecond = 12;
	}

	return out;
}

RealmStatus AuditMonsterKillAnomaly(
    const AuthoritativeCombatCeiling &ceiling,
    const EffectiveMonsterStats &monsterStats,
    const MonsterDropRequestPayload &req,
    uint64_t nowMs,
    GamePlayerRuntimeState &inOutPlayerState,
    std::string &outReason)
{
	// 1. Total damage must at least equal the monster's difficulty-scaled minimum HP
	if (req.totalDamageDealt < monsterStats.minHp) {
		outReason = "Total damage dealt (" + std::to_string(req.totalDamageDealt)
		    + ") is below monster minimum HP (" + std::to_string(monsterStats.minHp) + ")";
		return RealmStatus::ERR_ANOMALY_TTK_VIOLATION;
	}

	// 2. Single-hit damage must not exceed the character's authoritative single-hit ceiling
	if (req.maxSingleHitDealt > ceiling.effectiveMaxSingleHit) {
		outReason = "Single-hit damage (" + std::to_string(req.maxSingleHitDealt)
		    + ") exceeds character equipped ceiling (" + std::to_string(ceiling.effectiveMaxSingleHit) + ")";
		return RealmStatus::ERR_ANOMALY_TTK_VIOLATION;
	}

	// 3. Multi-hit Boss / Elite Time-To-Kill (TTK) Floor
	const int32_t minHitsRequired = (monsterStats.minHp + ceiling.effectiveMaxSingleHit - 1) / ceiling.effectiveMaxSingleHit;
	if (minHitsRequired >= 3) {
		const uint32_t minRequiredDurationMs = static_cast<uint32_t>(minHitsRequired - 1) * ceiling.minAttackIntervalMs;
		if (req.combatDurationMs < minRequiredDurationMs) {
			outReason = "Impossible TTK: monster requires at least " + std::to_string(minHitsRequired)
			    + " hits (" + std::to_string(minRequiredDurationMs) + " ms), but kill claimed in "
			    + std::to_string(req.combatDurationMs) + " ms";
			return RealmStatus::ERR_ANOMALY_TTK_VIOLATION;
		}
	}

	// 4. Sliding-Window Kill-Frequency Cadence Check
	if (nowMs - inOutPlayerState.killWindowStartMs >= kKillWindowDurationMs) {
		inOutPlayerState.killWindowStartMs = nowMs;
		inOutPlayerState.killsInRecentWindow = 0;
	}
	if (++inOutPlayerState.killsInRecentWindow > kMaxKillsPerWindow) {
		outReason = "Kill packet rate (" + std::to_string(inOutPlayerState.killsInRecentWindow)
		    + " kills in 2s window) exceeds maximum cadence";
		return RealmStatus::ERR_ANOMALY_CADENCE_VIOLATION;
	}

	// 5. Tile Travel-Distance Cadence Check (blocks cross-map teleport/speedhack killpackets without Teleport)
	if (inOutPlayerState.lastKillTimestampMs > 0 && nowMs >= inOutPlayerState.lastKillTimestampMs) {
		const uint64_t elapsedMs = nowMs - inOutPlayerState.lastKillTimestampMs;
		const int dx = std::abs(static_cast<int>(req.tileX) - static_cast<int>(inOutPlayerState.lastTileX));
		const int dy = std::abs(static_cast<int>(req.tileY) - static_cast<int>(inOutPlayerState.lastTileY));
		const int tileDist = std::max(dx, dy);

		// Ranged bow/spell screen reach is ~24 tiles; any distance beyond screen reach must obey maxTilesPerSecond
		const uint64_t maxReachableTiles = 24ULL + (elapsedMs * static_cast<uint64_t>(ceiling.maxTilesPerSecond)) / 1000ULL;
		if (!ceiling.hasTeleportAccess && static_cast<uint64_t>(tileDist) > maxReachableTiles) {
			outReason = "Impossible tile jump between kills (distance=" + std::to_string(tileDist)
			    + " tiles in " + std::to_string(elapsedMs) + " ms exceeds max reachable "
			    + std::to_string(maxReachableTiles) + " tiles without Teleport)";
			return RealmStatus::ERR_ANOMALY_CADENCE_VIOLATION;
		}
	}

	return RealmStatus::OK;
}

bool AuditCharacterSaveConservation(
    const RealmDatabase &db,
    const CharacterRecord &existingRecord,
    const PlayerPack &incomingPack,
    const GamePlayerRuntimeState *activePlayerSession,
    std::string &outReason)
{
	// 1. Structural, stat-cap, level/XP table, HP/Mana formula, and RecreateServerItem() checks
	if (!RealmVaultManager::ValidatePlayerPackSnapshot(existingRecord, incomingPack, outReason)) {
		return false;
	}

	// 2. Active Session XP & Gold Conservation Checks
	if (activePlayerSession != nullptr) {
		const int64_t initialXp = std::max<int64_t>(0, activePlayerSession->initialPack.pExperience);
		const int64_t incomingXp = std::max<int64_t>(0, incomingPack.pExperience);
		if (incomingXp > initialXp) {
			const auto xpGained = static_cast<uint64_t>(incomingXp - initialXp);
			// Quest/shrine XP allowance is only unlocked after verified monster kills on a dungeon floor
			const uint64_t bonusAllowance = (activePlayerSession->verifiedExpGained > 0)
			    ? std::min<uint64_t>(kQuestAndShrineXpAllowance, activePlayerSession->verifiedExpGained * 2ULL)
			    : 0ULL;
			const uint64_t maxAllowedXpGain = activePlayerSession->verifiedExpGained + bonusAllowance;
			if (xpGained > maxAllowedXpGain) {
				outReason = "XP conservation violation: gained " + std::to_string(xpGained)
				    + " XP, but verified session kills + allowance is " + std::to_string(maxAllowedXpGain);
				return false;
			}
		}

		const int64_t initialGold = std::max<int64_t>(0, activePlayerSession->initialPack.pGold);
		const int64_t incomingGold = std::max<int64_t>(0, incomingPack.pGold);
		if (incomingGold > initialGold) {
			const auto goldDelta = static_cast<uint64_t>(incomingGold - initialGold);
			// Vendor sale allowance is only unlocked if the player has visited a dungeon floor or killed a monster
			const bool hasDungeonActivity = (activePlayerSession->verifiedExpGained > 0)
			    || (activePlayerSession->verifiedGoldDropped > 0)
			    || (activePlayerSession->visitedFloors.count() > 1);
			const uint64_t vendorAllowance = hasDungeonActivity ? kVendorSaleAndStartGoldAllowance : 0ULL;
			const uint64_t maxAllowedGoldDelta = activePlayerSession->verifiedGoldDropped + vendorAllowance;
			if (goldDelta > maxAllowedGoldDelta) {
				outReason = "Gold conservation violation: gained " + std::to_string(goldDelta)
				    + " gold, exceeding verified drops + vendor sale allowance (" + std::to_string(maxAllowedGoldDelta) + ")";
				return false;
			}
		}

		// Anti-Boost Safeguard on Hell Diablo Kill Credit (`pDiabloKillLevel == 3`):
		// If the player was in a Hell party where `(highestPartyLevel - playerLevel) > 10` and `playerLevel < 32`,
		// or never slayed Hell Diablo in the session, reject claiming `pDiabloKillLevel >= 3`.
		if (incomingPack.pDiabloKillLevel >= TormentRequiredDiabloKillLevel
		    && existingRecord.diabloKillLevel < TormentRequiredDiabloKillLevel) {
			if (activePlayerSession->hellDiabloBoostBlocked || !activePlayerSession->hellDiabloCreditGranted) {
				outReason = "Hell Diablo kill credit blocked by anti-boost safeguard (>10 party level gap below level 32) or unverified Hell Diablo kill";
				return false;
			}
		}
	} else {
		// Saving outside an active game session cannot jump more than a single quest/shrine XP allowance
		if (incomingPack.pExperience > existingRecord.experience + static_cast<int32_t>(kQuestAndShrineXpAllowance)) {
			outReason = "XP spike outside an active game session";
			return false;
		}
		if (incomingPack.pGold > existingRecord.gold + static_cast<int32_t>(kVendorSaleAndStartGoldAllowance)) {
			outReason = "Gold spike outside an active game session";
			return false;
		}
	}

	// 3. 100% Item Ledger Provenance & Anti-Clone Verification
	// Count copies of each dungeon equipment item in the incoming PlayerPack
	std::map<ItemTupleKey, int64_t> incomingCopies;
	std::map<ItemTupleKey, PkItemStruct> samplePackedByKey;

	auto inspectSlot = [&](const PkItemStruct &slot) -> bool {
		if (IsEmptyPackedSlot(slot))
			return true;
		std::string err;
		ServerItem recreated {};
		if (!VerifyPackedItemIntegrity(incomingPack, slot, err, &recreated)) {
			outReason = "Save provenance item recreation failed: " + err;
			return false;
		}
		if (RequiresDungeonLedgerProvenance(recreated, slot)) {
			const ItemTupleKey key = MakeItemKey(slot);
			incomingCopies[key]++;
			samplePackedByKey[key] = slot;
		}
		return true;
	};

	for (const PkItemStruct &eq : incomingPack.InvBody) {
		if (!inspectSlot(eq))
			return false;
	}
	for (int i = 0; i < incomingPack._pNumInv; ++i) {
		if (!inspectSlot(incomingPack.InvList[i]))
			return false;
	}

	for (const auto &[key, neededCount] : incomingCopies) {
		const PkItemStruct &sample = samplePackedByKey[key];
		// Strictly enforce active `InInventoryOrStash` ledger ownership across the account (blocks Drop+Save rollback dupes)
		const int64_t allowedCount = std::max(
		    db.CountCharacterLedgerCopies(existingRecord.id, sample),
		    db.CountAccountLedgerCopies(existingRecord.accountId, sample));

		if (neededCount > allowedCount) {
			outReason = "Item provenance / dupe violation for item (idx=" + std::to_string(sample.idx)
			    + ", ci=" + std::to_string(sample.iCreateInfo)
			    + ", seed=" + std::to_string(sample.iSeed)
			    + "): save contains " + std::to_string(neededCount)
			    + " copy/copies, but item_ledger allows " + std::to_string(allowedCount);
			return false;
		}
	}

	return true;
}

namespace {

void AppendLE16(std::vector<uint8_t> &buf, uint16_t v)
{
	buf.push_back(static_cast<uint8_t>(v & 0xFFU));
	buf.push_back(static_cast<uint8_t>((v >> 8U) & 0xFFU));
}

void AppendLE32(std::vector<uint8_t> &buf, uint32_t v)
{
	buf.push_back(static_cast<uint8_t>(v & 0xFFU));
	buf.push_back(static_cast<uint8_t>((v >> 8U) & 0xFFU));
	buf.push_back(static_cast<uint8_t>((v >> 16U) & 0xFFU));
	buf.push_back(static_cast<uint8_t>((v >> 24U) & 0xFFU));
}

} // namespace

WardenChallengePayload GenerateWardenChallenge(uint64_t challengeId)
{
	WardenChallengePayload out {};
	out.challengeId = challengeId;
	randombytes_buf(out.wardenNonce, sizeof(out.wardenNonce));
	out.tableId = static_cast<uint8_t>(randombytes_uniform(4));

	switch (static_cast<WardenMemoryTableId>(out.tableId)) {
	case WardenMemoryTableId::AllItemsList:
		out.sliceIndex = static_cast<uint16_t>(randombytes_uniform(static_cast<uint32_t>(IDI_LAST) + 1U));
		break;
	case WardenMemoryTableId::UniqueItems:
		out.sliceIndex = static_cast<uint16_t>(randombytes_uniform(static_cast<uint32_t>(UITEM_LOXLEYSBEND) + 1U));
		break;
	case WardenMemoryTableId::SpellsData:
		out.sliceIndex = static_cast<uint16_t>(randombytes_uniform(MAX_SPELLS));
		break;
	case WardenMemoryTableId::MonstersData:
	default:
		out.tableId = static_cast<uint8_t>(WardenMemoryTableId::MonstersData);
		out.sliceIndex = static_cast<uint16_t>(randombytes_uniform(static_cast<uint32_t>(NUM_MTYPES)));
		break;
	}
	return out;
}

bool ComputeWardenSliceDigest(
    uint8_t tableId,
    uint16_t sliceIndex,
    const uint8_t wardenNonce[16],
    std::array<uint8_t, 32> &outDigest)
{
	std::vector<uint8_t> canonical;
	canonical.reserve(128);
	canonical.insert(canonical.end(), wardenNonce, wardenNonce + 16);
	canonical.push_back(tableId);
	AppendLE16(canonical, sliceIndex);

	switch (static_cast<WardenMemoryTableId>(tableId)) {
	case WardenMemoryTableId::AllItemsList: {
		if (sliceIndex > static_cast<uint16_t>(IDI_LAST))
			return false;
		const ItemData &it = AllItemsList[sliceIndex];
		canonical.push_back(static_cast<uint8_t>(it.iRnd));
		canonical.push_back(static_cast<uint8_t>(it.iClass));
		canonical.push_back(static_cast<uint8_t>(it.iLoc));
		canonical.push_back(static_cast<uint8_t>(it.iCurs));
		canonical.push_back(static_cast<uint8_t>(it.itype));
		canonical.push_back(static_cast<uint8_t>(it.iItemId));
		canonical.push_back(it.iMinMLvl);
		canonical.push_back(it.iDurability);
		canonical.push_back(it.iMinDam);
		canonical.push_back(it.iMaxDam);
		canonical.push_back(it.iMinAC);
		canonical.push_back(it.iMaxAC);
		canonical.push_back(it.iMinStr);
		canonical.push_back(it.iMinMag);
		canonical.push_back(it.iMinDex);
		AppendLE32(canonical, static_cast<uint32_t>(it.iFlags));
		canonical.push_back(static_cast<uint8_t>(it.iMiscId));
		canonical.push_back(static_cast<uint8_t>(it.iSpell));
		canonical.push_back(it.iUsable ? 1U : 0U);
		AppendLE16(canonical, it.iValue);
		break;
	}
	case WardenMemoryTableId::UniqueItems: {
		if (sliceIndex > static_cast<uint16_t>(UITEM_LOXLEYSBEND))
			return false;
		const UniqueItem &u = UniqueItems[sliceIndex];
		canonical.push_back(static_cast<uint8_t>(u.UIItemId));
		canonical.push_back(static_cast<uint8_t>(u.UIMinLvl));
		canonical.push_back(u.UINumPL);
		AppendLE32(canonical, static_cast<uint32_t>(u.UIValue));
		for (const ItemPower &pw : u.powers) {
			canonical.push_back(static_cast<uint8_t>(pw.type));
			AppendLE32(canonical, static_cast<uint32_t>(pw.param1));
			AppendLE32(canonical, static_cast<uint32_t>(pw.param2));
		}
		break;
	}
	case WardenMemoryTableId::SpellsData: {
		if (sliceIndex >= MAX_SPELLS)
			return false;
		const ServerSpellData &sp = SpellsData[sliceIndex];
		AppendLE16(canonical, sp.bookCost10);
		canonical.push_back(sp.staffCost10);
		canonical.push_back(sp.sManaCost);
		canonical.push_back(static_cast<uint8_t>(sp.sBookLvl));
		canonical.push_back(static_cast<uint8_t>(sp.sStaffLvl));
		canonical.push_back(sp.minInt);
		canonical.push_back(sp.sStaffMin);
		canonical.push_back(sp.sStaffMax);
		break;
	}
	case WardenMemoryTableId::MonstersData: {
		if (sliceIndex >= static_cast<uint16_t>(NUM_MTYPES))
			return false;
		const ServerMonsterData &m = ServerMonstersData[sliceIndex];
		canonical.push_back(static_cast<uint8_t>(m.availability));
		canonical.push_back(static_cast<uint8_t>(m.minDunLvl));
		canonical.push_back(static_cast<uint8_t>(m.maxDunLvl));
		canonical.push_back(static_cast<uint8_t>(m.level));
		AppendLE16(canonical, m.hitPointsMinimum);
		AppendLE16(canonical, m.hitPointsMaximum);
		canonical.push_back(m.toHit);
		canonical.push_back(m.minDamage);
		canonical.push_back(m.maxDamage);
		canonical.push_back(m.toHitSpecial);
		canonical.push_back(m.minDamageSpecial);
		canonical.push_back(m.maxDamageSpecial);
		canonical.push_back(m.armorClass);
		AppendLE16(canonical, m.treasure);
		AppendLE16(canonical, m.exp);
		break;
	}
	default:
		return false;
	}

	const AttestationKey key = GetDefaultAttestationKey();
	crypto_generichash(
	    outDigest.data(),
	    outDigest.size(),
	    canonical.data(),
	    canonical.size(),
	    key.data(),
	    key.size());
	return true;
}

RealmStatus VerifyWardenResponse(
    const WardenChallengePayload &challenge,
    uint64_t baselineServerMs,
    uint64_t baselineClientMs,
    uint64_t nowServerMs,
    const WardenResponsePayload &resp,
    uint32_t &outDriftPermille,
    std::string &outReason)
{
	outDriftPermille = 1000;

	if (resp.challengeId != challenge.challengeId) {
		outReason = "Warden challengeId mismatch (expected " + std::to_string(challenge.challengeId)
		    + ", got " + std::to_string(resp.challengeId) + ")";
		return RealmStatus::ERR_WARDEN_INTEGRITY_VIOLATION;
	}

	// 1. Client self-reported environment anomaly flags (Cheat Engine DLL, DR0..DR3, .text page permissions, XOR canary)
	if (resp.anomalyFlags != WARDEN_FLAG_CLEAN) {
		std::string details;
		if ((resp.anomalyFlags & WARDEN_FLAG_DEBUGGER_PRESENT) != 0)
			details += "DEBUGGER_PRESENT ";
		if ((resp.anomalyFlags & WARDEN_FLAG_HARDWARE_BREAKPOINT) != 0)
			details += "HW_BREAKPOINTS(DR0..DR3) ";
		if ((resp.anomalyFlags & WARDEN_FLAG_TEXT_SECTION_MODIFIED) != 0)
			details += "TEXT_SECTION_MODIFIED ";
		if ((resp.anomalyFlags & WARDEN_FLAG_SUSPICIOUS_DLL) != 0)
			details += "SUSPICIOUS_DLL(CheatEngine/Speedhack) ";
		if ((resp.anomalyFlags & WARDEN_FLAG_XOR_CANARY_MISMATCH) != 0)
			details += "ROLLING_XOR_CANARY_MISMATCH ";

		outReason = "Warden runtime environment violation: " + details
		    + "(flags=0x" + std::to_string(resp.anomalyFlags) + ")";
		return RealmStatus::ERR_WARDEN_INTEGRITY_VIOLATION;
	}

	// 2. Verify live memory-table slice BLAKE2b-256 digest in constant time
	std::array<uint8_t, 32> expectedDigest {};
	if (!ComputeWardenSliceDigest(challenge.tableId, challenge.sliceIndex, challenge.wardenNonce, expectedDigest)) {
		outReason = "Invalid Warden challenge table/slice parameters";
		return RealmStatus::ERR_WARDEN_INTEGRITY_VIOLATION;
	}
	if (sodium_memcmp(resp.sliceBlake2b, expectedDigest.data(), expectedDigest.size()) != 0) {
		outReason = "Warden live RAM table tampering detected on tableId=" + std::to_string(challenge.tableId)
		    + " sliceIndex=" + std::to_string(challenge.sliceIndex);
		return RealmStatus::ERR_WARDEN_INTEGRITY_VIOLATION;
	}

	// 3. Audit Speedhack Clock-Skew against server steady_clock baseline
	if (baselineServerMs > 0 && baselineClientMs > 0 && nowServerMs > baselineServerMs) {
		const uint64_t serverDeltaMs = nowServerMs - baselineServerMs;
		if (resp.clientMonotonicMs < baselineClientMs) {
			outReason = "Warden non-monotonic client clock rollback detected";
			return RealmStatus::ERR_WARDEN_SPEEDHACK_DETECTED;
		}
		const uint64_t clientDeltaMs = resp.clientMonotonicMs - baselineClientMs;
		if (serverDeltaMs >= 1000ULL) {
			outDriftPermille = static_cast<uint32_t>((clientDeltaMs * 1000ULL) / serverDeltaMs);
			// Allow up to 20% clock/network jitter (1200 permille = 1.20x); anything faster is Speedhack
			if (outDriftPermille > 1200U && (clientDeltaMs > serverDeltaMs + 400ULL)) {
				outReason = "Warden Speedhack clock-skew detected: client clock ran at "
				    + std::to_string(outDriftPermille) + " permille (" + std::to_string(clientDeltaMs)
				    + " ms vs server " + std::to_string(serverDeltaMs) + " ms)";
				return RealmStatus::ERR_WARDEN_SPEEDHACK_DETECTED;
			}
		}
	}

	outReason = "OK";
	return RealmStatus::OK;
}

} // namespace devilution::realm

