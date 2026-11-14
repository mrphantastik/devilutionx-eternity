/**
 * @file item_verifier.cpp
 * @brief Deterministic 1:1 DX:Eternity item recreation (RecreateItem) and anti-cheat verification engine.
 */
#include "game/item_verifier.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <functional>

#include "vault/realm_vault.hpp"

namespace devilution::realm {

namespace {

constexpr int MaxCharacterLevel = 50;

void CopyStrBounded(char *dst, size_t dstSize, const char *src)
{
	if (dstSize == 0)
		return;
	if (src == nullptr) {
		dst[0] = '\0';
		return;
	}
	std::snprintf(dst, dstSize, "%s", src);
}

int OilLevels[] = { 1, 10, 1, 10, 4, 1, 5, 17, 1, 10 };
int OilValues[] = { 500, 2500, 500, 2500, 1500, 100, 2500, 15000, 500, 2500 };
item_misc_id OilMagic[] = {
	IMISC_OILACC,
	IMISC_OILMAST,
	IMISC_OILSHARP,
	IMISC_OILDEATH,
	IMISC_OILSKILL,
	IMISC_OILBSMTH,
	IMISC_OILFORT,
	IMISC_OILPERM,
	IMISC_OILHARD,
	IMISC_OILIMP,
};
const char *const OilNames[10] = {
	"Oil of Accuracy",
	"Oil of Mastery",
	"Oil of Sharpness",
	"Oil of Death",
	"Oil of Skill",
	"Blacksmith Oil",
	"Oil of Fortitude",
	"Oil of Permanence",
	"Oil of Hardening",
	"Oil of Imperviousness"
};

int GetGoldCursor(int value)
{
	if (value >= GOLD_MEDIUM_LIMIT)
		return ICURS_GOLD_LARGE;
	if (value <= GOLD_SMALL_LIMIT)
		return ICURS_GOLD_SMALL;
	return ICURS_GOLD_MEDIUM;
}

void CalcItemValue(ServerItem &item)
{
	int v = item._iVMult1 + item._iVMult2;
	if (v > 0) {
		v *= item._ivalue;
	}
	if (v < 0) {
		v = item._ivalue / v;
	}
	v = item._iVAdd1 + item._iVAdd2 + v;
	item._iIvalue = std::max(v, 1);
}

int PLVal(int pv, int p1, int p2, int minv, int maxv)
{
	if (p1 == p2)
		return minv;
	if (minv == maxv)
		return minv;
	return minv + (maxv - minv) * (100 * (pv - p1) / (p2 - p1)) / 100;
}

int RndPL(LcgEngine &lcg, int param1, int param2, bool tormentMaxRoll)
{
	const int r = param1 + lcg.GenerateRnd(param2 - param1 + 1);
	return tormentMaxRoll ? param2 : r;
}

int CalculateToHitBonus(LcgEngine &lcg, int level, bool tormentMaxRoll)
{
	switch (level) {
	case -50:
		return -RndPL(lcg, 6, 10, tormentMaxRoll);
	case -25:
		return -RndPL(lcg, 1, 5, tormentMaxRoll);
	case 20:
		return RndPL(lcg, 1, 5, tormentMaxRoll);
	case 36:
		return RndPL(lcg, 6, 10, tormentMaxRoll);
	case 51:
		return RndPL(lcg, 11, 15, tormentMaxRoll);
	case 66:
		return RndPL(lcg, 16, 20, tormentMaxRoll);
	case 81:
		return RndPL(lcg, 21, 30, tormentMaxRoll);
	case 96:
		return RndPL(lcg, 31, 40, tormentMaxRoll);
	case 111:
		return RndPL(lcg, 41, 50, tormentMaxRoll);
	case 126:
		return RndPL(lcg, 51, 75, tormentMaxRoll);
	case 151:
		return RndPL(lcg, 76, 100, tormentMaxRoll);
	default:
		return 0;
	}
}

void SaveItemAffix(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, const PLStruct &affix)
{
	auto power = affix.power;
	int value = SaveItemPower(lcg, player, item, power);

	value = PLVal(value, power.param1, power.param2, affix.minVal, affix.maxVal);
	if (item._iVAdd1 != 0 || item._iVMult1 != 0) {
		item._iVAdd2 = value;
		item._iVMult2 = affix.multVal;
	} else {
		item._iVAdd1 = value;
		item._iVMult1 = affix.multVal;
	}
}

int GetStaffPrefixId(LcgEngine &lcg, int lvl, bool onlygood, bool hellfireItem)
{
	int preidx = -1;
	if (lcg.FlipCoin(10) || onlygood) {
		int nl = 0;
		int l[256];
		for (int j = 0; ItemPrefixes[j].power.type != IPL_INVALID; j++) {
			if (!IsPrefixValidForItemType(j, AffixItemType::Staff, hellfireItem) || ItemPrefixes[j].PLMinLvl > lvl)
				continue;
			if (onlygood && !ItemPrefixes[j].PLOk)
				continue;
			l[nl] = j;
			nl++;
			if (ItemPrefixes[j].PLDouble) {
				l[nl] = j;
				nl++;
			}
		}
		if (nl != 0) {
			preidx = l[lcg.GenerateRnd(nl)];
		}
	}
	return preidx;
}

void GetStaffPower(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, int lvl, SpellID bs, bool onlygood)
{
	(void)bs;
	const int preidx = GetStaffPrefixId(lcg, lvl, onlygood, player.isHellfire);
	if (preidx != -1) {
		item._iMagical = ITEM_QUALITY_MAGIC;
		SaveItemAffix(lcg, player, item, ItemPrefixes[preidx]);
		item._iPrePower = ItemPrefixes[preidx].power.type;
	}

	const ItemData &baseItemData = AllItemsList[item.IDidx];
	const char *spellName = GetServerSpellData(item._iSpell).sNameText != nullptr ? GetServerSpellData(item._iSpell).sNameText : "";
	std::snprintf(item._iName, sizeof(item._iName), "%s of %s", baseItemData.iName, spellName);
	if (preidx != -1) {
		std::snprintf(item._iIName, sizeof(item._iIName), "%s %s of %s", ItemPrefixes[preidx].PLName, baseItemData.iName, spellName);
	} else {
		CopyStrBounded(item._iIName, sizeof(item._iIName), item._iName);
	}

	CalcItemValue(item);
}

void GetItemPowerPrefixAndSuffix(
    LcgEngine &lcg,
    int minlvl,
    int maxlvl,
    AffixItemType flgs,
    bool onlygood,
    bool hellfireItem,
    const std::function<void(const PLStruct &)> &prefixFound,
    const std::function<void(const PLStruct &)> &suffixFound)
{
	int preidx = -1;
	int sufidx = -1;
	int l[256];
	goodorevil goe;

	bool allocatePrefix = lcg.FlipCoin(4);
	bool allocateSuffix = !lcg.FlipCoin(3);
	if (!allocatePrefix && !allocateSuffix) {
		if (lcg.FlipCoin())
			allocatePrefix = true;
		else
			allocateSuffix = true;
	}
	goe = GOE_ANY;
	if (!onlygood && !lcg.FlipCoin(3))
		onlygood = true;
	if (allocatePrefix) {
		int nt = 0;
		for (int j = 0; ItemPrefixes[j].power.type != IPL_INVALID; j++) {
			if (!IsPrefixValidForItemType(j, flgs, hellfireItem))
				continue;
			if (ItemPrefixes[j].PLMinLvl < minlvl || ItemPrefixes[j].PLMinLvl > maxlvl)
				continue;
			if (onlygood && !ItemPrefixes[j].PLOk)
				continue;
			if (HasAnyOf(flgs, AffixItemType::Staff) && ItemPrefixes[j].power.type == IPL_CHARGES)
				continue;
			l[nt] = j;
			nt++;
			if (ItemPrefixes[j].PLDouble) {
				l[nt] = j;
				nt++;
			}
		}
		if (nt != 0) {
			preidx = l[lcg.GenerateRnd(nt)];
			goe = ItemPrefixes[preidx].PLGOE;
			prefixFound(ItemPrefixes[preidx]);
		}
	}
	if (allocateSuffix) {
		int nl = 0;
		for (int j = 0; ItemSuffixes[j].power.type != IPL_INVALID; j++) {
			if (IsSuffixValidForItemType(j, flgs, hellfireItem)
			    && ItemSuffixes[j].PLMinLvl >= minlvl && ItemSuffixes[j].PLMinLvl <= maxlvl
			    && !((goe == GOE_GOOD && ItemSuffixes[j].PLGOE == GOE_EVIL) || (goe == GOE_EVIL && ItemSuffixes[j].PLGOE == GOE_GOOD))
			    && (!onlygood || ItemSuffixes[j].PLOk)) {
				l[nl] = j;
				nl++;
			}
		}
		if (nl != 0) {
			sufidx = l[lcg.GenerateRnd(nl)];
			suffixFound(ItemSuffixes[sufidx]);
		}
	}
}

void GetItemPower(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, int minlvl, int maxlvl, AffixItemType flgs, bool onlygood)
{
	const PLStruct *pPrefix = nullptr;
	const PLStruct *pSufix = nullptr;
	GetItemPowerPrefixAndSuffix(
	    lcg,
	    minlvl, maxlvl, flgs, onlygood, player.isHellfire,
	    [&](const PLStruct &prefix) {
		    item._iMagical = ITEM_QUALITY_MAGIC;
		    SaveItemAffix(lcg, player, item, prefix);
		    item._iPrePower = prefix.power.type;
		    pPrefix = &prefix;
	    },
	    [&](const PLStruct &suffix) {
		    item._iMagical = ITEM_QUALITY_MAGIC;
		    SaveItemAffix(lcg, player, item, suffix);
		    item._iSufPower = suffix.power.type;
		    pSufix = &suffix;
	    });

	if (pPrefix != nullptr && pSufix != nullptr) {
		std::snprintf(item._iIName, sizeof(item._iIName), "%s %s of %s", pPrefix->PLName, item._iName, pSufix->PLName);
	} else if (pPrefix != nullptr) {
		std::snprintf(item._iIName, sizeof(item._iIName), "%s %s", pPrefix->PLName, item._iName);
	} else if (pSufix != nullptr) {
		std::snprintf(item._iIName, sizeof(item._iIName), "%s of %s", item._iName, pSufix->PLName);
	} else {
		CopyStrBounded(item._iIName, sizeof(item._iIName), item._iName);
	}

	if (pPrefix != nullptr || pSufix != nullptr)
		CalcItemValue(item);
}

void GetStaffSpell(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, int lvl, bool onlygood)
{
	if (!player.isHellfire && lcg.FlipCoin(4)) {
		GetItemPower(lcg, player, item, lvl / 2, lvl, AffixItemType::Staff, onlygood);
		return;
	}

	const int maxSpells = player.isHellfire ? MAX_SPELLS : 37;
	int l = lvl / 2;
	if (l == 0)
		l = 1;
	int rv = lcg.GenerateRnd(maxSpells) + 1;

	int s = static_cast<int8_t>(SpellID::Firebolt);
	SpellID bs = SpellID::Null;
	while (rv > 0) {
		const int sLevel = GetSpellStaffLevel(static_cast<SpellID>(s), player.isHellfire);
		if (sLevel != -1 && l >= sLevel) {
			rv--;
			bs = static_cast<SpellID>(s);
		}
		s++;
		if (s == maxSpells)
			s = static_cast<int8_t>(SpellID::Firebolt);
	}

	const bool isTormentItem = (player.nDifficulty == DIFF_TORMENT) || ((item.dwBuff & CF_TORMENT) != 0);
	const int minc = GetServerSpellData(bs).sStaffMin;
	const int maxc = GetServerSpellData(bs).sStaffMax - minc + 1;
	item._iSpell = bs;
	const int chargeRoll = minc + lcg.GenerateRnd(maxc);
	item._iCharges = isTormentItem ? GetServerSpellData(bs).sStaffMax : chargeRoll;
	item._iMaxCharges = item._iCharges;

	item._iMinMag = GetServerSpellData(bs).minInt;
	const int v = item._iCharges * GetServerSpellData(bs).staffCost() / 5;
	item._ivalue += v;
	item._iIvalue += v;
	GetStaffPower(lcg, player, item, lvl, bs, onlygood);
}

void GetBookSpell(LcgEngine &lcg, ServerItem &item, int lvl, bool isHellfire)
{
	if (lvl == 0)
		lvl = 1;

	const int maxSpells = isHellfire ? MAX_SPELLS : 37;
	int rv = lcg.GenerateRnd(maxSpells) + 1;

	int s = static_cast<int8_t>(SpellID::Firebolt);
	SpellID bs = SpellID::Firebolt;
	while (rv > 0) {
		const int sLevel = GetSpellBookLevel(static_cast<SpellID>(s), isHellfire);
		if (sLevel != -1 && lvl >= sLevel) {
			rv--;
			bs = static_cast<SpellID>(s);
		}
		s++;
		if (s == maxSpells)
			s = 1;
	}

	const ServerSpellData &spellData = GetServerSpellData(bs);
	const char *spellName = spellData.sNameText != nullptr ? spellData.sNameText : "";
	const size_t nameLen = std::strlen(item._iName);
	if (nameLen < sizeof(item._iName) - 1) {
		std::snprintf(item._iName + nameLen, sizeof(item._iName) - nameLen, "%s", spellName);
	}
	const size_t iNameLen = std::strlen(item._iIName);
	if (iNameLen < sizeof(item._iIName) - 1) {
		std::snprintf(item._iIName + iNameLen, sizeof(item._iIName) - iNameLen, "%s", spellName);
	}

	item._iSpell = bs;
	item._iMinMag = spellData.minInt;
	item._ivalue += spellData.bookCost();
	item._iIvalue += spellData.bookCost();
}

void GetOilType(LcgEngine &lcg, ServerItem &item, int maxLvl)
{
	(void)maxLvl;
	// In multiplayer (Closed Realm), GetOilType always picks among { 5, 6 }
	constexpr int cnt = 2;
	constexpr int8_t rnd[2] = { 5, 6 };
	const int8_t t = rnd[lcg.GenerateRnd(cnt)];

	CopyStrBounded(item._iName, sizeof(item._iName), OilNames[t]);
	CopyStrBounded(item._iIName, sizeof(item._iIName), OilNames[t]);
	item._iMiscId = OilMagic[t];
	item._ivalue = OilValues[t];
	item._iIvalue = OilValues[t];
}

int GetItemBLevel(LcgEngine &lcg, int lvl, item_misc_id miscId, bool onlygood, bool uper15)
{
	int iblvl = -1;
	if (lcg.GenerateRnd(100) <= 10
	    || lcg.GenerateRnd(100) <= lvl
	    || onlygood
	    || IsAnyOf(miscId, IMISC_STAFF, IMISC_RING, IMISC_AMULET)) {
		iblvl = lvl;
	}
	if (uper15)
		iblvl = lvl + 4;
	return iblvl;
}

// Exact droppable filter matching Source/items.cpp IsItemAvailable for GetItemIndexForDroppableItem
bool IsDroppableItemAvailable(int i, bool isHellfire)
{
	if (i < 0 || i > IDI_LAST)
		return false;
	if (isHellfire)
		return true;

	const item_misc_id miscId = AllItemsList[i].iMiscId;
	if ((miscId > IMISC_OILFIRST && miscId < IMISC_OILLAST)
	    || (miscId > IMISC_RUNEFIRST && miscId < IMISC_RUNELAST)) {
		return false;
	}

	return i != IDI_MAPOFDOOM
	    && i != IDI_LGTFORGE
	    && (i < IDI_OIL || i > IDI_GREYSUIT)
	    && (i < 83 || i > 86)
	    && i != 92
	    && (i < 161 || i > 165)
	    && i != IDI_SORCERER
	    && i <= 160;
}

_item_indexes GetItemIndexForDroppableItem(
    LcgEngine &lcg,
    bool isHellfire,
    bool considerDropRate,
    const std::function<bool(const ItemData &)> &isItemOkay)
{
	std::array<_item_indexes, IDI_LAST * 2> ril {};
	size_t ri = 0;

	for (int i = IDI_GOLD; i <= IDI_LAST; i++) {
		if (!IsDroppableItemAvailable(i, isHellfire))
			continue;
		const ItemData &item = AllItemsList[i];
		if (item.iRnd == IDROP_NEVER)
			continue;
		if (!isItemOkay(item))
			continue;
		ril[ri] = static_cast<_item_indexes>(i);
		ri++;
		if (item.iRnd == IDROP_DOUBLE && considerDropRate) {
			ril[ri] = static_cast<_item_indexes>(i);
			ri++;
		}
	}

	if (ri == 0)
		return IDI_GOLD;

	return ril[lcg.GenerateRnd(static_cast<int>(ri))];
}

bool SmithItemOk(const ServerPlayerContext &player, const ItemData &item)
{
	if (item.itype == ItemType::Misc)
		return false;
	if (item.itype == ItemType::Gold)
		return false;
	if (item.itype == ItemType::Staff && (!player.isHellfire || item.iSpell != SpellID::Null))
		return false;
	if (item.itype == ItemType::Ring)
		return false;
	if (item.itype == ItemType::Amulet)
		return false;
	return true;
}

bool PremiumItemOk(const ServerPlayerContext &player, const ItemData &item)
{
	if (item.itype == ItemType::Misc)
		return false;
	if (item.itype == ItemType::Gold)
		return false;
	if (!player.isHellfire && item.itype == ItemType::Staff)
		return false;
	// Multiplayer rules (Closed Realm is always multiplayer)
	if (item.iMiscId == IMISC_OILOF)
		return false;
	if (item.itype == ItemType::Ring)
		return false;
	if (item.itype == ItemType::Amulet)
		return false;
	return true;
}

bool WitchItemOk(const ServerPlayerContext &player, const ItemData &item)
{
	(void)player;
	if (!IsAnyOf(item.itype, ItemType::Misc, ItemType::Staff))
		return false;
	if (item.iMiscId == IMISC_MANA)
		return false;
	if (item.iMiscId == IMISC_FULLMANA)
		return false;
	if (item.iSpell == SpellID::TownPortal)
		return false;
	if (item.iMiscId == IMISC_FULLHEAL)
		return false;
	if (item.iMiscId == IMISC_HEAL)
		return false;
	if (item.iMiscId > IMISC_OILFIRST && item.iMiscId < IMISC_OILLAST)
		return false;
	return true;
}

bool HealerItemOk(const ServerPlayerContext &player, const ItemData &item)
{
	(void)player;
	if (item.itype != ItemType::Misc)
		return false;
	if (item.iMiscId == IMISC_SCROLL)
		return item.iSpell == SpellID::Healing;
	if (item.iMiscId == IMISC_SCROLLT)
		return item.iSpell == SpellID::HealOther; // gbIsMultiplayer is true
	if (item.iMiscId == IMISC_REJUV)
		return true;
	if (item.iMiscId == IMISC_FULLREJUV)
		return true;
	return false;
}

template <bool (*Ok)(const ServerPlayerContext &, const ItemData &), bool ConsiderDropRate = false>
_item_indexes RndVendorItem(LcgEngine &lcg, const ServerPlayerContext &player, int minlvl, int maxlvl)
{
	return GetItemIndexForDroppableItem(lcg, player.isHellfire, ConsiderDropRate, [&](const ItemData &item) {
		if (!Ok(player, item))
			return false;
		if (item.iMinMLvl < minlvl || item.iMinMLvl > maxlvl)
			return false;
		return true;
	});
}

void RecreateSmithItem(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, int lvl, uint32_t iseed)
{
	lcg.SetRndSeed(iseed);
	const _item_indexes itype = RndVendorItem<SmithItemOk, true>(lcg, player, 0, lvl);
	GetItemAttrs(lcg, item, itype, lvl, player.isHellfire, player.nDifficulty);

	item._iSeed = iseed;
	item._iCreateInfo = static_cast<uint16_t>(lvl | CF_SMITH);
	item._iIdentified = true;
}

void RecreatePremiumItem(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, int plvl, uint32_t iseed)
{
	lcg.SetRndSeed(iseed);
	const _item_indexes itype = RndVendorItem<PremiumItemOk>(lcg, player, plvl / 4, plvl);
	GetItemAttrs(lcg, item, itype, plvl, player.isHellfire, player.nDifficulty);
	GetItemBonus(lcg, player, item, plvl / 2, plvl, true, !player.isHellfire);

	item._iSeed = iseed;
	item._iCreateInfo = static_cast<uint16_t>(plvl | CF_SMITHPREMIUM);
	item._iIdentified = true;
}

void RecreateBoyItem(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, int lvl, uint32_t iseed)
{
	lcg.SetRndSeed(iseed);
	const _item_indexes itype = RndVendorItem<PremiumItemOk>(lcg, player, 0, lvl);
	GetItemAttrs(lcg, item, itype, lvl, player.isHellfire, player.nDifficulty);
	GetItemBonus(lcg, player, item, lvl, 2 * lvl, true, true);

	item._iSeed = iseed;
	item._iCreateInfo = static_cast<uint16_t>(lvl | CF_BOY);
	item._iIdentified = true;
}

void RecreateWitchItem(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, _item_indexes idx, int lvl, uint32_t iseed)
{
	if (IsAnyOf(idx, IDI_MANA, IDI_FULLMANA, IDI_PORTAL)) {
		GetItemAttrs(lcg, item, idx, lvl, player.isHellfire, player.nDifficulty);
	} else if (player.isHellfire && idx >= 114 && idx <= 117) {
		lcg.SetRndSeed(iseed);
		lcg.DiscardRandomValues(1);
		GetItemAttrs(lcg, item, idx, lvl, player.isHellfire, player.nDifficulty);
	} else {
		lcg.SetRndSeed(iseed);
		const _item_indexes itype = RndVendorItem<WitchItemOk>(lcg, player, 0, lvl);
		GetItemAttrs(lcg, item, itype, lvl, player.isHellfire, player.nDifficulty);
		int iblvl = -1;
		if (lcg.GenerateRnd(100) <= 5)
			iblvl = 2 * lvl;
		if (iblvl == -1 && item._iMiscId == IMISC_STAFF)
			iblvl = 2 * lvl;
		if (iblvl != -1)
			GetItemBonus(lcg, player, item, iblvl / 2, iblvl, true, true);
	}

	item._iSeed = iseed;
	item._iCreateInfo = static_cast<uint16_t>(lvl | CF_WITCH);
	item._iIdentified = true;
}

void RecreateHealerItem(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, _item_indexes idx, int lvl, uint32_t iseed)
{
	if (IsAnyOf(idx, IDI_HEAL, IDI_FULLHEAL, IDI_RESURRECT)) {
		GetItemAttrs(lcg, item, idx, lvl, player.isHellfire, player.nDifficulty);
	} else {
		lcg.SetRndSeed(iseed);
		const _item_indexes itype = RndVendorItem<HealerItemOk>(lcg, player, 0, lvl);
		GetItemAttrs(lcg, item, itype, lvl, player.isHellfire, player.nDifficulty);
	}

	item._iSeed = iseed;
	item._iCreateInfo = static_cast<uint16_t>(lvl | CF_HEALER);
	item._iIdentified = true;
}

void RecreateTownItem(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, _item_indexes idx, uint16_t icreateinfo, uint32_t iseed)
{
	if ((icreateinfo & CF_SMITH) != 0)
		RecreateSmithItem(lcg, player, item, icreateinfo & CF_LEVEL, iseed);
	else if ((icreateinfo & CF_SMITHPREMIUM) != 0)
		RecreatePremiumItem(lcg, player, item, icreateinfo & CF_LEVEL, iseed);
	else if ((icreateinfo & CF_BOY) != 0)
		RecreateBoyItem(lcg, player, item, icreateinfo & CF_LEVEL, iseed);
	else if ((icreateinfo & CF_WITCH) != 0)
		RecreateWitchItem(lcg, player, item, idx, icreateinfo & CF_LEVEL, iseed);
	else if ((icreateinfo & CF_HEALER) != 0)
		RecreateHealerItem(lcg, player, item, idx, icreateinfo & CF_LEVEL, iseed);
}

bool HasMultipleFlags(uint16_t flags)
{
	return (flags & (flags - 1)) > 0;
}

} // namespace

ServerPlayerContext ServerPlayerContext::FromPlayerPack(const PlayerPack &pack)
{
	ServerPlayerContext ctx {};
	ctx.pClass = static_cast<HeroClass>(std::min<uint8_t>(pack.pClass, static_cast<uint8_t>(HeroClass::LAST)));
	ctx.pLevel = std::clamp<int8_t>(pack.pLevel, 1, MaxCharacterLevel);
	ctx.pMaxHPBase = pack.pMaxHPBase;
	ctx.pMaxManaBase = pack.pMaxManaBase;
	ctx.pBaseStr = pack.pBaseStr;
	ctx.pBaseMag = pack.pBaseMag;
	ctx.pBaseDex = pack.pBaseDex;
	ctx.pBaseVit = pack.pBaseVit;
	ctx.isHellfire = pack.bIsHellfire != 0;
	ctx.nDifficulty = static_cast<_difficulty>(std::min<uint32_t>(pack.pDifficulty, static_cast<uint32_t>(DIFF_LAST)));
	return ctx;
}

_item_indexes RemapItemIdxFromDiablo(_item_indexes i)
{
	int v = static_cast<int>(i);
	if (v == IDI_SORCERER) {
		return IDI_SORCERER_DIABLO;
	}
	if (v >= 156) {
		v += 5; // Hellfire exclusive items
	}
	if (v >= 88) {
		v += 1; // Scroll of Search
	}
	if (v >= 83) {
		v += 4; // Oils
	}
	return static_cast<_item_indexes>(v);
}

_item_indexes RemapItemIdxToDiablo(_item_indexes i)
{
	int v = static_cast<int>(i);
	if (v == IDI_SORCERER_DIABLO) {
		return IDI_SORCERER;
	}
	if ((v >= 83 && v <= 86) || v == 92 || v >= 161) {
		return IDI_NONE;
	}
	if (v >= 93) {
		v -= 1; // Scroll of Search
	}
	if (v >= 87) {
		v -= 4; // Oils
	}
	return static_cast<_item_indexes>(v);
}

bool IsItemAvailable(int i, bool isHellfire)
{
	if (i < 0 || i > IDI_LAST)
		return false;
	if (isHellfire)
		return true;

	const item_misc_id miscId = AllItemsList[i].iMiscId;
	if ((miscId > IMISC_OILFIRST && miscId < IMISC_OILLAST)
	    || (miscId > IMISC_RUNEFIRST && miscId < IMISC_RUNELAST)) {
		return false;
	}

	if (IsAnyOf(i, IDI_SORCERER_DIABLO, IDI_BARDSWORD, IDI_BARDDAGGER, IDI_SHORTSTAFF, IDI_BARBARIAN)) {
		return true;
	}

	return i != IDI_MAPOFDOOM
	    && i != IDI_LGTFORGE
	    && (i < IDI_OIL || i > IDI_GREYSUIT)
	    && (i < 83 || i > 86)
	    && i != 92
	    && (i < 161 || i > 165)
	    && i != IDI_SORCERER
	    && i <= 160;
}

bool IsUniqueAvailable(int i, bool isHellfire)
{
	if (i < 0 || i > UITEM_LOXLEYSBEND)
		return false;
	return isHellfire || i <= 89 || IsAnyOf(i, static_cast<int>(UITEM_KADISFURY), static_cast<int>(UITEM_APOSTATESCROOK), static_cast<int>(UITEM_LOXLEYSBEND));
}

int GetSpellBookLevel(SpellID s, bool isHellfire)
{
	if (!isHellfire) {
		switch (s) {
		case SpellID::Nova:
		case SpellID::Apocalypse:
			return -1;
		default:
			if (s > SpellID::LastDiablo)
				return -1;
			break;
		}
	}
	return GetServerSpellData(s).sBookLvl;
}

int GetSpellStaffLevel(SpellID s, bool isHellfire)
{
	if (!isHellfire && s > SpellID::LastDiablo)
		return -1;
	return GetServerSpellData(s).sStaffLvl;
}

bool IsPrefixValidForItemType(int i, AffixItemType flgs, bool hellfireItem)
{
	AffixItemType itemTypes = ItemPrefixes[i].PLIType;
	if (!hellfireItem) {
		if (i > 82)
			return false;
		if (i >= 12 && i <= 20)
			itemTypes &= ~AffixItemType::Staff;
	}
	return HasAnyOf(flgs, itemTypes);
}

bool IsSuffixValidForItemType(int i, AffixItemType flgs, bool hellfireItem)
{
	AffixItemType itemTypes = ItemSuffixes[i].PLIType;
	if (!hellfireItem) {
		if (i > 94)
			return false;
		if ((i >= 0 && i <= 1)
		    || (i >= 14 && i <= 15)
		    || (i >= 21 && i <= 22)
		    || (i >= 34 && i <= 36)
		    || (i >= 41 && i <= 44)
		    || (i >= 60 && i <= 63))
			itemTypes &= ~AffixItemType::Staff;
	}
	return HasAnyOf(flgs, itemTypes);
}

void InitializeServerItem(ServerItem &item, _item_indexes itemData, bool isHellfire, _difficulty difficulty)
{
	const auto &pAllItem = AllItemsList[static_cast<size_t>(itemData)];
	const uint32_t preservedTorment = item.dwBuff & CF_TORMENT;
	const bool isTormentItem = (difficulty == DIFF_TORMENT) || (preservedTorment != 0);

	item = {};
	item._itype = pAllItem.itype;
	item._iCurs = pAllItem.iCurs;
	CopyStrBounded(item._iName, sizeof(item._iName), pAllItem.iName);
	CopyStrBounded(item._iIName, sizeof(item._iIName), pAllItem.iName);
	item._iLoc = pAllItem.iLoc;
	item._iClass = pAllItem.iClass;
	item._iMinDam = pAllItem.iMinDam;
	item._iMaxDam = pAllItem.iMaxDam;
	item._iAC = isTormentItem ? pAllItem.iMaxAC : pAllItem.iMinAC;
	item._iMiscId = pAllItem.iMiscId;
	item._iSpell = pAllItem.iSpell;

	if (pAllItem.iMiscId == IMISC_STAFF) {
		item._iCharges = isHellfire ? 18 : 40;
	}

	item._iMaxCharges = item._iCharges;
	item._iDurability = pAllItem.iDurability;
	item._iMaxDur = pAllItem.iDurability;
	item._iMinStr = pAllItem.iMinStr;
	item._iMinMag = pAllItem.iMinMag;
	item._iMinDex = pAllItem.iMinDex;
	item._ivalue = pAllItem.iValue;
	item._iIvalue = pAllItem.iValue;
	item._iPrePower = IPL_INVALID;
	item._iSufPower = IPL_INVALID;
	item._iMagical = ITEM_QUALITY_NORMAL;
	item.IDidx = itemData;
	if (isHellfire)
		item.dwBuff |= CF_HELLFIRE;
	if (isTormentItem)
		item.dwBuff |= CF_TORMENT;
}

void GetItemAttrs(LcgEngine &lcg, ServerItem &item, _item_indexes itemData, int lvl, bool isHellfire, _difficulty difficulty)
{
	const auto &baseItemData = AllItemsList[static_cast<size_t>(itemData)];
	item._itype = baseItemData.itype;
	item._iCurs = baseItemData.iCurs;
	CopyStrBounded(item._iName, sizeof(item._iName), baseItemData.iName);
	CopyStrBounded(item._iIName, sizeof(item._iIName), baseItemData.iName);
	item._iLoc = baseItemData.iLoc;
	item._iClass = baseItemData.iClass;
	item._iMinDam = baseItemData.iMinDam;
	item._iMaxDam = baseItemData.iMaxDam;
	const bool isTormentItem = (difficulty == DIFF_TORMENT) || ((item.dwBuff & CF_TORMENT) != 0);
	const int acRoll = baseItemData.iMinAC + lcg.GenerateRnd(baseItemData.iMaxAC - baseItemData.iMinAC + 1);
	item._iAC = isTormentItem ? baseItemData.iMaxAC : acRoll;
	item._iFlags = baseItemData.iFlags;
	item._iMiscId = baseItemData.iMiscId;
	item._iSpell = baseItemData.iSpell;
	item._iMagical = ITEM_QUALITY_NORMAL;
	item._ivalue = baseItemData.iValue;
	item._iIvalue = baseItemData.iValue;
	item._iDurability = baseItemData.iDurability;
	item._iMaxDur = baseItemData.iDurability;
	item._iMinStr = baseItemData.iMinStr;
	item._iMinMag = baseItemData.iMinMag;
	item._iMinDex = baseItemData.iMinDex;
	item.IDidx = itemData;
	if (isHellfire)
		item.dwBuff |= CF_HELLFIRE;
	if (isTormentItem)
		item.dwBuff |= CF_TORMENT;
	item._iPrePower = IPL_INVALID;
	item._iSufPower = IPL_INVALID;

	if (item._iMiscId == IMISC_BOOK)
		GetBookSpell(lcg, item, lvl, isHellfire);

	if (isHellfire && item._iMiscId == IMISC_OILOF)
		GetOilType(lcg, item, lvl);

	if (item._itype != ItemType::Gold)
		return;

	int rndv = 0;
	const int itemlevel = std::max(lvl, 1);
	switch (difficulty) {
	case DIFF_NORMAL:
		rndv = 5 * itemlevel + lcg.GenerateRnd(10 * itemlevel);
		break;
	case DIFF_NIGHTMARE:
		rndv = 5 * (itemlevel + 16) + lcg.GenerateRnd(10 * (itemlevel + 16));
		break;
	case DIFF_HELL:
		rndv = 5 * (itemlevel + 32) + lcg.GenerateRnd(10 * (itemlevel + 32));
		break;
	case DIFF_TORMENT:
		(void)lcg.GenerateRnd(10 * (itemlevel + 32));
		rndv = ((15 * (itemlevel + 32) - 1) * 130) / 100;
		break;
	}
	item._ivalue = std::min(rndv, GOLD_MAX_LIMIT);
	item._iCurs = GetGoldCursor(item._ivalue);
}

int SaveItemPower(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, ItemPower power)
{
	const bool tormentMaxRoll = (player.nDifficulty == DIFF_TORMENT) || ((item.dwBuff & CF_TORMENT) != 0);

	if (!player.isHellfire) {
		if (power.type == IPL_TARGAC) {
			power.param1 = 1 << power.param1;
			power.param2 = 3 << power.param2;
		}
	}

	int r = RndPL(lcg, power.param1, power.param2, tormentMaxRoll);

	switch (power.type) {
	case IPL_TOHIT:
		item._iPLToHit += r;
		break;
	case IPL_TOHIT_CURSE:
		item._iPLToHit -= r;
		break;
	case IPL_DAMP:
		item._iPLDam += r;
		break;
	case IPL_DAMP_CURSE:
		item._iPLDam -= r;
		break;
	case IPL_DOPPELGANGER:
		item._iDamAcFlags |= ItemSpecialEffectHf::Doppelganger;
		[[fallthrough]];
	case IPL_TOHIT_DAMP:
		r = RndPL(lcg, power.param1, power.param2, tormentMaxRoll);
		item._iPLDam += r;
		item._iPLToHit += CalculateToHitBonus(lcg, power.param1, tormentMaxRoll);
		break;
	case IPL_TOHIT_DAMP_CURSE:
		item._iPLDam -= r;
		item._iPLToHit += CalculateToHitBonus(lcg, -power.param1, tormentMaxRoll);
		break;
	case IPL_ACP:
		item._iPLAC += r;
		break;
	case IPL_ACP_CURSE:
		item._iPLAC -= r;
		break;
	case IPL_SETAC:
		item._iAC = r;
		break;
	case IPL_AC_CURSE:
		item._iAC -= r;
		break;
	case IPL_FIRERES:
		item._iPLFR += r;
		break;
	case IPL_LIGHTRES:
		item._iPLLR += r;
		break;
	case IPL_MAGICRES:
		item._iPLMR += r;
		break;
	case IPL_ALLRES:
		item._iPLFR += r;
		item._iPLLR += r;
		item._iPLMR += r;
		break;
	case IPL_SPLLVLADD:
		item._iSplLvlAdd = r;
		break;
	case IPL_CHARGES:
		item._iCharges *= power.param1;
		item._iMaxCharges = item._iCharges;
		break;
	case IPL_SPELL:
		item._iSpell = static_cast<SpellID>(power.param1);
		item._iCharges = power.param2;
		item._iMaxCharges = power.param2;
		break;
	case IPL_FIREDAM:
		item._iFlags |= ItemSpecialEffect::FireDamage;
		item._iFlags &= ~ItemSpecialEffect::LightningDamage;
		item._iFMinDam = power.param1;
		item._iFMaxDam = power.param2;
		item._iLMinDam = 0;
		item._iLMaxDam = 0;
		break;
	case IPL_LIGHTDAM:
		item._iFlags |= ItemSpecialEffect::LightningDamage;
		item._iFlags &= ~ItemSpecialEffect::FireDamage;
		item._iLMinDam = power.param1;
		item._iLMaxDam = power.param2;
		item._iFMinDam = 0;
		item._iFMaxDam = 0;
		break;
	case IPL_STR:
		item._iPLStr += r;
		break;
	case IPL_STR_CURSE:
		item._iPLStr -= r;
		break;
	case IPL_MAG:
		item._iPLMag += r;
		break;
	case IPL_MAG_CURSE:
		item._iPLMag -= r;
		break;
	case IPL_DEX:
		item._iPLDex += r;
		break;
	case IPL_DEX_CURSE:
		item._iPLDex -= r;
		break;
	case IPL_VIT:
		item._iPLVit += r;
		break;
	case IPL_VIT_CURSE:
		item._iPLVit -= r;
		break;
	case IPL_ATTRIBS:
		item._iPLStr += r;
		item._iPLMag += r;
		item._iPLDex += r;
		item._iPLVit += r;
		break;
	case IPL_ATTRIBS_CURSE:
		item._iPLStr -= r;
		item._iPLMag -= r;
		item._iPLDex -= r;
		item._iPLVit -= r;
		break;
	case IPL_GETHIT_CURSE:
		item._iPLGetHit += r;
		break;
	case IPL_GETHIT:
		item._iPLGetHit -= r;
		break;
	case IPL_LIFE:
		item._iPLHP += r << 6;
		break;
	case IPL_LIFE_CURSE:
		item._iPLHP -= r << 6;
		break;
	case IPL_MANA:
		item._iPLMana += r << 6;
		break;
	case IPL_MANA_CURSE:
		item._iPLMana -= r << 6;
		break;
	case IPL_DUR: {
		const int bonus = r * item._iMaxDur / 100;
		item._iMaxDur += bonus;
		item._iDurability += bonus;
	} break;
	case IPL_CRYSTALLINE:
		item._iPLDam += 140 + r * 2;
		[[fallthrough]];
	case IPL_DUR_CURSE:
		item._iMaxDur -= r * item._iMaxDur / 100;
		item._iMaxDur = std::max<int>(item._iMaxDur, 1);
		item._iDurability = item._iMaxDur;
		break;
	case IPL_INDESTRUCTIBLE:
		item._iDurability = DUR_INDESTRUCTIBLE;
		item._iMaxDur = DUR_INDESTRUCTIBLE;
		break;
	case IPL_LIGHT:
		item._iPLLight += power.param1;
		break;
	case IPL_LIGHT_CURSE:
		item._iPLLight -= power.param1;
		break;
	case IPL_MULT_ARROWS:
		item._iFlags |= ItemSpecialEffect::MultipleArrows;
		break;
	case IPL_FIRE_ARROWS:
		item._iFlags |= ItemSpecialEffect::FireArrows;
		item._iFlags &= ~ItemSpecialEffect::LightningArrows;
		item._iFMinDam = power.param1;
		item._iFMaxDam = power.param2;
		item._iLMinDam = 0;
		item._iLMaxDam = 0;
		break;
	case IPL_LIGHT_ARROWS:
		item._iFlags |= ItemSpecialEffect::LightningArrows;
		item._iFlags &= ~ItemSpecialEffect::FireArrows;
		item._iLMinDam = power.param1;
		item._iLMaxDam = power.param2;
		item._iFMinDam = 0;
		item._iFMaxDam = 0;
		break;
	case IPL_FIREBALL:
		item._iFlags |= (ItemSpecialEffect::LightningArrows | ItemSpecialEffect::FireArrows);
		item._iFMinDam = power.param1;
		item._iFMaxDam = power.param2;
		item._iLMinDam = 0;
		item._iLMaxDam = 0;
		break;
	case IPL_THORNS:
		item._iFlags |= ItemSpecialEffect::Thorns;
		break;
	case IPL_NOMANA:
		item._iFlags |= ItemSpecialEffect::NoMana;
		break;
	case IPL_ABSHALFTRAP:
		item._iFlags |= ItemSpecialEffect::HalfTrapDamage;
		break;
	case IPL_KNOCKBACK:
		item._iFlags |= ItemSpecialEffect::Knockback;
		break;
	case IPL_3XDAMVDEM:
		item._iFlags |= ItemSpecialEffect::TripleDemonDamage;
		break;
	case IPL_ALLRESZERO:
		item._iFlags |= ItemSpecialEffect::ZeroResistance;
		break;
	case IPL_STEALMANA:
		if (power.param1 == 3)
			item._iFlags |= ItemSpecialEffect::StealMana3;
		if (power.param1 == 5)
			item._iFlags |= ItemSpecialEffect::StealMana5;
		break;
	case IPL_STEALLIFE:
		if (power.param1 == 3)
			item._iFlags |= ItemSpecialEffect::StealLife3;
		if (power.param1 == 5)
			item._iFlags |= ItemSpecialEffect::StealLife5;
		break;
	case IPL_TARGAC:
		if (player.isHellfire)
			item._iPLEnAc = power.param1;
		else
			item._iPLEnAc += r;
		break;
	case IPL_FASTATTACK:
		if (power.param1 == 1)
			item._iFlags |= ItemSpecialEffect::QuickAttack;
		if (power.param1 == 2)
			item._iFlags |= ItemSpecialEffect::FastAttack;
		if (power.param1 == 3)
			item._iFlags |= ItemSpecialEffect::FasterAttack;
		if (power.param1 == 4)
			item._iFlags |= ItemSpecialEffect::FastestAttack;
		break;
	case IPL_FASTRECOVER:
		if (power.param1 == 1)
			item._iFlags |= ItemSpecialEffect::FastHitRecovery;
		if (power.param1 == 2)
			item._iFlags |= ItemSpecialEffect::FasterHitRecovery;
		if (power.param1 == 3)
			item._iFlags |= ItemSpecialEffect::FastestHitRecovery;
		break;
	case IPL_FASTBLOCK:
		item._iFlags |= ItemSpecialEffect::FastBlock;
		break;
	case IPL_DAMMOD:
		item._iPLDamMod += r;
		break;
	case IPL_RNDARROWVEL:
		item._iFlags |= ItemSpecialEffect::RandomArrowVelocity;
		break;
	case IPL_SETDAM:
		item._iMinDam = power.param1;
		item._iMaxDam = power.param2;
		break;
	case IPL_SETDUR:
		item._iDurability = power.param1;
		item._iMaxDur = power.param1;
		break;
	case IPL_ONEHAND:
		item._iLoc = ILOC_ONEHAND;
		break;
	case IPL_DRAINLIFE:
		item._iFlags |= ItemSpecialEffect::DrainLife;
		break;
	case IPL_RNDSTEALLIFE:
		item._iFlags |= ItemSpecialEffect::RandomStealLife;
		break;
	case IPL_NOMINSTR:
		item._iMinStr = 0;
		break;
	case IPL_INVCURS:
		item._iCurs = power.param1;
		break;
	case IPL_ADDACLIFE:
		item._iFlags |= (ItemSpecialEffect::LightningArrows | ItemSpecialEffect::FireArrows);
		item._iFMinDam = power.param1;
		item._iFMaxDam = power.param2;
		item._iLMinDam = 1;
		item._iLMaxDam = 0;
		break;
	case IPL_ADDMANAAC:
		item._iFlags |= (ItemSpecialEffect::LightningDamage | ItemSpecialEffect::FireDamage);
		item._iFMinDam = power.param1;
		item._iFMaxDam = power.param2;
		item._iLMinDam = 2;
		item._iLMaxDam = 0;
		break;
	case IPL_FIRERES_CURSE:
		item._iPLFR -= r;
		break;
	case IPL_LIGHTRES_CURSE:
		item._iPLLR -= r;
		break;
	case IPL_MAGICRES_CURSE:
		item._iPLMR -= r;
		break;
	case IPL_DEVASTATION:
		item._iDamAcFlags |= ItemSpecialEffectHf::Devastation;
		break;
	case IPL_DECAY:
		item._iDamAcFlags |= ItemSpecialEffectHf::Decay;
		item._iPLDam += r;
		break;
	case IPL_PERIL:
		item._iDamAcFlags |= ItemSpecialEffectHf::Peril;
		break;
	case IPL_JESTERS:
		item._iDamAcFlags |= ItemSpecialEffectHf::Jesters;
		break;
	case IPL_ACDEMON:
		item._iDamAcFlags |= ItemSpecialEffectHf::ACAgainstDemons;
		break;
	case IPL_ACUNDEAD:
		item._iDamAcFlags |= ItemSpecialEffectHf::ACAgainstUndead;
		break;
	case IPL_MANATOLIFE: {
		const int portion = ((player.pMaxManaBase >> 6) * 50 / 100) << 6;
		item._iPLMana -= portion;
		item._iPLHP += portion;
	} break;
	case IPL_LIFETOMANA: {
		const int portion = ((player.pMaxHPBase >> 6) * 40 / 100) << 6;
		item._iPLHP -= portion;
		item._iPLMana += portion;
	} break;
	case IPL_FASTWALK:
		item._iDamAcFlags |= ItemSpecialEffectHf::FastWalk;
		break;
	case IPL_MAXDAM:
		item._iMaxDam += r;
		break;
	default:
		break;
	}

	return r;
}

void GetItemBonus(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, int minlvl, int maxlvl, bool onlygood, bool allowspells)
{
	if (minlvl > 25)
		minlvl = 25;

	switch (item._itype) {
	case ItemType::Sword:
	case ItemType::Axe:
	case ItemType::Mace:
		GetItemPower(lcg, player, item, minlvl, maxlvl, AffixItemType::Weapon, onlygood);
		break;
	case ItemType::Bow:
		GetItemPower(lcg, player, item, minlvl, maxlvl, AffixItemType::Bow, onlygood);
		break;
	case ItemType::Shield:
		GetItemPower(lcg, player, item, minlvl, maxlvl, AffixItemType::Shield, onlygood);
		break;
	case ItemType::LightArmor:
	case ItemType::Helm:
	case ItemType::MediumArmor:
	case ItemType::HeavyArmor:
		GetItemPower(lcg, player, item, minlvl, maxlvl, AffixItemType::Armor, onlygood);
		break;
	case ItemType::Staff:
		if (allowspells)
			GetStaffSpell(lcg, player, item, maxlvl, onlygood);
		else
			GetItemPower(lcg, player, item, minlvl, maxlvl, AffixItemType::Staff, onlygood);
		break;
	case ItemType::Ring:
	case ItemType::Amulet:
		GetItemPower(lcg, player, item, minlvl, maxlvl, AffixItemType::Misc, onlygood);
		break;
	case ItemType::None:
	case ItemType::Misc:
	case ItemType::Gold:
		break;
	}
}

_unique_items CheckUnique(LcgEngine &lcg, ServerItem &item, int lvl, int uper, bool recreate, bool isHellfire)
{
	std::bitset<128> uok = {};

	if (lcg.GenerateRnd(100) > uper)
		return UITEM_INVALID;

	int numu = 0;
	for (int j = 0; UniqueItems[j].UIItemId != UITYPE_INVALID; j++) {
		if (!IsUniqueAvailable(j, isHellfire))
			continue;
		if (!recreate && IsAnyOf(static_cast<_unique_items>(j), UITEM_KADISFURY, UITEM_APOSTATESCROOK, UITEM_LOXLEYSBEND))
			continue;
		if (UniqueItems[j].UIItemId == AllItemsList[item.IDidx].iItemId
		    && lvl >= UniqueItems[j].UIMinLvl) {
			uok[j] = true;
			numu++;
		}
	}

	if (numu == 0)
		return UITEM_INVALID;

	lcg.DiscardRandomValues(1);
	uint8_t itemData = 0;
	while (numu > 0) {
		if (uok[itemData])
			numu--;
		if (numu > 0)
			itemData = (itemData + 1) % 128;
	}

	return static_cast<_unique_items>(itemData);
}

void GetUniqueItem(LcgEngine &lcg, const ServerPlayerContext &player, ServerItem &item, _unique_items uid)
{
	for (auto power : UniqueItems[uid].powers) {
		if (power.type == IPL_INVALID)
			break;
		SaveItemPower(lcg, player, item, power);
	}

	CopyStrBounded(item._iIName, sizeof(item._iIName), UniqueItems[uid].UIName);
	item._iIvalue = UniqueItems[uid].UIValue;

	if (item._iMiscId == IMISC_UNIQUE || IsAnyOf(uid, UITEM_KADISFURY, UITEM_APOSTATESCROOK, UITEM_LOXLEYSBEND))
		item._iSeed = static_cast<uint32_t>(uid);

	item._iUid = uid;
	item._iMagical = ITEM_QUALITY_UNIQUE;
	item._iCreateInfo |= CF_UNIQUE;
}

void ItemRndDur(LcgEngine &lcg, ServerItem &item, _difficulty difficulty)
{
	if (item._iDurability > 0 && item._iDurability != DUR_INDESTRUCTIBLE) {
		const bool isTormentItem = (difficulty == DIFF_TORMENT) || ((item.dwBuff & CF_TORMENT) != 0);
		const int durRoll = lcg.GenerateRnd(item._iMaxDur / 2) + (item._iMaxDur / 4) + 1;
		item._iDurability = isTormentItem ? item._iMaxDur : durRoll;
	}
}

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
    bool pregen)
{
	item._iSeed = iseed;
	lcg.SetRndSeed(iseed);
	GetItemAttrs(lcg, item, idx, lvl / 2, player.isHellfire, player.nDifficulty);
	item._iCreateInfo = static_cast<uint16_t>(lvl);

	if (pregen)
		item._iCreateInfo |= CF_PREGEN;
	if (onlygood)
		item._iCreateInfo |= CF_ONLYGOOD;

	if (uper == 15)
		item._iCreateInfo |= CF_UPER15;
	else if (uper == 1)
		item._iCreateInfo |= CF_UPER1;

	const bool isExclusiveDiabloUnique = recreate && IsAnyOf(static_cast<_unique_items>(iseed), UITEM_KADISFURY, UITEM_APOSTATESCROOK, UITEM_LOXLEYSBEND);
	if (item._iMiscId != IMISC_UNIQUE && !isExclusiveDiabloUnique) {
		const int iblvl = GetItemBLevel(lcg, lvl, item._iMiscId, onlygood, uper == 15);
		if (iblvl != -1) {
			const _unique_items uid = CheckUnique(lcg, item, iblvl, uper, recreate, player.isHellfire);
			if (uid == UITEM_INVALID) {
				GetItemBonus(lcg, player, item, iblvl / 2, iblvl, onlygood, true);
			} else {
				GetUniqueItem(lcg, player, item, uid);
			}
		}
		if (item._iMagical != ITEM_QUALITY_UNIQUE)
			ItemRndDur(lcg, item, player.nDifficulty);
	} else {
		if (item._iLoc != ILOC_UNEQUIPABLE) {
			if (iseed > static_cast<uint32_t>(UITEM_LOXLEYSBEND)
			    || AllItemsList[static_cast<size_t>(idx)].iItemId != UniqueItems[iseed].UIItemId) {
				item.clear();
				return;
			}
			GetUniqueItem(lcg, player, item, static_cast<_unique_items>(iseed));
		}
	}
	item._iIdentified = false;
}

