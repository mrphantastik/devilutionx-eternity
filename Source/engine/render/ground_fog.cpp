/**
 * @file ground_fog.cpp
 *
 * Implementation of 3D Isometric Volumetric Ground Fog v6 (Continuous Float FBM Noise,
 * High-Resolution 2-Layer World-Locked Volumetric Separation with Lively Internal Swirl,
 * True Darkening Charcoal Drop-Shadows, 4x4 Bayer Sub-Pixel Dithering, 4-Corner Tile
 * State Interpolation for Locked 60+ FPS, and Pure Neutral Gray Shading Across All
 * Dungeons) and Caves Boiling Lava Bubbles (Rendered Below Fog with 6-Tile Proximity Audio).
 */
#include "engine/render/ground_fog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <SDL.h>

#include "diablo.h"
#include "effects.h"
#include "engine/backbuffer_state.hpp"
#include "engine/palette.h"
#include "engine/render/scrollrt.h"
#include "gmenu.h"
#include "levels/gendung.h"
#include "lighting.h"
#include "missiles.h"
#include "monster.h"
#include "options.h"
#include "player.h"
#include "utils/display.h"

namespace devilution {

namespace {

constexpr int HalfGridW = MAXDUNX * 2 + 3;
constexpr int HalfGridH = MAXDUNY * 2 + 3;

struct FogHalfCell {
	uint8_t density;
	int8_t velX;
	int8_t velY;
};

struct LevelFogConfig {
	bool enabled;
	uint8_t spawnChancePercent;
	uint8_t maxDensity;
	uint8_t intensityCutoff;
	uint8_t uvScale;
	uint8_t crestShade;
	uint8_t coreShade;
	uint8_t troughShade;
	bool allowHeavyBlend;
	bool hasPulse;
	int driftDivX;
	int driftDivY;
};

bool gNoiseInitialized = false;
bool gLevelHasFog = false;

std::array<std::array<uint8_t, 256>, 256> gFogNoiseLut;
std::array<std::array<uint8_t, 256>, 256> gFogCanopyNoiseLut;
std::array<std::array<uint8_t, 256>, 256> gFogMacroClearingLut;
std::array<std::array<uint8_t, 256>, 256> gFogWarpLut;

uint32_t gLastFogSheetUpdateTick = 0;
uint8_t gFogSheetSliceIdx = 0;
dungeon_type gLastFogSheetDtype = DTYPE_NONE;
int gCachedPulseOffset = 0;
std::array<std::array<uint8_t, 256>, 256> gCachedFloorFog;
std::array<std::array<uint8_t, 256>, 256> gCachedCanopyFog;
std::array<std::array<int8_t, 256>, 256> gCachedCanopySlope;

/**
 * Ordered 4x4 Bayer Dither Matrix (-15..+15):
 * Matches Ground Fog v6 (nice-fog2.png) to smoothly feather transitions across
 * the paletteTransparencyLookup blend tiers without contour rings or blockiness.
 */
constexpr int8_t Bayer4x4[4][4] = {
	{ -15,   5, -11,   9 },
	{  13,  -7,  15,  -3 },
	{  -9,  11, -13,   7 },
	{  15,  -5,   9, -11 },
};

std::array<std::array<FogHalfCell, HalfGridH>, HalfGridW> gFogHalfGrid;
std::array<std::array<bool, MAXDUNY>, MAXDUNX> gFogTilePassable;
std::array<std::array<uint16_t, MAXDUNY + 1>, MAXDUNX + 1> gFogVertexWallFade;

LevelFogConfig GetLevelFogConfig()
{
	// Matches Ground Fog v6 (nice-fog2.png): pure neutral PAL16_GRAY (240..254) smoky shades
	// with uvScale = 44 for Layer 2 cloud puffs (and uvScale1 = 64 for Layer 1 ground mist).
	switch (leveltype) {
	case DTYPE_CATHEDRAL:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/232,
			/*intensityCutoff=*/78,
			/*uvScale=*/44,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 3),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 10),
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/false,
			/*driftDivX=*/520,
			/*driftDivY=*/760
		};
	case DTYPE_CATACOMBS:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/230,
			/*intensityCutoff=*/78,
			/*uvScale=*/44,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 3),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 10),
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/false,
			/*driftDivX=*/560,
			/*driftDivY=*/800
		};
	case DTYPE_CAVES:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/234,
			/*intensityCutoff=*/74,
			/*uvScale=*/46,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 3),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 10),
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/false,
			/*driftDivX=*/440,
			/*driftDivY=*/-480
		};
	case DTYPE_HELL:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/238,
			/*intensityCutoff=*/74,
			/*uvScale=*/44,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 3),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 10),
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/false,
			/*driftDivX=*/440,
			/*driftDivY=*/-460
		};
	case DTYPE_NEST:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/232,
			/*intensityCutoff=*/78,
			/*uvScale=*/48,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 3),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 10),
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/true,
			/*driftDivX=*/480,
			/*driftDivY=*/-560
		};
	case DTYPE_CRYPT:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/240,
			/*intensityCutoff=*/72,
			/*uvScale=*/42,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 3),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 10),
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/false,
			/*driftDivX=*/600,
			/*driftDivY=*/820
		};
	default:
		return { false, 0, 0, 255, 44, 0, 0, 0, false, false, 600, 600 };
	}
}

/**
 * Shades a fog palette index smoothly by dungeon lightIndex (0 = bright, 14 = dark)
 * WITHOUT allowing PAL16_GRAY (240..254) to snap to 0 (Pure Black) at lightIndex >= 6.
 */
inline uint8_t ShadeFogColor(uint8_t baseColor, int lightIndex)
{
	const int clampedLight = std::clamp(lightIndex, 0, 14);
	if (baseColor >= PAL16_GRAY && baseColor < PAL16_GRAY + 15) {
		const int baseStep = static_cast<int>(baseColor - PAL16_GRAY);
		const int shadedStep = std::clamp(baseStep + ((clampedLight * (13 - baseStep)) / 16), 1, 13);
		return static_cast<uint8_t>(PAL16_GRAY + shadedStep);
	}
	const uint8_t lit = LightTables[clampedLight][baseColor];
	return (lit == 0) ? static_cast<uint8_t>(PAL16_GRAY + 13) : lit;
}

inline uint8_t Blend50(uint8_t a, uint8_t b)
{
	return paletteTransparencyLookup[a][b];
}

inline uint8_t Blend25(uint8_t a, uint8_t b)
{
	return Blend50(Blend50(a, b), b);
}

inline uint8_t Blend12(uint8_t a, uint8_t b)
{
	return Blend50(Blend25(a, b), b);
}

inline uint8_t Blend6(uint8_t a, uint8_t b)
{
	return Blend50(Blend12(a, b), b);
}

inline uint8_t Blend37(uint8_t a, uint8_t b)
{
	return Blend50(Blend50(a, b), Blend25(a, b));
}

inline uint8_t Blend31(uint8_t a, uint8_t b)
{
	return Blend50(Blend37(a, b), Blend25(a, b));
}

inline uint8_t Blend19(uint8_t a, uint8_t b)
{
	return Blend50(Blend25(a, b), Blend12(a, b));
}

inline uint8_t ApplySoftFogBlend(uint8_t shade, uint8_t dst, int alpha, bool allowHeavy)
{
	if (alpha > 195) {
		return allowHeavy ? Blend50(shade, dst) : Blend37(shade, dst);
	}
	if (alpha > 152) {
		return Blend37(shade, dst);
	}
	if (alpha > 114) {
		return Blend31(shade, dst);
	}
	if (alpha > 80) {
		return Blend25(shade, dst);
	}
	if (alpha > 48) {
		return Blend19(shade, dst);
	}
	if (alpha > 24) {
		return Blend12(shade, dst);
	}
	if (alpha > 8) {
		return Blend6(shade, dst);
	}
	return dst;
}

