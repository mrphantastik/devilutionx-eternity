/**
 * @file realm_session.cpp
 * @brief Closed Realm Game Session Registry, Difficulty Gates, Server-Oracle Loot Dispensing,
 *        and Atomic Ground-to-Inventory Item Ledger (Phase 5).
 */
#include "session/realm_session.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <sodium.h>

#include "anticheat/anomaly_detector.hpp"

namespace devilution::realm {

namespace {

constexpr uint8_t CMD_SEND_GITEM = 48; // DevilutionX network command ID for TCmdGItem
constexpr int NUM_UNIQMONSTERS = 101;

uint64_t NowMonotonicMs()
{
	const auto now = std::chrono::steady_clock::now().time_point::time_since_epoch();
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

uint64_t NowUnixSeconds()
{
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

std::string ExtractBoundedString(const char *buf, size_t maxLen)
{
	size_t len = 0;
	while (len < maxLen && buf[len] != '\0') {
		++len;
	}
	return std::string(buf, len);
}

bool ValidateLobbyName(const std::string &name)
{
	if (name.size() < 2 || name.size() > 28)
		return false;
	for (unsigned char c : name) {
		if (!std::isalnum(c) && c != '_' && c != '-' && c != ' ')
			return false;
	}
	return true;
}

bool EqualsCaseInsensitive(const std::string &a, const std::string &b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	}
	return true;
}

bool ContainsCaseInsensitive(const std::string &haystack, const std::string &needle)
{
	if (needle.empty())
		return true;
	if (haystack.size() < needle.size())
		return false;
	for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
		bool match = true;
		for (size_t j = 0; j < needle.size(); ++j) {
			if (std::tolower(static_cast<unsigned char>(haystack[i + j]))
			    != std::tolower(static_cast<unsigned char>(needle[j]))) {
				match = false;
				break;
			}
		}
		if (match)
			return true;
	}
	return false;
}

std::string SanitizeChatText(const std::string &raw)
{
	std::string clean;
	clean.reserve(raw.size());
	for (unsigned char c : raw) {
		if (c >= 0x20 && c != 0x7F) {
			clean.push_back(static_cast<char>(c));
		}
	}
	while (!clean.empty() && clean.front() == ' ')
		clean.erase(clean.begin());
	while (!clean.empty() && clean.back() == ' ')
		clean.pop_back();
	if (clean.size() >= MaxChatMessageLength) {
		clean.resize(MaxChatMessageLength - 1);
	}
	return clean;
}

/**
 * @brief Classic Diablo droppable item filter used in DX:Eternity across all 25 floors (including Hive & Crypt).
 * Excludes Hellfire oils, runes, and non-droppable items so `GetItemIndexForDroppableItem` matches client LCG.
 */
bool IsClassicDroppableItemAvailable(int idx)
{
	if (idx < 0 || idx > IDI_LAST)
		return false;
	const item_misc_id miscId = AllItemsList[idx].iMiscId;
	if ((miscId > IMISC_OILFIRST && miscId < IMISC_OILLAST)
	    || (miscId > IMISC_RUNEFIRST && miscId < IMISC_RUNELAST)) {
		return false;
	}
	return idx != IDI_MAPOFDOOM
	    && idx != IDI_LGTFORGE
	    && (idx < IDI_OIL || idx > IDI_GREYSUIT)
	    && (idx < 83 || idx > 86)
	    && idx != 92
	    && (idx < 161 || idx > 165)
	    && idx != IDI_SORCERER
	    && idx <= 160;
}

_item_indexes PickDroppableItemIndex(LcgEngine &lcg, int itemLevel)
{
	int candidates[170] {};
	int count = 0;

	for (int i = 0; i <= IDI_LAST; ++i) {
		if (!IsClassicDroppableItemAvailable(i))
			continue;
		if (AllItemsList[i].iRnd != IDROP_NEVER && itemLevel >= AllItemsList[i].iMinMLvl) {
			candidates[count++] = i;
			if (AllItemsList[i].iRnd == IDROP_DOUBLE) {
				candidates[count++] = i;
			}
		}
	}

	if (count <= 0)
		return IDI_GOLD;

	return static_cast<_item_indexes>(candidates[lcg.GenerateRnd(count)]);
}

} // namespace

EffectiveMonsterStats CalculateEffectiveMonsterStats(
    int16_t monsterTypeOrUid,
    bool isUniqueMonster,
    _difficulty difficulty,
    bool isHellfire)
{
	EffectiveMonsterStats out {};

	if (isUniqueMonster) {
		const int uid = std::clamp<int>(monsterTypeOrUid, 0, NUM_UNIQMONSTERS - 1);
		const ServerUniqueMonsterData &u = ServerUniqueMonstersData[uid];
		const int mtype = std::clamp<int>(static_cast<int>(u.mtype), 0, NUM_MTYPES - 1);
		const ServerMonsterData &base = ServerMonstersData[mtype];

		out.baseType = u.mtype;
		out.level = u.mlevel > 0 ? u.mlevel : base.level;
		out.minHp = u.mmaxhp;
		out.maxHp = u.mmaxhp;
		out.armorClass = base.armorClass;
		out.toHit = base.toHit;
		out.minDamage = u.mMinDamage;
		out.maxDamage = u.mMaxDamage;
		out.treasureFlags = base.treasure | T_UNIQ;
		out.expReward = static_cast<uint32_t>(base.exp) * 2U;
	} else {
		const int mtype = std::clamp<int>(monsterTypeOrUid, 0, NUM_MTYPES - 1);
		const ServerMonsterData &base = ServerMonstersData[mtype];

		out.baseType = static_cast<_monster_id>(mtype);
		out.level = base.level;
		out.minHp = base.hitPointsMinimum;
		out.maxHp = base.hitPointsMaximum;
		out.armorClass = base.armorClass;
		out.toHit = base.toHit;
		out.minDamage = base.minDamage;
		out.maxDamage = base.maxDamage;
		out.treasureFlags = base.treasure;
		out.expReward = base.exp;
	}

	const int hpBonusNightmare = isHellfire ? 100 : 50;
	const int hpBonusHell = isHellfire ? 200 : 100;

	if (difficulty == DIFF_NIGHTMARE) {
		out.minHp = out.minHp * 3 + hpBonusNightmare;
		out.maxHp = out.maxHp * 3 + hpBonusNightmare;
		out.armorClass += 50;
		out.toHit += 85;
		out.minDamage = out.minDamage * 2 + 4;
		out.maxDamage = out.maxDamage * 2 + 4;
		out.expReward = out.expReward * 2U + 1000U;
		out.level += 15;
	} else if (difficulty == DIFF_HELL || difficulty == DIFF_TORMENT) {
		out.minHp = out.minHp * 4 + hpBonusHell;
		out.maxHp = out.maxHp * 4 + hpBonusHell;
		out.armorClass += 80;
		out.toHit += 120;
		out.minDamage = out.minDamage * 4 + 6;
		out.maxDamage = out.maxDamage * 4 + 6;
		out.expReward = out.expReward * 4U + 3000U;
		out.level += 30;

		if (difficulty == DIFF_TORMENT) {
			// DX:Eternity DIFF_TORMENT rule: scale HP, damage, AC, To-Hit, level, and XP to 130% of DIFF_HELL (* 130 / 100)
			out.minHp = (out.minHp * 130) / 100;
			out.maxHp = (out.maxHp * 130) / 100;
			out.armorClass = (out.armorClass * 130) / 100;
			out.toHit = (out.toHit * 130) / 100;
			out.minDamage = (out.minDamage * 130) / 100;
			out.maxDamage = (out.maxDamage * 130) / 100;
			out.level = (out.level * 130) / 100;
			out.expReward = (out.expReward * 130U) / 100U;
		}
	}

	out.minHp = std::max(out.minHp, 1);
	out.maxHp = std::max(out.maxHp, out.minHp);
	return out;
}

bool IsFloorTransitionValid(
    uint8_t fromLevel,
    uint8_t toLevel,
    const std::bitset<MaxDungeonLevel + 1> &visitedFloors)
{
	if (fromLevel > MaxDungeonLevel || toLevel > MaxDungeonLevel)
		return false;
	if (fromLevel == toLevel)
		return true;

	// Town Portal return to Tristram (0) is valid from any dungeon floor
	if (toLevel == 0)
		return true;

	// From Tristram (0): can enter Cathedral (1), unlocked town entry points (Catacombs 5, Caves 9, Hell 13, Hive 17, Crypt 21),
	// or return through an active Town Portal to any previously visited floor.
	if (fromLevel == 0) {
		if (IsAnyOf(toLevel, 1, 5, 9, 13, 17, 21) || visitedFloors.test(toLevel))
			return true;
		return false;
	}

	// Sequential stairs within standard branches (1..16)
	if (fromLevel <= 16 && toLevel <= 16 && std::abs(static_cast<int>(fromLevel) - static_cast<int>(toLevel)) == 1) {
		return true;
	}

	// Sequential stairs within the Hive (17..20)
	if (fromLevel >= 17 && fromLevel <= 20 && toLevel >= 17 && toLevel <= 20
	    && std::abs(static_cast<int>(fromLevel) - static_cast<int>(toLevel)) == 1) {
		return true;
	}

	// Sequential stairs within the Crypt (21..24)
	if (fromLevel >= 21 && fromLevel <= 24 && toLevel >= 21 && toLevel <= 24
	    && std::abs(static_cast<int>(fromLevel) - static_cast<int>(toLevel)) == 1) {
		return true;
	}

	// DX:Eternity seamless dungeon branch entrances:
	// - Caves Level 12 (12) <-> Hive Level 17 (17)
	if ((fromLevel == 12 && toLevel == 17) || (fromLevel == 17 && toLevel == 12))
		return true;

	// - Hell Level 14 (14) <-> Crypt Level 21 (21)
	if ((fromLevel == 14 && toLevel == 21) || (fromLevel == 21 && toLevel == 14))
		return true;

	return false;
}

RealmSessionManager::RealmSessionManager(RealmDatabase &db)
    : db_(db)
{
	PostSystemChatMessage(RealmChatChannel::General, "REALM",
	    "Welcome to DX:Eternity Closed Realm [#General]. Use /games [search] to filter games or /w <hero> <msg> to whisper.");
	PostSystemChatMessage(RealmChatChannel::Trade, "REALM",
	    "Welcome to [#Trade]. Post items for trade or price checks. All Server-Vault items are ledger-verified.");
	PostSystemChatMessage(RealmChatChannel::LFG, "REALM",
	    "Welcome to [#LFG] (Looking For Group). Find parties for Cathedral, Hive, Crypt, Hell & Torment Diablo runs.");
	PostSystemChatMessage(RealmChatChannel::Torment, "REALM",
	    "Welcome to [#Torment]. Endgame channel for Level 35+ Hell Diablo slayers (130% monster stats & CF_TORMENT maxed rolls).");
}

RealmStatus RealmSessionManager::CheckDifficultyEntryGate(
    const CharacterRecord &charRecord,
    _difficulty difficulty,
    uint8_t maxPlayers,
    const std::string &remoteIp) const
{
	if (difficulty > DIFF_TORMENT) {
		return RealmStatus::ERR_MALFORMED_PACKET;
	}

	// DX:Eternity DIFF_TORMENT Gate:
	// 1. Character MUST have slain Diablo on Hell difficulty (`pDiabloKillLevel >= 3`) in both SP and MP.
	// 2. In multiplayer (`maxPlayers > 1`), character MUST be at least level 35 (`pLevel >= 35`).
	if (difficulty == DIFF_TORMENT) {
		if (charRecord.diabloKillLevel < TormentRequiredDiabloKillLevel) {
			db_.LogSecurityEvent(charRecord.accountId, charRecord.id, 0, "WARN", "TORMENT_GATE_DENIED",
			    "Rejected DIFF_TORMENT entry for '" + charRecord.heroName + "' (diabloKillLevel="
			        + std::to_string(charRecord.diabloKillLevel) + " < 3) from " + remoteIp);
			return RealmStatus::ERR_TORMENT_LOCKED_HELL_REQUIRED;
		}
		if (maxPlayers > 1 && charRecord.heroLevel < TormentRequiredMultiplayerLevel) {
			db_.LogSecurityEvent(charRecord.accountId, charRecord.id, 0, "WARN", "TORMENT_LEVEL_DENIED",
			    "Rejected multiplayer DIFF_TORMENT entry for '" + charRecord.heroName + "' (level="
			        + std::to_string(charRecord.heroLevel) + " < 35) from " + remoteIp);
			return RealmStatus::ERR_TORMENT_LEVEL_35_REQUIRED;
		}
	} else if (maxPlayers > 1) {
		if (difficulty == DIFF_NIGHTMARE && charRecord.heroLevel < 20) {
			return RealmStatus::ERR_DIFFICULTY_LEVEL_LOCKED;
		}
		if (difficulty == DIFF_HELL && charRecord.heroLevel < 30) {
			return RealmStatus::ERR_DIFFICULTY_LEVEL_LOCKED;
		}
	}

	return RealmStatus::OK;
}

RealmStatus RealmSessionManager::CreateGame(
    int64_t accountId,
    const GameCreateRequestPayload &req,
    const std::string &remoteIp,
    GameCreateOrJoinResponsePayload &outResp)
{
	outResp = {};
	const std::string gameName = ExtractBoundedString(req.gameName, sizeof(req.gameName));
	const std::string password = ExtractBoundedString(req.password, sizeof(req.password));

	if (!ValidateLobbyName(gameName) || req.maxPlayers < 1 || req.maxPlayers > MaxPlayersPerGame || req.difficulty > DIFF_TORMENT) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_MALFORMED_PACKET);
		return RealmStatus::ERR_MALFORMED_PACKET;
	}

	// Ensure game name is not already active
	for (const auto &[existingId, existingGame] : games_) {
		if (EqualsCaseInsensitive(existingGame.gameName, gameName)) {
			outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_GAME_NAME_TAKEN);
			return RealmStatus::ERR_GAME_NAME_TAKEN;
		}
	}

	CharacterRecord charRecord {};
	RealmStatus loadStatus = RealmStatus::OK;
	if (!db_.LoadCharacter(accountId, static_cast<int64_t>(req.charId), charRecord, loadStatus)) {
		outResp.statusCode = static_cast<uint16_t>(loadStatus);
		return loadStatus;
	}

	const auto diff = static_cast<_difficulty>(req.difficulty);
	const RealmStatus gateStatus = CheckDifficultyEntryGate(charRecord, diff, req.maxPlayers, remoteIp);
	if (gateStatus != RealmStatus::OK) {
		outResp.statusCode = static_cast<uint16_t>(gateStatus);
		return gateStatus;
	}

	ActiveGameSession game {};
	game.gameId = nextGameId_++;
	game.gameName = gameName;
	game.password = password;
	game.difficulty = diff;
	game.maxPlayers = req.maxPlayers;
	game.isHellfire = (req.bIsHellfire != 0);
	game.terrainInitSeed = randombytes_random();
	randombytes_buf(game.realmSecretSalt.data(), game.realmSecretSalt.size());
	game.createdAtMs = NowMonotonicMs();

	GamePlayerRuntimeState hostState {};
	hostState.accountId = accountId;
	hostState.charId = charRecord.id;
	hostState.playerSlot = 0;
	hostState.currentLevel = 0;
	hostState.visitedFloors.set(0);
	hostState.initialPack = charRecord.pack;
	hostState.combatStats = CalculatePlayerEquippedStats(charRecord.pack);
	game.players.push_back(hostState);

	const uint64_t createdGameId = game.gameId;
	const uint32_t createdTerrainSeed = game.terrainInitSeed;
	games_.emplace(createdGameId, std::move(game));

	db_.LogSecurityEvent(accountId, charRecord.id, createdGameId, "INFO", "GAME_CREATED",
	    "Created game '" + gameName + "' (diff=" + std::to_string(req.difficulty)
	        + ", maxPlayers=" + std::to_string(req.maxPlayers) + ") by '" + charRecord.heroName + "' from " + remoteIp);

	outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
	outResp.difficulty = req.difficulty;
	outResp.playerSlot = 0;
	outResp.gameId = createdGameId;
	outResp.terrainInitSeed = createdTerrainSeed;
	outResp.bIsHellfire = req.bIsHellfire != 0 ? 1 : 0;
	outResp.maxPlayers = req.maxPlayers;
	return RealmStatus::OK;
}