void SetupAllUseful(
    LcgEngine &lcg,
    ServerItem &item,
    uint32_t iseed,
    int lvl,
    bool isHellfire,
    _difficulty difficulty)
{
	item._iSeed = iseed;
	lcg.SetRndSeed(iseed);

	_item_indexes idx = IDI_HEAL;
	if (isHellfire) {
		switch (lcg.GenerateRnd(7)) {
		case 0:
			idx = (lvl <= 1) ? IDI_HEAL : IDI_PORTAL;
			break;
		case 1:
		case 2:
			idx = IDI_HEAL;
			break;
		case 3:
			idx = (lvl <= 1) ? IDI_MANA : IDI_PORTAL;
			break;
		case 4:
		case 5:
			idx = IDI_MANA;
			break;
		default:
			idx = IDI_OIL;
			break;
		}
	} else {
		idx = lcg.FlipCoin() ? IDI_HEAL : IDI_MANA;
		if (lvl > 1 && lcg.FlipCoin(3))
			idx = IDI_PORTAL;
	}

	GetItemAttrs(lcg, item, idx, lvl, isHellfire, difficulty);
	item._iCreateInfo = static_cast<uint16_t>(lvl | CF_USEFUL);
	item._iIdentified = false;
}

void SpawnUniqueServerItem(
    LcgEngine &lcg,
    const ServerPlayerContext &player,
    ServerItem &item,
    _unique_items uid,
    int curlv)
{
	int idx = 0;
	while (idx <= IDI_LAST && AllItemsList[idx].iItemId != UniqueItems[uid].UIItemId)
		idx++;
	if (idx > IDI_LAST) {
		item.clear();
		return;
	}

	if (player.nDifficulty == DIFF_NORMAL || IsAnyOf(uid, UITEM_KADISFURY, UITEM_APOSTATESCROOK, UITEM_LOXLEYSBEND)) {
		GetItemAttrs(lcg, item, static_cast<_item_indexes>(idx), curlv, player.isHellfire, player.nDifficulty);
		item._iCreateInfo = static_cast<uint16_t>(curlv);
		GetUniqueItem(lcg, player, item, uid);
		item._iIdentified = false;
	} else {
		const ItemData &uniqueItemData = AllItemsList[idx];
		const _item_indexes dropIdx = GetItemIndexForDroppableItem(lcg, player.isHellfire, false, [&](const ItemData &candidate) {
			return candidate.itype == uniqueItemData.itype;
		});
		SetupAllItems(lcg, player, item, dropIdx, static_cast<uint32_t>(lcg.AdvanceRndSeed()), curlv * 2, 15, true, false, false);
	}
}

