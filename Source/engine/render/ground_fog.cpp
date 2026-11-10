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
	uint8_t spawnChancePercent; // Chance (0..100%) for this dungeon floor to spawn fog (100% for testing)
	uint8_t maxDensity;         // Target ambient density (0..255)
	uint8_t crestShade;         // Brighter palette index for lit upper billow ridges
	uint8_t coreShade;          // Mid-tone palette index for main cloud body
	uint8_t troughShade;        // Darker palette index for lower shadow pockets/undersides
	bool isInfernalSmoke;       // True in Hell: uses ember-lit ash crests & visible smoke contrast on dark floors
	bool allowHeavyBlend;       // True in Crypt/Hell: allows 50% LUT blend on thickest billow crests
	int driftDivX;              // Horizontal drift divisor (lower = faster)
	int driftDivY;              // Vertical drift divisor (lower = faster; negative = upward thermal draft)
};

bool gNoiseInitialized = false;
bool gLevelHasFog = false;

// Precomputed 256x256 seamless multi-octave billow FBM LUT (0..255) and domain-warp LUT (0..255)
std::array<std::array<uint8_t, 256>, 256> gFogNoiseLut;
std::array<std::array<uint8_t, 256>, 256> gFogWarpLut;

// Per-tile interactive dispersion state (single-tile entity displacement)
std::array<std::array<FogCell, MAXDUNY>, MAXDUNX> gFogGrid;

// Per-grid-vertex (corner intersection of 4 tiles) wall-distance fade weight (0..256).
// Ensures fog smoothly fades to 0% opacity before touching walls in all 8 directions (N, S, E, W, NE, NW, SE, SW).
std::array<std::array<uint16_t, MAXDUNY + 1>, MAXDUNX + 1> gFogCornerWeight;

/**
 * @brief Distinct per-dungeon volumetric fog/smoke/steam/spore presets.
 * All dungeon spawn chances are currently set to 100% for testing.
 */
LevelFogConfig GetLevelFogConfig()
{
	switch (leveltype) {
	case DTYPE_CATHEDRAL:
		// Cold spectral silver-blue mist with wide open stone floor gaps
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/195,
			/*crestShade=*/PAL16_BLUE + 2,
			/*coreShade=*/PAL16_GRAY + 5,
			/*troughShade=*/PAL16_GRAY + 10,
			/*isInfernalSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*driftDivX=*/65,
			/*driftDivY=*/95
		};
	case DTYPE_CATACOMBS:
		// Swirling dusty bone-beige & sepulchral gray crypt haze
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/210,
			/*crestShade=*/PAL16_BEIGE + 2,
			/*coreShade=*/PAL16_BEIGE + 6,
			/*troughShade=*/PAL16_GRAY + 10,
			/*isInfernalSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*driftDivX=*/55,
			/*driftDivY=*/85
		};
	case DTYPE_CAVES:
		// Upward-rising geothermal volcanic steam (also rises over lava rivers)
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/220,
			/*crestShade=*/PAL16_YELLOW + 2,
			/*coreShade=*/PAL16_BEIGE + 4,
			/*troughShade=*/PAL16_GRAY + 8,
			/*isInfernalSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*driftDivX=*/42,
			/*driftDivY=*/-48 // Negative Y = rising thermal steam draft
		};
	case DTYPE_HELL:
		// Thick rising infernal ash-smoke with ember-tinted upper ridges & dark soot troughs
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/235,
			/*crestShade=*/PAL16_ORANGE + 5, // Warm ember-ash highlight on billow crests
			/*coreShade=*/PAL16_GRAY + 6,    // Visible medium ash-smoke body against dark charcoal floors
			/*troughShade=*/PAL16_GRAY + 12, // Deep soot shadow in cloud troughs
			/*isInfernalSmoke=*/true,
			/*allowHeavyBlend=*/true,
			/*driftDivX=*/32,
			/*driftDivY=*/-36 // Rapid upward thermal smoke draft
		};
	case DTYPE_CRYPT:
		// Heavy, dense multi-layered ectoplasmic tomb fog
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/245,
			/*crestShade=*/PAL16_GRAY + 2,
			/*coreShade=*/PAL16_GRAY + 5,
			/*troughShade=*/PAL16_GRAY + 10,
			/*isInfernalSmoke=*/false,
			/*allowHeavyBlend=*/true,
			/*driftDivX=*/75,
			/*driftDivY=*/110
		};
	case DTYPE_NEST:
		// Humid, pulsing toxic yellowish-green bio-spore miasma
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/215,
			/*crestShade=*/PAL16_YELLOW + 3,
			/*coreShade=*/PAL16_YELLOW + 7,
			/*troughShade=*/PAL16_GRAY + 9,
			/*isInfernalSmoke=*/false,
			/*allowHeavyBlend=*/false,
			/*driftDivX=*/50,
			/*driftDivY=*/-70
		};
	default:
		return { false, 0, 0, 0, 0, 0, false, false, 60, 90 };
	}
}