RealmStatus RealmSessionManager::JoinGame(
    int64_t accountId,
    const GameJoinRequestPayload &req,
    const std::string &remoteIp,
    GameCreateOrJoinResponsePayload &outResp)
{
	outResp = {};
	const std::string gameName = ExtractBoundedString(req.gameName, sizeof(req.gameName));
	const std::string password = ExtractBoundedString(req.password, sizeof(req.password));

	ActiveGameSession *targetGame = nullptr;
	for (auto &[id, game] : games_) {
		if (EqualsCaseInsensitive(game.gameName, gameName)) {
			targetGame = &game;
			break;
		}
	}

	if (targetGame == nullptr) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_GAME_NOT_FOUND);
		return RealmStatus::ERR_GAME_NOT_FOUND;
	}

	if (!targetGame->password.empty() && targetGame->password != password) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_GAME_PASSWORD_INVALID);
		return RealmStatus::ERR_GAME_PASSWORD_INVALID;
	}

	CharacterRecord charRecord {};
	RealmStatus loadStatus = RealmStatus::OK;
	if (!db_.LoadCharacter(accountId, static_cast<int64_t>(req.charId), charRecord, loadStatus)) {
		outResp.statusCode = static_cast<uint16_t>(loadStatus);
		return loadStatus;
	}

	const RealmStatus gateStatus = CheckDifficultyEntryGate(charRecord, targetGame->difficulty, targetGame->maxPlayers, remoteIp);
	if (gateStatus != RealmStatus::OK) {
		outResp.statusCode = static_cast<uint16_t>(gateStatus);
		return gateStatus;
	}

	// Check if character is already in this game
	for (const auto &p : targetGame->players) {
		if (p.charId == charRecord.id) {
			outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
			outResp.difficulty = static_cast<uint8_t>(targetGame->difficulty);
			outResp.playerSlot = p.playerSlot;
			outResp.gameId = targetGame->gameId;
			outResp.terrainInitSeed = targetGame->terrainInitSeed;
			outResp.bIsHellfire = targetGame->isHellfire ? 1 : 0;
			outResp.maxPlayers = targetGame->maxPlayers;
			return RealmStatus::OK;
		}
	}

	if (targetGame->players.size() >= targetGame->maxPlayers) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_GAME_FULL);
		return RealmStatus::ERR_GAME_FULL;
	}

	uint8_t assignedSlot = 0;
	for (uint8_t slot = 0; slot < targetGame->maxPlayers; ++slot) {
		bool used = false;
		for (const auto &p : targetGame->players) {
			if (p.playerSlot == slot) {
				used = true;
				break;
			}
		}
		if (!used) {
			assignedSlot = slot;
			break;
		}
	}

	GamePlayerRuntimeState joinState {};
	joinState.accountId = accountId;
	joinState.charId = charRecord.id;
	joinState.playerSlot = assignedSlot;
	joinState.currentLevel = 0;
	joinState.visitedFloors.set(0);
	joinState.initialPack = charRecord.pack;
	joinState.combatStats = CalculatePlayerEquippedStats(charRecord.pack);
	targetGame->players.push_back(joinState);

	db_.LogSecurityEvent(accountId, charRecord.id, targetGame->gameId, "INFO", "GAME_JOINED",
	    "Hero '" + charRecord.heroName + "' joined game '" + targetGame->gameName + "' (slot="
	        + std::to_string(assignedSlot) + ") from " + remoteIp);

	outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
	outResp.difficulty = static_cast<uint8_t>(targetGame->difficulty);
	outResp.playerSlot = assignedSlot;
	outResp.gameId = targetGame->gameId;
	outResp.terrainInitSeed = targetGame->terrainInitSeed;
	outResp.bIsHellfire = targetGame->isHellfire ? 1 : 0;
	outResp.maxPlayers = targetGame->maxPlayers;
	return RealmStatus::OK;
}

