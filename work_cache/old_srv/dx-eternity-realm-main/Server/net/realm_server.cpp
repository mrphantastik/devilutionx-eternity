/**
 * @file realm_server.cpp
 * @brief Cross-platform (Linux POSIX & Windows Winsock2) non-blocking TCP server for dx-eternity-realm.
 */
#include "net/realm_server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sodium.h>

#include "anticheat/anomaly_detector.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace devilution::realm {

namespace {

constexpr size_t MaxRecvBufferBytes = 131072U; // 128 KB max per-session receive buffer
constexpr size_t MaxSendBufferBytes = 262144U; // 256 KB max per-session send buffer
constexpr size_t MaxConcurrentSessions = 60U;  // Safe cross-platform cap for Winsock select() / POSIX poll()
constexpr uint32_t MaxPacketsPerSecond = 120U; // Rate limit per session
constexpr uint32_t MaxAuthAttemptsPerSession = 3U; // Max Argon2id attempts per TCP connection
constexpr uint64_t PreAuthTimeoutMs = 10000ULL;    // 10 seconds max to complete authentication
constexpr uint64_t IdleSessionTimeoutMs = 180000ULL; // 3 minutes idle timeout for authenticated sessions

uint64_t NowMonotonicMs()
{
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(
	        std::chrono::steady_clock::now().time_since_epoch())
	        .count());
}

void CloseRawSocket(SocketHandle s)
{
	if (s == InvalidSocketHandle)
		return;
#ifdef _WIN32
	closesocket(static_cast<SOCKET>(s));
#else
	close(s);
#endif
}

bool SetSocketNonBlockingAndNoDelay(SocketHandle s)
{
#ifdef _WIN32
	u_long nonBlock = 1;
	if (ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &nonBlock) != 0)
		return false;
	BOOL noDelay = TRUE;
	setsockopt(static_cast<SOCKET>(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));
	return true;
#else
	const int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0)
		return false;
	const int noDelay = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
	return true;
#endif
}

bool IsWouldBlockError()
{
#ifdef _WIN32
	const int err = WSAGetLastError();
	return err == WSAEWOULDBLOCK;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

} // namespace

