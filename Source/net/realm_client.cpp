#include "net/realm_client.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <sodium.h>

#include "engine/random.hpp"
#include "inv.h"
#include "items.h"
#include "loadsave.h"
#include "msg.h"
#include "multi.h"
#include "options.h"
#include "pack.h"
#include "pfile.h"
#include "player.h"
#include "qol/stash.h"
#include "utils/log.hpp"
#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution {

bool gbIsClosedRealm = false;

namespace {

// Matches Server/net/packet_codec.cpp GetDefaultHandshakeKey()
constexpr std::array<uint8_t, 32> DefaultHandshakeKey = {
	0x44, 0x58, 0x2D, 0x45, 0x54, 0x45, 0x52, 0x4E,
	0x49, 0x54, 0x59, 0x2D, 0x52, 0x45, 0x41, 0x4C,
	0x4D, 0x2D, 0x56, 0x31, 0x2D, 0x48, 0x41, 0x4E,
	0x44, 0x53, 0x48, 0x41, 0x4B, 0x45, 0x2D, 0x4B
};

// Matches Server/auth/realm_auth.cpp GetDefaultAttestationKey()
constexpr std::array<uint8_t, 32> DefaultAttestationKey = {
	0x8F, 0x3A, 0xC1, 0x94, 0x2B, 0x7E, 0x5D, 0x10,
	0xE6, 0x42, 0x9B, 0x08, 0x77, 0xD3, 0x19, 0xA5,
	0x61, 0xF0, 0x3C, 0x88, 0x54, 0x2E, 0xB9, 0x72,
	0x0D, 0xC6, 0x4F, 0x91, 0xA8, 0x33, 0xE2, 0x5B
};

void CloseRawSocketHandle(uintptr_t &sock)
{
	if (sock == ~uintptr_t(0))
		return;
#ifdef _WIN32
	::closesocket(static_cast<SOCKET>(sock));
#else
	::close(static_cast<int>(sock));
#endif
	sock = ~uintptr_t(0);
}

void ComputeRunningExecutableTextSectionHash(uint8_t outHash[32])
{
	std::memset(outHash, 0, 32);
#ifdef _WIN32
	const auto *base = reinterpret_cast<const uint8_t *>(GetModuleHandleA(nullptr));
	if (base != nullptr) {
		const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
		if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
			const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
			if (nt->Signature == IMAGE_NT_SIGNATURE) {
				const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
				for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
					if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
						const uint8_t *textPtr = base + sec[i].VirtualAddress;
						const DWORD textLen = sec[i].Misc.VirtualSize > 0 ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
						crypto_generichash_blake2b(outHash, 32, textPtr, textLen, nullptr, 0);
						return;
					}
				}
			}
		}
	}
#endif
	const char fallbackTag[] = "DXE-TEXT-FALLBACK";
	crypto_generichash_blake2b(outHash, 32, reinterpret_cast<const unsigned char *>(fallbackTag), sizeof(fallbackTag) - 1, nullptr, 0);
}

void ComputeClientAttestationProof(
    const uint8_t serverNonce[32],
    const uint8_t clientNonce[32],
    const uint8_t textSectionHash[32],
    uint32_t buildVersion,
    uint8_t outProof[32])
{
	crypto_generichash_state st {};
	crypto_generichash_init(&st, DefaultAttestationKey.data(), DefaultAttestationKey.size(), 32);
	crypto_generichash_update(&st, serverNonce, 32);
	crypto_generichash_update(&st, clientNonce, 32);
	crypto_generichash_update(&st, textSectionHash, 32);
	const uint8_t verLe[4] = {
		static_cast<uint8_t>(buildVersion & 0xFFU),
		static_cast<uint8_t>((buildVersion >> 8U) & 0xFFU),
		static_cast<uint8_t>((buildVersion >> 16U) & 0xFFU),
		static_cast<uint8_t>((buildVersion >> 24U) & 0xFFU)
	};
	crypto_generichash_update(&st, verLe, sizeof(verLe));
	crypto_generichash_final(&st, outProof, 32);
}

void DeriveSessionTrafficKey(
    const uint8_t serverNonce[32],
    const uint8_t clientNonce[32],
    uint8_t outTrafficKey[32])
{
	crypto_generichash_state st {};
	const char label[] = "DXER-TRAFFIC-V1";
	crypto_generichash_init(&st, DefaultAttestationKey.data(), DefaultAttestationKey.size(), 32);
	crypto_generichash_update(&st, reinterpret_cast<const unsigned char *>(label), sizeof(label) - 1);
	crypto_generichash_update(&st, serverNonce, 32);
	crypto_generichash_update(&st, clientNonce, 32);
	crypto_generichash_final(&st, outTrafficKey, 32);
}

void ComputePacketMac(
    const realm::RealmPacketHeader &hdr,
    const uint8_t *payload,
    std::size_t payloadLen,
    const uint8_t key[32],
    uint8_t outMac[realm::PacketMacSize])
{
	realm::RealmPacketHeader canonicalHeader = hdr;
	std::memset(canonicalHeader.mac, 0, sizeof(canonicalHeader.mac));
	crypto_generichash_state st {};
	crypto_generichash_init(&st, key, 32, realm::PacketMacSize);
	crypto_generichash_update(&st, reinterpret_cast<const unsigned char *>(&canonicalHeader), sizeof(canonicalHeader));
	if (payload != nullptr && payloadLen > 0)
		crypto_generichash_update(&st, payload, payloadLen);
	crypto_generichash_final(&st, outMac, realm::PacketMacSize);
}

bool SendAllBytes(uintptr_t sock, const uint8_t *buf, std::size_t len)
{
	std::size_t sentTotal = 0;
	while (sentTotal < len) {
#ifdef _WIN32
		int n = ::send(static_cast<SOCKET>(sock), reinterpret_cast<const char *>(buf + sentTotal), static_cast<int>(len - sentTotal), 0);
#else
		ssize_t n = ::send(static_cast<int>(sock), buf + sentTotal, len - sentTotal, 0);
#endif
		if (n <= 0)
			return false;
		sentTotal += static_cast<std::size_t>(n);
	}
	return true;
}

bool RecvAllBytes(uintptr_t sock, uint8_t *buf, std::size_t len)
{
	std::size_t recvdTotal = 0;
	while (recvdTotal < len) {
#ifdef _WIN32
		int n = ::recv(static_cast<SOCKET>(sock), reinterpret_cast<char *>(buf + recvdTotal), static_cast<int>(len - recvdTotal), 0);
#else
		ssize_t n = ::recv(static_cast<int>(sock), buf + recvdTotal, len - recvdTotal, 0);
#endif
		if (n <= 0)
			return false;
		recvdTotal += static_cast<std::size_t>(n);
	}
	return true;
}

void CopyItemPackToRealm(const ItemPack &src, realm::PkItemStruct &dst)
{
	static_assert(sizeof(ItemPack) == sizeof(realm::PkItemStruct), "ItemPack and PkItemStruct must match in size");
	std::memcpy(&dst, &src, sizeof(realm::PkItemStruct));
}

void CopyRealmToItemPack(const realm::PkItemStruct &src, ItemPack &dst)
{
	static_assert(sizeof(ItemPack) == sizeof(realm::PkItemStruct), "ItemPack and PkItemStruct must match in size");
	std::memcpy(&dst, &src, sizeof(ItemPack));
}