RealmStatus RealmSessionManager::ListGames(
    GameListResponsePayload &outResp,
    const GameListFilterRequestPayload *filter) const
{
	outResp = {};
	outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
	uint16_t count = 0;

	const std::string query = (filter != nullptr)
	    ? ExtractBoundedString(filter->searchQuery, sizeof(filter->searchQuery))
	    : std::string();

	for (const auto &[gameId, game] : games_) {
		if (count >= MaxListedGames)
			break;

		if (filter != nullptr) {
			if (filter->difficultyFilter <= DIFF_TORMENT
			    && static_cast<uint8_t>(game.difficulty) != filter->difficultyFilter) {
				continue;
			}
			if (filter->hideFullGames != 0 && game.players.size() >= game.maxPlayers) {
				continue;
			}
			if (filter->hidePasswordedGames != 0 && !game.password.empty()) {
				continue;
			}
			if (!query.empty()) {
				bool matched = ContainsCaseInsensitive(game.gameName, query);
				if (!matched) {
					for (const auto &p : game.players) {
						const std::string pName = ExtractBoundedString(p.initialPack.pName, sizeof(p.initialPack.pName));
						if (ContainsCaseInsensitive(pName, query)) {
							matched = true;
							break;
						}
					}
				}
				if (!matched)
					continue;
			}
		}

		GameLobbyInfo &info = outResp.games[count++];
		info.gameId = game.gameId;
		std::snprintf(info.gameName, sizeof(info.gameName), "%s", game.gameName.c_str());
		info.difficulty = static_cast<uint8_t>(game.difficulty);
		info.currentPlayers = static_cast<uint8_t>(game.players.size());
		info.maxPlayers = game.maxPlayers;
		info.passwordProtected = game.password.empty() ? 0 : 1;
	}

	outResp.gameCount = count;
	return RealmStatus::OK;
}