uint32_t HashLattice(uint32_t x, uint32_t y, uint32_t seed)
{
	uint32_t h = (x * 374761393U) + (y * 668265263U) + (seed * 1442695041U);
	h = (h ^ (h >> 13)) * 1274126177U;
	return h ^ (h >> 16);
}

/**
 * Continuous floating-point tileable value noise on a 256x256 torus.
 * Accepts continuous float coordinates (fx, fy) so domain-warping never truncates
 * coordinates to integer steps, eliminating baked staircase blocks in gFogNoiseLut.
 */
float ValueNoiseTileableFloat(float fx, float fy, int period, uint32_t seed)
{
	const float cellSize = 256.0F / static_cast<float>(period);
	const float gx = fx / cellSize;
	const float gy = fy / cellSize;

	const int ix = static_cast<int>(std::floor(gx));
	const int iy = static_cast<int>(std::floor(gy));
	const float tx = gx - static_cast<float>(ix);
	const float ty = gy - static_cast<float>(iy);

	const float ux = tx * tx * tx * (tx * (tx * 6.0F - 15.0F) + 10.0F);
	const float uy = ty * ty * ty * (ty * (ty * 6.0F - 15.0F) + 10.0F);

	const uint32_t gx0 = static_cast<uint32_t>((ix % period + period) % period);
	const uint32_t gy0 = static_cast<uint32_t>((iy % period + period) % period);
	const uint32_t gx1 = (gx0 + 1U) % static_cast<uint32_t>(period);
	const uint32_t gy1 = (gy0 + 1U) % static_cast<uint32_t>(period);

	const float v00 = static_cast<float>(HashLattice(gx0, gy0, seed) & 0xFFFFU) / 65535.0F;
	const float v10 = static_cast<float>(HashLattice(gx1, gy0, seed) & 0xFFFFU) / 65535.0F;
	const float v01 = static_cast<float>(HashLattice(gx0, gy1, seed) & 0xFFFFU) / 65535.0F;
	const float v11 = static_cast<float>(HashLattice(gx1, gy1, seed) & 0xFFFFU) / 65535.0F;

	const float nx0 = v00 + ux * (v10 - v00);
	const float nx1 = v01 + ux * (v11 - v01);
	return nx0 + uy * (nx1 - nx0);
}

void EnsureFogNoiseInitialized()
{
	if (gNoiseInitialized)
		return;

	for (int y = 0; y < 256; y++) {
		const float fy = static_cast<float>(y);
		for (int x = 0; x < 256; x++) {
			const float fx = static_cast<float>(x);

			const float wx = ValueNoiseTileableFloat(fx, fy, 4, 901U);
			const float wy = ValueNoiseTileableFloat(fx, fy, 4, 902U);
			gFogWarpLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(wx * 255.0F + 0.5F), 0, 255));

			const float mx = fx + (wx - 0.5F) * 48.0F;
			const float my = fy + (wy - 0.5F) * 48.0F;
			const float m1 = ValueNoiseTileableFloat(mx, my, 2, 771U);
			const float m2 = ValueNoiseTileableFloat(mx, my, 4, 772U);
			const float macroRaw = m1 * 0.60F + m2 * 0.40F;
			const float macroNorm = std::clamp((macroRaw - 0.30F) * 2.0F, 0.0F, 1.0F);
			gFogMacroClearingLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(macroNorm * 255.0F + 0.5F), 0, 255));

			// 1. Layer 1 Noise (gFogNoiseLut): Ground Fog v6 (nice-fog2.png) smoky FBM + ridge filaments
			const float sx1 = fx + (wx - 0.5F) * 44.0F;
			const float sy1 = fy + (wy - 0.5F) * 44.0F;

			const float n0 = ValueNoiseTileableFloat(sx1, sy1, 2, 101U);
			const float n1 = ValueNoiseTileableFloat(sx1, sy1, 4, 202U);
			const float n2 = ValueNoiseTileableFloat(sx1, sy1, 8, 303U);
			const float n3 = ValueNoiseTileableFloat(sx1, sy1, 16, 404U);

			const float fbm1 = n0 * 0.38F + n1 * 0.32F + n2 * 0.20F + n3 * 0.10F;
			const float ridge1 = 1.0F - std::fabs(n1 * 2.0F - 1.0F);
			const float ridgeSmooth1 = ridge1 * ridge1 * (3.0F - 2.0F * ridge1);
			const float raw1 = fbm1 * 0.72F + ridgeSmooth1 * 0.28F;

			const float norm1 = std::clamp((raw1 - 0.28F) * 1.85F, 0.0F, 1.0F);
			const float mist1 = norm1 * norm1 * (3.0F - 2.0F * norm1);
			gFogNoiseLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(mist1 * 255.0F + 0.5F), 0, 255));

			// 2. Layer 2 Noise (gFogCanopyNoiseLut): Ground Fog v6 sculpted 3D cloud puff banks
			const float sx2 = fx + (wy - 0.5F) * 48.0F;
			const float sy2 = fy - (wx - 0.5F) * 48.0F;

			const float c0 = ValueNoiseTileableFloat(sx2, sy2, 2, 505U);
			const float c1 = ValueNoiseTileableFloat(sx2, sy2, 4, 606U);
			const float c2 = ValueNoiseTileableFloat(sx2, sy2, 8, 707U);
			const float c3 = ValueNoiseTileableFloat(sx2, sy2, 16, 808U);

			const float fbm2 = c0 * 0.44F + c1 * 0.32F + c2 * 0.16F + c3 * 0.08F;
			const float norm2 = std::clamp((fbm2 - 0.36F) * 2.15F, 0.0F, 1.0F);
			const float puff2 = norm2 * norm2 * (3.0F - 2.0F * norm2);
			gFogCanopyNoiseLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(puff2 * 255.0F + 0.5F), 0, 255));
		}
	}

	gNoiseInitialized = true;
}

inline int SampleLutBilinear(const std::array<std::array<uint8_t, 256>, 256> &lut, int uFixed, int vFixed)
{
	const int u0 = (uFixed >> 8) & 0xFF;
	const int v0 = (vFixed >> 8) & 0xFF;
	const int u1 = (u0 + 1) & 0xFF;
	const int v1 = (v0 + 1) & 0xFF;
	const int fu = uFixed & 0xFF;
	const int fv = vFixed & 0xFF;

	const int p00 = lut[v0][u0];
	const int p10 = lut[v0][u1];
	const int p01 = lut[v1][u0];
	const int p11 = lut[v1][u1];

	const int top = (p00 << 8) + (p10 - p00) * fu;
	const int bot = (p01 << 8) + (p11 - p01) * fu;
	return (top + (((bot - top) * fv) >> 8)) >> 8;
}

/**
 * Updates the world-locked fog sheets in 4 interleaved 64-row slices.
 * Step 1: Travel and internal swirl speeds are reduced by 40% (0.60x velocity) while
 * preserving the exact Ground Fog v6 (nice-fog2.png) visual look.
 */