RealmServer::RealmServer(RealmDatabase &db)
    : db_(db)
    , auth_(db)
    , vault_(db)
    , sessionManager_(db)
{
#ifdef _WIN32
	WSADATA wsaData {};
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

RealmServer::~RealmServer()
{
	Stop();
#ifdef _WIN32
	WSACleanup();
#endif
}

bool RealmServer::Start(const std::string &bindIp, uint16_t port, std::string &outError)
{
	Stop();

#ifdef _WIN32
	const SOCKET rawSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (rawSock == INVALID_SOCKET) {
		outError = "socket() failed";
		return false;
	}
	listenSocket_ = static_cast<SocketHandle>(rawSock);
	BOOL reuse = TRUE;
	setsockopt(rawSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
	const int rawSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (rawSock < 0) {
		outError = "socket() failed";
		return false;
	}
	listenSocket_ = rawSock;
	const int reuse = 1;
	setsockopt(rawSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

	if (!SetSocketNonBlockingAndNoDelay(listenSocket_)) {
		outError = "Failed to set listener non-blocking";
		Stop();
		return false;
	}

	sockaddr_in addr {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (bindIp.empty() || bindIp == "0.0.0.0") {
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	} else if (inet_pton(AF_INET, bindIp.c_str(), &addr.sin_addr) != 1) {
		outError = "Invalid bind IPv4 address: " + bindIp;
		Stop();
		return false;
	}

	if (bind(
#ifdef _WIN32
	        static_cast<SOCKET>(listenSocket_),
#else
	        listenSocket_,
#endif
	        reinterpret_cast<const sockaddr *>(&addr),
	        sizeof(addr))
	    != 0) {
		outError = "bind() failed on port " + std::to_string(port);
		Stop();
		return false;
	}

	if (listen(
#ifdef _WIN32
	        static_cast<SOCKET>(listenSocket_),
#else
	        listenSocket_,
#endif
	        128)
	    != 0) {
		outError = "listen() failed";
		Stop();
		return false;
	}

	sockaddr_in boundAddr {};
#ifdef _WIN32
	int addrLen = sizeof(boundAddr);
	if (getsockname(static_cast<SOCKET>(listenSocket_), reinterpret_cast<sockaddr *>(&boundAddr), &addrLen) == 0) {
		boundPort_ = ntohs(boundAddr.sin_port);
	}
#else
	socklen_t addrLen = sizeof(boundAddr);
	if (getsockname(listenSocket_, reinterpret_cast<sockaddr *>(&boundAddr), &addrLen) == 0) {
		boundPort_ = ntohs(boundAddr.sin_port);
	}
#endif

	return true;
}

void RealmServer::Stop()
{
	for (auto &[sock, session] : sessions_) {
		CloseRawSocket(sock);
	}
	sessions_.clear();

	if (listenSocket_ != InvalidSocketHandle) {
		CloseRawSocket(listenSocket_);
		listenSocket_ = InvalidSocketHandle;
	}
	boundPort_ = 0;
}

void RealmServer::PollOnce(int timeoutMs)
{
	if (listenSocket_ == InvalidSocketHandle)
		return;

#ifdef _WIN32
	fd_set readSet;
	fd_set writeSet;
	FD_ZERO(&readSet);
	FD_ZERO(&writeSet);

	FD_SET(static_cast<SOCKET>(listenSocket_), &readSet);
	for (const auto &[sock, session] : sessions_) {
		FD_SET(static_cast<SOCKET>(sock), &readSet);
		if (!session.sendBuffer.empty()) {
			FD_SET(static_cast<SOCKET>(sock), &writeSet);
		}
	}

	timeval tv {};
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = (timeoutMs % 1000) * 1000;
	const int ready = select(0, &readSet, &writeSet, nullptr, &tv);
	if (ready > 0) {
		if (FD_ISSET(static_cast<SOCKET>(listenSocket_), &readSet)) {
			AcceptPendingConnections();
		}
		std::vector<SocketHandle> activeSockets;
		activeSockets.reserve(sessions_.size());
		for (const auto &[sock, _] : sessions_) {
			activeSockets.push_back(sock);
		}
		for (SocketHandle sock : activeSockets) {
			auto it = sessions_.find(sock);
			if (it == sessions_.end())
				continue;
			if (FD_ISSET(static_cast<SOCKET>(sock), &readSet)) {
				if (!ProcessClientRead(it->second)) {
					CloseSession(sock);
					continue;
				}
			}
			it = sessions_.find(sock);
			if (it != sessions_.end() && !it->second.sendBuffer.empty()) {
				if (!FlushClientWrite(it->second)) {
					CloseSession(sock);
					continue;
				}
			}
		}
	}
#else
	std::vector<pollfd> fds;
	fds.reserve(sessions_.size() + 1);

	pollfd listenPfd {};
	listenPfd.fd = listenSocket_;
	listenPfd.events = POLLIN;
	fds.push_back(listenPfd);

	for (const auto &[sock, session] : sessions_) {
		pollfd pfd {};
		pfd.fd = sock;
		pfd.events = POLLIN | (!session.sendBuffer.empty() ? POLLOUT : 0);
		fds.push_back(pfd);
	}

	const int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
	if (ready > 0) {
		if ((fds[0].revents & POLLIN) != 0) {
			AcceptPendingConnections();
		}
		for (size_t i = 1; i < fds.size(); i++) {
			const SocketHandle sock = fds[i].fd;
			const short revents = fds[i].revents;
			if (revents == 0)
				continue;

			auto it = sessions_.find(sock);
			if (it == sessions_.end())
				continue;

			if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
				CloseSession(sock);
				continue;
			}
			if ((revents & POLLIN) != 0) {
				if (!ProcessClientRead(it->second)) {
					CloseSession(sock);
					continue;
				}
			}
			it = sessions_.find(sock);
			if (it != sessions_.end() && !it->second.sendBuffer.empty()) {
				if (!FlushClientWrite(it->second)) {
					CloseSession(sock);
					continue;
				}
			}
		}
	}
#endif

	// Evict idle sessions (10s pre-auth timeout, 180s authenticated idle timeout)
	const uint64_t nowMs = NowMonotonicMs();
	for (auto it = sessions_.begin(); it != sessions_.end();) {
		const uint64_t timeoutLimit = it->second.authenticated ? IdleSessionTimeoutMs : PreAuthTimeoutMs;
		if (nowMs - it->second.lastActivityMs > timeoutLimit) {
			if (it->second.activeGameId != 0 && it->second.activeCharId != 0) {
				sessionManager_.LeaveGame(it->second.activeGameId, it->second.activeCharId);
			}
			CloseRawSocket(it->first);
			it = sessions_.erase(it);
		} else {
			++it;
		}
	}
}

void RealmServer::AcceptPendingConnections()
{
	while (true) {
		sockaddr_in clientAddr {};
#ifdef _WIN32
		int addrLen = sizeof(clientAddr);
		const SOCKET rawClient = accept(static_cast<SOCKET>(listenSocket_), reinterpret_cast<sockaddr *>(&clientAddr), &addrLen);
		if (rawClient == INVALID_SOCKET)
			break;
		const SocketHandle clientSock = static_cast<SocketHandle>(rawClient);
#else
		socklen_t addrLen = sizeof(clientAddr);
		const int clientSock = accept(listenSocket_, reinterpret_cast<sockaddr *>(&clientAddr), &addrLen);
		if (clientSock < 0)
			break;
#endif

		if (sessions_.size() >= MaxConcurrentSessions) {
			// Evict oldest unauthenticated connection if one exists so syn/socket floods cannot lock out players
			auto oldestUnauth = sessions_.end();
			for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
				if (!it->second.authenticated) {
					if (oldestUnauth == sessions_.end() || it->second.connectedAtMs < oldestUnauth->second.connectedAtMs) {
						oldestUnauth = it;
					}
				}
			}
			if (oldestUnauth != sessions_.end()) {
				CloseRawSocket(oldestUnauth->first);
				sessions_.erase(oldestUnauth);
			}
		}

		if (sessions_.size() >= MaxConcurrentSessions || !SetSocketNonBlockingAndNoDelay(clientSock)) {
			CloseRawSocket(clientSock);
			continue;
		}

		char ipBuf[INET_ADDRSTRLEN] {};
		inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));

		const uint64_t nowMs = NowMonotonicMs();
		ClientSession session {};
		session.sessionId = nextSessionId_++;
		session.socket = clientSock;
		session.remoteIp = ipBuf;
		session.remotePort = ntohs(clientAddr.sin_port);
		session.connectedAtMs = nowMs;
		session.lastActivityMs = nowMs;
		session.rateWindowStartMs = nowMs;
		randombytes_buf(session.challengeNonce.data(), session.challengeNonce.size());

		sessions_.emplace(clientSock, std::move(session));
	}
}