void RecreateServerItem(
    LcgEngine &lcg,
    const ServerPlayerContext &player,
    ServerItem &item,
    _item_indexes idx,
    uint16_t icreateinfo,
    uint32_t iseed,
    int ivalue,
    bool isHellfire)
{
	ServerPlayerContext effectivePlayer = player;
	effectivePlayer.isHellfire = isHellfire;
	if ((item.dwBuff & CF_TORMENT) != 0) {
		effectivePlayer.nDifficulty = DIFF_TORMENT;
	}

	if (idx == IDI_GOLD) {
		InitializeServerItem(item, IDI_GOLD, isHellfire, effectivePlayer.nDifficulty);
		item._iSeed = iseed;
		item._iCreateInfo = icreateinfo;
		item._ivalue = ivalue;
		item._iCurs = GetGoldCursor(item._ivalue);
		return;
	}

	if (icreateinfo == 0) {
		InitializeServerItem(item, idx, isHellfire, effectivePlayer.nDifficulty);
		item._iSeed = iseed;
		return;
	}

	if ((icreateinfo & CF_UNIQUE) == 0) {
		if ((icreateinfo & CF_TOWN) != 0) {
			RecreateTownItem(lcg, effectivePlayer, item, idx, icreateinfo, iseed);
			return;
		}

		if ((icreateinfo & CF_USEFUL) == CF_USEFUL) {
			SetupAllUseful(lcg, item, iseed, icreateinfo & CF_LEVEL, isHellfire, effectivePlayer.nDifficulty);
			return;
		}
	}

	const int level = icreateinfo & CF_LEVEL;
	int uper = 0;
	if ((icreateinfo & CF_UPER1) != 0)
		uper = 1;
	if ((icreateinfo & CF_UPER15) != 0)
		uper = 15;

	const bool onlygood = (icreateinfo & CF_ONLYGOOD) != 0;
	const bool recreate = (icreateinfo & CF_UNIQUE) != 0;
	const bool pregen = (icreateinfo & CF_PREGEN) != 0;

	SetupAllItems(lcg, effectivePlayer, item, idx, iseed, level, uper, onlygood, recreate, pregen);
}

