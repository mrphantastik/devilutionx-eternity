/**
 * @file ground_fog.cpp
 *
 * Implementation of 3D Isometric Volumetric Ground Fog & Interactive Entity Dispersion.
 */
#include "engine/render/ground_fog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <SDL.h>

#include "diablo.h"
#include "engine/palette.h"
#include "levels/gendung.h"
#include "lighting.h"
#include "missiles.h"
#include "monster.h"
#include "options.h"
#include "player.h"

namespace devilution {

namespace {

struct FogCell {
	uint8_t density; // 0 = completely dispersed, up to LevelFogConfig::maxDensity
	int8_t velX;     // Swirl/push displacement X (-127..127)
	int8_t velY;     // Swirl/push displacement Y (-127..127)
};

struct LevelFogConfig {
	bool enabled;
	uint8_t maxDensity;    // Target ambient density (0..255): e.g. 150 = low/med, 250 = thick
	uint8_t coreShade;     // Palette index for dense core puffs
	uint8_t edgeShade;     // Palette index for outer wisps
	bool isDarkSmoke;      // True in Hell: darkens/soots pixels instead of brightening
	bool allowHeavyBlend;  // True in Crypt/Hell: allows 50% LUT blend on thickest cores
	int driftDivX;         // Horizontal drift divisor (lower = faster)
	int driftDivY;         // Vertical drift divisor (lower = faster; negative = upward thermal draft)
};

bool gNoiseInitialized = false;
// Precomputed 256x256 seamless 3-octave FBM turbulence LUT (0..255)
std::array<std::array<uint8_t, 256>, 256> gFogNoiseLut;

// Per-tile interactive dispersion & swirl state
std::array<std::array<FogCell, MAXDUNY>, MAXDUNX> gFogGrid;

/**
 * @brief Easy-to-tune per-dungeon fog/smoke appearance table.
 */
LevelFogConfig GetLevelFogConfig()
{
	switch (leveltype) {
	case DTYPE_CATHEDRAL:
		// Low/medium density cold spectral mist
		return {
			/*enabled=*/true,
			/*maxDensity=*/185,
			/*coreShade=*/PAL16_GRAY + 3,
			/*edgeShade=*/PAL16_GRAY + 6,
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*driftDivX=*/65,
			/*driftDivY=*/95
		};
	case DTYPE_CATACOMBS:
		// Medium density dusty bone-beige / eerie gray mist
		return {
			/*enabled=*/true,
			/*maxDensity=*/200,
			/*coreShade=*/PAL16_BEIGE + 3,
			/*edgeShade=*/PAL16_GRAY + 6,
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*driftDivX=*/60,
			/*driftDivY=*/90
		};
	case DTYPE_CAVES:
		// Medium sulfurous steam
		return {
			/*enabled=*/true,
			/*maxDensity=*/205,
			/*coreShade=*/PAL16_GRAY + 3,
			/*edgeShade=*/PAL16_GRAY + 6,
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*driftDivX=*/45,
			/*driftDivY=*/-60
		};
	case DTYPE_HELL:
		// Thick black-ish / charcoal rising smoke
		return {
			/*enabled=*/true,
			/*maxDensity=*/235,
			/*coreShade=*/PAL16_GRAY + 14, // Very dark charcoal-black shade
			/*edgeShade=*/PAL16_GRAY + 12, // Dark soot gray shade
			/*isDarkSmoke=*/true,          // Uses raw dark soot shades for black smoke look
			/*allowHeavyBlend=*/true,
			/*driftDivX=*/35,
			/*driftDivY=*/-40              // Negative Y = rising thermal smoke draft
		};
	case DTYPE_CRYPT:
		// High density, thick ancient tomb fog
		return {
			/*enabled=*/true,
			/*maxDensity=*/255,
			/*coreShade=*/PAL16_GRAY + 2,
			/*edgeShade=*/PAL16_GRAY + 5,
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/true,      // Allows 50% opacity core banks in the Crypt
			/*driftDivX=*/75,
			/*driftDivY=*/110
		};
	case DTYPE_NEST:
		// Humid spore haze
		return {
			/*enabled=*/true,
			/*maxDensity=*/195,
			/*coreShade=*/PAL16_YELLOW + 5,
			/*edgeShade=*/PAL16_GRAY + 6,
			/*isDarkSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*driftDivX=*/55,
			/*driftDivY=*/80
		};
	default:
		return { false, 0, 0, 0, false, false, 60, 90 };
	}
}

inline uint32_t FastHash2D(int x, int y)
{
	uint32_t h = static_cast<uint32_t>(x & 0xFF) * 374761393U + static_cast<uint32_t>(y & 0xFF) * 668265263U;
	h = (h ^ (h >> 13)) * 1274126177U;
	return (h ^ (h >> 16)) & 0xFFU;
}

float SmoothValueNoise(float x, float y, int period)
{
	const int xi = static_cast<int>(std::floor(x));
	const int yi = static_cast<int>(std::floor(y));
	const float xf = x - static_cast<float>(xi);
	const float yf = y - static_cast<float>(yi);

	const float u = xf * xf * (3.0F - 2.0F * xf);
	const float v = yf * yf * (3.0F - 2.0F * yf);

	const int x0 = ((xi % period) + period) % period;
	const int y0 = ((yi % period) + period) % period;
	const int x1 = (x0 + 1) % period;
	const int y1 = (y0 + 1) % period;

	const float n00 = static_cast<float>(FastHash2D(x0, y0));
	const float n10 = static_cast<float>(FastHash2D(x1, y0));
	const float n01 = static_cast<float>(FastHash2D(x0, y1));
	const float n11 = static_cast<float>(FastHash2D(x1, y1));

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
			const float n1 = SmoothValueNoise(x / 32.0F, y / 32.0F, 8);
			const float n2 = SmoothValueNoise(x / 16.0F, y / 16.0F, 16);
			const float n3 = SmoothValueNoise(x / 8.0F, y / 8.0F, 32);
			const float v = n1 * 0.55F + n2 * 0.30F + n3 * 0.15F;
			raw[y][x] = v;
			minV = std::min(minV, v);
			maxV = std::max(maxV, v);
		}
	}

