/**
 * @file realm_admin_server.cpp
 * @brief Embedded C++20 Admin HTTP Control Interface for dx-eternity-realm (Phase 7.1).
 */
#include "admin/realm_admin_server.hpp"
#include "admin/realm_updater.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <sodium.h>

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
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace devilution::realm {

namespace {

void CloseAdminSocket(SocketHandle s)
{
	if (s == InvalidSocketHandle)
		return;
#ifdef _WIN32
	closesocket(static_cast<SOCKET>(s));
#else
	close(s);
#endif
}

std::string BytesToHex(const uint8_t *bytes, size_t len)
{
	static constexpr char kHex[] = "0123456789abcdef";
	std::string out(len * 2, '0');
	for (size_t i = 0; i < len; ++i) {
		out[i * 2] = kHex[(bytes[i] >> 4) & 0x0FU];
		out[i * 2 + 1] = kHex[bytes[i] & 0x0FU];
	}
	return out;
}

bool ConstantTimeStringEquals(const std::string &a, const std::string &b)
{
	if (a.empty() || a.size() != b.size())
		return false;
	return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string EscapeJsonString(const std::string &input)
{
	std::string out;
	out.reserve(input.size() + 8);
	for (unsigned char c : input) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c >= 0x20)
				out.push_back(static_cast<char>(c));
			break;
		}
	}
	return out;
}

std::string ExtractHeaderValue(const std::string &rawHeaders, const std::string &headerNameLower)
{
	std::istringstream ss(rawHeaders);
	std::string line;
	while (std::getline(ss, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t colonPos = line.find(':');
		if (colonPos == std::string::npos)
			continue;

		std::string key = line.substr(0, colonPos);
		for (char &ch : key) {
			if (ch >= 'A' && ch <= 'Z')
				ch = static_cast<char>(ch - 'A' + 'a');
		}
		if (key != headerNameLower)
			continue;

		size_t valStart = colonPos + 1;
		while (valStart < line.size() && (line[valStart] == ' ' || line[valStart] == '\t'))
			++valStart;
		return line.substr(valStart);
	}
	return {};
}

std::string ExtractJsonField(const std::string &body, const std::string &fieldName)
{
	const std::string pattern = "\"" + fieldName + "\"";
	const size_t keyPos = body.find(pattern);
	if (keyPos == std::string::npos)
		return {};
	size_t pos = body.find(':', keyPos + pattern.size());
	if (pos == std::string::npos)
		return {};
	++pos;
	while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t' || body[pos] == '\n' || body[pos] == '\r'))
		++pos;
	if (pos >= body.size())
		return {};

	if (body[pos] == '"') {
		++pos;
		std::string val;
		while (pos < body.size() && body[pos] != '"') {
			if (body[pos] == '\\' && pos + 1 < body.size())
				++pos;
			val.push_back(body[pos++]);
		}
		return val;
	}

	size_t endPos = pos;
	while (endPos < body.size() && body[endPos] != ',' && body[endPos] != '}' && body[endPos] != ' ' && body[endPos] != '\r' && body[endPos] != '\n')
		++endPos;
	return body.substr(pos, endPos - pos);
}

void SendHttpResponse(
    SocketHandle sock,
    int statusCode,
    const char *statusText,
    const char *contentType,
    const std::string &body,
    const std::string &extraHeaders = "")
{
	std::ostringstream oss;
	oss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n"
	    << "Content-Type: " << contentType << "\r\n"
	    << "Content-Length: " << body.size() << "\r\n"
	    << "Cache-Control: no-store\r\n"
	    << "X-Content-Type-Options: nosniff\r\n"
	    << "X-Frame-Options: DENY\r\n"
	    << "Content-Security-Policy: default-src 'self' 'unsafe-inline'; frame-ancestors 'none'\r\n"
	    << "Connection: close\r\n";
	if (!extraHeaders.empty()) {
		oss << extraHeaders;
	}
	oss << "\r\n"
	    << body;

	const std::string wire = oss.str();
	size_t sent = 0;
	while (sent < wire.size()) {
#ifdef _WIN32
		const int n = send(static_cast<SOCKET>(sock), wire.data() + sent, static_cast<int>(wire.size() - sent), 0);
#else
		const ssize_t n = send(sock, wire.data() + sent, wire.size() - sent, 0);
#endif
		if (n <= 0)
			break;
		sent += static_cast<size_t>(n);
	}
}

} // namespace

RealmAdminServer::RealmAdminServer(RealmDatabase &db, RealmServer &realmServer)
    : db_(db)
    , realmServer_(realmServer)
{
}

RealmAdminServer::~RealmAdminServer()
{
	Stop();
}