void RecreateServerEar(ServerItem &item, uint16_t ic, uint32_t iseed, uint8_t bCursval, const char *heroName)
{
	InitializeServerItem(item, IDI_EAR, false, DIFF_NORMAL);
	std::snprintf(item._iName, sizeof(item._iName), "Ear of %s", heroName != nullptr ? heroName : "");
	CopyStrBounded(item._iIName, sizeof(item._iIName), heroName);
	item._iCurs = ((bCursval >> 6) & 3) + ICURS_EAR_SORCERER;
	item._ivalue = bCursval & 0x3F;
	item._iCreateInfo = ic;
	item._iSeed = iseed;
}

void UnPackServerItem(const PkItemStruct &packedItem, const ServerPlayerContext &player, ServerItem &outItem)
{
	if (packedItem.idx == 0xFFFFU) {
		outItem.clear();
		return;
	}

	auto idx = static_cast<_item_indexes>(packedItem.idx);
	if (!player.isHellfire) {
		idx = RemapItemIdxFromDiablo(idx);
	}

	if (!IsItemAvailable(idx, player.isHellfire)) {
		outItem.clear();
		return;
	}

	if (idx == IDI_EAR) {
		const uint16_t ic = packedItem.iCreateInfo;
		const uint32_t iseed = packedItem.iSeed;
		const uint16_t ivalue = packedItem.wValue;
		const uint32_t ibuff = packedItem.dwBuff;

		char heroName[17];
		heroName[0] = static_cast<char>((ic >> 8) & 0x7F);
		heroName[1] = static_cast<char>(ic & 0x7F);
		heroName[2] = static_cast<char>((iseed >> 24) & 0x7F);
		heroName[3] = static_cast<char>((iseed >> 16) & 0x7F);
		heroName[4] = static_cast<char>((iseed >> 8) & 0x7F);
		heroName[5] = static_cast<char>(iseed & 0x7F);
		heroName[6] = static_cast<char>(packedItem.bId & 0x7F);
		heroName[7] = static_cast<char>(packedItem.bDur & 0x7F);
		heroName[8] = static_cast<char>(packedItem.bMDur & 0x7F);
		heroName[9] = static_cast<char>(packedItem.bCh & 0x7F);
		heroName[10] = static_cast<char>(packedItem.bMCh & 0x7F);
		heroName[11] = static_cast<char>((ivalue >> 8) & 0x7F);
		heroName[12] = static_cast<char>((ibuff >> 24) & 0x7F);
		heroName[13] = static_cast<char>((ibuff >> 16) & 0x7F);
		heroName[14] = static_cast<char>((ibuff >> 8) & 0x7F);
		heroName[15] = static_cast<char>(ibuff & 0x7F);
		heroName[16] = '\0';

		RecreateServerEar(outItem, ic, iseed, static_cast<uint8_t>(ivalue & 0xFFU), heroName);
		return;
	}

	outItem = {};
	outItem.dwBuff = packedItem.dwBuff;
	LcgEngine lcg;
	RecreateServerItem(lcg, player, outItem, idx, packedItem.iCreateInfo, packedItem.iSeed, packedItem.wValue, player.isHellfire);
	if (outItem.isEmpty()) {
		return;
	}
	outItem._iIdentified = (packedItem.bId & 1) != 0;
	if (outItem._iMagical == ITEM_QUALITY_UNIQUE && outItem._iUid == UITEM_KADISFURY) {
		outItem._iMaxDur = DUR_INDESTRUCTIBLE;
		outItem._iDurability = DUR_INDESTRUCTIBLE;
	} else {
		outItem._iMaxDur = packedItem.bMDur;
		outItem._iDurability = std::clamp<int>(packedItem.bDur, 0, outItem._iMaxDur);
	}
	outItem._iMaxCharges = std::clamp<int>(packedItem.bMCh, 0, outItem._iMaxCharges);
	outItem._iCharges = std::clamp<int>(packedItem.bCh, 0, outItem._iMaxCharges);
	outItem.dwBuff |= (packedItem.dwBuff & CF_TORMENT);
}