void UnpackDropAndSpawnFloorItem(const realm::RealmCmdGItem &gitem, Point fallbackPos)
{
	ItemPack pk {};
	pk.iSeed = gitem.dwSeed;
	pk.iCreateInfo = gitem.wCI;
	pk.idx = gitem.wIndx;
	pk.bId = gitem.bId;
	pk.bDur = gitem.bDur;
	pk.bMDur = gitem.bMDur;
	pk.bCh = gitem.bCh;
	pk.bMCh = gitem.bMCh;
	pk.wValue = gitem.wValue;
	pk.dwBuff = gitem.dwBuff;

	Item tempItem {};
	UnPackItem(pk, MyPlayer != nullptr ? *MyPlayer : Players[0], tempItem, (gitem.dwBuff & CF_HELLFIRE) != 0);
	Point pos { gitem.x, gitem.y };
	if (!InDungeonBounds(pos))
		pos = fallbackPos;
	const int ii = AllocateItem();
	if (ii < 0)
		return;
	Items[ii] = tempItem;
	Items[ii].position = pos;
	RespawnItem(Items[ii], true);
	dItem[pos.x][pos.y] = ii + 1;
}

} // namespace

void ConvertClientPackToRealmPack(const PlayerPack &src, realm::RealmPlayerPack &dst)
{
	std::memset(&dst, 0, sizeof(dst));
	dst.dwLowDateTime = src.dwLowDateTime;
	dst.dwHighDateTime = src.dwHighDateTime;
	dst.destAction = src.destAction;
	dst.destParam1 = src.destParam1;
	dst.destParam2 = src.destParam2;
	dst.plrlevel = src.plrlevel;
	dst.px = src.px;
	dst.py = src.py;
	dst.targx = src.targx;
	dst.targy = src.targy;
	std::memcpy(dst.pName, src.pName, sizeof(dst.pName));
	dst.pClass = src.pClass;
	dst.pBaseStr = src.pBaseStr;
	dst.pBaseMag = src.pBaseMag;
	dst.pBaseDex = src.pBaseDex;
	dst.pBaseVit = src.pBaseVit;
	dst.pLevel = src.pLevel;
	dst.pStatPts = src.pStatPts;
	dst.pExperience = src.pExperience;
	dst.pGold = src.pGold;
	dst.pHPBase = src.pHPBase;
	dst.pMaxHPBase = src.pMaxHPBase;
	dst.pManaBase = src.pManaBase;
	dst.pMaxManaBase = src.pMaxManaBase;
	std::memcpy(dst.pSplLvl, src.pSplLvl, 37);
	dst.pMemSpells = src.pMemSpells;
	for (int i = 0; i < 7; ++i)
		CopyItemPackToRealm(src.InvBody[i], dst.InvBody[i]);
	for (int i = 0; i < 40; ++i)
		CopyItemPackToRealm(src.InvList[i], dst.InvList[i]);
	dst._pNumInv = src._pNumInv;
	std::memcpy(dst.InvGrid, src.InvGrid, 40);
	for (int i = 0; i < 8; ++i)
		CopyItemPackToRealm(src.SpdList[i], dst.SpdList[i]);
	dst.pTownWarps = src.pTownWarps;
	dst.pDungMsgs = src.pDungMsgs;
	dst.pLvlLoad = src.pLvlLoad;
	dst.pBattleNet = src.pBattleNet;
	dst.pManaShield = src.pManaShield;
	dst.pDungMsgs2 = src.pDungMsgs2;
	dst.bIsHellfire = 1;
	dst.wReflections = src.wReflections;
	std::memcpy(dst.pSplLvl2, &src.pSplLvl[37], 10);
	dst.pDiabloKillLevel = src.pDiabloKillLevel;
	dst.pDifficulty = src.pDifficulty;
	dst.pDamAcFlags = static_cast<uint32_t>(src.pDamAcFlags);
}

void ConvertRealmPackToClientPack(const realm::RealmPlayerPack &src, PlayerPack &dst)
{
	std::memset(&dst, 0, sizeof(dst));
	dst.dwLowDateTime = src.dwLowDateTime;
	dst.dwHighDateTime = src.dwHighDateTime;
	dst.destAction = src.destAction;
	dst.destParam1 = src.destParam1;
	dst.destParam2 = src.destParam2;
	dst.plrlevel = src.plrlevel;
	dst.px = src.px;
	dst.py = src.py;
	dst.targx = src.targx;
	dst.targy = src.targy;
	std::memcpy(dst.pName, src.pName, sizeof(dst.pName));
	dst.pClass = src.pClass;
	dst.pBaseStr = src.pBaseStr;
	dst.pBaseMag = src.pBaseMag;
	dst.pBaseDex = src.pBaseDex;
	dst.pBaseVit = src.pBaseVit;
	dst.pLevel = src.pLevel;
	dst.pStatPts = src.pStatPts;
	dst.pExperience = src.pExperience;
	dst.pGold = src.pGold;
	dst.pHPBase = src.pHPBase;
	dst.pMaxHPBase = src.pMaxHPBase;
	dst.pManaBase = src.pManaBase;
	dst.pMaxManaBase = src.pMaxManaBase;
	std::memcpy(dst.pSplLvl, src.pSplLvl, 37);
	dst.pMemSpells = src.pMemSpells;
	for (int i = 0; i < 7; ++i)
		CopyRealmToItemPack(src.InvBody[i], dst.InvBody[i]);
	for (int i = 0; i < 40; ++i)
		CopyRealmToItemPack(src.InvList[i], dst.InvList[i]);
	dst._pNumInv = src._pNumInv;
	std::memcpy(dst.InvGrid, src.InvGrid, 40);
	for (int i = 0; i < 8; ++i)
		CopyRealmToItemPack(src.SpdList[i], dst.SpdList[i]);
	dst.pTownWarps = src.pTownWarps;
	dst.pDungMsgs = src.pDungMsgs;
	dst.pLvlLoad = src.pLvlLoad;
	dst.pBattleNet = src.pBattleNet;
	dst.pManaShield = src.pManaShield;
	dst.pDungMsgs2 = src.pDungMsgs2;
	dst.bIsHellfire = gbIsHellfire ? 1 : 0;
	dst.wReflections = src.wReflections;
	std::memcpy(&dst.pSplLvl[37], src.pSplLvl2, 10);
	dst.pDiabloKillLevel = src.pDiabloKillLevel;
	dst.pDifficulty = src.pDifficulty;
	dst.pDamAcFlags = static_cast<decltype(dst.pDamAcFlags)>(src.pDamAcFlags);
}

RealmClient &GetRealmClient()
{
	static RealmClient instance;
	return instance;
}

RealmClient::RealmClient()
{
	slotCharIds_.fill(0);
	std::memcpy(trafficKey_, DefaultHandshakeKey.data(), 32);
}

RealmClient::~RealmClient()
{
	Disconnect();
}

void RealmClient::SetServerEndpoint(std::string host, uint16_t port)
{
	std::lock_guard<std::mutex> lock(mutex_);
	uint16_t newPort = port != 0 ? port : realm::DefaultRealmPort;
	const auto colonPos = host.find(':');
	if (colonPos != std::string::npos) {
		const std::string portPart = host.substr(colonPos + 1);
		const int parsedPort = std::atoi(portPart.c_str());
		if (parsedPort > 0 && parsedPort <= 65535)
			newPort = static_cast<uint16_t>(parsedPort);
		host = host.substr(0, colonPos);
	}
	if (serverHost_ != host || serverPort_ != newPort) {
		serverHost_ = std::move(host);
		serverPort_ = newPort;
		if (connected_) {
			CloseRawSocketHandle(sock_);
			connected_ = false;
			authenticated_ = false;
		}
	}
}

