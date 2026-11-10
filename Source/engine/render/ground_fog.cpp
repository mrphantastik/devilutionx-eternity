/**
 * @file ground_fog.cpp
 *
 * Implementation of 3D Isometric Volumetric Ground Fog, Interactive Half-Tile Entity Dispersion,
 * and Caves 3D Boiling Lava Bubbles.
 */
#include "engine/render/ground_fog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <SDL.h>

#include "diablo.h"
#include "effects.h"
#include "engine/palette.h"
#include "engine/render/scrollrt.h"
#include "levels/gendung.h"
#include "lighting.h"
#include "missiles.h"
#include "monster.h"
#include "options.h"
#include "player.h"

namespace devilution {

namespace {

struct HalfFogNode {
	uint8_t density; // 0 = completely dispersed, up to LevelFogConfig::maxDensity
	int8_t velX;     // Swirl/push displacement X (-127..127)
	int8_t velY;     // Swirl/push displacement Y (-127..127)
};

struct LevelFogConfig {
	bool enabled;
	uint8_t spawnChancePercent; // Chance (0..100%) for this dungeon floor to spawn fog (100% for testing)
	uint8_t maxDensity;         // Target ambient density (0..255)
	uint8_t intensityCutoff;    // Cloud shape threshold (decoupled from wallFade)
	int uvScale;                // Texels per tile (lower = larger cloud banks, higher = tighter clusters)
	uint8_t crestShade;         // Brighter palette index for lit upper 3D billow domes
	uint8_t coreShade;          // Mid-tone palette index for mid cloud body
	uint8_t troughShade;        // Darker palette index for base mist & cast underside drop-shadows
	bool isDarkSmoke;           // True in Hell: uses raw dark charcoal (252/254) & Pure Black (0) soot
	bool allowHeavyBlend;       // True in Crypt/Hell: allows Blend37 top-tier opacity
	bool hasPulse;              // True in Hive/Nest: slowly pulses bio-spore density
	int driftDivX;              // Sub-texel horizontal drift divisor (115..240)
	int driftDivY;              // Sub-texel vertical drift divisor (120..310; negative = rising draft)
};

struct LavaBubble {
	bool active;
	Point tile;
	int8_t offsetX;      // Pixel offset relative to tile diamond center (-20..20)
	int8_t offsetY;      // Pixel offset relative to tile diamond center (-10..10)
	bool snappedToLava;  // True once verified/snapped onto liquid magma pixel (1..31)
	bool burstSoundSent; // True once burst audio has been triggered
	uint8_t stage;       // 0..2 = swelling 3D blister dome, 3 = bursting crater + droplets, 4 = subsiding ring
	uint8_t tickInStage; // Sub-tick counter within current stage
	uint8_t maxRadX;     // Horizontal radius (6..9 px)
	uint8_t maxRadY;     // Vertical radius (4..6 px)
	uint32_t seed;       // Deterministic droplet trajectory seed
};

constexpr size_t MaxLavaBubbles = 64;

bool gNoiseInitialized = false;
bool gLevelHasFog = false;
uint32_t gLavaBubbleRngState = 0xA341316CU;

// Precomputed 256x256 seamless quintic C2-continuous 4-octave cumulus FBM LUT (0..255) and domain-warp LUT (0..255)
std::array<std::array<uint8_t, 256>, 256> gFogNoiseLut;
std::array<std::array<uint8_t, 256>, 256> gFogWarpLut;

// Continuous half-tile (0.5-tile) resolution dispersion grid so players, monsters, and spells
// carve a smooth +0.5-tile wake on each side across tile boundaries with zero disconnected circles.
constexpr int HalfGridW = MAXDUNX * 2 + 3;
constexpr int HalfGridH = MAXDUNY * 2 + 3;
std::array<std::array<HalfFogNode, HalfGridH>, HalfGridW> gFogHalfGrid;

// Active boiling lava bubbles for Caves (DTYPE_CAVES)
std::array<LavaBubble, MaxLavaBubbles> gLavaBubbles;

/**
 * @brief Distinct per-dungeon volumetric fog/smoke/steam/spore presets.
 */
LevelFogConfig GetLevelFogConfig()
{
	switch (leveltype) {
	case DTYPE_CATHEDRAL:
		// Sparse cold spectral silver-blue mist
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/165,
			/*intensityCutoff=*/86,
			/*uvScale=*/22,
			/*crestShade=*/PAL16_BLUE + 2,
			/*coreShade=*/PAL16_GRAY + 5,
			/*troughShade=*/PAL16_GRAY + 10,
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*hasPulse=*/false,
			/*driftDivX=*/210,
			/*driftDivY=*/290
		};
	case DTYPE_CATACOMBS:
		// Medium warm dusty bone-beige crypt haze
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/190,
			/*intensityCutoff=*/76,
			/*uvScale=*/24,
			/*crestShade=*/PAL16_BEIGE + 2,
			/*coreShade=*/PAL16_BEIGE + 6,
			/*troughShade=*/PAL16_GRAY + 10,
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*hasPulse=*/false,
			/*driftDivX=*/185,
			/*driftDivY=*/260
		};
	case DTYPE_CAVES:
		// Upward-rising geothermal steam
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/200,
			/*intensityCutoff=*/72,
			/*uvScale=*/26,
			/*crestShade=*/PAL16_GRAY + 2,
			/*coreShade=*/PAL16_BEIGE + 4,
			/*troughShade=*/PAL16_GRAY + 9,
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*hasPulse=*/false,
			/*driftDivX=*/140,
			/*driftDivY=*/-155
		};
	case DTYPE_HELL:
		// Dark charcoal/black rising soot smoke:
		// Palette index 0 is Pure Black (0,0,0), PAL16_GRAY + 14 (254) is darkest charcoal soot,
		// and PAL16_GRAY + 12 (252) is dark ash-charcoal (never PAL16_GRAY + 15 = 255 system white!).
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/225,
			/*intensityCutoff=*/64,
			/*uvScale=*/24,
			/*crestShade=*/PAL16_GRAY + 12, // 252: dark ash-charcoal top dome
			/*coreShade=*/PAL16_GRAY + 14,  // 254: darkest charcoal soot body
			/*troughShade=*/0,              // 0: Pure Black (0,0,0) soot trough
			/*isDarkSmoke=*/true,
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/false,
			/*driftDivX=*/115,
			/*driftDivY=*/-120
		};
	case DTYPE_CRYPT:
		// Heavy, dense tomb fog blanket with large billowing banks
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/250,
			/*intensityCutoff=*/54,
			/*uvScale=*/20,
			/*crestShade=*/PAL16_GRAY + 2,
			/*coreShade=*/PAL16_GRAY + 5,
			/*troughShade=*/PAL16_GRAY + 10,
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/false,
			/*driftDivX=*/240,
			/*driftDivY=*/310
		};
	case DTYPE_NEST:
		// Slowly pulsing yellowish-green bio-spore clusters
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/205,
			/*intensityCutoff=*/70,
			/*uvScale=*/34,
			/*crestShade=*/PAL16_YELLOW + 2,
			/*coreShade=*/PAL16_YELLOW + 6,
			/*troughShade=*/PAL16_GRAY + 10,
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*hasPulse=*/true,
			/*driftDivX=*/165,
			/*driftDivY=*/-210
		};
	default:
		return { false, 0, 0, 80, 24, 0, 0, 0, false, false, false, 200, 260 };
	}
}

