/**
 * @file ground_fog.cpp
 *
 * Implementation of 3D Isometric Volumetric Ground Fog (with 2-Layer Isometric Camera Parallax
 * & 30 Hz Cached Sheet Optimization), Interactive Half-Tile Entity Dispersion, and
 * Caves Boiling Lava Bubbles.
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
	bool isDarkSmoke;
	bool allowHeavyBlend;
	bool hasPulse;
	int driftDivX;
	int driftDivY;
};

bool gNoiseInitialized = false;
bool gLevelHasFog = false;

std::array<std::array<uint8_t, 256>, 256> gFogNoiseLut;
std::array<std::array<uint8_t, 256>, 256> gFogWarpLut;

uint32_t gLastFogSheetUpdateTick = 0;
dungeon_type gLastFogSheetDtype = DTYPE_NONE;
int gCachedPulseOffset = 0;
std::array<std::array<uint8_t, 256>, 256> gCachedFloorFog;
std::array<std::array<uint8_t, 256>, 256> gCachedCanopyFog;
std::array<std::array<uint8_t, 256>, 256> gCachedCanopyLight;

std::array<std::array<FogHalfCell, HalfGridH>, HalfGridW> gFogHalfGrid;
std::array<std::array<bool, MAXDUNY>, MAXDUNX> gFogTilePassable;

LevelFogConfig GetLevelFogConfig()
{
	switch (leveltype) {
	case DTYPE_CATHEDRAL:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/172,
			/*intensityCutoff=*/84,
			/*uvScale=*/11,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 4),
			/*coreShade=*/static_cast<uint8_t>(PAL16_BLUE + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 11),
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*hasPulse=*/false,
			/*driftDivX=*/175,
			/*driftDivY=*/255
		};
	case DTYPE_CATACOMBS:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/184,
			/*intensityCutoff=*/80,
			/*uvScale=*/10,
			/*crestShade=*/static_cast<uint8_t>(PAL16_BEIGE + 5),
			/*coreShade=*/static_cast<uint8_t>(PAL16_BEIGE + 9),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 12),
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*hasPulse=*/false,
			/*driftDivX=*/210,
			/*driftDivY=*/285
		};
	case DTYPE_CAVES:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/186,
			/*intensityCutoff=*/80,
			/*uvScale=*/13,
			/*crestShade=*/static_cast<uint8_t>(PAL16_YELLOW + 5),
			/*coreShade=*/static_cast<uint8_t>(PAL16_ORANGE + 8),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 11),
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*hasPulse=*/false,
			/*driftDivX=*/115,
			/*driftDivY=*/-120
		};
	case DTYPE_HELL:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/220,
			/*intensityCutoff=*/70,
			/*uvScale=*/12,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 12),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 14),
			/*troughShade=*/0,
			/*isDarkSmoke=*/true,
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/false,
			/*driftDivX=*/125,
			/*driftDivY=*/-135
		};
	case DTYPE_NEST:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/192,
			/*intensityCutoff=*/76,
			/*uvScale=*/14,
			/*crestShade=*/static_cast<uint8_t>(PAL16_YELLOW + 4),
			/*coreShade=*/static_cast<uint8_t>(PAL16_YELLOW + 9),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 11),
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*hasPulse=*/true,
			/*driftDivX=*/165,
			/*driftDivY=*/-190
		};
	case DTYPE_CRYPT:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/218,
			/*intensityCutoff=*/68,
			/*uvScale=*/9,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 3),
			/*coreShade=*/static_cast<uint8_t>(PAL16_BEIGE + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 10),
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/false,
			/*driftDivX=*/240,
			/*driftDivY=*/310
		};
	default:
		return { false, 0, 0, 255, 10, 0, 0, 0, false, false, false, 100, 100 };
	}
}

inline uint8_t Blend50(uint8_t fg, uint8_t bg)
{
	return paletteTransparencyLookup[bg][fg];
}

inline uint8_t Blend25(uint8_t fg, uint8_t bg)
{
	const uint8_t mid = paletteTransparencyLookup[bg][fg];
	return paletteTransparencyLookup[bg][mid];
}

inline uint8_t Blend37(uint8_t fg, uint8_t bg)
{
	const uint8_t b50 = paletteTransparencyLookup[bg][fg];
	const uint8_t b25 = paletteTransparencyLookup[bg][b50];
	return paletteTransparencyLookup[b25][b50];
}

inline uint8_t Blend31(uint8_t fg, uint8_t bg)
{
	const uint8_t b50 = paletteTransparencyLookup[bg][fg];
	const uint8_t b25 = paletteTransparencyLookup[bg][b50];
	const uint8_t b37 = paletteTransparencyLookup[b25][b50];
	return paletteTransparencyLookup[b25][b37];
}

inline uint8_t Blend12(uint8_t fg, uint8_t bg)
{
	const uint8_t b50 = paletteTransparencyLookup[bg][fg];
	const uint8_t b25 = paletteTransparencyLookup[bg][b50];
	return paletteTransparencyLookup[bg][b25];
}

inline uint8_t Blend19(uint8_t fg, uint8_t bg)
{
	const uint8_t b50 = paletteTransparencyLookup[bg][fg];
	const uint8_t b25 = paletteTransparencyLookup[bg][b50];
	const uint8_t b12 = paletteTransparencyLookup[bg][b25];
	return paletteTransparencyLookup[b12][b25];
}

inline uint8_t Blend6(uint8_t fg, uint8_t bg)
{
	const uint8_t b50 = paletteTransparencyLookup[bg][fg];
	const uint8_t b25 = paletteTransparencyLookup[bg][b50];
	const uint8_t b12 = paletteTransparencyLookup[bg][b25];
	return paletteTransparencyLookup[bg][b12];
}

