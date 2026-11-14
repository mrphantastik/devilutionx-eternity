/**
 * @file item_verifier.hpp
 * @brief Deterministic 1:1 DX:Eternity item recreation (RecreateItem) and anti-cheat verification engine.
 */
#pragma once

#include <cstdint>
#include <string>

#include "game/item_tables.hpp"
#include "game/lcg.hpp"
#include "protocol/realm_protocol.hpp"

namespace devilution::realm {

constexpr int GOLD_SMALL_LIMIT = 1000;
constexpr int GOLD_MEDIUM_LIMIT = 2500;
constexpr int GOLD_MAX_LIMIT = 5000;

struct ServerPlayerContext {
	HeroClass pClass = HeroClass::Warrior;
	int8_t pLevel = 1;
	int32_t pMaxHPBase = (20 << 6);
	int32_t pMaxManaBase = (10 << 6);
	uint8_t pBaseStr = 30;
	uint8_t pBaseMag = 10;
	uint8_t pBaseDex = 20;
	uint8_t pBaseVit = 25;
	bool isHellfire = false;
	_difficulty nDifficulty = DIFF_NORMAL;

	static ServerPlayerContext FromPlayerPack(const PlayerPack &pack);
};

struct ServerItem {
	_item_indexes IDidx = IDI_NONE;
	ItemType _itype = ItemType::None;
	item_class _iClass = ICLASS_NONE;
	item_equip_type _iLoc = ILOC_NONE;
	int _iCurs = 0;
	char _iName[64] {};
	char _iIName[128] {};

	uint32_t _iSeed = 0;
	uint16_t _iCreateInfo = 0;
	uint32_t dwBuff = 0;

	item_quality _iMagical = ITEM_QUALITY_NORMAL;
	bool _iIdentified = false;
	bool _iStatFlag = true;

	int _iMinDam = 0;
	int _iMaxDam = 0;
	int _iAC = 0;
	int _iDurability = 0;
	int _iMaxDur = 0;
	int _iCharges = 0;
	int _iMaxCharges = 0;

	uint8_t _iMinStr = 0;
	uint8_t _iMinMag = 0;
	uint8_t _iMinDex = 0;

	int _ivalue = 0;
	int _iIvalue = 0;
	int _iVAdd1 = 0;
	int _iVMult1 = 0;
	int _iVAdd2 = 0;
	int _iVMult2 = 0;

	item_effect_type _iPrePower = IPL_INVALID;
	item_effect_type _iSufPower = IPL_INVALID;
	_unique_items _iUid = UITEM_INVALID;
	item_misc_id _iMiscId = IMISC_NONE;
	SpellID _iSpell = SpellID::Null;

	ItemSpecialEffect _iFlags = ItemSpecialEffect::None;
	ItemSpecialEffectHf _iDamAcFlags = ItemSpecialEffectHf::None;

	int _iPLToHit = 0;
	int _iPLDam = 0;
	int _iPLAC = 0;
	int _iPLFR = 0;
	int _iPLLR = 0;
	int _iPLMR = 0;
	int _iPLStr = 0;
	int _iPLMag = 0;
	int _iPLDex = 0;
	int _iPLVit = 0;
	int _iPLGetHit = 0;
	int _iPLHP = 0;
	int _iPLMana = 0;
	int _iSplLvlAdd = 0;
	int _iPLEnAc = 0;
	int _iPLLight = 0;
	int _iPLDamMod = 0;
	int _iFMinDam = 0;
	int _iFMaxDam = 0;
	int _iLMinDam = 0;
	int _iLMaxDam = 0;

	[[nodiscard]] bool isEmpty() const
	{
		return _itype == ItemType::None;
	}

	void clear()
	{
		*this = ServerItem {};
	}