void UpdateCachedFogSheets30Hz(uint32_t ticks, const LevelFogConfig &cfg)
{
	const bool forceFullRebuild = (gLastFogSheetDtype != leveltype);
	if (!forceFullRebuild && (ticks - gLastFogSheetUpdateTick) < 10U)
		return;

	gLastFogSheetUpdateTick = ticks;
	gLastFogSheetDtype = leveltype;

	const int32_t sTicks = static_cast<int32_t>(ticks);
	// Step 1: 40% slower travel speeds (0.60x of Ground Fog v6 velocity)
	const int driftX1 = (sTicks * 256) / ((cfg.driftDivX * 100) / 336);
	const int driftY1 = (sTicks * 256) / ((cfg.driftDivY * 100) / 168);

	// Layer 2 (Elevated 3D Cloud Puffs): 40% slower cross-wind drift (0.60x of Ground Fog v6 velocity)
	const int driftX2 = -((sTicks * 256) / ((cfg.driftDivX * 100) / 462));
	const int driftY2 = (sTicks * 256) / ((cfg.driftDivY * 100) / 294);

	// Step 1: 40% slower counter-rotating internal swirl velocities (divisors scaled by 1 / 0.60)
	const int warpDriftX1 = -(sTicks * 256) / 115;
	const int warpDriftY1 = (sTicks * 256) / 148;
	const int warpDriftX2 = (sTicks * 256) / 100;
	const int warpDriftY2 = -(sTicks * 256) / 128;

	if (cfg.hasPulse) {
		const int phase = (sTicks / 153) & 0xFF;
		const int triWave = (phase < 128) ? phase : (255 - phase);
		gCachedPulseOffset = (triWave >> 2) - 16;
	} else {
		gCachedPulseOffset = 0;
	}

	const int yStart = forceFullRebuild ? 0 : (static_cast<int>(gFogSheetSliceIdx) * 64);
	const int yEnd = forceFullRebuild ? 256 : (yStart + 64);
	gFogSheetSliceIdx = (gFogSheetSliceIdx + 1U) & 3U;

	for (int y = yStart; y < yEnd; y++) {
		const int baseV = (y << 8);
		const int vWarpFixed0 = baseV + warpDriftY1;
		const int vWarpFixed1 = baseV + (73 << 8) - warpDriftX1;
		const int vWarpFixed2 = ((y + 131) << 8) + warpDriftY2;

		for (int x = 0; x < 256; x++) {
			const int baseU = (x << 8);
			const int uWarpFixed0 = baseU + warpDriftX1;
			const int uWarpFixed1 = baseU + (41 << 8) + warpDriftY1;
			const int uWarpFixed2 = ((x + 59) << 8) + warpDriftX2;

			const int wA = SampleLutBilinear(gFogWarpLut, uWarpFixed0, vWarpFixed0) - 128;
			const int wB = SampleLutBilinear(gFogWarpLut, uWarpFixed1, vWarpFixed1) - 128;
			const int wC = SampleLutBilinear(gFogWarpLut, uWarpFixed2, vWarpFixed2) - 128;

			const int warpX1 = wA * 28 + wC * 16;
			const int warpY1 = wB * 22 - wC * 12;

			// 1. Layer 1: Low Ground-Hugging Mist Bed
			const int u1 = baseU + warpX1 + driftX1;
			const int v1 = baseV + warpY1 + driftY1;
			gCachedFloorFog[y][x] = static_cast<uint8_t>(SampleLutBilinear(gFogNoiseLut, u1, v1));

			// 2. Layer 2: Elevated 3D Cloud Puffs
			const int warpX2 = -wB * 30 + wC * 18;
			const int warpY2 = wA * 24 + wC * 14;
			const int u2 = baseU + warpX2 + driftX2 + (137 << 8);
			const int v2 = baseV + warpY2 + driftY2 + (89 << 8);
			gCachedCanopyFog[y][x] = static_cast<uint8_t>(SampleLutBilinear(gFogCanopyNoiseLut, u2, v2));
		}
	}

	// Update 3D NW->SE dome slope for the updated slice
	for (int y = yStart; y < yEnd; y++) {
		const int yNW = (y - 2) & 0xFF;
		const int ySE = (y + 2) & 0xFF;
		for (int x = 0; x < 256; x++) {
			const int xNW = (x - 2) & 0xFF;
			const int xSE = (x + 2) & 0xFF;
			const int slope = (static_cast<int>(gCachedCanopyFog[y][x]) - static_cast<int>(gCachedCanopyFog[yNW][xNW]))
			    + ((static_cast<int>(gCachedCanopyFog[yNW][xNW]) - static_cast<int>(gCachedCanopyFog[ySE][xSE])) >> 1);
			gCachedCanopySlope[y][x] = static_cast<int8_t>(std::clamp(slope, -127, 127));
		}
	}
}

uint32_t GetFloorFogSeedHash()
{
	uint32_t h = 2166136261U;
	const uint32_t floorSeed = (currlevel < 25) ? glSeedTbl[currlevel] : 0U;
	h = (h ^ floorSeed) * 16777619U;
	h = (h ^ static_cast<uint32_t>(currlevel)) * 16777619U;
	h = (h ^ static_cast<uint32_t>(leveltype)) * 16777619U;
	h ^= h >> 16;
	h *= 0x85ebca6bU;
	h ^= h >> 13;
	return h;
}

inline bool IsCavesLavaTile(Point tile)
{
	if (leveltype != DTYPE_CAVES || !InDungeonBounds(tile))
		return false;
	const uint16_t piece = dPiece[tile.x][tile.y];
	if (piece == 0)
		return false;
	// Open lava river & pool pieces in l3.til
	if ((piece >= 55 && piece <= 146) || (piece >= 153 && piece <= 160) || piece == 149 || piece == 151)
		return true;
	return false;
}

inline bool ComputeFogPassableTile(Point tile)
{
	if (!InDungeonBounds(tile))
		return false;

	const uint16_t piece = dPiece[tile.x][tile.y];
	if (piece == 0)
		return false;

	if (!TileHasAny(piece, TileProperties::Solid))
		return true;

	// Cathedral standing floor torches/candlesticks (LAMPS miniset: dPiece 269)
	if (leveltype == DTYPE_CATHEDRAL && piece == 269)
		return true;

	// Caves open lava river tiles (excluding cliff/wall rock pieces!)
	if (IsCavesLavaTile(tile)) {
		const MICROS &micros = DPieceMicros[piece];
		for (int i = 2; i < MicroTileLen; i++) {
			if (micros.mt[i] != 0)
				return false;
		}
		return true;
	}

	return false;
}

inline bool IsFogPassableTile(Point tile)
{
	if (!InDungeonBounds(tile))
		return false;
	return gFogTilePassable[tile.x][tile.y];
}

void CarveHalfTileEntityWake(Point tile, Displacement subTileOffset, int carveStrength, int pushVx, int pushVy)
{
	if (!InDungeonBounds(tile))
		return;

	const int offX = std::clamp(subTileOffset.deltaX, -32, 32);
	const int offY = std::clamp(subTileOffset.deltaY, -16, 16);
	// Matches scrollrt.cpp isometric axes: tx++ moves (+32, +16), ty++ moves (-32, +16)
	const int dHx16 = (offY * 2 + offX) / 2;
	const int dHy16 = (offY * 2 - offX) / 2;

	const int centerHx16 = (tile.x * 2 + 1) * 16 + dHx16;
	const int centerHy16 = (tile.y * 2 + 1) * 16 + dHy16;

	// Wider 1.6-tile wake radius (Radius16 = 52) so walking/attacking/missiles carve all 4 shared tile vertices!
	constexpr int Radius16 = 52;
	constexpr int RadiusSq = Radius16 * Radius16;

	const int minHx = std::max(0, (centerHx16 - Radius16) / 16);
	const int maxHx = std::min(HalfGridW - 1, (centerHx16 + Radius16 + 15) / 16);
	const int minHy = std::max(0, (centerHy16 - Radius16) / 16);
	const int maxHy = std::min(HalfGridH - 1, (centerHy16 + Radius16 + 15) / 16);

	for (int hx = minHx; hx <= maxHx; hx++) {
		const int nodeX16 = hx * 16;
		const int dx = nodeX16 - centerHx16;
		for (int hy = minHy; hy <= maxHy; hy++) {
			const int nodeY16 = hy * 16;
			const int dy = nodeY16 - centerHy16;
			const int distSq = dx * dx + dy * dy;
			if (distSq >= RadiusSq)
				continue;

			const int falloff256 = ((RadiusSq - distSq) * 256) / RadiusSq;
			const int smoothFalloff = (falloff256 * falloff256) >> 8;
			const int carve = (carveStrength * smoothFalloff) >> 8;

			FogHalfCell &cell = gFogHalfGrid[hx][hy];
			cell.density = static_cast<uint8_t>(std::max(0, static_cast<int>(cell.density) - carve));

			const int radialPushX = (dx * 16) / Radius16;
			const int radialPushY = (dy * 16) / Radius16;
			const int targetVx = ((pushVx + radialPushX) * smoothFalloff) >> 8;
			const int targetVy = ((pushVy + radialPushY) * smoothFalloff) >> 8;
			cell.velX = static_cast<int8_t>(std::clamp(static_cast<int>(cell.velX) + targetVx, -96, 96));
			cell.velY = static_cast<int8_t>(std::clamp(static_cast<int>(cell.velY) + targetVy, -96, 96));
		}
	}
}