void RealmClient::Disconnect()
{
	std::lock_guard<std::mutex> lock(mutex_);
	CloseRawSocketHandle(sock_);
	connected_ = false;
	authenticated_ = false;
	hasResumeToken_ = false;
	activeCharId_ = 0;
	activeGameId_ = 0;
	currentServerFloor_ = 0;
	floorItemUids_.clear();
	nextSeq_ = 1;
	std::memcpy(trafficKey_, DefaultHandshakeKey.data(), 32);
}

bool RealmClient::OpenAndAttestSocket(std::string &outError)
{
	CloseRawSocketHandle(sock_);
	connected_ = false;
	authenticated_ = false;
	nextSeq_ = 1;
	std::memcpy(trafficKey_, DefaultHandshakeKey.data(), 32);

	if (sodium_init() < 0) {
		outError = "Failed to initialize libsodium.";
		return false;
	}

	if (sgOptions.Network.szRealmHost[0] != '\0') {
		std::string optHost = sgOptions.Network.szRealmHost;
		uint16_t optPort = *sgOptions.Network.realmPort != 0 ? *sgOptions.Network.realmPort : realm::DefaultRealmPort;
		const auto colonPos = optHost.find(':');
		if (colonPos != std::string::npos) {
			const int parsedPort = std::atoi(optHost.substr(colonPos + 1).c_str());
			if (parsedPort > 0 && parsedPort <= 65535)
				optPort = static_cast<uint16_t>(parsedPort);
			optHost = optHost.substr(0, colonPos);
		}
		serverHost_ = std::move(optHost);
		serverPort_ = optPort;
	}

#ifdef _WIN32
	static bool wsaStarted = false;
	if (!wsaStarted) {
		WSADATA wsa {};
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			outError = "WSAStartup failed.";
			return false;
		}
		wsaStarted = true;
	}
#endif

	addrinfo hints {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	const std::string portStr = std::to_string(serverPort_);
	addrinfo *res = nullptr;
	if (::getaddrinfo(serverHost_.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr) {
		outError = fmt::format("Cannot resolve Realm host {}:{}", serverHost_, serverPort_);
		return false;
	}

#ifdef _WIN32
	SOCKET s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (s == INVALID_SOCKET) {
		::freeaddrinfo(res);
		outError = "Failed to create TCP socket.";
		return false;
	}
	DWORD timeoutMs = 8000;
	::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
	::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
	BOOL noDelay = TRUE;
	::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));
	if (::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
		::closesocket(s);
		::freeaddrinfo(res);
		outError = fmt::format("Could not connect to Realm server at {}:{}", serverHost_, serverPort_);
		return false;
	}
	sock_ = static_cast<uintptr_t>(s);
#else
	int s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (s < 0) {
		::freeaddrinfo(res);
		outError = "Failed to create TCP socket.";
		return false;
	}
	if (::connect(s, res->ai_addr, res->ai_addrlen) != 0) {
		::close(s);
		::freeaddrinfo(res);
		outError = fmt::format("Could not connect to Realm server at {}:{}", serverHost_, serverPort_);
		return false;
	}
	sock_ = static_cast<uintptr_t>(s);
#endif
	::freeaddrinfo(res);
	connected_ = true;

	// Step 1: Request 32-byte challenge nonce (0-byte payload)
	realm::RealmPacketHeader respHdr {};
	std::vector<uint8_t> respPayload;
	if (!SendAndReceive(realm::RealmOpcode::OP_AUTH_CHALLENGE_REQ, nullptr, 0, respHdr, respPayload, outError)) {
		CloseRawSocketHandle(sock_);
		connected_ = false;
		return false;
	}
	if (static_cast<realm::RealmOpcode>(respHdr.opcode) != realm::RealmOpcode::OP_AUTH_CHALLENGE_RESP
	    || respPayload.size() < sizeof(realm::AuthChallengeResponsePayload)) {
		outError = "Invalid challenge response from Realm server.";
		CloseRawSocketHandle(sock_);
		connected_ = false;
		return false;
	}
	realm::AuthChallengeResponsePayload chResp {};
	std::memcpy(&chResp, respPayload.data(), sizeof(chResp));
	std::memcpy(serverNonce_, chResp.serverNonce, 32);

	// Step 2: Official Binary Attestation Handshake
	realm::AttestHandshakeRequestPayload attReq {};
	randombytes_buf(attReq.clientNonce, sizeof(attReq.clientNonce));
	ComputeRunningExecutableTextSectionHash(attReq.clientTextSectionHash);
	attReq.clientBuildVersion = realm::ClientBuildVersion;
	ComputeClientAttestationProof(
	    serverNonce_,
	    attReq.clientNonce,
	    attReq.clientTextSectionHash,
	    attReq.clientBuildVersion,
	    attReq.attestationHmac);

	if (!SendAndReceive(realm::RealmOpcode::OP_ATTEST_HANDSHAKE_REQ, &attReq, sizeof(attReq), respHdr, respPayload, outError)) {
		CloseRawSocketHandle(sock_);
		connected_ = false;
		return false;
	}
	if (static_cast<realm::RealmOpcode>(respHdr.opcode) != realm::RealmOpcode::OP_ATTEST_HANDSHAKE_RESP
	    || respPayload.size() < sizeof(realm::AttestHandshakeResponsePayload)) {
		outError = "Binary attestation handshake failed.";
		CloseRawSocketHandle(sock_);
		connected_ = false;
		return false;
	}
	realm::AttestHandshakeResponsePayload attResp {};
	std::memcpy(&attResp, respPayload.data(), sizeof(attResp));
	if (static_cast<realm::RealmStatus>(attResp.statusCode) != realm::RealmStatus::OK) {
		outError = fmt::format("Attestation rejected: {}", realm::StatusToString(static_cast<realm::RealmStatus>(attResp.statusCode)));
		CloseRawSocketHandle(sock_);
		connected_ = false;
		return false;
	}

	// Rotate session traffic MAC key immediately after receiving OP_ATTEST_HANDSHAKE_RESP
	DeriveSessionTrafficKey(serverNonce_, attReq.clientNonce, trafficKey_);
	return true;
}

bool RealmClient::EnsureAuthenticatedSession(std::string &outError)
{
	if (connected_ && authenticated_ && sock_ != ~uintptr_t(0))
		return true;

	if (!hasResumeToken_ || accountId_ == 0) {
		outError = "Not logged in to Closed Realm.";
		return false;
	}

	if (!OpenAndAttestSocket(outError))
		return false;

	realm::AuthResumeRequestPayload req {};
	req.accountId = accountId_;
	std::memcpy(req.sessionToken, sessionToken_, sizeof(req.sessionToken));

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	if (!SendAndReceive(realm::RealmOpcode::OP_AUTH_RESUME_REQ, &req, sizeof(req), hdr, payload, outError))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_AUTH_RESULT
	    || payload.size() < sizeof(realm::AuthResultPayload)) {
		outError = "Session resume failed.";
		return false;
	}

	realm::AuthResultPayload res {};
	std::memcpy(&res, payload.data(), sizeof(res));
	if (static_cast<realm::RealmStatus>(res.statusCode) != realm::RealmStatus::OK) {
		outError = res.message[0] != '\0' ? res.message : realm::StatusToString(static_cast<realm::RealmStatus>(res.statusCode));
		return false;
	}

	authenticated_ = true;
	return true;
}

