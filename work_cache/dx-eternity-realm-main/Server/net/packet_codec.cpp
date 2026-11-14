/**
 * @file packet_codec.cpp
 * @brief Implementation of libsodium BLAKE2b-128 packet authentication and wire framing.
 */
#include "net/packet_codec.hpp"

#include <cstring>
#include <sodium.h>

namespace devilution::realm {

SessionKey GetDefaultHandshakeKey()
{
	// Deterministic domain-separation key for initial pre-login packets (OP_PING, OP_AUTH_CHALLENGE_REQ)
	static constexpr SessionKey kHandshakeKey = {
		0x44, 0x58, 0x2D, 0x45, 0x54, 0x45, 0x52, 0x4E,
		0x49, 0x54, 0x59, 0x2D, 0x52, 0x45, 0x41, 0x4C,
		0x4D, 0x2D, 0x56, 0x31, 0x2D, 0x48, 0x41, 0x4E,
		0x44, 0x53, 0x48, 0x41, 0x4B, 0x45, 0x2D, 0x4B
	};
	return kHandshakeKey;
}

std::array<uint8_t, PacketMacSize> ComputePacketMac(
    const RealmPacketHeader &headerWithoutMac,
    const uint8_t *payload,
    size_t payloadLen,
    const SessionKey &key)
{
	RealmPacketHeader canonicalHeader = headerWithoutMac;
	std::memset(canonicalHeader.mac, 0, sizeof(canonicalHeader.mac));

	crypto_generichash_state state;
	crypto_generichash_init(&state, key.data(), key.size(), PacketMacSize);
	crypto_generichash_update(&state, reinterpret_cast<const unsigned char *>(&canonicalHeader), sizeof(canonicalHeader));
	if (payload != nullptr && payloadLen > 0) {
		crypto_generichash_update(&state, payload, payloadLen);
	}

	std::array<uint8_t, PacketMacSize> tag {};
	crypto_generichash_final(&state, tag.data(), tag.size());
	return tag;
}

std::vector<uint8_t> EncodePacketFrame(
    RealmOpcode opcode,
    uint32_t sequenceNumber,
    const void *payload,
    size_t payloadLen,
    const SessionKey &key)
{
	RealmPacketHeader hdr {};
	hdr.magic = ProtocolMagic;
	hdr.version = ProtocolVersion;
	hdr.opcode = static_cast<uint16_t>(opcode);
	hdr.payloadLength = static_cast<uint32_t>(payloadLen);
	hdr.sequenceNumber = sequenceNumber;
	std::memset(hdr.mac, 0, sizeof(hdr.mac));

	const auto *payloadBytes = static_cast<const uint8_t *>(payload);
	const auto mac = ComputePacketMac(hdr, payloadBytes, payloadLen, key);
	std::memcpy(hdr.mac, mac.data(), PacketMacSize);

	std::vector<uint8_t> frame(sizeof(RealmPacketHeader) + payloadLen);
	std::memcpy(frame.data(), &hdr, sizeof(RealmPacketHeader));
	if (payloadBytes != nullptr && payloadLen > 0) {
		std::memcpy(frame.data() + sizeof(RealmPacketHeader), payloadBytes, payloadLen);
	}
	return frame;
}

bool VerifyPacketMac(
    const RealmPacketHeader &header,
    const uint8_t *payload,
    size_t payloadLen,
    const SessionKey &key)
{
	const auto expected = ComputePacketMac(header, payload, payloadLen, key);
	return sodium_memcmp(header.mac, expected.data(), PacketMacSize) == 0;
}

} // namespace devilution::realm