inline int LightIndexToVisibility256(int lIdx)
{
	if (lIdx >= LightsMax)
		return 0;
	if (lIdx <= 9)
		return 256;
	static constexpr int Lut[5] = { 218, 172, 120, 68, 24 };
	return Lut[std::clamp(lIdx - 10, 0, 4)];
}

/**
 * Evaluates fog weight (density * wallFade * lightVisibility), light shade index (0..14 * 256),
 * and entity wake swirl at a SHARED tile-grid vertex (vx, vy).
 * Samples the exact vertex half-grid node (vx * 2, vy * 2) so entity wakes disperse fog down to 0%
 * while preserving 100% C0 continuity across adjacent tiles!
 */
inline void SampleSharedGridVertex(
    int vx,
    int vy,
    int &outWeight256,
    int &outLight256,
    int &outSwirlX,
    int &outSwirlY)
{
	const int cvx = std::clamp(vx, 0, MAXDUNX);
	const int cvy = std::clamp(vy, 0, MAXDUNY);
	const int wallFade = static_cast<int>(gFogVertexWallFade[cvx][cvy]);
	if (wallFade == 0) {
		outWeight256 = 0;
		outLight256 = 14 * 256;
		outSwirlX = 0;
		outSwirlY = 0;
		return;
	}

	const int hx = std::clamp(cvx * 2, 0, HalfGridW - 1);
	const int hy = std::clamp(cvy * 2, 0, HalfGridH - 1);
	const FogHalfCell &vCell = gFogHalfGrid[hx][hy];

	const int tX0 = std::clamp(vx - 1, 0, MAXDUNX - 1);
	const int tX1 = std::clamp(vx, 0, MAXDUNX - 1);
	const int tY0 = std::clamp(vy - 1, 0, MAXDUNY - 1);
	const int tY1 = std::clamp(vy, 0, MAXDUNY - 1);

	const int l00 = dLight[tX0][tY0];
	const int l10 = dLight[tX1][tY0];
	const int l01 = dLight[tX0][tY1];
	const int l11 = dLight[tX1][tY1];

	const int avgVis = (LightIndexToVisibility256(l00) + LightIndexToVisibility256(l10)
	                       + LightIndexToVisibility256(l01) + LightIndexToVisibility256(l11))
	    >> 2;
	const int avgLight = (std::clamp(l00, 0, 14) + std::clamp(l10, 0, 14)
	                         + std::clamp(l01, 0, 14) + std::clamp(l11, 0, 14))
	    << 6; // * 256 / 4

	outWeight256 = (((static_cast<int>(vCell.density) * wallFade) >> 8) * avgVis) >> 8;
	outLight256 = avgLight;
	outSwirlX = static_cast<int>(vCell.velX);
	outSwirlY = static_cast<int>(vCell.velY);
}

// ============================================================================
// Caves Boiling Lava Bubbles (Rendered Below Ground Fog + 15-Tile Smooth Audio)
// ============================================================================

struct LavaBubble {
	bool active;
	Point tile;
	int8_t offsetX;
	int8_t offsetY;
	bool snappedToLava;
	uint8_t swellTicks;
	uint8_t burstTicks;
	uint8_t rippleTicks;
	uint8_t age;
	uint8_t maxRadX;
	uint8_t maxRadY;
	uint8_t seed;
};

constexpr size_t MaxLavaBubbles = 36;
std::array<LavaBubble, MaxLavaBubbles> gLavaBubbles;
std::array<std::array<uint8_t, MAXDUNY>, MAXDUNX> gBubbleCountOnTile;
uint32_t gLavaBubbleRngState = 0xA341316CU;

uint32_t gCachedLavaPaletteVersion = 0xFFFFFFFFU;
uint8_t gLavaCrustIdx = 1;
uint8_t gLavaBodyIdx = 10;
uint8_t gLavaRimIdx = 22;
uint8_t gLavaSpecIdx = 31;

uint32_t NextBubbleRand()
{
	uint32_t x = gLavaBubbleRngState;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	gLavaBubbleRngState = (x != 0) ? x : 0xA341316CU;
	return gLavaBubbleRngState;
}

/**
 * Updates live lava bubble indices from the rotated system_palette[1..31].
 */
void EnsureLiveLavaBubbleShades()
{
	if (PauseMode != 0 || (!gbIsMultiplayer && gmenu_is_active()))
		return;
	if (gCachedLavaPaletteVersion == pal_surface_palette_version)
		return;
	gCachedLavaPaletteVersion = pal_surface_palette_version;

	struct IdxLum {
		uint8_t idx;
		int lum;
	};
	std::array<IdxLum, 31> entries;
	for (int i = 1; i <= 31; i++) {
		const SDL_Color &c = system_palette[i];
		entries[i - 1] = { static_cast<uint8_t>(i), static_cast<int>(c.r) * 2 + static_cast<int>(c.g) * 3 + static_cast<int>(c.b) };
	}
	std::sort(entries.begin(), entries.end(), [](const IdxLum &a, const IdxLum &b) {
		return a.lum < b.lum;
	});

	gLavaCrustIdx = entries[4].idx;
	gLavaBodyIdx = entries[15].idx;
	gLavaRimIdx = entries[24].idx;
	gLavaSpecIdx = entries[30].idx;
}

/**
 * Plays LS_LAVABUBBLES with a 2.5x extended hearing distance (MaxHearRadius = 15 tiles)
 * and a smooth quadratic distance attenuation curve (no abrupt light-radius cutoff).
 */
void SyncLavaBubbleAudio()
{
	const bool isMenuPaused = (PauseMode != 0 || (!gbIsMultiplayer && gmenu_is_active()));
	if (!gbRunGame || !gbSoundOn || isMenuPaused || leveltype != DTYPE_CAVES || setlevel) {
		StopSFX(LS_LAVABUBBLES);
		return;
	}

	const Point center = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : ViewPosition;
	constexpr int MaxHearRadius = 15; // 2.5x previous 6-tile radius
	int minDistSq = (MaxHearRadius + 1) * (MaxHearRadius + 1);

	const int minX = std::max(0, center.x - MaxHearRadius);
	const int maxX = std::min(MAXDUNX - 1, center.x + MaxHearRadius);
	const int minY = std::max(0, center.y - MaxHearRadius);
	const int maxY = std::min(MAXDUNY - 1, center.y + MaxHearRadius);

	for (int x = minX; x <= maxX; x++) {
		const int dx = x - center.x;
		for (int y = minY; y <= maxY; y++) {
			const int dy = y - center.y;
			const int dSq = dx * dx + dy * dy;
			if (dSq >= minDistSq)
				continue;
			if (IsCavesLavaTile({ x, y })) {
				minDistSq = dSq;
			}
		}
	}

	if (minDistSq > MaxHearRadius * MaxHearRadius) {
		StopSFX(LS_LAVABUBBLES);
		return;
	}

	const float dist = std::sqrt(static_cast<float>(minDistSq));
	const float normDist = std::clamp(dist / static_cast<float>(MaxHearRadius), 0.0F, 1.0F);
	// Smoothly fade from -150 mB at 0 tiles down to -2350 mB at 15 tiles (no abrupt step)
	const float smoothCurve = normDist * (0.45F + 0.55F * normDist);
	const int lavaVolume = -150 - static_cast<int>(smoothCurve * 2200.0F);
	PlayLoopingSFX(LS_LAVABUBBLES, lavaVolume);
}

