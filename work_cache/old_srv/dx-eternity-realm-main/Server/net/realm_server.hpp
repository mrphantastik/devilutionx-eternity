/**
 * @file realm_server.hpp
 * @brief Non-blocking TCP server and client session multiplexer for dx-eternity-realm.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "auth/realm_auth.hpp"
#include "db/realm_database.hpp"
#include "net/packet_codec.hpp"
#include "protocol/realm_protocol.hpp"
#include "session/realm_session.hpp"
#include "vault/realm_vault.hpp"

namespace devilution::realm {

#ifdef _WIN32
using SocketHandle = uintptr_t;
constexpr SocketHandle InvalidSocketHandle = ~static_cast<uintptr_t>(0);
#else
using SocketHandle = int;
constexpr SocketHandle InvalidSocketHandle = -1;
#endif

struct ClientSession {
	uint64_t sessionId = 0;
	SocketHandle socket = InvalidSocketHandle;
	std::string remoteIp;
	uint16_t remotePort = 0;

	SessionKey trafficKey = GetDefaultHandshakeKey();
	std::array<uint8_t, 32> challengeNonce {};
	bool challengeIssued = false;
	bool attested = false;
	bool authenticated = false;

	int64_t accountId = 0;
	int64_t activeCharId = 0;
	uint64_t activeGameId = 0;
	uint8_t activeChannel = static_cast<uint8_t>(RealmChatChannel::General);

	uint32_t lastClientSeq = 0;
	uint32_t nextServerSeq = 1;
	uint32_t authAttempts = 0;
	uint64_t connectedAtMs = 0;
	uint64_t lastActivityMs = 0;
	uint32_t packetsInCurrentWindow = 0;
	uint64_t rateWindowStartMs = 0;
	uint32_t chatMessagesInWindow = 0;
	uint64_t chatWindowStartMs = 0;

	WardenChallengePayload activeWardenChallenge {};
	bool wardenChallengePending = false;
	uint64_t wardenIssuedAtMs = 0;
	uint64_t lastWardenCompletedAtMs = 0;
	uint64_t wardenBaselineServerMs = 0;
	uint64_t wardenBaselineClientMs = 0;
	uint64_t nextWardenChallengeId = 1;

	std::vector<uint8_t> recvBuffer;
	std::vector<uint8_t> sendBuffer;
};

class RealmServer {
public:
	explicit RealmServer(RealmDatabase &db);
	~RealmServer();

	RealmServer(const RealmServer &) = delete;
	RealmServer &operator=(const RealmServer &) = delete;

	/**
	 * @brief Binds and listens on `bindIp:port` (pass `port = 0` to bind an ephemeral port).
	 */
	bool Start(const std::string &bindIp, uint16_t port, std::string &outError);

	/**
	 * @brief Runs one non-blocking event-loop tick (`timeoutMs` poll wait).
	 */
	void PollOnce(int timeoutMs);

	/**
	 * @brief Stops the listener and closes all active client sessions.
	 */
	void Stop();

	uint16_t BoundPort() const
	{
		return boundPort_;
	}

	size_t ActiveSessionCount() const
	{
		return sessions_.size();
	}

	uint64_t TotalPacketsProcessed() const
	{
		return totalPacketsProcessed_;
	}

	RealmAuthManager &AuthManager()
	{
		return auth_;
	}

	const RealmAuthManager &AuthManager() const
	{
		return auth_;
	}

	RealmVaultManager &VaultManager()
	{
		return vault_;
	}

	const RealmVaultManager &VaultManager() const
	{
		return vault_;
	}

	RealmSessionManager &SessionManager()
	{
		return sessionManager_;
	}

	const RealmSessionManager &SessionManager() const
	{
		return sessionManager_;
	}

	/**
	 * @brief Immediately disconnects all active TCP sessions belonging to `accountId` (e.g., on admin ban).
	 */
	size_t DisconnectAccountSessions(int64_t accountId);

	/**
	 * @brief Counts authenticated sessions currently tuned to `channelId` (0..3).
	 */
	uint16_t CountSessionsInChannel(uint8_t channelId) const;

private:
	void AcceptPendingConnections();
	bool ProcessClientRead(ClientSession &session);
	bool FlushClientWrite(ClientSession &session);
	bool DispatchPacket(ClientSession &session, const RealmPacketHeader &hdr, const uint8_t *payload, size_t payloadLen);
	void QueuePacket(ClientSession &session, RealmOpcode opcode, const void *payload, size_t payloadLen);
	void QueueError(ClientSession &session, RealmOpcode reqOpcode, RealmStatus status, const char *message);
	void CloseSession(SocketHandle sock);

	RealmDatabase &db_;
	RealmAuthManager auth_;
	RealmVaultManager vault_;
	RealmSessionManager sessionManager_;
	SocketHandle listenSocket_ = InvalidSocketHandle;
	uint16_t boundPort_ = 0;
	uint64_t nextSessionId_ = 1;
	uint64_t totalPacketsProcessed_ = 0;
	std::unordered_map<SocketHandle, ClientSession> sessions_;
};

} // namespace devilution::realm