bool RealmClient::SendAndReceive(
    realm::RealmOpcode opcode,
    const void *reqPayload,
    uint32_t reqLen,
    realm::RealmPacketHeader &outHdr,
    std::vector<uint8_t> &outPayload,
    std::string &outError)
{
	realm::RealmPacketHeader hdr {};
	hdr.magic = realm::ProtocolMagic;
	hdr.version = realm::ProtocolVersion;
	hdr.opcode = static_cast<uint16_t>(opcode);
	hdr.payloadLength = reqLen;
	hdr.sequenceNumber = nextSeq_++;
	std::memset(hdr.mac, 0, sizeof(hdr.mac));

	const auto *payloadBytes = static_cast<const uint8_t *>(reqPayload);
	ComputePacketMac(hdr, payloadBytes, reqLen, trafficKey_, hdr.mac);

	if (!SendAllBytes(sock_, reinterpret_cast<const uint8_t *>(&hdr), sizeof(hdr))
	    || (reqLen > 0 && !SendAllBytes(sock_, payloadBytes, reqLen))) {
		outError = "Connection lost while sending packet to Realm server.";
		CloseRawSocketHandle(sock_);
		connected_ = false;
		authenticated_ = false;
		return false;
	}

	if (!RecvAllBytes(sock_, reinterpret_cast<uint8_t *>(&outHdr), sizeof(outHdr))) {
		outError = "Connection closed by Realm server.";
		CloseRawSocketHandle(sock_);
		connected_ = false;
		authenticated_ = false;
		return false;
	}

	if (outHdr.magic != realm::ProtocolMagic || outHdr.version != realm::ProtocolVersion || outHdr.payloadLength > realm::MaxPayloadSize) {
		outError = "Malformed packet header received from Realm server.";
		CloseRawSocketHandle(sock_);
		connected_ = false;
		authenticated_ = false;
		return false;
	}

	outPayload.resize(outHdr.payloadLength);
	if (outHdr.payloadLength > 0 && !RecvAllBytes(sock_, outPayload.data(), outHdr.payloadLength)) {
		outError = "Connection lost while reading Realm payload.";
		CloseRawSocketHandle(sock_);
		connected_ = false;
		authenticated_ = false;
		return false;
	}

	uint8_t expectedMac[realm::PacketMacSize] {};
	ComputePacketMac(outHdr, outPayload.data(), outPayload.size(), trafficKey_, expectedMac);
	if (sodium_memcmp(expectedMac, outHdr.mac, realm::PacketMacSize) != 0) {
		outError = "Realm packet MAC verification failed.";
		CloseRawSocketHandle(sock_);
		connected_ = false;
		authenticated_ = false;
		return false;
	}

	if (static_cast<realm::RealmOpcode>(outHdr.opcode) == realm::RealmOpcode::OP_ERROR_RESPONSE
	    && outPayload.size() >= sizeof(realm::ErrorResponsePayload)) {
		realm::ErrorResponsePayload errPayload {};
		std::memcpy(&errPayload, outPayload.data(), sizeof(errPayload));
		errPayload.message[sizeof(errPayload.message) - 1] = '\0';
		outError = errPayload.message[0] != '\0'
		    ? errPayload.message
		    : realm::StatusToString(static_cast<realm::RealmStatus>(errPayload.statusCode));
		return false;
	}

	return true;
}

bool RealmClient::RegisterAccount(const std::string &username, const std::string &password, std::string &outError)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!OpenAndAttestSocket(outError))
		return false;

	realm::AuthCredentialsRequestPayload req {};
	std::snprintf(req.username, sizeof(req.username), "%s", username.c_str());
	std::snprintf(req.password, sizeof(req.password), "%s", password.c_str());

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	const bool sentOk = SendAndReceive(realm::RealmOpcode::OP_AUTH_REGISTER_REQ, &req, sizeof(req), hdr, payload, outError);
	sodium_memzero(&req, sizeof(req));
	if (!sentOk)
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_AUTH_RESULT
	    || payload.size() < sizeof(realm::AuthResultPayload)) {
		outError = "Unexpected registration response from server.";
		return false;
	}

	realm::AuthResultPayload resp {};
	std::memcpy(&resp, payload.data(), sizeof(resp));
	resp.message[sizeof(resp.message) - 1] = '\0';
	if (static_cast<realm::RealmStatus>(resp.statusCode) != realm::RealmStatus::OK) {
		outError = resp.message[0] != '\0'
		    ? resp.message
		    : realm::StatusToString(static_cast<realm::RealmStatus>(resp.statusCode));
		return false;
	}

	accountId_ = resp.accountId;
	std::memcpy(sessionToken_, resp.sessionToken, sizeof(sessionToken_));
	username_ = username;
	hasResumeToken_ = true;
	authenticated_ = true;
	return true;
}

bool RealmClient::LoginAccount(const std::string &username, const std::string &password, std::string &outError)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!OpenAndAttestSocket(outError))
		return false;

	realm::AuthCredentialsRequestPayload req {};
	std::snprintf(req.username, sizeof(req.username), "%s", username.c_str());
	std::snprintf(req.password, sizeof(req.password), "%s", password.c_str());

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	const bool sentOk = SendAndReceive(realm::RealmOpcode::OP_AUTH_LOGIN_REQ, &req, sizeof(req), hdr, payload, outError);
	sodium_memzero(&req, sizeof(req));
	if (!sentOk)
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_AUTH_RESULT
	    || payload.size() < sizeof(realm::AuthResultPayload)) {
		outError = "Unexpected login response from server.";
		return false;
	}

	realm::AuthResultPayload resp {};
	std::memcpy(&resp, payload.data(), sizeof(resp));
	resp.message[sizeof(resp.message) - 1] = '\0';
	if (static_cast<realm::RealmStatus>(resp.statusCode) != realm::RealmStatus::OK) {
		outError = resp.message[0] != '\0'
		    ? resp.message
		    : realm::StatusToString(static_cast<realm::RealmStatus>(resp.statusCode));
		return false;
	}

	accountId_ = resp.accountId;
	std::memcpy(sessionToken_, resp.sessionToken, sizeof(sessionToken_));
	username_ = username;
	hasResumeToken_ = true;
	authenticated_ = true;
	return true;
}

bool RealmClient::ListHeroes(std::vector<realm::HeroSummaryInfo> &outHeroes, std::string &outError)
{
	std::lock_guard<std::mutex> lock(mutex_);
	outHeroes.clear();
	if (!EnsureAuthenticatedSession(outError))
		return false;

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	if (!SendAndReceive(realm::RealmOpcode::OP_HERO_LIST_REQ, nullptr, 0, hdr, payload, outError))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_HERO_LIST_RESP
	    || payload.size() < sizeof(realm::HeroListResponsePayload)) {
		outError = "Invalid hero list response.";
		return false;
	}

	realm::HeroListResponsePayload resp {};
	std::memcpy(&resp, payload.data(), sizeof(resp));
	if (static_cast<realm::RealmStatus>(resp.statusCode) != realm::RealmStatus::OK) {
		outError = realm::StatusToString(static_cast<realm::RealmStatus>(resp.statusCode));
		return false;
	}

	const std::size_t count = std::min<std::size_t>(resp.heroCount, realm::MaxHeroesPerAccount);
	for (std::size_t i = 0; i < count; ++i)
		outHeroes.push_back(resp.heroes[i]);
	return true;
}

