/**
 * @file main.cpp
 * @brief Entry point and automated Phase 1 + Phase 2 + Phase 3 self-test suite for dx-eternity-realm.
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sodium.h>

#include "admin/realm_admin_server.hpp"
#include "admin/realm_updater.hpp"
#include "anticheat/anomaly_detector.hpp"
#include "auth/realm_auth.hpp"
#include "db/realm_database.hpp"
#include "game/item_verifier.hpp"
#include "game/lcg.hpp"
#include "net/packet_codec.hpp"
#include "net/realm_server.hpp"
#include "protocol/realm_protocol.hpp"
#include "vault/realm_vault.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace devilution::realm {

namespace {

void CloseTestSocket(SocketHandle s)
{
	if (s == InvalidSocketHandle)
		return;
#ifdef _WIN32
	closesocket(static_cast<SOCKET>(s));
#else
	close(s);
#endif
}

SocketHandle ConnectLoopbackClient(uint16_t port)
{
#ifdef _WIN32
	const SOCKET rawClient = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (rawClient == INVALID_SOCKET)
		return InvalidSocketHandle;
	const SocketHandle clientSock = static_cast<SocketHandle>(rawClient);
#else
	const int clientSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (clientSock < 0)
		return InvalidSocketHandle;
#endif

	sockaddr_in addr {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	if (connect(
#ifdef _WIN32
	        static_cast<SOCKET>(clientSock),
#else
	        clientSock,
#endif
	        reinterpret_cast<const sockaddr *>(&addr),
	        sizeof(addr))
	    != 0) {
		CloseTestSocket(clientSock);
		return InvalidSocketHandle;
	}
	return clientSock;
}

bool SendAllBlocking(SocketHandle s, const uint8_t *data, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
#ifdef _WIN32
		const int n = send(static_cast<SOCKET>(s), reinterpret_cast<const char *>(data + sent), static_cast<int>(len - sent), 0);
#else
		const ssize_t n = send(s, data + sent, len - sent, 0);
#endif
		if (n <= 0)
			return false;
		sent += static_cast<size_t>(n);
	}
	return true;
}

bool RecvExactWithServerPoll(SocketHandle s, RealmServer &server, uint8_t *outBuf, size_t expectedBytes)
{
	for (int i = 0; i < 6; i++) {
		server.PollOnce(5);
	}
	size_t received = 0;
	while (received < expectedBytes) {
#ifdef _WIN32
		const int n = recv(static_cast<SOCKET>(s), reinterpret_cast<char *>(outBuf + received),
		    static_cast<int>(expectedBytes - received), 0);
#else
		const ssize_t n = recv(s, outBuf + received, expectedBytes - received, 0);
#endif
		if (n <= 0)
			return false;
		received += static_cast<size_t>(n);
	}
	return true;
}

bool PerformAttestedHandshake(SocketHandle sock, RealmServer &server, uint32_t &inOutClientSeq, uint32_t &inOutServerSeq, SessionKey &outTrafficKey)
{
	const SessionKey initialKey = GetDefaultHandshakeKey();

	// 1. Send OP_AUTH_CHALLENGE_REQ
	const std::vector<uint8_t> chalReq = EncodePacketFrame(RealmOpcode::OP_AUTH_CHALLENGE_REQ, inOutClientSeq++, nullptr, 0U, initialKey);
	if (!SendAllBlocking(sock, chalReq.data(), chalReq.size()))
		return false;

	std::vector<uint8_t> chalRespFrame(sizeof(RealmPacketHeader) + sizeof(AuthChallengeResponsePayload));
	if (!RecvExactWithServerPoll(sock, server, chalRespFrame.data(), chalRespFrame.size()))
		return false;

	RealmPacketHeader chalHdr {};
	std::memcpy(&chalHdr, chalRespFrame.data(), sizeof(chalHdr));
	const uint8_t *chalPayloadPtr = chalRespFrame.data() + sizeof(RealmPacketHeader);
	if (chalHdr.opcode != static_cast<uint16_t>(RealmOpcode::OP_AUTH_CHALLENGE_RESP)
	    || chalHdr.sequenceNumber != inOutServerSeq++
	    || !VerifyPacketMac(chalHdr, chalPayloadPtr, sizeof(AuthChallengeResponsePayload), initialKey)) {
		return false;
	}

	AuthChallengeResponsePayload chalResp {};
	std::memcpy(&chalResp, chalPayloadPtr, sizeof(chalResp));

	// 2. Build and send OP_ATTEST_HANDSHAKE_REQ
	AttestHandshakeRequestPayload attestReq {};
	randombytes_buf(attestReq.clientNonce, sizeof(attestReq.clientNonce));
	std::memset(attestReq.clientTextSectionHash, 0xA5, sizeof(attestReq.clientTextSectionHash));
	attestReq.clientBuildVersion = 0x00000704U; // Beta v0.7d

	const AttestationKey attestKey = server.AuthManager().GetAttestationKey();
	const auto hmac = ComputeClientAttestationHmac(
	    attestKey,
	    chalResp.serverNonce,
	    attestReq.clientNonce,
	    attestReq.clientTextSectionHash,
	    attestReq.clientBuildVersion);
	std::memcpy(attestReq.attestationHmac, hmac.data(), sizeof(attestReq.attestationHmac));

	const std::vector<uint8_t> attestFrame = EncodePacketFrame(
	    RealmOpcode::OP_ATTEST_HANDSHAKE_REQ, inOutClientSeq++, &attestReq, sizeof(attestReq), initialKey);
	if (!SendAllBlocking(sock, attestFrame.data(), attestFrame.size()))
		return false;

	std::vector<uint8_t> attestRespFrame(sizeof(RealmPacketHeader) + sizeof(AttestHandshakeResponsePayload));
	if (!RecvExactWithServerPoll(sock, server, attestRespFrame.data(), attestRespFrame.size()))
		return false;

	RealmPacketHeader attestHdr {};
	std::memcpy(&attestHdr, attestRespFrame.data(), sizeof(attestHdr));
	const uint8_t *attestPayloadPtr = attestRespFrame.data() + sizeof(RealmPacketHeader);
	if (attestHdr.opcode != static_cast<uint16_t>(RealmOpcode::OP_ATTEST_HANDSHAKE_RESP)
	    || attestHdr.sequenceNumber != inOutServerSeq++
	    || !VerifyPacketMac(attestHdr, attestPayloadPtr, sizeof(AttestHandshakeResponsePayload), initialKey)) {
		return false;
	}

	AttestHandshakeResponsePayload attestResp {};
	std::memcpy(&attestResp, attestPayloadPtr, sizeof(attestResp));
	if (attestResp.statusCode != static_cast<uint16_t>(RealmStatus::OK))
		return false;

	outTrafficKey = DeriveSessionTrafficKey(attestKey, chalResp.serverNonce, attestReq.clientNonce);
	return true;
}

template <typename TResp>
bool ExchangeRpc(
    SocketHandle sock,
    RealmServer &server,
    uint32_t &inOutClientSeq,
    uint32_t &inOutServerSeq,
    const SessionKey &trafficKey,
    RealmOpcode reqOpcode,
    const void *reqPayload,
    size_t reqPayloadLen,
    RealmOpcode expectedRespOpcode,
    TResp &outResp)
{
	const std::vector<uint8_t> reqFrame = EncodePacketFrame(reqOpcode, inOutClientSeq++, reqPayload, reqPayloadLen, trafficKey);
	if (!SendAllBlocking(sock, reqFrame.data(), reqFrame.size()))
		return false;

	std::vector<uint8_t> respFrame(sizeof(RealmPacketHeader) + sizeof(TResp));
	if (!RecvExactWithServerPoll(sock, server, respFrame.data(), respFrame.size()))
		return false;

	RealmPacketHeader hdr {};
	std::memcpy(&hdr, respFrame.data(), sizeof(hdr));
	const uint8_t *payloadPtr = respFrame.data() + sizeof(RealmPacketHeader);
	if (hdr.opcode != static_cast<uint16_t>(expectedRespOpcode)
	    || hdr.sequenceNumber != inOutServerSeq++
	    || !VerifyPacketMac(hdr, payloadPtr, sizeof(TResp), trafficKey)) {
		return false;
	}
	std::memcpy(&outResp, payloadPtr, sizeof(TResp));
	return true;
}

int RunSelfTest()
{
	std::printf("[SelfTest] Starting DX:Eternity Closed Realm Server Verification (Phase 1 + Phase 2 + Phase 3)...\n");

	// =========================================================================
	// PHASE 1: Binary Layout, SQLite3 WAL, Framing & Non-Blocking TCP Loop
	// =========================================================================
	std::printf("[SelfTest] [Phase 1] Verifying binary structure parity: PkItemStruct=%zu, PlayerPack=%zu, TCmdGItem=%zu, RealmPacketHeader=%zu\n",
	    sizeof(PkItemStruct), sizeof(PlayerPack), sizeof(TCmdGItem), sizeof(RealmPacketHeader));

	RealmDatabase db;
	std::string dbError;
	if (!db.Open(":memory:", dbError)) {
		std::fprintf(stderr, "[SelfTest] FAIL: RealmDatabase::Open failed: %s\n", dbError.c_str());
		return 1;
	}
	if (!db.LogSecurityEvent(0, 0, 0, "INFO", "SELFTEST_BOOT", "Self-test database initialized")) {
		std::fprintf(stderr, "[SelfTest] FAIL: LogSecurityEvent failed\n");
		return 1;
	}

	RealmServer server(db);
	std::string netError;
	if (!server.Start("127.0.0.1", 0, netError)) {
		std::fprintf(stderr, "[SelfTest] FAIL: RealmServer::Start failed: %s\n", netError.c_str());
		return 1;
	}
	const uint16_t port = server.BoundPort();
	std::printf("[SelfTest] [Phase 1] RealmServer bound to 127.0.0.1:%u\n", port);

	{
		const SocketHandle clientSock = ConnectLoopbackClient(port);
		if (clientSock == InvalidSocketHandle) {
			std::fprintf(stderr, "[SelfTest] FAIL: Client connect() failed\n");
			return 1;
		}
		server.PollOnce(10);

		const SessionKey key = GetDefaultHandshakeKey();
		PingPongPayload ping {};
		ping.clientTimestampMs = 123456789ULL;
		ping.serverTimestampMs = 0ULL;

		const std::vector<uint8_t> pingFrame = EncodePacketFrame(RealmOpcode::OP_PING, 1U, &ping, sizeof(ping), key);
		const std::vector<uint8_t> chalFrame = EncodePacketFrame(RealmOpcode::OP_AUTH_CHALLENGE_REQ, 2U, nullptr, 0U, key);

		if (!SendAllBlocking(clientSock, pingFrame.data(), pingFrame.size())
		    || !SendAllBlocking(clientSock, chalFrame.data(), chalFrame.size())) {
			std::fprintf(stderr, "[SelfTest] FAIL: Client send() failed\n");
			return 1;
		}

		const size_t expectedBytes = (sizeof(RealmPacketHeader) + sizeof(PingPongPayload))
		    + (sizeof(RealmPacketHeader) + sizeof(AuthChallengeResponsePayload));
		std::vector<uint8_t> respBuf(expectedBytes);
		if (!RecvExactWithServerPoll(clientSock, server, respBuf.data(), expectedBytes)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Failed receiving OP_PONG + OP_AUTH_CHALLENGE_RESP\n");
			return 1;
		}

		// Forged MAC packet rejection check
		std::vector<uint8_t> tamperedFrame = EncodePacketFrame(RealmOpcode::OP_PING, 3U, &ping, sizeof(ping), key);
		tamperedFrame[sizeof(RealmPacketHeader) - 1] ^= 0xFFU;
		SendAllBlocking(clientSock, tamperedFrame.data(), tamperedFrame.size());
		for (int i = 0; i < 5; i++) {
			server.PollOnce(5);
		}
		CloseTestSocket(clientSock);
	}

	// =========================================================================
	// PHASE 2: Official Binary Attestation & Argon2id Account Authentication
	// =========================================================================
	{
		const SocketHandle unattestedSock = ConnectLoopbackClient(port);
		server.PollOnce(5);

		const SessionKey initialKey = GetDefaultHandshakeKey();
		AuthCredentialsRequestPayload creds {};
		std::snprintf(creds.username, sizeof(creds.username), "%s", "cheater");
		std::snprintf(creds.password, sizeof(creds.password), "%s", "password123");

		const std::vector<uint8_t> loginFrame = EncodePacketFrame(
		    RealmOpcode::OP_AUTH_LOGIN_REQ, 1U, &creds, sizeof(creds), initialKey);
		SendAllBlocking(unattestedSock, loginFrame.data(), loginFrame.size());

		std::vector<uint8_t> respBuf(sizeof(RealmPacketHeader) + sizeof(AuthResultPayload));
		if (!RecvExactWithServerPoll(unattestedSock, server, respBuf.data(), respBuf.size())) {
			std::fprintf(stderr, "[SelfTest] FAIL: Did not receive OP_AUTH_RESULT for unattested client\n");
			return 1;
		}
		AuthResultPayload authRes {};
		std::memcpy(&authRes, respBuf.data() + sizeof(RealmPacketHeader), sizeof(authRes));
		if (authRes.statusCode != static_cast<uint16_t>(RealmStatus::ERR_ATTESTATION_FAILED)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Expected ERR_ATTESTATION_FAILED for unattested client, got %u\n", authRes.statusCode);
			return 1;
		}
		CloseTestSocket(unattestedSock);
		std::printf("[SelfTest] [Phase 2] Unattested login attempt properly rejected with ERR_ATTESTATION_FAILED.\n");
	}

	{
		const SocketHandle forgedSock = ConnectLoopbackClient(port);
		server.PollOnce(5);

		const SessionKey initialKey = GetDefaultHandshakeKey();
		const std::vector<uint8_t> chalReq = EncodePacketFrame(RealmOpcode::OP_AUTH_CHALLENGE_REQ, 1U, nullptr, 0U, initialKey);
		SendAllBlocking(forgedSock, chalReq.data(), chalReq.size());

		std::vector<uint8_t> chalRespBuf(sizeof(RealmPacketHeader) + sizeof(AuthChallengeResponsePayload));
		if (!RecvExactWithServerPoll(forgedSock, server, chalRespBuf.data(), chalRespBuf.size())) {
			std::fprintf(stderr, "[SelfTest] FAIL: Challenge response failed on forged client test\n");
			return 1;
		}

		AttestHandshakeRequestPayload badAttest {};
		randombytes_buf(badAttest.clientNonce, sizeof(badAttest.clientNonce));
		randombytes_buf(badAttest.attestationHmac, sizeof(badAttest.attestationHmac));
		badAttest.clientBuildVersion = 0x00000704U;

		const std::vector<uint8_t> attestFrame = EncodePacketFrame(
		    RealmOpcode::OP_ATTEST_HANDSHAKE_REQ, 2U, &badAttest, sizeof(badAttest), initialKey);
		SendAllBlocking(forgedSock, attestFrame.data(), attestFrame.size());

		std::vector<uint8_t> attestRespBuf(sizeof(RealmPacketHeader) + sizeof(AttestHandshakeResponsePayload));
		if (!RecvExactWithServerPoll(forgedSock, server, attestRespBuf.data(), attestRespBuf.size())) {
			std::fprintf(stderr, "[SelfTest] FAIL: Did not receive OP_ATTEST_HANDSHAKE_RESP for forged client\n");
			return 1;
		}
		AttestHandshakeResponsePayload attestResp {};
		std::memcpy(&attestResp, attestRespBuf.data() + sizeof(RealmPacketHeader), sizeof(attestResp));
		if (attestResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_ATTESTATION_FAILED)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Forged binary attestation was not rejected\n");
			return 1;
		}
		CloseTestSocket(forgedSock);
		std::printf("[SelfTest] [Phase 2] Forged client binary attestation properly rejected with ERR_ATTESTATION_FAILED.\n");
	}

	uint64_t registeredAccountId = 0;
	SessionToken issuedToken {};
	{
		const SocketHandle sock = ConnectLoopbackClient(port);
		server.PollOnce(5);

		uint32_t clientSeq = 1;
		uint32_t serverSeq = 1;
		SessionKey sessionTrafficKey {};
		if (!PerformAttestedHandshake(sock, server, clientSeq, serverSeq, sessionTrafficKey)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Official binary attestation handshake failed\n");
			return 1;
		}
		std::printf("[SelfTest] [Phase 2] Official binary attestation succeeded; traffic MAC key rotated.\n");

		// Verify unauthenticated OP_HERO_LIST_REQ is rejected before login
		ErrorResponsePayload unauthErr {};
		if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_LIST_REQ, nullptr, 0U, RealmOpcode::OP_ERROR_RESPONSE, unauthErr)
		    || unauthErr.statusCode != static_cast<uint16_t>(RealmStatus::ERR_UNAUTHORIZED)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Expected ERR_UNAUTHORIZED on unauthenticated OP_HERO_LIST_REQ\n");
			return 1;
		}

		// Register account "wanderer_01"
		AuthCredentialsRequestPayload regReq {};
		std::snprintf(regReq.username, sizeof(regReq.username), "%s", "wanderer_01");
		std::snprintf(regReq.password, sizeof(regReq.password), "%s", "Tristram_Torment#2026");

		AuthResultPayload regRes {};
		if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_AUTH_REGISTER_REQ, &regReq, sizeof(regReq), RealmOpcode::OP_AUTH_RESULT, regRes)
		    || regRes.statusCode != static_cast<uint16_t>(RealmStatus::OK) || regRes.accountId == 0) {
			std::fprintf(stderr, "[SelfTest] FAIL: Account registration failed with status %u\n", regRes.statusCode);
			return 1;
		}
		registeredAccountId = regRes.accountId;
		std::memcpy(issuedToken.data(), regRes.sessionToken, sizeof(regRes.sessionToken));
		std::printf("[SelfTest] [Phase 2] Argon2id account registration verified (accountId=%llu).\n",
		    static_cast<unsigned long long>(registeredAccountId));

		// =========================================================================
		// PHASE 3: Server-Vault Hero Roster, Creation, Save Validation & Stash
		// =========================================================================
		// 3A. Create a Classic Female Barbarian hero (HeroClass::Barbarian == 5)
		HeroCreateRequestPayload createBarbReq {};
		std::snprintf(createBarbReq.name, sizeof(createBarbReq.name), "%s", "Valkyrie_BB");
		createBarbReq.heroClass = static_cast<uint8_t>(HeroClass::Barbarian);
		createBarbReq.bIsHellfire = 1;

		HeroCreateResponsePayload createBarbResp {};
		if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_CREATE_REQ, &createBarbReq, sizeof(createBarbReq), RealmOpcode::OP_HERO_CREATE_RESP, createBarbResp)
		    || createBarbResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Barbarian hero creation failed (status=%u)\n", createBarbResp.statusCode);
			return 1;
		}

		const uint64_t barbCharId = createBarbResp.summary.charId;
		if (createBarbResp.pack.pBaseStr != 40 || createBarbResp.pack.pBaseMag != 0
		    || createBarbResp.pack.pBaseDex != 20 || createBarbResp.pack.pBaseVit != 25
		    || createBarbResp.pack.pGold != 100
		    || createBarbResp.pack.pHPBase != CalculateHeroBaseLife(HeroClass::Barbarian, 1, 25)
		    || createBarbResp.pack.pManaBase != 0) {
			std::fprintf(stderr, "[SelfTest] FAIL: Barbarian starting PlayerPack stats mismatch\n");
			return 1;
		}
		if (db.CountLedgerItemsForCharacter(static_cast<int64_t>(barbCharId)) != 4) {
			std::fprintf(stderr, "[SelfTest] FAIL: Expected 4 starting equipment items in item_ledger for Barbarian\n");
			return 1;
		}
		std::printf("[SelfTest] [Phase 3] Created Classic Female Barbarian 'Valkyrie_BB' (charId=%llu, Str=40, Mag=0, Dex=20, Vit=25, LedgerItems=4).\n",
		    static_cast<unsigned long long>(barbCharId));

		// 3B. Create a Sorcerer hero and verify Firebolt level 2 & duplicate name rejection
		HeroCreateRequestPayload createSorcReq {};
		std::snprintf(createSorcReq.name, sizeof(createSorcReq.name), "%s", "Horazon_SR");
		createSorcReq.heroClass = static_cast<uint8_t>(HeroClass::Sorcerer);
		createSorcReq.bIsHellfire = 1;

		HeroCreateResponsePayload createSorcResp {};
		if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_CREATE_REQ, &createSorcReq, sizeof(createSorcReq), RealmOpcode::OP_HERO_CREATE_RESP, createSorcResp)
		    || createSorcResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)
		    || createSorcResp.pack.pMemSpells != 1ULL || createSorcResp.pack.pSplLvl[1] != 2) {
			std::fprintf(stderr, "[SelfTest] FAIL: Sorcerer hero creation or starting Firebolt level failed\n");
			return 1;
		}
		const uint64_t sorcCharId = createSorcResp.summary.charId;

		// Case-insensitive duplicate hero name rejection ("valkyrie_bb")
		HeroCreateRequestPayload dupHeroReq = createBarbReq;
		std::snprintf(dupHeroReq.name, sizeof(dupHeroReq.name), "%s", "valkyrie_bb");
		HeroCreateResponsePayload dupHeroResp {};
		if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_CREATE_REQ, &dupHeroReq, sizeof(dupHeroReq), RealmOpcode::OP_HERO_CREATE_RESP, dupHeroResp)
		    || dupHeroResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_HERO_NAME_TAKEN)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Duplicate hero name was not rejected with ERR_HERO_NAME_TAKEN\n");
			return 1;
		}

		// 3C. List heroes via OP_HERO_LIST_REQ
		HeroListResponsePayload listResp {};
		if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_LIST_REQ, nullptr, 0U, RealmOpcode::OP_HERO_LIST_RESP, listResp)
		    || listResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)
		    || listResp.heroCount != 2) {
			std::fprintf(stderr, "[SelfTest] FAIL: OP_HERO_LIST_RESP expected 2 heroes, got %u\n", listResp.heroCount);
			return 1;
		}

		// 3D. Load Valkyrie_BB, level her up legitimately to Level 2 + Normal Diablo kill (pDiabloKillLevel=1), and save
		HeroLoadRequestPayload loadReq { barbCharId };
		HeroLoadResponsePayload loadResp {};
		if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_LOAD_REQ, &loadReq, sizeof(loadReq), RealmOpcode::OP_HERO_LOAD_RESP, loadResp)
		    || loadResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)) {
			std::fprintf(stderr, "[SelfTest] FAIL: OP_HERO_LOAD_REQ failed\n");
			return 1;
		}

		HeroSaveRequestPayload validSave {};
		validSave.charId = barbCharId;
		validSave.pack = loadResp.pack;
		validSave.pack.pLevel = 2;
		validSave.pack.pExperience = 2500;
		validSave.pack.pBaseStr = 45; // +5 stat points from level 2
		validSave.pack.pMaxHPBase = CalculateHeroBaseLife(HeroClass::Barbarian, 2, 25);
		validSave.pack.pHPBase = validSave.pack.pMaxHPBase;
		validSave.pack.pDiabloKillLevel = 1; // Normal Diablo slain

		HeroSaveResponsePayload saveResp {};
		if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_SAVE_REQ, &validSave, sizeof(validSave), RealmOpcode::OP_HERO_SAVE_RESP, saveResp)
		    || saveResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Legitimate OP_HERO_SAVE_REQ failed (status=%u)\n", saveResp.statusCode);
			return 1;
		}

		// 3E. Attempt a hacked OP_HERO_SAVE_REQ (stat point edit: pBaseStr=250 at level 2) -> must be rejected with ERR_SAVE_AUDIT_FAILED
		HeroSaveRequestPayload hackedSave = validSave;
		hackedSave.pack.pBaseStr = 250;
		if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_SAVE_REQ, &hackedSave, sizeof(hackedSave), RealmOpcode::OP_HERO_SAVE_RESP, saveResp)
		    || saveResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_SAVE_AUDIT_FAILED)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Hacked stat point PlayerPack was not rejected with ERR_SAVE_AUDIT_FAILED\n");
			return 1;
		}
		std::printf("[SelfTest] [Phase 3] Hero Load, legitimate Save, and hacked PlayerPack rejection (ERR_SAVE_AUDIT_FAILED) verified.\n");

		// 3F. Save and reload shared 1-Page Server-Vault Stash (physical <=5000g IDI_GOLD stacks + CF_TORMENT item + 300k hard cap)
		PkItemStruct goldStack5k = loadResp.pack.InvList[0]; // Starting IDI_GOLD template
		goldStack5k.idx = IDI_GOLD;
		goldStack5k.wValue = 5000U;

		StashDataHeader stashSaveHdr {};
		stashSaveHdr.statusCode = 0;
		stashSaveHdr.itemCount = 4;
		stashSaveHdr.stashGold = 10000U; // Exactly matches 2 x 5000g physical IDI_GOLD stacks below

		PkItemStruct stashItems[4] {};
		stashItems[0] = loadResp.pack.InvBody[4]; // Starting club
		stashItems[1] = loadResp.pack.InvBody[5]; // Starting buckler
		stashItems[1].dwBuff |= CF_TORMENT;       // Marked as Torment-rolled item
		stashItems[2] = goldStack5k;              // Physical 5,000g stack #1
		stashItems[3] = goldStack5k;              // Physical 5,000g stack #2

		std::vector<uint8_t> stashSavePayload(sizeof(StashDataHeader) + sizeof(stashItems));
		std::memcpy(stashSavePayload.data(), &stashSaveHdr, sizeof(StashDataHeader));
		std::memcpy(stashSavePayload.data() + sizeof(StashDataHeader), stashItems, sizeof(stashItems));

		StashSaveResponsePayload stashSaveResp {};
		if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_STASH_SAVE_REQ, stashSavePayload.data(), stashSavePayload.size(),
		        RealmOpcode::OP_STASH_SAVE_RESP, stashSaveResp)
		    || stashSaveResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)
		    || stashSaveResp.savedItemCount != 4 || stashSaveResp.stashGold != 10000U) {
			std::fprintf(stderr, "[SelfTest] FAIL: OP_STASH_SAVE_REQ failed\n");
			return 1;
		}

		// Verify rejection of unbacked virtual stashGold (e.g. header claims 50,000g with only 10,000g physical stacks)
		{
			StashDataHeader badVirtHdr = stashSaveHdr;
			badVirtHdr.stashGold = 50000U;
			std::vector<uint8_t> badVirtPayload = stashSavePayload;
			std::memcpy(badVirtPayload.data(), &badVirtHdr, sizeof(StashDataHeader));
			StashSaveResponsePayload badVirtResp {};
			if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_STASH_SAVE_REQ, badVirtPayload.data(), badVirtPayload.size(),
			        RealmOpcode::OP_STASH_SAVE_RESP, badVirtResp)
			    || badVirtResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_SAVE_AUDIT_FAILED)) {
				std::fprintf(stderr, "[SelfTest] FAIL: Unbacked virtual stashGold was not rejected!\n");
				return 1;
			}
		}

		// Verify rejection of a single IDI_GOLD stack > 5,000g (e.g. wValue = 5001)
		{
			PkItemStruct overStackItems[1] = { goldStack5k };
			overStackItems[0].wValue = 5001U;
			StashDataHeader overStackHdr { 0, 1, 5001U };
			std::vector<uint8_t> overStackPayload(sizeof(StashDataHeader) + sizeof(overStackItems));
			std::memcpy(overStackPayload.data(), &overStackHdr, sizeof(StashDataHeader));
			std::memcpy(overStackPayload.data() + sizeof(StashDataHeader), overStackItems, sizeof(overStackItems));
			StashSaveResponsePayload overStackResp {};
			if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_STASH_SAVE_REQ, overStackPayload.data(), overStackPayload.size(),
			        RealmOpcode::OP_STASH_SAVE_RESP, overStackResp)
			    || overStackResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_SAVE_AUDIT_FAILED)) {
				std::fprintf(stderr, "[SelfTest] FAIL: Over-limit 5001g gold stack was not rejected!\n");
				return 1;
			}
		}

		// Verify rejection of physical stash gold > 300,000g hard limit (61 x 5,000g stacks = 305,000g)
		{
			std::vector<PkItemStruct> over300kItems(61, goldStack5k);
			StashDataHeader over300kHdr { 0, 61, 305000U };
			std::vector<uint8_t> over300kPayload(sizeof(StashDataHeader) + over300kItems.size() * sizeof(PkItemStruct));
			std::memcpy(over300kPayload.data(), &over300kHdr, sizeof(StashDataHeader));
			std::memcpy(over300kPayload.data() + sizeof(StashDataHeader), over300kItems.data(), over300kItems.size() * sizeof(PkItemStruct));
			StashSaveResponsePayload over300kResp {};
			if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_STASH_SAVE_REQ, over300kPayload.data(), over300kPayload.size(),
			        RealmOpcode::OP_STASH_SAVE_RESP, over300kResp)
			    || over300kResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_SAVE_AUDIT_FAILED)) {
				std::fprintf(stderr, "[SelfTest] FAIL: >300,000g stash gold hard cap was not enforced!\n");
				return 1;
			}
		}

		// Verify rejection of >100 stash items (multi-page stash attempt: 101 items)
		{
			PkItemStruct smallGold = goldStack5k;
			smallGold.wValue = 100U;
			std::vector<PkItemStruct> multiPageItems(101, smallGold);
			StashDataHeader multiPageHdr { 0, 101, 10100U };
			std::vector<uint8_t> multiPagePayload(sizeof(StashDataHeader) + multiPageItems.size() * sizeof(PkItemStruct));
			std::memcpy(multiPagePayload.data(), &multiPageHdr, sizeof(StashDataHeader));
			std::memcpy(multiPagePayload.data() + sizeof(StashDataHeader), multiPageItems.data(), multiPageItems.size() * sizeof(PkItemStruct));
			StashSaveResponsePayload multiPageResp {};
			if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_STASH_SAVE_REQ, multiPagePayload.data(), multiPagePayload.size(),
			        RealmOpcode::OP_STASH_SAVE_RESP, multiPageResp)
			    || multiPageResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_SAVE_AUDIT_FAILED)) {
				std::fprintf(stderr, "[SelfTest] FAIL: >100 items multi-page stash save was not rejected!\n");
				return 1;
			}
		}

		// Load stash back
		const std::vector<uint8_t> stashLoadReqFrame = EncodePacketFrame(
		    RealmOpcode::OP_STASH_LOAD_REQ, clientSeq++, nullptr, 0U, sessionTrafficKey);
		SendAllBlocking(sock, stashLoadReqFrame.data(), stashLoadReqFrame.size());

		std::vector<uint8_t> stashLoadRespBuf(sizeof(RealmPacketHeader) + sizeof(StashDataHeader) + sizeof(stashItems));
		if (!RecvExactWithServerPoll(sock, server, stashLoadRespBuf.data(), stashLoadRespBuf.size())) {
			std::fprintf(stderr, "[SelfTest] FAIL: Failed receiving OP_STASH_LOAD_RESP\n");
			return 1;
		}
		serverSeq++;
		StashDataHeader loadedStashHdr {};
		std::memcpy(&loadedStashHdr, stashLoadRespBuf.data() + sizeof(RealmPacketHeader), sizeof(loadedStashHdr));
		PkItemStruct loadedStashItems[4] {};
		std::memcpy(loadedStashItems, stashLoadRespBuf.data() + sizeof(RealmPacketHeader) + sizeof(StashDataHeader), sizeof(loadedStashItems));
		if (loadedStashHdr.itemCount != 4 || loadedStashHdr.stashGold != 10000U
		    || (loadedStashItems[1].dwBuff & CF_TORMENT) == 0
		    || loadedStashItems[2].wValue != 5000U) {
			std::fprintf(stderr, "[SelfTest] FAIL: Loaded Stash data mismatch\n");
			return 1;
		}
		std::printf("[SelfTest] [Phase 3] 1-Page Server-Vault Stash (<=100 items, physical <=5,000g stacks, 300k gold cap, CF_TORMENT flag) verified.\n");

		// 3G. Delete Horazon_SR and verify roster & item_ledger cleanup
		HeroDeleteRequestPayload delReq { sorcCharId };
		HeroDeleteResponsePayload delResp {};
		if (!ExchangeRpc(sock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_DELETE_REQ, &delReq, sizeof(delReq), RealmOpcode::OP_HERO_DELETE_RESP, delResp)
		    || delResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)
		    || db.CountLedgerItemsForCharacter(static_cast<int64_t>(sorcCharId)) != 0) {
			std::fprintf(stderr, "[SelfTest] FAIL: OP_HERO_DELETE_REQ or item_ledger cleanup failed\n");
			return 1;
		}

		CloseTestSocket(sock);
	}

	// Reconnect, resume session with 256-bit token, and test ban enforcement
	{
		const SocketHandle resumeSock = ConnectLoopbackClient(port);
		server.PollOnce(5);

		uint32_t clientSeq = 1;
		uint32_t serverSeq = 1;
		SessionKey sessionTrafficKey {};
		if (!PerformAttestedHandshake(resumeSock, server, clientSeq, serverSeq, sessionTrafficKey)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Attestation failed on session resume connection\n");
			return 1;
		}

		AuthResumeRequestPayload resumeReq {};
		resumeReq.accountId = registeredAccountId;
		std::memcpy(resumeReq.sessionToken, issuedToken.data(), sizeof(resumeReq.sessionToken));

		AuthResultPayload resumeRes {};
		if (!ExchangeRpc(resumeSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_AUTH_RESUME_REQ, &resumeReq, sizeof(resumeReq), RealmOpcode::OP_AUTH_RESULT, resumeRes)
		    || resumeRes.statusCode != static_cast<uint16_t>(RealmStatus::OK)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Session token resume failed\n");
			return 1;
		}

		db.SetAccountBanStatus(static_cast<int64_t>(registeredAccountId), true, "Automated anomaly ban test");
		if (!ExchangeRpc(resumeSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_AUTH_RESUME_REQ, &resumeReq, sizeof(resumeReq), RealmOpcode::OP_AUTH_RESULT, resumeRes)
		    || resumeRes.statusCode != static_cast<uint16_t>(RealmStatus::ERR_AUTH_ACCOUNT_BANNED)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Expected ERR_AUTH_ACCOUNT_BANNED\n");
			return 1;
		}

		CloseTestSocket(resumeSock);
		std::printf("[SelfTest] [Phase 2] 256-bit session token resume & ban enforcement verified.\n");
	}

	// =========================================================================
	// PHASE 4: Deterministic 1:1 DX:Eternity LCG & RecreateItem() Engine
	// =========================================================================
	{
		// 4A. Borland C++ LCG parity check
		LcgEngine lcg;
		lcg.SetRndSeed(0x12345678U);
		const int32_t s1 = lcg.AdvanceRndSeed();
		const uint32_t expectedState = 0x12345678U * 0x015A4E35U + 1U;
		const int32_t expectedAbs = static_cast<int32_t>(expectedState) < 0 ? -static_cast<int32_t>(expectedState) : static_cast<int32_t>(expectedState);
		const int32_t r100 = lcg.GenerateRnd(100);
		if (lcg.GetLCGEngineState() == 0 || s1 != expectedAbs || r100 < 0 || r100 >= 100) {
			std::fprintf(stderr, "[SelfTest] FAIL: Borland C++ LCG state transition mismatch\n");
			return 1;
		}

		// 4B. Normal vs Torment (CF_TORMENT) deterministic LCG & maxed stat parity
		// Find Great Sword in AllItemsList
		_item_indexes greatSwordIdx = static_cast<_item_indexes>(1);
		for (int idx = 0; idx <= IDI_LAST; idx++) {
			if (AllItemsList[idx].itype == ItemType::Sword && AllItemsList[idx].iMinDam == 10 && AllItemsList[idx].iMaxDam == 20) {
				greatSwordIdx = static_cast<_item_indexes>(idx);
				break;
			}
		}

		PlayerPack testOwnerPack {};
		testOwnerPack.pClass = static_cast<uint8_t>(HeroClass::Barbarian);
		testOwnerPack.pLevel = 35;
		testOwnerPack.pBaseStr = 120;
		testOwnerPack.pBaseMag = 20;
		testOwnerPack.pBaseDex = 80;
		testOwnerPack.pBaseVit = 90;
		testOwnerPack.pMaxHPBase = (180 << 6);
		testOwnerPack.pMaxManaBase = (30 << 6);
		testOwnerPack.bIsHellfire = 1;

		ServerPlayerContext normCtx = ServerPlayerContext::FromPlayerPack(testOwnerPack);
		normCtx.nDifficulty = DIFF_NORMAL;
		ServerPlayerContext tormCtx = ServerPlayerContext::FromPlayerPack(testOwnerPack);
		tormCtx.nDifficulty = DIFF_TORMENT;

		const uint32_t testSeed = 0x5F3759DFU;
		ServerItem normSword {};
		SetupAllItems(lcg, normCtx, normSword, greatSwordIdx, testSeed, 30, 1, true, false, false);
		ServerItem tormSword {};
		SetupAllItems(lcg, tormCtx, tormSword, greatSwordIdx, testSeed, 30, 1, true, false, false);

		if (normSword._iPrePower != tormSword._iPrePower
		    || normSword._iSufPower != tormSword._iSufPower
		    || normSword._iMagical != tormSword._iMagical) {
			std::fprintf(stderr, "[SelfTest] FAIL: CF_TORMENT desynchronized LCG prefix/suffix selection from Normal roll!\n");
			return 1;
		}
		if ((tormSword.dwBuff & CF_TORMENT) == 0
		    || tormSword._iMaxDur != AllItemsList[greatSwordIdx].iDurability
		    || tormSword._iPLToHit < normSword._iPLToHit
		    || tormSword._iPLDam < normSword._iPLDam) {
			std::fprintf(stderr, "[SelfTest] FAIL: CF_TORMENT did not roll maximum possible affix/durability values!\n");
			return 1;
		}

		PkItemStruct packedNorm {};
		PackServerItem(packedNorm, normSword, true);
		PkItemStruct packedTorm {};
		PackServerItem(packedTorm, tormSword, true);
		std::string itemVerifyReason;
		if (!VerifyPackedItemIntegrity(testOwnerPack, packedNorm, itemVerifyReason)) {
			std::fprintf(stderr, "[SelfTest] FAIL: VerifyPackedItemIntegrity rejected valid Normal Great Sword: %s\n", itemVerifyReason.c_str());
			return 1;
		}
		if (!VerifyPackedItemIntegrity(testOwnerPack, packedTorm, itemVerifyReason)) {
			std::fprintf(stderr, "[SelfTest] FAIL: VerifyPackedItemIntegrity rejected valid CF_TORMENT Great Sword: %s\n", itemVerifyReason.c_str());
			return 1;
		}
		std::printf("[SelfTest] [Phase 4] CF_TORMENT LCG parity & maxed roll verification passed (Dur=%d/%d, ToHit=+%d vs +%d, Dam=+%d%% vs +%d%%).\n",
		    tormSword._iMaxDur, normSword._iMaxDur,
		    tormSword._iPLToHit, normSword._iPLToHit,
		    tormSword._iPLDam, normSword._iPLDam);

		// 4C. Verify Hell/Torment Diablo Exclusive Uniques (Kadis' Fury, Apostate's Crook, Loxley's Bend)
		const _unique_items exclusiveUniques[3] = {
			UITEM_KADISFURY,
			UITEM_APOSTATESCROOK,
			UITEM_LOXLEYSBEND
		};
		for (_unique_items uid : exclusiveUniques) {
			ServerItem uitem {};
			lcg.SetRndSeed(0x31415926);
			SpawnUniqueServerItem(lcg, tormCtx, uitem, uid, 32);
			PkItemStruct pkU {};
			PackServerItem(pkU, uitem, true);
			if (!VerifyPackedItemIntegrity(testOwnerPack, pkU, itemVerifyReason)) {
				std::fprintf(stderr, "[SelfTest] FAIL: VerifyPackedItemIntegrity failed for exclusive unique uid=%d: %s\n",
				    static_cast<int>(uid), itemVerifyReason.c_str());
				return 1;
			}
		}
		std::printf("[SelfTest] [Phase 4] Hell/Torment Diablo exclusive uniques (UITEM_KADISFURY, UITEM_APOSTATESCROOK, UITEM_LOXLEYSBEND) verified.\n");

		// 4D. Reject forged/injected items (forged bId quality bits, forged bMDur, invalid CF_PREGEN flag)
		PkItemStruct forgedQuality = packedTorm;
		forgedQuality.bId ^= 0x04U; // Tamper item quality bits
		if (VerifyPackedItemIntegrity(testOwnerPack, forgedQuality, itemVerifyReason)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Forged bId quality item was not rejected!\n");
			return 1;
		}

		PkItemStruct forgedDur = packedNorm;
		forgedDur.bMDur = 250; // Tamper max durability
		if (VerifyPackedItemIntegrity(testOwnerPack, forgedDur, itemVerifyReason)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Forged bMDur item was not rejected!\n");
			return 1;
		}

		PkItemStruct forgedPregen = packedNorm;
		forgedPregen.iCreateInfo |= CF_PREGEN; // Invalid pregen flag on monster drop
		if (VerifyPackedItemIntegrity(testOwnerPack, forgedPregen, itemVerifyReason)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Forged CF_PREGEN item was not rejected!\n");
			return 1;
		}
		std::printf("[SelfTest] [Phase 4] Forged PkItemStruct detection (tampered bId quality, bMDur, and CF_PREGEN flags) verified.\n");
	}

	// =========================================================================
	// PHASE 5: Game Session Registry & Server-Oracle Loot Dispensing RPCs
	// =========================================================================
	{
		// Clear ban from Phase 2 test so wanderer_01 can host a game
		db.SetAccountBanStatus(static_cast<int64_t>(registeredAccountId), false, "");

		const SocketHandle gameSock = ConnectLoopbackClient(port);
		server.PollOnce(5);

		uint32_t clientSeq = 1;
		uint32_t serverSeq = 1;
		SessionKey sessionTrafficKey {};
		if (!PerformAttestedHandshake(gameSock, server, clientSeq, serverSeq, sessionTrafficKey)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Attestation failed on Phase 5 game connection\n");
			return 1;
		}

		AuthCredentialsRequestPayload loginReq {};
		std::snprintf(loginReq.username, sizeof(loginReq.username), "%s", "wanderer_01");
		std::snprintf(loginReq.password, sizeof(loginReq.password), "%s", "Tristram_Torment#2026");
		AuthResultPayload loginRes {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_AUTH_LOGIN_REQ, &loginReq, sizeof(loginReq), RealmOpcode::OP_AUTH_RESULT, loginRes)
		    || loginRes.statusCode != static_cast<uint16_t>(RealmStatus::OK)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Login after unban failed in Phase 5\n");
			return 1;
		}

		// 5A. Verify DIFF_TORMENT game creation is rejected when character has only slain Normal Diablo (diabloKillLevel=1 < 3)
		GameCreateRequestPayload tormentReq {};
		tormentReq.charId = 1; // Valkyrie_BB (level 2, diabloKillLevel=1)
		std::snprintf(tormentReq.gameName, sizeof(tormentReq.gameName), "%s", "TormentSanctuary");
		tormentReq.difficulty = DIFF_TORMENT;
		tormentReq.maxPlayers = 4;
		tormentReq.bIsHellfire = 1;

		GameCreateOrJoinResponsePayload tormentResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_GAME_CREATE_REQ, &tormentReq, sizeof(tormentReq),
		        RealmOpcode::OP_GAME_CREATE_RESP, tormentResp)
		    || tormentResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_TORMENT_LOCKED_HELL_REQUIRED)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Expected ERR_TORMENT_LOCKED_HELL_REQUIRED, got %u\n", tormentResp.statusCode);
			return 1;
		}
		std::printf("[SelfTest] [Phase 5] DIFF_TORMENT entry gate enforced (rejected hero with diabloKillLevel=1).\n");

		// 5B. Verify Closed Realm Game Speed Cap (Default & Max = 30 'Fast'; >30 'Faster'/'Fastest' rejected)
		{
			GameCreateRequestPayload fasterSpeedReq {};
			fasterSpeedReq.charId = 1;
			std::snprintf(fasterSpeedReq.gameName, sizeof(fasterSpeedReq.gameName), "%s", "IllegalFasterRun");
			fasterSpeedReq.difficulty = DIFF_NORMAL;
			fasterSpeedReq.maxPlayers = 4;
			fasterSpeedReq.bIsHellfire = 1;
			fasterSpeedReq.tickRate = 40U; // 'Faster' (40 Hz) -> must be rejected on Closed Realm!

			GameCreateOrJoinResponsePayload fasterSpeedResp {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_GAME_CREATE_REQ, &fasterSpeedReq, sizeof(fasterSpeedReq),
			        RealmOpcode::OP_GAME_CREATE_RESP, fasterSpeedResp)
			    || fasterSpeedResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_MALFORMED_PACKET)) {
				std::fprintf(stderr, "[SelfTest] FAIL: Expected ERR_MALFORMED_PACKET for tickRate=40 ('Faster'), got %u\n",
				    fasterSpeedResp.statusCode);
				return 1;
			}
		}

		// Create a valid DIFF_NORMAL game (default tickRate=0 -> 30 'Fast') and verify OP_GAME_LIST_REQ
		GameCreateRequestPayload normGameReq {};
		normGameReq.charId = 1;
		std::snprintf(normGameReq.gameName, sizeof(normGameReq.gameName), "%s", "CathedralRun_01");
		normGameReq.difficulty = DIFF_NORMAL;
		normGameReq.maxPlayers = 4;
		normGameReq.bIsHellfire = 1;
		normGameReq.tickRate = 0U; // 0 = Server Default 'Fast' (30 ticks/sec)

		GameCreateOrJoinResponsePayload normGameResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_GAME_CREATE_REQ, &normGameReq, sizeof(normGameReq),
		        RealmOpcode::OP_GAME_CREATE_RESP, normGameResp)
		    || normGameResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)
		    || normGameResp.gameId == 0
		    || normGameResp.tickRate != DefaultClosedRealmTickRate) {
			std::fprintf(stderr, "[SelfTest] FAIL: Valid DIFF_NORMAL OP_GAME_CREATE_REQ failed or tickRate!=30\n");
			return 1;
		}
		const uint64_t activeGameId = normGameResp.gameId;

		GameListResponsePayload gameListResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_GAME_LIST_REQ, nullptr, 0U, RealmOpcode::OP_GAME_LIST_RESP, gameListResp)
		    || gameListResp.gameCount != 1) {
			std::fprintf(stderr, "[SelfTest] FAIL: OP_GAME_LIST_RESP expected 1 active game\n");
			return 1;
		}

		// 5C. Verify Floor Progression Gate (0 -> 1 valid; illegal warp 1 -> 16 rejected)
		FloorTransitionRequestPayload stepToCathedral { activeGameId, 0, 1, 25, 29 };
		FloorTransitionResponsePayload floorResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_FLOOR_TRANSITION_REQ, &stepToCathedral, sizeof(stepToCathedral),
		        RealmOpcode::OP_FLOOR_TRANSITION_RESP, floorResp)
		    || floorResp.statusCode != static_cast<uint16_t>(RealmStatus::OK) || floorResp.currentLevel != 1) {
			std::fprintf(stderr, "[SelfTest] FAIL: Legal floor transition 0 -> 1 failed\n");
			return 1;
		}

		FloorTransitionRequestPayload illegalWarp { activeGameId, 1, 16, 40, 40 };
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_FLOOR_TRANSITION_REQ, &illegalWarp, sizeof(illegalWarp),
		        RealmOpcode::OP_FLOOR_TRANSITION_RESP, floorResp)
		    || floorResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_ANOMALY_FLOOR_GATE_VIOLATION)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Illegal floor transition 1 -> 16 was not rejected\n");
			return 1;
		}
		std::printf("[SelfTest] [Phase 5] Floor Progression Gate verified (0 -> 1 allowed, 1 -> 16 warp blocked).\n");

		// 5D. Server-Oracle Monster Kill RPC (`OP_MONSTER_DROP_REQ`) & Duplicate Kill Rejection
		MonsterDropRequestPayload killReq {};
		killReq.gameId = activeGameId;
		killReq.currlevel = 1;
		killReq.tileX = 28;
		killReq.tileY = 31;
		killReq.isUniqueMonster = 0;
		killReq.monsterIndex = 0;
		killReq.monsterTypeOrUid = MT_NZOMBIE;
		killReq.totalDamageDealt = 12;
		killReq.maxSingleHitDealt = 6;
		killReq.combatDurationMs = 1200;

		MonsterOrChestDropResponsePayload killResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_MONSTER_DROP_REQ, &killReq, sizeof(killReq),
		        RealmOpcode::OP_MONSTER_DROP_RESP, killResp)
		    || killResp.statusCode != static_cast<uint16_t>(RealmStatus::OK) || killResp.awardedExp == 0) {
			std::fprintf(stderr, "[SelfTest] FAIL: Legitimate OP_MONSTER_DROP_REQ failed\n");
			return 1;
		}

		// Duplicate kill of same monsterIndex=0 on floor 1 must be rejected
		MonsterOrChestDropResponsePayload dupKillResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_MONSTER_DROP_REQ, &killReq, sizeof(killReq),
		        RealmOpcode::OP_MONSTER_DROP_RESP, dupKillResp)
		    || dupKillResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_DUPLICATE_KILL_OR_CHEST)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Duplicate monster kill was not rejected with ERR_DUPLICATE_KILL_OR_CHEST\n");
			return 1;
		}

		// Impossible single-hit damage (99999) on monsterIndex=1 must be rejected with ERR_ANOMALY_TTK_VIOLATION
		MonsterDropRequestPayload hackedKillReq = killReq;
		hackedKillReq.monsterIndex = 1;
		hackedKillReq.maxSingleHitDealt = 99999;
		MonsterOrChestDropResponsePayload hackedKillResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_MONSTER_DROP_REQ, &hackedKillReq, sizeof(hackedKillReq),
		        RealmOpcode::OP_MONSTER_DROP_RESP, hackedKillResp)
		    || hackedKillResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_ANOMALY_TTK_VIOLATION)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Impossible 99999 single-hit kill was not rejected with ERR_ANOMALY_TTK_VIOLATION\n");
			return 1;
		}

		// 5E. Server-Oracle Chest Drop (`OP_CHEST_DROP_REQ`) & Atomic Ground-to-Inventory Pickup (`OP_ITEM_PICKUP_REQ`)
		ChestDropRequestPayload chestReq {};
		chestReq.gameId = activeGameId;
		chestReq.currlevel = 1;
		chestReq.tileX = 33;
		chestReq.tileY = 35;
		chestReq.chestQuality = 2; // Large chest (guarantees magic item drop)
		chestReq.objectIndex = 7;

		MonsterOrChestDropResponsePayload chestResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_CHEST_DROP_REQ, &chestReq, sizeof(chestReq),
		        RealmOpcode::OP_CHEST_DROP_RESP, chestResp)
		    || chestResp.statusCode != static_cast<uint16_t>(RealmStatus::OK) || chestResp.dropCount == 0) {
			std::fprintf(stderr, "[SelfTest] FAIL: OP_CHEST_DROP_REQ failed or dispensed 0 items\n");
			return 1;
		}

		const OracleDroppedItemEntry &dispensed = chestResp.drops[0];
		ItemPickupRequestPayload pickupReq {};
		pickupReq.gameId = activeGameId;
		pickupReq.itemUid = dispensed.itemUid;
		pickupReq.currlevel = 1;
		pickupReq.tileX = chestReq.tileX;
		pickupReq.tileY = chestReq.tileY;
		pickupReq.iSeed = dispensed.packed.iSeed;
		pickupReq.idx = dispensed.packed.idx;
		pickupReq.iCreateInfo = dispensed.packed.iCreateInfo;

		ItemPickupResponsePayload pickupResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_ITEM_PICKUP_REQ, &pickupReq, sizeof(pickupReq),
		        RealmOpcode::OP_ITEM_PICKUP_RESP, pickupResp)
		    || pickupResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)
		    || !db.HasCharacterLedgerProvenance(1, pickupResp.item)) {
			std::fprintf(stderr, "[SelfTest] FAIL: First OP_ITEM_PICKUP_REQ or item_ledger provenance check failed\n");
			return 1;
		}

		// Second pickup attempt of the same ground item (dupe attempt) must be rejected with ERR_ITEM_NOT_ON_GROUND
		ItemPickupResponsePayload dupPickupResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_ITEM_PICKUP_REQ, &pickupReq, sizeof(pickupReq),
		        RealmOpcode::OP_ITEM_PICKUP_RESP, dupPickupResp)
		    || dupPickupResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_ITEM_NOT_ON_GROUND)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Duplicate OP_ITEM_PICKUP_REQ was not rejected with ERR_ITEM_NOT_ON_GROUND\n");
			return 1;
		}
		std::printf("[SelfTest] [Phase 5] Server-Oracle monster/chest drop dispensing & atomic anti-dupe pickup ledger verified.\n");

		// =========================================================================
		// PHASE 6: Server-Side Anti-Cheat Anomaly Detection & Save Conservation
		// =========================================================================
		// 6A. Multi-Hit Boss Time-To-Kill (TTK) Floor Check (instant 10ms kill on Diablo / high-HP boss must fail)
		MonsterDropRequestPayload instantBossReq {};
		instantBossReq.gameId = activeGameId;
		instantBossReq.currlevel = 1;
		instantBossReq.tileX = 29;
		instantBossReq.tileY = 32;
		instantBossReq.isUniqueMonster = 0;
		instantBossReq.monsterIndex = 2;
		instantBossReq.monsterTypeOrUid = MT_DIABLO;
		instantBossReq.totalDamageDealt = 2500;
		instantBossReq.maxSingleHitDealt = 45;
		instantBossReq.combatDurationMs = 10; // Impossible 10ms TTK for ~55+ hits

		MonsterOrChestDropResponsePayload instantBossResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_MONSTER_DROP_REQ, &instantBossReq, sizeof(instantBossReq),
		        RealmOpcode::OP_MONSTER_DROP_RESP, instantBossResp)
		    || instantBossResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_ANOMALY_TTK_VIOLATION)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Instant 10ms multi-hit boss kill was not rejected with ERR_ANOMALY_TTK_VIOLATION\n");
			return 1;
		}

		// 6B. Cross-Map Teleport Kill-Cadence Check (tile jump 28,31 -> 95,95 in < 200ms without Teleport)
		MonsterDropRequestPayload teleKillReq = killReq;
		teleKillReq.monsterIndex = 3;
		teleKillReq.tileX = 95;
		teleKillReq.tileY = 95;
		MonsterOrChestDropResponsePayload teleKillResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_MONSTER_DROP_REQ, &teleKillReq, sizeof(teleKillReq),
		        RealmOpcode::OP_MONSTER_DROP_RESP, teleKillResp)
		    || teleKillResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_ANOMALY_CADENCE_VIOLATION)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Cross-map teleport kill jump was not rejected with ERR_ANOMALY_CADENCE_VIOLATION\n");
			return 1;
		}
		std::printf("[SelfTest] [Phase 6] Multi-hit Boss TTK floor & Travel-Distance Cadence anti-cheat checks verified.\n");

		// 6C. Save Conservation (XP Conservation & 100% Item Ledger Provenance / Anti-Clone)
		HeroLoadRequestPayload reloadReq { 1 };
		HeroLoadResponsePayload reloadResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_LOAD_REQ, &reloadReq, sizeof(reloadReq),
		        RealmOpcode::OP_HERO_LOAD_RESP, reloadResp)
		    || reloadResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)) {
			std::fprintf(stderr, "[SelfTest] FAIL: OP_HERO_LOAD_REQ failed in Phase 6\n");
			return 1;
		}

		// Attempt save with +500,000 unverified XP gain -> must fail XP conservation audit
		HeroSaveRequestPayload xpHackSave {};
		xpHackSave.charId = 1;
		xpHackSave.pack = reloadResp.pack;
		xpHackSave.pack.pExperience += 500000;
		HeroSaveResponsePayload xpHackResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_SAVE_REQ, &xpHackSave, sizeof(xpHackSave),
		        RealmOpcode::OP_HERO_SAVE_RESP, xpHackResp)
		    || xpHackResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_SAVE_AUDIT_FAILED)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Unverified +500,000 XP save was not rejected with ERR_SAVE_AUDIT_FAILED\n");
			return 1;
		}

		// Attempt save with cloned starting club in InvList[1] (2 copies in pack vs 1 copy in item_ledger) -> must fail provenance audit
		HeroSaveRequestPayload cloneItemSave {};
		cloneItemSave.charId = 1;
		cloneItemSave.pack = reloadResp.pack;
		cloneItemSave.pack.InvList[1] = reloadResp.pack.InvBody[4]; // Clone equipped club into inventory
		cloneItemSave.pack._pNumInv = 2;
		HeroSaveResponsePayload cloneItemResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_SAVE_REQ, &cloneItemSave, sizeof(cloneItemSave),
		        RealmOpcode::OP_HERO_SAVE_RESP, cloneItemResp)
		    || cloneItemResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_SAVE_AUDIT_FAILED)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Cloned item in OP_HERO_SAVE_REQ was not rejected with ERR_SAVE_AUDIT_FAILED\n");
			return 1;
		}

		// 6D. Adversarial Exploit Test: Barbarian (maxMag=0) forged Teleport spell level injection -> must fail
		HeroSaveRequestPayload barbSpellHack {};
		barbSpellHack.charId = 1;
		barbSpellHack.pack = reloadResp.pack;
		barbSpellHack.pack.pSplLvl[static_cast<int>(SpellID::Teleport)] = 5;
		HeroSaveResponsePayload barbSpellResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_SAVE_REQ, &barbSpellHack, sizeof(barbSpellHack),
		        RealmOpcode::OP_HERO_SAVE_RESP, barbSpellResp)
		    || barbSpellResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_SAVE_AUDIT_FAILED)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Forged Barbarian Teleport spell level was not rejected!\n");
			return 1;
		}

		// 6E. Adversarial Exploit Test: Drop + Save Rollback Dupe (drop starting buckler on ground, then try to save pack still holding it)
		ItemDropRequestPayload dropBucklerReq {};
		dropBucklerReq.gameId = activeGameId;
		dropBucklerReq.currlevel = 1;
		dropBucklerReq.tileX = 30;
		dropBucklerReq.tileY = 30;
		dropBucklerReq.item = reloadResp.pack.InvBody[5]; // Equipped starting buckler
		ItemDropResponsePayload dropBucklerResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_ITEM_DROP_REQ, &dropBucklerReq, sizeof(dropBucklerReq),
		        RealmOpcode::OP_ITEM_DROP_RESP, dropBucklerResp)
		    || dropBucklerResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Legitimate OP_ITEM_DROP_REQ failed\n");
			return 1;
		}

		// Now try to save reloadResp.pack which STILL has InvBody[5] equipped while it's sitting on the ground!
		HeroSaveRequestPayload rollbackDupeSave {};
		rollbackDupeSave.charId = 1;
		rollbackDupeSave.pack = reloadResp.pack;
		HeroSaveResponsePayload rollbackDupeResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_HERO_SAVE_REQ, &rollbackDupeSave, sizeof(rollbackDupeSave),
		        RealmOpcode::OP_HERO_SAVE_RESP, rollbackDupeResp)
		    || rollbackDupeResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_SAVE_AUDIT_FAILED)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Drop + Save Rollback Dupe was not rejected!\n");
			return 1;
		}

		// 6F. Adversarial Exploit Test: Stash item cloning (2 copies of starting club in Stash vs 1 in ledger) -> must fail
		StashDataHeader cloneStashHdr {};
		cloneStashHdr.itemCount = 2;
		cloneStashHdr.stashGold = 0U;
		PkItemStruct clonedStashItems[2] = { reloadResp.pack.InvBody[4], reloadResp.pack.InvBody[4] };
		std::vector<uint8_t> cloneStashPayload(sizeof(StashDataHeader) + sizeof(clonedStashItems));
		std::memcpy(cloneStashPayload.data(), &cloneStashHdr, sizeof(StashDataHeader));
		std::memcpy(cloneStashPayload.data() + sizeof(StashDataHeader), clonedStashItems, sizeof(clonedStashItems));
		StashSaveResponsePayload cloneStashResp {};
		if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
		        RealmOpcode::OP_STASH_SAVE_REQ, cloneStashPayload.data(), cloneStashPayload.size(),
		        RealmOpcode::OP_STASH_SAVE_RESP, cloneStashResp)
		    || cloneStashResp.statusCode != static_cast<uint16_t>(RealmStatus::ERR_SAVE_AUDIT_FAILED)) {
			std::fprintf(stderr, "[SelfTest] FAIL: Cloned item in OP_STASH_SAVE_REQ was not rejected!\n");
			return 1;
		}

		std::printf("[SelfTest] [Phase 6] Character Save XP Conservation, Drop+Save Rollback Dupe & Stash Anti-Clone checks verified.\n");

		// =========================================================================
		// PHASE 7: Embedded C++20 Admin HTTP Control Interface Verification
		// =========================================================================
		RealmAdminServer adminServer(db, server);
		std::string adminErr;
		const std::string testAdminToken = "DXER_SELFTEST_ADMIN_SECRET_2026";
		if (!adminServer.Start("127.0.0.1", 0, testAdminToken, adminErr)) {
			std::fprintf(stderr, "[SelfTest] FAIL: RealmAdminServer::Start failed: %s\n", adminErr.c_str());
			return 1;
		}
		const uint16_t adminPort = adminServer.BoundPort();

		auto sendAdminHttp = [&](const std::string &httpReq) -> std::string {
			const SocketHandle s = ConnectLoopbackClient(adminPort);
			if (s == InvalidSocketHandle)
				return {};
			SendAllBlocking(s, reinterpret_cast<const uint8_t *>(httpReq.data()), httpReq.size());
			for (int i = 0; i < 4; ++i) {
				adminServer.PollOnce(5);
			}
			char buf[8192] {};
#ifdef _WIN32
			const int n = recv(static_cast<SOCKET>(s), buf, sizeof(buf) - 1, 0);
#else
			const ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
#endif
			CloseTestSocket(s);
			return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string {};
		};

		// 7A. Unauthenticated /api/admin/status must return HTTP 401
		const std::string unauthHttpResp = sendAdminHttp("GET /api/admin/status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
		if (unauthHttpResp.find("401 Unauthorized") == std::string::npos) {
			std::fprintf(stderr, "[SelfTest] FAIL: Unauthenticated admin request was not rejected with 401\n");
			return 1;
		}

		// 7B. Authenticated Bearer token /api/admin/status & /api/admin/ban
		const std::string authStatusResp = sendAdminHttp(
		    "GET /api/admin/status HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + testAdminToken + "\r\n\r\n");
		if (authStatusResp.find("200 OK") == std::string::npos
		    || authStatusResp.find("\"activeGames\":1") == std::string::npos) {
			std::fprintf(stderr, "[SelfTest] FAIL: Authenticated /api/admin/status response mismatch\n");
			return 1;
		}

		// 7D. Admin Torment Promotion & Single-Player/Multiplayer Character Import (`CF_PREGEN` conversion + `item_ledger` registration)
		const std::string promoteBody = R"({"accountId":1,"charId":1,"level":40,"diabloKillLevel":3})";
		const std::string promoteResp = sendAdminHttp(
		    "POST /api/admin/promote-torment HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + testAdminToken
		    + "\r\nContent-Length: " + std::to_string(promoteBody.size()) + "\r\n\r\n" + promoteBody);
		if (promoteResp.find("200 OK") == std::string::npos) {
			std::fprintf(stderr, "[SelfTest] FAIL: Admin HTTP /api/admin/promote-torment failed: %s\n", promoteResp.c_str());
			return 1;
		}

		// Build a Single Player (`CF_PREGEN`) Level 38 Warrior pack and import via `/api/admin/import-hero`
		PlayerPack spWarriorPack = BuildInitialPlayerPack("SP_Warlord", HeroClass::Warrior, true);
		spWarriorPack.pLevel = 38;
		spWarriorPack.pDiabloKillLevel = 3;
		// Simulate Single Player `CF_PREGEN` (`0x8000`) flag on a dungeon item in InvList[0]
		{
			LcgEngine spLcg;
			ServerPlayerContext spCtx = ServerPlayerContext::FromPlayerPack(spWarriorPack);
			ServerItem spSword {};
			SetupAllItems(spLcg, spCtx, spSword, static_cast<_item_indexes>(1), 0x13572468U, 20, 1, true, false, true); // pregen=true sets CF_PREGEN!
			PackServerItem(spWarriorPack.InvList[0], spSword, true);
		}
		char spPackHex[sizeof(PlayerPack) * 2 + 1] {};
		sodium_bin2hex(spPackHex, sizeof(spPackHex), reinterpret_cast<const unsigned char *>(&spWarriorPack), sizeof(PlayerPack));
		const std::string importHeroBody = std::string("{\"accountId\":1,\"packHex\":\"") + spPackHex + "\"}";
		const std::string importHeroResp = sendAdminHttp(
		    "POST /api/admin/import-hero HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + testAdminToken
		    + "\r\nContent-Length: " + std::to_string(importHeroBody.size()) + "\r\n\r\n" + importHeroBody);
		if (importHeroResp.find("200 OK") == std::string::npos || importHeroResp.find("LedgerItems=5") == std::string::npos || db.CountLedgerItemsForCharacter(3) < 5) {
			std::fprintf(stderr, "[SelfTest] FAIL: Admin Single-Player Hero Import (/api/admin/import-hero) failed: %s\n", importHeroResp.c_str());
			return 1;
		}

		// 7E. Static Lobby Chat Channels (#General, #Trade, #LFG, #Torment), Whispers & Game Search Filtering
		{
			ChatJoinChannelRequestPayload joinTradeReq {};
			joinTradeReq.charId = 3; // Imported SP_Warlord (Lvl 38, DiabloKillLevel 3)
			joinTradeReq.channelId = static_cast<uint8_t>(RealmChatChannel::Trade);
			ChatChannelFeedResponsePayload joinTradeResp {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_CHAT_JOIN_CHANNEL_REQ, &joinTradeReq, sizeof(joinTradeReq),
			        RealmOpcode::OP_CHAT_JOIN_CHANNEL_RESP, joinTradeResp)
			    || joinTradeResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)
			    || joinTradeResp.messageCount == 0) {
				std::fprintf(stderr, "[SelfTest] FAIL: OP_CHAT_JOIN_CHANNEL_REQ (#Trade) failed\n");
				return 1;
			}

			ChatSendRequestPayload tradePostReq {};
			tradePostReq.channelId = static_cast<uint8_t>(RealmChatChannel::Trade);
			std::snprintf(tradePostReq.text, sizeof(tradePostReq.text), "WTS King's Sword of Haste - pst!");
			ChatSendResponsePayload tradePostResp {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_CHAT_SEND_REQ, &tradePostReq, sizeof(tradePostReq),
			        RealmOpcode::OP_CHAT_SEND_RESP, tradePostResp)
			    || tradePostResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)) {
				std::fprintf(stderr, "[SelfTest] FAIL: OP_CHAT_SEND_REQ (#Trade) failed\n");
				return 1;
			}

			// Verify server-stamped sender identity (`SP_Warlord`, Lvl 38, Torment Crown) & `/games` slash command
			ChatSendRequestPayload slashGamesReq {};
			slashGamesReq.channelId = static_cast<uint8_t>(RealmChatChannel::Trade);
			std::snprintf(slashGamesReq.text, sizeof(slashGamesReq.text), "/games Sanctum");
			ChatSendResponsePayload slashGamesResp {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_CHAT_SEND_REQ, &slashGamesReq, sizeof(slashGamesReq),
			        RealmOpcode::OP_CHAT_SEND_RESP, slashGamesResp)
			    || slashGamesResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)) {
				std::fprintf(stderr, "[SelfTest] FAIL: OP_CHAT_SEND_REQ (/games) failed\n");
				return 1;
			}

			ChatPollRequestPayload pollTradeReq {};
			pollTradeReq.channelId = static_cast<uint8_t>(RealmChatChannel::Trade);
			pollTradeReq.afterMessageId = joinTradeResp.latestMessageId;
			ChatChannelFeedResponsePayload pollTradeResp {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_CHAT_POLL_REQ, &pollTradeReq, sizeof(pollTradeReq),
			        RealmOpcode::OP_CHAT_POLL_RESP, pollTradeResp)
			    || pollTradeResp.statusCode != static_cast<uint16_t>(RealmStatus::OK)
			    || pollTradeResp.messageCount < 2
			    || std::string(pollTradeResp.messages[0].senderName) != "SP_Warlord"
			    || pollTradeResp.messages[0].senderLevel != 38
			    || (pollTradeResp.messages[0].flags & CHAT_FLAG_TORMENT_CROWN) == 0) {
				std::fprintf(stderr, "[SelfTest] FAIL: OP_CHAT_POLL_REQ verification failed\n");
				return 1;
			}

			// Verify filtered Game Search (`GameListFilterRequestPayload`)
			GameListFilterRequestPayload searchMatch {};
			std::snprintf(searchMatch.searchQuery, sizeof(searchMatch.searchQuery), "cathedral");
			searchMatch.difficultyFilter = DIFF_NORMAL;
			searchMatch.hideFullGames = 1;
			searchMatch.hidePasswordedGames = 1;

			GameListResponsePayload matchListResp {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_GAME_LIST_REQ, &searchMatch, sizeof(searchMatch),
			        RealmOpcode::OP_GAME_LIST_RESP, matchListResp)
			    || matchListResp.gameCount != 1) {
				std::fprintf(stderr, "[SelfTest] FAIL: Filtered OP_GAME_LIST_REQ (matching query) expected 1 game, got %u\n", matchListResp.gameCount);
				return 1;
			}

			GameListFilterRequestPayload searchMiss {};
			std::snprintf(searchMiss.searchQuery, sizeof(searchMiss.searchQuery), "nonexistent");
			searchMiss.difficultyFilter = 0xFF;
			GameListResponsePayload missListResp {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_GAME_LIST_REQ, &searchMiss, sizeof(searchMiss),
			        RealmOpcode::OP_GAME_LIST_RESP, missListResp)
			    || missListResp.gameCount != 0) {
				std::fprintf(stderr, "[SelfTest] FAIL: Filtered OP_GAME_LIST_REQ (non-matching query) expected 0 games, got %u\n", missListResp.gameCount);
				return 1;
			}
			std::printf("[SelfTest] [Phase 7E] Lobby Chat Channels (#General/#Trade/#LFG/#Torment), Whispers (/w) & Game Search verified.\n");
		}

		// 7F. Eternity Warden Runtime Memory-Table Challenge, DLL/Breakpoint Flag & Speedhack Clock-Skew Verification
		{
			// 1. Valid Warden challenge-response
			WardenChallengePayload wChallenge {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_WARDEN_CHALLENGE_REQ, nullptr, 0U,
			        RealmOpcode::OP_WARDEN_CHALLENGE_REQ, wChallenge)
			    || wChallenge.challengeId == 0) {
				std::fprintf(stderr, "[SelfTest] FAIL: OP_WARDEN_CHALLENGE_REQ failed\n");
				return 1;
			}

			std::array<uint8_t, 32> validDigest {};
			if (!ComputeWardenSliceDigest(wChallenge.tableId, wChallenge.sliceIndex, wChallenge.wardenNonce, validDigest)) {
				std::fprintf(stderr, "[SelfTest] FAIL: ComputeWardenSliceDigest failed\n");
				return 1;
			}

			WardenResponsePayload wResp {};
			wResp.challengeId = wChallenge.challengeId;
			wResp.clientMonotonicMs = 100000ULL;
			wResp.anomalyFlags = WARDEN_FLAG_CLEAN;
			std::memcpy(wResp.sliceBlake2b, validDigest.data(), validDigest.size());

			WardenVerdictPayload wVerdict {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_WARDEN_CHALLENGE_RESP, &wResp, sizeof(wResp),
			        RealmOpcode::OP_WARDEN_VERDICT_RESP, wVerdict)
			    || wVerdict.statusCode != static_cast<uint16_t>(RealmStatus::OK)) {
				std::fprintf(stderr, "[SelfTest] FAIL: Valid OP_WARDEN_CHALLENGE_RESP rejected with status=%u\n", wVerdict.statusCode);
				return 1;
			}

			// 2. Tampered live memory-table slice (Cheat Engine RAM edit to AllItemsList/UniqueItems/SpellsData/MonstersData)
			WardenChallengePayload wChallengeTamper {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_WARDEN_CHALLENGE_REQ, nullptr, 0U,
			        RealmOpcode::OP_WARDEN_CHALLENGE_REQ, wChallengeTamper)) {
				std::fprintf(stderr, "[SelfTest] FAIL: Second OP_WARDEN_CHALLENGE_REQ failed\n");
				return 1;
			}
			std::array<uint8_t, 32> tamperDigest {};
			ComputeWardenSliceDigest(wChallengeTamper.tableId, wChallengeTamper.sliceIndex, wChallengeTamper.wardenNonce, tamperDigest);
			tamperDigest[0] ^= 0xFFU; // Simulate 1-byte Cheat Engine RAM modification

			WardenResponsePayload wRespTamper {};
			wRespTamper.challengeId = wChallengeTamper.challengeId;
			wRespTamper.clientMonotonicMs = 100050ULL;
			wRespTamper.anomalyFlags = WARDEN_FLAG_CLEAN;
			std::memcpy(wRespTamper.sliceBlake2b, tamperDigest.data(), tamperDigest.size());

			WardenVerdictPayload wVerdictTamper {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_WARDEN_CHALLENGE_RESP, &wRespTamper, sizeof(wRespTamper),
			        RealmOpcode::OP_WARDEN_VERDICT_RESP, wVerdictTamper)
			    || wVerdictTamper.statusCode != static_cast<uint16_t>(RealmStatus::ERR_WARDEN_INTEGRITY_VIOLATION)) {
				std::fprintf(stderr, "[SelfTest] FAIL: Tampered Warden slice was not rejected with ERR_WARDEN_INTEGRITY_VIOLATION (got %u)\n",
				    wVerdictTamper.statusCode);
				return 1;
			}

			// 3. Suspicious DLL / Hardware Breakpoint (`DR0..DR3`) / Rolling-XOR Canary violation
			WardenChallengePayload wChallengeDll {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_WARDEN_CHALLENGE_REQ, nullptr, 0U,
			        RealmOpcode::OP_WARDEN_CHALLENGE_REQ, wChallengeDll)) {
				std::fprintf(stderr, "[SelfTest] FAIL: Third OP_WARDEN_CHALLENGE_REQ failed\n");
				return 1;
			}
			std::array<uint8_t, 32> dllDigest {};
			ComputeWardenSliceDigest(wChallengeDll.tableId, wChallengeDll.sliceIndex, wChallengeDll.wardenNonce, dllDigest);

			WardenResponsePayload wRespDll {};
			wRespDll.challengeId = wChallengeDll.challengeId;
			wRespDll.clientMonotonicMs = 100100ULL;
			wRespDll.anomalyFlags = WARDEN_FLAG_SUSPICIOUS_DLL | WARDEN_FLAG_HARDWARE_BREAKPOINT;
			std::memcpy(wRespDll.sliceBlake2b, dllDigest.data(), dllDigest.size());

			WardenVerdictPayload wVerdictDll {};
			if (!ExchangeRpc(gameSock, server, clientSeq, serverSeq, sessionTrafficKey,
			        RealmOpcode::OP_WARDEN_CHALLENGE_RESP, &wRespDll, sizeof(wRespDll),
			        RealmOpcode::OP_WARDEN_VERDICT_RESP, wVerdictDll)
			    || wVerdictDll.statusCode != static_cast<uint16_t>(RealmStatus::ERR_WARDEN_INTEGRITY_VIOLATION)) {
				std::fprintf(stderr, "[SelfTest] FAIL: CheatEngine DLL / HW breakpoint flag was not rejected (got %u)\n",
				    wVerdictDll.statusCode);
				return 1;
			}

			// 4. Speedhack 2.0x Clock-Skew Detection (`10000ms` client elapsed vs `5000ms` server elapsed -> 2000 permille)
			WardenResponsePayload wRespSpeed {};
			wRespSpeed.challengeId = wChallengeDll.challengeId;
			wRespSpeed.clientMonotonicMs = 110000ULL;
			wRespSpeed.anomalyFlags = WARDEN_FLAG_CLEAN;
			std::memcpy(wRespSpeed.sliceBlake2b, dllDigest.data(), dllDigest.size());

			uint32_t driftPermille = 0;
			std::string speedReason;
			const RealmStatus speedStatus = VerifyWardenResponse(
			    wChallengeDll,
			    10000ULL,  // baselineServerMs
			    100000ULL, // baselineClientMs
			    15000ULL,  // nowServerMs (+5000ms wall-clock)
			    wRespSpeed,
			    driftPermille,
			    speedReason);
			if (speedStatus != RealmStatus::ERR_WARDEN_SPEEDHACK_DETECTED || driftPermille != 2000U) {
				std::fprintf(stderr, "[SelfTest] FAIL: Speedhack 2.0x clock skew was not rejected with ERR_WARDEN_SPEEDHACK_DETECTED (status=%u, drift=%u)\n",
				    static_cast<unsigned>(speedStatus), driftPermille);
				return 1;
			}

			// 5. High-Ping Coalesced Walk-Kill Burst vs. Cross-Map Teleport Hack Verification
			AuthoritativeCombatCeiling warriorCeil {};
			warriorCeil.effectiveMaxSingleHit = 100;
			warriorCeil.minAttackIntervalMs = 150;
			warriorCeil.maxTilesPerSecond = 12;
			warriorCeil.hasTeleportAccess = false;

			EffectiveMonsterStats zombStats {};
			zombStats.minHp = 10;
			zombStats.maxHp = 15;

			GamePlayerRuntimeState highPingPlayer {};
			highPingPlayer.highPingJitterMs = 650U; // High-ping WAN tolerance active
			highPingPlayer.lastTileX = 20;
			highPingPlayer.lastTileY = 20;
			highPingPlayer.lastKillTimestampMs = 50000ULL;

			// 29-tile ranged+walk kill arriving only 10ms after previous packet due to TCP coalescing:
			// maxReachableTiles = 24 + ((10 + 650) * 12) / 1000 = 31 tiles -> MUST PASS in High-Ping mode!
			MonsterDropRequestPayload coalescedKill {};
			coalescedKill.tileX = 49;
			coalescedKill.tileY = 20;
			coalescedKill.totalDamageDealt = 20;
			coalescedKill.maxSingleHitDealt = 20;
			coalescedKill.combatDurationMs = 500;
			std::string cadenceReason;
			if (AuditMonsterKillAnomaly(warriorCeil, zombStats, coalescedKill, 50010ULL, highPingPlayer, cadenceReason) != RealmStatus::OK) {
				std::fprintf(stderr, "[SelfTest] FAIL: High-ping coalesced 29-tile kill burst was falsely rejected: %s\n", cadenceReason.c_str());
				return 1;
			}

			// 70-tile cross-map teleport kill without Teleport spell -> MUST STILL BE REJECTED!
			MonsterDropRequestPayload teleportCheatKill = coalescedKill;
			teleportCheatKill.tileX = 90;
			teleportCheatKill.tileY = 90;
			if (AuditMonsterKillAnomaly(warriorCeil, zombStats, teleportCheatKill, 50020ULL, highPingPlayer, cadenceReason)
			    != RealmStatus::ERR_ANOMALY_CADENCE_VIOLATION) {
				std::fprintf(stderr, "[SelfTest] FAIL: 70-tile teleport hack was not rejected in High-Ping mode\n");
				return 1;
			}
			std::printf("[SelfTest] [Phase 7F] Eternity Warden & High-Ping Coalesced Burst Tolerance verified.\n");
		}

		const std::string banBody = R"({"accountId":1,"reason":"Admin HTTP self-test ban"})";
		const std::string banHttpResp = sendAdminHttp(
		    "POST /api/admin/ban HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + testAdminToken
		    + "\r\nContent-Length: " + std::to_string(banBody.size()) + "\r\n\r\n" + banBody);
		const auto bannedAcc = db.FindAccountById(1);
		if (banHttpResp.find("200 OK") == std::string::npos || !bannedAcc.has_value() || !bannedAcc->isBanned) {
			std::fprintf(stderr, "[SelfTest] FAIL: Admin HTTP /api/admin/ban failed\n");
			return 1;
		}

#ifndef _WIN32
		// 7C. Linux Semi-Automatic Self-Updater Verification (when not running inside child pre-flight self-test)
		if (std::getenv("DXER_UPDATER_CHILD_SELFTEST") == nullptr) {
			const std::string currentExe = ResolveCurrentExecutablePath(nullptr);
			const std::string tempUpdateTarget = currentExe + ".updtest";
			// Stage a copy of the running binary as the target to be updated in-place
			{
				const SelfUpdateResult prep = StageAndApplyLinuxSelfUpdate("file://" + currentExe, tempUpdateTarget);
				if (!prep.ok) {
					std::fprintf(stderr, "[SelfTest] FAIL: Linux self-updater StageAndApplyLinuxSelfUpdate failed: %s\n", prep.message.c_str());
					return 1;
				}
			}
			adminServer.SetExecutablePath(tempUpdateTarget);
			const std::string updBody = "{\"source\":\"file://" + currentExe + "\",\"force\":true}";
			const std::string updHttpResp = sendAdminHttp(
			    "POST /api/admin/update HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + testAdminToken
			    + "\r\nContent-Length: " + std::to_string(updBody.size()) + "\r\n\r\n" + updBody);
			unlink(tempUpdateTarget.c_str());
			unlink((tempUpdateTarget + ".bak").c_str());
			if (updHttpResp.find("200 OK") == std::string::npos || !adminServer.ShouldRestartForUpdate()) {
				std::fprintf(stderr, "[SelfTest] FAIL: Admin HTTP /api/admin/update failed: %s\n", updHttpResp.c_str());
				return 1;
			}
		}
#endif

		adminServer.Stop();
		std::printf("[SelfTest] [Phase 7] Embedded C++20 Admin HTTP Console, SP/MP Hero Importer & Linux Self-Updater (port=%u) verified.\n",
		    adminPort);

		CloseTestSocket(gameSock);
	}

	server.Stop();
	std::printf("[SelfTest] PASS: All Phase 1, Phase 2, Phase 3, Phase 4, Phase 5, Phase 6 & Phase 7 checks succeeded! (security_audit_log rows=%lld)\n",
	    static_cast<long long>(db.GetSecurityAuditCount()));
	return 0;
}

} // namespace
} // namespace devilution::realm

int main(int argc, char *argv[])
{
	if (sodium_init() < 0) {
		std::fprintf(stderr, "FATAL: libsodium initialization failed!\n");
		return 1;
	}

	uint16_t port = devilution::realm::DefaultRealmPort;
	uint16_t adminPort = devilution::realm::DefaultAdminPort;
	std::string bindIp = "0.0.0.0";
	std::string adminBindIp = "127.0.0.1";
	std::string adminToken;
	std::string dbPath = "dx-eternity-realm.db";
	std::string logPath = "dx-eternity-realm.log";
	std::string updateSource;
	bool highPingTolerance = true;
	bool runUpdateCli = false;
	bool selfTest = false;

	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--self-test") == 0) {
			selfTest = true;
		} else if (std::strcmp(argv[i], "--high-ping-tolerance") == 0) {
			highPingTolerance = true;
		} else if (std::strcmp(argv[i], "--strict-lan-cadence") == 0) {
			highPingTolerance = false;
		} else if (std::strcmp(argv[i], "--update") == 0) {
			runUpdateCli = true;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				updateSource = argv[++i];
			} else {
				updateSource = devilution::realm::DefaultGitHubRepo;
			}
		} else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
			port = static_cast<uint16_t>(std::atoi(argv[++i]));
		} else if (std::strcmp(argv[i], "--admin-port") == 0 && i + 1 < argc) {
			adminPort = static_cast<uint16_t>(std::atoi(argv[++i]));
		} else if (std::strcmp(argv[i], "--admin-bind") == 0 && i + 1 < argc) {
			adminBindIp = argv[++i];
		} else if (std::strcmp(argv[i], "--admin-token") == 0 && i + 1 < argc) {
			adminToken = argv[++i];
		} else if (std::strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
			bindIp = argv[++i];
		} else if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
			dbPath = argv[++i];
		} else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
			logPath = argv[++i];
		}
	}

	if (selfTest) {
		return devilution::realm::RunSelfTest();
	}

	const std::string resolvedExePath = devilution::realm::ResolveCurrentExecutablePath(argv[0]);
	if (runUpdateCli) {
		std::printf("[Updater] Staging and verifying Linux self-update from '%s' -> '%s'...\n",
		    updateSource.c_str(), resolvedExePath.c_str());
		const devilution::realm::SelfUpdateResult res = devilution::realm::StageAndApplyLinuxSelfUpdate(
		    updateSource, resolvedExePath);
		if (!res.ok) {
			std::fprintf(stderr, "[Updater] FAIL: %s\n", res.message.c_str());
			return 1;
		}
		std::printf("[Updater] SUCCESS: %s\n", res.message.c_str());
		return 0;
	}

	devilution::realm::RealmDatabase db;
	db.SetLogFilePath(logPath);
	std::string error;
	if (!db.Open(dbPath, error)) {
		std::fprintf(stderr, "FATAL: Failed to open realm database '%s': %s\n", dbPath.c_str(), error.c_str());
		return 1;
	}

	devilution::realm::RealmServer server(db);
	server.SessionManager().SetHighPingTolerance(highPingTolerance);
	if (!server.Start(bindIp, port, error)) {
		std::fprintf(stderr, "FATAL: Failed to start realm server on %s:%u: %s\n", bindIp.c_str(), port, error.c_str());
		return 1;
	}

	devilution::realm::RealmAdminServer adminServer(db, server);
	adminServer.SetExecutablePath(resolvedExePath);
	if (!adminServer.Start(adminBindIp, adminPort, adminToken, error)) {
		std::fprintf(stderr, "WARN: Admin HTTP listener could not bind %s:%u (%s)\n", adminBindIp.c_str(), adminPort, error.c_str());
	} else {
		std::printf("DX:Eternity Admin HTTP Console listening on http://%s:%u (token: %s)\n",
		    adminBindIp.c_str(), adminServer.BoundPort(), adminServer.AdminToken().c_str());
	}

	db.LogSecurityEvent(0, 0, 0, "INFO", "SERVER_START",
	    "DX:Eternity Closed Realm Server listening on " + bindIp + ":" + std::to_string(server.BoundPort())
	        + " (db=" + dbPath + ", log=" + logPath + ")");

	std::printf("DX:Eternity Closed Realm Server listening on %s:%u (db: %s, log: %s)\n",
	    bindIp.c_str(), server.BoundPort(), dbPath.c_str(), logPath.c_str());

	while (true) {
		server.PollOnce(20);
		adminServer.PollOnce(2);
		if (adminServer.ShouldRestartForUpdate()) {
			db.LogSecurityEvent(0, 0, 0, "INFO", "SERVER_EXEC_RESTART",
			    "Restarting in-place via execv(" + resolvedExePath + ") after verified self-update");
			adminServer.Stop();
			server.Stop();
			db.Close();
			devilution::realm::ExecRestartSelf(resolvedExePath, argv);
		}
	}
	return 0;
}
