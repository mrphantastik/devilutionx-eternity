/**
 * @file realm_auth.hpp
 * @brief Cryptographic authentication (libsodium Argon2id + 256-bit Session Tokens)
 *        and Official Binary Attestation (BLAKE2b-256 keyed by REALM_ATTESTATION_KEY).
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "db/realm_database.hpp"
#include "net/packet_codec.hpp"
#include "protocol/realm_protocol.hpp"

namespace devilution::realm {

using AttestationKey = std::array<uint8_t, 32>;
using SessionToken = std::array<uint8_t, 32>;

struct ActiveSessionTokenRecord {
	int64_t accountId = 0;
	SessionToken token {};
	uint64_t issuedAtUnixSec = 0;
	uint64_t expiresAtUnixSec = 0;
	std::string remoteIp;
};

/**
 * @brief Returns the default compile-time 256-bit REALM_ATTESTATION_KEY shared with official
 * DX:Eternity release binaries (can be overridden at runtime via CLI/config).
 */
AttestationKey GetDefaultAttestationKey();

/**
 * @brief Computes the 256-bit BLAKE2b attestation HMAC:
 *   BLAKE2b-256(key = attestationKey,
 *               msg = serverNonce[32] || clientNonce[32] || clientTextHash[32] || clientBuildVersionLE[4])
 */
std::array<uint8_t, 32> ComputeClientAttestationHmac(
    const AttestationKey &attestationKey,
    const uint8_t serverNonce[32],
    const uint8_t clientNonce[32],
    const uint8_t clientTextSectionHash[32],
    uint32_t clientBuildVersion);

/**
 * @brief Derives the per-connection 256-bit session traffic MAC key after successful binary attestation:
 *   BLAKE2b-256(key = attestationKey,
 *               msg = "DXER-TRAFFIC-V1" || serverNonce[32] || clientNonce[32])
 */
SessionKey DeriveSessionTrafficKey(
    const AttestationKey &attestationKey,
    const uint8_t serverNonce[32],
    const uint8_t clientNonce[32]);

class RealmAuthManager {
public:
	explicit RealmAuthManager(RealmDatabase &db);

	void SetAttestationKey(const AttestationKey &key)
	{
		attestationKey_ = key;
	}

	const AttestationKey &GetAttestationKey() const
	{
		return attestationKey_;
	}

	/**
	 * @brief Optionally enforces an exact 32-byte `.text` section hash for official client builds.
	 */
	void SetExpectedClientTextHash(const std::array<uint8_t, 32> &expectedHash, bool enforce)
	{
		expectedTextHash_ = expectedHash;
		enforceTextHash_ = enforce;
	}

	/**
	 * @brief Verifies the client's `OP_ATTEST_HANDSHAKE_REQ` in constant time (`sodium_memcmp`)
	 * and derives the rotated per-connection `outNewTrafficKey` on success.
	 */
	RealmStatus VerifyAttestationHandshake(
	    const uint8_t serverNonce[32],
	    const AttestHandshakeRequestPayload &req,
	    const std::string &remoteIp,
	    SessionKey &outNewTrafficKey);

	/**
	 * @brief Validates username format: 3..24 ASCII characters in [A-Za-z0-9_-].
	 */
	static bool ValidateUsername(const std::string &username);

	/**
	 * @brief Validates password length: 8..63 UTF-8 bytes.
	 */
	static bool ValidatePassword(const std::string &password);

	/**
	 * @brief Registers a new account using libsodium Argon2id (`crypto_pwhash_str`) and issues a 256-bit session token.
	 */
	RealmStatus RegisterAccount(
	    const std::string &username,
	    const std::string &password,
	    const std::string &remoteIp,
	    int64_t &outAccountId,
	    SessionToken &outSessionToken,
	    std::string &outMessage);

	/**
	 * @brief Authenticates an existing account via libsodium Argon2id (`crypto_pwhash_str_verify`) and issues a 256-bit session token.
	 */
	RealmStatus LoginAccount(
	    const std::string &username,
	    const std::string &password,
	    const std::string &remoteIp,
	    int64_t &outAccountId,
	    SessionToken &outSessionToken,
	    std::string &outMessage);

	/**
	 * @brief Resumes an authenticated session using an active 256-bit session token verified in constant time.
	 */
	RealmStatus ResumeSession(
	    int64_t accountId,
	    const uint8_t tokenBytes[32],
	    const std::string &remoteIp,
	    std::string &outMessage);

	/**
	 * @brief Revokes any active session token for `accountId`.
	 */
	void RevokeSessionToken(int64_t accountId);

private:
	SessionToken IssueSessionToken(int64_t accountId, const std::string &remoteIp);

	RealmDatabase &db_;
	AttestationKey attestationKey_;
	std::array<uint8_t, 32> expectedTextHash_ {};
	bool enforceTextHash_ = false;
	std::unordered_map<int64_t, ActiveSessionTokenRecord> activeTokens_;
};

} // namespace devilution::realm