void PackServerItem(PkItemStruct &outPackedItem, const ServerItem &item, bool isHellfire)
{
	outPackedItem = {};
	if (item.isEmpty() || item._iMiscId == IMISC_ARENAPOT) {
		outPackedItem.idx = 0xFFFFU;
		return;
	}

	_item_indexes idx = item.IDidx;
	if (!isHellfire) {
		idx = RemapItemIdxToDiablo(idx);
	}
	outPackedItem.idx = static_cast<uint16_t>(idx);
	outPackedItem.iSeed = item._iSeed;
	outPackedItem.iCreateInfo = item._iCreateInfo;
	outPackedItem.bId = static_cast<uint8_t>((static_cast<uint8_t>(item._iMagical) << 1) | (item._iIdentified ? 1 : 0));
	if (item._iMaxDur > 255)
		outPackedItem.bMDur = 254;
	else
		outPackedItem.bMDur = static_cast<uint8_t>(item._iMaxDur);
	outPackedItem.bDur = static_cast<uint8_t>(std::min<int>(item._iDurability, outPackedItem.bMDur));
	outPackedItem.bCh = static_cast<uint8_t>(item._iCharges);
	outPackedItem.bMCh = static_cast<uint8_t>(item._iMaxCharges);
	if (item.IDidx == IDI_GOLD)
		outPackedItem.wValue = static_cast<uint16_t>(item._ivalue);
	outPackedItem.dwBuff = item.dwBuff;
}