uint64_t RealmSessionManager::PostSystemChatMessage(
    RealmChatChannel channel,
    const std::string &senderBadge,
    const std::string &text,
    int64_t privateTargetAccountId)
{
	const auto chanIdx = static_cast<size_t>(channel);
	if (chanIdx >= static_cast<size_t>(RealmChatChannel::COUNT))
		return 0;

	ChatMessageEntry msg {};
	msg.messageId = nextChatMessageId_++;
	msg.timestampSec = NowUnixSeconds();
	msg.channelId = static_cast<uint8_t>(channel);
	msg.senderClass = 0;
	msg.senderLevel = 50;
	msg.senderKillLevel = 4;
	msg.flags = CHAT_FLAG_SYSTEM | CHAT_FLAG_TORMENT_CROWN;
	std::snprintf(msg.senderName, sizeof(msg.senderName), "%s", senderBadge.c_str());
	std::snprintf(msg.text, sizeof(msg.text), "%s", SanitizeChatText(text).c_str());

	if (privateTargetAccountId > 0) {
		auto &inbox = accountWhisperInbox_[privateTargetAccountId];
		if (inbox.size() >= 64) {
			inbox.erase(inbox.begin());
		}
		inbox.push_back(msg);
	} else {
		auto &ring = channelHistory_[chanIdx];
		if (ring.size() >= 64) {
			ring.erase(ring.begin());
		}
		ring.push_back(msg);
	}
	return msg.messageId;
}

RealmStatus RealmSessionManager::SendChatMessage(
    int64_t accountId,
    int64_t charId,
    const ChatSendRequestPayload &req,
    const std::string &remoteIp,
    ChatSendResponsePayload &outResp)
{
	outResp = {};
	outResp.channelId = req.channelId;

	if (req.channelId >= static_cast<uint8_t>(RealmChatChannel::COUNT)) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_CHAT_INVALID_CHANNEL);
		return RealmStatus::ERR_CHAT_INVALID_CHANNEL;
	}

	const std::string cleanText = SanitizeChatText(ExtractBoundedString(req.text, sizeof(req.text)));
	if (cleanText.empty()) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_CHAT_EMPTY_MESSAGE);
		return RealmStatus::ERR_CHAT_EMPTY_MESSAGE;
	}

	// Resolve server-verified character identity so clients cannot spoof name/level/Torment crown
	std::string senderName = "Acct#" + std::to_string(accountId);
	uint8_t senderClass = 0;
	uint8_t senderLevel = 1;
	uint8_t senderKillLevel = 0;

	CharacterRecord charRec {};
	RealmStatus dummyStatus = RealmStatus::OK;
	if (charId > 0 && db_.LoadCharacter(accountId, charId, charRec, dummyStatus)) {
		senderName = charRec.heroName;
		senderClass = charRec.heroClass;
		senderLevel = charRec.heroLevel;
		senderKillLevel = charRec.diabloKillLevel;
	} else {
		const auto chars = db_.ListCharactersForAccount(accountId);
		if (!chars.empty()) {
			senderName = chars.front().heroName;
			senderClass = chars.front().heroClass;
			senderLevel = chars.front().heroLevel;
			senderKillLevel = chars.front().diabloKillLevel;
		}
	}

	const auto channel = static_cast<RealmChatChannel>(req.channelId);

	// 1. Slash command: `/games [search]`
	if (cleanText == "/games" || cleanText.rfind("/games ", 0) == 0) {
		const std::string searchArg = (cleanText.size() > 7) ? SanitizeChatText(cleanText.substr(7)) : "";
		GameListFilterRequestPayload filter {};
		filter.difficultyFilter = 0xFF;
		std::snprintf(filter.searchQuery, sizeof(filter.searchQuery), "%s", searchArg.c_str());

		GameListResponsePayload listResp {};
		ListGames(listResp, &filter);

		static constexpr const char *kDiffNames[4] = { "Norm", "NM", "Hell", "Torment" };
		std::string summary;
		if (listResp.gameCount == 0) {
			summary = searchArg.empty() ? "No active games open." : ("No games matching '" + searchArg + "'.");
		} else {
			summary = "Games (" + std::to_string(listResp.gameCount) + "): ";
			for (uint16_t i = 0; i < std::min<uint16_t>(listResp.gameCount, 4); ++i) {
				const auto &g = listResp.games[i];
				if (i > 0)
					summary += ", ";
				summary += std::string(g.gameName) + " [" + kDiffNames[g.difficulty & 3U] + " "
				    + std::to_string(g.currentPlayers) + "/" + std::to_string(g.maxPlayers) + "]";
			}
		}
		outResp.messageId = PostSystemChatMessage(channel, "LOBBY", summary, accountId);
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
		return RealmStatus::OK;
	}

	// 2. Slash command: `/who`
	if (cleanText == "/who") {
		std::string summary = "Active Games: " + std::to_string(games_.size())
		    + " | Registered Heroes: " + std::to_string(db_.GetTotalCharactersCount());
		outResp.messageId = PostSystemChatMessage(channel, "LOBBY", summary, accountId);
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
		return RealmStatus::OK;
	}

	// 3. Slash command: `/w <targetHeroName> <message>` (Direct Whisper)
	if (cleanText.rfind("/w ", 0) == 0) {
		const std::string rest = cleanText.substr(3);
		const size_t spacePos = rest.find(' ');
		if (spacePos == std::string::npos || spacePos == 0 || spacePos + 1 >= rest.size()) {
			outResp.messageId = PostSystemChatMessage(channel, "SYSTEM", "Usage: /w <HeroName> <message>", accountId);
			outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
			return RealmStatus::OK;
		}
		const std::string targetName = rest.substr(0, spacePos);
		const std::string whisperBody = SanitizeChatText(rest.substr(spacePos + 1));

		int64_t targetAccountId = 0;
		std::string resolvedTargetName = targetName;
		const auto allChars = db_.ListAllCharacters(200);
		for (const auto &c : allChars) {
			if (EqualsCaseInsensitive(c.heroName, targetName)) {
				targetAccountId = c.accountId;
				resolvedTargetName = c.heroName;
				break;
			}
		}
		if (targetAccountId <= 0) {
			outResp.messageId = PostSystemChatMessage(channel, "SYSTEM", "Hero '" + targetName + "' not found.", accountId);
			outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
			return RealmStatus::OK;
		}

		ChatMessageEntry wMsg {};
		wMsg.messageId = nextChatMessageId_++;
		wMsg.timestampSec = NowUnixSeconds();
		wMsg.channelId = req.channelId;
		wMsg.senderClass = senderClass;
		wMsg.senderLevel = senderLevel;
		wMsg.senderKillLevel = senderKillLevel;
		wMsg.flags = CHAT_FLAG_WHISPER | (senderKillLevel >= TormentRequiredDiabloKillLevel ? CHAT_FLAG_TORMENT_CROWN : 0);
		std::snprintf(wMsg.senderName, sizeof(wMsg.senderName), "%s", senderName.c_str());
		std::snprintf(wMsg.text, sizeof(wMsg.text), "[Whisper -> %s] %s", resolvedTargetName.c_str(), whisperBody.c_str());

		auto &targetInbox = accountWhisperInbox_[targetAccountId];
		if (targetInbox.size() >= 64)
			targetInbox.erase(targetInbox.begin());
		targetInbox.push_back(wMsg);

		if (targetAccountId != accountId) {
			auto &senderInbox = accountWhisperInbox_[accountId];
			if (senderInbox.size() >= 64)
				senderInbox.erase(senderInbox.begin());
			senderInbox.push_back(wMsg);
		}

		outResp.messageId = wMsg.messageId;
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
		return RealmStatus::OK;
	}

	// 4. Standard Static Channel Broadcast (#General, #Trade, #LFG, #Torment)
	ChatMessageEntry entry {};
	entry.messageId = nextChatMessageId_++;
	entry.timestampSec = NowUnixSeconds();
	entry.channelId = req.channelId;
	entry.senderClass = senderClass;
	entry.senderLevel = senderLevel;
	entry.senderKillLevel = senderKillLevel;
	entry.flags = (senderKillLevel >= TormentRequiredDiabloKillLevel) ? CHAT_FLAG_TORMENT_CROWN : CHAT_FLAG_NONE;
	std::snprintf(entry.senderName, sizeof(entry.senderName), "%s", senderName.c_str());
	std::snprintf(entry.text, sizeof(entry.text), "%s", cleanText.c_str());

	auto &ring = channelHistory_[req.channelId];
	if (ring.size() >= 64) {
		ring.erase(ring.begin());
	}
	ring.push_back(entry);

	(void)remoteIp;
	outResp.messageId = entry.messageId;
	outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
	return RealmStatus::OK;
}

