/**
 * @file realm_auth.cpp
 * @brief Implementation of Argon2id password hashing, 256-bit session token management,
 *        and BLAKE2b official binary attestation for dx-eternity-realm.
 */
#include "auth/realm_auth.hpp"

#include <chrono>
#include <cstring>
#include <sodium.h>

namespace devilution::realm {

namespace {

constexpr uint64_t SessionTokenLifetimeSec = 86400ULL; // 24-hour session token lifetime
constexpr char TrafficKeyDomainPrefix[] = "DXER-TRAFFIC-V1";

uint64_t CurrentUnixTimestampSec()
{
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::seconds>(
	        std::chrono::system_clock::now().time_since_epoch())
	        .count());
}

} // namespace

AttestationKey GetDefaultAttestationKey()
{
	if (const char *envHex = std::getenv("DXER_ATTESTATION_KEY_HEX")) {
		AttestationKey customKey {};
		size_t binLen = 0;
		if (std::strlen(envHex) == 64
		    && sodium_hex2bin(customKey.data(), customKey.size(), envHex, 64, nullptr, &binLen, nullptr) == 0
		    && binLen == 32) {
			return customKey;
		}
	}

	// Default 256-bit compile-time REALM_ATTESTATION_KEY (override via DXER_ATTESTATION_KEY_HEX in production if repo is public)
	return AttestationKey {
		0x8F, 0x3A, 0xC1, 0x94, 0x2B, 0x7E, 0x5D, 0x10,
		0xE6, 0x42, 0x9B, 0x08, 0x77, 0xD3, 0x19, 0xA5,
		0x61, 0xF0, 0x3C, 0x88, 0x54, 0x2E, 0xB9, 0x72,
		0x0D, 0xC6, 0x4F, 0x91, 0xA8, 0x33, 0xE2, 0x5B
	};
}

std::array<uint8_t, 32> ComputeClientAttestationHmac(
    const AttestationKey &attestationKey,
    const uint8_t serverNonce[32],
    const uint8_t clientNonce[32],
    const uint8_t clientTextSectionHash[32],
    uint32_t clientBuildVersion)
{
	std::array<uint8_t, 32> outHmac {};
	crypto_generichash_state state;
	crypto_generichash_init(&state, attestationKey.data(), attestationKey.size(), outHmac.size());
	crypto_generichash_update(&state, serverNonce, 32);
	crypto_generichash_update(&state, clientNonce, 32);
	crypto_generichash_update(&state, clientTextSectionHash, 32);

	const uint8_t versionLe[4] = {
		static_cast<uint8_t>(clientBuildVersion & 0xFFU),
		static_cast<uint8_t>((clientBuildVersion >> 8) & 0xFFU),
		static_cast<uint8_t>((clientBuildVersion >> 16) & 0xFFU),
		static_cast<uint8_t>((clientBuildVersion >> 24) & 0xFFU),
	};
	crypto_generichash_update(&state, versionLe, sizeof(versionLe));
	crypto_generichash_final(&state, outHmac.data(), outHmac.size());
	return outHmac;
}

SessionKey DeriveSessionTrafficKey(
    const AttestationKey &attestationKey,
    const uint8_t serverNonce[32],
    const uint8_t clientNonce[32])
{
	SessionKey derivedKey {};
	crypto_generichash_state state;
	crypto_generichash_init(&state, attestationKey.data(), attestationKey.size(), derivedKey.size());
	crypto_generichash_update(&state, reinterpret_cast<const uint8_t *>(TrafficKeyDomainPrefix), sizeof(TrafficKeyDomainPrefix) - 1);
	crypto_generichash_update(&state, serverNonce, 32);
	crypto_generichash_update(&state, clientNonce, 32);
	crypto_generichash_final(&state, derivedKey.data(), derivedKey.size());
	return derivedKey;
}

RealmAuthManager::RealmAuthManager(RealmDatabase &db)
    : db_(db)
    , attestationKey_(GetDefaultAttestationKey())
{
}