bool IsCreationFlagComboValid(uint16_t iCreateInfo)
{
	iCreateInfo = static_cast<uint16_t>(iCreateInfo & ~CF_LEVEL);
	const bool isTownItem = (iCreateInfo & CF_TOWN) != 0;
	const bool isPregenItem = (iCreateInfo & CF_PREGEN) != 0;
	const bool isUsefulItem = (iCreateInfo & CF_USEFUL) == CF_USEFUL;

	if (isPregenItem) {
		return false;
	}
	if (isUsefulItem && (iCreateInfo & ~CF_USEFUL) != 0)
		return false;
	if (isTownItem && HasMultipleFlags(iCreateInfo)) {
		return false;
	}
	return true;
}

bool IsTownItemValid(uint16_t iCreateInfo)
{
	const uint8_t level = static_cast<uint8_t>(iCreateInfo & CF_LEVEL);
	const bool isBoyItem = (iCreateInfo & CF_BOY) != 0;
	constexpr uint8_t maxTownItemLevel = 30;

	if (isBoyItem && level <= MaxCharacterLevel)
		return true;

	return level <= maxTownItemLevel;
}

bool IsShopPriceValid(const ServerItem &item, bool isHellfire)
{
	if (!isHellfire && (item._iCreateInfo & CF_BOY) != 0 && item._iIvalue > MaxBoyValue)
		return false;

	if (!isHellfire && (item._iCreateInfo & CF_SMITHPREMIUM) != 0 && item._iIvalue > MaxVendorValue)
		return false;

	const uint16_t smithOrWitch = CF_SMITH | CF_WITCH;
	const int smithAndWitchPriceLimit = isHellfire ? MaxVendorValueHf : MaxVendorValue;
	if ((item._iCreateInfo & smithOrWitch) != 0 && item._iIvalue > smithAndWitchPriceLimit)
		return false;

	return true;
}