void TrySpawnCavesLavaBubble(Point center, bool randomInitialAge)
{
	const uint32_t r = NextBubbleRand();
	const int tx = center.x - 15 + static_cast<int>(r % 31U);
	const int ty = center.y - 15 + static_cast<int>((r >> 6) % 31U);
	const Point candidate { tx, ty };
	if (!IsCavesLavaTile(candidate))
		return;

	if (gBubbleCountOnTile[tx][ty] >= 1)
		return;

	LavaBubble *freeSlot = nullptr;
	for (LavaBubble &b : gLavaBubbles) {
		if (!b.active) {
			freeSlot = &b;
			break;
		}
	}
	if (freeSlot == nullptr)
		return;

	const uint32_t r2 = NextBubbleRand();
	freeSlot->active = true;
	freeSlot->tile = candidate;
	// Keep bubble center well inside the current tile's 64x32 diamond so neighbor tiles never clip it
	freeSlot->offsetX = static_cast<int8_t>(-7 + static_cast<int>(r2 % 15U));
	freeSlot->offsetY = static_cast<int8_t>(-3 + static_cast<int>((r2 >> 5) % 7U));
	freeSlot->snappedToLava = false;
	// Slow, viscous magma bubble growth (~3.0 - 4.5 seconds at 20 Hz simulation tick)
	freeSlot->swellTicks = static_cast<uint8_t>(60 + ((r2 >> 9) % 32U));
	freeSlot->burstTicks = static_cast<uint8_t>(14 + ((r2 >> 15) % 6U));
	freeSlot->rippleTicks = static_cast<uint8_t>(16 + ((r2 >> 18) % 8U));
	freeSlot->age = randomInitialAge ? static_cast<uint8_t>(r2 % freeSlot->swellTicks) : 0;
	// +40% larger lava bubble radii (7..10 px horizontal, 5..7 px vertical)
	freeSlot->maxRadX = static_cast<uint8_t>(7 + ((r2 >> 21) % 4U));
	freeSlot->maxRadY = static_cast<uint8_t>(std::max<int>(5, (freeSlot->maxRadX * 2 + 1) / 3));
	freeSlot->seed = static_cast<uint8_t>((r2 >> 24) & 0xFFU);
	gBubbleCountOnTile[tx][ty]++;
}

void ResetCavesLavaBubbles()
{
	StopSFX(LS_LAVABUBBLES);
	for (LavaBubble &b : gLavaBubbles) {
		b.active = false;
	}
	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			gBubbleCountOnTile[x][y] = 0;
		}
	}
	gLavaBubbleRngState = GetFloorFogSeedHash() ^ 0x5BF03635U;
	gCachedLavaPaletteVersion = 0xFFFFFFFFU;

	if (leveltype == DTYPE_CAVES) {
		const Point center = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : ViewPosition;
		for (int i = 0; i < 64; i++) {
			TrySpawnCavesLavaBubble(center, true);
		}
	}
}

void UpdateCavesLavaBubblesSimulation()
{
	if (leveltype != DTYPE_CAVES) {
		StopSFX(LS_LAVABUBBLES);
		return;
	}

	if (PauseMode != 0 || (!gbIsMultiplayer && gmenu_is_active())) {
		StopSFX(LS_LAVABUBBLES);
		return;
	}

	const Point center = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : ViewPosition;

	for (LavaBubble &b : gLavaBubbles) {
		if (!b.active)
			continue;

		b.age++;
		if (b.age >= b.swellTicks + b.burstTicks + b.rippleTicks
		    || std::abs(b.tile.x - center.x) > 20
		    || std::abs(b.tile.y - center.y) > 20) {
			if (InDungeonBounds(b.tile) && gBubbleCountOnTile[b.tile.x][b.tile.y] > 0) {
				gBubbleCountOnTile[b.tile.x][b.tile.y]--;
			}
			b.active = false;
		}
	}

	for (int attempt = 0; attempt < 3; attempt++) {
		TrySpawnCavesLavaBubble(center, false);
	}

	SyncLavaBubbleAudio();
}

inline bool IsSafeLavaFootprint(const Surface &out, int cx, int cy, int radX, int radY)
{
	const int rx = radX + 2;
	const int ry = radY + 2;
	if (cx - rx < 0 || cx + rx >= out.w() || cy - ry < 0 || cy + ry >= out.h())
		return false;

	auto isLavaPx = [&](int px, int py) {
		const uint8_t c = out[{ px, py }];
		return c >= 1 && c <= 31;
	};

	return isLavaPx(cx, cy) && isLavaPx(cx - rx, cy) && isLavaPx(cx + rx, cy)
	    && isLavaPx(cx, cy - ry) && isLavaPx(cx, cy + ry);
}