inline uint32_t FastHash2D(int x, int y)
{
	uint32_t h = static_cast<uint32_t>(x & 0xFF) * 374761393U + static_cast<uint32_t>(y & 0xFF) * 668265263U;
	h = (h ^ (h >> 13)) * 1274126177U;
	return (h ^ (h >> 16)) & 0xFFU;
}

inline uint32_t NextVisualRng()
{
	gLavaBubbleRngState ^= gLavaBubbleRngState << 13;
	gLavaBubbleRngState ^= gLavaBubbleRngState >> 17;
	gLavaBubbleRngState ^= gLavaBubbleRngState << 5;
	return gLavaBubbleRngState;
}

/**
 * @brief Deterministic per-floor hash from glSeedTbl[currlevel] so fog spawn % never
 * calls GenerateRnd() or perturbs level generation / multiplayer sync.
 */
uint32_t GetFloorFogSeedHash()
{
	uint32_t h = (currlevel < NUMLEVELS) ? static_cast<uint32_t>(glSeedTbl[currlevel]) : 0x811C9DC5U;
	h ^= static_cast<uint32_t>(currlevel) * 0x9E3779B9U;
	h ^= static_cast<uint32_t>(leveltype) * 0x85EBCA6BU;
	h ^= h >> 16;
	h *= 0x85EBCA6BU;
	h ^= h >> 13;
	h *= 0xC2B2AE35U;
	h ^= h >> 16;
	return h;
}

/**
 * @brief Quintic C2-continuous value noise (zero second-derivative creases at lattice edges).
 */
float QuinticValueNoise(float x, float y, int period, int seedOffset = 0)
{
	const int xi = static_cast<int>(std::floor(x));
	const int yi = static_cast<int>(std::floor(y));
	const float xf = x - static_cast<float>(xi);
	const float yf = y - static_cast<float>(yi);

	// 6t^5 - 15t^4 + 10t^3 for smooth C2 spherical dome curvature
	const float u = xf * xf * xf * (xf * (xf * 6.0F - 15.0F) + 10.0F);
	const float v = yf * yf * yf * (yf * (yf * 6.0F - 15.0F) + 10.0F);

	const int x0 = ((xi % period) + period) % period;
	const int y0 = ((yi % period) + period) % period;
	const int x1 = (x0 + 1) % period;
	const int y1 = (y0 + 1) % period;

	const float n00 = static_cast<float>(FastHash2D(x0 + seedOffset, y0 + seedOffset));
	const float n10 = static_cast<float>(FastHash2D(x1 + seedOffset, y0 + seedOffset));
	const float n01 = static_cast<float>(FastHash2D(x0 + seedOffset, y1 + seedOffset));
	const float n11 = static_cast<float>(FastHash2D(x1 + seedOffset, y1 + seedOffset));

	const float nx0 = n00 + u * (n10 - n00);
	const float nx1 = n01 + u * (n11 - n01);
	return nx0 + v * (nx1 - nx0);
}

void EnsureFogNoiseInitialized()
{
	if (gNoiseInitialized)
		return;

	float raw[256][256];
	float minV = 255.0F;
	float maxV = 0.0F;

	for (int y = 0; y < 256; y++) {
		for (int x = 0; x < 256; x++) {
			// Quintic C2-continuous 4-octave cumulus FBM (no fabs crease lines)
			const float n1 = QuinticValueNoise(x / 64.0F, y / 64.0F, 4, 0);
			const float n2 = QuinticValueNoise(x / 32.0F, y / 32.0F, 8, 41);
			const float n3 = QuinticValueNoise(x / 16.0F, y / 16.0F, 16, 97);
			const float n4 = QuinticValueNoise(x / 8.0F, y / 8.0F, 32, 163);

			// Soft rounded dome shaping on the primary octaves
			const float dome1 = (n1 * n1) / 255.0F;
			const float dome2 = (n2 * n2) / 255.0F;
			const float v = (n1 * 0.30F + dome1 * 0.16F) + (n2 * 0.22F + dome2 * 0.10F) + n3 * 0.15F + n4 * 0.07F;
			raw[y][x] = v;
			minV = std::min(minV, v);
			maxV = std::max(maxV, v);

			// Secondary domain-warp LUT for organic curling motion
			const float w1 = QuinticValueNoise(x / 32.0F, y / 32.0F, 8, 151);
			const float w2 = QuinticValueNoise(x / 16.0F, y / 16.0F, 16, 211);
			gFogWarpLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(w1 * 0.65F + w2 * 0.35F), 0, 255));
		}
	}

	const float span = std::max(1.0F, maxV - minV);
	for (int y = 0; y < 256; y++) {
		for (int x = 0; x < 256; x++) {
			float norm = std::clamp((raw[y][x] - minV) / span, 0.0F, 1.0F);
			norm = norm * norm * (3.0F - 2.0F * norm);
			const int stretched = static_cast<int>(norm * 255.0F + 0.5F);
			gFogNoiseLut[y][x] = static_cast<uint8_t>(std::clamp(stretched, 0, 255));
		}
	}
	gNoiseInitialized = true;
}

/**
 * @brief Fast 2D bilinear sampler on a 256x256 periodic LUT using 24.8 fixed-point coordinates.
 * Eliminates 20Hz / integer-texel stepping so fog drifts smoothly at 60Hz/144Hz/240Hz.
 */
inline int SampleLutBilinear(const std::array<std::array<uint8_t, 256>, 256> &lut, int uFixed, int vFixed)
{
	const int u0 = (uFixed >> 8) & 0xFF;
	const int v0 = (vFixed >> 8) & 0xFF;
	const int u1 = (u0 + 1) & 0xFF;
	const int v1 = (v0 + 1) & 0xFF;
	const int fu = uFixed & 0xFF;
	const int fv = vFixed & 0xFF;

	const int s00 = lut[v0][u0];
	const int s10 = lut[v0][u1];
	const int s01 = lut[v1][u0];
	const int s11 = lut[v1][u1];

	const int row0 = (s00 << 8) + fu * (s10 - s00);
	const int row1 = (s01 << 8) + fu * (s11 - s01);
	return ((row0 << 8) + fv * (row1 - row0)) >> 16;
}