bool IsUniqueMonsterItemValid(uint16_t iCreateInfo, uint32_t dwBuff)
{
	const uint8_t level = static_cast<uint8_t>(iCreateInfo & CF_LEVEL);
	(void)dwBuff;

	if (IsAnyOf(level,
	        ServerMonstersData[MT_DIABLO].level,
	        ServerMonstersData[MT_NAKRUL].level,
	        ServerMonstersData[MT_DEFILER].level,
	        ServerMonstersData[MT_HORKDMN].level)) {
		return true;
	}

	for (int i = 0; ServerUniqueMonstersData[i].mtype != MT_INVALID; i++) {
		const auto &uniqueMonsterData = ServerUniqueMonstersData[i];
		const uint8_t uniqueMonsterLevel = ServerMonstersData[uniqueMonsterData.mtype].level;
		if (level == uniqueMonsterLevel) {
			return true;
		}
	}

	return false;
}

bool IsDungeonItemValid(uint16_t iCreateInfo, uint32_t dwBuff)
{
	const uint8_t level = static_cast<uint8_t>(iCreateInfo & CF_LEVEL);
	(void)dwBuff;

	for (int16_t i = 0; i < static_cast<int16_t>(NUM_MTYPES); i++) {
		const auto &monsterData = ServerMonstersData[i];
		const uint8_t monsterLevel = monsterData.level;

		if (!IsAnyOf(static_cast<_monster_id>(i), MT_DIABLO, MT_NAKRUL, MT_DEFILER, MT_HORKDMN)
		    && monsterData.availability == MonsterAvailability::Never) {
			continue;
		}

		if (level == monsterLevel || (i == MT_DIABLO && level == monsterLevel - 15)) {
			return true;
		}
	}

	constexpr uint8_t maxDungeonLevel = 24 - 7;
	return level <= (maxDungeonLevel * 2);
}

bool IsHellfireSpellBookValid(const ServerItem &spellBook, bool isHellfire)
{
	int spellBookLevel = GetSpellBookLevel(spellBook._iSpell, isHellfire);
	spellBookLevel++;

	if (spellBookLevel >= 1 && (spellBook._iCreateInfo & CF_LEVEL) == spellBookLevel * 2) {
		return true;
	}

	return IsDungeonItemValid(spellBook._iCreateInfo, spellBook.dwBuff);
}

bool IsServerItemValid(const ServerItem &item, bool isHellfire)
{
	if (item.IDidx != IDI_GOLD && !IsCreationFlagComboValid(item._iCreateInfo))
		return false;
	if ((item._iCreateInfo & CF_TOWN) != 0)
		return IsTownItemValid(item._iCreateInfo) && IsShopPriceValid(item, isHellfire);
	if ((item._iCreateInfo & CF_USEFUL) == CF_UPER15)
		return IsUniqueMonsterItemValid(item._iCreateInfo, item.dwBuff);
	if ((item.dwBuff & CF_HELLFIRE) != 0 && AllItemsList[item.IDidx].iMiscId == IMISC_BOOK)
		return IsHellfireSpellBookValid(item, isHellfire);

	return IsDungeonItemValid(item._iCreateInfo, item.dwBuff);
}

bool VerifyPackedItemIntegrity(
    const PlayerPack &ownerPack,
    const PkItemStruct &packedItem,
    std::string &outError,
    ServerItem *outRecreated)
{
	if (packedItem.idx == 0xFFFFU) {
		if (outRecreated != nullptr)
			outRecreated->clear();
		return true;
	}

	const ServerPlayerContext player = ServerPlayerContext::FromPlayerPack(ownerPack);
	auto idx = static_cast<_item_indexes>(packedItem.idx);
	if (!player.isHellfire) {
		idx = RemapItemIdxFromDiablo(idx);
	}

	if (!IsItemAvailable(idx, player.isHellfire)) {
		outError = "Item index " + std::to_string(packedItem.idx) + " is not available in DX:Eternity";
		return false;
	}

	if (idx == IDI_EAR) {
		ServerItem ear {};
		UnPackServerItem(packedItem, player, ear);
		if (outRecreated != nullptr)
			*outRecreated = ear;
		return true;
	}

	if (idx == IDI_GOLD) {
		if (packedItem.wValue < 1 || packedItem.wValue > static_cast<uint16_t>(GOLD_MAX_LIMIT)) {
			outError = "Gold stack value out of bounds (must be 1..5000): " + std::to_string(packedItem.wValue);
			return false;
		}
		ServerItem gold {};
		UnPackServerItem(packedItem, player, gold);
		if (outRecreated != nullptr)
			*outRecreated = gold;
		return true;
	}

	// Deterministic 1:1 recreation from (idx, iCreateInfo, iSeed, dwBuff)
	ServerItem recreated {};
	recreated.dwBuff = packedItem.dwBuff;
	LcgEngine lcg;
	RecreateServerItem(lcg, player, recreated, idx, packedItem.iCreateInfo, packedItem.iSeed, packedItem.wValue, player.isHellfire);
	if (recreated.isEmpty() || recreated.IDidx != idx) {
		outError = "RecreateServerItem failed or produced mismatched base item IDidx (expected "
		    + std::to_string(static_cast<int>(idx)) + ", got " + std::to_string(static_cast<int>(recreated.IDidx)) + ")";
		return false;
	}

	if (!IsServerItemValid(recreated, player.isHellfire)) {
		outError = "Item failed DX:Eternity IsItemValid creation flag/level/vendor validation";
		return false;
	}

	// Verify quality bits in packedItem.bId (bits 1..7 encode _iMagical)
	const auto claimedQuality = static_cast<item_quality>(packedItem.bId >> 1);
	if (claimedQuality != recreated._iMagical) {
		outError = "Packed item quality bit mismatch (claimed " + std::to_string(static_cast<int>(claimedQuality))
		    + ", recreated " + std::to_string(static_cast<int>(recreated._iMagical)) + ")";
		return false;
	}

	// Verify max durability and current durability bounds
	// Note: Blacksmith repair can reduce _iMaxDur below its initial recreated value, or Blacksmith Oil in HF can change it,
	// but in Classic Diablo (no oils), bMDur can NEVER exceed the recreated _iMaxDur!
	uint8_t expectedMaxDur = (recreated._iMaxDur > 255) ? 254 : static_cast<uint8_t>(recreated._iMaxDur);
	if (recreated._iMagical == ITEM_QUALITY_UNIQUE && recreated._iUid == UITEM_KADISFURY) {
		expectedMaxDur = DUR_INDESTRUCTIBLE;
	}
	if (expectedMaxDur == DUR_INDESTRUCTIBLE) {
		if (packedItem.bMDur != DUR_INDESTRUCTIBLE || packedItem.bDur != DUR_INDESTRUCTIBLE) {
			outError = "Indestructible item durability tampered";
			return false;
		}
	} else {
		if (packedItem.bMDur > expectedMaxDur) {
			outError = "Packed item max durability (" + std::to_string(packedItem.bMDur)
			    + ") exceeds recreated ceiling (" + std::to_string(expectedMaxDur) + ")";
			return false;
		}
		if (packedItem.bDur > packedItem.bMDur) {
			outError = "Packed item current durability exceeds max durability";
			return false;
		}
	}

	// Verify staff charges (Adria recharge / Staff Recharge spell can reduce _iMaxCharges, never increase above recreated _iMaxCharges)
	const uint8_t expectedMaxCharges = static_cast<uint8_t>(std::clamp(recreated._iMaxCharges, 0, 255));
	if (packedItem.bMCh > expectedMaxCharges || packedItem.bCh > packedItem.bMCh) {
		outError = "Packed item staff charges (" + std::to_string(packedItem.bCh) + "/" + std::to_string(packedItem.bMCh)
		    + ") exceed recreated ceiling (" + std::to_string(expectedMaxCharges) + ")";
		return false;
	}

	// Apply unpacked runtime state (identified, current dur/charges)
	recreated._iIdentified = (packedItem.bId & 1) != 0;
	if (recreated._iMagical == ITEM_QUALITY_UNIQUE && recreated._iUid == UITEM_KADISFURY) {
		recreated._iMaxDur = DUR_INDESTRUCTIBLE;
		recreated._iDurability = DUR_INDESTRUCTIBLE;
	} else {
		recreated._iMaxDur = packedItem.bMDur;
		recreated._iDurability = std::clamp<int>(packedItem.bDur, 0, recreated._iMaxDur);
	}
	recreated._iMaxCharges = std::clamp<int>(packedItem.bMCh, 0, recreated._iMaxCharges);
	recreated._iCharges = std::clamp<int>(packedItem.bCh, 0, recreated._iMaxCharges);
	recreated.dwBuff |= (packedItem.dwBuff & CF_TORMENT);

	if (outRecreated != nullptr) {
		*outRecreated = recreated;
	}
	return true;
}