bool RealmServer::ProcessClientRead(ClientSession &session)
{
	uint8_t tempBuf[8192];
	while (true) {
#ifdef _WIN32
		const int n = recv(static_cast<SOCKET>(session.socket), reinterpret_cast<char *>(tempBuf), sizeof(tempBuf), 0);
#else
		const ssize_t n = recv(session.socket, tempBuf, sizeof(tempBuf), 0);
#endif
		if (n > 0) {
			if (session.recvBuffer.size() + static_cast<size_t>(n) > MaxRecvBufferBytes) {
				db_.LogSecurityEvent(session.accountId, session.activeCharId, session.activeGameId,
				    "WARN", "NET_BUFFER_OVERFLOW", "Session exceeded 128 KB receive buffer limit (" + session.remoteIp + ")");
				return false;
			}
			session.recvBuffer.insert(session.recvBuffer.end(), tempBuf, tempBuf + n);
			session.lastActivityMs = NowMonotonicMs();
		} else if (n == 0) {
			return false; // Clean peer EOF
		} else {
			if (IsWouldBlockError())
				break;
			return false;
		}
	}

	// Parse complete frames from session.recvBuffer
	size_t offset = 0;
	while (session.recvBuffer.size() - offset >= sizeof(RealmPacketHeader)) {
		RealmPacketHeader hdr {};
		std::memcpy(&hdr, session.recvBuffer.data() + offset, sizeof(RealmPacketHeader));

		if (hdr.magic != ProtocolMagic || hdr.version != ProtocolVersion || hdr.payloadLength > MaxPayloadSize) {
			db_.LogSecurityEvent(session.accountId, session.activeCharId, session.activeGameId,
			    "WARN", "PROTOCOL_FRAMING_ERROR", "Invalid magic/version/payloadLength from " + session.remoteIp);
			return false;
		}

		const size_t totalFrameBytes = sizeof(RealmPacketHeader) + hdr.payloadLength;
		if (session.recvBuffer.size() - offset < totalFrameBytes) {
			break; // Wait for remaining payload bytes
		}

		const uint8_t *payloadPtr = session.recvBuffer.data() + offset + sizeof(RealmPacketHeader);

		// 1. Rate-limit check
		const uint64_t nowMs = NowMonotonicMs();
		if (nowMs - session.rateWindowStartMs >= 1000ULL) {
			session.rateWindowStartMs = nowMs;
			session.packetsInCurrentWindow = 0;
		}
		if (++session.packetsInCurrentWindow > MaxPacketsPerSecond) {
			db_.LogSecurityEvent(session.accountId, session.activeCharId, session.activeGameId,
			    "WARN", "RATE_LIMIT_EXCEEDED", "Packet rate limit exceeded by " + session.remoteIp);
			return false;
		}

		// 2. Monotonic sequence number anti-replay check
		if (hdr.sequenceNumber <= session.lastClientSeq) {
			db_.LogSecurityEvent(session.accountId, session.activeCharId, session.activeGameId,
			    "ERROR", "REPLAY_DETECTED", "Non-monotonic packet sequence number from " + session.remoteIp);
			QueueError(session, static_cast<RealmOpcode>(hdr.opcode), RealmStatus::ERR_REPLAY_DETECTED, "Replay sequence detected");
			FlushClientWrite(session);
			return false;
		}

		// 3. Constant-time libsodium BLAKE2b-128 MAC check
		if (!VerifyPacketMac(hdr, payloadPtr, hdr.payloadLength, session.trafficKey)) {
			db_.LogSecurityEvent(session.accountId, session.activeCharId, session.activeGameId,
			    "ERROR", "MAC_VERIFICATION_FAILED", "Packet BLAKE2b MAC mismatch from " + session.remoteIp);
			QueueError(session, static_cast<RealmOpcode>(hdr.opcode), RealmStatus::ERR_MAC_VERIFICATION_FAILED, "Packet MAC verification failed");
			FlushClientWrite(session);
			return false;
		}

		session.lastClientSeq = hdr.sequenceNumber;
		totalPacketsProcessed_++;

		if (!DispatchPacket(session, hdr, payloadPtr, hdr.payloadLength)) {
			return false;
		}

		offset += totalFrameBytes;
	}

	if (offset > 0) {
		session.recvBuffer.erase(session.recvBuffer.begin(), session.recvBuffer.begin() + static_cast<std::ptrdiff_t>(offset));
	}

	return FlushClientWrite(session);
}