	const float span = std::max(1.0F, maxV - minV);
	for (int y = 0; y < 256; y++) {
		for (int x = 0; x < 256; x++) {
			// Stretch FBM dynamic range to full 0..255 so billowing cloud crests reach 220..255
			// while troughs drop to 0..60 for distinct, volumetric swirling mist banks.
			const float norm = (raw[y][x] - minV) / span;
			const int stretched = static_cast<int>(norm * 255.0F + 0.5F);
			gFogNoiseLut[y][x] = static_cast<uint8_t>(std::clamp(stretched, 0, 255));
		}
	}
	gNoiseInitialized = true;
}

inline bool IsFogWalkableTile(Point tile)
{
	if (!InDungeonBounds(tile))
		return false;
	const uint16_t piece = dPiece[tile.x][tile.y];
	if (piece == 0)
		return false;
	return !TileHasAny(piece, TileProperties::Solid);
}

inline uint8_t Blend50(uint8_t fg, uint8_t bg)
{
	return paletteTransparencyLookup[fg][bg];
}

inline uint8_t Blend37(uint8_t fg, uint8_t bg)
{
	const uint8_t half = paletteTransparencyLookup[fg][bg];
	const uint8_t qtr = paletteTransparencyLookup[bg][half];
	return paletteTransparencyLookup[half][qtr]; // 37.5% fg, 62.5% bg
}

inline uint8_t Blend25(uint8_t fg, uint8_t bg)
{
	const uint8_t mid = paletteTransparencyLookup[fg][bg];
	return paletteTransparencyLookup[bg][mid];
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
	return paletteTransparencyLookup[bg][qtr];
}

inline uint8_t Blend6(uint8_t fg, uint8_t bg)
{
	const uint8_t eighth = Blend12(fg, bg);
	return paletteTransparencyLookup[bg][eighth];
}

