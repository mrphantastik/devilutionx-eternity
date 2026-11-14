/**
 * @file packet_codec.hpp
 * @brief Keyed BLAKE2b-128 packet authentication and framing for dx-eternity-realm.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protocol/realm_protocol.hpp"

namespace devilution::realm {

using SessionKey = std::array<uint8_t, 32>;

/**
 * @brief Default initial pre-authentication transport key (overridden per session after handshake).
 */
SessionKey GetDefaultHandshakeKey();

/**
 * @brief Computes the 16-byte BLAKE2b MAC for a packet header and payload using `key`.
 */
std::array<uint8_t, PacketMacSize> ComputePacketMac(
    const RealmPacketHeader &headerWithoutMac,
    const uint8_t *payload,
    size_t payloadLen,
    const SessionKey &key);

/**
 * @brief Serializes a complete wire frame (32-byte header with computed MAC + payload bytes).
 */
std::vector<uint8_t> EncodePacketFrame(
    RealmOpcode opcode,
    uint32_t sequenceNumber,
    const void *payload,
    size_t payloadLen,
    const SessionKey &key);

/**
 * @brief Verifies the 16-byte BLAKE2b MAC on a received packet in constant time (`sodium_memcmp`).
 */
bool VerifyPacketMac(
    const RealmPacketHeader &header,
    const uint8_t *payload,
    size_t payloadLen,
    const SessionKey &key);

} // namespace devilution::realm