/**
 * @brief Returns true if a Caves tile is a molten lava river/pool tile.
 * Matches PlaceCaveLights() in Source/levels/drlg_l3.cpp plus flat solid pool pieces.
 */
inline bool IsCavesLavaTile(Point tile)
{
	if (leveltype != DTYPE_CAVES || !InDungeonBounds(tile))
		return false;
	const uint16_t piece = dPiece[tile.x][tile.y];
	if (piece == 0)
		return false;
	if ((piece >= 55 && piece <= 146) || piece == 149 || piece == 151 || (piece >= 153 && piece <= 160))
		return true;
	if (TileHasAny(piece, TileProperties::Solid)) {
		const MICROS &micros = DPieceMicros[piece];
		if (micros.mt[0] == 0 && micros.mt[1] == 0)
			return false;
		for (int i = 2; i < MicroTileLen; i++) {
			if (micros.mt[i] != 0)
				return false;
		}
		return true;
	}
	return false;
}

/**
 * @brief Returns true if a tile can hold ground fog/steam/smoke.
 */
inline bool IsFogPassableTile(Point tile)
{
	if (!InDungeonBounds(tile))
		return false;
	const uint16_t piece = dPiece[tile.x][tile.y];
	if (piece == 0)
		return false;
	if (!TileHasAny(piece, TileProperties::Solid))
		return true;
	if (leveltype == DTYPE_CAVES && IsCavesLavaTile(tile))
		return true;

	if (leveltype == DTYPE_CAVES || leveltype == DTYPE_HELL || leveltype == DTYPE_NEST) {
		const MICROS &micros = DPieceMicros[piece];
		if (micros.mt[0] == 0 && micros.mt[1] == 0)
			return false;
		for (int i = 2; i < MicroTileLen; i++) {
			if (micros.mt[i] != 0)
				return false;
		}
		return true;
	}
	return false;
}

inline uint8_t Blend37(uint8_t fg, uint8_t bg)
{
	const uint8_t half = paletteTransparencyLookup[fg][bg];
	const uint8_t qtr = paletteTransparencyLookup[bg][half];
	return paletteTransparencyLookup[half][qtr]; // 37.5% fg, 62.5% bg
}

inline uint8_t Blend31(uint8_t fg, uint8_t bg)
{
	const uint8_t half = paletteTransparencyLookup[fg][bg];
	const uint8_t qtr = paletteTransparencyLookup[bg][half];
	const uint8_t eighth = paletteTransparencyLookup[bg][qtr];
	const uint8_t thirtySeven = paletteTransparencyLookup[half][qtr];
	return paletteTransparencyLookup[thirtySeven][eighth]; // 31.25% fg, 68.75% bg
}

inline uint8_t Blend25(uint8_t fg, uint8_t bg)
{
	const uint8_t mid = paletteTransparencyLookup[fg][bg];
	return paletteTransparencyLookup[bg][mid]; // 25.0% fg, 75.0% bg
}

inline uint8_t Blend19(uint8_t fg, uint8_t bg)
{
	const uint8_t qtr = Blend25(fg, bg);
	const uint8_t eighth = paletteTransparencyLookup[bg][qtr];
	return paletteTransparencyLookup[qtr][eighth]; // 18.75% fg, 81.25% bg
}

inline uint8_t Blend12(uint8_t fg, uint8_t bg)
{
	const uint8_t qtr = Blend25(fg, bg);
	return paletteTransparencyLookup[bg][qtr]; // 12.5% fg, 87.5% bg
}

inline uint8_t Blend6(uint8_t fg, uint8_t bg)
{
	const uint8_t eighth = Blend12(fg, bg);
	return paletteTransparencyLookup[bg][eighth]; // 6.25% fg, 93.75% bg
}

/**
 * @brief Carves a continuous radial dispersion wake in the 0.5-tile resolution grid
 * extending +0.5 tile on each side of the entity/missile (radius = 2.0 half-tiles = 1.0 full tile
 * from entity center) so there are zero disconnected circles across tile seams.
 */
void CarveContinuousEntityWake(Point tile, Displacement pixelOffset, Direction dir, bool isMoving, int dropStrength)
{
	if (!InDungeonBounds(tile))
		return;

	// Convert screen-space walking/flying pixelOffset into fractional half-tile shift (-1.0 .. +1.0)
	const int dUHalf256 = std::clamp(pixelOffset.deltaX * 4 + pixelOffset.deltaY * 8, -256, 256);
	const int dVHalf256 = std::clamp(pixelOffset.deltaY * 8 - pixelOffset.deltaX * 4, -256, 256);

	// Entity center in 24.8 fixed-point half-tile coordinates
	const int centerHX256 = (tile.x * 2 + 1) * 256 + dUHalf256;
	const int centerHY256 = (tile.y * 2 + 1) * 256 + dVHalf256;

	static constexpr int DirPushX[8] = { 0, -20, -28, -20, 0, 20, 28, 20 };
	static constexpr int DirPushY[8] = { 28, 20, 0, -20, -28, -20, 0, 20 };
	const unsigned dirIdx = static_cast<unsigned>(dir) & 7U;

	const int minHX = std::max(0, (centerHX256 >> 8) - 2);
	const int maxHX = std::min(HalfGridW - 1, (centerHX256 >> 8) + 2);
	const int minHY = std::max(0, (centerHY256 >> 8) - 2);
	const int maxHY = std::min(HalfGridH - 1, (centerHY256 >> 8) + 2);

	// Radius = 2.0 half-tiles (512 in 24.8 fixed-point), so radiusSq = (512*512) >> 8 = 1024
	constexpr int MaxRadiusSq = 1024;

	for (int hx = minHX; hx <= maxHX; hx++) {
		for (int hy = minHY; hy <= maxHY; hy++) {
			const int dx256 = hx * 256 - centerHX256;
			const int dy256 = hy * 256 - centerHY256;
			const int distSq = (dx256 * dx256 + dy256 * dy256) >> 8;
			if (distSq >= MaxRadiusSq)
				continue;

			const int lin = 256 - ((distSq * 256) / MaxRadiusSq);
			const int weight = (lin * lin) >> 8; // Smooth bell falloff across the +0.5-tile border
			const int drop = (dropStrength * weight) >> 8;

			HalfFogNode &node = gFogHalfGrid[hx][hy];
			node.density = static_cast<uint8_t>(std::max(0, static_cast<int>(node.density) - drop));

			if (isMoving) {
				const int pushX = (DirPushX[dirIdx] * weight) >> 8;
				const int pushY = (DirPushY[dirIdx] * weight) >> 8;
				node.velX = static_cast<int8_t>(std::clamp(static_cast<int>(node.velX) + pushX, -120, 120));
				node.velY = static_cast<int8_t>(std::clamp(static_cast<int>(node.velY) + pushY, -120, 120));
			}
		}
	}
}