bool RealmClient::CreateHero(
    const std::string &heroName,
    uint8_t heroClass,
    realm::HeroSummaryInfo &outSummary,
    PlayerPack &outPack,
    std::string &outError)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!EnsureAuthenticatedSession(outError))
		return false;

	realm::HeroCreateRequestPayload req {};
	std::snprintf(req.name, sizeof(req.name), "%s", heroName.c_str());
	req.heroClass = heroClass;
	req.bIsHellfire = 1;
	req.reserved = 0;

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	if (!SendAndReceive(realm::RealmOpcode::OP_HERO_CREATE_REQ, &req, sizeof(req), hdr, payload, outError))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_HERO_CREATE_RESP
	    || payload.size() < sizeof(realm::HeroCreateResponsePayload)) {
		outError = "Invalid hero create response.";
		return false;
	}

	realm::HeroCreateResponsePayload resp {};
	std::memcpy(&resp, payload.data(), sizeof(resp));
	if (static_cast<realm::RealmStatus>(resp.statusCode) != realm::RealmStatus::OK) {
		outError = realm::StatusToString(static_cast<realm::RealmStatus>(resp.statusCode));
		return false;
	}

	outSummary = resp.summary;
	ConvertRealmPackToClientPack(resp.pack, outPack);
	activeCharId_ = resp.summary.charId;
	return true;
}

bool RealmClient::DeleteHero(uint64_t charId, std::string &outError)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!EnsureAuthenticatedSession(outError))
		return false;

	realm::HeroDeleteRequestPayload req {};
	req.charId = charId;

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	if (!SendAndReceive(realm::RealmOpcode::OP_HERO_DELETE_REQ, &req, sizeof(req), hdr, payload, outError))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_HERO_DELETE_RESP
	    || payload.size() < sizeof(realm::HeroDeleteResponsePayload)) {
		outError = "Invalid hero delete response.";
		return false;
	}

	realm::HeroDeleteResponsePayload resp {};
	std::memcpy(&resp, payload.data(), sizeof(resp));
	if (static_cast<realm::RealmStatus>(resp.statusCode) != realm::RealmStatus::OK) {
		outError = realm::StatusToString(static_cast<realm::RealmStatus>(resp.statusCode));
		return false;
	}
	return true;
}

bool RealmClient::LoadHero(uint64_t charId, PlayerPack &outPack, std::string &outError)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!EnsureAuthenticatedSession(outError))
		return false;

	realm::HeroLoadRequestPayload req {};
	req.charId = charId;

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	if (!SendAndReceive(realm::RealmOpcode::OP_HERO_LOAD_REQ, &req, sizeof(req), hdr, payload, outError))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_HERO_LOAD_RESP
	    || payload.size() < sizeof(realm::HeroLoadResponsePayload)) {
		outError = "Invalid hero load response.";
		return false;
	}

	realm::HeroLoadResponsePayload resp {};
	std::memcpy(&resp, payload.data(), sizeof(resp));
	if (static_cast<realm::RealmStatus>(resp.statusCode) != realm::RealmStatus::OK) {
		outError = realm::StatusToString(static_cast<realm::RealmStatus>(resp.statusCode));
		return false;
	}

	activeCharId_ = resp.charId;
	ConvertRealmPackToClientPack(resp.pack, outPack);
	return true;
}

bool RealmClient::SaveHero(uint64_t charId, const PlayerPack &pack, std::string &outError)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!EnsureAuthenticatedSession(outError))
		return false;

	realm::HeroSaveRequestPayload req {};
	req.charId = charId;
	ConvertClientPackToRealmPack(pack, req.pack);

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	if (!SendAndReceive(realm::RealmOpcode::OP_HERO_SAVE_REQ, &req, sizeof(req), hdr, payload, outError))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_HERO_SAVE_RESP
	    || payload.size() < sizeof(realm::HeroSaveResponsePayload)) {
		outError = "Invalid hero save response.";
		return false;
	}

	realm::HeroSaveResponsePayload resp {};
	std::memcpy(&resp, payload.data(), sizeof(resp));
	if (static_cast<realm::RealmStatus>(resp.statusCode) != realm::RealmStatus::OK) {
		outError = realm::StatusToString(static_cast<realm::RealmStatus>(resp.statusCode));
		Log("Closed Realm SaveHero status={}: {}", resp.statusCode, outError);
		return false;
	}
	return true;
}

bool RealmClient::LoadStashFromRealm()
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::string err;
	if (!EnsureAuthenticatedSession(err))
		return false;

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	if (!SendAndReceive(realm::RealmOpcode::OP_STASH_LOAD_REQ, nullptr, 0, hdr, payload, err))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_STASH_LOAD_RESP
	    || payload.size() < sizeof(realm::StashDataHeader))
		return false;

	Stash = StashStruct {};
	realm::StashDataHeader respHdr {};
	std::memcpy(&respHdr, payload.data(), sizeof(respHdr));
	if (static_cast<realm::RealmStatus>(respHdr.statusCode) != realm::RealmStatus::OK)
		return false;

	Stash.gold = respHdr.stashGold;
	Stash.SetPage(0);

	std::size_t offset = sizeof(realm::StashDataHeader);
	for (uint16_t i = 0; i < respHdr.itemCount; ++i) {
		if (offset + sizeof(realm::PkItemStruct) > payload.size())
			break;
		realm::PkItemStruct wireItem {};
		std::memcpy(&wireItem, payload.data() + offset, sizeof(wireItem));
		offset += sizeof(wireItem);

		ItemPack pk {};
		CopyRealmToItemPack(wireItem, pk);
		Item decoded {};
		UnPackItem(pk, MyPlayer != nullptr ? *MyPlayer : Players[0], decoded, (pk.dwBuff & CF_HELLFIRE) != 0);
		Stash.stashList.push_back(decoded);

		const unsigned page = i / 100U;
		const uint8_t slot = static_cast<uint8_t>(i % 100U);
		const uint8_t gx = slot % 10U;
		const uint8_t gy = slot / 10U;
		Stash.stashGrids[page][gx][gy] = static_cast<uint16_t>(Stash.stashList.size());
	}
	Stash.dirty = false;
	return true;
}

bool RealmClient::SaveStashToRealm()
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::string err;
	if (!EnsureAuthenticatedSession(err))
		return false;

	std::vector<realm::PkItemStruct> wireItems;
	for (std::size_t idx = 0; idx < Stash.stashList.size() && wireItems.size() < realm::MaxStashItems; ++idx) {
		if (Stash.stashList[idx].isEmpty())
			continue;
		ItemPack pk {};
		PackItem(pk, Stash.stashList[idx], gbIsHellfire);
		realm::PkItemStruct wire {};
		CopyItemPackToRealm(pk, wire);
		wireItems.push_back(wire);
	}

	realm::StashDataHeader reqHdr {};
	reqHdr.statusCode = 0;
	reqHdr.itemCount = static_cast<uint16_t>(wireItems.size());
	reqHdr.stashGold = Stash.gold;

	std::vector<uint8_t> reqBuf(sizeof(reqHdr) + wireItems.size() * sizeof(realm::PkItemStruct));
	std::memcpy(reqBuf.data(), &reqHdr, sizeof(reqHdr));
	if (!wireItems.empty())
		std::memcpy(reqBuf.data() + sizeof(reqHdr), wireItems.data(), wireItems.size() * sizeof(realm::PkItemStruct));

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	if (!SendAndReceive(realm::RealmOpcode::OP_STASH_SAVE_REQ, reqBuf.data(), static_cast<uint32_t>(reqBuf.size()), hdr, payload, err))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_STASH_SAVE_RESP
	    || payload.size() < sizeof(realm::StashSaveResponsePayload))
		return false;

	realm::StashSaveResponsePayload resp {};
	std::memcpy(&resp, payload.data(), sizeof(resp));
	return static_cast<realm::RealmStatus>(resp.statusCode) == realm::RealmStatus::OK;
}