bool RealmAdminServer::Start(
    const std::string &bindIp,
    uint16_t port,
    const std::string &adminSecretToken,
    std::string &outError)
{
	Stop();

	randombytes_buf(cookieSigningKey_.data(), cookieSigningKey_.size());
	uint8_t csrfBytes[16] {};
	randombytes_buf(csrfBytes, sizeof(csrfBytes));
	csrfToken_ = BytesToHex(csrfBytes, sizeof(csrfBytes));

	if (!adminSecretToken.empty()) {
		adminToken_ = adminSecretToken;
	} else {
		uint8_t randTok[16] {};
		randombytes_buf(randTok, sizeof(randTok));
		adminToken_ = BytesToHex(randTok, sizeof(randTok));
	}

#ifdef _WIN32
	const SOCKET rawSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (rawSock == INVALID_SOCKET) {
		outError = "admin socket() failed";
		return false;
	}
	listenSocket_ = static_cast<SocketHandle>(rawSock);
	BOOL reuse = TRUE;
	setsockopt(rawSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
	u_long nonBlock = 1;
	ioctlsocket(rawSock, FIONBIO, &nonBlock);
#else
	const int rawSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (rawSock < 0) {
		outError = "admin socket() failed";
		return false;
	}
	listenSocket_ = rawSock;
	const int reuse = 1;
	setsockopt(rawSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	const int flags = fcntl(rawSock, F_GETFL, 0);
	fcntl(rawSock, F_SETFL, flags | O_NONBLOCK);
#endif

	sockaddr_in addr {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (bindIp.empty() || bindIp == "0.0.0.0") {
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	} else if (inet_pton(AF_INET, bindIp.c_str(), &addr.sin_addr) != 1) {
		outError = "Invalid admin bind IPv4: " + bindIp;
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
		outError = "admin bind() failed on port " + std::to_string(port);
		Stop();
		return false;
	}

	if (listen(
#ifdef _WIN32
	        static_cast<SOCKET>(listenSocket_),
#else
	        listenSocket_,
#endif
	        32)
	    != 0) {
		outError = "admin listen() failed";
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

void RealmAdminServer::Stop()
{
	if (listenSocket_ != InvalidSocketHandle) {
		CloseAdminSocket(listenSocket_);
		listenSocket_ = InvalidSocketHandle;
	}
	boundPort_ = 0;
}

void RealmAdminServer::PollOnce(int timeoutMs)
{
	if (listenSocket_ == InvalidSocketHandle)
		return;

#ifdef _WIN32
	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(static_cast<SOCKET>(listenSocket_), &readSet);
	timeval tv {};
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = (timeoutMs % 1000) * 1000;
	if (select(0, &readSet, nullptr, nullptr, &tv) <= 0)
		return;
#else
	pollfd pfd {};
	pfd.fd = listenSocket_;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, timeoutMs) <= 0 || (pfd.revents & POLLIN) == 0)
		return;
#endif

	while (true) {
		sockaddr_in clientAddr {};
#ifdef _WIN32
		int addrLen = sizeof(clientAddr);
		const SOCKET rawClient = accept(static_cast<SOCKET>(listenSocket_), reinterpret_cast<sockaddr *>(&clientAddr), &addrLen);
		if (rawClient == INVALID_SOCKET)
			break;
		DWORD sockTimeoutMs = 250;
		setsockopt(rawClient, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&sockTimeoutMs), sizeof(sockTimeoutMs));
		setsockopt(rawClient, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&sockTimeoutMs), sizeof(sockTimeoutMs));
		const SocketHandle clientSock = static_cast<SocketHandle>(rawClient);
#else
		socklen_t addrLen = sizeof(clientAddr);
		const int clientSock = accept(listenSocket_, reinterpret_cast<sockaddr *>(&clientAddr), &addrLen);
		if (clientSock < 0)
			break;
		timeval sockTv {};
		sockTv.tv_sec = 0;
		sockTv.tv_usec = 250000;
		setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, &sockTv, sizeof(sockTv));
		setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, &sockTv, sizeof(sockTv));
#endif

		char ipBuf[INET_ADDRSTRLEN] {};
		inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
		HandleClientConnection(clientSock, ipBuf);
		CloseAdminSocket(clientSock);
	}
}

std::string RealmAdminServer::ComputeSessionCookieHmac() const
{
	uint8_t mac[32] {};
	const std::string context = "DXER-ADMIN-SESSION:" + adminToken_;
	crypto_generichash(mac, sizeof(mac),
	    reinterpret_cast<const unsigned char *>(context.data()), context.size(),
	    cookieSigningKey_.data(), cookieSigningKey_.size());
	return BytesToHex(mac, sizeof(mac));
}

bool RealmAdminServer::IsRequestAuthorized(const std::string &rawHeaders, bool isMutatingPost) const
{
	// 1. Check Authorization: Bearer <token>
	const std::string authHdr = ExtractHeaderValue(rawHeaders, "authorization");
	const std::string bearerPrefix = "Bearer ";
	if (authHdr.size() > bearerPrefix.size() && authHdr.compare(0, bearerPrefix.size(), bearerPrefix) == 0) {
		const std::string candidate = authHdr.substr(bearerPrefix.size());
		if (ConstantTimeStringEquals(candidate, adminToken_)) {
			return true;
		}
	}

	// 2. Check HttpOnly Cookie `dxer_admin_session=<hmac>`
	const std::string cookieHdr = ExtractHeaderValue(rawHeaders, "cookie");
	const std::string cookiePrefix = "dxer_admin_session=";
	const size_t cookiePos = cookieHdr.find(cookiePrefix);
	if (cookiePos != std::string::npos) {
		size_t valStart = cookiePos + cookiePrefix.size();
		size_t valEnd = cookieHdr.find(';', valStart);
		const std::string cookieVal = (valEnd == std::string::npos)
		    ? cookieHdr.substr(valStart)
		    : cookieHdr.substr(valStart, valEnd - valStart);
		const std::string expectedHmac = ComputeSessionCookieHmac();
		if (ConstantTimeStringEquals(cookieVal, expectedHmac)) {
			if (!isMutatingPost)
				return true;
			const std::string csrfHdr = ExtractHeaderValue(rawHeaders, "x-csrf-token");
			return ConstantTimeStringEquals(csrfHdr, csrfToken_);
		}
	}

	return false;
}