/**
 * @brief Samples the continuous half-tile dispersion state AND exact 8-direction fractional
 * wall distance at sub-tile coordinates (u256, v256) in [0..255] x [0..255].
 *
 * Notice that cloud shape (outDispersionScale) and wall opacity fade (outWallFade) are
 * kept separate so cloud banks extend all the way into alcoves and within ~1.5px of walls
 * before smoothly fading out through Blend37 -> Blend31 -> Blend25 -> Blend19 -> Blend12 -> Blend6 -> 0%.
 */
inline void SampleTileFogState(
    Point tile,
    int u256,
    int v256,
    bool passN,
    bool passS,
    bool passW,
    bool passE,
    bool passNW,
    bool passNE,
    bool passSW,
    bool passSE,
    int &outDispersionDensity,
    int &outWallFade,
    int &outSwirlX,
    int &outSwirlY)
{
	// 1. Exact fractional distance (0..256) to the nearest non-passable wall edge or corner
	int minDist = 256;
	if (!passW)
		minDist = std::min(minDist, u256);
	if (!passE)
		minDist = std::min(minDist, 256 - u256);
	if (!passN)
		minDist = std::min(minDist, v256);
	if (!passS)
		minDist = std::min(minDist, 256 - v256);

	if (!passNW) {
		const int d = static_cast<int>(std::sqrt(static_cast<float>(u256 * u256 + v256 * v256)));
		minDist = std::min(minDist, d);
	}
	if (!passNE) {
		const int du = 256 - u256;
		const int d = static_cast<int>(std::sqrt(static_cast<float>(du * du + v256 * v256)));
		minDist = std::min(minDist, d);
	}
	if (!passSW) {
		const int dv = 256 - v256;
		const int d = static_cast<int>(std::sqrt(static_cast<float>(u256 * u256 + dv * dv)));
		minDist = std::min(minDist, d);
	}
	if (!passSE) {
		const int du = 256 - u256;
		const int dv = 256 - v256;
		const int d = static_cast<int>(std::sqrt(static_cast<float>(du * du + dv * dv)));
		minDist = std::min(minDist, d);
	}

	// Leave a tiny ~1.5px wall margin (minDist <= 12 -> 0%), then ramp smoothly over 108 sub-tile
	// units (~7px) from 6.25% (Blend6) up to 100% full fog opacity.
	if (minDist <= 12) {
		outWallFade = 0;
		outDispersionDensity = 0;
		outSwirlX = 0;
		outSwirlY = 0;
		return;
	}
	if (minDist >= 120) {
		outWallFade = 256;
	} else {
		const int lin = ((minDist - 12) * 256) / 108;
		outWallFade = (lin * (512 - lin)) >> 8;
	}

	// 2. Bilinearly interpolate the continuous 0.5-tile dispersion grid
	const int hxFixed = (tile.x * 512) + (u256 * 2);
	const int hyFixed = (tile.y * 512) + (v256 * 2);
	const int hx0 = std::clamp(hxFixed >> 8, 0, HalfGridW - 2);
	const int hy0 = std::clamp(hyFixed >> 8, 0, HalfGridH - 2);
	const int fx = hxFixed & 0xFF;
	const int fy = hyFixed & 0xFF;
	const int invFx = 256 - fx;
	const int invFy = 256 - fy;

	const int w00 = (invFx * invFy) >> 8;
	const int w10 = (fx * invFy) >> 8;
	const int w01 = (invFx * fy) >> 8;
	const int w11 = (fx * fy) >> 8;

	const HalfFogNode &n00 = gFogHalfGrid[hx0][hy0];
	const HalfFogNode &n10 = gFogHalfGrid[hx0 + 1][hy0];
	const HalfFogNode &n01 = gFogHalfGrid[hx0][hy0 + 1];
	const HalfFogNode &n11 = gFogHalfGrid[hx0 + 1][hy0 + 1];

	outDispersionDensity = (n00.density * w00 + n10.density * w10 + n01.density * w01 + n11.density * w11) >> 8;
	outSwirlX = (n00.velX * w00 + n10.velX * w10 + n01.velX * w01 + n11.velX * w11) >> 8;
	outSwirlY = (n00.velY * w00 + n10.velY * w10 + n01.velY * w01 + n11.velY * w11) >> 8;
}

bool TrySpawnSingleLavaBubble(Point center, bool randomStage)
{
	for (LavaBubble &b : gLavaBubbles) {
		if (b.active)
			continue;

		for (int attempt = 0; attempt < 8; attempt++) {
			const uint32_t r = NextVisualRng();
			const int dx = static_cast<int>(r % 31U) - 15;
			const int dy = static_cast<int>((r >> 5) % 31U) - 15;
			const Point candidate { center.x + dx, center.y + dy };
			if (!IsCavesLavaTile(candidate))
				continue;

			b.active = true;
			b.tile = candidate;
			b.offsetX = static_cast<int8_t>(static_cast<int>((r >> 10) % 29U) - 14);
			b.offsetY = static_cast<int8_t>(static_cast<int>((r >> 15) % 15U) - 7);
			b.snappedToLava = false;
			b.burstSoundSent = false;
			b.stage = randomStage ? static_cast<uint8_t>((r >> 20) % 4U) : 0;
			b.tickInStage = 0;
			b.maxRadX = static_cast<uint8_t>(6 + ((r >> 22) % 4U)); // 6..9 px
			b.maxRadY = static_cast<uint8_t>(4 + ((r >> 24) % 3U)); // 4..6 px
			b.seed = r;
			return true;
		}
		return false;
	}
	return false;
}

void ResetCavesLavaBubbles()
{
	for (LavaBubble &b : gLavaBubbles) {
		b.active = false;
	}
	if (leveltype != DTYPE_CAVES)
		return;

	gLavaBubbleRngState = GetFloorFogSeedHash() ^ 0x9E3779B9U;
	if (gLavaBubbleRngState == 0)
		gLavaBubbleRngState = 0xA341316CU;

	const Point center = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : ViewPosition;
	for (size_t i = 0; i < 40; i++) {
		TrySpawnSingleLavaBubble(center, true);
	}
}