bool RealmClient::ListGames(std::vector<realm::GameLobbyInfo> &outGames, std::string &outError)
{
	std::lock_guard<std::mutex> lock(mutex_);
	outGames.clear();
	if (!EnsureAuthenticatedSession(outError))
		return false;

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	if (!SendAndReceive(realm::RealmOpcode::OP_GAME_LIST_REQ, nullptr, 0, hdr, payload, outError))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_GAME_LIST_RESP
	    || payload.size() < sizeof(realm::GameListResponsePayload)) {
		outError = "Invalid game list response.";
		return false;
	}

	realm::GameListResponsePayload resp {};
	std::memcpy(&resp, payload.data(), sizeof(resp));
	if (static_cast<realm::RealmStatus>(resp.statusCode) != realm::RealmStatus::OK) {
		outError = realm::StatusToString(static_cast<realm::RealmStatus>(resp.statusCode));
		return false;
	}

	const std::size_t count = std::min<std::size_t>(resp.gameCount, realm::MaxListedGames);
	for (std::size_t i = 0; i < count; ++i)
		outGames.push_back(resp.games[i]);
	return true;
}

bool RealmClient::CreateGame(
    const std::string &gameName,
    const std::string &gamePassword,
    uint8_t difficulty,
    uint8_t tickRate,
    realm::GameCreateOrJoinResponsePayload &outResp,
    std::string &outError)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!EnsureAuthenticatedSession(outError))
		return false;

	realm::GameCreateRequestPayload req {};
	req.charId = activeCharId_;
	std::snprintf(req.gameName, sizeof(req.gameName), "%s", gameName.c_str());
	std::snprintf(req.password, sizeof(req.password), "%s", gamePassword.c_str());
	req.difficulty = difficulty;
	req.maxPlayers = 4;
	req.bIsHellfire = 1;
	req.tickRate = (tickRate >= realm::MinClosedRealmTickRate && tickRate <= realm::MaxClosedRealmTickRate)
	    ? tickRate
	    : realm::DefaultClosedRealmTickRate;

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	if (!SendAndReceive(realm::RealmOpcode::OP_GAME_CREATE_REQ, &req, sizeof(req), hdr, payload, outError))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_GAME_CREATE_RESP
	    || payload.size() < sizeof(realm::GameCreateOrJoinResponsePayload)) {
		outError = "Invalid game create response.";
		return false;
	}

	std::memcpy(&outResp, payload.data(), sizeof(outResp));
	if (static_cast<realm::RealmStatus>(outResp.statusCode) != realm::RealmStatus::OK) {
		outError = realm::StatusToString(static_cast<realm::RealmStatus>(outResp.statusCode));
		return false;
	}

	activeGameId_ = outResp.gameId;
	currentServerFloor_ = 0;
	floorItemUids_.clear();
	return true;
}

bool RealmClient::JoinGame(
    const std::string &gameName,
    const std::string &gamePassword,
    realm::GameCreateOrJoinResponsePayload &outResp,
    std::string &outError)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!EnsureAuthenticatedSession(outError))
		return false;

	realm::GameJoinRequestPayload req {};
	req.charId = activeCharId_;
	std::snprintf(req.gameName, sizeof(req.gameName), "%s", gameName.c_str());
	std::snprintf(req.password, sizeof(req.password), "%s", gamePassword.c_str());

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	if (!SendAndReceive(realm::RealmOpcode::OP_GAME_JOIN_REQ, &req, sizeof(req), hdr, payload, outError))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_GAME_JOIN_RESP
	    || payload.size() < sizeof(realm::GameCreateOrJoinResponsePayload)) {
		outError = "Invalid game join response.";
		return false;
	}

	std::memcpy(&outResp, payload.data(), sizeof(outResp));
	if (static_cast<realm::RealmStatus>(outResp.statusCode) != realm::RealmStatus::OK) {
		outError = realm::StatusToString(static_cast<realm::RealmStatus>(outResp.statusCode));
		return false;
	}

	activeGameId_ = outResp.gameId;
	currentServerFloor_ = 0;
	floorItemUids_.clear();
	return true;
}

void RealmClient::NotifyFloorTransition(uint8_t targetFloor, Point pos)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!connected_ || activeGameId_ == 0 || activeCharId_ == 0)
		return;

	realm::FloorTransitionRequestPayload req {};
	req.gameId = activeGameId_;
	req.fromLevel = currentServerFloor_;
	req.toLevel = targetFloor;
	req.tileX = static_cast<uint8_t>(std::clamp<int>(pos.x, 0, MAXDUNX - 1));
	req.tileY = static_cast<uint8_t>(std::clamp<int>(pos.y, 0, MAXDUNY - 1));

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	std::string err;
	if (SendAndReceive(realm::RealmOpcode::OP_FLOOR_TRANSITION_REQ, &req, sizeof(req), hdr, payload, err)) {
		if (static_cast<realm::RealmOpcode>(hdr.opcode) == realm::RealmOpcode::OP_FLOOR_TRANSITION_RESP
		    && payload.size() >= sizeof(realm::FloorTransitionResponsePayload)) {
			realm::FloorTransitionResponsePayload resp {};
			std::memcpy(&resp, payload.data(), sizeof(resp));
			if (static_cast<realm::RealmStatus>(resp.statusCode) == realm::RealmStatus::OK)
				currentServerFloor_ = resp.currentLevel;
		}
	}
}

void RealmClient::RecordGroundItemUid(const realm::PkItemStruct &packed, uint64_t itemUid)
{
	if (itemUid == 0)
		return;
	floorItemUids_[{ packed.iSeed, packed.idx, packed.iCreateInfo }] = itemUid;
}

uint64_t RealmClient::LookupGroundItemUid(uint32_t iSeed, uint16_t idx, uint16_t iCreateInfo) const
{
	const auto it = floorItemUids_.find({ iSeed, idx, iCreateInfo });
	return it != floorItemUids_.end() ? it->second : 0ULL;
}

bool RealmClient::HandleMonsterKill(const Monster &monster, bool /*sendmsg*/)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!connected_ || activeGameId_ == 0 || activeCharId_ == 0)
		return false;

	const int32_t monsterHp = std::max<int32_t>(monster.maxHitPoints >> 6, 1);
	realm::MonsterDropRequestPayload req {};
	req.gameId = activeGameId_;
	req.currlevel = currlevel;
	req.tileX = static_cast<uint8_t>(std::clamp<int>(monster.position.tile.x, 0, MAXDUNX - 1));
	req.tileY = static_cast<uint8_t>(std::clamp<int>(monster.position.tile.y, 0, MAXDUNY - 1));
	req.isUniqueMonster = monster.isUnique() ? 1 : 0;
	req.monsterIndex = static_cast<uint16_t>(monster.getId());
	req.monsterTypeOrUid = monster.isUnique()
	    ? static_cast<int16_t>(monster.uniqueType)
	    : static_cast<int16_t>(monster.type().type);
	req.totalDamageDealt = monsterHp;
	req.maxSingleHitDealt = std::clamp<int32_t>(MyPlayer != nullptr ? MyPlayer->_pIMaxDam : 10, 1, 25);
	req.combatDurationMs = static_cast<uint32_t>(std::max<int32_t>(monsterHp, 10) * 200);

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	std::string err;
	if (!SendAndReceive(realm::RealmOpcode::OP_MONSTER_DROP_REQ, &req, sizeof(req), hdr, payload, err))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_MONSTER_DROP_RESP
	    || payload.size() < sizeof(realm::MonsterOrChestDropResponsePayload))
		return false;

	realm::MonsterOrChestDropResponsePayload resp {};
	std::memcpy(&resp, payload.data(), sizeof(resp));
	if (static_cast<realm::RealmStatus>(resp.statusCode) == realm::RealmStatus::OK) {
		const uint8_t count = std::min<uint8_t>(resp.dropCount, realm::MaxDropsPerKill);
		for (uint8_t i = 0; i < count; ++i) {
			RecordGroundItemUid(resp.drops[i].packed, resp.drops[i].itemUid);
			UnpackDropAndSpawnFloorItem(resp.drops[i].dropCmd, monster.position.tile);
		}
	}
	return true;
}