RealmStatus RealmAuthManager::VerifyAttestationHandshake(
    const uint8_t serverNonce[32],
    const AttestHandshakeRequestPayload &req,
    const std::string &remoteIp,
    SessionKey &outNewTrafficKey)
{
	if (enforceTextHash_) {
		if (sodium_memcmp(req.clientTextSectionHash, expectedTextHash_.data(), expectedTextHash_.size()) != 0) {
			db_.LogSecurityEvent(0, 0, 0, "ERROR", "ATTEST_TEXT_HASH_MISMATCH",
			    "Client .text section hash mismatch from " + remoteIp);
			return RealmStatus::ERR_ATTESTATION_FAILED;
		}
	}

	const std::array<uint8_t, 32> expectedHmac = ComputeClientAttestationHmac(
	    attestationKey_,
	    serverNonce,
	    req.clientNonce,
	    req.clientTextSectionHash,
	    req.clientBuildVersion);

	if (sodium_memcmp(req.attestationHmac, expectedHmac.data(), expectedHmac.size()) != 0) {
		db_.LogSecurityEvent(0, 0, 0, "ERROR", "ATTEST_HMAC_FAILED",
		    "Official binary attestation BLAKE2b-256 HMAC verification failed from " + remoteIp);
		return RealmStatus::ERR_ATTESTATION_FAILED;
	}

	outNewTrafficKey = DeriveSessionTrafficKey(attestationKey_, serverNonce, req.clientNonce);
	return RealmStatus::OK;
}

bool RealmAuthManager::ValidateUsername(const std::string &username)
{
	if (username.size() < 3 || username.size() > 24)
		return false;
	for (char c : username) {
		const bool isAlphaNum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		const bool isAllowedSymbol = (c == '_' || c == '-');
		if (!isAlphaNum && !isAllowedSymbol)
			return false;
	}
	return true;
}

bool RealmAuthManager::ValidatePassword(const std::string &password)
{
	return password.size() >= 8 && password.size() <= 63;
}

RealmStatus RealmAuthManager::RegisterAccount(
    const std::string &username,
    const std::string &password,
    const std::string &remoteIp,
    int64_t &outAccountId,
    SessionToken &outSessionToken,
    std::string &outMessage)
{
	outAccountId = 0;
	outSessionToken.fill(0);

	if (!ValidateUsername(username)) {
		outMessage = "Username must be 3..24 characters [A-Za-z0-9_-]";
		return RealmStatus::ERR_AUTH_INVALID_CREDENTIALS;
	}
	if (!ValidatePassword(password)) {
		outMessage = "Password must be 8..63 characters";
		return RealmStatus::ERR_AUTH_INVALID_CREDENTIALS;
	}

	if (db_.FindAccountByUsername(username).has_value()) {
		outMessage = "Account username already registered";
		return RealmStatus::ERR_AUTH_ACCOUNT_EXISTS;
	}

	char hashedPassword[crypto_pwhash_STRBYTES] {};
	if (crypto_pwhash_str(
	        hashedPassword,
	        password.c_str(),
	        password.size(),
	        crypto_pwhash_OPSLIMIT_INTERACTIVE,
	        crypto_pwhash_MEMLIMIT_INTERACTIVE)
	    != 0) {
		outMessage = "Argon2id password hashing failed";
		return RealmStatus::ERR_INTERNAL_DATABASE;
	}

	RealmStatus dbStatus = RealmStatus::OK;
	if (!db_.CreateAccount(username, hashedPassword, outAccountId, dbStatus)) {
		outMessage = (dbStatus == RealmStatus::ERR_AUTH_ACCOUNT_EXISTS)
		    ? "Account username already registered"
		    : "Database error creating account";
		return dbStatus;
	}

	outSessionToken = IssueSessionToken(outAccountId, remoteIp);
	outMessage = "Account registered and authenticated";

	db_.LogSecurityEvent(outAccountId, 0, 0, "INFO", "AUTH_REGISTER_OK",
	    "Registered account '" + username + "' from " + remoteIp);
	return RealmStatus::OK;
}