inline uint32_t FastHash2D(int x, int y)
{
	uint32_t h = static_cast<uint32_t>(x & 0xFF) * 374761393U + static_cast<uint32_t>(y & 0xFF) * 668265263U;
	h = (h ^ (h >> 13)) * 1274126177U;
	return (h ^ (h >> 16)) & 0xFFU;
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

float SmoothValueNoise(float x, float y, int period, int seedOffset = 0)
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
			// Primary billow FBM: combines broad cloud banks (period 6), medium puffs (period 12),
			// and fine curling tendrils (period 24) with ridge/billow shaping for 3D depth.
			const float n1 = SmoothValueNoise(x / 42.6667F, y / 42.6667F, 6, 0);
			const float n2 = SmoothValueNoise(x / 21.3333F, y / 21.3333F, 12, 37);
			const float n3 = SmoothValueNoise(x / 10.6667F, y / 10.6667F, 24, 91);

			// Billow abs-ridge term gives rounded volumetric cloud tops instead of flat noise
			const float ridge = 255.0F - std::fabs(n2 - 128.0F) * 1.65F;
			const float v = n1 * 0.48F + ridge * 0.34F + n3 * 0.18F;
			raw[y][x] = v;
			minV = std::min(minV, v);
			maxV = std::max(maxV, v);

			// Secondary domain-warp LUT for organic curling motion
			const float w1 = SmoothValueNoise(x / 32.0F, y / 32.0F, 8, 151);
			const float w2 = SmoothValueNoise(x / 16.0F, y / 16.0F, 16, 211);
			gFogWarpLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(w1 * 0.65F + w2 * 0.35F), 0, 255));
		}
	}

	const float span = std::max(1.0F, maxV - minV);
	for (int y = 0; y < 256; y++) {
		for (int x = 0; x < 256; x++) {
			float norm = std::clamp((raw[y][x] - minV) / span, 0.0F, 1.0F);
			// Apply contrast S-curve so low-density regions drop away cleanly (revealing the stone floor)
			// while cloud banks swell into rounded 3D volumes with distinct crests and troughs.
			norm = norm * norm * (3.0F - 2.0F * norm);
			const int stretched = static_cast<int>(norm * 255.0F + 0.5F);
			gFogNoiseLut[y][x] = static_cast<uint8_t>(std::clamp(stretched, 0, 255));
		}
	}
	gNoiseInitialized = true;
}