	[[nodiscard]] bool isUsable() const
	{
		if (IDidx < 0 || IDidx > IDI_LAST)
			return false;
		return AllItemsList[IDidx].iUsable;
	}
};

struct ComputedPlayerCombatStats {
	int32_t strength = 0;
	int32_t magic = 0;
	int32_t dexterity = 0;
	int32_t vitality = 0;
	int32_t minDam = 1;
	int32_t maxDam = 1;
	int32_t bonusDamPercent = 0;
	int32_t bonusDamMod = 0;
	int32_t classDamageMod = 0;
	int32_t bonusToHit = 0;
	int32_t totalAC = 0;
	int32_t fireMinDam = 0;
	int32_t fireMaxDam = 0;
	int32_t lightMinDam = 0;
	int32_t lightMaxDam = 0;
	int32_t spellLvlAdd = 0;
	ItemSpecialEffect flags = ItemSpecialEffect::None;
	ItemSpecialEffectHf hfFlags = ItemSpecialEffectHf::None;
	int32_t maxSingleHitCeiling = 10;
};

_item_indexes RemapItemIdxFromDiablo(_item_indexes i);
_item_indexes RemapItemIdxToDiablo(_item_indexes i);

bool IsItemAvailable(int i, bool isHellfire = false);
bool IsUniqueAvailable(int i, bool isHellfire = false);

int GetSpellBookLevel(SpellID s, bool isHellfire = false);
int GetSpellStaffLevel(SpellID s, bool isHellfire = false);

bool IsPrefixValidForItemType(int i, AffixItemType flgs, bool hellfireItem);
bool IsSuffixValidForItemType(int i, AffixItemType flgs, bool hellfireItem);

void InitializeServerItem(ServerItem &item, _item_indexes itemData, bool isHellfire = false, _difficulty difficulty = DIFF_NORMAL);
void GetItemAttrs(LcgEngine &lcg, ServerItem &item, _item_indexes itemData, int lvl, bool isHellfire = false, _difficulty difficulty = DIFF_NORMAL);
int SaveItemPower(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, ItemPower power);
void GetItemBonus(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, int minlvl, int maxlvl, bool onlygood, bool allowspells);
_unique_items CheckUnique(LcgEngine &lcg, ServerItem &item, int lvl, int uper, bool recreate, bool isHellfire = false);
void GetUniqueItem(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, _unique_items uid);
void ItemRndDur(LcgEngine &lcg, ServerItem &item, _difficulty difficulty = DIFF_NORMAL);

void SetupAllItems(
    LcgEngine &lcg,
    const ServerPlayerContext &player,
    ServerItem &item,
    _item_indexes idx,
    uint32_t iseed,
    int lvl,
    int uper,
    bool onlygood,
    bool recreate,
    bool pregen);

void SetupAllUseful(
    LcgEngine &lcg,
    ServerItem &item,
    uint32_t iseed,
    int lvl,
    bool isHellfire = false,
    _difficulty difficulty = DIFF_NORMAL);

void SpawnUniqueServerItem(
    LcgEngine &lcg,
    const ServerPlayerContext &player,
    ServerItem &item,
    _unique_items uid,
    int curlv);

void RecreateServerItem(
    LcgEngine &lcg,
    const ServerPlayerContext &player,
    ServerItem &item,
    _item_indexes idx,
    uint16_t icreateinfo,
    uint32_t iseed,
    int ivalue,
    bool isHellfire);

void RecreateServerEar(ServerItem &item, uint16_t ic, uint32_t iseed, uint8_t bCursval, const char *heroName);

void UnPackServerItem(const PkItemStruct &packedItem, const ServerPlayerContext &player, ServerItem &outItem);
void PackServerItem(PkItemStruct &outPackedItem, const ServerItem &item, bool isHellfire);

// Item validation rules matching Source/items/validation.cpp + Closed Realm seed/durability/charge verification
bool IsCreationFlagComboValid(uint16_t iCreateInfo);
bool IsTownItemValid(uint16_t iCreateInfo);
bool IsShopPriceValid(const ServerItem &item, bool isHellfire);
bool IsUniqueMonsterItemValid(uint16_t iCreateInfo, uint32_t dwBuff);
bool IsDungeonItemValid(uint16_t iCreateInfo, uint32_t dwBuff);
bool IsHellfireSpellBookValid(const ServerItem &spellBook, bool isHellfire);
bool IsServerItemValid(const ServerItem &item, bool isHellfire);

/**
 * @brief Verifies a packed PkItemStruct against deterministic 1:1 RecreateServerItem() output.
 * Rejects forged seeds, impossible creation flags, modified max durability/charges, or quality bit tampering.
 */
bool VerifyPackedItemIntegrity(
    const PlayerPack &ownerPack,
    const PkItemStruct &packedItem,
    std::string &outError,
    ServerItem *outRecreated = nullptr);

/**
 * @brief Computes effective player attributes and single-hit damage ceiling from equipped items in PlayerPack.
 */
ComputedPlayerCombatStats CalculatePlayerEquippedStats(const PlayerPack &pack);

} // namespace devilution::realm
