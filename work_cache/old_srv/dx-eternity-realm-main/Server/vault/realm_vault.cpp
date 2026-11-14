/**
 * @file realm_vault.cpp
 * @brief Implementation of Server-Vault character creation, roster listing, validation, and stash storage.
 */
#include "vault/realm_vault.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <sodium.h>

#include "anticheat/anomaly_detector.hpp"
#include "game/item_verifier.hpp"

namespace devilution::realm {

namespace {

constexpr int8_t MaxCharacterLevel = 50;
constexpr uint16_t EmptyPackedItemIdx = 0xFFFFU;

// Logical equipment slots (matches inv_body_loc in Source/player.h)
enum InvBodyLoc : size_t {
	INVLOC_HEAD = 0,
	INVLOC_RING_LEFT = 1,
	INVLOC_RING_RIGHT = 2,
	INVLOC_AMULET = 3,
	INVLOC_HAND_LEFT = 4,
	INVLOC_HAND_RIGHT = 5,
	INVLOC_CHEST = 6,
	NUM_INVLOC = 7,
};

constexpr int8_t SpellIdFirebolt = 1; // SpellID::Firebolt in Source/spelldat.h

// Exact ExpLvlsTbl[50] from Source/playerdat.cpp
constexpr int32_t kExpLvlsTbl[MaxCharacterLevel] = {
	0,
	2000,
	4620,
	8040,
	12489,
	18258,
	25712,
	35309,
	47622,
	63364,
	83419,
	108879,
	141086,
	181683,
	231075,
	313656,
	424067,
	571190,
	766569,
	1025154,
	1366227,
	1814568,
	2401895,
	3168651,
	4166200,
	5459523,
	7130496,
	9281874,
	12042092,
	15571031,
	20066900,
	25774405,
	32994399,
	42095202,
	53525811,
	67831218,
	85670061,
	107834823,
	135274799,
	169122009,
	210720231,
	261657253,
	323800420,
	399335440,
	490808349,
	601170414,
	733825617,
	892680222,
	1082908612,
	1310707109
};

// Exact PlayersData[6] from DX:Eternity Source/playerdat.cpp
constexpr HeroClassStatsSpec kHeroClassSpecs[6] = {
	// Warrior
	{ "Warrior", 30, 10, 20, 25, 250, 50, 60, 100, 30, (18 << 6), -(1 << 6), (2 << 6), (1 << 6), (2 << 6), (1 << 6), (2 << 6), (1 << 6) },
	// Rogue
	{ "Rogue", 20, 15, 30, 20, 55, 70, 250, 80, 20, (23 << 6), 352, (2 << 6), (2 << 6), (1 << 6), (1 << 6), 96, 96 },
	// Sorcerer
	{ "Sorcerer", 15, 35, 15, 20, 45, 250, 85, 80, 10, (9 << 6), -(2 << 6), (1 << 6), (2 << 6), (1 << 6), (2 << 6), (1 << 6), (2 << 6) },
	// Monk
	{ "Monk", 25, 15, 25, 20, 150, 80, 150, 80, 25, (23 << 6), 352, (2 << 6), (2 << 6), (1 << 6), (1 << 6), 96, 96 },
	// Bard
	{ "Bard", 20, 20, 25, 20, 120, 120, 120, 100, 25, (23 << 6), (3 << 6), (2 << 6), (2 << 6), (1 << 6), 96, 96, 112 },
	// Barbarian (Classic Female Barbarian in DX:Eternity)
	{ "Barbarian", 40, 0, 20, 25, 255, 0, 55, 150, 30, (18 << 6), 0, (2 << 6), 0, (2 << 6), (1 << 6), 160, (1 << 6) },
};

uint32_t GenerateNonZeroItemSeed()
{
	uint32_t seed = 0;
	while (seed == 0) {
		seed = randombytes_uniform(0x7FFFFFFFU);
	}
	return seed;
}

PkItemStruct MakeEmptyPackedItem()
{
	PkItemStruct item {};
	item.idx = EmptyPackedItemIdx;
	return item;
}

PkItemStruct MakeStartingPackedItem(_item_indexes idx, uint8_t durability, uint8_t charges, uint16_t value, bool isHellfire)
{
	(void)durability;
	(void)charges;
	ServerItem serverItem {};
	InitializeServerItem(serverItem, idx, isHellfire, DIFF_NORMAL);
	serverItem._iSeed = GenerateNonZeroItemSeed();
	if (idx == IDI_GOLD) {
		serverItem._ivalue = value;
	}
	PkItemStruct packed {};
	PackServerItem(packed, serverItem, isHellfire);
	return packed;
}

HeroSummaryInfo BuildHeroSummary(int64_t charId, const PlayerPack &pack)
{
	HeroSummaryInfo info {};
	info.charId = static_cast<uint64_t>(charId);
	std::snprintf(info.name, sizeof(info.name), "%s", pack.pName);
	info.heroClass = pack.pClass;
	info.level = static_cast<uint8_t>(std::clamp<int8_t>(pack.pLevel, 1, MaxCharacterLevel));
	info.diabloKillLevel = static_cast<uint8_t>(std::min<uint32_t>(pack.pDiabloKillLevel, 4U));
	info.bIsHellfire = pack.bIsHellfire;
	info.baseStr = pack.pBaseStr;
	info.baseMag = pack.pBaseMag;
	info.baseDex = pack.pBaseDex;
	info.baseVit = pack.pBaseVit;
	return info;
}

} // namespace

const HeroClassStatsSpec &GetHeroClassStatsSpec(HeroClass heroClass)
{
	const auto idx = static_cast<size_t>(heroClass);
	if (idx <= static_cast<size_t>(HeroClass::LAST)) {
		return kHeroClassSpecs[idx];
	}
	return kHeroClassSpecs[0];
}

int32_t CalculateHeroBaseLife(HeroClass heroClass, int8_t level, uint8_t baseVit)
{
	const HeroClassStatsSpec &spec = GetHeroClassStatsSpec(heroClass);
	return static_cast<int32_t>(spec.adjLife)
	    + (static_cast<int32_t>(spec.lvlLife) * static_cast<int32_t>(level))
	    + (static_cast<int32_t>(spec.chrLife) * static_cast<int32_t>(baseVit));
}

int32_t CalculateHeroBaseMana(HeroClass heroClass, int8_t level, uint8_t baseMag)
{
	const HeroClassStatsSpec &spec = GetHeroClassStatsSpec(heroClass);
	return static_cast<int32_t>(spec.adjMana)
	    + (static_cast<int32_t>(spec.lvlMana) * static_cast<int32_t>(level))
	    + (static_cast<int32_t>(spec.chrMana) * static_cast<int32_t>(baseMag));
}

PlayerPack BuildInitialPlayerPack(const std::string &heroName, HeroClass heroClass, bool isHellfire)
{
	PlayerPack pack {};
	const HeroClassStatsSpec &spec = GetHeroClassStatsSpec(heroClass);

	std::snprintf(pack.pName, sizeof(pack.pName), "%s", heroName.c_str());
	pack.pClass = static_cast<uint8_t>(heroClass);
	pack.plrlevel = 0; // Tristram Town
	pack.px = 75;
	pack.py = 68;

	pack.pBaseStr = spec.baseStr;
	pack.pBaseMag = spec.baseMag;
	pack.pBaseDex = spec.baseDex;
	pack.pBaseVit = spec.baseVit;
	pack.pLevel = 1;
	pack.pStatPts = 0;
	pack.pExperience = 0;

	const int32_t baseLife = CalculateHeroBaseLife(heroClass, 1, spec.baseVit);
	const int32_t baseMana = CalculateHeroBaseMana(heroClass, 1, spec.baseMag);
	pack.pHPBase = baseLife;
	pack.pMaxHPBase = baseLife;
	pack.pManaBase = baseMana;
	pack.pMaxManaBase = baseMana;

	if (heroClass == HeroClass::Sorcerer) {
		pack.pMemSpells = 1ULL << (SpellIdFirebolt - 1);
		pack.pSplLvl[SpellIdFirebolt] = 2;
	}

	for (auto &slot : pack.InvBody) {
		slot = MakeEmptyPackedItem();
	}
	for (auto &slot : pack.InvList) {
		slot = MakeEmptyPackedItem();
	}
	for (auto &slot : pack.SpdList) {
		slot = MakeEmptyPackedItem();
	}

	// Populate starting class equipment & potions matching CreatePlrItems(player) in Source/items.cpp
	switch (heroClass) {
	case HeroClass::Warrior:
		pack.InvBody[INVLOC_HAND_LEFT] = MakeStartingPackedItem(IDI_WARRIOR, 20, 0, 50, isHellfire);
		pack.InvBody[INVLOC_HAND_RIGHT] = MakeStartingPackedItem(IDI_WARRSHLD, 10, 0, 50, isHellfire);
		// Starting club in inventory slot 0 (1x3 cells at grid indices 0, 10, 20)
		pack.InvList[0] = MakeStartingPackedItem(IDI_WARRCLUB, 20, 0, 20, isHellfire);
		pack._pNumInv = 1;
		pack.InvGrid[0] = -1;
		pack.InvGrid[10] = -1;
		pack.InvGrid[20] = 1;
		pack.SpdList[0] = MakeStartingPackedItem(IDI_HEAL, 0, 0, 50, isHellfire);
		pack.SpdList[1] = MakeStartingPackedItem(IDI_HEAL, 0, 0, 50, isHellfire);
		break;

	case HeroClass::Rogue:
		pack.InvBody[INVLOC_HAND_LEFT] = MakeStartingPackedItem(IDI_ROGUE, 30, 0, 100, isHellfire);
		pack.SpdList[0] = MakeStartingPackedItem(IDI_HEAL, 0, 0, 50, isHellfire);
		pack.SpdList[1] = MakeStartingPackedItem(IDI_HEAL, 0, 0, 50, isHellfire);
		break;

	case HeroClass::Sorcerer: {
		const _item_indexes staffIdx = isHellfire ? IDI_SORCERER : IDI_SORCERER_DIABLO;
		const uint8_t staffCharges = isHellfire ? 18 : 40;
		const _item_indexes potIdx = isHellfire ? IDI_HEAL : IDI_MANA;
		pack.InvBody[INVLOC_HAND_LEFT] = MakeStartingPackedItem(staffIdx, 25, staffCharges, 520, isHellfire);
		pack.SpdList[0] = MakeStartingPackedItem(potIdx, 0, 0, 50, isHellfire);
		pack.SpdList[1] = MakeStartingPackedItem(potIdx, 0, 0, 50, isHellfire);
		break;
	}

	case HeroClass::Monk:
		pack.InvBody[INVLOC_HAND_LEFT] = MakeStartingPackedItem(IDI_SHORTSTAFF, 25, 0, 20, isHellfire);
		pack.SpdList[0] = MakeStartingPackedItem(IDI_HEAL, 0, 0, 50, isHellfire);
		pack.SpdList[1] = MakeStartingPackedItem(IDI_HEAL, 0, 0, 50, isHellfire);
		break;

	case HeroClass::Bard:
		pack.InvBody[INVLOC_HAND_LEFT] = MakeStartingPackedItem(IDI_BARDSWORD, 8, 0, 20, isHellfire);
		pack.InvBody[INVLOC_HAND_RIGHT] = MakeStartingPackedItem(IDI_BARDDAGGER, 16, 0, 20, isHellfire);
		pack.SpdList[0] = MakeStartingPackedItem(IDI_HEAL, 0, 0, 50, isHellfire);
		pack.SpdList[1] = MakeStartingPackedItem(IDI_HEAL, 0, 0, 50, isHellfire);
		break;

	case HeroClass::Barbarian:
		pack.InvBody[INVLOC_HAND_LEFT] = MakeStartingPackedItem(IDI_BARBARIAN, 20, 0, 20, isHellfire);
		pack.InvBody[INVLOC_HAND_RIGHT] = MakeStartingPackedItem(IDI_WARRSHLD, 10, 0, 50, isHellfire);
		pack.SpdList[0] = MakeStartingPackedItem(IDI_HEAL, 0, 0, 50, isHellfire);
		pack.SpdList[1] = MakeStartingPackedItem(IDI_HEAL, 0, 0, 50, isHellfire);
		break;
	}

	// Starting 100 Gold stack at InvGrid[30]
	pack.InvList[pack._pNumInv] = MakeStartingPackedItem(IDI_GOLD, 0, 0, 100, isHellfire);
	pack._pNumInv++;
	pack.InvGrid[30] = pack._pNumInv;
	pack.pGold = 100;

	pack.bIsHellfire = isHellfire ? 1 : 0;
	pack.pDiabloKillLevel = 0;
	pack.pDifficulty = DIFF_NORMAL;
	pack.pDamAcFlags = 0;
	return pack;
}

RealmVaultManager::RealmVaultManager(RealmDatabase &db)
    : db_(db)
{
}

bool RealmVaultManager::ValidateHeroName(const std::string &name)
{
	if (name.size() < 2 || name.size() > 15)
		return false;
	for (char c : name) {
		const bool isAlphaNum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		const bool isAllowedPunct = (c == '_' || c == '-');
		if (!isAlphaNum && !isAllowedPunct)
			return false;
	}
	return true;
}

RealmStatus RealmVaultManager::ListHeroes(int64_t accountId, HeroListResponsePayload &outResp) const
{
	outResp = {};
	const std::vector<CharacterRecord> records = db_.ListCharactersForAccount(accountId);
	outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
	outResp.heroCount = static_cast<uint16_t>(std::min<size_t>(records.size(), MaxHeroesPerAccount));

	for (size_t i = 0; i < outResp.heroCount; i++) {
		outResp.heroes[i] = BuildHeroSummary(records[i].id, records[i].pack);
	}
	return RealmStatus::OK;
}

RealmStatus RealmVaultManager::CreateHero(
    int64_t accountId,
    const std::string &heroName,
    uint8_t rawHeroClass,
    bool isHellfire,
    const std::string &remoteIp,
    HeroSummaryInfo &outSummary,
    PlayerPack &outInitialPack)
{
	outSummary = {};
	outInitialPack = {};

	if (!ValidateHeroName(heroName) || rawHeroClass > static_cast<uint8_t>(HeroClass::LAST)) {
		return RealmStatus::ERR_MALFORMED_PACKET;
	}

	const auto heroClass = static_cast<HeroClass>(rawHeroClass);
	const PlayerPack initialPack = BuildInitialPlayerPack(heroName, heroClass, isHellfire);

	int64_t charId = 0;
	RealmStatus status = RealmStatus::OK;
	if (!db_.CreateCharacter(accountId, heroName, rawHeroClass, initialPack, charId, status)) {
		return status;
	}

	// Register all non-empty, non-gold starting items in `item_ledger` for 100% provenance tracking
	for (const auto &bodyItem : initialPack.InvBody) {
		if (bodyItem.idx != EmptyPackedItemIdx && bodyItem.idx != IDI_GOLD) {
			db_.RecordLedgerItem(0, charId, bodyItem, LedgerItemSource::StartingEquipment, 0, LedgerItemState::InInventoryOrStash);
		}
	}
	for (int i = 0; i < initialPack._pNumInv; i++) {
		const auto &invItem = initialPack.InvList[i];
		if (invItem.idx != EmptyPackedItemIdx && invItem.idx != IDI_GOLD) {
			db_.RecordLedgerItem(0, charId, invItem, LedgerItemSource::StartingEquipment, 0, LedgerItemState::InInventoryOrStash);
		}
	}
	for (const auto &beltItem : initialPack.SpdList) {
		if (beltItem.idx != EmptyPackedItemIdx && beltItem.idx != IDI_GOLD) {
			db_.RecordLedgerItem(0, charId, beltItem, LedgerItemSource::StartingEquipment, 0, LedgerItemState::InInventoryOrStash);
		}
	}

	outInitialPack = initialPack;
	outSummary = BuildHeroSummary(charId, initialPack);

	db_.LogSecurityEvent(accountId, charId, 0, "INFO", "VAULT_HERO_CREATE",
	    "Created hero '" + heroName + "' (class=" + std::to_string(rawHeroClass) + ") from " + remoteIp);
	return RealmStatus::OK;
}

RealmStatus RealmVaultManager::DeleteHero(int64_t accountId, int64_t charId, const std::string &remoteIp)
{
	RealmStatus status = RealmStatus::OK;
	if (!db_.DeleteCharacter(accountId, charId, status)) {
		return status;
	}
	db_.LogSecurityEvent(accountId, charId, 0, "INFO", "VAULT_HERO_DELETE",
	    "Deleted hero charId=" + std::to_string(charId) + " from " + remoteIp);
	return RealmStatus::OK;
}

RealmStatus RealmVaultManager::LoadHero(int64_t accountId, int64_t charId, PlayerPack &outPack) const
{
	CharacterRecord rec {};
	RealmStatus status = RealmStatus::OK;
	if (!db_.LoadCharacter(accountId, charId, rec, status)) {
		return status;
	}
	outPack = rec.pack;
	return RealmStatus::OK;
}

bool RealmVaultManager::ValidatePlayerPackSnapshot(
    const CharacterRecord &existing,
    const PlayerPack &incoming,
    std::string &outReason)
{
	if (incoming.pClass != existing.heroClass || incoming.pClass > static_cast<uint8_t>(HeroClass::LAST)) {
		outReason = "Hero class mismatch or out of bounds";
		return false;
	}

	char cleanName[sizeof(incoming.pName)] {};
	std::memcpy(cleanName, incoming.pName, sizeof(cleanName) - 1);
	if (existing.heroName != cleanName) {
		outReason = "Hero name mismatch in PlayerPack";
		return false;
	}

	if (incoming.pLevel < 1 || incoming.pLevel > MaxCharacterLevel || incoming.pLevel < static_cast<int8_t>(existing.heroLevel)) {
		outReason = "Invalid or regressed character level";
		return false;
	}

	if (incoming.plrlevel > MaxDungeonLevel) {
		outReason = "Invalid dungeon level (> 24)";
		return false;
	}

	const auto heroClass = static_cast<HeroClass>(incoming.pClass);
	const HeroClassStatsSpec &spec = GetHeroClassStatsSpec(heroClass);

	if (incoming.pBaseStr < spec.baseStr || incoming.pBaseStr > spec.maxStr
	    || incoming.pBaseMag < spec.baseMag || incoming.pBaseMag > spec.maxMag
	    || incoming.pBaseDex < spec.baseDex || incoming.pBaseDex > spec.maxDex
	    || incoming.pBaseVit < spec.baseVit || incoming.pBaseVit > spec.maxVit) {
		outReason = "Base attribute exceeds class cap or is below starting minimum";
		return false;
	}

	// Total allocated stat points cannot exceed (level - 1) * 5 + maximum possible elixir/quest bonuses
	const int32_t totalBaseGain = (static_cast<int32_t>(incoming.pBaseStr) - spec.baseStr)
	    + (static_cast<int32_t>(incoming.pBaseMag) - spec.baseMag)
	    + (static_cast<int32_t>(incoming.pBaseDex) - spec.baseDex)
	    + (static_cast<int32_t>(incoming.pBaseVit) - spec.baseVit)
	    + static_cast<int32_t>(incoming.pStatPts);
	const int32_t maxLegitLevelPoints = (static_cast<int32_t>(incoming.pLevel) - 1) * 5;
	// Allow elixir gains up to class caps when level >= 15 (elixirs require min mlvl 15 / town witch)
	if (incoming.pLevel < 15 && totalBaseGain > maxLegitLevelPoints + 4) {
		outReason = "Stat point conservation violation for low-level character";
		return false;
	}

	if (incoming.pExperience < 0 || incoming.pExperience < existing.experience) {
		outReason = "Negative or regressed experience points";
		return false;
	}
	const int32_t minExpForLevel = kExpLvlsTbl[incoming.pLevel - 1];
	if (incoming.pExperience < minExpForLevel) {
		outReason = "Character level exceeds experience table threshold";
		return false;
	}

	if (incoming._pNumInv < 0 || incoming._pNumInv > 40) {
		outReason = "Invalid inventory item count (_pNumInv out of 0..40)";
		return false;
	}

	if (incoming.pGold < 0 || incoming.pGold > 2000000) {
		outReason = "Invalid inventory gold total";
		return false;
	}

	// Base life/mana ceiling check (matches pack.cpp ValidateFields)
	// Quest/shrine permanent life bonuses can add up to +(10 << 6)
	const int32_t maxLegitBaseLife = CalculateHeroBaseLife(heroClass, incoming.pLevel, incoming.pBaseVit) + (20 << 6);
	const int32_t maxLegitBaseMana = CalculateHeroBaseMana(heroClass, incoming.pLevel, incoming.pBaseMag) + (20 << 6);
	if (incoming.pMaxHPBase <= 0 || incoming.pMaxHPBase > maxLegitBaseLife || incoming.pHPBase > incoming.pMaxHPBase) {
		outReason = "Base HP exceeds class/vitality formula ceiling";
		return false;
	}
	if (incoming.pMaxManaBase < 0 || incoming.pMaxManaBase > maxLegitBaseMana || incoming.pManaBase > incoming.pMaxManaBase) {
		outReason = "Base Mana exceeds class/magic formula ceiling";
		return false;
	}

	// pDiabloKillLevel (0..4) cannot jump more than +1 tier at a time or exceed 4
	if (incoming.pDiabloKillLevel > 4U || incoming.pDiabloKillLevel > static_cast<uint32_t>(existing.diabloKillLevel) + 1U) {
		outReason = "Invalid pDiabloKillLevel progression jump";
		return false;
	}

	// Entering/saving Torment difficulty kill credit (pDiabloKillLevel == 4) requires prior Hell kill (existing.diabloKillLevel >= 3)
	if (incoming.pDiabloKillLevel == 4U && existing.diabloKillLevel < TormentRequiredDiabloKillLevel) {
		outReason = "Torment kill credit claimed without prior Hell Diablo kill";
		return false;
	}

	// Validate spell levels (`pSplLvl` and `pSplLvl2`) and `pMemSpells`
	for (int s = 0; s < 37; ++s) {
		if (incoming.pSplLvl[s] < 0 || incoming.pSplLvl[s] > 15) {
			outReason = "Spell level out of bounds (0..15) at index " + std::to_string(s);
			return false;
		}
		if (heroClass == HeroClass::Barbarian && incoming.pSplLvl[s] > 0) {
			outReason = "Barbarian class (maxMag=0) cannot learn spellbooks";
			return false;
		}
		if (incoming.pSplLvl[s] > 0 && !(heroClass == HeroClass::Sorcerer && s == SpellIdFirebolt && incoming.pSplLvl[s] <= 2)) {
			// Learning spellbooks requires minimum magic or progression beyond a fresh level-1 character
			if (incoming.pLevel == 1 && incoming.pExperience == 0 && existing.pack.pSplLvl[s] == 0) {
				outReason = "Forged learned spell level on unprogressed level-1 character";
				return false;
			}
			const int prevLvl = std::max<int>(0, existing.pack.pSplLvl[s]);
			if (incoming.pSplLvl[s] > prevLvl + 6) {
				outReason = "Abnormal single-save spell level jump at spell " + std::to_string(s);
				return false;
			}
		}
	}
	for (int s = 0; s < 10; ++s) {
		if (incoming.pSplLvl2[s] < 0 || incoming.pSplLvl2[s] > 15) {
			outReason = "Extended spell level out of bounds (0..15)";
			return false;
		}
		if (heroClass == HeroClass::Barbarian && incoming.pSplLvl2[s] > 0) {
			outReason = "Barbarian class (maxMag=0) cannot learn extended spellbooks";
			return false;
		}
	}
	if (heroClass == HeroClass::Barbarian && incoming.pMemSpells != 0ULL) {
		outReason = "Barbarian class cannot have memorized spellbook bitmask";
		return false;
	}

	// Deterministic 1:1 RecreateServerItem() verification across equipped, inventory, and belt items
	std::array<ServerItem, NUM_INVLOC> recreatedBody {};
	std::array<item_equip_type, NUM_INVLOC> effectiveLoc {};
	for (size_t i = 0; i < NUM_INVLOC; i++) {
		ServerItem recreated {};
		std::string itemErr;
		if (!VerifyPackedItemIntegrity(incoming, incoming.InvBody[i], itemErr, &recreated)) {
			outReason = "Equipped slot " + std::to_string(i) + " failed verification: " + itemErr;
			return false;
		}
		recreatedBody[i] = recreated;
		effectiveLoc[i] = ILOC_NONE;
		if (recreated.isEmpty())
			continue;

		item_equip_type loc = recreated._iLoc;
		if (heroClass == HeroClass::Barbarian && loc == ILOC_TWOHAND && IsAnyOf(recreated._itype, ItemType::Sword, ItemType::Mace)) {
			loc = ILOC_ONEHAND;
		}
		effectiveLoc[i] = loc;
		bool slotOk = false;
		switch (i) {
		case INVLOC_HEAD:
			slotOk = (loc == ILOC_HELM);
			break;
		case INVLOC_RING_LEFT:
		case INVLOC_RING_RIGHT:
			slotOk = (loc == ILOC_RING);
			break;
		case INVLOC_AMULET:
			slotOk = (loc == ILOC_AMULET);
			break;
		case INVLOC_HAND_LEFT:
		case INVLOC_HAND_RIGHT:
			slotOk = IsAnyOf(loc, ILOC_ONEHAND, ILOC_TWOHAND);
			break;
		case INVLOC_CHEST:
			slotOk = (loc == ILOC_ARMOR);
			break;
		default:
			slotOk = false;
			break;
		}
		if (!slotOk) {
			outReason = "Equipped slot " + std::to_string(i) + " holds incompatible item equip location";
			return false;
		}
	}

	// Hand-slot mutual exclusion:
	// 1. If either hand holds an effective ILOC_TWOHAND item, the other hand MUST be empty.
	// 2. Only Bard and Barbarian can dual-wield two non-shield weapons simultaneously.
	const ServerItem &leftHand = recreatedBody[INVLOC_HAND_LEFT];
	const ServerItem &rightHand = recreatedBody[INVLOC_HAND_RIGHT];
	if (!leftHand.isEmpty() && !rightHand.isEmpty()) {
		if (effectiveLoc[INVLOC_HAND_LEFT] == ILOC_TWOHAND || effectiveLoc[INVLOC_HAND_RIGHT] == ILOC_TWOHAND) {
			outReason = "Two-handed weapon equipped alongside a second hand item";
			return false;
		}
		const bool hasShield = (leftHand._itype == ItemType::Shield) || (rightHand._itype == ItemType::Shield);
		const bool canDualWield = (heroClass == HeroClass::Bard) || (heroClass == HeroClass::Barbarian);
		if (!hasShield && !canDualWield) {
			outReason = "Hero class cannot dual-wield two weapons simultaneously";
			return false;
		}
	}

	int32_t calculatedGold = 0;
	for (int i = 0; i < incoming._pNumInv; i++) {
		ServerItem recreated {};
		std::string itemErr;
		if (!VerifyPackedItemIntegrity(incoming, incoming.InvList[i], itemErr, &recreated)) {
			outReason = "Inventory slot " + std::to_string(i) + " failed verification: " + itemErr;
			return false;
		}
		if (!recreated.isEmpty() && recreated.IDidx == IDI_GOLD) {
			calculatedGold += recreated._ivalue;
		}
	}
	if (incoming.pGold != calculatedGold) {
		outReason = "PlayerPack pGold (" + std::to_string(incoming.pGold) + ") does not match sum of gold items in inventory ("
		    + std::to_string(calculatedGold) + ")";
		return false;
	}

	for (int i = 0; i < 8; i++) {
		ServerItem recreated {};
		std::string itemErr;
		if (!VerifyPackedItemIntegrity(incoming, incoming.SpdList[i], itemErr, &recreated)) {
			outReason = "Belt slot " + std::to_string(i) + " failed verification: " + itemErr;
			return false;
		}
		if (recreated.isEmpty())
			continue;
		if (recreated._itype == ItemType::Gold || !recreated.isUsable()) {
			outReason = "Belt slot " + std::to_string(i) + " holds non-usable or gold item";
			return false;
		}
	}

	return true;
}

RealmStatus RealmVaultManager::SaveHero(
    int64_t accountId,
    int64_t charId,
    const PlayerPack &incomingPack,
    const std::string &remoteIp,
    const GamePlayerRuntimeState *activePlayerSession)
{
	CharacterRecord existing {};
	RealmStatus status = RealmStatus::OK;
	if (!db_.LoadCharacter(accountId, charId, existing, status)) {
		return status;
	}

	std::string failureReason;
	if (!AuditCharacterSaveConservation(db_, existing, incomingPack, activePlayerSession, failureReason)) {
		db_.LogSecurityEvent(accountId, charId, 0, "ERROR", "SAVE_AUDIT_FAILED",
		    "Rejected OP_HERO_SAVE_REQ for '" + existing.heroName + "' from " + remoteIp + ": " + failureReason);
		return RealmStatus::ERR_SAVE_AUDIT_FAILED;
	}

	if (!db_.SaveCharacter(accountId, charId, incomingPack, status)) {
		return status;
	}

	return RealmStatus::OK;
}

RealmStatus RealmVaultManager::LoadStash(
    int64_t accountId,
    uint32_t &outStashGold,
    std::vector<PkItemStruct> &outItems) const
{
	RealmStatus status = RealmStatus::OK;
	db_.LoadStash(accountId, outStashGold, outItems, status);
	return status;
}

RealmStatus RealmVaultManager::SaveStash(
    int64_t accountId,
    uint32_t stashGold,
    const std::vector<PkItemStruct> &items,
    const std::string &remoteIp)
{
	if (items.size() > MaxStashItems || stashGold > 50000000U) {
		db_.LogSecurityEvent(accountId, 0, 0, "WARN", "STASH_SAVE_INVALID",
		    "Rejected invalid stash save from " + remoteIp);
		return RealmStatus::ERR_SAVE_AUDIT_FAILED;
	}

	// Verify Stash Gold Conservation against existing stash + account characters' gold
	uint32_t existingStashGold = 0;
	std::vector<PkItemStruct> existingStashItems;
	RealmStatus loadStatus = RealmStatus::OK;
	db_.LoadStash(accountId, existingStashGold, existingStashItems, loadStatus);

	uint64_t totalAccountHeroGold = 0;
	const auto acctHeroes = db_.ListCharactersForAccount(accountId);
	for (const auto &h : acctHeroes) {
		totalAccountHeroGold += static_cast<uint64_t>(std::max<int32_t>(0, h.gold));
	}
	const uint64_t maxAllowedStashGold = static_cast<uint64_t>(existingStashGold) + totalAccountHeroGold + 350000ULL;
	if (static_cast<uint64_t>(stashGold) > maxAllowedStashGold) {
		db_.LogSecurityEvent(accountId, 0, 0, "ERROR", "STASH_GOLD_CONSERVATION_FAILED",
		    "Rejected OP_STASH_SAVE_REQ gold spike (" + std::to_string(stashGold)
		        + " > allowed " + std::to_string(maxAllowedStashGold) + ") from " + remoteIp);
		return RealmStatus::ERR_SAVE_AUDIT_FAILED;
	}

	using StashKey = std::tuple<uint16_t, uint16_t, uint32_t>;
	std::map<StashKey, int64_t> stashCopies;
	std::map<StashKey, PkItemStruct> stashSamples;

	for (size_t i = 0; i < items.size(); i++) {
		if (items[i].idx == EmptyPackedItemIdx)
			continue;
		PlayerPack stashCtx {};
		stashCtx.pLevel = 50;
		stashCtx.pMaxHPBase = (100 << 6);
		stashCtx.pMaxManaBase = (100 << 6);
		stashCtx.bIsHellfire = ((items[i].dwBuff & CF_HELLFIRE) != 0) ? 1 : 0;
		stashCtx.pDifficulty = ((items[i].dwBuff & CF_TORMENT) != 0) ? DIFF_TORMENT : DIFF_NORMAL;

		ServerItem recreated {};
		std::string itemErr;
		if (!VerifyPackedItemIntegrity(stashCtx, items[i], itemErr, &recreated)) {
			db_.LogSecurityEvent(accountId, 0, 0, "ERROR", "STASH_ITEM_AUDIT_FAILED",
			    "Rejected OP_STASH_SAVE_REQ item[" + std::to_string(i) + "] from " + remoteIp + ": " + itemErr);
			return RealmStatus::ERR_SAVE_AUDIT_FAILED;
		}

		const bool needsLedgerProvenance = !recreated.isEmpty()
		    && recreated._itype != ItemType::Gold
		    && ((items[i].iCreateInfo & CF_TOWN) == 0)
		    && !recreated.isUsable()
		    && recreated._itype != ItemType::Misc;
		if (needsLedgerProvenance) {
			const StashKey k { items[i].idx, items[i].iCreateInfo, items[i].iSeed };
			stashCopies[k]++;
			stashSamples[k] = items[i];
		}
	}

	// Enforce 100% Account Item Ledger Provenance on Stash (blocks Stash item cloning)
	for (const auto &[k, count] : stashCopies) {
		const PkItemStruct &sample = stashSamples[k];
		const int64_t allowedAccountCopies = db_.CountAccountLedgerCopies(accountId, sample);
		if (count > allowedAccountCopies) {
			db_.LogSecurityEvent(accountId, 0, 0, "ERROR", "STASH_DUPE_DETECTED",
			    "Rejected OP_STASH_SAVE_REQ cloned item (seed=" + std::to_string(sample.iSeed)
			        + ", copies=" + std::to_string(count) + ", allowed=" + std::to_string(allowedAccountCopies)
			        + ") from " + remoteIp);
			return RealmStatus::ERR_SAVE_AUDIT_FAILED;
		}
	}

	RealmStatus status = RealmStatus::OK;
	db_.SaveStash(accountId, stashGold, items, status);
	return status;
}

bool RealmVaultManager::AdminPromoteHeroToTorment(
    int64_t accountId,
    int64_t charId,
    int8_t minLevel,
    uint32_t minDiabloKillLevel,
    std::string &outMessage)
{
	CharacterRecord rec {};
	RealmStatus status = RealmStatus::OK;
	if (!db_.LoadCharacter(accountId, charId, rec, status)) {
		outMessage = "Character not found (charId=" + std::to_string(charId) + ")";
		return false;
	}

	PlayerPack pack = rec.pack;
	const int8_t targetLvl = std::clamp<int8_t>(minLevel, 1, MaxCharacterLevel);
	if (pack.pLevel < targetLvl) {
		const int32_t levelsGained = targetLvl - pack.pLevel;
		pack.pLevel = targetLvl;
		pack.pStatPts += levelsGained * 5;
	}

	const int32_t minExp = static_cast<int32_t>(kExpLvlsTbl[static_cast<size_t>(pack.pLevel - 1)]);
	if (pack.pExperience < minExp) {
		pack.pExperience = minExp;
	}

	const auto heroClass = static_cast<HeroClass>(pack.pClass);
	const int32_t expectedLife = CalculateHeroBaseLife(heroClass, pack.pLevel, pack.pBaseVit);
	const int32_t expectedMana = CalculateHeroBaseMana(heroClass, pack.pLevel, pack.pBaseMag);
	pack.pMaxHPBase = std::max<int32_t>(pack.pMaxHPBase, expectedLife);
	pack.pHPBase = pack.pMaxHPBase;
	pack.pMaxManaBase = std::max<int32_t>(pack.pMaxManaBase, expectedMana);
	pack.pManaBase = pack.pMaxManaBase;

	const uint32_t targetKills = std::min<uint32_t>(4U, minDiabloKillLevel);
	if (pack.pDiabloKillLevel < targetKills) {
		pack.pDiabloKillLevel = targetKills;
	}

	if (!db_.SaveCharacter(accountId, charId, pack, status)) {
		outMessage = "Database SaveCharacter failed during admin promote";
		return false;
	}

	outMessage = "Promoted '" + rec.heroName + "' to Level " + std::to_string(pack.pLevel)
	    + " (pDiabloKillLevel=" + std::to_string(pack.pDiabloKillLevel) + ", Torment Unlocked)";
	db_.LogSecurityEvent(accountId, charId, 0, "INFO", "ADMIN_PROMOTE_TORMENT", outMessage);
	return true;
}

bool RealmVaultManager::AdminImportCharacterPack(
    int64_t accountId,
    const PlayerPack &rawImportedPack,
    int64_t &outCharId,
    std::string &outMessage)
{
	outCharId = 0;
	PlayerPack pack = rawImportedPack;

	if (pack.pClass > static_cast<uint8_t>(HeroClass::LAST)) {
		outMessage = "Invalid heroClass in imported PlayerPack";
		return false;
	}

	char rawName[sizeof(pack.pName)] {};
	std::memcpy(rawName, pack.pName, sizeof(rawName) - 1);
	std::string heroName = rawName;
	if (!ValidateHeroName(heroName)) {
		outMessage = "Invalid hero name '" + heroName + "' in imported PlayerPack (must be 2..15 [A-Za-z0-9_-])";
		return false;
	}

	const auto heroClass = static_cast<HeroClass>(pack.pClass);
	const HeroClassStatsSpec &spec = GetHeroClassStatsSpec(heroClass);

	pack.pLevel = std::clamp<int8_t>(pack.pLevel, 1, MaxCharacterLevel);
	pack.plrlevel = 0; // Place imported hero safely in Tristram Town
	pack.pBaseStr = std::clamp<uint8_t>(pack.pBaseStr, spec.baseStr, spec.maxStr);
	pack.pBaseMag = std::clamp<uint8_t>(pack.pBaseMag, spec.baseMag, spec.maxMag);
	pack.pBaseDex = std::clamp<uint8_t>(pack.pBaseDex, spec.baseDex, spec.maxDex);
	pack.pBaseVit = std::clamp<uint8_t>(pack.pBaseVit, spec.baseVit, spec.maxVit);
	pack.pDiabloKillLevel = std::min<uint32_t>(pack.pDiabloKillLevel, 4U);
	pack._pNumInv = std::clamp<int8_t>(pack._pNumInv, 0, 40);

	const int32_t minExp = static_cast<int32_t>(kExpLvlsTbl[static_cast<size_t>(pack.pLevel - 1)]);
	if (pack.pExperience < minExp) {
		pack.pExperience = minExp;
	}

	const int32_t expectedLife = CalculateHeroBaseLife(heroClass, pack.pLevel, pack.pBaseVit);
	const int32_t expectedMana = CalculateHeroBaseMana(heroClass, pack.pLevel, pack.pBaseMag);
	pack.pMaxHPBase = std::clamp<int32_t>(pack.pMaxHPBase, expectedLife, expectedLife + (20 << 6));
	pack.pHPBase = pack.pMaxHPBase;
	pack.pMaxManaBase = std::clamp<int32_t>(pack.pMaxManaBase, expectedMana, expectedMana + (20 << 6));
	pack.pManaBase = pack.pMaxManaBase;

	// Strip Single Player `CF_PREGEN` flag (`0x8000`) from all equipped, inventory, and belt items
	// so Single Player (`single_*.sv`) characters convert cleanly into Closed Realm characters!
	for (auto &bodyItem : pack.InvBody) {
		if (bodyItem.idx != EmptyPackedItemIdx && bodyItem.idx != IDI_GOLD) {
			bodyItem.iCreateInfo = static_cast<uint16_t>(bodyItem.iCreateInfo & ~CF_PREGEN);
			std::string itemErr;
			if (!VerifyPackedItemIntegrity(pack, bodyItem, itemErr)) {
				outMessage = "Imported equipped item (idx=" + std::to_string(bodyItem.idx) + ") failed verification: " + itemErr;
				return false;
			}
		}
	}

	int64_t sumInvGold = 0;
	for (int i = 0; i < pack._pNumInv; ++i) {
		auto &invItem = pack.InvList[i];
		if (invItem.idx == EmptyPackedItemIdx)
			continue;
		if (invItem.idx != IDI_GOLD) {
			invItem.iCreateInfo = static_cast<uint16_t>(invItem.iCreateInfo & ~CF_PREGEN);
		}
		ServerItem recreated {};
		std::string itemErr;
		if (!VerifyPackedItemIntegrity(pack, invItem, itemErr, &recreated)) {
			outMessage = "Imported inventory item[" + std::to_string(i) + "] failed verification: " + itemErr;
			return false;
		}
		if (recreated._itype == ItemType::Gold) {
			sumInvGold += invItem.wValue;
		}
	}
	pack.pGold = static_cast<int32_t>(sumInvGold);

	for (auto &beltItem : pack.SpdList) {
		if (beltItem.idx != EmptyPackedItemIdx && beltItem.idx != IDI_GOLD) {
			beltItem.iCreateInfo = static_cast<uint16_t>(beltItem.iCreateInfo & ~CF_PREGEN);
			std::string itemErr;
			if (!VerifyPackedItemIntegrity(pack, beltItem, itemErr)) {
				outMessage = "Imported belt item failed verification: " + itemErr;
				return false;
			}
		}
	}

	// Check if account already owns a character with this name; if so, overwrite it, otherwise create a new row
	int64_t charId = 0;
	const auto existingHeroes = db_.ListCharactersForAccount(accountId);
	for (const auto &h : existingHeroes) {
		if (h.heroName == heroName) {
			charId = h.id;
			break;
		}
	}

	RealmStatus status = RealmStatus::OK;
	if (charId > 0) {
		db_.ClearCharacterLedgerItems(charId);
		if (!db_.SaveCharacter(accountId, charId, pack, status)) {
			outMessage = "Failed to update existing character '" + heroName + "'";
			return false;
		}
	} else {
		if (!db_.CreateCharacter(accountId, heroName, pack.pClass, pack, charId, status)) {
			outMessage = "Failed to create imported character '" + heroName + "' (name may already exist on another account)";
			return false;
		}
	}

	// Register 100% of imported equipped, inventory, and belt items in `item_ledger`
	int64_t ledgerRegistered = 0;
	for (const auto &bodyItem : pack.InvBody) {
		if (bodyItem.idx != EmptyPackedItemIdx && bodyItem.idx != IDI_GOLD) {
			db_.RecordLedgerItem(0, charId, bodyItem, LedgerItemSource::StartingEquipment, 0, LedgerItemState::InInventoryOrStash);
			++ledgerRegistered;
		}
	}
	for (int i = 0; i < pack._pNumInv; ++i) {
		const auto &invItem = pack.InvList[i];
		if (invItem.idx != EmptyPackedItemIdx && invItem.idx != IDI_GOLD) {
			db_.RecordLedgerItem(0, charId, invItem, LedgerItemSource::StartingEquipment, 0, LedgerItemState::InInventoryOrStash);
			++ledgerRegistered;
		}
	}
	for (const auto &beltItem : pack.SpdList) {
		if (beltItem.idx != EmptyPackedItemIdx && beltItem.idx != IDI_GOLD) {
			db_.RecordLedgerItem(0, charId, beltItem, LedgerItemSource::StartingEquipment, 0, LedgerItemState::InInventoryOrStash);
			++ledgerRegistered;
		}
	}

	outCharId = charId;
	outMessage = "Imported hero '" + heroName + "' (charId=" + std::to_string(charId)
	    + ", Lvl=" + std::to_string(pack.pLevel) + ", DiabloKillLevel=" + std::to_string(pack.pDiabloKillLevel)
	    + ", LedgerItems=" + std::to_string(ledgerRegistered) + ")";
	db_.LogSecurityEvent(accountId, charId, 0, "INFO", "ADMIN_IMPORT_HERO", outMessage);
	return true;
}

bool RealmVaultManager::AdminImportStash(
    int64_t accountId,
    uint32_t stashGold,
    const std::vector<PkItemStruct> &rawItems,
    std::string &outMessage)
{
	if (rawItems.size() > MaxStashItems || stashGold > 50000000U) {
		outMessage = "Imported stash exceeds maximum item count (400) or gold limit";
		return false;
	}

	const auto heroes = db_.ListCharactersForAccount(accountId);
	if (heroes.empty()) {
		outMessage = "Import or create at least one character on this account before importing a Stash (needed for account item_ledger anchor)";
		return false;
	}
	const int64_t anchorCharId = heroes.front().id;

	std::vector<PkItemStruct> cleanItems;
	cleanItems.reserve(rawItems.size());

	int64_t ledgerRegistered = 0;
	for (size_t i = 0; i < rawItems.size(); ++i) {
		PkItemStruct item = rawItems[i];
		if (item.idx == EmptyPackedItemIdx)
			continue;

		// Strip Single Player `CF_PREGEN` flag (`0x8000`)
		if (item.idx != IDI_GOLD) {
			item.iCreateInfo = static_cast<uint16_t>(item.iCreateInfo & ~CF_PREGEN);
		}

		PlayerPack stashCtx {};
		stashCtx.pLevel = 50;
		stashCtx.pMaxHPBase = (100 << 6);
		stashCtx.pMaxManaBase = (100 << 6);
		stashCtx.bIsHellfire = ((item.dwBuff & CF_HELLFIRE) != 0) ? 1 : 0;
		stashCtx.pDifficulty = ((item.dwBuff & CF_TORMENT) != 0) ? DIFF_TORMENT : DIFF_NORMAL;

		ServerItem recreated {};
		std::string itemErr;
		if (!VerifyPackedItemIntegrity(stashCtx, item, itemErr, &recreated)) {
			outMessage = "Imported stash item[" + std::to_string(i) + "] failed verification: " + itemErr;
			return false;
		}

		cleanItems.push_back(item);
		if (item.idx != IDI_GOLD) {
			db_.RecordLedgerItem(0, anchorCharId, item, LedgerItemSource::StartingEquipment, 0, LedgerItemState::InInventoryOrStash);
			++ledgerRegistered;
		}
	}

	RealmStatus status = RealmStatus::OK;
	if (!db_.SaveStash(accountId, stashGold, cleanItems, status)) {
		outMessage = "Database SaveStash failed during admin stash import";
		return false;
	}

	outMessage = "Imported Stash for accountId=" + std::to_string(accountId)
	    + " (gold=" + std::to_string(stashGold) + ", items=" + std::to_string(cleanItems.size())
	    + ", ledgerRegistered=" + std::to_string(ledgerRegistered) + ")";
	db_.LogSecurityEvent(accountId, anchorCharId, 0, "INFO", "ADMIN_IMPORT_STASH", outMessage);
	return true;
}

} // namespace devilution::realm