RealmStatus RealmSessionManager::PollChatChannel(
    int64_t accountId,
    uint8_t rawChannelId,
    uint64_t afterMessageId,
    uint16_t channelUsers,
    ChatChannelFeedResponsePayload &outResp) const
{
	outResp = {};
	outResp.channelId = rawChannelId;
	outResp.channelUsers = channelUsers;

	if (rawChannelId >= static_cast<uint8_t>(RealmChatChannel::COUNT)) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_CHAT_INVALID_CHANNEL);
		return RealmStatus::ERR_CHAT_INVALID_CHANNEL;
	}

	std::vector<ChatMessageEntry> merged;
	const auto &ring = channelHistory_[rawChannelId];
	for (const auto &m : ring) {
		if (m.messageId > afterMessageId) {
			merged.push_back(m);
		}
	}

	auto wIt = accountWhisperInbox_.find(accountId);
	if (wIt != accountWhisperInbox_.end()) {
		for (const auto &w : wIt->second) {
			if (w.messageId > afterMessageId) {
				merged.push_back(w);
			}
		}
	}

	std::sort(merged.begin(), merged.end(),
	    [](const ChatMessageEntry &a, const ChatMessageEntry &b) { return a.messageId < b.messageId; });

	if (merged.size() > MaxChatHistoryBatch) {
		merged.erase(merged.begin(), merged.end() - static_cast<std::ptrdiff_t>(MaxChatHistoryBatch));
	}

	outResp.messageCount = static_cast<uint8_t>(merged.size());
	uint64_t maxId = afterMessageId;
	for (size_t i = 0; i < merged.size(); ++i) {
		outResp.messages[i] = merged[i];
		maxId = std::max(maxId, merged[i].messageId);
	}
	outResp.latestMessageId = maxId;
	outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
	return RealmStatus::OK;
}

void RealmSessionManager::LeaveGame(uint64_t gameId, int64_t charId)
{
	auto it = games_.find(gameId);
	if (it == games_.end())
		return;

	ActiveGameSession &game = it->second;
	game.players.erase(
	    std::remove_if(game.players.begin(), game.players.end(),
	        [charId](const GamePlayerRuntimeState &p) { return p.charId == charId; }),
	    game.players.end());

	if (game.players.empty()) {
		games_.erase(it);
	}
}

RealmStatus RealmSessionManager::HandleFloorTransition(
    int64_t accountId,
    int64_t charId,
    const FloorTransitionRequestPayload &req,
    const std::string &remoteIp,
    FloorTransitionResponsePayload &outResp)
{
	outResp = {};
	auto it = games_.find(req.gameId);
	if (it == games_.end()) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_GAME_NOT_FOUND);
		return RealmStatus::ERR_GAME_NOT_FOUND;
	}

	ActiveGameSession &game = it->second;
	GamePlayerRuntimeState *player = nullptr;
	for (auto &p : game.players) {
		if (p.charId == charId && p.accountId == accountId) {
			player = &p;
			break;
		}
	}
	if (player == nullptr) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_UNAUTHORIZED);
		return RealmStatus::ERR_UNAUTHORIZED;
	}

	if (!IsFloorTransitionValid(player->currentLevel, req.toLevel, player->visitedFloors)) {
		db_.LogSecurityEvent(accountId, charId, req.gameId, "ALERT", "FLOOR_GATE_VIOLATION",
		    "Illegal floor transition " + std::to_string(player->currentLevel) + " -> "
		        + std::to_string(req.toLevel) + " from " + remoteIp);
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_ANOMALY_FLOOR_GATE_VIOLATION);
		outResp.currentLevel = player->currentLevel;
		return RealmStatus::ERR_ANOMALY_FLOOR_GATE_VIOLATION;
	}

	player->currentLevel = req.toLevel;
	player->visitedFloors.set(req.toLevel);
	player->lastTileX = req.tileX;
	player->lastTileY = req.tileY;
	player->lastKillTimestampMs = 0; // Reset floor walking cadence baseline on new floor

	outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
	outResp.currentLevel = player->currentLevel;
	return RealmStatus::OK;
}

uint32_t RealmSessionManager::DeriveOracleDropSeed(
    const ActiveGameSession &game,
    uint8_t currlevel,
    uint16_t entityIndex,
    uint8_t entityTag) const
{
	uint8_t msg[12] {};
	std::memcpy(msg, &game.gameId, sizeof(uint64_t));
	msg[8] = currlevel;
	std::memcpy(msg + 9, &entityIndex, sizeof(uint16_t));
	msg[11] = entityTag;

	uint8_t digest[16] {};
	crypto_generichash(digest, sizeof(digest), msg, sizeof(msg),
	    game.realmSecretSalt.data(), game.realmSecretSalt.size());

	uint32_t seed = 0;
	std::memcpy(&seed, digest, sizeof(uint32_t));
	if (seed == 0)
		seed = 0x015A4E35U;
	return seed;
}

void RealmSessionManager::RollOracleDungeonItem(
    LcgEngine &lcg,
    const ServerPlayerContext &playerCtx,
    int itemLevel,
    int uper,
    bool onlyGood,
    ServerItem &outItem) const
{
	outItem.clear();

	if (!onlyGood && lcg.GenerateRnd(100) < 40) {
		// Gold or Useful (Potion / Scroll) drop
		if (lcg.GenerateRnd(100) < 50) {
			InitializeServerItem(outItem, IDI_GOLD, playerCtx.isHellfire, playerCtx.nDifficulty);
			outItem._iSeed = lcg.GenerateSeed();
			int baseGold = (5 * itemLevel) + lcg.GenerateRnd(10 * itemLevel);
			if (playerCtx.nDifficulty == DIFF_NIGHTMARE)
				baseGold *= 2;
			else if (playerCtx.nDifficulty == DIFF_HELL)
				baseGold *= 3;
			else if (playerCtx.nDifficulty == DIFF_TORMENT) {
				// DX:Eternity DIFF_TORMENT rule: +30% max gold over Hell (* 130 / 100)
				baseGold = ((5 * itemLevel + (10 * itemLevel - 1)) * 3 * 130) / 100;
			}
			outItem._ivalue = std::clamp(baseGold, 1, GOLD_MAX_LIMIT);
			outItem._iCreateInfo = static_cast<uint16_t>(itemLevel & CF_LEVEL);
			return;
		}

		const uint32_t usefulSeed = lcg.GenerateSeed();
		SetupAllUseful(lcg, outItem, usefulSeed, itemLevel, playerCtx.isHellfire, playerCtx.nDifficulty);
		return;
	}

	const _item_indexes pickedIdx = PickDroppableItemIndex(lcg, itemLevel);
	const uint32_t itemSeed = lcg.GenerateSeed();
	SetupAllItems(lcg, playerCtx, outItem, pickedIdx, itemSeed, itemLevel, uper, onlyGood, false, false);
}

