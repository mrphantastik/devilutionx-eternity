#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <random>

namespace devilution::realm {

/**
 * @brief Exact 1:1 Borland C/C++ Linear Congruential Generator (LCG) matching
 *        DevilutionX 1.5.4 / DX:Eternity (Source/engine/random.cpp).
 *
 * Multiplier: 0x015A4E35 (22695477)
 * Increment:  1
 * Modulus:    2^32 (uint32_t overflow)
 */
class LcgEngine {
public:
	static constexpr uint32_t kMultiplier = 0x015A4E35U;
	static constexpr uint32_t kIncrement = 1U;

	constexpr LcgEngine() noexcept = default;
	constexpr explicit LcgEngine(uint32_t initialSeed) noexcept
	    : sglGameSeed_(initialSeed)
	{
	}

	constexpr void SetRndSeed(uint32_t seed) noexcept
	{
		sglGameSeed_ = seed;
	}

	[[nodiscard]] constexpr uint32_t GetLCGEngineState() const noexcept
	{
		return sglGameSeed_;
	}

	constexpr uint32_t GenerateSeed() noexcept
	{
		sglGameSeed_ = static_cast<uint32_t>(kMultiplier * sglGameSeed_ + kIncrement);
		return sglGameSeed_;
	}

	constexpr void DiscardRandomValues(unsigned count) noexcept
	{
		while (count != 0) {
			GenerateSeed();
			count--;
		}
	}

	[[nodiscard]] constexpr int32_t AdvanceRndSeed() noexcept
	{
		const int32_t seed = static_cast<int32_t>(GenerateSeed());
		if (seed == std::numeric_limits<int32_t>::min())
			return std::numeric_limits<int32_t>::min();
		return seed < 0 ? -seed : seed;
	}

	[[nodiscard]] constexpr int32_t GenerateRnd(int32_t v) noexcept
	{
		if (v <= 0)
			return 0;
		if (v <= 0x7FFF)
			return (AdvanceRndSeed() >> 16) % v;
		return AdvanceRndSeed() % v;
	}

	[[nodiscard]] constexpr bool FlipCoin(unsigned frequency = 2) noexcept
	{
		return GenerateRnd(static_cast<int32_t>(frequency)) == 0;
	}

	template <typename T>
	[[nodiscard]] constexpr T PickRandomlyAmong(std::initializer_list<T> values) noexcept
	{
		const int32_t index = std::max<int32_t>(GenerateRnd(static_cast<int32_t>(values.size())), 0);
		return *(values.begin() + index);
	}

private:
	uint32_t sglGameSeed_ = 0;
};

} // namespace devilution::realm