void UpdateCavesLavaBubbles(Point center)
{
	if (leveltype != DTYPE_CAVES)
		return;

	static constexpr uint8_t StageTicks[5] = { 3, 3, 2, 2, 2 };

	for (LavaBubble &b : gLavaBubbles) {
		if (!b.active)
			continue;

		if (std::abs(b.tile.x - center.x) > 20 || std::abs(b.tile.y - center.y) > 20 || !IsCavesLavaTile(b.tile)) {
			b.active = false;
			continue;
		}

		b.tickInStage++;
		if (b.tickInStage >= StageTicks[b.stage]) {
			b.tickInStage = 0;
			b.stage++;
			if (b.stage == 3 && !b.burstSoundSent) {
				b.burstSoundSent = true;
				const int dist = std::max(std::abs(b.tile.x - center.x), std::abs(b.tile.y - center.y));
				if (dist <= 13 && (b.seed & 3U) == 0U) {
					PlaySfxLoc(LS_LAVABUBBLES, b.tile, false);
				}
			}
			if (b.stage >= 5) {
				b.active = false;
			}
		}
	}

	// Test 16 candidates per tick within +-15 tiles of the player
	for (int i = 0; i < 16; i++) {
		TrySpawnSingleLavaBubble(center, false);
	}
}

/**
 * @brief Renders swelling 3D molten lava blisters, bursting craters, and arcing magma droplets
 * ON TOP of the cave steam using the pre-fog wasLavaPx[32][64] mask so steam never dulls them.
 */
void DrawCavesLavaBubblesOnTile(
    const Surface &out,
    Point tilePosition,
    Point targetBufferPosition,
    const bool wasLavaPx[32][64])
{
	const int outW = out.w();
	const int outH = out.h();

	for (LavaBubble &b : gLavaBubbles) {
		if (!b.active || b.tile != tilePosition)
			continue;

		// Ensure the bubble center sits on actual liquid magma pixels (1..31), snapping away from shoreline rock
		if (!b.snappedToLava) {
			const int testDy = std::clamp(16 + static_cast<int>(b.offsetY), 2, 29);
			const int testDx = std::clamp(32 + static_cast<int>(b.offsetX), 4, 59);
			if (!wasLavaPx[testDy][testDx]) {
				bool found = false;
				for (int r = 2; r <= 14 && !found; r += 2) {
					for (int oy = -r; oy <= r && !found; oy += 2) {
						for (int ox = -r * 2; ox <= r * 2 && !found; ox += 4) {
							const int ny = std::clamp(16 + oy, 3, 28);
							const int nx = std::clamp(32 + ox, 6, 57);
							if (wasLavaPx[ny][nx]) {
								b.offsetX = static_cast<int8_t>(nx - 32);
								b.offsetY = static_cast<int8_t>(ny - 16);
								found = true;
							}
						}
					}
				}
				if (!found) {
					b.active = false;
					continue;
				}
			}
			b.snappedToLava = true;
		}

		const int cx = targetBufferPosition.x + 32 + b.offsetX;
		const int cy = targetBufferPosition.y - 15 + b.offsetY;

		// Sample live animated lava palette shades from the active cycle (indices 1..31):
		// Find the brightest, mid, and darkest colors currently in system_palette[1..31] so bubbles
		// automatically match all 4 Caves lava color variants (Orange, Golden, Toxic Green, Cyan)!
		uint8_t hotIdx = 1;
		uint8_t midIdx = 8;
		uint8_t rimIdx = 16;
		int maxLum = -1;
		int minLum = 100000;
		for (uint8_t idx = 1; idx <= 31; idx++) {
			const SDL_Color c = system_palette[idx];
			const int lum = c.r + c.g + c.b;
			if (lum > maxLum) {
				maxLum = lum;
				hotIdx = idx;
			}
			if (lum < minLum) {
				minLum = lum;
				rimIdx = idx;
			}
		}
		midIdx = paletteTransparencyLookup[hotIdx][rimIdx];

		if (b.stage <= 2) {
			// Stages 0..2: Swelling 3D hemispherical lava blister dome with specular hotspot
			const int scaleNum = (b.stage == 0) ? 3 : ((b.stage == 1) ? 4 : 5);
			const int rx = std::max(2, (static_cast<int>(b.maxRadX) * scaleNum) / 5);
			const int ry = std::max(2, (static_cast<int>(b.maxRadY) * scaleNum) / 5);
			const int domeLift = (b.stage == 0) ? 1 : ((b.stage == 1) ? 2 : 3);

			for (int dy = -ry - domeLift; dy <= ry; dy++) {
				const int sy = cy + dy;
				if (sy < 0 || sy >= outH)
					continue;
				const int localDy = std::clamp((cy - (targetBufferPosition.y - 31)), 0, 31);

				for (int dx = -rx; dx <= rx; dx++) {
					const int sx = cx + dx;
					if (sx < 0 || sx >= outW)
						continue;

					const int localDx = std::clamp(sx - targetBufferPosition.x, 0, 63);
					if (!wasLavaPx[localDy][localDx])
						continue;

					// Elliptical dome metric with vertical bulge (dy shifted up by domeLift/2)
					const float nx = static_cast<float>(dx) / static_cast<float>(rx);
					const float ny = static_cast<float>(dy + (domeLift / 2)) / static_cast<float>(ry);
					const float rSq = nx * nx + ny * ny;
					if (rSq > 1.0F)
						continue;

					uint8_t &dst = out[{ sx, sy }];
					// Upper-left incandescent specular highlight on the 3D blister dome
					const float specDistSq = (nx + 0.28F) * (nx + 0.28F) + (ny + 0.32F) * (ny + 0.32F);
					if (specDistSq < 0.22F) {
						dst = hotIdx;
					} else if (rSq < 0.55F) {
						dst = Blend37(hotIdx, midIdx);
					} else if (rSq < 0.82F) {
						dst = midIdx;
					} else {
						dst = Blend37(rimIdx, dst);
					}
				}
			}
		} else {
			// Stages 3..4: Bursting molten crater ring + 4 arcing magma droplets
			const int rx = (b.stage == 3) ? b.maxRadX : (b.maxRadX + 2);
			const int ry = (b.stage == 3) ? b.maxRadY : (b.maxRadY + 1);

			for (int dy = -ry; dy <= ry; dy++) {
				const int sy = cy + dy;
				if (sy < 0 || sy >= outH)
					continue;
				for (int dx = -rx; dx <= rx; dx++) {
					const int sx = cx + dx;
					if (sx < 0 || sx >= outW)
						continue;

					const float nx = static_cast<float>(dx) / static_cast<float>(rx);
					const float ny = static_cast<float>(dy) / static_cast<float>(ry);
					const float rSq = nx * nx + ny * ny;
					if (rSq < 0.42F || rSq > 1.05F)
						continue;

					uint8_t &dst = out[{ sx, sy }];
					if (b.stage == 3) {
						dst = (rSq < 0.76F) ? hotIdx : midIdx;
					} else {
						dst = Blend37(midIdx, dst);
					}
				}
			}

			// 4 molten spatter droplets arcing above the bursting bubble
			const int arcHeight = (b.stage == 3) ? 6 : 3;
			const int spread = (b.stage == 3) ? 4 : 7;
			static constexpr int DirX[4] = { -1, 1, -2, 2 };
			static constexpr int DirY[4] = { -1, -1, 0, 0 };
			for (int d = 0; d < 4; d++) {
				const int jitX = static_cast<int>((b.seed >> (d * 3)) & 3U) - 1;
				const int dropX = cx + (DirX[d] * spread) / 2 + jitX;
				const int dropY = cy - arcHeight + DirY[d];
				if (dropX >= 1 && dropX < outW - 1 && dropY >= 1 && dropY < outH - 1) {
					out[{ dropX, dropY }] = hotIdx;
					if (b.stage == 3) {
						out[{ dropX, dropY + 1 }] = midIdx;
					}
				}
			}
		}
	}
}

} // namespace