bool RealmSessionManager::RecordDispensedGroundItem(
    ActiveGameSession &game,
    uint8_t currlevel,
    uint8_t tileX,
    uint8_t tileY,
    uint8_t playerSlot,
    LedgerItemSource sourceType,
    const ServerItem &serverItem,
    OracleDroppedItemEntry &outEntry)
{
	outEntry = {};
	PkItemStruct packed {};
	PackServerItem(packed, serverItem, game.isHellfire);

	int64_t ledgerUid = 0;
	if (!db_.RecordLedgerItem(game.gameId, 0, packed, sourceType, currlevel, LedgerItemState::OnGround, &ledgerUid)) {
		return false;
	}

	TCmdGItem gitem {};
	gitem.bCmd = CMD_SEND_GITEM;
	gitem.bMaster = playerSlot;
	gitem.bPnum = playerSlot;
	gitem.bCursitem = 0;
	gitem.bLevel = currlevel;
	gitem.x = tileX;
	gitem.y = tileY;
	gitem.wIndx = packed.idx;
	gitem.wCI = packed.iCreateInfo;
	gitem.dwSeed = packed.iSeed;
	gitem.bId = packed.bId;
	gitem.bDur = packed.bDur;
	gitem.bMDur = packed.bMDur;
	gitem.bCh = packed.bCh;
	gitem.bMCh = packed.bMCh;
	gitem.wValue = packed.wValue;
	gitem.dwBuff = packed.dwBuff;
	gitem.dwTime = 0;

	GroundLedgerEntry groundEntry {};
	groundEntry.itemUid = static_cast<uint64_t>(ledgerUid);
	groundEntry.currlevel = currlevel;
	groundEntry.tileX = tileX;
	groundEntry.tileY = tileY;
	groundEntry.packed = packed;
	groundEntry.dropCmd = gitem;
	groundEntry.droppedAtMs = NowMonotonicMs();
	game.groundLedger.push_back(groundEntry);

	outEntry.itemUid = static_cast<uint64_t>(ledgerUid);
	outEntry.dropCmd = gitem;
	outEntry.packed = packed;
	return true;
}

RealmStatus RealmSessionManager::HandleMonsterDrop(
    int64_t accountId,
    int64_t charId,
    const MonsterDropRequestPayload &req,
    const std::string &remoteIp,
    MonsterOrChestDropResponsePayload &outResp)
{
	outResp = {};
	auto it = games_.find(req.gameId);
	if (it == games_.end()) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_GAME_NOT_FOUND);
		return RealmStatus::ERR_GAME_NOT_FOUND;
	}

	ActiveGameSession &game = it->second;
	GamePlayerRuntimeState *player = nullptr;
	for (auto &p : game.players) {
		if (p.charId == charId && p.accountId == accountId) {
			player = &p;
			break;
		}
	}
	if (player == nullptr) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_UNAUTHORIZED);
		return RealmStatus::ERR_UNAUTHORIZED;
	}

	if (req.currlevel < 1 || req.currlevel > MaxDungeonLevel || req.monsterIndex >= MaxFloorMonsters) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_MALFORMED_PACKET);
		return RealmStatus::ERR_MALFORMED_PACKET;
	}

	// Floor progression gate: player must currently be on `req.currlevel`
	if (player->currentLevel != req.currlevel || !player->visitedFloors.test(req.currlevel)) {
		db_.LogSecurityEvent(accountId, charId, req.gameId, "ALERT", "KILL_UNVISITED_FLOOR",
		    "Rejected OP_MONSTER_DROP_REQ on floor " + std::to_string(req.currlevel)
		        + " while player is on floor " + std::to_string(player->currentLevel) + " from " + remoteIp);
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_ANOMALY_FLOOR_GATE_VIOLATION);
		return RealmStatus::ERR_ANOMALY_FLOOR_GATE_VIOLATION;
	}

	GameFloorState &floor = game.floors[req.currlevel];
	if (floor.killedMonsters.test(req.monsterIndex)) {
		db_.LogSecurityEvent(accountId, charId, req.gameId, "WARN", "DUPLICATE_MONSTER_KILL",
		    "Duplicate OP_MONSTER_DROP_REQ for floor=" + std::to_string(req.currlevel)
		        + " monsterIndex=" + std::to_string(req.monsterIndex) + " from " + remoteIp);
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_DUPLICATE_KILL_OR_CHEST);
		return RealmStatus::ERR_DUPLICATE_KILL_OR_CHEST;
	}

	const EffectiveMonsterStats mstats = CalculateEffectiveMonsterStats(
	    req.monsterTypeOrUid, req.isUniqueMonster != 0, game.difficulty, game.isHellfire);

	// Phase 6: Authoritative Combat Ceiling, Multi-Hit TTK, and Travel-Distance / Kill-Cadence Auditing
	const AuthoritativeCombatCeiling ceiling = CalculateAuthoritativeCombatCeiling(player->initialPack);
	const uint64_t nowMs = NowMonotonicMs();
	std::string anomalyReason;
	const RealmStatus anomalyStatus = AuditMonsterKillAnomaly(ceiling, mstats, req, nowMs, *player, anomalyReason);
	if (anomalyStatus != RealmStatus::OK) {
		const char *category = (anomalyStatus == RealmStatus::ERR_ANOMALY_CADENCE_VIOLATION)
		    ? "KILL_CADENCE_ANOMALY"
		    : "TTK_DAMAGE_ANOMALY";
		db_.LogSecurityEvent(accountId, charId, req.gameId, "ALERT", category,
		    "Rejected OP_MONSTER_DROP_REQ on floor=" + std::to_string(req.currlevel)
		        + " (" + anomalyReason + ") from " + remoteIp);
		outResp.statusCode = static_cast<uint16_t>(anomalyStatus);
		return anomalyStatus;
	}

	// Mark monster as slain on this floor
	floor.killedMonsters.set(req.monsterIndex);
	player->verifiedExpGained += mstats.expReward;
	player->lastTileX = req.tileX;
	player->lastTileY = req.tileY;
	player->lastKillTimestampMs = nowMs;

	// Anti-Boost Safeguard on Hell Diablo Kill (`MT_DIABLO` on `DIFF_HELL`):
	// If there is > 10 level difference between the highest level player in the game and another player,
	// that player does NOT receive credit for killing Hell Diablo (`pDiabloKillLevel = 3`) unless their level is >= 32.
	if (mstats.baseType == MT_DIABLO && game.difficulty == DIFF_HELL) {
		int highestPartyLevel = 1;
		for (const auto &partyMember : game.players) {
			highestPartyLevel = std::max<int>(highestPartyLevel, partyMember.initialPack.pLevel);
		}
		for (auto &partyMember : game.players) {
			const int memberLevel = partyMember.initialPack.pLevel;
			if ((highestPartyLevel - memberLevel) > 10 && memberLevel < 32) {
				partyMember.hellDiabloBoostBlocked = true;
				partyMember.hellDiabloCreditGranted = false;
				db_.LogSecurityEvent(partyMember.accountId, partyMember.charId, req.gameId, "INFO", "HELL_DIABLO_BOOST_BLOCKED",
				    "Withheld Hell Diablo kill credit (playerLevel=" + std::to_string(memberLevel)
				        + ", highestPartyLevel=" + std::to_string(highestPartyLevel) + ", diff="
				        + std::to_string(highestPartyLevel - memberLevel) + " > 10 and level < 32)");
			} else {
				partyMember.hellDiabloCreditGranted = true;
			}
		}
	}

	outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
	outResp.currlevel = req.currlevel;
	outResp.awardedExp = mstats.expReward;
	outResp.dropCount = 0;

	if ((mstats.treasureFlags & T_NODROP) != 0) {
		return RealmStatus::OK;
	}

	const uint32_t dropSeed = DeriveOracleDropSeed(game, req.currlevel, req.monsterIndex, 0xA1U);
	LcgEngine lcg(dropSeed);

	ServerPlayerContext playerCtx = ServerPlayerContext::FromPlayerPack(player->initialPack);
	playerCtx.nDifficulty = game.difficulty;
	playerCtx.isHellfire = game.isHellfire;

	const bool isUnique = (req.isUniqueMonster != 0) || ((mstats.treasureFlags & T_UNIQ) != 0);
	const int itemLevel = std::clamp(mstats.level, 1, 63);

	// 1. Standard / Unique Monster Drop
	const bool shouldDropStandard = isUnique || (lcg.GenerateRnd(100) < 40);
	if (shouldDropStandard && outResp.dropCount < MaxDropsPerKill) {
		ServerItem droppedItem {};
		const int uper = isUnique ? 15 : 1;
		const bool onlyGood = isUnique;
		RollOracleDungeonItem(lcg, playerCtx, itemLevel, uper, onlyGood, droppedItem);

		if (!droppedItem.isEmpty()) {
			if (droppedItem._itype == ItemType::Gold) {
				player->verifiedGoldDropped += static_cast<uint64_t>(droppedItem._ivalue);
			}
			OracleDroppedItemEntry entry {};
			if (RecordDispensedGroundItem(game, req.currlevel, req.tileX, req.tileY, player->playerSlot,
			        LedgerItemSource::MonsterDrop, droppedItem, entry)) {
				outResp.drops[outResp.dropCount++] = entry;
			}
		}
	}

	// 2. DX:Eternity Hell & Torment Diablo Exclusive Uniques (2% independent chance each):
	//    - UITEM_KADISFURY (110)
	//    - UITEM_APOSTATESCROOK (111)
	//    - UITEM_LOXLEYSBEND (112)
	if (mstats.baseType == MT_DIABLO && (game.difficulty == DIFF_HELL || game.difficulty == DIFF_TORMENT)) {
		constexpr _unique_items kDiabloExclusives[3] = {
			UITEM_KADISFURY,
			UITEM_APOSTATESCROOK,
			UITEM_LOXLEYSBEND,
		};

		for (int i = 0; i < 3 && outResp.dropCount < MaxDropsPerKill; ++i) {
			if (lcg.GenerateRnd(100) < 2) {
				ServerItem exclusiveItem {};
				SpawnUniqueServerItem(lcg, playerCtx, exclusiveItem, kDiabloExclusives[i], itemLevel);
				OracleDroppedItemEntry entry {};
				if (RecordDispensedGroundItem(game, req.currlevel,
				        static_cast<uint8_t>(req.tileX + i), req.tileY, player->playerSlot,
				        LedgerItemSource::MonsterDrop, exclusiveItem, entry)) {
					outResp.drops[outResp.dropCount++] = entry;
				}
			}
		}
	}

	return RealmStatus::OK;
}