void RealmAdminServer::HandleClientConnection(SocketHandle clientSock, const std::string &remoteIp)
{
	std::vector<char> reqBuf(32768, '\0');
#ifdef _WIN32
	const int n = recv(static_cast<SOCKET>(clientSock), reqBuf.data(), static_cast<int>(reqBuf.size() - 1), 0);
#else
	const ssize_t n = recv(clientSock, reqBuf.data(), reqBuf.size() - 1, 0);
#endif
	if (n <= 0)
		return;

	const std::string rawReq(reqBuf.data(), static_cast<size_t>(n));
	const size_t lineEnd = rawReq.find("\r\n");
	if (lineEnd == std::string::npos)
		return;

	std::istringstream firstLine(rawReq.substr(0, lineEnd));
	std::string method;
	std::string path;
	firstLine >> method >> path;

	const size_t hdrEnd = rawReq.find("\r\n\r\n");
	const std::string rawHeaders = (hdrEnd != std::string::npos)
	    ? rawReq.substr(lineEnd + 2, hdrEnd - (lineEnd + 2))
	    : "";
	const std::string body = (hdrEnd != std::string::npos)
	    ? rawReq.substr(hdrEnd + 4)
	    : "";

	if (method == "GET" && (path == "/" || path == "/admin")) {
		SendHttpResponse(clientSock, 200, "OK", "text/html; charset=utf-8", BuildEmbeddedDashboardHtml());
		return;
	}

	if (method == "POST" && path == "/api/admin/login") {
		const std::string submittedToken = ExtractJsonField(body, "token");
		if (!ConstantTimeStringEquals(submittedToken, adminToken_)) {
			db_.LogSecurityEvent(0, 0, 0, "WARN", "ADMIN_AUTH_FAILED", "Failed admin login attempt from " + remoteIp);
			SendHttpResponse(clientSock, 401, "Unauthorized", "application/json", R"({"ok":false,"error":"Invalid admin token"})");
			return;
		}
		const std::string sessionCookie = "Set-Cookie: dxer_admin_session=" + ComputeSessionCookieHmac()
		    + "; HttpOnly; SameSite=Strict; Path=/\r\n";
		const std::string respJson = R"({"ok":true,"csrfToken":")" + csrfToken_ + R"("})";
		db_.LogSecurityEvent(0, 0, 0, "INFO", "ADMIN_LOGIN_SUCCESS", "Admin console login from " + remoteIp);
		SendHttpResponse(clientSock, 200, "OK", "application/json", respJson, sessionCookie);
		return;
	}

	const bool isMutatingPost = (method == "POST");
	if (!IsRequestAuthorized(rawHeaders, isMutatingPost)) {
		SendHttpResponse(clientSock, 401, "Unauthorized", "application/json",
		    R"({"ok":false,"error":"Unauthorized admin request"})");
		return;
	}

	if (method == "GET" && path == "/api/admin/status") {
		SendHttpResponse(clientSock, 200, "OK", "application/json", BuildStatusJson());
		return;
	}
	if (method == "GET" && path == "/api/admin/accounts") {
		SendHttpResponse(clientSock, 200, "OK", "application/json", BuildAccountsJson());
		return;
	}
	if (method == "GET" && path == "/api/admin/characters") {
		SendHttpResponse(clientSock, 200, "OK", "application/json", BuildCharactersJson());
		return;
	}
	if (method == "GET" && path == "/api/admin/audit") {
		SendHttpResponse(clientSock, 200, "OK", "application/json", BuildAuditLogJson());
		return;
	}
	if (method == "POST" && (path == "/api/admin/ban" || path == "/api/admin/unban")) {
		const std::string accountIdStr = ExtractJsonField(body, "accountId");
		const std::string reasonStr = ExtractJsonField(body, "reason");
		const int64_t targetAccountId = std::strtoll(accountIdStr.c_str(), nullptr, 10);
		if (targetAccountId <= 0) {
			SendHttpResponse(clientSock, 400, "Bad Request", "application/json", R"({"ok":false,"error":"Invalid accountId"})");
			return;
		}
		const bool banFlag = (path == "/api/admin/ban");
		const std::string finalReason = banFlag ? (reasonStr.empty() ? "Banned by Realm Admin" : reasonStr) : "";
		if (!db_.SetAccountBanStatus(targetAccountId, banFlag, finalReason)) {
			SendHttpResponse(clientSock, 500, "Internal Server Error", "application/json", R"({"ok":false,"error":"Database update failed"})");
			return;
		}
		if (banFlag) {
			realmServer_.DisconnectAccountSessions(targetAccountId);
		}
		db_.LogSecurityEvent(targetAccountId, 0, 0, "WARN", banFlag ? "ADMIN_BAN" : "ADMIN_UNBAN",
		    (banFlag ? "Admin banned accountId=" : "Admin unbanned accountId=") + std::to_string(targetAccountId)
		        + (finalReason.empty() ? "" : (" (" + finalReason + ")")) + " from " + remoteIp);
		SendHttpResponse(clientSock, 200, "OK", "application/json", R"({"ok":true})");
		return;
	}

	if (method == "POST" && path == "/api/admin/promote-torment") {
		const int64_t accountId = std::strtoll(ExtractJsonField(body, "accountId").c_str(), nullptr, 10);
		const int64_t charId = std::strtoll(ExtractJsonField(body, "charId").c_str(), nullptr, 10);
		const std::string lvlStr = ExtractJsonField(body, "level");
		const std::string killStr = ExtractJsonField(body, "diabloKillLevel");
		const int8_t targetLevel = lvlStr.empty() ? 35 : static_cast<int8_t>(std::atoi(lvlStr.c_str()));
		const uint32_t targetKills = killStr.empty() ? 3U : static_cast<uint32_t>(std::atoi(killStr.c_str()));

		if (accountId <= 0 || charId <= 0) {
			SendHttpResponse(clientSock, 400, "Bad Request", "application/json", R"({"ok":false,"error":"Invalid accountId or charId"})");
			return;
		}

		std::string msg;
		if (!realmServer_.VaultManager().AdminPromoteHeroToTorment(accountId, charId, targetLevel, targetKills, msg)) {
			SendHttpResponse(clientSock, 400, "Bad Request", "application/json",
			    "{\"ok\":false,\"error\":\"" + EscapeJsonString(msg) + "\"}");
			return;
		}
		SendHttpResponse(clientSock, 200, "OK", "application/json",
		    "{\"ok\":true,\"message\":\"" + EscapeJsonString(msg) + "\"}");
		return;
	}

	if (method == "POST" && path == "/api/admin/import-hero") {
		const int64_t accountId = std::strtoll(ExtractJsonField(body, "accountId").c_str(), nullptr, 10);
		const std::string packHex = ExtractJsonField(body, "packHex");
		if (accountId <= 0 || packHex.size() != sizeof(PlayerPack) * 2) {
			SendHttpResponse(clientSock, 400, "Bad Request", "application/json",
			    R"JSON({"ok":false,"error":"Invalid accountId or packHex length (expected 2524 hex chars for 1262-byte PlayerPack)"})JSON");
			return;
		}

		PlayerPack importedPack {};
		size_t decodedLen = 0;
		if (sodium_hex2bin(reinterpret_cast<unsigned char *>(&importedPack), sizeof(PlayerPack),
		        packHex.c_str(), packHex.size(), nullptr, &decodedLen, nullptr) != 0
		    || decodedLen != sizeof(PlayerPack)) {
			SendHttpResponse(clientSock, 400, "Bad Request", "application/json",
			    R"JSON({"ok":false,"error":"packHex contains invalid hexadecimal characters"})JSON");
			return;
		}

		int64_t importedCharId = 0;
		std::string msg;
		if (!realmServer_.VaultManager().AdminImportCharacterPack(accountId, importedPack, importedCharId, msg)) {
			SendHttpResponse(clientSock, 400, "Bad Request", "application/json",
			    "{\"ok\":false,\"error\":\"" + EscapeJsonString(msg) + "\"}");
			return;
		}
		SendHttpResponse(clientSock, 200, "OK", "application/json",
		    "{\"ok\":true,\"charId\":" + std::to_string(importedCharId) + ",\"message\":\"" + EscapeJsonString(msg) + "\"}");
		return;
	}

	if (method == "POST" && path == "/api/admin/import-stash") {
		const int64_t accountId = std::strtoll(ExtractJsonField(body, "accountId").c_str(), nullptr, 10);
		const uint32_t stashGold = static_cast<uint32_t>(std::strtoul(ExtractJsonField(body, "stashGold").c_str(), nullptr, 10));
		const std::string itemsHex = ExtractJsonField(body, "itemsHex");
		if (accountId <= 0 || (itemsHex.size() % (sizeof(PkItemStruct) * 2)) != 0) {
			SendHttpResponse(clientSock, 400, "Bad Request", "application/json",
			    R"JSON({"ok":false,"error":"Invalid accountId or itemsHex length (must be a multiple of 38 hex chars = 19-byte PkItemStruct)"})JSON");
			return;
		}

		const size_t itemCount = itemsHex.size() / (sizeof(PkItemStruct) * 2);
		std::vector<PkItemStruct> items(itemCount);
		if (itemCount > 0) {
			size_t decodedLen = 0;
			if (sodium_hex2bin(reinterpret_cast<unsigned char *>(items.data()), itemCount * sizeof(PkItemStruct),
			        itemsHex.c_str(), itemsHex.size(), nullptr, &decodedLen, nullptr) != 0
			    || decodedLen != itemCount * sizeof(PkItemStruct)) {
				SendHttpResponse(clientSock, 400, "Bad Request", "application/json",
				    R"({"ok":false,"error":"itemsHex contains invalid hexadecimal characters"})");
				return;
			}
		}

		std::string msg;
		if (!realmServer_.VaultManager().AdminImportStash(accountId, stashGold, items, msg)) {
			SendHttpResponse(clientSock, 400, "Bad Request", "application/json",
			    "{\"ok\":false,\"error\":\"" + EscapeJsonString(msg) + "\"}");
			return;
		}
		SendHttpResponse(clientSock, 200, "OK", "application/json",
		    "{\"ok\":true,\"message\":\"" + EscapeJsonString(msg) + "\"}");
		return;
	}

	if (method == "POST" && path == "/api/admin/update") {
		const std::string sourceStr = ExtractJsonField(body, "source");
		const std::string blake2bHex = ExtractJsonField(body, "blake2b");
		const std::string ghToken = ExtractJsonField(body, "githubToken");
		const std::string forceStr = ExtractJsonField(body, "force");
		const bool forceUpdate = (forceStr == "true" || forceStr == "1");

		if (!forceUpdate && realmServer_.SessionManager().ActiveGameCount() > 0) {
			SendHttpResponse(clientSock, 409, "Conflict", "application/json",
			    R"({"ok":false,"error":"Active games are currently running. Wait for 0 active games or pass force=true."})");
			return;
		}

		const std::string targetExe = executablePath_.empty()
		    ? ResolveCurrentExecutablePath(nullptr)
		    : executablePath_;
		const SelfUpdateResult res = StageAndApplyLinuxSelfUpdate(
		    sourceStr.empty() ? DefaultGitHubRepo : sourceStr,
		    targetExe,
		    blake2bHex,
		    ghToken);

		if (!res.ok) {
			db_.LogSecurityEvent(0, 0, 0, "WARN", "SERVER_UPDATE_FAILED",
			    "Self-update from '" + (sourceStr.empty() ? DefaultGitHubRepo : sourceStr) + "' failed: " + res.message);
			SendHttpResponse(clientSock, 400, "Bad Request", "application/json",
			    "{\"ok\":false,\"error\":\"" + EscapeJsonString(res.message) + "\"}");
			return;
		}

		db_.LogSecurityEvent(0, 0, 0, "INFO", "SERVER_SELF_UPDATE",
		    res.message + " (triggered by admin from " + remoteIp + ")");
		shouldRestartForUpdate_ = true;

		SendHttpResponse(clientSock, 200, "OK", "application/json",
		    "{\"ok\":true,\"message\":\"" + EscapeJsonString(res.message)
		        + "\",\"blake2b\":\"" + EscapeJsonString(res.computedBlake2b256Hex) + "\"}");
		return;
	}

	if (method == "GET" && path == "/api/admin/chat") {
		std::ostringstream oss;
		oss << "{\"ok\":true,\"channels\":[";
		static constexpr const char *kChanNames[4] = { "General", "Trade", "LFG", "Torment" };
		for (uint8_t ch = 0; ch < static_cast<uint8_t>(RealmChatChannel::COUNT); ++ch) {
			if (ch > 0)
				oss << ",";
			ChatChannelFeedResponsePayload feed {};
			realmServer_.SessionManager().PollChatChannel(0, ch, 0, realmServer_.CountSessionsInChannel(ch), feed);
			oss << "{\"channelId\":" << static_cast<int>(ch)
			    << ",\"name\":\"" << kChanNames[ch] << "\""
			    << ",\"users\":" << feed.channelUsers
			    << ",\"messages\":[";
			for (uint8_t m = 0; m < feed.messageCount; ++m) {
				if (m > 0)
					oss << ",";
				const auto &entry = feed.messages[m];
				oss << "{\"id\":" << entry.messageId
				    << ",\"sender\":\"" << EscapeJsonString(entry.senderName) << "\""
				    << ",\"level\":" << static_cast<int>(entry.senderLevel)
				    << ",\"crown\":" << ((entry.flags & CHAT_FLAG_TORMENT_CROWN) != 0 ? "true" : "false")
				    << ",\"text\":\"" << EscapeJsonString(entry.text) << "\"}";
			}
			oss << "]}";
		}
		oss << "]}";
		SendHttpResponse(clientSock, 200, "OK", "application/json", oss.str());
		return;
	}

	if (method == "POST" && path == "/api/admin/chat") {
		const uint8_t ch = static_cast<uint8_t>(std::atoi(ExtractJsonField(body, "channelId").c_str()));
		const std::string text = ExtractJsonField(body, "text");
		if (ch >= static_cast<uint8_t>(RealmChatChannel::COUNT) || text.empty()) {
			SendHttpResponse(clientSock, 400, "Bad Request", "application/json",
			    R"JSON({"ok":false,"error":"Invalid channelId (0..3) or empty text"})JSON");
			return;
		}
		const uint64_t msgId = realmServer_.SessionManager().PostSystemChatMessage(
		    static_cast<RealmChatChannel>(ch), "REALM ADMIN", text);
		SendHttpResponse(clientSock, 200, "OK", "application/json",
		    "{\"ok\":true,\"messageId\":" + std::to_string(msgId) + "}");
		return;
	}

	SendHttpResponse(clientSock, 404, "Not Found", "application/json", R"({"ok":false,"error":"Endpoint not found"})");
}