void DrawCavesLavaBubblesOnTile(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
{
	if (leveltype != DTYPE_CAVES || lightIndex >= LightsMax)
		return;
	if (!InDungeonBounds(tilePosition) || gBubbleCountOnTile[tilePosition.x][tilePosition.y] == 0)
		return;

	EnsureLiveLavaBubbleShades();

	// Use the live luminance-sorted lava palette indices (1..31) directly!
	const uint8_t crustCol = gLavaCrustIdx;
	const uint8_t bodyCol = gLavaBodyIdx;
	const uint8_t rimCol = gLavaRimIdx;
	const uint8_t specCol = gLavaSpecIdx;

	const int tileCenterX = targetBufferPosition.x + 32;
	const int tileCenterY = targetBufferPosition.y - 16;

	auto plotLavaOnly = [&](int px, int py, uint8_t col) {
		if (px < 0 || px >= out.w() || py < 0 || py >= out.h())
			return;
		uint8_t &dst = out[{ px, py }];
		if (dst >= 1 && dst <= 31) {
			dst = col;
		}
	};

	for (LavaBubble &b : gLavaBubbles) {
		if (!b.active || b.tile != tilePosition)
			continue;

		const int maxRx = static_cast<int>(b.maxRadX);
		const int maxRy = static_cast<int>(b.maxRadY);

		if (!b.snappedToLava) {
			bool foundSafeSpot = IsSafeLavaFootprint(out, tileCenterX + b.offsetX, tileCenterY + b.offsetY, maxRx, maxRy);
			if (!foundSafeSpot) {
				for (int tryIdx = 0; tryIdx < 16; tryIdx++) {
					const int candOffX = -8 + ((b.seed * 7 + tryIdx * 5) % 17);
					const int candOffY = -4 + ((b.seed * 3 + tryIdx * 3) % 9);
					if (IsSafeLavaFootprint(out, tileCenterX + candOffX, tileCenterY + candOffY, maxRx, maxRy)) {
						b.offsetX = static_cast<int8_t>(candOffX);
						b.offsetY = static_cast<int8_t>(candOffY);
						foundSafeSpot = true;
						break;
					}
				}
			}
			if (!foundSafeSpot) {
				if (gBubbleCountOnTile[tilePosition.x][tilePosition.y] > 0)
					gBubbleCountOnTile[tilePosition.x][tilePosition.y]--;
				b.active = false;
				continue;
			}
			b.snappedToLava = true;
		}

		const int cx = tileCenterX + static_cast<int>(b.offsetX);
		const int cy = tileCenterY + static_cast<int>(b.offsetY);

		if (b.age < b.swellTicks) {
			const int progress256 = (static_cast<int>(b.age) * 256) / std::max<int>(1, b.swellTicks);
			const int ease256 = 256 - (((256 - progress256) * (256 - progress256)) >> 8);
			// Sub-pixel 1/16th fixed-point radii eliminate integer step-snapping during growth!
			const int rx16 = std::max(40, (maxRx * 16 * ease256 + 128) >> 8);
			const int ry16 = std::max(32, (maxRy * 16 * ease256 + 128) >> 8);
			const int rxBound = (rx16 + 15) >> 4;
			const int ryBound = (ry16 + 15) >> 4;
			const int domeLift = (6 * ease256 + 128) >> 8;

			const int rxSq256 = std::max(1, rx16 * rx16);
			const int rySq256 = std::max(1, ry16 * ry16);

			for (int dy = -ryBound - domeLift - 1; dy <= ryBound + 1; dy++) {
				for (int dx = -rxBound - 1; dx <= rxBound + 1; dx++) {
					const int baseNorm256 = (dx * dx * 65536) / rxSq256 + (dy * dy * 65536) / rySq256;
					if (baseNorm256 >= 196 && baseNorm256 <= 284 && dy >= -1) {
						plotLavaOnly(cx + dx, cy + dy, crustCol);
					}

					const int domeDy = dy + domeLift;
					const int domeNorm256 = (dx * dx * 65536) / rxSq256 + (domeDy * domeDy * 65536) / rySq256;
					if (domeNorm256 > 256)
						continue;

					const int specDx16 = dx * 16 + std::max(16, rx16 / 3);
					const int specDy16 = domeDy * 16 + std::max(16, ry16 / 3);
					const int specNorm256 = (specDx16 * specDx16 * 256) / rxSq256 + (specDy16 * specDy16 * 256) / rySq256;

					uint8_t col = bodyCol;
					if (domeNorm256 >= 196) {
						col = crustCol;
					} else if (specNorm256 <= 44) {
						col = specCol;
					} else if (specNorm256 <= 118) {
						col = rimCol;
					}
					plotLavaOnly(cx + dx, cy + dy, col);
				}
			}
		} else if (b.age < b.swellTicks + b.burstTicks) {
			const int burstAge = static_cast<int>(b.age - b.swellTicks);
			const int burst256 = (burstAge * 256) / std::max<int>(1, b.burstTicks);
			const int rx16 = maxRx * 16 + ((burst256 * 48) >> 8);
			const int ry16 = maxRy * 16 + ((burst256 * 28) >> 8);
			const int rxBound = (rx16 + 15) >> 4;
			const int ryBound = (ry16 + 15) >> 4;
			const int rxSq256 = std::max(1, rx16 * rx16);
			const int rySq256 = std::max(1, ry16 * ry16);

			for (int dy = -ryBound - 1; dy <= ryBound + 1; dy++) {
				for (int dx = -rxBound - 1; dx <= rxBound + 1; dx++) {
					const int norm256 = (dx * dx * 65536) / rxSq256 + (dy * dy * 65536) / rySq256;
					if (norm256 < 115 || norm256 > 264)
						continue;

					const int angleHash = ((dx * 7 + dy * 13 + static_cast<int>(b.seed)) & 7);
					if (burst256 > 140 && angleHash < 2)
						continue;

					const uint8_t col = (norm256 >= 170 && norm256 <= 225)
					    ? ((angleHash == 3) ? specCol : rimCol)
					    : bodyCol;
					plotLavaOnly(cx + dx, cy + dy, col);
				}
			}
		} else {
			const int rippleAge = static_cast<int>(b.age - b.swellTicks - b.burstTicks);
			const int ripple256 = (rippleAge * 256) / std::max<int>(1, b.rippleTicks);
			const int rx16 = (maxRx + 3) * 16 + ((ripple256 * 40) >> 8);
			const int ry16 = (maxRy + 2) * 16 + ((ripple256 * 24) >> 8);
			const int rxBound = (rx16 + 15) >> 4;
			const int ryBound = (ry16 + 15) >> 4;
			const int rxSq256 = std::max(1, rx16 * rx16);
			const int rySq256 = std::max(1, ry16 * ry16);

			for (int dy = -ryBound; dy <= ryBound; dy++) {
				for (int dx = -rxBound; dx <= rxBound; dx++) {
					const int norm256 = (dx * dx * 65536) / rxSq256 + (dy * dy * 65536) / rySq256;
					if (norm256 >= 185 && norm256 <= 256) {
						plotLavaOnly(cx + dx, cy + dy, (ripple256 < 128) ? bodyCol : crustCol);
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
	gLastFogSheetDtype = DTYPE_NONE;

	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			gFogTilePassable[x][y] = ComputeFogPassableTile({ x, y });
		}
	}
	for (int x = 1; x < MAXDUNX - 1; x++) {
		for (int y = 1; y < MAXDUNY - 1; y++) {
			if (gFogTilePassable[x][y] || dPiece[x][y] == 0)
				continue;
			const int passableCardinals = (gFogTilePassable[x - 1][y] ? 1 : 0)
			    + (gFogTilePassable[x + 1][y] ? 1 : 0)
			    + (gFogTilePassable[x][y - 1] ? 1 : 0)
			    + (gFogTilePassable[x][y + 1] ? 1 : 0);
			if (passableCardinals >= 3) {
				gFogTilePassable[x][y] = true;
			}
		}
	}

	// Precompute vertex wall-fade weights at every shared tile-grid vertex (vx, vy):
	for (int vx = 0; vx <= MAXDUNX; vx++) {
		for (int vy = 0; vy <= MAXDUNY; vy++) {
			int pass4 = 0;
			for (int ox = -1; ox <= 0; ox++) {
				const int tx = std::clamp(vx + ox, 0, MAXDUNX - 1);
				for (int oy = -1; oy <= 0; oy++) {
					const int ty = std::clamp(vy + oy, 0, MAXDUNY - 1);
					if (gFogTilePassable[tx][ty])
						pass4++;
				}
			}
			if (pass4 == 0) {
				gFogVertexWallFade[vx][vy] = 0;
			} else if (pass4 == 1) {
				gFogVertexWallFade[vx][vy] = 148;
			} else if (pass4 == 2) {
				gFogVertexWallFade[vx][vy] = 192;
			} else if (pass4 == 3) {
				gFogVertexWallFade[vx][vy] = 228;
			} else {
				gFogVertexWallFade[vx][vy] = 256;
			}
		}
	}

	const LevelFogConfig cfg = GetLevelFogConfig();
	if (!cfg.enabled || leveltype == DTYPE_TOWN || setlevel) {
		gLevelHasFog = false;
		for (int hx = 0; hx < HalfGridW; hx++) {
			for (int hy = 0; hy < HalfGridH; hy++) {
				gFogHalfGrid[hx][hy] = { 0, 0, 0 };
			}
		}
		ResetCavesLavaBubbles();
		return;
	}

	const uint32_t roll = GetFloorFogSeedHash() % 100U;
	gLevelHasFog = (roll < cfg.spawnChancePercent);

	const uint8_t initDensity = gLevelHasFog ? cfg.maxDensity : 0;
	for (int hx = 0; hx < HalfGridW; hx++) {
		const int tx = std::clamp(hx / 2, 0, MAXDUNX - 1);
		for (int hy = 0; hy < HalfGridH; hy++) {
			const int ty = std::clamp(hy / 2, 0, MAXDUNY - 1);
			const bool pass = gFogTilePassable[tx][ty];
			gFogHalfGrid[hx][hy] = { static_cast<uint8_t>(pass ? initDensity : 0), 0, 0 };
		}
	}

	ResetCavesLavaBubbles();
}

void UpdateGroundFogSimulation()
{
	UpdateCavesLavaBubblesSimulation();

	if (!*sgOptions.Graphics.groundFog || !gLevelHasFog)
		return;

	const LevelFogConfig cfg = GetLevelFogConfig();
	if (!cfg.enabled || leveltype == DTYPE_TOWN || setlevel)
		return;

	for (int hx = 0; hx < HalfGridW; hx++) {
		const int tx = std::clamp(hx / 2, 0, MAXDUNX - 1);
		for (int hy = 0; hy < HalfGridH; hy++) {
			const int ty = std::clamp(hy / 2, 0, MAXDUNY - 1);
			FogHalfCell &cell = gFogHalfGrid[hx][hy];
			if (!gFogTilePassable[tx][ty]) {
				cell = { 0, 0, 0 };
				continue;
			}

			if (cell.density < cfg.maxDensity) {
				const int step = (cell.density < 110) ? 1 : 2;
				cell.density = static_cast<uint8_t>(std::min<int>(cfg.maxDensity, static_cast<int>(cell.density) + step));
			}
			cell.velX = static_cast<int8_t>((static_cast<int>(cell.velX) * 7) / 8);
			cell.velY = static_cast<int8_t>((static_cast<int>(cell.velY) * 7) / 8);
		}
	}

	for (const Player &player : Players) {
		if (!player.plractive || !player.isOnActiveLevel())
			continue;

		const Point pos = player.position.tile;
		const Point fut = player.position.future;
		const bool isMoving = (fut != pos) || (player._pmode == PM_WALK_NORTHWARDS || player._pmode == PM_WALK_SOUTHWARDS || player._pmode == PM_WALK_SIDEWAYS);
		const bool isSwinging = (player._pmode == PM_ATTACK || player._pmode == PM_RATTACK || player._pmode == PM_SPELL);
		if (!isMoving && !isSwinging)
			continue;

		const int dirX = std::clamp(fut.x - pos.x, -1, 1);
		const int dirY = std::clamp(fut.y - pos.y, -1, 1);
		const int pushVx = -dirX * 36 + dirY * 14;
		const int pushVy = -dirY * 36 - dirX * 14;
		const int carve = isMoving ? 155 : 135;

		CarveHalfTileEntityWake(pos, { 0, 0 }, carve, pushVx, pushVy);
		if (isMoving && InDungeonBounds(fut) && IsFogPassableTile(fut)) {
			CarveHalfTileEntityWake(fut, { 0, 0 }, 115, pushVx, pushVy);
		}
	}

	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (monster.isInvalid || monster.hitPoints <= 0)
			continue;

		const Point pos = monster.position.tile;
		const Point fut = monster.position.future;
		const bool isMoving = (fut != pos);
		if (!isMoving && monster.mode != MonsterMode::MeleeAttack && monster.mode != MonsterMode::SpecialMeleeAttack)
			continue;

		const int dirX = std::clamp(fut.x - pos.x, -1, 1);
		const int dirY = std::clamp(fut.y - pos.y, -1, 1);
		const int carve = isMoving ? 135 : 95;

		CarveHalfTileEntityWake(pos, { 0, 0 }, carve, -dirX * 28, -dirY * 28);
		if (isMoving && InDungeonBounds(fut) && IsFogPassableTile(fut)) {
			CarveHalfTileEntityWake(fut, { 0, 0 }, 95, -dirX * 28, -dirY * 28);
		}
	}

	for (const Missile &missile : Missiles) {
		if (missile._miDelFlag)
			continue;

		const Point pos = missile.position.tile;
		const int pushVx = std::clamp(-missile.position.velocity.deltaX / 512, -48, 48);
		const int pushVy = std::clamp(-missile.position.velocity.deltaY / 512, -48, 48);
		CarveHalfTileEntityWake(pos, missile.position.offset, 160, pushVx, pushVy);
	}
}

void DrawCavesLavaBubblesTile(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
{
	if (leveltype == DTYPE_CAVES) {
		DrawCavesLavaBubblesOnTile(out, tilePosition, targetBufferPosition, lightIndex);
	}
}

void DrawGroundFogTileSpan(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex, int dyMin, int dyMax)
{
	if (!*sgOptions.Graphics.groundFog || !gLevelHasFog)
		return;

	if (!InDungeonBounds(tilePosition) || lightIndex >= LightsMax)
		return;

	const int tx = tilePosition.x;
	const int ty = tilePosition.y;
	if (!gFogTilePassable[tx][ty])
		return;

	const LevelFogConfig cfg = GetLevelFogConfig();
	if (!cfg.enabled)
		return;

	EnsureFogNoiseInitialized();
	if (leveltype == DTYPE_CAVES) {
		EnsureLiveLavaBubbleShades();
	}

	const uint32_t ticks = SDL_GetTicks();
	UpdateCachedFogSheets30Hz(ticks, cfg);

	// Evaluate fog weight, light index, and swirl at the 4 SHARED tile-grid vertices:
	//   (tx, ty) = Top, (tx + 1, ty) = Right, (tx, ty + 1) = Left, (tx + 1, ty + 1) = Bottom.
	int w00 = 0, l00 = 0, sx00 = 0, sy00 = 0;
	int w10 = 0, l10 = 0, sx10 = 0, sy10 = 0;
	int w01 = 0, l01 = 0, sx01 = 0, sy01 = 0;
	int w11 = 0, l11 = 0, sx11 = 0, sy11 = 0;

	SampleSharedGridVertex(tx,     ty,     w00, l00, sx00, sy00);
	SampleSharedGridVertex(tx + 1, ty,     w10, l10, sx10, sy10);
	SampleSharedGridVertex(tx,     ty + 1, w01, l01, sx01, sy01);
	SampleSharedGridVertex(tx + 1, ty + 1, w11, l11, sx11, sy11);

	if ((w00 | w10 | w01 | w11) <= 4)
		return;

	// Precompute shaded fog colors for all 15 light steps (0..14)
	uint8_t crestByLight[15];
	uint8_t coreByLight[15];
	uint8_t troughByLight[15];
	uint8_t lowerDomeByLight[15];
	for (int l = 0; l < 15; l++) {
		crestByLight[l] = ShadeFogColor(cfg.crestShade, l);
		coreByLight[l] = ShadeFogColor(cfg.coreShade, l);
		troughByLight[l] = ShadeFogColor(cfg.troughShade, l);
		lowerDomeByLight[l] = Blend50(troughByLight[l], coreByLight[l]);
	}

	// Average swirl offset across the tile
	const int swirlTexX = ((sx00 + sx10 + sx01 + sx11) >> 3);
	const int swirlTexY = ((sy00 + sy10 + sy01 + sy11) >> 3);

	// Ground Fog v6 (nice-fog2.png) isometric floor UV scales:
	//   Layer 1 (Low Ground Mist): uvScale1 = 64 (1:1 isometric texel resolution)
	//   Layer 2 (3D Cloud Puffs):  uvScale2 = cfg.uvScale (44 -> ~1.45x larger billowing 3D cloud puffs)
	constexpr int UvScale1 = 64;
	const int uvScale2 = static_cast<int>(cfg.uvScale);

	const int floorCutoff = static_cast<int>(cfg.intensityCutoff) + gCachedPulseOffset;
	const int canopyCutoff = static_cast<int>(cfg.intensityCutoff) + 24 + gCachedPulseOffset;
	const int floorScaleRecip = (256 * 256) / std::max(64, 255 - floorCutoff);
	const int canopyScaleRecip = (256 * 256) / std::max(64, 255 - canopyCutoff);

	const int topY = targetBufferPosition.y - 31;
	const int leftX = targetBufferPosition.x;
	const int clipW = out.w();
	const int clipH = out.h();

	auto evalBilinear256 = [](int c00, int c10, int c01, int c11, int u256, int v256) -> int {
		const int top = (c00 << 8) + (c10 - c00) * u256;
		const int bot = (c01 << 8) + (c11 - c01) * u256;
		return (top + (((bot - top) * v256) >> 8)) >> 8;
	};

	const int clampedDyMin = std::max(1, dyMin);
	const int clampedDyMax = std::min(31, dyMax);

	// Exact non-overlapping isometric diamond scanline partition (dy = clampedDyMin..clampedDyMax)
	for (int dy = clampedDyMin; dy <= clampedDyMax; dy++) {
		const int screenY = topY + dy;
		if (screenY < 0 || screenY >= clipH)
			continue;

		const int halfSpan = (dy <= 16) ? (dy * 2) : ((32 - dy) * 2);
		const int xStart = 32 - halfSpan;
		const int xEnd = 32 + halfSpan;
		const int spanLen = std::max(1, xEnd - xStart);
		const int relY = dy - 16;

		// Evaluate u256/v256 at the left and right endpoints of this scanline
		const int relXLeft = xStart - 32;
		const int uLeft = std::clamp((relY * 2 + relXLeft) * 4 + 128, 0, 256);
		const int vLeft = std::clamp((relY * 2 - relXLeft) * 4 + 128, 0, 256);

		const int relXRight = xEnd - 32;
		const int uRight = std::clamp((relY * 2 + relXRight) * 4 + 128, 0, 256);
		const int vRight = std::clamp((relY * 2 - relXRight) * 4 + 128, 0, 256);

		const int wLeft = evalBilinear256(w00, w10, w01, w11, uLeft, vLeft);
		const int wRight = evalBilinear256(w00, w10, w01, w11, uRight, vRight);
		if ((wLeft | wRight) <= 4)
			continue;

		const int lLeft = evalBilinear256(l00, l10, l01, l11, uLeft, vLeft);
		const int lRight = evalBilinear256(l00, l10, l01, l11, uRight, vRight);

		int wFixed = wLeft << 8;
		const int wStep = ((wRight - wLeft) << 8) / spanLen;
		int lFixed = lLeft << 8;
		const int lStep = ((lRight - lLeft) << 8) / spanLen;

		// Isometric tile-space UV coordinates (matching Ground Fog v6 / nice-fog2.png):
		// Each dx++ step advances u256 by +4 and v256 by -4.
		int u1Tex = (tx * UvScale1 + (uLeft >> 2) + swirlTexX) & 0xFF;
		int v1Tex = (ty * UvScale1 + (vLeft >> 2) + swirlTexY) & 0xFF;

		int u2Fixed = (((tx << 8) + uLeft) * uvScale2) + (swirlTexX << 7);
		int v2Fixed = (((ty << 8) + vLeft) * uvScale2) + (swirlTexY << 7);
		const int u2Step = 4 * uvScale2;
		const int v2Step = -4 * uvScale2;

		const int8_t *bayerRow = Bayer4x4[screenY & 3];
		uint8_t *dstRow = &out[{ 0, screenY }];

		for (int dx = xStart; dx < xEnd; dx++, wFixed += wStep, lFixed += lStep,
		         u1Tex = (u1Tex + 1) & 0xFF, v1Tex = (v1Tex - 1) & 0xFF,
		         u2Fixed += u2Step, v2Fixed += v2Step) {
			const int screenX = leftX + dx;
			if (screenX < 0 || screenX >= clipW)
				continue;

			const int weight = wFixed >> 8;
			if (weight <= 6)
				continue;

			const int rawDensity1 = gCachedFloorFog[v1Tex][u1Tex];
			const int u2Tex = (u2Fixed >> 8) & 0xFF;
			const int v2Tex = (v2Fixed >> 8) & 0xFF;
			const int rawDensity2 = SampleLutBilinear(gCachedCanopyFog, u2Fixed, v2Fixed);
			// Elevated Layer 2 casts its drop-shadow onto the floor ~7px below (+9 texels in u2, v2)
			const int rawShadow2 = gCachedCanopyFog[(v2Tex - 5) & 0xFF][(u2Tex - 5) & 0xFF];

			if (rawDensity1 <= floorCutoff && rawDensity2 <= canopyCutoff && rawShadow2 <= canopyCutoff)
				continue;

			uint8_t outPx = dstRow[screenX];
			// Do not apply static paletteTransparencyLookup over animated Caves lava pixels (1..31)
			if (leveltype == DTYPE_CAVES && outPx >= 1 && outPx <= 31)
				continue;

			const int dither = bayerRow[screenX & 3];
			const int smoothLightIdx = std::clamp(((lFixed >> 8) + dither * 8) >> 8, 0, 14);
			const uint8_t litCrest = crestByLight[smoothLightIdx];
			const uint8_t litCore = coreByLight[smoothLightIdx];
			const uint8_t litTrough = troughByLight[smoothLightIdx];
			const uint8_t litLowerDome = lowerDomeByLight[smoothLightIdx];

			// Pass 1: Layer 1 (Low Ground-Hugging Mist Bed — smoky slate-gray)
			if (rawDensity1 > floorCutoff) {
				const int norm1 = std::min(255, ((rawDensity1 - floorCutoff) * floorScaleRecip) >> 8);
				const int smooth1 = (norm1 * (384 - norm1)) >> 8;
				const int alpha1 = (((smooth1 * weight) >> 8) * 205 >> 8) + dither;
				if (alpha1 > 8) {
					const uint8_t shade1 = (norm1 + dither * 3 > 140) ? litCore : litTrough;
					outPx = ApplySoftFogBlend(shade1, outPx, alpha1, false);
				}
			}

			// Pass 2: Layer 2 Cast Drop-Shadow (blends Pure Black (0) onto bare floor & Layer 1 mist)
			if (rawShadow2 > canopyCutoff && rawShadow2 > rawDensity2 - 18) {
				const int normS = std::min(255, ((rawShadow2 - canopyCutoff) * canopyScaleRecip) >> 8);
				const int shadowAlpha = ((normS * weight) >> 8) + dither;
				uint8_t darkened = outPx;
				if (shadowAlpha > 115) {
					darkened = Blend19(0, outPx);
				} else if (shadowAlpha > 64) {
					darkened = Blend12(0, outPx);
				} else if (shadowAlpha > 24) {
					darkened = Blend6(0, outPx);
				}
				if (darkened != 0) {
					outPx = darkened;
				}
			}

			// Pass 3: Layer 2 (Elevated 3D Cloud Puffs — sculpted smoky silver-white crests & body)
			if (rawDensity2 > canopyCutoff) {
				const int norm2 = std::min(255, ((rawDensity2 - canopyCutoff) * canopyScaleRecip) >> 8);
				const int smooth2 = (norm2 * (384 - norm2)) >> 8;
				const int alpha2 = ((((smooth2 * 230) >> 8) + 20) * weight >> 8) + dither;
				if (alpha2 > 8) {
					const int slope = gCachedCanopySlope[v2Tex][u2Tex];
					uint8_t shade2;
					if (slope * 4 + dither > 22 || norm2 + dither * 3 > 145) {
						shade2 = litCrest;
					} else if (slope * 4 + dither >= -28) {
						shade2 = litCore;
					} else {
						shade2 = litLowerDome;
					}
					outPx = ApplySoftFogBlend(shade2, outPx, alpha2, cfg.allowHeavyBlend);
				}
			}

			dstRow[screenX] = outPx;
		}
	}
}

void DrawGroundFogTileBase(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
{
	if (leveltype == DTYPE_CAVES) {
		DrawCavesLavaBubblesOnTile(out, tilePosition, targetBufferPosition, lightIndex);
	}
	DrawGroundFogTileSpan(out, tilePosition, targetBufferPosition, lightIndex, 1, 31);
}

void DrawGroundFogEntitySubmersion(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
{
	(void)out;
	(void)tilePosition;
	(void)targetBufferPosition;
	(void)lightIndex;
}

} // namespace devilution