RealmStatus RealmSessionManager::HandleChestDrop(
    int64_t accountId,
    int64_t charId,
    const ChestDropRequestPayload &req,
    const std::string &remoteIp,
    MonsterOrChestDropResponsePayload &outResp)
{
	outResp = {};
	auto it = games_.find(req.gameId);
	if (it == games_.end()) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_GAME_NOT_FOUND);
		return RealmStatus::ERR_GAME_NOT_FOUND;
	}

	ActiveGameSession &game = it->second;
	GamePlayerRuntimeState *player = nullptr;
	for (auto &p : game.players) {
		if (p.charId == charId && p.accountId == accountId) {
			player = &p;
			break;
		}
	}
	if (player == nullptr) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_UNAUTHORIZED);
		return RealmStatus::ERR_UNAUTHORIZED;
	}

	if (req.currlevel < 1 || req.currlevel > MaxDungeonLevel || req.objectIndex >= MaxFloorObjects) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_MALFORMED_PACKET);
		return RealmStatus::ERR_MALFORMED_PACKET;
	}

	if (player->currentLevel != req.currlevel || !player->visitedFloors.test(req.currlevel)) {
		db_.LogSecurityEvent(accountId, charId, req.gameId, "ALERT", "CHEST_UNVISITED_FLOOR",
		    "Rejected OP_CHEST_DROP_REQ on floor " + std::to_string(req.currlevel)
		        + " while player is on floor " + std::to_string(player->currentLevel) + " from " + remoteIp);
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_ANOMALY_FLOOR_GATE_VIOLATION);
		return RealmStatus::ERR_ANOMALY_FLOOR_GATE_VIOLATION;
	}

	GameFloorState &floor = game.floors[req.currlevel];
	if (floor.openedObjects.test(req.objectIndex)) {
		db_.LogSecurityEvent(accountId, charId, req.gameId, "WARN", "DUPLICATE_CHEST_OPEN",
		    "Duplicate OP_CHEST_DROP_REQ for floor=" + std::to_string(req.currlevel)
		        + " objectIndex=" + std::to_string(req.objectIndex) + " from " + remoteIp);
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_DUPLICATE_KILL_OR_CHEST);
		return RealmStatus::ERR_DUPLICATE_KILL_OR_CHEST;
	}

	floor.openedObjects.set(req.objectIndex);

	const uint32_t dropSeed = DeriveOracleDropSeed(game, req.currlevel, req.objectIndex, 0xC2U);
	LcgEngine lcg(dropSeed);

	ServerPlayerContext playerCtx = ServerPlayerContext::FromPlayerPack(player->initialPack);
	playerCtx.nDifficulty = game.difficulty;
	playerCtx.isHellfire = game.isHellfire;

	int baseItemLvl = static_cast<int>(req.currlevel) * 2;
	if (game.difficulty == DIFF_NIGHTMARE)
		baseItemLvl += 15;
	else if (game.difficulty == DIFF_HELL)
		baseItemLvl += 30;
	else if (game.difficulty == DIFF_TORMENT)
		baseItemLvl = ((baseItemLvl + 30) * 130) / 100;
	baseItemLvl = std::clamp(baseItemLvl, 1, 63);

	const int maxChestDrops = std::clamp<int>(req.chestQuality + 1, 1, 3);
	outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
	outResp.currlevel = req.currlevel;
	outResp.awardedExp = 0;
	outResp.dropCount = 0;

	for (int i = 0; i < maxChestDrops && outResp.dropCount < MaxDropsPerKill; ++i) {
		ServerItem chestItem {};
		const bool onlyGood = (req.chestQuality >= 2 && i == 0);
		RollOracleDungeonItem(lcg, playerCtx, baseItemLvl, 1, onlyGood, chestItem);
		if (!chestItem.isEmpty()) {
			if (chestItem._itype == ItemType::Gold) {
				player->verifiedGoldDropped += static_cast<uint64_t>(chestItem._ivalue);
			}
			OracleDroppedItemEntry entry {};
			if (RecordDispensedGroundItem(game, req.currlevel, req.tileX, req.tileY, player->playerSlot,
			        LedgerItemSource::ChestDrop, chestItem, entry)) {
				outResp.drops[outResp.dropCount++] = entry;
			}
		}
	}

	return RealmStatus::OK;
}