void ResetGroundFogLevel()
{
	EnsureFogNoiseInitialized();
	const LevelFogConfig cfg = GetLevelFogConfig();

	if (!cfg.enabled || cfg.spawnChancePercent == 0) {
		gLevelHasFog = false;
	} else if (cfg.spawnChancePercent >= 100) {
		gLevelHasFog = true;
	} else {
		gLevelHasFog = ((GetFloorFogSeedHash() % 100U) < cfg.spawnChancePercent);
	}

	const uint8_t target = gLevelHasFog ? cfg.maxDensity : 0;

	for (int hx = 0; hx < HalfGridW; hx++) {
		for (int hy = 0; hy < HalfGridH; hy++) {
			const Point parentTile { std::clamp((hx - 1) / 2, 0, MAXDUNX - 1), std::clamp((hy - 1) / 2, 0, MAXDUNY - 1) };
			const bool passable = IsFogPassableTile(parentTile);
			gFogHalfGrid[hx][hy].density = passable ? target : 0;
			gFogHalfGrid[hx][hy].velX = 0;
			gFogHalfGrid[hx][hy].velY = 0;
		}
	}

	ResetCavesLavaBubbles();
}

void UpdateGroundFogSimulation()
{
	const Point center = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : ViewPosition;
	UpdateCavesLavaBubbles(center);

	if (!*sgOptions.Graphics.groundFog || !gLevelHasFog)
		return;

	const LevelFogConfig cfg = GetLevelFogConfig();
	if (!cfg.enabled)
		return;

	EnsureFogNoiseInitialized();

	const int maxDens = cfg.maxDensity;

	// 1. Relax velocity and regrow density in the 0.5-tile grid around the player
	const int minHX = std::max(0, (center.x - 28) * 2);
	const int maxHX = std::min(HalfGridW - 1, (center.x + 28) * 2 + 2);
	const int minHY = std::max(0, (center.y - 28) * 2);
	const int maxHY = std::min(HalfGridH - 1, (center.y + 28) * 2 + 2);

	for (int hx = minHX; hx <= maxHX; hx++) {
		for (int hy = minHY; hy <= maxHY; hy++) {
			HalfFogNode &node = gFogHalfGrid[hx][hy];
			if (node.density < maxDens) {
				node.density = static_cast<uint8_t>(std::min(maxDens, static_cast<int>(node.density) + 9));
			} else if (node.density > maxDens) {
				node.density = static_cast<uint8_t>(maxDens);
			}

			node.velX = static_cast<int8_t>((node.velX * 3) / 4);
			node.velY = static_cast<int8_t>((node.velY * 3) / 4);
		}
	}

	// 2. Continuous +0.5-tile wake on each side for active Players
	for (const Player &player : Players) {
		if (!player.plractive || !player.isOnActiveLevel())
			continue;
		const bool moving = player.isWalking();
		const Displacement walkOffset = moving ? GetOffsetForWalking(player.AnimInfo, player._pdir) : Displacement {};
		CarveContinuousEntityWake(player.position.tile, walkOffset, player._pdir, moving, moving ? 145 : 70);
	}

	// 3. Continuous +0.5-tile wake on each side for active Monsters
	if (leveltype != DTYPE_TOWN) {
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			const Monster &monster = Monsters[ActiveMonsters[i]];
			if ((monster.flags & MFLAG_HIDDEN) != 0)
				continue;
			const bool moving = monster.isWalking() || monster.mode == MonsterMode::Charge;
			const bool attacking = monster.mode == MonsterMode::MeleeAttack
			    || monster.mode == MonsterMode::SpecialMeleeAttack
			    || monster.mode == MonsterMode::RangedAttack
			    || monster.mode == MonsterMode::SpecialRangedAttack;
			const Displacement walkOffset = monster.isWalking() ? GetOffsetForWalking(monster.animInfo, monster.direction) : Displacement {};
			CarveContinuousEntityWake(monster.position.tile, walkOffset, monster.direction, moving, (moving || attacking) ? 130 : 60);
		}
	}

	// 4. Continuous +0.5-tile wake on each side for active Missiles / Spells
	for (const Missile &missile : Missiles) {
		if (missile._miDelFlag)
			continue;
		CarveContinuousEntityWake(missile.position.tile, missile.position.offset, Direction::South, true, 135);
	}
}