/**
 * @brief Returns true if a tile can hold ground fog/steam/smoke.
 * Walkable floor tiles always qualify. In Caves, Hell, and Nest, flat liquid/lava/acid pool
 * tiles (which have Solid set to block walking, but have no tall wall microtiles in mt[2..15])
 * also qualify so volcanic steam and infernal smoke naturally roll across lava pools!
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

	if (leveltype == DTYPE_CAVES || leveltype == DTYPE_HELL || leveltype == DTYPE_NEST) {
		const MICROS &micros = DPieceMicros[piece];
		if (micros.mt[0] == 0 && micros.mt[1] == 0)
			return false;
		for (int i = 2; i < MicroTileLen; i++) {
			if (micros.mt[i] != 0)
				return false; // Has tall wall/pillar/rock structure
		}
		return true; // Flat lava / blood-lava / acid surface
	}
	return false;
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

/**
 * @brief Displaces fog strictly on the exact single tile (tile.x, tile.y) occupied by
 * an entity or missile — no multi-tile radial aura or surrounding room falloff.
 */
void CarveSingleTileEntityWake(Point tile, Direction dir, bool isMoving, int dropStrength)
{
	if (!InDungeonBounds(tile))
		return;

	static constexpr int DirPushX[8] = { 0, -20, -28, -20, 0, 20, 28, 20 };
	static constexpr int DirPushY[8] = { 28, 20, 0, -20, -28, -20, 0, 20 };
	const unsigned dirIdx = static_cast<unsigned>(dir) & 7U;

	FogCell &cell = gFogGrid[tile.x][tile.y];
	cell.density = static_cast<uint8_t>(std::max(0, static_cast<int>(cell.density) - dropStrength));

	if (isMoving) {
		cell.velX = static_cast<int8_t>(std::clamp(static_cast<int>(cell.velX) + DirPushX[dirIdx], -120, 120));
		cell.velY = static_cast<int8_t>(std::clamp(static_cast<int>(cell.velY) + DirPushY[dirIdx], -120, 120));
	}
}

/**
 * @brief Computes the bilinearly interpolated wall-fade weight (0..256) and single-tile
 * entity dispersion density at sub-tile coordinates (u256, v256) in [0..256] x [0..256].
 *
 * Because gFogCornerWeight[vx][vy] is 0 whenever ANY of the 4 tiles meeting at corner
 * (vx, vy) is a solid wall, fog smoothly fades to 0% along every wall boundary in all
 * 8 directions (N, S, E, W, NE, NW, SE, SW).
 */
