/**
 * @file realm_admin_server.hpp
 * @brief Embedded C++20 Admin HTTP Control Interface for dx-eternity-realm (Phase 7.1).
 *
 * Provides a zero-dependency, non-blocking HTTP control listener inside the native C++20
 * server binary, protected by constant-time libsodium BLAKE2b HMAC cookies / Bearer tokens.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "db/realm_database.hpp"
#include "net/realm_server.hpp"

namespace devilution::realm {

constexpr uint16_t DefaultAdminPort = 6113U;

class RealmAdminServer {
public:
	RealmAdminServer(RealmDatabase &db, RealmServer &realmServer);
	~RealmAdminServer();

	RealmAdminServer(const RealmAdminServer &) = delete;
	RealmAdminServer &operator=(const RealmAdminServer &) = delete;

	/**
	 * @brief Binds the embedded admin listener on `bindIp:port` (pass `port = 0` for ephemeral port).
	 *        If `adminSecretToken` is empty, a random 32-hex-character token is generated on startup.
	 */
	bool Start(const std::string &bindIp, uint16_t port, const std::string &adminSecretToken, std::string &outError);

	/**
	 * @brief Polls and processes pending admin HTTP requests non-blockingly (`timeoutMs`).
	 */
	void PollOnce(int timeoutMs);

	/**
	 * @brief Stops the admin listener socket.
	 */
	void Stop();

	uint16_t BoundPort() const
	{
		return boundPort_;
	}

	const std::string &AdminToken() const
	{
		return adminToken_;
	}

	const std::string &CsrfToken() const
	{
		return csrfToken_;
	}

	void SetExecutablePath(const std::string &exePath)
	{
		executablePath_ = exePath;
	}

	bool ShouldRestartForUpdate() const
	{
		return shouldRestartForUpdate_;
	}

private:
	void HandleClientConnection(SocketHandle clientSock, const std::string &remoteIp);
	bool IsRequestAuthorized(const std::string &rawHeaders, bool isMutatingPost) const;
	std::string ComputeSessionCookieHmac() const;

	std::string BuildStatusJson() const;
	std::string BuildAccountsJson() const;
	std::string BuildCharactersJson() const;
	std::string BuildAuditLogJson() const;
	std::string BuildEmbeddedDashboardHtml() const;

	RealmDatabase &db_;
	RealmServer &realmServer_;
	SocketHandle listenSocket_ = InvalidSocketHandle;
	uint16_t boundPort_ = 0;
	std::string adminToken_;
	std::string csrfToken_;
	std::string executablePath_;
	bool shouldRestartForUpdate_ = false;
	std::array<uint8_t, 32> cookieSigningKey_ {};
};

} // namespace devilution::realm