uint32_t HashLattice(uint32_t x, uint32_t y, uint32_t seed)
{
	uint32_t h = (x * 374761393U) + (y * 668265263U) + (seed * 1442695041U);
	h = (h ^ (h >> 13)) * 1274126177U;
	return h ^ (h >> 16);
}

float ValueNoiseTileable(int x, int y, int period, uint32_t seed)
{
	const int cellSize = 256 / period;
	const int gx0 = (x / cellSize) % period;
	const int gy0 = (y / cellSize) % period;
	const int gx1 = (gx0 + 1) % period;
	const int gy1 = (gy0 + 1) % period;

	const float fx = static_cast<float>(x % cellSize) / static_cast<float>(cellSize);
	const float fy = static_cast<float>(y % cellSize) / static_cast<float>(cellSize);

	const float ux = fx * fx * fx * (fx * (fx * 6.0F - 15.0F) + 10.0F);
	const float uy = fy * fy * fy * (fy * (fy * 6.0F - 15.0F) + 10.0F);

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
		for (int x = 0; x < 256; x++) {
			const float wx = ValueNoiseTileable(x, y, 4, 901U) * 0.65F
			    + ValueNoiseTileable(x, y, 8, 902U) * 0.35F;
			gFogWarpLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(wx * 255.0F), 0, 255));

			const int shiftX = static_cast<int>((wx - 0.5F) * 44.0F) & 255;
			const int shiftY = static_cast<int>((ValueNoiseTileable(x, y, 4, 903U) - 0.5F) * 44.0F) & 255;
			const int sx = (x + shiftX) & 255;
			const int sy = (y + shiftY) & 255;

			const float n1 = ValueNoiseTileable(sx, sy, 4, 101U);
			const float n2 = ValueNoiseTileable(sx, sy, 8, 202U);
			const float n3 = ValueNoiseTileable(sx, sy, 16, 303U);
			const float n4 = ValueNoiseTileable(sx, sy, 32, 404U);

			const float dome1 = std::sqrt(std::max(0.0F, n1));
			const float dome2 = n2 * n2 * (3.0F - 2.0F * n2);
			const float raw = dome1 * 0.44F + dome2 * 0.30F + n3 * 0.17F + n4 * 0.09F;

			const float contrast = std::clamp((raw - 0.34F) * 1.68F, 0.0F, 1.0F);
			const float billow = contrast * (2.0F - contrast);
			gFogNoiseLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(billow * 255.0F), 0, 255));
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