void CarveEntityWake(Point tile, Direction dir, bool isMoving, int strength, int radius)
{
	if (!InDungeonBounds(tile))
		return;

	static constexpr int DirPushX[8] = { 0, -18, -24, -18, 0, 18, 24, 18 };
	static constexpr int DirPushY[8] = { 24, 18, 0, -18, -24, -18, 0, 18 };
	const unsigned dirIdx = static_cast<unsigned>(dir) & 7U;
	const int pushX = isMoving ? DirPushX[dirIdx] : 0;
	const int pushY = isMoving ? DirPushY[dirIdx] : 0;

	for (int dy = -radius; dy <= radius; dy++) {
		for (int dx = -radius; dx <= radius; dx++) {
			const int tx = tile.x + dx;
			const int ty = tile.y + dy;
			if (!InDungeonBounds({ tx, ty }))
				continue;

			const int distSq = dx * dx + dy * dy;
			if (distSq > radius * radius)
				continue;

			FogCell &cell = gFogGrid[tx][ty];
			const int falloff = (radius * radius + 1 - distSq);
			const int drop = (strength * falloff) / (radius * radius + 1);
			cell.density = static_cast<uint8_t>(std::max(0, static_cast<int>(cell.density) - drop));

			if (isMoving) {
				const int swirlX = pushX + dx * 10;
				const int swirlY = pushY + dy * 10;
				cell.velX = static_cast<int8_t>(std::clamp(static_cast<int>(cell.velX) + swirlX / 2, -120, 120));
				cell.velY = static_cast<int8_t>(std::clamp(static_cast<int>(cell.velY) + swirlY / 2, -120, 120));
			}
		}
	}
}

inline void SampleTileFogState(Point tile, int u256, int v256, int &outDensity, int &outSwirlX, int &outSwirlY)
{
	const int tx0 = std::clamp(tile.x, 0, MAXDUNX - 2);
	const int ty0 = std::clamp(tile.y, 0, MAXDUNY - 2);

	const FogCell &c00 = gFogGrid[tx0][ty0];
	const FogCell &c10 = gFogGrid[tx0 + 1][ty0];
	const FogCell &c01 = gFogGrid[tx0][ty0 + 1];
	const FogCell &c11 = gFogGrid[tx0 + 1][ty0 + 1];

	const int invU = 256 - u256;
	const int invV = 256 - v256;

	const int w00 = (invU * invV) >> 8;
	const int w10 = (u256 * invV) >> 8;
	const int w01 = (invU * v256) >> 8;
	const int w11 = (u256 * v256) >> 8;

	outDensity = (c00.density * w00 + c10.density * w10 + c01.density * w01 + c11.density * w11) >> 8;
	outSwirlX = (c00.velX * w00 + c10.velX * w10 + c01.velX * w01 + c11.velX * w11) >> 8;
	outSwirlY = (c00.velY * w00 + c10.velY * w10 + c01.velY * w01 + c11.velY * w11) >> 8;
}

} // namespace

void ResetGroundFogLevel()
{
	EnsureFogNoiseInitialized();
	const LevelFogConfig cfg = GetLevelFogConfig();
	const uint8_t target = cfg.enabled ? cfg.maxDensity : 0;
	const uint8_t wallFeather = static_cast<uint8_t>(target / 4);

	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			const bool walkable = IsFogWalkableTile({ x, y });
			gFogGrid[x][y].density = walkable ? target : wallFeather;
			gFogGrid[x][y].velX = 0;
			gFogGrid[x][y].velY = 0;
		}
	}
}