std::string RealmAdminServer::BuildStatusJson() const
{
	GameListResponsePayload lobby {};
	realmServer_.SessionManager().ListGames(lobby);

	std::ostringstream oss;
	oss << "{"
	    << "\"ok\":true,"
	    << "\"realmPort\":" << realmServer_.BoundPort() << ","
	    << "\"adminPort\":" << boundPort_ << ","
	    << "\"activeSessions\":" << realmServer_.ActiveSessionCount() << ","
	    << "\"totalPacketsProcessed\":" << realmServer_.TotalPacketsProcessed() << ","
	    << "\"activeGames\":" << realmServer_.SessionManager().ActiveGameCount() << ","
	    << "\"totalAccounts\":" << db_.GetTotalAccountsCount() << ","
	    << "\"totalCharacters\":" << db_.GetTotalCharactersCount() << ","
	    << "\"totalLedgerItems\":" << db_.GetTotalLedgerItemsCount() << ","
	    << "\"securityAuditCount\":" << db_.GetSecurityAuditCount() << ","
	    << "\"csrfToken\":\"" << csrfToken_ << "\","
	    << "\"games\":[";
	for (uint16_t i = 0; i < lobby.gameCount; ++i) {
		if (i > 0)
			oss << ",";
		const GameLobbyInfo &g = lobby.games[i];
		oss << "{"
		    << "\"gameId\":" << g.gameId << ","
		    << "\"gameName\":\"" << EscapeJsonString(g.gameName) << "\","
		    << "\"difficulty\":" << static_cast<int>(g.difficulty) << ","
		    << "\"currentPlayers\":" << static_cast<int>(g.currentPlayers) << ","
		    << "\"maxPlayers\":" << static_cast<int>(g.maxPlayers) << ","
		    << "\"passwordProtected\":" << (g.passwordProtected != 0 ? "true" : "false")
		    << "}";
	}
	oss << "]}";
	return oss.str();
}