void UpdateCachedFogSheets30Hz(uint32_t ticks, const LevelFogConfig &cfg)
{
	if (gLastFogSheetDtype == leveltype && (ticks - gLastFogSheetUpdateTick) < 33U)
		return;

	gLastFogSheetUpdateTick = ticks;
	gLastFogSheetDtype = leveltype;

	const int32_t sTicks = static_cast<int32_t>(ticks);
	const int driftX1 = (sTicks * 256) / cfg.driftDivX;
	const int driftY1 = (sTicks * 256) / cfg.driftDivY;

	const int driftX2 = ((driftX1 * 19) >> 4) + ((driftY1 * 7) >> 4);
	const int driftY2 = ((driftY1 * 17) >> 4) - ((driftX1 * 6) >> 4);

	const int warpDriftX = -(sTicks * 160) / cfg.driftDivX;
	const int warpDriftY = (sTicks * 160) / cfg.driftDivY;

	if (cfg.hasPulse) {
		const int phase = (sTicks / 45) & 0xFF;
		const int triWave = (phase < 128) ? phase : (255 - phase);
		gCachedPulseOffset = (triWave >> 2) - 16;
	} else {
		gCachedPulseOffset = 0;
	}

	for (int y = 0; y < 256; y++) {
		const int baseV = (y << 8);
		const int vWarp0 = (y + (warpDriftY >> 8)) & 0xFF;
		const int vWarp1 = (y + 73 + (warpDriftY >> 8)) & 0xFF;

		for (int x = 0; x < 256; x++) {
			const int baseU = (x << 8);
			const int uWarp0 = (x + (warpDriftX >> 8)) & 0xFF;
			const int uWarp1 = (x + 41 + (warpDriftX >> 8)) & 0xFF;

			const int warpX = (static_cast<int>(gFogWarpLut[vWarp0][uWarp0]) - 128) * 18;
			const int warpY = (static_cast<int>(gFogWarpLut[vWarp1][uWarp1]) - 128) * 18;

			const int u1 = baseU + warpX + driftX1;
			const int v1 = baseV + warpY + driftY1;
			gCachedFloorFog[y][x] = static_cast<uint8_t>(SampleLutBilinear(gFogNoiseLut, u1, v1));

			const int u2 = baseU - warpX + driftX2 + (137 << 8);
			const int v2 = baseV + warpY + driftY2 + (89 << 8);
			const int hCanopy = SampleLutBilinear(gFogNoiseLut, u2, v2);
			gCachedCanopyFog[y][x] = static_cast<uint8_t>(hCanopy);

			const int hCanopyNW = SampleLutBilinear(gFogNoiseLut, u2 - (4 << 8), v2 - (4 << 8));
			const int slope = hCanopy - hCanopyNW;
			if (slope > 10 && hCanopy > cfg.intensityCutoff + 28) {
				gCachedCanopyLight[y][x] = 2;
			} else if (slope < -10) {
				gCachedCanopyLight[y][x] = 0;
			} else {
				gCachedCanopyLight[y][x] = 1;
			}
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

inline bool ComputeFogPassableTile(Point tile)
{
	if (!InDungeonBounds(tile))
		return false;

	const uint16_t piece = dPiece[tile.x][tile.y];
	if (piece == 0)
		return false;

	if (!TileHasAny(piece, TileProperties::Solid))
		return true;

	const MICROS &micros = DPieceMicros[piece];
	for (int i = 2; i < MicroTileLen; i++) {
		if (micros.mt[i] != 0)
			return false;
	}
	return true;
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
	const int dHx16 = (offY * 2 - offX) / 2;
	const int dHy16 = (offY * 2 + offX) / 2;

	const int centerHx16 = (tile.x * 2 + 1) * 16 + dHx16;
	const int centerHy16 = (tile.y * 2 + 1) * 16 + dHy16;

	constexpr int Radius16 = 32;
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

			const int radialPushX = (dx * 10) / Radius16;
			const int radialPushY = (dy * 10) / Radius16;
			const int targetVx = ((pushVx + radialPushX) * smoothFalloff) >> 8;
			const int targetVy = ((pushVy + radialPushY) * smoothFalloff) >> 8;
			cell.velX = static_cast<int8_t>(std::clamp(static_cast<int>(cell.velX) + targetVx, -96, 96));
			cell.velY = static_cast<int8_t>(std::clamp(static_cast<int>(cell.velY) + targetVy, -96, 96));
		}
	}
}

inline void SampleTileFogState(
    Point tile,
    int u256,
    int v256,
    bool nW,
    bool nE,
    bool nN,
    bool nS,
    bool nNW,
    bool nNE,
    bool nSW,
    bool nSE,
    int &outDensity,
    int &outWallFade,
    int &outSwirlX,
    int &outSwirlY)
{
	const int hxFixed = (tile.x * 2) * 256 + u256 * 2;
	const int hyFixed = (tile.y * 2) * 256 + v256 * 2;
	const int hx0 = std::clamp(hxFixed >> 8, 0, HalfGridW - 2);
	const int hy0 = std::clamp(hyFixed >> 8, 0, HalfGridH - 2);
	const int fx = hxFixed & 0xFF;
	const int fy = hyFixed & 0xFF;

	const FogHalfCell &c00 = gFogHalfGrid[hx0][hy0];
	const FogHalfCell &c10 = gFogHalfGrid[hx0 + 1][hy0];
	const FogHalfCell &c01 = gFogHalfGrid[hx0][hy0 + 1];
	const FogHalfCell &c11 = gFogHalfGrid[hx0 + 1][hy0 + 1];

	const int dTop = (static_cast<int>(c00.density) << 8) + (static_cast<int>(c10.density) - static_cast<int>(c00.density)) * fx;
	const int dBot = (static_cast<int>(c01.density) << 8) + (static_cast<int>(c11.density) - static_cast<int>(c01.density)) * fx;
	outDensity = (dTop + (((dBot - dTop) * fy) >> 8)) >> 8;

	const int vxTop = (static_cast<int>(c00.velX) << 8) + (static_cast<int>(c10.velX) - static_cast<int>(c00.velX)) * fx;
	const int vxBot = (static_cast<int>(c01.velX) << 8) + (static_cast<int>(c11.velX) - static_cast<int>(c01.velX)) * fx;
	outSwirlX = (vxTop + (((vxBot - vxTop) * fy) >> 8)) >> 8;

	const int vyTop = (static_cast<int>(c00.velY) << 8) + (static_cast<int>(c10.velY) - static_cast<int>(c00.velY)) * fx;
	const int vyBot = (static_cast<int>(c01.velY) << 8) + (static_cast<int>(c11.velY) - static_cast<int>(c01.velY)) * fx;
	outSwirlY = (vyTop + (((vyBot - vyTop) * fy) >> 8)) >> 8;

	int minDist = 256;
	if (!nW)
		minDist = std::min(minDist, u256);
	if (!nE)
		minDist = std::min(minDist, 255 - u256);
	if (!nN)
		minDist = std::min(minDist, v256);
	if (!nS)
		minDist = std::min(minDist, 255 - v256);

	if (!nNW)
		minDist = std::min(minDist, std::max(u256, v256));
	if (!nNE)
		minDist = std::min(minDist, std::max(255 - u256, v256));
	if (!nSW)
		minDist = std::min(minDist, std::max(u256, 255 - v256));
	if (!nSE)
		minDist = std::min(minDist, std::max(255 - u256, 255 - v256));

	constexpr int WallMargin = 12;
	constexpr int WallFadeSpan = 124;

	if (minDist <= WallMargin) {
		outWallFade = 0;
	} else if (minDist >= WallMargin + WallFadeSpan) {
		outWallFade = 256;
	} else {
		const int t = ((minDist - WallMargin) * 256) / WallFadeSpan;
		outWallFade = (t * t * (768 - 2 * t)) >> 16;
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

inline int GetTileLightVisibility256(int tx, int ty)
{
	if (tx < 0 || tx >= MAXDUNX || ty < 0 || ty >= MAXDUNY)
		return 0;
	return LightIndexToVisibility256(dLight[tx][ty]);
}

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

constexpr size_t MaxLavaBubbles = 64;
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

inline bool IsCavesLavaTile(Point tile)
{
	if (leveltype != DTYPE_CAVES || !InDungeonBounds(tile))
		return false;
	const uint16_t piece = dPiece[tile.x][tile.y];
	if (piece == 0)
		return false;
	if ((piece >= 55 && piece <= 146) || (piece >= 153 && piece <= 160) || piece == 149 || piece == 151)
		return true;

	if (!TileHasAny(piece, TileProperties::Solid))
		return false;
	const MICROS &micros = DPieceMicros[piece];
	if (micros.mt[0] == 0 && micros.mt[1] == 0)
		return false;
	for (int i = 2; i < MicroTileLen; i++) {
		if (micros.mt[i] != 0)
			return false;
	}
	return true;
}

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

	gLavaCrustIdx = entries[3].idx;
	gLavaBodyIdx = entries[14].idx;
	gLavaRimIdx = entries[24].idx;
	gLavaSpecIdx = entries[30].idx;
}

void SyncLavaBubbleAudio()
{
	const bool isMenuPaused = (PauseMode != 0 || (!gbIsMultiplayer && gmenu_is_active()));
	if (!gbRunGame || !gbSoundOn || isMenuPaused || leveltype != DTYPE_CAVES || setlevel) {
		StopSFX(LS_LAVABUBBLES);
		return;
	}

	const Point center = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : ViewPosition;
	constexpr int MaxHearRadius = 15;
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

	const int dist256 = static_cast<int>(std::sqrt(static_cast<float>(minDistSq)) * 256.0F) / MaxHearRadius;
	const int clamped256 = std::clamp(dist256, 0, 256);
	const int lavaVolume = -150 - ((clamped256 * 1600) >> 8);
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

	if (gBubbleCountOnTile[tx][ty] >= 2)
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
	freeSlot->offsetX = static_cast<int8_t>(-10 + static_cast<int>(r2 % 21U));
	freeSlot->offsetY = static_cast<int8_t>(-4 + static_cast<int>((r2 >> 5) % 9U));
	freeSlot->snappedToLava = false;
	freeSlot->swellTicks = static_cast<uint8_t>(34 + ((r2 >> 9) % 23U));
	freeSlot->burstTicks = static_cast<uint8_t>(8 + ((r2 >> 15) % 5U));
	freeSlot->rippleTicks = static_cast<uint8_t>(8 + ((r2 >> 18) % 6U));
	freeSlot->age = randomInitialAge ? static_cast<uint8_t>(r2 % freeSlot->swellTicks) : 0;
	freeSlot->maxRadX = static_cast<uint8_t>(6 + ((r2 >> 21) % 3U));
	freeSlot->maxRadY = static_cast<uint8_t>(std::max<int>(4, (freeSlot->maxRadX * 2 + 1) / 3));
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
		for (int i = 0; i < 120; i++) {
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

	for (int attempt = 0; attempt < 12; attempt++) {
		TrySpawnCavesLavaBubble(center, false);
	}
}

void DrawCavesLavaBubblesTile(const Surface &out, Point tilePosition, Point targetBufferPosition, const bool safeLavaPx[32][64])
{
	EnsureLiveLavaBubbleShades();

	const int outW = out.w();
	const int outH = out.h();
	const int tileLeft = targetBufferPosition.x;
	const int tileTop = targetBufferPosition.y - 31;

	auto isSafeLavaAtScreen = [&](int sx, int sy) -> bool {
		const int lx = sx - tileLeft;
		const int ly = sy - tileTop;
		if (lx >= 0 && lx < 64 && ly >= 0 && ly < 32)
			return safeLavaPx[ly][lx];
		return false;
	};

	auto bubbleFootprintFits = [&](int cx, int cy, int rx, int ry) -> bool {
		return isSafeLavaAtScreen(cx, cy)
		    && isSafeLavaAtScreen(cx - rx, cy)
		    && isSafeLavaAtScreen(cx + rx, cy)
		    && isSafeLavaAtScreen(cx, cy - ry)
		    && isSafeLavaAtScreen(cx, cy + ry);
	};

	for (LavaBubble &b : gLavaBubbles) {
		if (!b.active || b.tile != tilePosition)
			continue;

		int cx = targetBufferPosition.x + 32 + b.offsetX;
		int cy = targetBufferPosition.y - 16 + b.offsetY;
		if (cx < 10 || cx >= outW - 10 || cy < 14 || cy >= outH - 10)
			continue;

		if (!b.snappedToLava) {
			if (!bubbleFootprintFits(cx, cy, b.maxRadX, b.maxRadY)) {
				static constexpr int ProbeDx[9] = { 0, -6, 6, 0, 0, -10, 10, -4, 4 };
				static constexpr int ProbeDy[9] = { 0, 0, 0, -3, 3, -2, 2, -4, 4 };
				bool foundSafe = false;
				for (int p = 0; p < 9; p++) {
					const int px = targetBufferPosition.x + 32 + ProbeDx[p];
					const int py = targetBufferPosition.y - 16 + ProbeDy[p];
					if (px >= 10 && px < outW - 10 && py >= 14 && py < outH - 10
					    && bubbleFootprintFits(px, py, b.maxRadX, b.maxRadY)) {
						b.offsetX = static_cast<int8_t>(ProbeDx[p]);
						b.offsetY = static_cast<int8_t>(ProbeDy[p]);
						cx = px;
						cy = py;
						foundSafe = true;
						break;
					}
				}
				if (!foundSafe) {
					if (InDungeonBounds(b.tile) && gBubbleCountOnTile[b.tile.x][b.tile.y] > 0) {
						gBubbleCountOnTile[b.tile.x][b.tile.y]--;
					}
					b.active = false;
					continue;
				}
			}
			b.snappedToLava = true;
		} else if (!isSafeLavaAtScreen(cx, cy)) {
			continue;
		}

		if (b.age < b.swellTicks) {
			const int p256 = (static_cast<int>(b.age) * 256) / std::max<int>(1, b.swellTicks);
			const int rx = std::max(2, (static_cast<int>(b.maxRadX) * p256 + 180) >> 8);
			const int ry = std::max(1, (static_cast<int>(b.maxRadY) * p256 + 180) >> 8);
			const int domeLift = (p256 * ry) >> 8;

			for (int dy = -ry; dy <= ry; dy++) {
				const int sy = cy + dy;
				if (sy < 0 || sy >= outH)
					continue;
				const int nyScaled = (dy * 16) / ry;
				for (int dx = -rx; dx <= rx; dx++) {
					const int sx = cx + dx;
					if (sx < 0 || sx >= outW || !isSafeLavaAtScreen(sx, sy))
						continue;
					const int nx = (dx * 16) / rx;
					const int distSq = nx * nx + nyScaled * nyScaled;
					if (distSq >= 160 && distSq <= 256) {
						out[{ sx, sy }] = gLavaCrustIdx;
					}
				}
			}

			const int domeCy = cy - domeLift;
			for (int dy = -ry; dy <= ry; dy++) {
				const int sy = domeCy + dy;
				if (sy < 0 || sy >= outH)
					continue;
				const int nyScaled = (dy * 16) / ry;
				for (int dx = -rx; dx <= rx; dx++) {
					const int sx = cx + dx;
					if (sx < 0 || sx >= outW)
						continue;
					const int nx = (dx * 16) / rx;
					const int distSq = nx * nx + nyScaled * nyScaled;
					if (distSq > 256)
						continue;
					if (!isSafeLavaAtScreen(sx, cy + std::max(0, dy)))
						continue;

					if (distSq > 190) {
						out[{ sx, sy }] = gLavaCrustIdx;
					} else if (dx <= 1 && dy <= 0 && distSq < 125) {
						out[{ sx, sy }] = gLavaRimIdx;
					} else {
						out[{ sx, sy }] = gLavaBodyIdx;
					}
				}
			}

			if (rx >= 2) {
				const int specX = std::clamp(cx - (rx / 3), 0, outW - 1);
				const int specY = std::clamp(domeCy - (ry / 3), 0, outH - 1);
				out[{ specX, specY }] = gLavaSpecIdx;
				if (rx >= 4 && specX + 1 < outW) {
					out[{ specX + 1, specY }] = gLavaSpecIdx;
				}
				if (rx >= 6 && specY + 1 < outH) {
					out[{ specX, specY + 1 }] = gLavaRimIdx;
				}
			}
		} else if (b.age < b.swellTicks + b.burstTicks) {
			const int bAge = b.age - b.swellTicks;
			const int b256 = (bAge * 256) / std::max<int>(1, b.burstTicks);
			const int rx = b.maxRadX;
			const int ry = b.maxRadY;

			for (int dy = -ry; dy <= ry; dy++) {
				const int sy = cy + dy;
				if (sy < 0 || sy >= outH)
					continue;
				const int nyScaled = (dy * 16) / ry;
				for (int dx = -rx; dx <= rx; dx++) {
					const int sx = cx + dx;
					if (sx < 0 || sx >= outW || !isSafeLavaAtScreen(sx, sy))
						continue;
					const int nx = (dx * 16) / rx;
					const int distSq = nx * nx + nyScaled * nyScaled;
					if (distSq < 110) {
						out[{ sx, sy }] = gLavaCrustIdx;
					} else if (distSq <= 256 && !(dy < -1 && std::abs(dx) <= 1)) {
						out[{ sx, sy }] = (b256 < 128) ? gLavaSpecIdx : gLavaRimIdx;
					}
				}
			}

			for (int d = 0; d < 5; d++) {
				const int dirSign = ((d & 1) != 0) ? 1 : -1;
				const int spreadX = dirSign * (2 + ((b.seed >> (d + 1)) & 5));
				const int peakLift = 6 + ((b.seed >> d) & 3);
				const int parabola256 = (b256 * (256 - b256)) >> 6;
				const int dropX = cx + ((spreadX * b256) >> 8);
				const int dropY = (cy - 1) - ((peakLift * parabola256) >> 8);
				if (dropX >= 0 && dropX < outW && dropY >= 0 && dropY < outH) {
					out[{ dropX, dropY }] = (b256 < 140) ? gLavaSpecIdx : gLavaRimIdx;
				}
			}
		} else {
			const int rAge = b.age - (b.swellTicks + b.burstTicks);
			const int r256 = (rAge * 256) / std::max<int>(1, b.rippleTicks);
			const int rx = b.maxRadX + 1 + (r256 >> 7);
			const int ry = b.maxRadY + 1 + (r256 >> 8);

			for (int dy = -ry; dy <= ry; dy++) {
				const int sy = cy + dy;
				if (sy < 0 || sy >= outH)
					continue;
				const int nyScaled = (dy * 16) / ry;
				for (int dx = -rx; dx <= rx; dx++) {
					const int sx = cx + dx;
					if (sx < 0 || sx >= outW || !isSafeLavaAtScreen(sx, sy))
						continue;
					const int nx = (dx * 16) / rx;
					const int distSq = nx * nx + nyScaled * nyScaled;
					if (distSq >= 160 && distSq <= 256) {
						out[{ sx, sy }] = (r256 < 128) ? gLavaRimIdx : gLavaCrustIdx;
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
	const LevelFogConfig cfg = GetLevelFogConfig();

	if (!cfg.enabled || cfg.spawnChancePercent == 0) {
		gLevelHasFog = false;
	} else if (cfg.spawnChancePercent >= 100) {
		gLevelHasFog = true;
	} else {
		gLevelHasFog = ((GetFloorFogSeedHash() % 100U) < cfg.spawnChancePercent);
	}

	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			gFogTilePassable[x][y] = ComputeFogPassableTile({ x, y });
		}
	}

	const uint8_t target = gLevelHasFog ? cfg.maxDensity : 0;
	for (int hx = 0; hx < HalfGridW; hx++) {
		for (int hy = 0; hy < HalfGridH; hy++) {
			gFogHalfGrid[hx][hy].density = target;
			gFogHalfGrid[hx][hy].velX = 0;
			gFogHalfGrid[hx][hy].velY = 0;
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
	if (!cfg.enabled)
		return;

	EnsureFogNoiseInitialized();

	const int maxDens = cfg.maxDensity;

	const Point center = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : ViewPosition;
	const int minHx = std::max(0, (center.x - 26) * 2);
	const int maxHx = std::min(HalfGridW - 1, (center.x + 26) * 2 + 2);
	const int minHy = std::max(0, (center.y - 26) * 2);
	const int maxHy = std::min(HalfGridH - 1, (center.y + 26) * 2 + 2);

	for (int hx = minHx; hx <= maxHx; hx++) {
		for (int hy = minHy; hy <= maxHy; hy++) {
			FogHalfCell &cell = gFogHalfGrid[hx][hy];
			if (cell.density < maxDens) {
				const int regrowStep = (cell.density < 110) ? 1 : 2;
				cell.density = static_cast<uint8_t>(std::min(maxDens, static_cast<int>(cell.density) + regrowStep));
			} else if (cell.density > maxDens) {
				cell.density = static_cast<uint8_t>(maxDens);
			}
			cell.velX = static_cast<int8_t>((cell.velX * 7) / 8);
			cell.velY = static_cast<int8_t>((cell.velY * 7) / 8);
		}
	}

	static constexpr int DirPushX[8] = { 0, -12, -16, -12, 0, 12, 16, 12 };
	static constexpr int DirPushY[8] = { 16, 12, 0, -12, -16, -12, 0, 12 };

	for (const Player &player : Players) {
		if (!player.plractive || !player.isOnActiveLevel())
			continue;
		const bool moving = player.isWalking();
		const unsigned dirIdx = static_cast<unsigned>(player._pdir) & 7U;
		const int vx = moving ? DirPushX[dirIdx] : 0;
		const int vy = moving ? DirPushY[dirIdx] : 0;
		const Displacement walkOffset = moving ? GetOffsetForWalking(player.AnimInfo, player._pdir) : Displacement {};
		CarveHalfTileEntityWake(player.position.tile, walkOffset, moving ? 120 : 48, vx, vy);
	}

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
			const unsigned dirIdx = static_cast<unsigned>(monster.direction) & 7U;
			const int vx = moving ? DirPushX[dirIdx] : 0;
			const int vy = moving ? DirPushY[dirIdx] : 0;
			const Displacement walkOffset = monster.isWalking() ? GetOffsetForWalking(monster.animInfo, monster.direction) : Displacement {};
			CarveHalfTileEntityWake(monster.position.tile, walkOffset, (moving || attacking) ? 105 : 40, vx, vy);
		}
	}

	for (const Missile &missile : Missiles) {
		if (missile._miDelFlag)
			continue;
		const int vx = std::clamp(missile.position.velocity.deltaX >> 12, -20, 20);
		const int vy = std::clamp(missile.position.velocity.deltaY >> 12, -20, 20);
		CarveHalfTileEntityWake(missile.position.tile, missile.position.offset, 115, vx, vy);
	}
}

void DrawGroundFogTileBase(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
{
	static uint32_t sLastAudioSyncTick = 0;
	const uint32_t nowTicks = SDL_GetTicks();
	if (nowTicks - sLastAudioSyncTick >= 30U) {
		sLastAudioSyncTick = nowTicks;
		SyncLavaBubbleAudio();
	}

	const int outW = out.w();
	const int outH = out.h();
	const int tx = std::clamp(tilePosition.x, 0, MAXDUNX - 1);
	const int ty = std::clamp(tilePosition.y, 0, MAXDUNY - 1);

	const bool hasBubblesOnTile = (leveltype == DTYPE_CAVES && lightIndex < LightsMax && gBubbleCountOnTile[tx][ty] > 0 && IsCavesLavaTile(tilePosition));
	bool safeLavaPx[32][64];
	if (hasBubblesOnTile) {
		bool rawLava[32][64];
		for (int dy = 0; dy < 32; dy++) {
			const int sy = targetBufferPosition.y - 31 + dy;
			for (int dx = 0; dx < 64; dx++) {
				const int sx = targetBufferPosition.x + dx;
				if (sx >= 0 && sx < outW && sy >= 0 && sy < outH) {
					const uint8_t px = out[{ sx, sy }];
					rawLava[dy][dx] = (px >= 1 && px <= 31);
				} else {
					rawLava[dy][dx] = false;
				}
			}
		}
		for (int dy = 0; dy < 32; dy++) {
			for (int dx = 0; dx < 64; dx++) {
				if (!rawLava[dy][dx]) {
					safeLavaPx[dy][dx] = false;
					continue;
				}
				const int xL = std::max(0, dx - 4);
				const int xR = std::min(63, dx + 4);
				const int yT = std::max(0, dy - 3);
				const int yB = std::min(31, dy + 3);
				safeLavaPx[dy][dx] = rawLava[dy][xL] && rawLava[dy][xR]
				    && rawLava[yT][dx] && rawLava[yB][dx]
				    && rawLava[yT][xL] && rawLava[yT][xR]
				    && rawLava[yB][xL] && rawLava[yB][xR];
			}
		}
	}

	if (*sgOptions.Graphics.groundFog && gLevelHasFog && lightIndex < LightsMax && IsFogPassableTile(tilePosition)) {
		const LevelFogConfig cfg = GetLevelFogConfig();
		if (cfg.enabled) {
			EnsureFogNoiseInitialized();
			UpdateCachedFogSheets30Hz(nowTicks, cfg);

			const bool nW = IsFogPassableTile({ tx - 1, ty });
			const bool nE = IsFogPassableTile({ tx + 1, ty });
			const bool nN = IsFogPassableTile({ tx, ty - 1 });
			const bool nS = IsFogPassableTile({ tx, ty + 1 });
			const bool nNW = IsFogPassableTile({ tx - 1, ty - 1 });
			const bool nNE = IsFogPassableTile({ tx + 1, ty - 1 });
			const bool nSW = IsFogPassableTile({ tx - 1, ty + 1 });
			const bool nSE = IsFogPassableTile({ tx + 1, ty + 1 });
			const bool allWallsClear = (nW && nE && nN && nS && nNW && nNE && nSW && nSE);

			const uint8_t *lightTbl = LightTables[std::clamp(lightIndex, 0, LightsMax - 1)].data();
			const uint8_t *shadowLightTbl = LightTables[std::clamp(lightIndex + 3, 0, LightsMax - 1)].data();
			const uint8_t litCrest = lightTbl[cfg.crestShade];
			const uint8_t litCore = lightTbl[cfg.coreShade];
			const uint8_t litTrough = cfg.isDarkSmoke ? cfg.troughShade : shadowLightTbl[cfg.troughShade];

			const bool nearLightEdge = (lightIndex >= 8);
			int cLight00 = 256;
			int cLight10 = 256;
			int cLight01 = 256;
			int cLight11 = 256;
			if (nearLightEdge) {
				const int lC = GetTileLightVisibility256(tx, ty);
				const int lW = GetTileLightVisibility256(tx - 1, ty);
				const int lE = GetTileLightVisibility256(tx + 1, ty);
				const int lN = GetTileLightVisibility256(tx, ty - 1);
				const int lS = GetTileLightVisibility256(tx, ty + 1);
				const int lNW = GetTileLightVisibility256(tx - 1, ty - 1);
				const int lNE = GetTileLightVisibility256(tx + 1, ty - 1);
				const int lSW = GetTileLightVisibility256(tx - 1, ty + 1);
				const int lSE = GetTileLightVisibility256(tx + 1, ty + 1);
				cLight00 = (lC + lW + lN + lNW) >> 2;
				cLight10 = (lC + lE + lN + lNE) >> 2;
				cLight01 = (lC + lW + lS + lSW) >> 2;
				cLight11 = (lC + lE + lS + lSE) >> 2;
			}

			const int effectiveMaxDensity = std::clamp(static_cast<int>(cfg.maxDensity) + gCachedPulseOffset, 0, 255);

			const int hxBase = tx * 2;
			const int hyBase = ty * 2;
			bool tileUndispersed = true;
			for (int ox = 0; ox <= 2 && tileUndispersed; ox++) {
				for (int oy = 0; oy <= 2; oy++) {
					const FogHalfCell &cell = gFogHalfGrid[hxBase + ox][hyBase + oy];
					if (cell.density < cfg.maxDensity - 2 || cell.velX != 0 || cell.velY != 0) {
						tileUndispersed = false;
						break;
					}
				}
			}
			const bool fastInteriorTile = allWallsClear && tileUndispersed;

			const int uvScale1 = cfg.uvScale;
			const int uvScale2 = std::max(6, (uvScale1 * 2) / 3);

			const int baseWorldU1 = tx * uvScale1;
			const int baseWorldV1 = ty * uvScale1;

			const int tileWorldPxX = (tx - ty) * 32;
			const int tileWorldPxY = (tx + ty) * 16;
			const int camWorldPxX = tileWorldPxX - (targetBufferPosition.x - (outW >> 1));
			const int camWorldPxY = tileWorldPxY - (targetBufferPosition.y - (outH >> 1));

			const int camParU = ((camWorldPxY * 2 - camWorldPxX) * uvScale2) / 140;
			const int camParV = ((camWorldPxY * 2 + camWorldPxX) * uvScale2) / 140;
			const int baseWorldU2 = tx * uvScale2 + camParU;
			const int baseWorldV2 = ty * uvScale2 + camParV;

			const int invSpan = (256 * 256) / std::max(1, 255 - static_cast<int>(cfg.intensityCutoff));

			for (int dy = 0; dy < 32; dy++) {
				const int sy = targetBufferPosition.y - 31 + dy;
				if (sy < 0 || sy >= outH)
					continue;

				const int halfSpan = (dy < 16) ? ((dy + 1) * 2) : ((32 - dy) * 2);
				const int xCenter = targetBufferPosition.x + 32;
				const int xStart = std::max(0, xCenter - halfSpan);
				const int xEnd = std::min(outW, xCenter + halfSpan);
				if (xStart >= xEnd)
					continue;

				const int localY = dy - 16;
				const int bayerRowShift = (sy & 1) << 1;

				for (int sx = xStart; sx < xEnd; sx++) {
					const int localX = sx - xCenter;
					const int u256 = std::clamp((localX * 4) + (localY * 8) + 128, 0, 255);
					const int v256 = std::clamp((localY * 8) - (localX * 4) + 128, 0, 255);

					int entityDensity = effectiveMaxDensity;
					int wallFade = 256;
					int swirlX = 0;
					int swirlY = 0;

					if (!fastInteriorTile) {
						SampleTileFogState(tilePosition, u256, v256, nW, nE, nN, nS, nNW, nNE, nSW, nSE, entityDensity, wallFade, swirlX, swirlY);
						if (wallFade <= 0 || entityDensity < 16)
							continue;
						if (entityDensity > effectiveMaxDensity)
							entityDensity = effectiveMaxDensity;
					}

					if (nearLightEdge) {
						const int lTop = (cLight00 << 8) + (cLight10 - cLight00) * u256;
						const int lBot = (cLight01 << 8) + (cLight11 - cLight01) * u256;
						const int lightFade = (lTop + (((lBot - lTop) * v256) >> 8)) >> 8;
						wallFade = (wallFade * lightFade) >> 8;
						if (wallFade <= 0)
							continue;
					}

					const int u1Idx = (baseWorldU1 + ((u256 * uvScale1 + swirlX * 4) >> 8)) & 0xFF;
					const int v1Idx = (baseWorldV1 + ((v256 * uvScale1 + swirlY * 4) >> 8)) & 0xFF;
					const int hFloor = gCachedFloorFog[v1Idx][u1Idx];

					const int u2Idx = (baseWorldU2 + ((u256 * uvScale2 + swirlX * 2) >> 8)) & 0xFF;
					const int v2Idx = (baseWorldV2 + ((v256 * uvScale2 + swirlY * 2) >> 8)) & 0xFF;
					const int hCanopy = gCachedCanopyFog[v2Idx][u2Idx];
					const uint8_t canopyLight = gCachedCanopyLight[v2Idx][u2Idx];

					const int canopyEntityDens = entityDensity + (((effectiveMaxDensity - entityDensity) * 3) >> 3);
					const int floorIntensity = (hFloor * entityDensity) >> 8;
					const int canopyIntensity = (hCanopy * canopyEntityDens) >> 8;

					const int cloudIntensity = std::max(floorIntensity, canopyIntensity);
					if (cloudIntensity < cfg.intensityCutoff)
						continue;

					uint8_t shade = litCore;
					if (canopyIntensity >= floorIntensity - 6) {
						if (canopyLight == 2) {
							shade = litCrest;
						} else if (canopyLight == 0) {
							shade = litTrough;
						} else {
							shade = litCore;
						}
					} else {
						const int hCanopyOverhead = gCachedCanopyFog[(v2Idx - 2) & 0xFF][(u2Idx - 2) & 0xFF];
						shade = (hCanopyOverhead > cfg.intensityCutoff + 24) ? litTrough : litCore;
					}

					const int bayer = (((sx & 1) + bayerRowShift) * 4) - 6;
					const int normCloud = ((cloudIntensity - cfg.intensityCutoff) * invSpan) >> 8;
					const int alpha = ((normCloud * wallFade) >> 8) + bayer;
					if (alpha < 14)
						continue;

					uint8_t &dst = out[{ sx, sy }];
					if (alpha > 205) {
						dst = cfg.allowHeavyBlend ? Blend37(shade, dst) : Blend31(shade, dst);
					} else if (alpha > 155) {
						dst = cfg.allowHeavyBlend ? Blend31(shade, dst) : Blend25(shade, dst);
					} else if (alpha > 110) {
						dst = Blend25(shade, dst);
					} else if (alpha > 68) {
						dst = Blend19(shade, dst);
					} else if (alpha > 34) {
						dst = Blend12(shade, dst);
					} else {
						dst = Blend6(shade, dst);
					}
				}
			}
		}
	}

	if (hasBubblesOnTile) {
		DrawCavesLavaBubblesTile(out, tilePosition, targetBufferPosition, safeLavaPx);
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

	const int lightVis = LightIndexToVisibility256(lightIndex);
	if (lightVis <= 0)
		return;

	const int collarDensity = (((static_cast<int>(cfg.maxDensity) * 11) / 20) * lightVis) >> 8;
	if (collarDensity < 32)
		return;

	const uint8_t *lightTbl = LightTables[std::clamp(lightIndex, 0, LightsMax - 1)].data();
	const uint8_t litCrest = lightTbl[cfg.crestShade];
	const uint8_t litCore = lightTbl[cfg.coreShade];

	const int outW = out.w();
	const int outH = out.h();

	const int centerX = targetBufferPosition.x + 32;
	const int centerY = targetBufferPosition.y - 10;
	constexpr int RadiusX = 14;
	constexpr int RadiusY = 8;

	const int baseU = tilePosition.x * cfg.uvScale;
	const int baseV = tilePosition.y * cfg.uvScale;

	for (int dy = -RadiusY; dy <= RadiusY; dy++) {
		const int sy = centerY + dy;
		if (sy < 0 || sy >= outH)
			continue;

		const int normY256 = (dy * 256) / RadiusY;
		const int ySq = (normY256 * normY256) >> 8;
		const int vIdx = (baseV + dy * 2) & 0xFF;

		for (int dx = -RadiusX; dx <= RadiusX; dx++) {
			const int sx = centerX + dx;
			if (sx < 0 || sx >= outW)
				continue;

			const int normX256 = (dx * 256) / RadiusX;
			const int distSq = ySq + ((normX256 * normX256) >> 8);
			if (distSq >= 256)
				continue;

			const int radialFalloff = 256 - distSq;
			const int uIdx = (baseU + dx) & 0xFF;
			const int noise = gCachedCanopyFog[vIdx][uIdx];

			const int wispIntensity = (((noise * collarDensity) >> 8) * radialFalloff) >> 8;
			if (wispIntensity < 76)
				continue;

			uint8_t &dst = out[{ sx, sy }];
			if (wispIntensity > 155) {
				dst = Blend19(litCrest, dst);
			} else if (wispIntensity > 110) {
				dst = Blend12(litCore, dst);
			} else {
				dst = Blend6(litCore, dst);
			}
		}
	}
}

} // namespace devilution