RealmStatus RealmSessionManager::HandleItemPickup(
    int64_t accountId,
    int64_t charId,
    const ItemPickupRequestPayload &req,
    const std::string &remoteIp,
    ItemPickupResponsePayload &outResp)
{
	outResp = {};
	auto it = games_.find(req.gameId);
	if (it == games_.end()) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_GAME_NOT_FOUND);
		return RealmStatus::ERR_GAME_NOT_FOUND;
	}

	ActiveGameSession &game = it->second;
	GamePlayerRuntimeState *player = nullptr;
	for (auto &p : game.players) {
		if (p.charId == charId && p.accountId == accountId) {
			player = &p;
			break;
		}
	}
	if (player == nullptr) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_UNAUTHORIZED);
		return RealmStatus::ERR_UNAUTHORIZED;
	}

	auto groundIt = std::find_if(game.groundLedger.begin(), game.groundLedger.end(),
	    [&req](const GroundLedgerEntry &e) {
		    if (e.currlevel != req.currlevel)
			    return false;
		    if (req.itemUid != 0)
			    return e.itemUid == req.itemUid;
		    return e.packed.iSeed == req.iSeed && e.packed.idx == req.idx && e.packed.iCreateInfo == req.iCreateInfo;
	    });

	if (groundIt == game.groundLedger.end()) {
		db_.LogSecurityEvent(accountId, charId, req.gameId, "WARN", "PICKUP_NOT_ON_GROUND",
		    "Rejected OP_ITEM_PICKUP_REQ for missing/already-picked item (uid=" + std::to_string(req.itemUid)
		        + ", seed=" + std::to_string(req.iSeed) + ") from " + remoteIp);
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_ITEM_NOT_ON_GROUND);
		return RealmStatus::ERR_ITEM_NOT_ON_GROUND;
	}

	const GroundLedgerEntry pickedEntry = *groundIt;
	if (!db_.TransitionLedgerItemState(
	        static_cast<int64_t>(pickedEntry.itemUid),
	        game.gameId,
	        charId,
	        LedgerItemState::OnGround,
	        LedgerItemState::InInventoryOrStash)) {
		db_.LogSecurityEvent(accountId, charId, req.gameId, "ALERT", "PICKUP_RACE_OR_DUPE",
		    "Atomic SQLite ledger transition failed for itemUid=" + std::to_string(pickedEntry.itemUid)
		        + " from " + remoteIp);
		game.groundLedger.erase(groundIt);
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_ITEM_NOT_ON_GROUND);
		return RealmStatus::ERR_ITEM_NOT_ON_GROUND;
	}

	game.groundLedger.erase(groundIt);

	outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
	outResp.itemUid = pickedEntry.itemUid;
	outResp.item = pickedEntry.packed;
	return RealmStatus::OK;
}

RealmStatus RealmSessionManager::HandleItemDrop(
    int64_t accountId,
    int64_t charId,
    const ItemDropRequestPayload &req,
    const std::string &remoteIp,
    ItemDropResponsePayload &outResp)
{
	outResp = {};
	auto it = games_.find(req.gameId);
	if (it == games_.end()) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_GAME_NOT_FOUND);
		return RealmStatus::ERR_GAME_NOT_FOUND;
	}

	ActiveGameSession &game = it->second;
	GamePlayerRuntimeState *player = nullptr;
	for (auto &p : game.players) {
		if (p.charId == charId && p.accountId == accountId) {
			player = &p;
			break;
		}
	}
	if (player == nullptr) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_UNAUTHORIZED);
		return RealmStatus::ERR_UNAUTHORIZED;
	}

	if (player->currentLevel != req.currlevel || !player->visitedFloors.test(req.currlevel)) {
		db_.LogSecurityEvent(accountId, charId, req.gameId, "ALERT", "DROP_UNVISITED_FLOOR",
		    "Rejected OP_ITEM_DROP_REQ on floor " + std::to_string(req.currlevel) + " from " + remoteIp);
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_ANOMALY_FLOOR_GATE_VIOLATION);
		return RealmStatus::ERR_ANOMALY_FLOOR_GATE_VIOLATION;
	}

	std::string verifyErr;
	ServerItem recreated {};
	if (!VerifyPackedItemIntegrity(player->initialPack, req.item, verifyErr, &recreated)) {
		db_.LogSecurityEvent(accountId, charId, req.gameId, "ALERT", "FORGED_ITEM_DROP",
		    "Rejected forged OP_ITEM_DROP_REQ (" + verifyErr + ") from " + remoteIp);
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_SAVE_AUDIT_FAILED);
		return RealmStatus::ERR_SAVE_AUDIT_FAILED;
	}

	// Prevent drop-and-pickup ledger duplication:
	// First attempt to transition an existing owned `InInventoryOrStash` ledger row to `OnGround`.
	int64_t existingLedgerUid = 0;
	if (db_.TransitionOwnedItemToGround(charId, game.gameId, req.item, existingLedgerUid)) {
		PkItemStruct packed {};
		PackServerItem(packed, recreated, game.isHellfire);

		TCmdGItem gitem {};
		gitem.bCmd = CMD_SEND_GITEM;
		gitem.bMaster = player->playerSlot;
		gitem.bPnum = player->playerSlot;
		gitem.bCursitem = 0;
		gitem.bLevel = req.currlevel;
		gitem.x = req.tileX;
		gitem.y = req.tileY;
		gitem.wIndx = packed.idx;
		gitem.wCI = packed.iCreateInfo;
		gitem.dwSeed = packed.iSeed;
		gitem.bId = packed.bId;
		gitem.bDur = packed.bDur;
		gitem.bMDur = packed.bMDur;
		gitem.bCh = packed.bCh;
		gitem.bMCh = packed.bMCh;
		gitem.wValue = packed.wValue;
		gitem.dwBuff = packed.dwBuff;
		gitem.dwTime = 0;

		GroundLedgerEntry groundEntry {};
		groundEntry.itemUid = static_cast<uint64_t>(existingLedgerUid);
		groundEntry.currlevel = req.currlevel;
		groundEntry.tileX = req.tileX;
		groundEntry.tileY = req.tileY;
		groundEntry.packed = packed;
		groundEntry.dropCmd = gitem;
		groundEntry.droppedAtMs = NowMonotonicMs();
		game.groundLedger.push_back(groundEntry);

		outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
		outResp.dropped.itemUid = static_cast<uint64_t>(existingLedgerUid);
		outResp.dropped.dropCmd = gitem;
		outResp.dropped.packed = packed;
		return RealmStatus::OK;
	}

	// Only town-bought items (`CF_TOWN`), gold (`IDI_GOLD`), or consumables (`CF_USEFUL`) may create a new ledger row on drop
	const bool isLegitUnregisteredDrop = (recreated.IDidx == IDI_GOLD)
	    || ((req.item.iCreateInfo & CF_TOWN) != 0)
	    || ((req.item.iCreateInfo & CF_USEFUL) != 0);
	if (!isLegitUnregisteredDrop) {
		db_.LogSecurityEvent(accountId, charId, req.gameId, "ALERT", "UNOWNED_DUNGEON_ITEM_DROP",
		    "Rejected OP_ITEM_DROP_REQ for dungeon item with no owned ledger provenance (seed="
		        + std::to_string(req.item.iSeed) + ") from " + remoteIp);
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_SAVE_AUDIT_FAILED);
		return RealmStatus::ERR_SAVE_AUDIT_FAILED;
	}

	OracleDroppedItemEntry entry {};
	if (!RecordDispensedGroundItem(game, req.currlevel, req.tileX, req.tileY, player->playerSlot,
	        LedgerItemSource::VendorOrReward, recreated, entry)) {
		outResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_INTERNAL_DATABASE);
		return RealmStatus::ERR_INTERNAL_DATABASE;
	}

	outResp.statusCode = static_cast<uint16_t>(RealmStatus::OK);
	outResp.dropped = entry;
	return RealmStatus::OK;
}

const ActiveGameSession *RealmSessionManager::FindGame(uint64_t gameId) const
{
	auto it = games_.find(gameId);
	return it != games_.end() ? &it->second : nullptr;
}

const GamePlayerRuntimeState *RealmSessionManager::FindPlayerInGame(uint64_t gameId, int64_t charId) const
{
	const ActiveGameSession *game = FindGame(gameId);
	if (game == nullptr)
		return nullptr;
	for (const auto &p : game->players) {
		if (p.charId == charId)
			return &p;
	}
	return nullptr;
}

} // namespace devilution::realm