inline void SampleTileFogState(Point tile, int u256, int v256, int maxDensity, int &outDensity, int &outSwirlX, int &outSwirlY)
{
	const int tx = std::clamp(tile.x, 0, MAXDUNX - 1);
	const int ty = std::clamp(tile.y, 0, MAXDUNY - 1);

	const int invU = 256 - u256;
	const int invV = 256 - v256;

	const int w00 = (invU * invV) >> 8;
	const int w10 = (u256 * invV) >> 8;
	const int w01 = (invU * v256) >> 8;
	const int w11 = (u256 * v256) >> 8;

	// 1. Bilinearly interpolate the 4 true corner vertex wall-fade weights (0..256)
	const int cw00 = gFogCornerWeight[tx][ty];
	const int cw10 = gFogCornerWeight[tx + 1][ty];
	const int cw01 = gFogCornerWeight[tx][ty + 1];
	const int cw11 = gFogCornerWeight[tx + 1][ty + 1];

	int wallFade = (cw00 * w00 + cw10 * w10 + cw01 * w01 + cw11 * w11) >> 8;
	// Smoothstep the wall fade so the edge feathers out organically before touching masonry
	wallFade = (wallFade * wallFade * (768 - 2 * wallFade)) >> 16;

	// 2. Apply single-tile entity dispersion localized to this tile's interior (soft diamond bell)
	const FogCell &centerCell = gFogGrid[tx][ty];
	const int missing = std::max(0, maxDensity - static_cast<int>(centerCell.density));

	// Center weight peaks in the middle of the tile and tapers at the tile borders so a single-tile
	// dispersion parts the fog inside this exact cell without hard seam lines against neighbor tiles.
	const int distU = std::abs(u256 - 128);
	const int distV = std::abs(v256 - 128);
	const int radial = std::max(0, 256 - ((distU * distU + distV * distV) >> 6));
	const int localDensity = std::max(0, maxDensity - ((missing * radial) >> 8));

	outDensity = (localDensity * wallFade) >> 8;
	outSwirlX = (static_cast<int>(centerCell.velX) * radial) >> 8;
	outSwirlY = (static_cast<int>(centerCell.velY) * radial) >> 8;
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

	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			const bool passable = IsFogPassableTile({ x, y });
			gFogGrid[x][y].density = passable ? target : 0;
			gFogGrid[x][y].velX = 0;
			gFogGrid[x][y].velY = 0;
		}
	}

	// Precompute per-corner vertex wall-fade weights (0..256) at every grid intersection (vx, vy).
	// Vertex (vx, vy) is shared by the 4 tiles (vx-1, vy-1), (vx, vy-1), (vx-1, vy), and (vx, vy).
	for (int vx = 0; vx <= MAXDUNX; vx++) {
		for (int vy = 0; vy <= MAXDUNY; vy++) {
			const bool t00 = IsFogPassableTile({ vx - 1, vy - 1 });
			const bool t10 = IsFogPassableTile({ vx, vy - 1 });
			const bool t01 = IsFogPassableTile({ vx - 1, vy });
			const bool t11 = IsFogPassableTile({ vx, vy });

			// If ANY of the 4 tiles meeting at this vertex is a solid wall, weight at the wall is 0!
			if (!t00 || !t10 || !t01 || !t11) {
				gFogCornerWeight[vx][vy] = 0;
				continue;
			}

			// Count how many 1-tile-away outer ring tiles around this vertex are passable (out of 12)
			// so fog begins gently tapering 1 tile before reaching a wall.
			int outerPassable = 0;
			for (int dx = -2; dx <= 1; dx++) {
				for (int dy = -2; dy <= 1; dy++) {
					if (dx >= -1 && dx <= 0 && dy >= -1 && dy <= 0)
						continue;
					if (IsFogPassableTile({ vx + dx, vy + dy }))
						outerPassable++;
				}
			}
			// Ranges smoothly from 140 (next to a wall) up to 256 (open room interior)
			gFogCornerWeight[vx][vy] = static_cast<uint16_t>(140 + (outerPassable * 116) / 12);
		}
	}
}

void UpdateGroundFogSimulation()
{
	if (!*sgOptions.Graphics.groundFog || !gLevelHasFog)
		return;

	const LevelFogConfig cfg = GetLevelFogConfig();
	if (!cfg.enabled)
		return;

	EnsureFogNoiseInitialized();

	const int maxDens = cfg.maxDensity;

	// 1. Relax velocity and regrow density locally in a window around the player
	const Point center = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : ViewPosition;
	const int minX = std::max(1, center.x - 28);
	const int maxX = std::min(MAXDUNX - 2, center.x + 28);
	const int minY = std::max(1, center.y - 28);
	const int maxY = std::min(MAXDUNY - 2, center.y + 28);

	for (int x = minX; x <= maxX; x++) {
		for (int y = minY; y <= maxY; y++) {
			FogCell &cell = gFogGrid[x][y];
			if (!IsFogPassableTile({ x, y })) {
				cell.density = 0;
				cell.velX = 0;
				cell.velY = 0;
				continue;
			}

			if (cell.density < maxDens) {
				cell.density = static_cast<uint8_t>(std::min(maxDens, static_cast<int>(cell.density) + 4));
			} else if (cell.density > maxDens) {
				cell.density = static_cast<uint8_t>(maxDens);
			}

			cell.velX = static_cast<int8_t>((cell.velX * 3) / 4);
			cell.velY = static_cast<int8_t>((cell.velY * 3) / 4);
		}
	}

	// 2. Single-tile wake for active Players (only the exact tile the player occupies)
	for (const Player &player : Players) {
		if (!player.plractive || !player.isOnActiveLevel())
			continue;
		const bool moving = player.isWalking();
		CarveSingleTileEntityWake(player.position.tile, player._pdir, moving, moving ? 135 : 45);
	}

	// 3. Single-tile wake for active Monsters (only the exact tile the monster occupies)
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
			CarveSingleTileEntityWake(monster.position.tile, monster.direction, moving, (moving || attacking) ? 120 : 35);
		}
	}

	// 4. Single-tile wake for active Missiles / Spells (only the exact cell the missile occupies)
	for (const Missile &missile : Missiles) {
		if (missile._miDelFlag)
			continue;
		CarveSingleTileEntityWake(missile.position.tile, Direction::South, true, 130);
	}
}