bool RealmClient::HandleChestOpen(const Object &chest, bool /*sendLootMsg*/)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!connected_ || activeGameId_ == 0 || activeCharId_ == 0)
		return false;

	realm::ChestDropRequestPayload req {};
	req.gameId = activeGameId_;
	req.currlevel = currlevel;
	req.tileX = static_cast<uint8_t>(std::clamp<int>(chest.position.x, 0, MAXDUNX - 1));
	req.tileY = static_cast<uint8_t>(std::clamp<int>(chest.position.y, 0, MAXDUNY - 1));
	req.chestQuality = (chest._oVar2 >= 0 && chest._oVar2 <= 2) ? static_cast<uint8_t>(chest._oVar2) : 0;
	req.objectIndex = static_cast<uint16_t>(&chest - &Objects[0]);
	req.reserved = 0;

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	std::string err;
	if (!SendAndReceive(realm::RealmOpcode::OP_CHEST_DROP_REQ, &req, sizeof(req), hdr, payload, err))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != realm::RealmOpcode::OP_CHEST_DROP_RESP
	    || payload.size() < sizeof(realm::MonsterOrChestDropResponsePayload))
		return false;

	realm::MonsterOrChestDropResponsePayload resp {};
	std::memcpy(&resp, payload.data(), sizeof(resp));
	if (static_cast<realm::RealmStatus>(resp.statusCode) == realm::RealmStatus::OK) {
		const uint8_t count = std::min<uint8_t>(resp.dropCount, realm::MaxDropsPerKill);
		for (uint8_t i = 0; i < count; ++i) {
			RecordGroundItemUid(resp.drops[i].packed, resp.drops[i].itemUid);
			UnpackDropAndSpawnFloorItem(resp.drops[i].dropCmd, chest.position);
		}
	}
	return true;
}

bool RealmClient::NotifyItemPickup(const Item &item)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!connected_ || activeGameId_ == 0 || activeCharId_ == 0)
		return true;

	const uint32_t seed = static_cast<uint32_t>(item._iSeed);
	const uint16_t idx = static_cast<uint16_t>(item.IDidx);
	const uint16_t ci = item._iCreateInfo;

	realm::ItemPickupRequestPayload req {};
	req.gameId = activeGameId_;
	req.itemUid = LookupGroundItemUid(seed, idx, ci);
	req.currlevel = currlevel;
	req.tileX = static_cast<uint8_t>(std::clamp<int>(item.position.x, 0, MAXDUNX - 1));
	req.tileY = static_cast<uint8_t>(std::clamp<int>(item.position.y, 0, MAXDUNY - 1));
	req.reserved = 0;
	req.iSeed = seed;
	req.idx = idx;
	req.iCreateInfo = ci;

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	std::string err;
	SendAndReceive(realm::RealmOpcode::OP_ITEM_PICKUP_REQ, &req, sizeof(req), hdr, payload, err);
	return true;
}

bool RealmClient::NotifyItemDrop(const Item &item, Point pos)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!connected_ || activeGameId_ == 0 || activeCharId_ == 0)
		return true;

	ItemPack pk {};
	PackItem(pk, item, gbIsHellfire);

	realm::ItemDropRequestPayload req {};
	req.gameId = activeGameId_;
	req.currlevel = currlevel;
	req.tileX = static_cast<uint8_t>(std::clamp<int>(pos.x, 0, MAXDUNX - 1));
	req.tileY = static_cast<uint8_t>(std::clamp<int>(pos.y, 0, MAXDUNY - 1));
	req.reserved = 0;
	CopyItemPackToRealm(pk, req.item);

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	std::string err;
	if (SendAndReceive(realm::RealmOpcode::OP_ITEM_DROP_REQ, &req, sizeof(req), hdr, payload, err)) {
		if (static_cast<realm::RealmOpcode>(hdr.opcode) == realm::RealmOpcode::OP_ITEM_DROP_RESP
		    && payload.size() >= sizeof(realm::ItemDropResponsePayload)) {
			realm::ItemDropResponsePayload resp {};
			std::memcpy(&resp, payload.data(), sizeof(resp));
			if (static_cast<realm::RealmStatus>(resp.statusCode) == realm::RealmStatus::OK)
				RecordGroundItemUid(resp.dropped.packed, resp.dropped.itemUid);
		}
	}
	return true;
}

void RealmClient::SetSlotCharId(uint32_t saveNum, uint64_t charId)
{
	if (saveNum < slotCharIds_.size())
		slotCharIds_[saveNum] = charId;
}

uint64_t RealmClient::GetSlotCharId(uint32_t saveNum) const
{
	return saveNum < slotCharIds_.size() ? slotCharIds_[saveNum] : 0;
}

void RealmClient::ClearSlotCharIds()
{
	slotCharIds_.fill(0);
}

void RealmClient::WriteLocalCharacterBackup(const PlayerPack &pack)
{
	if (paths::PrefPath().empty())
		return;
	char heroShortName[16] {};
	std::snprintf(heroShortName, sizeof(heroShortName), "%s", pack.pName);
	for (char &c : heroShortName) {
		if (c == ' ' || c == '/' || c == '\\' || c == ':')
			c = '_';
	}
	const std::string path = StrCat(paths::PrefPath(), "realm_", heroShortName, "_", activeCharId_, ".dxe_bak");
	FILE *fp = std::fopen(path.c_str(), "wb");
	if (fp != nullptr) {
		std::fwrite(&pack, sizeof(PlayerPack), 1, fp);
		std::fclose(fp);
	}
}

bool RealmClient::SendRawRpc(
    realm::RealmOpcode reqOpcode,
    const void *reqPayload,
    uint32_t reqLen,
    realm::RealmOpcode expectedRespOpcode,
    void *outResp,
    uint32_t outRespLen)
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::string err;
	if (!EnsureAuthenticatedSession(err))
		return false;

	realm::RealmPacketHeader hdr {};
	std::vector<uint8_t> payload;
	if (!SendAndReceive(reqOpcode, reqPayload, reqLen, hdr, payload, err))
		return false;

	if (static_cast<realm::RealmOpcode>(hdr.opcode) != expectedRespOpcode || payload.size() < outRespLen)
		return false;

	if (outResp != nullptr && outRespLen > 0)
		std::memcpy(outResp, payload.data(), outRespLen);
	return true;
}

void RealmClientLog(const char *severity, const char *category, const char *fmt, ...)
{
	if (!gbIsClosedRealm)
		return;

	const std::string logPath = paths::PrefPath() + "dx-eternity-client-realm.log";
	std::FILE *fp = std::fopen(logPath.c_str(), "a");
	if (fp == nullptr)
		return;

	const std::time_t nowSec = std::time(nullptr);
	std::tm tmBuf {};
#ifdef _WIN32
	gmtime_s(&tmBuf, &nowSec);
#else
	gmtime_r(&nowSec, &tmBuf);
#endif
	char ts[32] {};
	std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S UTC", &tmBuf);

	std::fprintf(fp, "[%s] [%s] [%s] ", ts, severity, category);
	va_list args;
	va_start(args, fmt);
	std::vfprintf(fp, fmt, args);
	va_end(args);
	std::fprintf(fp, "\n");
	std::fclose(fp);
}