bool RealmServer::FlushClientWrite(ClientSession &session)
{
	while (!session.sendBuffer.empty()) {
#ifdef _WIN32
		const int n = send(static_cast<SOCKET>(session.socket),
		    reinterpret_cast<const char *>(session.sendBuffer.data()),
		    static_cast<int>(session.sendBuffer.size()), 0);
#else
		const ssize_t n = send(session.socket,
		    session.sendBuffer.data(),
		    session.sendBuffer.size(),
#ifdef MSG_NOSIGNAL
		    MSG_NOSIGNAL
#else
		    0
#endif
		);
#endif
		if (n > 0) {
			session.sendBuffer.erase(session.sendBuffer.begin(), session.sendBuffer.begin() + n);
		} else {
			if (IsWouldBlockError())
				return true;
			return false;
		}
	}
	return true;
}

bool RealmServer::DispatchPacket(ClientSession &session, const RealmPacketHeader &hdr, const uint8_t *payload, size_t payloadLen)
{
	const auto opcode = static_cast<RealmOpcode>(hdr.opcode);
	switch (opcode) {
	case RealmOpcode::OP_PING: {
		if (payloadLen != sizeof(PingPongPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid OP_PING payload size");
			return true;
		}
		PingPongPayload pong {};
		std::memcpy(&pong, payload, sizeof(PingPongPayload));
		pong.serverTimestampMs = NowMonotonicMs();
		QueuePacket(session, RealmOpcode::OP_PONG, &pong, sizeof(pong));
		return true;
	}
	case RealmOpcode::OP_AUTH_CHALLENGE_REQ: {
		if (session.challengeIssued || session.attested) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Challenge already issued on this session");
			FlushClientWrite(session);
			return false;
		}
		session.challengeIssued = true;
		AuthChallengeResponsePayload resp {};
		std::memcpy(resp.serverNonce, session.challengeNonce.data(), sizeof(resp.serverNonce));
		resp.serverProtocolVersion = ProtocolVersion;
		QueuePacket(session, RealmOpcode::OP_AUTH_CHALLENGE_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_ATTEST_HANDSHAKE_REQ: {
		if (payloadLen != sizeof(AttestHandshakeRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid OP_ATTEST_HANDSHAKE_REQ payload size");
			return true;
		}
		if (!session.challengeIssued || session.attested) {
			db_.LogSecurityEvent(session.accountId, session.activeCharId, session.activeGameId,
			    "WARN", "ATTEST_STATE_VIOLATION", "Invalid OP_ATTEST_HANDSHAKE_REQ state from " + session.remoteIp);
			AttestHandshakeResponsePayload resp {};
			resp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_ATTESTATION_FAILED);
			QueuePacket(session, RealmOpcode::OP_ATTEST_HANDSHAKE_RESP, &resp, sizeof(resp));
			FlushClientWrite(session);
			return false;
		}

		AttestHandshakeRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));

		// Reject all-zero clientNonce
		uint8_t zeroNonce[32] {};
		if (sodium_memcmp(req.clientNonce, zeroNonce, sizeof(zeroNonce)) == 0) {
			AttestHandshakeResponsePayload resp {};
			resp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_ATTESTATION_FAILED);
			QueuePacket(session, RealmOpcode::OP_ATTEST_HANDSHAKE_RESP, &resp, sizeof(resp));
			FlushClientWrite(session);
			return false;
		}

		SessionKey rotatedTrafficKey {};
		const RealmStatus status = auth_.VerifyAttestationHandshake(
		    session.challengeNonce.data(), req, session.remoteIp, rotatedTrafficKey);

		AttestHandshakeResponsePayload resp {};
		resp.statusCode = static_cast<uint16_t>(status);
		resp.reserved = 0;
		// Send OP_ATTEST_HANDSHAKE_RESP signed with the initial handshake key
		QueuePacket(session, RealmOpcode::OP_ATTEST_HANDSHAKE_RESP, &resp, sizeof(resp));

		if (status != RealmStatus::OK) {
			FlushClientWrite(session);
			return false;
		}

		// Rotate session traffic MAC key to the per-connection key derived from REALM_ATTESTATION_KEY + nonces
		session.trafficKey = rotatedTrafficKey;
		session.attested = true;
		return true;
	}
	case RealmOpcode::OP_AUTH_REGISTER_REQ:
	case RealmOpcode::OP_AUTH_LOGIN_REQ: {
		if (payloadLen != sizeof(AuthCredentialsRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid AuthCredentialsRequestPayload size");
			return true;
		}
		if (!session.attested) {
			db_.LogSecurityEvent(session.accountId, session.activeCharId, session.activeGameId,
			    "ERROR", "AUTH_UNATTESTED_CLIENT", "Auth request attempted before binary attestation from " + session.remoteIp);
			AuthResultPayload res {};
			res.statusCode = static_cast<uint16_t>(RealmStatus::ERR_ATTESTATION_FAILED);
			std::snprintf(res.message, sizeof(res.message), "%s", "Binary attestation required before authentication");
			QueuePacket(session, RealmOpcode::OP_AUTH_RESULT, &res, sizeof(res));
			FlushClientWrite(session);
			return false;
		}
		if (session.authenticated || ++session.authAttempts > MaxAuthAttemptsPerSession) {
			db_.LogSecurityEvent(session.accountId, session.activeCharId, session.activeGameId,
			    "WARN", "AUTH_RATE_LIMIT", "Excessive or duplicate Argon2id auth attempts from " + session.remoteIp);
			return false;
		}

		AuthCredentialsRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));
		req.username[sizeof(req.username) - 1] = '\0';
		req.password[sizeof(req.password) - 1] = '\0';

		const std::string username(req.username);
		std::string password(req.password);

		int64_t accountId = 0;
		SessionToken sessionToken {};
		std::string message;
		const RealmStatus status = (opcode == RealmOpcode::OP_AUTH_REGISTER_REQ)
		    ? auth_.RegisterAccount(username, password, session.remoteIp, accountId, sessionToken, message)
		    : auth_.LoginAccount(username, password, session.remoteIp, accountId, sessionToken, message);

		sodium_memzero(req.password, sizeof(req.password));
		if (!password.empty()) {
			sodium_memzero(password.data(), password.size());
		}

		AuthResultPayload res {};
		res.statusCode = static_cast<uint16_t>(status);
		res.accountId = static_cast<uint64_t>(accountId);
		std::memcpy(res.sessionToken, sessionToken.data(), sizeof(res.sessionToken));
		std::snprintf(res.message, sizeof(res.message), "%s", message.c_str());

		if (status == RealmStatus::OK) {
			session.authenticated = true;
			session.accountId = accountId;
		}

		QueuePacket(session, RealmOpcode::OP_AUTH_RESULT, &res, sizeof(res));
		return true;
	}
	case RealmOpcode::OP_AUTH_RESUME_REQ: {
		if (payloadLen != sizeof(AuthResumeRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid AuthResumeRequestPayload size");
			return true;
		}
		if (!session.attested) {
			db_.LogSecurityEvent(session.accountId, session.activeCharId, session.activeGameId,
			    "ERROR", "AUTH_UNATTESTED_CLIENT", "Session resume attempted before binary attestation from " + session.remoteIp);
			AuthResultPayload res {};
			res.statusCode = static_cast<uint16_t>(RealmStatus::ERR_ATTESTATION_FAILED);
			std::snprintf(res.message, sizeof(res.message), "%s", "Binary attestation required before session resume");
			QueuePacket(session, RealmOpcode::OP_AUTH_RESULT, &res, sizeof(res));
			FlushClientWrite(session);
			return false;
		}

		AuthResumeRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));

		std::string message;
		const auto accountId = static_cast<int64_t>(req.accountId);
		const RealmStatus status = auth_.ResumeSession(accountId, req.sessionToken, session.remoteIp, message);

		AuthResultPayload res {};
		res.statusCode = static_cast<uint16_t>(status);
		res.accountId = (status == RealmStatus::OK) ? req.accountId : 0ULL;
		if (status == RealmStatus::OK) {
			std::memcpy(res.sessionToken, req.sessionToken, sizeof(res.sessionToken));
			session.authenticated = true;
			session.accountId = accountId;
		}
		std::snprintf(res.message, sizeof(res.message), "%s", message.c_str());

		QueuePacket(session, RealmOpcode::OP_AUTH_RESULT, &res, sizeof(res));
		return true;
	}
	case RealmOpcode::OP_HERO_LIST_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		HeroListResponsePayload resp {};
		vault_.ListHeroes(session.accountId, resp);
		QueuePacket(session, RealmOpcode::OP_HERO_LIST_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_HERO_CREATE_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		if (payloadLen != sizeof(HeroCreateRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid HeroCreateRequestPayload size");
			return true;
		}
		HeroCreateRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));
		req.name[sizeof(req.name) - 1] = '\0';

		HeroCreateResponsePayload resp {};
		const RealmStatus status = vault_.CreateHero(
		    session.accountId,
		    req.name,
		    req.heroClass,
		    req.bIsHellfire != 0,
		    session.remoteIp,
		    resp.summary,
		    resp.pack);
		resp.statusCode = static_cast<uint16_t>(status);
		QueuePacket(session, RealmOpcode::OP_HERO_CREATE_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_HERO_DELETE_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		if (payloadLen != sizeof(HeroDeleteRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid HeroDeleteRequestPayload size");
			return true;
		}
		HeroDeleteRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));

		const auto charId = static_cast<int64_t>(req.charId);
		const RealmStatus status = vault_.DeleteHero(session.accountId, charId, session.remoteIp);
		if (status == RealmStatus::OK && session.activeCharId == charId) {
			session.activeCharId = 0;
		}

		HeroDeleteResponsePayload resp {};
		resp.statusCode = static_cast<uint16_t>(status);
		resp.charId = req.charId;
		QueuePacket(session, RealmOpcode::OP_HERO_DELETE_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_HERO_LOAD_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		if (payloadLen != sizeof(HeroLoadRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid HeroLoadRequestPayload size");
			return true;
		}
		HeroLoadRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));

		HeroLoadResponsePayload resp {};
		resp.charId = req.charId;
		const auto charId = static_cast<int64_t>(req.charId);
		const RealmStatus status = vault_.LoadHero(session.accountId, charId, resp.pack);
		resp.statusCode = static_cast<uint16_t>(status);
		if (status == RealmStatus::OK) {
			session.activeCharId = charId;
		}
		QueuePacket(session, RealmOpcode::OP_HERO_LOAD_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_HERO_SAVE_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		if (payloadLen != sizeof(HeroSaveRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid HeroSaveRequestPayload size");
			return true;
		}
		HeroSaveRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));

		const auto charId = static_cast<int64_t>(req.charId);
		const GamePlayerRuntimeState *activePlayerSession = (session.activeGameId != 0)
		    ? sessionManager_.FindPlayerInGame(session.activeGameId, charId)
		    : nullptr;
		const RealmStatus status = vault_.SaveHero(session.accountId, charId, req.pack, session.remoteIp, activePlayerSession);

		HeroSaveResponsePayload resp {};
		resp.statusCode = static_cast<uint16_t>(status);
		resp.charId = req.charId;
		QueuePacket(session, RealmOpcode::OP_HERO_SAVE_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_STASH_LOAD_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		uint32_t stashGold = 0;
		std::vector<PkItemStruct> items;
		const RealmStatus status = vault_.LoadStash(session.accountId, stashGold, items);

		StashDataHeader hdrResp {};
		hdrResp.statusCode = static_cast<uint16_t>(status);
		hdrResp.itemCount = static_cast<uint16_t>(items.size());
		hdrResp.stashGold = stashGold;

		std::vector<uint8_t> buf(sizeof(StashDataHeader) + items.size() * sizeof(PkItemStruct));
		std::memcpy(buf.data(), &hdrResp, sizeof(StashDataHeader));
		if (!items.empty()) {
			std::memcpy(buf.data() + sizeof(StashDataHeader), items.data(), items.size() * sizeof(PkItemStruct));
		}
		QueuePacket(session, RealmOpcode::OP_STASH_LOAD_RESP, buf.data(), buf.size());
		return true;
	}
	case RealmOpcode::OP_STASH_SAVE_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		if (payloadLen < sizeof(StashDataHeader)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "StashSaveRequest missing StashDataHeader");
			return true;
		}
		StashDataHeader reqHdr {};
		std::memcpy(&reqHdr, payload, sizeof(reqHdr));
		const size_t expectedLen = sizeof(StashDataHeader) + static_cast<size_t>(reqHdr.itemCount) * sizeof(PkItemStruct);
		if (reqHdr.itemCount > MaxStashItems || payloadLen != expectedLen) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "StashSaveRequest payload length mismatch");
			return true;
		}

		std::vector<PkItemStruct> items(reqHdr.itemCount);
		if (reqHdr.itemCount > 0) {
			std::memcpy(items.data(), payload + sizeof(StashDataHeader), reqHdr.itemCount * sizeof(PkItemStruct));
		}

		const RealmStatus status = vault_.SaveStash(session.accountId, reqHdr.stashGold, items, session.remoteIp);
		StashSaveResponsePayload resp {};
		resp.statusCode = static_cast<uint16_t>(status);
		resp.savedItemCount = (status == RealmStatus::OK) ? reqHdr.itemCount : 0U;
		resp.stashGold = (status == RealmStatus::OK) ? reqHdr.stashGold : 0U;
		QueuePacket(session, RealmOpcode::OP_STASH_SAVE_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_GAME_CREATE_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		if (payloadLen != sizeof(GameCreateRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid GameCreateRequestPayload size");
			return true;
		}
		GameCreateRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));

		if (session.activeGameId != 0 && session.activeCharId != 0) {
			sessionManager_.LeaveGame(session.activeGameId, session.activeCharId);
			session.activeGameId = 0;
		}

		GameCreateOrJoinResponsePayload resp {};
		const RealmStatus status = sessionManager_.CreateGame(session.accountId, req, session.remoteIp, resp);
		if (status == RealmStatus::OK) {
			session.activeCharId = static_cast<int64_t>(req.charId);
			session.activeGameId = resp.gameId;
		}
		QueuePacket(session, RealmOpcode::OP_GAME_CREATE_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_GAME_JOIN_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		if (payloadLen != sizeof(GameJoinRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid GameJoinRequestPayload size");
			return true;
		}
		GameJoinRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));

		if (session.activeGameId != 0 && session.activeCharId != 0) {
			sessionManager_.LeaveGame(session.activeGameId, session.activeCharId);
			session.activeGameId = 0;
		}

		GameCreateOrJoinResponsePayload resp {};
		const RealmStatus status = sessionManager_.JoinGame(session.accountId, req, session.remoteIp, resp);
		if (status == RealmStatus::OK) {
			session.activeCharId = static_cast<int64_t>(req.charId);
			session.activeGameId = resp.gameId;
		}
		QueuePacket(session, RealmOpcode::OP_GAME_JOIN_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_GAME_LIST_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		GameListResponsePayload resp {};
		if (payloadLen == 0) {
			sessionManager_.ListGames(resp, nullptr);
		} else if (payloadLen == sizeof(GameListFilterRequestPayload)) {
			GameListFilterRequestPayload filter {};
			std::memcpy(&filter, payload, sizeof(filter));
			sessionManager_.ListGames(resp, &filter);
		} else {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid GameListRequestPayload size");
			return true;
		}
		QueuePacket(session, RealmOpcode::OP_GAME_LIST_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_FLOOR_TRANSITION_REQ: {
		if (!session.authenticated || session.accountId <= 0 || session.activeCharId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Active character session required");
			return true;
		}
		if (payloadLen != sizeof(FloorTransitionRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid FloorTransitionRequestPayload size");
			return true;
		}
		FloorTransitionRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));

		FloorTransitionResponsePayload resp {};
		sessionManager_.HandleFloorTransition(session.accountId, session.activeCharId, req, session.remoteIp, resp);
		QueuePacket(session, RealmOpcode::OP_FLOOR_TRANSITION_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_MONSTER_DROP_REQ: {
		if (!session.authenticated || session.accountId <= 0 || session.activeCharId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Active character session required");
			return true;
		}
		if (payloadLen != sizeof(MonsterDropRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid MonsterDropRequestPayload size");
			return true;
		}
		MonsterDropRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));

		MonsterOrChestDropResponsePayload resp {};
		sessionManager_.HandleMonsterDrop(session.accountId, session.activeCharId, req, session.remoteIp, resp);
		QueuePacket(session, RealmOpcode::OP_MONSTER_DROP_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_CHEST_DROP_REQ: {
		if (!session.authenticated || session.accountId <= 0 || session.activeCharId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Active character session required");
			return true;
		}
		if (payloadLen != sizeof(ChestDropRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid ChestDropRequestPayload size");
			return true;
		}
		ChestDropRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));

		MonsterOrChestDropResponsePayload resp {};
		sessionManager_.HandleChestDrop(session.accountId, session.activeCharId, req, session.remoteIp, resp);
		QueuePacket(session, RealmOpcode::OP_CHEST_DROP_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_ITEM_PICKUP_REQ: {
		if (!session.authenticated || session.accountId <= 0 || session.activeCharId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Active character session required");
			return true;
		}
		if (payloadLen != sizeof(ItemPickupRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid ItemPickupRequestPayload size");
			return true;
		}
		ItemPickupRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));

		ItemPickupResponsePayload resp {};
		sessionManager_.HandleItemPickup(session.accountId, session.activeCharId, req, session.remoteIp, resp);
		QueuePacket(session, RealmOpcode::OP_ITEM_PICKUP_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_ITEM_DROP_REQ: {
		if (!session.authenticated || session.accountId <= 0 || session.activeCharId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Active character session required");
			return true;
		}
		if (payloadLen != sizeof(ItemDropRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid ItemDropRequestPayload size");
			return true;
		}
		ItemDropRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));

		ItemDropResponsePayload resp {};
		sessionManager_.HandleItemDrop(session.accountId, session.activeCharId, req, session.remoteIp, resp);
		QueuePacket(session, RealmOpcode::OP_ITEM_DROP_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_CHAT_JOIN_CHANNEL_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		if (payloadLen != sizeof(ChatJoinChannelRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid ChatJoinChannelRequestPayload size");
			return true;
		}
		ChatJoinChannelRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));
		if (req.channelId >= static_cast<uint8_t>(RealmChatChannel::COUNT)) {
			QueueError(session, opcode, RealmStatus::ERR_CHAT_INVALID_CHANNEL, "Invalid chat channel ID");
			return true;
		}
		session.activeChannel = req.channelId;
		if (req.charId != 0) {
			CharacterRecord rec {};
			RealmStatus dummy = RealmStatus::OK;
			if (db_.LoadCharacter(session.accountId, static_cast<int64_t>(req.charId), rec, dummy)) {
				session.activeCharId = rec.id;
			}
		}
		ChatChannelFeedResponsePayload resp {};
		sessionManager_.PollChatChannel(session.accountId, req.channelId, 0, CountSessionsInChannel(req.channelId), resp);
		QueuePacket(session, RealmOpcode::OP_CHAT_JOIN_CHANNEL_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_CHAT_SEND_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		if (payloadLen != sizeof(ChatSendRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid ChatSendRequestPayload size");
			return true;
		}
		const uint64_t nowMs = NowMonotonicMs();
		if (nowMs - session.chatWindowStartMs >= 5000ULL) {
			session.chatWindowStartMs = nowMs;
			session.chatMessagesInWindow = 0;
		}
		if (++session.chatMessagesInWindow > 8U) {
			ChatSendResponsePayload rateResp {};
			rateResp.statusCode = static_cast<uint16_t>(RealmStatus::ERR_CHAT_RATE_LIMITED);
			rateResp.channelId = session.activeChannel;
			QueuePacket(session, RealmOpcode::OP_CHAT_SEND_RESP, &rateResp, sizeof(rateResp));
			return true;
		}
		ChatSendRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));
		session.activeChannel = (req.channelId < static_cast<uint8_t>(RealmChatChannel::COUNT))
		    ? req.channelId
		    : session.activeChannel;

		ChatSendResponsePayload resp {};
		sessionManager_.SendChatMessage(session.accountId, session.activeCharId, req, session.remoteIp, resp);
		QueuePacket(session, RealmOpcode::OP_CHAT_SEND_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_CHAT_POLL_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		if (payloadLen != sizeof(ChatPollRequestPayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid ChatPollRequestPayload size");
			return true;
		}
		ChatPollRequestPayload req {};
		std::memcpy(&req, payload, sizeof(req));
		if (req.channelId < static_cast<uint8_t>(RealmChatChannel::COUNT)) {
			session.activeChannel = req.channelId;
		}
		ChatChannelFeedResponsePayload resp {};
		sessionManager_.PollChatChannel(session.accountId, req.channelId, req.afterMessageId, CountSessionsInChannel(req.channelId), resp);
		QueuePacket(session, RealmOpcode::OP_CHAT_POLL_RESP, &resp, sizeof(resp));
		return true;
	}
	case RealmOpcode::OP_WARDEN_CHALLENGE_REQ: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		if (payloadLen != 0 && payloadLen != sizeof(WardenChallengePayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid WardenChallengeRequest size");
			return true;
		}
		session.activeWardenChallenge = GenerateWardenChallenge(session.nextWardenChallengeId++);
		session.wardenChallengePending = true;
		session.wardenIssuedAtMs = NowMonotonicMs();
		QueuePacket(session, RealmOpcode::OP_WARDEN_CHALLENGE_REQ, &session.activeWardenChallenge, sizeof(session.activeWardenChallenge));
		return true;
	}
	case RealmOpcode::OP_WARDEN_CHALLENGE_RESP: {
		if (!session.authenticated || session.accountId <= 0) {
			QueueError(session, opcode, RealmStatus::ERR_UNAUTHORIZED, "Authentication required");
			return true;
		}
		if (payloadLen != sizeof(WardenResponsePayload)) {
			QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Invalid WardenResponsePayload size");
			return true;
		}
		if (!session.wardenChallengePending) {
			QueueError(session, opcode, RealmStatus::ERR_WARDEN_INTEGRITY_VIOLATION, "No active Warden challenge pending");
			return true;
		}
		WardenResponsePayload req {};
		std::memcpy(&req, payload, sizeof(req));

		const uint64_t nowMs = NowMonotonicMs();
		uint32_t driftPermille = 1000;
		std::string reason;
		const RealmStatus status = VerifyWardenResponse(
		    session.activeWardenChallenge,
		    session.wardenBaselineServerMs,
		    session.wardenBaselineClientMs,
		    nowMs,
		    req,
		    driftPermille,
		    reason);

		session.wardenChallengePending = false;
		if (status == RealmStatus::OK) {
			session.lastWardenCompletedAtMs = nowMs;
			if (session.wardenBaselineServerMs == 0 && req.clientMonotonicMs > 0) {
				session.wardenBaselineServerMs = nowMs;
				session.wardenBaselineClientMs = req.clientMonotonicMs;
			}
		} else {
			const char *eventType = (status == RealmStatus::ERR_WARDEN_SPEEDHACK_DETECTED)
			    ? "WARDEN_SPEEDHACK_DETECTED"
			    : "WARDEN_INTEGRITY_VIOLATION";
			db_.LogSecurityEvent(session.accountId, session.activeCharId, session.activeGameId, "CRITICAL", eventType, reason);
		}

		WardenVerdictPayload resp {};
		resp.statusCode = static_cast<uint16_t>(status);
		resp.driftPermille = driftPermille;
		resp.challengeId = req.challengeId;
		QueuePacket(session, RealmOpcode::OP_WARDEN_VERDICT_RESP, &resp, sizeof(resp));
		return true;
	}
	default:
		QueueError(session, opcode, RealmStatus::ERR_MALFORMED_PACKET, "Unsupported or unimplemented opcode");
		return true;
	}
}

void RealmServer::QueuePacket(ClientSession &session, RealmOpcode opcode, const void *payload, size_t payloadLen)
{
	const uint32_t seq = session.nextServerSeq++;
	const std::vector<uint8_t> frame = EncodePacketFrame(opcode, seq, payload, payloadLen, session.trafficKey);
	if (session.sendBuffer.size() + frame.size() <= MaxSendBufferBytes) {
		session.sendBuffer.insert(session.sendBuffer.end(), frame.begin(), frame.end());
	}
}

void RealmServer::QueueError(ClientSession &session, RealmOpcode reqOpcode, RealmStatus status, const char *message)
{
	ErrorResponsePayload err {};
	err.requestOpcode = static_cast<uint16_t>(reqOpcode);
	err.statusCode = static_cast<uint16_t>(status);
	if (message != nullptr) {
		std::snprintf(err.message, sizeof(err.message), "%s", message);
	}
	QueuePacket(session, RealmOpcode::OP_ERROR_RESPONSE, &err, sizeof(err));
}

void RealmServer::CloseSession(SocketHandle sock)
{
	auto it = sessions_.find(sock);
	if (it != sessions_.end()) {
		if (it->second.activeGameId != 0 && it->second.activeCharId != 0) {
			sessionManager_.LeaveGame(it->second.activeGameId, it->second.activeCharId);
		}
		CloseRawSocket(sock);
		sessions_.erase(it);
	}
}

size_t RealmServer::DisconnectAccountSessions(int64_t accountId)
{
	if (accountId <= 0)
		return 0;
	size_t count = 0;
	for (auto it = sessions_.begin(); it != sessions_.end();) {
		if (it->second.accountId == accountId) {
			if (it->second.activeGameId != 0 && it->second.activeCharId != 0) {
				sessionManager_.LeaveGame(it->second.activeGameId, it->second.activeCharId);
			}
			CloseRawSocket(it->first);
			it = sessions_.erase(it);
			++count;
		} else {
			++it;
		}
	}
	return count;
}

uint16_t RealmServer::CountSessionsInChannel(uint8_t channelId) const
{
	uint16_t count = 0;
	for (const auto &[sock, s] : sessions_) {
		if (s.authenticated && s.activeChannel == channelId) {
			++count;
		}
	}
	return count;
}

} // namespace devilution::realm