void DrawGroundFogTileBase(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
{
	if (lightIndex >= LightsMax)
		return;

	const bool isCavesLava = (leveltype == DTYPE_CAVES && IsCavesLavaTile(tilePosition));

	// Pre-fog liquid magma mask (captures indices 1..31 BEFORE fog blends over the tile)
	bool wasLavaPx[32][64];
	if (isCavesLava) {
		const int outW = out.w();
		const int outH = out.h();
		for (int dy = 0; dy < 32; dy++) {
			const int sy = targetBufferPosition.y - 31 + dy;
			for (int dx = 0; dx < 64; dx++) {
				const int sx = targetBufferPosition.x + dx;
				if (sx >= 0 && sx < outW && sy >= 0 && sy < outH) {
					const uint8_t px = out[{ sx, sy }];
					wasLavaPx[dy][dx] = (px >= 1 && px <= 31);
				} else {
					wasLavaPx[dy][dx] = false;
				}
			}
		}
	}

	if (*sgOptions.Graphics.groundFog && gLevelHasFog && IsFogPassableTile(tilePosition)) {
		const LevelFogConfig cfg = GetLevelFogConfig();
		if (cfg.enabled) {
			EnsureFogNoiseInitialized();

			// Resolve volumetric shades:
			// In Hell (isDarkSmoke), crestShade (252), coreShade (254), and troughShade (0 = Pure Black)
			// are used directly so black/charcoal soot darkens the floor without turning into index 255 white.
			const uint8_t *lightTbl = LightTables[std::clamp(lightIndex, 0, LightsMax - 1)].data();
			const uint8_t litCrest = cfg.isDarkSmoke ? cfg.crestShade : lightTbl[cfg.crestShade];
			const uint8_t litCore = cfg.isDarkSmoke ? cfg.coreShade : lightTbl[cfg.coreShade];
			const uint8_t litTrough = cfg.isDarkSmoke ? cfg.troughShade : lightTbl[cfg.troughShade];

			// 60+ Hz sub-texel 24.8 fixed-point drift coordinates
			const int32_t ticks = static_cast<int32_t>(SDL_GetTicks());
			const int driftXFixed1 = (ticks * 256) / cfg.driftDivX;
			const int driftYFixed1 = (ticks * 256) / cfg.driftDivY;
			const int driftXFixed0 = (driftXFixed1 * 13) >> 4; // 0.81x slow base mist
			const int driftYFixed0 = (driftYFixed1 * 13) >> 4;
			const int driftXFixed2 = (driftXFixed1 * 19) >> 4; // 1.19x fast upper billow domes
			const int driftYFixed2 = (driftYFixed1 * 19) >> 4;
			const int warpDriftXFixed = -(ticks * 192) / cfg.driftDivX;
			const int warpDriftYFixed = (ticks * 192) / cfg.driftDivY;

			// Optional slow bio-spore breathing pulse for Hive/Nest
			int pulseOffset = 0;
			if (cfg.hasPulse) {
				const int phase = (ticks / 28 + tilePosition.x * 19 + tilePosition.y * 31) & 0xFF;
				const int triWave = (phase < 128) ? phase : (255 - phase);
				pulseOffset = (triWave >> 2) - 16;
			}

			// 8-direction neighbor passability for exact ~1.5px wall margin + 6-step fade-out
			const int tx = tilePosition.x;
			const int ty = tilePosition.y;
			const bool passN = IsFogPassableTile({ tx, ty - 1 });
			const bool passS = IsFogPassableTile({ tx, ty + 1 });
			const bool passW = IsFogPassableTile({ tx - 1, ty });
			const bool passE = IsFogPassableTile({ tx + 1, ty });
			const bool passNW = IsFogPassableTile({ tx - 1, ty - 1 });
			const bool passNE = IsFogPassableTile({ tx + 1, ty - 1 });
			const bool passSW = IsFogPassableTile({ tx - 1, ty + 1 });
			const bool passSE = IsFogPassableTile({ tx + 1, ty + 1 });

			const int uvScale = cfg.uvScale;
			const int baseWorldUFixed = (tx * uvScale) << 8;
			const int baseWorldVFixed = (ty * uvScale) << 8;

			// Isometric altitude ray-slicing offsets in 24.8 fixed-point texels:
			// z = +3.5px mid-body and z = +7.0px upper billow dome
			const int zMidTexFixed = (7 * uvScale) << 3;  // +3.5px altitude
			const int zTopTexFixed = (14 * uvScale) << 3; // +7.0px altitude
			const int zOvhTexFixed = (24 * uvScale) << 3; // +12.0px overhead shadow caster check

			static constexpr int Bayer2x2[2][2] = {
				{ -5, 2 },
				{ 5, -2 }
			};

			const int outW = out.w();
			const int outH = out.h();

			for (int dy = 0; dy < 32; dy++) {
				const int sy = targetBufferPosition.y - 31 + dy;
				if (sy < 0 || sy >= outH)
					continue;

				const int halfSpan = (dy < 16) ? ((dy + 1) * 2) : ((32 - dy) * 2);
				const int xStart = targetBufferPosition.x + 32 - halfSpan;
				const int xEnd = targetBufferPosition.x + 32 + halfSpan;
				const int bayerRow = sy & 1;

				for (int sx = xStart; sx < xEnd; sx++) {
					if (sx < 0 || sx >= outW)
						continue;

					const int localX = sx - (targetBufferPosition.x + 32);
					const int localY = dy - 16;
					const int u256 = std::clamp((localX * 4) + (localY * 8) + 128, 0, 255);
					const int v256 = std::clamp((localY * 8) - (localX * 4) + 128, 0, 255);

					int dispDensity = cfg.maxDensity;
					int wallFade = 256;
					int swirlX = 0;
					int swirlY = 0;
					SampleTileFogState(
					    tilePosition, u256, v256,
					    passN, passS, passW, passE, passNW, passNE, passSW, passSE,
					    dispDensity, wallFade, swirlX, swirlY);

					if (wallFade <= 0 || dispDensity < 16)
						continue;

					const int dispVal = std::clamp(dispDensity + pulseOffset, 0, 255);
					const int tileUFixed = u256 * uvScale;
					const int tileVFixed = v256 * uvScale;

					// Smooth bilinear domain-warp curl in 24.8 fixed-point
					const int warpU = baseWorldUFixed + tileUFixed + warpDriftXFixed;
					const int warpV = baseWorldVFixed + tileVFixed + warpDriftYFixed;
					const int warpXFixed = (SampleLutBilinear(gFogWarpLut, warpU, warpV) - 128) * 14;
					const int warpYFixed = (SampleLutBilinear(gFogWarpLut, warpU + (41 << 8), warpV + (73 << 8)) - 128) * 14;

					const int swirlUFixed = swirlX * 48;
					const int swirlVFixed = swirlY * 48;

					const int baseU = baseWorldUFixed + tileUFixed + warpXFixed + swirlUFixed;
					const int baseV = baseWorldVFixed + tileVFixed + warpYFixed + swirlVFixed;

					// 3-Altitude Volumetric Ray-Slicing:
					// Altitude 0 (z = 0 px): Ground-hugging mist (0.81x drift)
					const int hBase = SampleLutBilinear(gFogNoiseLut, baseU + driftXFixed0, baseV + driftYFixed0);
					// Altitude 1 (z = +3.5 px): Mid cloud body on decorrelated slice (+97, +151) at 1.00x drift
					const int hMid = SampleLutBilinear(
					    gFogNoiseLut,
					    baseU + zMidTexFixed + driftXFixed1 + (97 << 8),
					    baseV + zMidTexFixed + driftYFixed1 + (151 << 8));
					// Altitude 2 (z = +7.0 px): Upper 3D billow domes on decorrelated slice (+193, +53) at 1.19x wind shear
					const int hTop = SampleLutBilinear(
					    gFogNoiseLut,
					    baseU + zTopTexFixed + driftXFixed2 + (193 << 8),
					    baseV + zTopTexFixed + driftYFixed2 + (53 << 8));

					// Combine the 3 altitude slices into a single volumetric ray density
					const int weightedTop = (hTop * 15) >> 4;
					const int weightedMid = (hMid * 15) >> 4;
					const int weightedBase = (hBase * 13) >> 4;
					const int peakSlice = std::max({ weightedTop, weightedMid, weightedBase });
					const int avgSlice = (weightedBase + weightedMid + weightedTop) / 3;
					const int rayCloud = (peakSlice * 3 + avgSlice) >> 2;

					// Decouple cloud shape (cloudIntensity >= intensityCutoff) from wallFade so
					// clouds reach all the way into alcoves and wall edges without shrinking away!
					const int cloudIntensity = (rayCloud * dispVal) >> 8;
					if (cloudIntensity < cfg.intensityCutoff)
						continue;

					// Volumetric self-shadowing: check if an upper cloud dome 5px above casts a dark
					// underside drop-shadow onto the lower mist / floor beneath its southern edge
					const int hOverhead = SampleLutBilinear(
					    gFogNoiseLut,
					    baseU + zOvhTexFixed + driftXFixed2 + (193 << 8),
					    baseV + zOvhTexFixed + driftYFixed2 + (53 << 8));

					uint8_t shade = litCore;
					if (hOverhead > hTop + 26 && weightedTop < 145) {
						shade = litTrough; // Cast underside drop-shadow from overhead billow dome
					} else if (weightedTop >= weightedMid + 8 && weightedTop > 132) {
						shade = litCrest; // Lit upper 3D billow dome rolling over the mist
					} else if (weightedBase >= weightedMid && weightedTop < 112) {
						shade = litTrough; // Low ground-hugging trough mist
					}

					// Apply smooth wallFade attenuation + 2x2 Bayer gradient dithering across 6 LUT blend tiers
					// (+10% transparency shift: max tier is Blend37, fading all the way down to Blend6 = 6.25%)
					const int dither = Bayer2x2[bayerRow][sx & 1];
					const int alpha = ((cloudIntensity * wallFade) >> 8) + dither;
					if (alpha < 52)
						continue;

					uint8_t &dst = out[{ sx, sy }];
					if (alpha > 192) {
						dst = cfg.allowHeavyBlend ? Blend37(shade, dst) : Blend31(shade, dst);
					} else if (alpha > 156) {
						dst = Blend31(shade, dst);
					} else if (alpha > 124) {
						dst = Blend25(shade, dst);
					} else if (alpha > 96) {
						dst = Blend19(shade, dst);
					} else if (alpha > 72) {
						dst = Blend12(shade, dst);
					} else {
						dst = Blend6(shade, dst);
					}
				}
			}
		}
	}

	// Render Caves 3D boiling lava bubbles ON TOP of the cave steam so fog never dulls them
	if (isCavesLava) {
		DrawCavesLavaBubblesOnTile(out, tilePosition, targetBufferPosition, wasLavaPx);
	}
}

void DrawGroundFogEntitySubmersion(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
{
	if (!*sgOptions.Graphics.groundFog || !gLevelHasFog)
		return;
	if (lightIndex >= LightsMax || !IsFogPassableTile(tilePosition))
		return;

	const LevelFogConfig cfg = GetLevelFogConfig();
	if (!cfg.enabled)
		return;

	EnsureFogNoiseInitialized();

	// Soft ankle/boot wisp (+10% more transparent: Blend19 / Blend12 / Blend6)
	const int collarDensity = (static_cast<int>(cfg.maxDensity) * 11) / 20;
	if (collarDensity < 32)
		return;

	const uint8_t *lightTbl = LightTables[std::clamp(lightIndex, 0, LightsMax - 1)].data();
	const uint8_t litCrest = cfg.isDarkSmoke ? cfg.crestShade : lightTbl[cfg.crestShade];
	const uint8_t litCore = cfg.isDarkSmoke ? cfg.coreShade : lightTbl[cfg.coreShade];

	const int32_t ticks = static_cast<int32_t>(SDL_GetTicks());
	const int driftXFixed = (ticks * 256) / cfg.driftDivX;
	const int driftYFixed = (ticks * 256) / cfg.driftDivY;

	const int outW = out.w();
	const int outH = out.h();

	// Tight elliptical wisp centered strictly over the entity's boots/lower legs
	const int centerX = targetBufferPosition.x + 32;
	const int centerY = targetBufferPosition.y - 10;
	constexpr int RadiusX = 14;
	constexpr int RadiusY = 8;

	for (int dy = -RadiusY; dy <= RadiusY; dy++) {
		const int sy = centerY + dy;
		if (sy < 0 || sy >= outH)
			continue;

		const int normY256 = (dy * 256) / RadiusY;
		const int ySq = (normY256 * normY256) >> 8;

		for (int dx = -RadiusX; dx <= RadiusX; dx++) {
			const int sx = centerX + dx;
			if (sx < 0 || sx >= outW)
				continue;

			const int normX256 = (dx * 256) / RadiusX;
			const int distSq = ySq + ((normX256 * normX256) >> 8);
			if (distSq >= 256)
				continue;

			const int radialFalloff = 256 - distSq;
			const int uFixed = ((tilePosition.x * cfg.uvScale + dx) << 8) + driftXFixed;
			const int vFixed = ((tilePosition.y * cfg.uvScale + dy * 2) << 8) + driftYFixed;
			const int noise = SampleLutBilinear(gFogNoiseLut, uFixed, vFixed);

			const int wispIntensity = (((noise * collarDensity) >> 8) * radialFalloff) >> 8;
			if (wispIntensity < 68)
				continue;

			uint8_t &dst = out[{ sx, sy }];
			if (wispIntensity > 150) {
				dst = Blend19(litCrest, dst);
			} else if (wispIntensity > 105) {
				dst = Blend12(litCore, dst);
			} else {
				dst = Blend6(litCore, dst);
			}
		}
	}
}

} // namespace devilution