#ifdef REALM_CHAOS_TEST
void RunRealmChaosExploitTest(int exploitId)
{
	if (!gbIsClosedRealm || MyPlayerId < 0 || MyPlayerId >= MAX_PLRS)
		return;

	auto &plr = Players[MyPlayerId];
	switch (exploitId) {
	case 1: // [Ctrl+Alt+F5] Simulate Memory Trainer: +500,000 Gold & +50 Base Strength before Save
		plr._pGold += 500000;
		plr._pBaseStr = 250;
		RealmClientLog("WARN", "CHAOS_TEST", "Triggered Trainer Gold/Str mutation -> sending OP_HERO_SAVE_REQ (expect ERR_SAVE_AUDIT_FAILED=19)");
		pfile_write_hero();
		NetSendCmdString(1 << MyPlayerId, "[Chaos Test F5] Trainer Gold/Str mutation -> sent OP_HERO_SAVE_REQ");
		break;

	case 2: // [Ctrl+Alt+F6] Simulate Item Dupe Trainer: Clone Equipped Weapon (InvBody[4]) into InvList[0]
		plr.InvList[0] = plr.InvBody[INVLOC_HAND_LEFT];
		if (plr._pNumInv < 1)
			plr._pNumInv = 1;
		RealmClientLog("WARN", "CHAOS_TEST", "Triggered Equipped Item Clone -> sending OP_HERO_SAVE_REQ (expect ERR_SAVE_AUDIT_FAILED=19)");
		pfile_write_hero();
		NetSendCmdString(1 << MyPlayerId, "[Chaos Test F6] Cloned Weapon into InvList[0] -> sent OP_HERO_SAVE_REQ");
		break;

	case 3: // [Ctrl+Alt+F7] Simulate 1-Shot God-Mode Boss Kill Packet (combatDurationMs=10ms, maxSingleHitDealt=99999)
	{
		devilution::realm::MonsterDropRequestPayload fakeKill {};
		fakeKill.gameId = GetRealmClient().GetActiveGameId();
		fakeKill.currlevel = currlevel;
		fakeKill.tileX = static_cast<uint8_t>(std::clamp<int>(plr.position.tile.x, 0, MAXDUNX - 1));
		fakeKill.tileY = static_cast<uint8_t>(std::clamp<int>(plr.position.tile.y, 0, MAXDUNY - 1));
		fakeKill.isUniqueMonster = 0;
		fakeKill.monsterIndex = 0;
		fakeKill.monsterTypeOrUid = MT_DIABLO;
		fakeKill.totalDamageDealt = 99999;
		fakeKill.maxSingleHitDealt = 99999;
		fakeKill.combatDurationMs = 10; // Impossible 10ms kill!
		devilution::realm::MonsterOrChestDropResponsePayload resp {};
		GetRealmClient().SendRawRpc(
		    devilution::realm::RealmOpcode::OP_MONSTER_DROP_REQ, &fakeKill, sizeof(fakeKill),
		    devilution::realm::RealmOpcode::OP_MONSTER_DROP_RESP, &resp, sizeof(resp));
		RealmClientLog("WARN", "CHAOS_TEST", "Sent 1-Shot Diablo Kill RPC -> server returned statusCode=%u (expect ERR_ANOMALY_TTK_VIOLATION=16)", resp.statusCode);
		NetSendCmdString(1 << MyPlayerId, fmt::format("[Chaos Test F7] 1-Shot Diablo Kill RPC -> status={} (expect 16)", resp.statusCode).c_str());
		break;
	}

	case 4: // [Ctrl+Alt+F8] Simulate Maphack / Town-to-Hell-16 Floor Warp Packet
	{
		devilution::realm::FloorTransitionRequestPayload fakeWarp {
			GetRealmClient().GetActiveGameId(),
			currlevel,
			16,
			40,
			40
		};
		devilution::realm::FloorTransitionResponsePayload resp {};
		GetRealmClient().SendRawRpc(
		    devilution::realm::RealmOpcode::OP_FLOOR_TRANSITION_REQ, &fakeWarp, sizeof(fakeWarp),
		    devilution::realm::RealmOpcode::OP_FLOOR_TRANSITION_RESP, &resp, sizeof(resp));
		RealmClientLog("WARN", "CHAOS_TEST", "Sent illegal Floor Warp %u->16 -> server returned statusCode=%u (expect ERR_ANOMALY_FLOOR_GATE_VIOLATION=18)", currlevel, resp.statusCode);
		NetSendCmdString(1 << MyPlayerId, fmt::format("[Chaos Test F8] Illegal Floor Warp {}->16 -> status={} (expect 18)", currlevel, resp.statusCode).c_str());
		break;
	}

	case 5: // [Ctrl+Alt+F9] Simulate Forged Ground Item Pickup Packet
	{
		devilution::realm::ItemPickupRequestPayload fakePickup {};
		fakePickup.gameId = GetRealmClient().GetActiveGameId();
		fakePickup.itemUid = 0xDEADBEEFULL;
		fakePickup.currlevel = currlevel;
		fakePickup.tileX = static_cast<uint8_t>(std::clamp<int>(plr.position.tile.x, 0, MAXDUNX - 1));
		fakePickup.tileY = static_cast<uint8_t>(std::clamp<int>(plr.position.tile.y, 0, MAXDUNY - 1));
		fakePickup.iSeed = 0x1337BEEFU;
		fakePickup.idx = 10;
		fakePickup.iCreateInfo = 1;
		devilution::realm::ItemPickupResponsePayload resp {};
		GetRealmClient().SendRawRpc(
		    devilution::realm::RealmOpcode::OP_ITEM_PICKUP_REQ, &fakePickup, sizeof(fakePickup),
		    devilution::realm::RealmOpcode::OP_ITEM_PICKUP_RESP, &resp, sizeof(resp));
		RealmClientLog("WARN", "CHAOS_TEST", "Sent Forged Item Pickup RPC -> server returned statusCode=%u (expect ERR_ITEM_NOT_ON_GROUND=15)", resp.statusCode);
		NetSendCmdString(1 << MyPlayerId, fmt::format("[Chaos Test F9] Forged Item Pickup RPC -> status={} (expect 15)", resp.statusCode).c_str());
		break;
	}

	case 6: // [Ctrl+Alt+F10] Simulate Unbacked Stash Gold Injection (> 300k hard cap)
	{
		devilution::realm::StashDataHeader fakeStash { 0, 0, 999999U };
		devilution::realm::StashSaveResponsePayload resp {};
		GetRealmClient().SendRawRpc(
		    devilution::realm::RealmOpcode::OP_STASH_SAVE_REQ, &fakeStash, sizeof(fakeStash),
		    devilution::realm::RealmOpcode::OP_STASH_SAVE_RESP, &resp, sizeof(resp));
		RealmClientLog("WARN", "CHAOS_TEST", "Sent Over-Cap Stash Gold Save RPC -> server returned statusCode=%u (expect ERR_SAVE_AUDIT_FAILED=19)", resp.statusCode);
		NetSendCmdString(1 << MyPlayerId, fmt::format("[Chaos Test F10] Over-Cap Stash Gold RPC -> status={} (expect 19)", resp.statusCode).c_str());
		break;
	}
	}
}
#endif

} // namespace devilution