void UpdateGroundFogSimulation()
{
	if (!*sgOptions.Graphics.groundFog)
		return;

	const LevelFogConfig cfg = GetLevelFogConfig();
	if (!cfg.enabled)
		return;

	EnsureFogNoiseInitialized();

	const int maxDens = cfg.maxDensity;
	const uint8_t wallFeather = static_cast<uint8_t>(maxDens / 4);

	// 1. Relax velocity and regrow density in a window around the player
	const Point center = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : ViewPosition;
	const int minX = std::max(1, center.x - 28);
	const int maxX = std::min(MAXDUNX - 2, center.x + 28);
	const int minY = std::max(1, center.y - 28);
	const int maxY = std::min(MAXDUNY - 2, center.y + 28);

	for (int x = minX; x <= maxX; x++) {
		for (int y = minY; y <= maxY; y++) {
			FogCell &cell = gFogGrid[x][y];
			if (!IsFogWalkableTile({ x, y })) {
				cell.density = wallFeather;
				cell.velX = 0;
				cell.velY = 0;
				continue;
			}

			if (cell.density < maxDens) {
				const int neighborAvg = (gFogGrid[x - 1][y].density + gFogGrid[x + 1][y].density
				                            + gFogGrid[x][y - 1].density + gFogGrid[x][y + 1].density)
				    >> 2;
				const int nextVal = std::max(static_cast<int>(cell.density) + 2, (cell.density * 3 + neighborAvg) >> 2);
				cell.density = static_cast<uint8_t>(std::min(maxDens, nextVal + 2));
			} else if (cell.density > maxDens) {
				cell.density = static_cast<uint8_t>(maxDens);
			}

			cell.velX = static_cast<int8_t>((cell.velX * 7) / 8);
			cell.velY = static_cast<int8_t>((cell.velY * 7) / 8);
		}
	}

	// 2. Carve wakes for active Players
	for (const Player &player : Players) {
		if (!player.plractive || !player.isOnActiveLevel())
			continue;
		const bool moving = player.isWalking();
		CarveEntityWake(player.position.tile, player._pdir, moving, moving ? 115 : 24, moving ? 2 : 1);
		if (moving) {
			CarveEntityWake(player.position.future, player._pdir, true, 85, 1);
		}
	}

	// 3. Carve wakes for active Monsters
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
			if (moving || attacking) {
				CarveEntityWake(monster.position.tile, monster.direction, moving, moving ? 105 : 65, 2);
			} else {
				CarveEntityWake(monster.position.tile, monster.direction, false, 16, 1);
			}
		}
	}

	// 4. Blast shockwave holes in fog from active Missiles / Explosions
	for (const Missile &missile : Missiles) {
		if (missile._miDelFlag)
			continue;
		CarveEntityWake(missile.position.tile, Direction::South, true, 95, 2);
	}
}