RealmStatus RealmAuthManager::LoginAccount(
    const std::string &username,
    const std::string &password,
    const std::string &remoteIp,
    int64_t &outAccountId,
    SessionToken &outSessionToken,
    std::string &outMessage)
{
	outAccountId = 0;
	outSessionToken.fill(0);

	if (!ValidateUsername(username) || !ValidatePassword(password)) {
		outMessage = "Invalid username or password";
		return RealmStatus::ERR_AUTH_INVALID_CREDENTIALS;
	}

	const std::optional<AccountRecord> acct = db_.FindAccountByUsername(username);
	if (!acct.has_value()) {
		db_.LogSecurityEvent(0, 0, 0, "WARN", "AUTH_LOGIN_UNKNOWN_USER",
		    "Login attempt for non-existent username '" + username + "' from " + remoteIp);
		outMessage = "Invalid username or password";
		return RealmStatus::ERR_AUTH_INVALID_CREDENTIALS;
	}

	if (crypto_pwhash_str_verify(acct->passwordHash.c_str(), password.c_str(), password.size()) != 0) {
		db_.LogSecurityEvent(acct->id, 0, 0, "WARN", "AUTH_LOGIN_BAD_PASSWORD",
		    "Failed password verification for '" + acct->username + "' from " + remoteIp);
		outMessage = "Invalid username or password";
		return RealmStatus::ERR_AUTH_INVALID_CREDENTIALS;
	}

	if (acct->isBanned) {
		db_.LogSecurityEvent(acct->id, 0, 0, "WARN", "AUTH_LOGIN_BANNED",
		    "Rejected login for banned account '" + acct->username + "' from " + remoteIp);
		outMessage = acct->banReason.empty() ? "Account is banned" : ("Banned: " + acct->banReason);
		return RealmStatus::ERR_AUTH_ACCOUNT_BANNED;
	}

	db_.UpdateLastLogin(acct->id);
	outAccountId = acct->id;
	outSessionToken = IssueSessionToken(acct->id, remoteIp);
	outMessage = "Authenticated";

	db_.LogSecurityEvent(acct->id, 0, 0, "INFO", "AUTH_LOGIN_OK",
	    "Authenticated account '" + acct->username + "' from " + remoteIp);
	return RealmStatus::OK;
}

RealmStatus RealmAuthManager::ResumeSession(
    int64_t accountId,
    const uint8_t tokenBytes[32],
    const std::string &remoteIp,
    std::string &outMessage)
{
	if (accountId <= 0) {
		outMessage = "Invalid account ID";
		return RealmStatus::ERR_AUTH_INVALID_CREDENTIALS;
	}

	auto it = activeTokens_.find(accountId);
	if (it == activeTokens_.end()) {
		outMessage = "No active session token for account";
		return RealmStatus::ERR_AUTH_INVALID_CREDENTIALS;
	}

	const uint64_t nowSec = CurrentUnixTimestampSec();
	if (nowSec >= it->second.expiresAtUnixSec) {
		activeTokens_.erase(it);
		outMessage = "Session token expired";
		return RealmStatus::ERR_AUTH_INVALID_CREDENTIALS;
	}

	if (sodium_memcmp(it->second.token.data(), tokenBytes, it->second.token.size()) != 0) {
		db_.LogSecurityEvent(accountId, 0, 0, "WARN", "AUTH_RESUME_BAD_TOKEN",
		    "Invalid session resume token for accountId=" + std::to_string(accountId) + " from " + remoteIp);
		outMessage = "Invalid session token";
		return RealmStatus::ERR_AUTH_INVALID_CREDENTIALS;
	}

	const std::optional<AccountRecord> acct = db_.FindAccountById(accountId);
	if (!acct.has_value()) {
		activeTokens_.erase(it);
		outMessage = "Account not found";
		return RealmStatus::ERR_AUTH_INVALID_CREDENTIALS;
	}
	if (acct->isBanned) {
		activeTokens_.erase(it);
		outMessage = acct->banReason.empty() ? "Account is banned" : ("Banned: " + acct->banReason);
		return RealmStatus::ERR_AUTH_ACCOUNT_BANNED;
	}

	db_.UpdateLastLogin(accountId);
	outMessage = "Session resumed";
	return RealmStatus::OK;
}

void RealmAuthManager::RevokeSessionToken(int64_t accountId)
{
	activeTokens_.erase(accountId);
}

SessionToken RealmAuthManager::IssueSessionToken(int64_t accountId, const std::string &remoteIp)
{
	ActiveSessionTokenRecord rec {};
	rec.accountId = accountId;
	randombytes_buf(rec.token.data(), rec.token.size());
	rec.issuedAtUnixSec = CurrentUnixTimestampSec();
	rec.expiresAtUnixSec = rec.issuedAtUnixSec + SessionTokenLifetimeSec;
	rec.remoteIp = remoteIp;

	activeTokens_[accountId] = rec;
	return rec.token;
}

} // namespace devilution::realm