void DrawGroundFogTileBase(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
{
	if (!*sgOptions.Graphics.groundFog || !gLevelHasFog)
		return;
	if (lightIndex >= LightsMax || !IsFogPassableTile(tilePosition))
		return;

	const LevelFogConfig cfg = GetLevelFogConfig();
	if (!cfg.enabled)
		return;

	// Quick reject if all 4 corners of this tile are 0 (completely boxed in by walls)
	const int tx = std::clamp(tilePosition.x, 0, MAXDUNX - 1);
	const int ty = std::clamp(tilePosition.y, 0, MAXDUNY - 1);
	if ((gFogCornerWeight[tx][ty] | gFogCornerWeight[tx + 1][ty] | gFogCornerWeight[tx][ty + 1] | gFogCornerWeight[tx + 1][ty + 1]) == 0)
		return;

	EnsureFogNoiseInitialized();

	// Attenuate crest, core, and trough shades through local dungeon lighting (dLight).
	// For Hell's infernal smoke, clamp light attenuation slightly brighter so rising ash/ember
	// smoke billows remain distinctly visible even in darker corners of Hell.
	const int effLightIdx = cfg.isInfernalSmoke ? std::min(lightIndex, LightsMax - 5) : lightIndex;
	const uint8_t *lightTbl = LightTables[std::clamp(effLightIdx, 0, LightsMax - 1)].data();
	const uint8_t litCrest = lightTbl[cfg.crestShade];
	const uint8_t litCore = lightTbl[cfg.coreShade];
	const uint8_t litTrough = lightTbl[cfg.troughShade];

	const int32_t ticks = static_cast<int32_t>(SDL_GetTicks());
	const int driftX1 = ticks / cfg.driftDivX;
	const int driftY1 = ticks / cfg.driftDivY;
	const int driftX2 = -ticks / ((cfg.driftDivX * 4) / 3);
	const int driftY2 = ticks / ((cfg.driftDivY * 3) / 4);

	// Use larger world-space UV stride (24 texels per tile instead of 32) so individual cloud
	// banks span 2.5 to 4 tiles across for natural multi-tile volumetric formations.
	const int baseWorldU = tilePosition.x * 24;
	const int baseWorldV = tilePosition.y * 24;

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
			SampleTileFogState(tilePosition, u256, v256, cfg.maxDensity, cellDensity, swirlX, swirlY);
			if (cellDensity < 18)
				continue;

			const int tileU = (u256 * 24) >> 8;
			const int tileV = (v256 * 24) >> 8;

			// Sample domain-warp field to curl the mist organically
			const int warpUIdx = (baseWorldU + tileU + driftX2) & 0xFF;
			const int warpVIdx = (baseWorldV + tileV + driftY2) & 0xFF;
			const int warpX = (static_cast<int>(gFogWarpLut[warpVIdx][warpUIdx]) - 128) >> 3;
			const int warpY = (static_cast<int>(gFogWarpLut[(warpVIdx + 73) & 0xFF][(warpUIdx + 41) & 0xFF]) - 128) >> 3;

			// Base ground-hugging fog layer
			const int wu1 = (baseWorldU + tileU + driftX1 + warpX + (swirlX >> 2)) & 0xFF;
			const int wv1 = (baseWorldV + tileV + driftY1 + warpY + (swirlY >> 2)) & 0xFF;
			const int baseNoise = gFogNoiseLut[wv1][wu1];

			// Upper 3D lofted billow layer (sampled with isometric vertical parallax offset)
			const int loftOffset = (baseNoise >> 5); // 0..7 texels of 3D vertical parallax loft
			const int wu2 = (baseWorldU + tileU - loftOffset + driftX2 - warpY + (swirlX >> 1)) & 0xFF;
			const int wv2 = (baseWorldV + tileV - loftOffset + driftY1 + warpX + (swirlY >> 1)) & 0xFF;
			const int upperNoise = gFogNoiseLut[wv2][wu2];

			// Combine base blanket and upper 3D billow crests without flattening peaks
			const int billowPeak = std::max(baseNoise, (baseNoise + upperNoise) >> 1);
			const int finalIntensity = (billowPeak * cellDensity) >> 8;
			if (finalIntensity < 68)
				continue;

			// Vertical ridge lighting gradient inside each billow: where upperNoise exceeds baseNoise,
			// the top ridge of the cloud catches the torchlight (litCrest); deep pockets use litTrough.
			const bool isLitCrest = (upperNoise > baseNoise + 14) && (finalIntensity > 138);

			uint8_t &dst = out[{ sx, sy }];
			if (finalIntensity > 182) {
				const uint8_t shade = isLitCrest ? litCrest : litCore;
				dst = cfg.allowHeavyBlend ? Blend50(shade, dst) : Blend37(shade, dst);
			} else if (finalIntensity > 142) {
				const uint8_t shade = isLitCrest ? litCrest : litCore;
				dst = Blend37(shade, dst);
			} else if (finalIntensity > 104) {
				dst = Blend25(litCore, dst);
			} else if (finalIntensity > 82) {
				dst = Blend19(litTrough, dst);
			} else {
				dst = Blend12(litTrough, dst);
			}
		}
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

	const int tx = std::clamp(tilePosition.x, 0, MAXDUNX - 1);
	const int ty = std::clamp(tilePosition.y, 0, MAXDUNY - 1);
	const int cornerAvg = (gFogCornerWeight[tx][ty] + gFogCornerWeight[tx + 1][ty]
	                          + gFogCornerWeight[tx][ty + 1] + gFogCornerWeight[tx + 1][ty + 1])
	    >> 2;
	if (cornerAvg < 48)
		return;

	EnsureFogNoiseInitialized();

	// Soft ankle/boot wisp density scaled by wall fade so no hard box ever appears around entities
	const int collarDensity = ((static_cast<int>(cfg.maxDensity) * 3) / 5 * cornerAvg) >> 8;
	if (collarDensity < 32)
		return;

	const int effLightIdx = cfg.isInfernalSmoke ? std::min(lightIndex, LightsMax - 5) : lightIndex;
	const uint8_t *lightTbl = LightTables[std::clamp(effLightIdx, 0, LightsMax - 1)].data();
	const uint8_t litCrest = lightTbl[cfg.crestShade];
	const uint8_t litCore = lightTbl[cfg.coreShade];

	const int32_t ticks = static_cast<int32_t>(SDL_GetTicks());
	const int driftX = ticks / cfg.driftDivX;
	const int driftY = ticks / cfg.driftDivY;

	const int outW = out.w();
	const int outH = out.h();

	// Tight elliptical wisp centered strictly over the entity's boots/lower legs (no 48x20 rectangle)
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
			const int wu = (tilePosition.x * 24 + dx + driftX) & 0xFF;
			const int wv = (tilePosition.y * 24 + dy * 2 + driftY) & 0xFF;
			const int noise = gFogNoiseLut[wv][wu];

			const int wispIntensity = (((noise * collarDensity) >> 8) * radialFalloff) >> 8;
			if (wispIntensity < 72)
				continue;

			uint8_t &dst = out[{ sx, sy }];
			if (wispIntensity > 145) {
				dst = Blend25(litCrest, dst);
			} else if (wispIntensity > 102) {
				dst = Blend19(litCore, dst);
			} else {
				dst = Blend12(litCore, dst);
			}
		}
	}
}

} // namespace devilution