ComputedPlayerCombatStats CalculatePlayerEquippedStats(const PlayerPack &pack)
{
	ComputedPlayerCombatStats out {};
	const ServerPlayerContext player = ServerPlayerContext::FromPlayerPack(pack);

	std::array<ServerItem, 7> equipped {};
	for (size_t i = 0; i < 7; i++) {
		UnPackServerItem(pack.InvBody[i], player, equipped[i]);
	}

	// CalcSelfItems equivalent
	int sa = 0;
	int ma = 0;
	int da = 0;
	for (auto &eq : equipped) {
		if (eq.isEmpty())
			continue;
		eq._iStatFlag = true;
		if (eq._iIdentified) {
			sa += eq._iPLStr;
			ma += eq._iPLMag;
			da += eq._iPLDex;
		}
	}

	bool changeflag = false;
	do {
		const int currstr = std::max(0, sa + static_cast<int>(player.pBaseStr));
		const int currmag = std::max(0, ma + static_cast<int>(player.pBaseMag));
		const int currdex = std::max(0, da + static_cast<int>(player.pBaseDex));
		changeflag = false;

		for (auto &eq : equipped) {
			if (eq.isEmpty() || !eq._iStatFlag)
				continue;
			if (currstr >= eq._iMinStr && currmag >= eq._iMinMag && currdex >= eq._iMinDex)
				continue;
			changeflag = true;
			eq._iStatFlag = false;
			if (eq._iIdentified) {
				sa -= eq._iPLStr;
				ma -= eq._iPLMag;
				da -= eq._iPLDex;
			}
		}
	} while (changeflag);

	int mind = 0;
	int maxd = 0;
	int tac = 0;
	int bdam = 0;
	int btohit = 0;
	int bac = 0;
	ItemSpecialEffect iflgs = ItemSpecialEffect::None;
	ItemSpecialEffectHf pDamAcFlags = ItemSpecialEffectHf::None;
	int sadd = 0;
	int madd = 0;
	int dadd = 0;
	int vadd = 0;
	int dmod = 0;
	int spllvladd = 0;
	int fmin = 0;
	int fmax = 0;
	int lmin = 0;
	int lmax = 0;

	for (auto &item : equipped) {
		if (item.isEmpty() || !item._iStatFlag)
			continue;

		mind += item._iMinDam;
		maxd += item._iMaxDam;
		tac += item._iAC;

		if (item._iMagical == ITEM_QUALITY_UNIQUE && item._iUid == UITEM_KADISFURY) {
			item._iDamAcFlags |= ItemSpecialEffectHf::FastWalk;
		}
		if (item._iMagical == ITEM_QUALITY_NORMAL || item._iIdentified) {
			bdam += item._iPLDam;
			btohit += item._iPLToHit;
			if (item._iPLAC != 0) {
				int tmpac = item._iAC * item._iPLAC / 100;
				if (tmpac == 0)
					tmpac = (item._iPLAC > 0) ? 1 : -1;
				bac += tmpac;
			}
			iflgs |= item._iFlags;
			pDamAcFlags |= item._iDamAcFlags;
			sadd += item._iPLStr;
			madd += item._iPLMag;
			dadd += item._iPLDex;
			vadd += item._iPLVit;
			dmod += item._iPLDamMod;
			spllvladd += item._iSplLvlAdd;
			fmin += item._iFMinDam;
			fmax += item._iFMaxDam;
			lmin += item._iLMinDam;
			lmax += item._iLMaxDam;
		}
	}

	if (mind == 0 && maxd == 0) {
		mind = 1;
		maxd = 1;
		if ((equipped[4]._itype == ItemType::Shield && equipped[4]._iStatFlag)
		    || (equipped[5]._itype == ItemType::Shield && equipped[5]._iStatFlag)) {
			maxd = 3;
		}
		if (player.pClass == HeroClass::Monk) {
			mind = std::max(mind, static_cast<int>(player.pLevel) / 2);
			maxd = std::max(maxd, static_cast<int>(player.pLevel));
		}
	}

	out.strength = std::max(0, sadd + static_cast<int>(player.pBaseStr));
	out.magic = std::max(0, madd + static_cast<int>(player.pBaseMag));
	out.dexterity = std::max(0, dadd + static_cast<int>(player.pBaseDex));
	out.vitality = std::max(0, vadd + static_cast<int>(player.pBaseVit));
	out.minDam = mind;
	out.maxDam = maxd;
	out.bonusDamPercent = bdam;
	out.bonusDamMod = dmod;
	out.bonusToHit = btohit;
	out.totalAC = tac + bac;
	out.fireMinDam = fmin;
	out.fireMaxDam = fmax;
	out.lightMinDam = lmin;
	out.lightMaxDam = lmax;
	out.spellLvlAdd = spllvladd;
	out.flags = iflgs;
	out.hfFlags = pDamAcFlags;

	const ItemType leftType = equipped[4]._itype;
	const ItemType rightType = equipped[5]._itype;

	if (player.pClass == HeroClass::Rogue) {
		out.classDamageMod = player.pLevel * (out.strength + out.dexterity) / 200;
	} else if (player.pClass == HeroClass::Monk) {
		out.classDamageMod = player.pLevel * (out.strength + out.dexterity) / 150;
		if ((!equipped[4].isEmpty() && leftType != ItemType::Staff) || (!equipped[5].isEmpty() && rightType != ItemType::Staff))
			out.classDamageMod /= 2;
	} else if (player.pClass == HeroClass::Bard) {
		if (leftType == ItemType::Sword || rightType == ItemType::Sword)
			out.classDamageMod = player.pLevel * (out.strength + out.dexterity) / 150;
		else if (leftType == ItemType::Bow || rightType == ItemType::Bow)
			out.classDamageMod = player.pLevel * (out.strength + out.dexterity) / 250;
		else
			out.classDamageMod = player.pLevel * out.strength / 100;
	} else if (player.pClass == HeroClass::Barbarian) {
		if (IsAnyOf(leftType, ItemType::Axe, ItemType::Mace) || IsAnyOf(rightType, ItemType::Axe, ItemType::Mace))
			out.classDamageMod = player.pLevel * out.strength / 75;
		else if (leftType == ItemType::Bow || rightType == ItemType::Bow)
			out.classDamageMod = player.pLevel * out.strength / 300;
		else
			out.classDamageMod = player.pLevel * out.strength / 100;

		if (leftType != ItemType::Shield && rightType != ItemType::Shield
		    && !IsAnyOf(leftType, ItemType::Staff, ItemType::Bow) && !IsAnyOf(rightType, ItemType::Staff, ItemType::Bow)) {
			out.classDamageMod += player.pLevel * out.vitality / 100;
		}
	} else {
		out.classDamageMod = player.pLevel * out.strength / 100;
	}

	// Calculate generous single-hit physical/elemental/crit ceiling (including 2x Warrior/Barbarian crit, 3x TripleDemonDamage/Devastation, Peril 2x)
	int32_t baseSwingMax = out.maxDam + (out.maxDam * std::max(0, out.bonusDamPercent) / 100) + out.classDamageMod + out.bonusDamMod;
	if (baseSwingMax < 5)
		baseSwingMax = 5;
	// Allow Barbarian Rage (+2*lvl str/vit) and crit (2x) and TripleDemonDamage (3x)
	int32_t multiplier = 2; // Critical hit
	if (HasAnyOf(out.flags, ItemSpecialEffect::TripleDemonDamage) || HasAnyOf(out.hfFlags, ItemSpecialEffectHf::Devastation))
		multiplier *= 3;
	if (HasAnyOf(out.hfFlags, ItemSpecialEffectHf::Peril))
		multiplier *= 2;
	if (HasAnyOf(out.hfFlags, ItemSpecialEffectHf::Jesters))
		multiplier *= 6;

	const int32_t elementalMax = out.fireMaxDam + out.lightMaxDam + 60;
	// Also account for high-level spells (Fireball / Apocalypse / Lightning) based on magic/spell level
	const int32_t spellCeiling = 120 + (player.pLevel * 12) + (std::max(0, out.spellLvlAdd) * 35);
	out.maxSingleHitCeiling = std::max(baseSwingMax * multiplier + elementalMax + 50, spellCeiling);
	return out;
}

} // namespace devilution::realm