void DrawGroundFogTileBase(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
{
	if (!*sgOptions.Graphics.groundFog)
		return;
	if (lightIndex >= LightsMax || !IsFogWalkableTile(tilePosition))
		return;

	const LevelFogConfig cfg = GetLevelFogConfig();
	if (!cfg.enabled)
		return;

	EnsureFogNoiseInitialized();

	// For dark smoke (Hell), keep the dark charcoal/soot palette shade directly;
	// for normal mist, attenuate shade through vanilla LightTables[lightIndex].
	const uint8_t *lightTbl = LightTables[std::clamp(lightIndex, 0, LightsMax - 1)].data();
	const uint8_t litCore = cfg.isDarkSmoke ? cfg.coreShade : lightTbl[cfg.coreShade];
	const uint8_t litEdge = cfg.isDarkSmoke ? cfg.edgeShade : lightTbl[cfg.edgeShade];

	const int32_t ticks = static_cast<int32_t>(SDL_GetTicks());
	const int driftX1 = ticks / cfg.driftDivX;
	const int driftY1 = ticks / cfg.driftDivY;
	const int driftX2 = -ticks / ((cfg.driftDivX * 3) / 4);
	const int driftY2 = ticks / ((cfg.driftDivY * 3) / 4);

	const int baseWorldU = tilePosition.x * 32;
	const int baseWorldV = tilePosition.y * 32;

	const int outW = out.w();
	const int outH = out.h();

	for (int dy = 0; dy < 32; dy++) {
		const int sy = targetBufferPosition.y - 31 + dy;
		if (sy < 0 || sy >= outH)
			continue;

		const int halfSpan = (dy < 16) ? ((dy + 1) * 2) : ((32 - dy) * 2);
		const int xStart = targetBufferPosition.x + 32 - halfSpan;
		const int xEnd = targetBufferPosition.x + 32 + halfSpan;

		for (int sx = xStart; sx < xEnd; sx++) {
			if (sx < 0 || sx >= outW)
				continue;

			const int localX = sx - (targetBufferPosition.x + 32);
			const int localY = dy - 16;
			const int u256 = std::clamp((localX * 4) + (localY * 8) + 128, 0, 255);
			const int v256 = std::clamp((localY * 8) - (localX * 4) + 128, 0, 255);

			int cellDensity = cfg.maxDensity;
			int swirlX = 0;
			int swirlY = 0;
			SampleTileFogState(tilePosition, u256, v256, cellDensity, swirlX, swirlY);
			if (cellDensity < 20)
				continue;

			const int wu1 = (baseWorldU + (u256 >> 3) + driftX1 + (swirlX >> 2)) & 0xFF;
			const int wv1 = (baseWorldV + (v256 >> 3) + driftY1 + (swirlY >> 2)) & 0xFF;
			const int wu2 = (baseWorldU + ((u256 + 48) >> 3) + driftX2 + (swirlX >> 1)) & 0xFF;
			const int wv2 = (baseWorldV + ((v256 - 64) >> 3) + driftY2 + (swirlY >> 1)) & 0xFF;

			const int n1 = gFogNoiseLut[wv1][wu1];
			const int n2 = gFogNoiseLut[wv2][wu2];
			const int combinedNoise = (n1 * 3 + n2 * 2) / 5;

			const int finalIntensity = (combinedNoise * cellDensity) >> 8;
			if (finalIntensity < 75)
				continue;

			uint8_t &dst = out[{ sx, sy }];
			if (cfg.allowHeavyBlend && finalIntensity > 165) {
				dst = Blend50(litCore, dst);
			} else if (finalIntensity > 140) {
				dst = cfg.allowHeavyBlend ? Blend37(litCore, dst) : Blend37(litCore, dst);
			} else if (finalIntensity > 105) {
				dst = Blend25(litCore, dst);
			} else {
				dst = Blend19(litEdge, dst);
			}
		}
	}
}

void DrawGroundFogEntitySubmersion(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
{
	if (!*sgOptions.Graphics.groundFog)
		return;
	if (lightIndex >= LightsMax || !IsFogWalkableTile(tilePosition))
		return;

	const LevelFogConfig cfg = GetLevelFogConfig();
	if (!cfg.enabled)
		return;

	EnsureFogNoiseInitialized();

	const int minCollar = static_cast<int>(cfg.maxDensity) / 2;
	const int centerDensity = std::max(minCollar, static_cast<int>(gFogGrid[tilePosition.x][tilePosition.y].density));

	const uint8_t *lightTbl = LightTables[std::clamp(lightIndex, 0, LightsMax - 1)].data();
	const uint8_t litCore = cfg.isDarkSmoke ? cfg.coreShade : lightTbl[cfg.coreShade];
	const uint8_t litEdge = cfg.isDarkSmoke ? cfg.edgeShade : lightTbl[cfg.edgeShade];

	const int32_t ticks = static_cast<int32_t>(SDL_GetTicks());
	const int driftX = ticks / cfg.driftDivX;
	const int driftY = ticks / cfg.driftDivY;

	const int outW = out.w();
	const int outH = out.h();

	const int footBaseY = targetBufferPosition.y - 6;
	constexpr int SubmergeHeight = 20;

	for (int h = 0; h < SubmergeHeight; h++) {
		const int sy = footBaseY - h;
		if (sy < 0 || sy >= outH)
			continue;

		const int heightAtten = 256 - (h * 256) / SubmergeHeight;

		const int xStart = std::max(0, targetBufferPosition.x + 8);
		const int xEnd = std::min(outW, targetBufferPosition.x + 56);

		for (int sx = xStart; sx < xEnd; sx++) {
			const int wu = (tilePosition.x * 32 + (sx - targetBufferPosition.x) + driftX) & 0xFF;
			const int wv = (tilePosition.y * 32 + h * 2 + driftY) & 0xFF;
			const int noise = gFogNoiseLut[wv][wu];

			const int wispIntensity = (((noise * centerDensity) >> 8) * heightAtten) >> 8;
			if (wispIntensity < 75)
				continue;

			uint8_t &dst = out[{ sx, sy }];
			if (cfg.allowHeavyBlend && wispIntensity > 155) {
				dst = Blend37(litCore, dst);
			} else if (wispIntensity > 115) {
				dst = Blend25(litCore, dst);
			} else {
				dst = Blend19(litEdge, dst);
			}
		}
	}
}

} // namespace devilution