std::string RealmAdminServer::BuildAccountsJson() const
{
	const auto accounts = db_.ListAccounts(50);
	std::ostringstream oss;
	oss << "{\"ok\":true,\"accounts\":[";
	for (size_t i = 0; i < accounts.size(); ++i) {
		if (i > 0)
			oss << ",";
		const auto &a = accounts[i];
		oss << "{"
		    << "\"id\":" << a.id << ","
		    << "\"username\":\"" << EscapeJsonString(a.username) << "\","
		    << "\"createdAt\":" << a.createdAt << ","
		    << "\"lastLoginAt\":" << a.lastLoginAt << ","
		    << "\"isBanned\":" << (a.isBanned ? "true" : "false") << ","
		    << "\"banReason\":\"" << EscapeJsonString(a.banReason) << "\""
		    << "}";
	}
	oss << "]}";
	return oss.str();
}

std::string RealmAdminServer::BuildCharactersJson() const
{
	const auto chars = db_.ListAllCharacters(50);
	std::ostringstream oss;
	oss << "{\"ok\":true,\"characters\":[";
	for (size_t i = 0; i < chars.size(); ++i) {
		if (i > 0)
			oss << ",";
		const auto &c = chars[i];
		oss << "{"
		    << "\"id\":" << c.id << ","
		    << "\"accountId\":" << c.accountId << ","
		    << "\"heroName\":\"" << EscapeJsonString(c.heroName) << "\","
		    << "\"heroClass\":" << static_cast<int>(c.heroClass) << ","
		    << "\"heroLevel\":" << static_cast<int>(c.heroLevel) << ","
		    << "\"diabloKillLevel\":" << static_cast<int>(c.diabloKillLevel) << ","
		    << "\"experience\":" << c.experience << ","
		    << "\"gold\":" << c.gold << ","
		    << "\"ledgerItems\":" << db_.CountLedgerItemsForCharacter(c.id)
		    << "}";
	}
	oss << "]}";
	return oss.str();
}

