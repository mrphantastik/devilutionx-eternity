/**
 * @file realm_client.h
 * @brief Closed Realm (dx-eternity-realm) client networking, authentication, binary attestation,
 *        Server-Vault character/stash persistence, game session registry, and drop oracle hooks.
 */

#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/point.hpp"
#include "items.h"
#include "levels/gendung.h"
#include "monster.h"
#include "net/realm_protocol.hpp"
#include "objects.h"
#include "pack.h"
#include "player.h"

namespace devilution {

extern bool gbIsClosedRealm;

class RealmClient {
public:
	RealmClient();
	~RealmClient();

	RealmClient(const RealmClient &) = delete;
	RealmClient &operator=(const RealmClient &) = delete;

	void SetServerEndpoint(std::string host, uint16_t port = realm::DefaultRealmPort);
	const std::string &GetHost() const { return serverHost_; }
	uint16_t GetPort() const { return serverPort_; }

	bool IsConnected() const { return connected_; }
	bool IsAuthenticated() const { return authenticated_; }
	uint64_t GetAccountId() const { return accountId_; }
	const std::string &GetUsername() const { return username_; }

	void Disconnect();

	bool RegisterAccount(const std::string &username, const std::string &password, std::string &outError);
	bool LoginAccount(const std::string &username, const std::string &password, std::string &outError);

	// Server-Vault Character & Stash Management
	bool ListHeroes(std::vector<realm::HeroSummaryInfo> &outHeroes, std::string &outError);
	bool CreateHero(const std::string &heroName, uint8_t heroClass, realm::HeroSummaryInfo &outSummary, PlayerPack &outPack, std::string &outError);
	bool DeleteHero(uint64_t charId, std::string &outError);
	bool LoadHero(uint64_t charId, PlayerPack &outPack, std::string &outError);
	bool SaveHero(uint64_t charId, const PlayerPack &pack, std::string &outError);

	bool LoadStashFromRealm();
	bool SaveStashToRealm();

	void SetSlotCharId(uint32_t saveNum, uint64_t charId);
	uint64_t GetSlotCharId(uint32_t saveNum) const;
	void ClearSlotCharIds();
	void SetActiveCharId(uint64_t charId) { activeCharId_ = charId; }
	uint64_t GetActiveCharId() const { return activeCharId_; }
	uint64_t GetActiveGameId() const { return activeGameId_; }
	void WriteLocalCharacterBackup(const PlayerPack &pack);

	// Game Session & Oracle Operations
	bool ListGames(std::vector<realm::GameLobbyInfo> &outGames, std::string &outError);
	bool CreateGame(const std::string &gameName, const std::string &gamePassword, uint8_t difficulty, uint8_t tickRate, realm::GameCreateOrJoinResponsePayload &outResp, std::string &outError);
	bool JoinGame(const std::string &gameName, const std::string &gamePassword, realm::GameCreateOrJoinResponsePayload &outResp, std::string &outError);

	void NotifyFloorTransition(uint8_t targetFloor, Point pos);
	bool HandleMonsterKill(const Monster &monster, bool sendmsg);
	bool HandleChestOpen(const Object &chest, bool sendLootMsg);
	bool NotifyItemPickup(const Item &item);
	bool NotifyItemDrop(const Item &item, Point pos);

	bool SendRawRpc(
	    realm::RealmOpcode reqOpcode,
	    const void *reqPayload,
	    uint32_t reqLen,
	    realm::RealmOpcode expectedRespOpcode,
	    void *outResp,
	    uint32_t outRespLen);

private:
	bool OpenAndAttestSocket(std::string &outError);
	bool EnsureAuthenticatedSession(std::string &outError);
	bool SendAndReceive(
	    realm::RealmOpcode opcode,
	    const void *reqPayload,
	    uint32_t reqLen,
	    realm::RealmPacketHeader &outHdr,
	    std::vector<uint8_t> &outPayload,
	    std::string &outError);

	void RecordGroundItemUid(const realm::PkItemStruct &packed, uint64_t itemUid);
	uint64_t LookupGroundItemUid(uint32_t iSeed, uint16_t idx, uint16_t iCreateInfo) const;

	mutable std::mutex mutex_;
	std::string serverHost_ = "127.0.0.1";
	uint16_t serverPort_ = realm::DefaultRealmPort;
	uintptr_t sock_ = ~uintptr_t(0);

	bool connected_ = false;
	bool authenticated_ = false;
	bool hasResumeToken_ = false;
	uint64_t accountId_ = 0;
	uint8_t sessionToken_[32] {};
	uint8_t serverNonce_[32] {};
	uint8_t trafficKey_[32] {};
	uint32_t nextSeq_ = 1;
	std::string username_;

	std::array<uint64_t, 64> slotCharIds_ {};
	uint64_t activeCharId_ = 0;
	uint64_t activeGameId_ = 0;
	uint8_t currentServerFloor_ = 0;

	struct GroundItemKey {
		uint32_t iSeed;
		uint16_t idx;
		uint16_t iCreateInfo;
		bool operator==(const GroundItemKey &o) const noexcept
		{
			return iSeed == o.iSeed && idx == o.idx && iCreateInfo == o.iCreateInfo;
		}
	};
	struct GroundItemKeyHash {
		size_t operator()(const GroundItemKey &k) const noexcept
		{
			return (static_cast<size_t>(k.iSeed) << 16U) ^ (static_cast<size_t>(k.idx) << 8U) ^ static_cast<size_t>(k.iCreateInfo);
		}
	};
	std::unordered_map<GroundItemKey, uint64_t, GroundItemKeyHash> floorItemUids_;
};

RealmClient &GetRealmClient();

void RealmClientLog(const char *severity, const char *category, const char *fmt, ...);

#ifdef REALM_CHAOS_TEST
void RunRealmChaosExploitTest(int exploitId);
#endif

void ConvertClientPackToRealmPack(const PlayerPack &src, realm::RealmPlayerPack &dst);
void ConvertRealmPackToClientPack(const realm::RealmPlayerPack &src, PlayerPack &dst);

} // namespace devilution
