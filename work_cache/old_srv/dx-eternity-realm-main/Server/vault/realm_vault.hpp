/**
 * @file realm_vault.hpp
 * @brief Server-Vault character and stash lifecycle manager for dx-eternity-realm (Phase 3).
 *
 * Replaces client-side `.sv` files with server-authoritative `PlayerPack` creation,
 * validation, hero roster listing (`_uiheroinfo` parity), and multi-page Stash storage.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/realm_database.hpp"
#include "protocol/realm_protocol.hpp"

namespace devilution::realm {

/**
 * @brief Authoritative class attribute & life/mana scaling table (matches Source/playerdat.cpp 1:1).
 */
struct HeroClassStatsSpec {
	const char *className;
	uint8_t baseStr;
	uint8_t baseMag;
	uint8_t baseDex;
	uint8_t baseVit;
	uint8_t maxStr;
	uint8_t maxMag;
	uint8_t maxDex;
	uint8_t maxVit;
	uint8_t blockBonus;
	int16_t adjLife;
	int16_t adjMana;
	int16_t lvlLife;
	int16_t lvlMana;
	int16_t chrLife;
	int16_t chrMana;
	int16_t itmLife;
	int16_t itmMana;
};

/**
 * @brief Returns the authoritative `DX:Eternity` `PlayersData` entry for `heroClass` (0..5, including Barbarian).
 */
const HeroClassStatsSpec &GetHeroClassStatsSpec(HeroClass heroClass);

/**
 * @brief Computes exact base life (in 1/64th fixed-point units, matching `Player::calculateBaseLife()`).
 */
int32_t CalculateHeroBaseLife(HeroClass heroClass, int8_t level, uint8_t baseVit);

/**
 * @brief Computes exact base mana (in 1/64th fixed-point units, matching `Player::calculateBaseMana()`).
 */
int32_t CalculateHeroBaseMana(HeroClass heroClass, int8_t level, uint8_t baseMag);

/**
 * @brief Builds a server-authoritative level-1 `PlayerPack` for `heroClass` matching `CreatePlayer()` + `CreatePlrItems()`.
 */
PlayerPack BuildInitialPlayerPack(const std::string &heroName, HeroClass heroClass, bool isHellfire);

class RealmVaultManager {
public:
	explicit RealmVaultManager(RealmDatabase &db);

	/**
	 * @brief Validates a hero name (2..15 ASCII characters in [A-Za-z0-9_-]).
	 */
	static bool ValidateHeroName(const std::string &name);

	/**
	 * @brief Populates `HeroListResponsePayload` for `accountId`.
	 */
	RealmStatus ListHeroes(int64_t accountId, HeroListResponsePayload &outResp) const;

	/**
	 * @brief Creates a new hero with server-generated `PlayerPack` and records its starting items in `item_ledger`.
	 */
	RealmStatus CreateHero(
	    int64_t accountId,
	    const std::string &heroName,
	    uint8_t rawHeroClass,
	    bool isHellfire,
	    const std::string &remoteIp,
	    HeroSummaryInfo &outSummary,
	    PlayerPack &outInitialPack);

	/**
	 * @brief Deletes an existing hero owned by `accountId`.
	 */
	RealmStatus DeleteHero(int64_t accountId, int64_t charId, const std::string &remoteIp);

	/**
	 * @brief Loads the authoritative `PlayerPack` for `(accountId, charId)`.
	 */
	RealmStatus LoadHero(int64_t accountId, int64_t charId, PlayerPack &outPack) const;

	/**
	 * @brief Validates and persists a `PlayerPack` snapshot for `(accountId, charId)`.
	 */
	RealmStatus SaveHero(
	    int64_t accountId,
	    int64_t charId,
	    const PlayerPack &incomingPack,
	    const std::string &remoteIp,
	    const struct GamePlayerRuntimeState *activePlayerSession = nullptr);

	/**
	 * @brief Loads the account's shared Server-Vault stash.
	 */
	RealmStatus LoadStash(
	    int64_t accountId,
	    uint32_t &outStashGold,
	    std::vector<PkItemStruct> &outItems) const;

	/**
	 * @brief Validates and saves the account's shared Server-Vault stash.
	 */
	RealmStatus SaveStash(
	    int64_t accountId,
	    uint32_t stashGold,
	    const std::vector<PkItemStruct> &items,
	    const std::string &remoteIp);

	/**
	 * @brief Validates structural and class-cap invariants on an incoming `PlayerPack` against the stored record.
	 */
	static bool ValidatePlayerPackSnapshot(
	    const CharacterRecord &existing,
	    const PlayerPack &incoming,
	    std::string &outReason);

	/**
	 * @brief Admin-only: Promotes an existing hero to `minLevel` (default 35, with matching `ExpLvlsTbl` XP,
	 * unspent stat points, and base HP/Mana) and `minDiabloKillLevel` (default 3 = Hell Diablo slain),
	 * immediately unlocking `DIFF_TORMENT`.
	 */
	bool AdminPromoteHeroToTorment(
	    int64_t accountId,
	    int64_t charId,
	    int8_t minLevel,
	    uint32_t minDiabloKillLevel,
	    std::string &outMessage);

	/**
	 * @brief Admin-only: Imports an existing Single Player (`single_*.sv`) or Multiplayer (`multi_*.sv`)
	 * `PlayerPack` into `accountId`, automatically stripping `CF_PREGEN` from Single Player items,
	 * verifying `RecreateServerItem()` integrity, and registering 100% of its items in `item_ledger`.
	 */
	bool AdminImportCharacterPack(
	    int64_t accountId,
	    const PlayerPack &rawImportedPack,
	    int64_t &outCharId,
	    std::string &outMessage);

	/**
	 * @brief Admin-only: Imports an existing local Stash (`stash.sv`) into `accountId`, stripping `CF_PREGEN`
	 * from Single Player items, verifying `RecreateServerItem()` integrity, and registering all stash items
	 * in `item_ledger`.
	 */
	bool AdminImportStash(
	    int64_t accountId,
	    uint32_t stashGold,
	    const std::vector<PkItemStruct> &rawItems,
	    std::string &outMessage);

private:
	RealmDatabase &db_;
};

} // namespace devilution::realm