std::string RealmAdminServer::BuildAuditLogJson() const
{
	const auto logs = db_.ListRecentAuditLogs(100);
	std::ostringstream oss;
	oss << "{\"ok\":true,\"logs\":[";
	for (size_t i = 0; i < logs.size(); ++i) {
		if (i > 0)
			oss << ",";
		const auto &e = logs[i];
		oss << "{"
		    << "\"id\":" << e.id << ","
		    << "\"accountId\":" << e.accountId << ","
		    << "\"charId\":" << e.charId << ","
		    << "\"gameId\":" << e.gameId << ","
		    << "\"severity\":\"" << EscapeJsonString(e.severity) << "\","
		    << "\"category\":\"" << EscapeJsonString(e.category) << "\","
		    << "\"details\":\"" << EscapeJsonString(e.details) << "\","
		    << "\"createdAt\":" << e.createdAt
		    << "}";
	}
	oss << "]}";
	return oss.str();
}

std::string RealmAdminServer::BuildEmbeddedDashboardHtml() const
{
	return R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<title>DX:Eternity Closed Realm — Admin Control Console</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #0b0c10; color: #d0d4dc; font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; padding: 24px; }
  h1 { color: #e2b714; font-size: 18px; margin-bottom: 4px; }
  .sub { color: #7d8494; font-size: 12px; margin-bottom: 18px; }
  .bar { display: flex; gap: 10px; margin-bottom: 18px; align-items: center; }
  input, button { background: #141720; color: #e2e6ef; border: 1px solid #2b3040; padding: 7px 12px; border-radius: 4px; font-family: inherit; font-size: 12px; }
  button { cursor: pointer; background: #1e2433; border-color: #3a435e; }
  button:hover { background: #283044; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; margin-bottom: 18px; }
  .stat { background: #13161f; border: 1px solid #222736; padding: 12px; border-radius: 4px; }
  .stat .k { color: #7d8494; font-size: 11px; }
  .stat .v { color: #e2b714; font-size: 18px; font-weight: bold; margin-top: 4px; }
  .panel { background: #13161f; border: 1px solid #222736; padding: 14px; border-radius: 4px; margin-bottom: 16px; }
  .panel h2 { font-size: 14px; color: #9rb; margin-bottom: 10px; color: #a5b4fc; }
  table { width: 100%; border-collapse: collapse; font-size: 12px; }
  th, td { text-align: left; padding: 6px 8px; border-bottom: 1px solid #1e2230; }
  th { color: #7d8494; }
</style>
</head>
<body>
  <h1>DX:Eternity Closed Realm — Admin Control Console</h1>
  <div class="sub">Embedded C++20 Zero-Dependency Realm Telemetry, Account Ban Control &amp; Anomaly Feed</div>
  <div class="bar">
    <input id="tok" type="password" placeholder="Admin Secret Token" style="width:280px"/>
    <button onclick="login()">Authenticate</button>
    <button onclick="refreshAll()">Refresh Telemetry</button>
    <span id="msg" style="font-size:12px;color:#56d364"></span>
  </div>
  <div class="grid" id="stats"></div>
  <div class="panel">
    <h2>Linux Semi-Auto Self-Updater (mrphantastik/dx-eternity-realm)</h2>
    <div class="bar" style="margin-bottom:0">
      <input id="updSrc" value="mrphantastik/dx-eternity-realm" placeholder="mrphantastik/dx-eternity-realm or 'git' or https://..." style="width:290px"/>
      <input id="updToken" type="password" placeholder="Optional GitHub Token (if private release)" style="width:240px"/>
      <input id="updHash" placeholder="Optional BLAKE2b-256 hex" style="width:200px"/>
      <button onclick="runSelfUpdate(false)">Verify (--self-test) &amp; Update Server</button>
    </div>
  </div>
  <div class="panel"><h2>Accounts &amp; Ban Management</h2><table id="acctTbl"><thead><tr><th>ID</th><th>Username</th><th>Status</th><th>Reason</th><th>Action</th></tr></thead><tbody></tbody></table></div>
  <div class="panel">
    <h2>Admin Character (SP / MP .pack) &amp; Stash (.stash) Importer</h2>
    <div class="bar" style="margin-bottom:0;flex-wrap:wrap">
      <input id="impAcctId" type="number" value="1" placeholder="Account ID" style="width:100px"/>
      <input id="impPackFile" type="file" accept=".pack,.bin"/>
      <button onclick="importHeroFile()">Import SP/MP Hero (.pack)</button>
      <input id="impStashFile" type="file" accept=".stash,.bin"/>
      <button onclick="importStashFile()">Import Stash (.stash)</button>
    </div>
  </div>
  <div class="panel"><h2>Characters &amp; Server-Vault Provenance</h2><table id="charTbl"><thead><tr><th>ID</th><th>Acct</th><th>Hero</th><th>Class</th><th>Lvl</th><th>Diablo Kills</th><th>Gold</th><th>Ledger Items</th><th>Action</th></tr></thead><tbody></tbody></table></div>
  <div class="panel">
    <h2>Lobby Static Chat Channels (#General / #Trade / #LFG / #Torment)</h2>
    <div class="bar" style="margin-bottom:8px">
      <select id="chatChan" style="background:#141720;color:#e2e6ef;border:1px solid #2b3040;padding:7px;border-radius:4px">
        <option value="0">#General</option>
        <option value="1">#Trade</option>
        <option value="2">#LFG</option>
        <option value="3">#Torment</option>
      </select>
      <input id="chatText" placeholder="Broadcast [REALM ADMIN] message to channel..." style="width:360px"/>
      <button onclick="sendAdminChat()">Broadcast</button>
    </div>
    <div id="chatFeed" style="font-size:12px;max-height:140px;overflow-y:auto;background:#0e1017;padding:8px;border:1px solid #1e2230;border-radius:4px"></div>
  </div>
  <div class="panel"><h2>Real-Time Security &amp; Anti-Cheat Audit Log</h2><table id="auditTbl"><thead><tr><th>ID</th><th>Severity</th><th>Category</th><th>Acct/Hero/Game</th><th>Details</th></tr></thead><tbody></tbody></table></div>
<script>
let csrf = '';
function esc(v) {
  return String(v ?? '').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');
}
function bytesToHex(u8) {
  return Array.from(u8, b => b.toString(16).padStart(2, '0')).join('');
}
async function login() {
  const token = document.getElementById('tok').value;
  const r = await fetch('/api/admin/login', {method:'POST', body:JSON.stringify({token})});
  const j = await r.json();
  if (j.ok) { csrf = j.csrfToken; document.getElementById('msg').textContent = 'Authenticated'; refreshAll(); }
  else { document.getElementById('msg').textContent = j.error || 'Auth failed'; }
}
async function toggleBan(id, ban) {
  const reason = ban ? (prompt('Ban reason:', 'Anti-cheat violation') || 'Anti-cheat violation') : '';
  await fetch(ban ? '/api/admin/ban' : '/api/admin/unban', {
    method:'POST', headers:{'X-CSRF-Token': csrf}, body:JSON.stringify({accountId:id, reason})
  });
  refreshAll();
}
async function promoteTorment(acctId, charId) {
  const r = await fetch('/api/admin/promote-torment', {
    method:'POST', headers:{'X-CSRF-Token': csrf},
    body:JSON.stringify({accountId:acctId, charId:charId, level:35, diabloKillLevel:3})
  });
  const j = await r.json();
  document.getElementById('msg').textContent = j.ok ? j.message : (j.error || 'Promote failed');
  refreshAll();
}
async function importHeroFile() {
  const acctId = Number(document.getElementById('impAcctId').value);
  const f = document.getElementById('impPackFile').files[0];
  if (!f) { document.getElementById('msg').textContent = 'Select a 1262-byte .pack file first'; return; }
  const u8 = new Uint8Array(await f.arrayBuffer());
  const r = await fetch('/api/admin/import-hero', {
    method:'POST', headers:{'X-CSRF-Token': csrf},
    body:JSON.stringify({accountId:acctId, packHex:bytesToHex(u8)})
  });
  const j = await r.json();
  document.getElementById('msg').textContent = j.ok ? j.message : (j.error || 'Import failed');
  refreshAll();
}
async function importStashFile() {
  const acctId = Number(document.getElementById('impAcctId').value);
  const f = document.getElementById('impStashFile').files[0];
  if (!f) { document.getElementById('msg').textContent = 'Select an exported .stash file first'; return; }
  const u8 = new Uint8Array(await f.arrayBuffer());
  if (u8.byteLength < 6) { document.getElementById('msg').textContent = 'Invalid .stash file'; return; }
  const dv = new DataView(u8.buffer);
  const stashGold = dv.getUint32(2, true);
  const itemsHex = bytesToHex(u8.subarray(6));
  const r = await fetch('/api/admin/import-stash', {
    method:'POST', headers:{'X-CSRF-Token': csrf},
    body:JSON.stringify({accountId:acctId, stashGold, itemsHex})
  });
  const j = await r.json();
  document.getElementById('msg').textContent = j.ok ? j.message : (j.error || 'Stash import failed');
  refreshAll();
}
async function sendAdminChat() {
  const channelId = Number(document.getElementById('chatChan').value);
  const text = document.getElementById('chatText').value.trim();
  if (!text) return;
  await fetch('/api/admin/chat', {
    method:'POST', headers:{'X-CSRF-Token': csrf},
    body:JSON.stringify({channelId, text})
  });
  document.getElementById('chatText').value = '';
  refreshAll();
}
async function runSelfUpdate(force) {
  const source = document.getElementById('updSrc').value.trim();
  const githubToken = document.getElementById('updToken').value.trim();
  const blake2b = document.getElementById('updHash').value.trim();
  document.getElementById('msg').textContent = 'Staging binary & running pre-flight --self-test...';
  const r = await fetch('/api/admin/update', {
    method:'POST', headers:{'X-CSRF-Token': csrf},
    body:JSON.stringify({source, githubToken, blake2b, force})
  });
  const j = await r.json();
  document.getElementById('msg').textContent = j.ok ? j.message : (j.error || 'Update failed');
  setTimeout(refreshAll, 1500);
}
async function refreshAll() {
  const s = await (await fetch('/api/admin/status')).json();
  if (!s.ok) return;
  csrf = s.csrfToken || csrf;
  document.getElementById('stats').innerHTML = [
    ['TCP Sessions', s.activeSessions], ['Active Games', s.activeGames],
    ['Accounts', s.totalAccounts], ['Characters', s.totalCharacters],
    ['Ledger Items', s.totalLedgerItems], ['Audit Events', s.securityAuditCount]
  ].map(x => `<div class="stat"><div class="k">${esc(x[0])}</div><div class="v">${esc(x[1])}</div></div>`).join('');
  const a = await (await fetch('/api/admin/accounts')).json();
  if (a.ok) {
    document.querySelector('#acctTbl tbody').innerHTML = a.accounts.map(x =>
      `<tr><td>${Number(x.id)}</td><td>${esc(x.username)}</td><td>${x.isBanned ? 'BANNED' : 'ACTIVE'}</td><td>${esc(x.banReason)}</td>` +
      `<td><button onclick="toggleBan(${Number(x.id)},${!x.isBanned})">${x.isBanned ? 'Unban' : 'Ban'}</button></td></tr>`).join('');
  }
  const c = await (await fetch('/api/admin/characters')).json();
  if (c.ok) {
    const cls = ['Warrior','Rogue','Sorcerer','Monk','Bard','Barbarian'];
    document.querySelector('#charTbl tbody').innerHTML = c.characters.map(x =>
      `<tr><td>${Number(x.id)}</td><td>${Number(x.accountId)}</td><td>${esc(x.heroName)}</td><td>${esc(cls[x.heroClass]||x.heroClass)}</td><td>${Number(x.heroLevel)}</td><td>${Number(x.diabloKillLevel)}</td><td>${Number(x.gold)}</td><td>${Number(x.ledgerItems)}</td>` +
      `<td><button onclick="promoteTorment(${Number(x.accountId)},${Number(x.id)})">Unlock Torment (Lvl 35)</button></td></tr>`).join('');
  }
  const ch = await (await fetch('/api/admin/chat')).json();
  if (ch.ok) {
    document.getElementById('chatFeed').innerHTML = ch.channels.map(chan =>
      chan.messages.map(m => `<div><span style="color:#a5b4fc">[#${esc(chan.name)}]</span> <b style="color:#e2b714">${esc(m.sender)} (L${Number(m.level)})</b>: ${esc(m.text)}</div>`).join('')
    ).join('');
  }
  const l = await (await fetch('/api/admin/audit')).json();
  if (l.ok) {
    document.querySelector('#auditTbl tbody').innerHTML = l.logs.slice(0, 30).map(x =>
      `<tr><td>${Number(x.id)}</td><td>${esc(x.severity)}</td><td>${esc(x.category)}</td><td>A:${Number(x.accountId)} C:${Number(x.charId)} G:${Number(x.gameId)}</td><td>${esc(x.details)}</td></tr>`).join('');
  }
}
refreshAll();
</script>
</body>
</html>)HTML";
}

} // namespace devilution::realm
