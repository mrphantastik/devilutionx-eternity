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
	uint8_t dir;     // 8-way Direction of the entity/missile occupying or parting this tile
};

struct LevelFogConfig {
	bool enabled;
	uint8_t spawnChancePercent; // Chance (0..100%) for this dungeon floor to spawn fog (100% for testing)
	uint8_t maxDensity;         // Target ambient density (0..255)
	uint8_t intensityCutoff;    // Minimum intensity threshold before fog pixels render
	int uvScale;                // Texels per tile (lower = larger cloud banks, higher = tighter clusters)
	uint8_t crestShade;         // Brighter palette index for lit upper billow domes
	uint8_t coreShade;          // Mid-tone palette index for main cloud body
	uint8_t troughShade;        // Darker palette index for lower shadow creases/undersides
	bool isDarkSmoke;           // True in Hell: uses raw dark charcoal body + black soot trough + ember-orange crests
	bool allowHeavyBlend;       // True in Crypt/Hell: allows 50% LUT blend on thickest billow crests
	bool hasPulse;              // True in Hive/Nest: slowly pulses bio-spore density
	int driftDivX;              // Horizontal drift divisor (~3x slower: 115..240)
	int driftDivY;              // Vertical drift divisor (~3x slower: 120..310; negative = upward thermal draft)
};

bool gNoiseInitialized = false;
bool gLevelHasFog = false;

// Precomputed 256x256 seamless multi-octave billow FBM LUT (0..255) and domain-warp LUT (0..255)
std::array<std::array<uint8_t, 256>, 256> gFogNoiseLut;
std::array<std::array<uint8_t, 256>, 256> gFogWarpLut;

// Per-tile interactive dispersion state (single-tile directional U-shield displacement)
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
		// Upward-rising geothermal steam (also rises over lava rivers)
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
			/*driftDivY=*/-155 // Negative Y = rising thermal steam draft
		};
	case DTYPE_HELL:
		// Dark charcoal/black rising soot smoke with glowing ember-orange top crests
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/225,
			/*intensityCutoff=*/64,
			/*uvScale=*/24,
			/*crestShade=*/PAL16_ORANGE + 4, // Glowing ember-orange top crests
			/*coreShade=*/PAL16_GRAY + 13,   // Raw dark charcoal smoke body
			/*troughShade=*/PAL16_GRAY + 15, // Deep black soot shadow trough
			/*isDarkSmoke=*/true,
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/false,
			/*driftDivX=*/115,
			/*driftDivY=*/-120 // Upward thermal smoke draft
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
 * @brief Computes the 8-way Direction of a moving missile from its screen-space velocity
 * vector (deltaX, deltaY), falling back to _mimfnum if stationary.
 */
Direction VelocityToDirection(int vx, int vy, int fallbackMimfnum)
{
	if (vx == 0 && vy == 0) {
		if (fallbackMimfnum >= 0 && fallbackMimfnum < 8)
			return static_cast<Direction>(fallbackMimfnum);
		if (fallbackMimfnum >= 0 && fallbackMimfnum < 16)
			return static_cast<Direction>((fallbackMimfnum / 2) & 7);
		return Direction::South;
	}

	// Compensate for 2:1 isometric vertical aspect ratio
	const int sy = vy * 2;
	const int ax = std::abs(vx);
	const int ay = std::abs(sy);

	if (ay > ax * 2) {
		return (sy >= 0) ? Direction::South : Direction::North;
	}
	if (ax > ay * 2) {
		return (vx >= 0) ? Direction::East : Direction::West;
	}
	if (vx < 0) {
		return (sy >= 0) ? Direction::SouthWest : Direction::NorthWest;
	}
	return (sy >= 0) ? Direction::SouthEast : Direction::NorthEast;
}

/**
 * @brief Displaces fog strictly on the exact single tile (tile.x, tile.y) occupied by
 * an entity or missile, recording its facing Direction for the U-shield cutout.
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
	cell.dir = static_cast<uint8_t>(dirIdx);

	if (isMoving) {
		cell.velX = static_cast<int8_t>(std::clamp(static_cast<int>(cell.velX) + DirPushX[dirIdx], -120, 120));
		cell.velY = static_cast<int8_t>(std::clamp(static_cast<int>(cell.velY) + DirPushY[dirIdx], -120, 120));
	}
}

/**
 * @brief Computes the tightened wall-fade weight (0..256) and directional U-shield entity
 * cutout at sub-tile coordinates (u256, v256) in [0..256] x [0..256].
 *
 * - Behind the entity (fwd < 0): smooth rounded semicircle (side^2 + fwd^2).
 * - In front of the entity (fwd >= 0, facing direction): straight side walls capped by a
 *   flat straight leading edge perpendicular to the facing vector (std::max(absSide, fwdFlat)).
 */
inline void SampleTileFogState(Point tile, int u256, int v256, int maxDensity, int &outDensity, int &outWallFade, int &outSwirlX, int &outSwirlY)
{
	const int tx = std::clamp(tile.x, 0, MAXDUNX - 1);
	const int ty = std::clamp(tile.y, 0, MAXDUNY - 1);

	const int invU = 256 - u256;
	const int invV = 256 - v256;

	const int w00 = (invU * invV) >> 8;
	const int w10 = (u256 * invV) >> 8;
	const int w01 = (invU * v256) >> 8;
	const int w11 = (u256 * v256) >> 8;

	// 1. Tightened wall-to-fog margin (cut in half): 2x steeper ramp + concave ease-out
	const int cw00 = gFogCornerWeight[tx][ty];
	const int cw10 = gFogCornerWeight[tx + 1][ty];
	const int cw01 = gFogCornerWeight[tx][ty + 1];
	const int cw11 = gFogCornerWeight[tx + 1][ty + 1];

	const int rawWallFade = (cw00 * w00 + cw10 * w10 + cw01 * w01 + cw11 * w11) >> 8;
	const int ramped = std::min(256, rawWallFade * 2);
	const int wallFade = (ramped * (512 - ramped)) >> 8;
	outWallFade = wallFade;

	// 2. Directional U-Shield cutout around entities & spells on this single tile
	const FogCell &centerCell = gFogGrid[tx][ty];
	const int missing = std::max(0, maxDensity - static_cast<int>(centerCell.density));
	if (missing <= 0) {
		outDensity = (maxDensity * wallFade) >> 8;
		outSwirlX = 0;
		outSwirlY = 0;
		return;
	}

	// Screen-aligned isometric offset from tile center (-128..128)
	// isoX > 0 = East (Right), isoY > 0 = South (Down)
	const int isoX = ((u256 - 128) - (v256 - 128)) >> 1;
	const int isoY = ((u256 - 128) + (v256 - 128)) >> 1;

	// 8-way unit forward vectors in screen space (256 = 1.0, 181 = 256 / sqrt(2))
	// Order: South(0), SouthWest(1), West(2), NorthWest(3), North(4), NorthEast(5), East(6), SouthEast(7)
	static constexpr int FwdX[8] = { 0, -181, -256, -181, 0, 181, 256, 181 };
	static constexpr int FwdY[8] = { 256, 181, 0, -181, -256, -181, 0, 181 };

	const unsigned dirIdx = centerCell.dir & 7U;
	const int fx = FwdX[dirIdx];
	const int fy = FwdY[dirIdx];

	// Project local offset onto entity's facing vector (fwd) and perpendicular axis (side)
	const int fwd = (isoX * fx + isoY * fy) >> 8;
	const int side = (isoX * (-fy) + isoY * fx) >> 8;
	const int absSide = std::abs(side);

	int distMetric = 0;
	if (fwd < 0) {
		// Behind the entity: smooth rounded semicircle
		distMetric = (side * side + fwd * fwd) >> 6;
	} else {
		// In front of the entity: straight side walls capped by a flat straight leading edge
		const int fwdFlat = (fwd * 12) / 10;
		const int boxDist = std::max(absSide, fwdFlat);
		distMetric = (boxDist * boxDist) >> 6;
	}

	const int uShield = std::max(0, 256 - distMetric);
	const int localDensity = std::max(0, maxDensity - ((missing * uShield) >> 8));

	outDensity = (localDensity * wallFade) >> 8;
	outSwirlX = (static_cast<int>(centerCell.velX) * uShield) >> 8;
	outSwirlY = (static_cast<int>(centerCell.velY) * uShield) >> 8;
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
			gFogGrid[x][y].dir = static_cast<uint8_t>(Direction::South);
		}
	}

	// Precompute per-corner vertex wall-fade weights (0..256) at every grid intersection (vx, vy).
	// Vertex (vx, vy) is shared by the 4 tiles (vx-1, vy-1), (vx, vy-1), (vx-1, vy), and (vx, vy).
	// Full 256 weight is set immediately at the first interior vertex so the wall-to-fog margin is cut in half.
	for (int vx = 0; vx <= MAXDUNX; vx++) {
		for (int vy = 0; vy <= MAXDUNY; vy++) {
			const bool t00 = IsFogPassableTile({ vx - 1, vy - 1 });
			const bool t10 = IsFogPassableTile({ vx, vy - 1 });
			const bool t01 = IsFogPassableTile({ vx - 1, vy });
			const bool t11 = IsFogPassableTile({ vx, vy });

			if (!t00 || !t10 || !t01 || !t11) {
				gFogCornerWeight[vx][vy] = 0;
			} else {
				gFogCornerWeight[vx][vy] = 256;
			}
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

	// 1. Relax velocity and regrow density (+10 per tick so stepped-off tiles close up cleanly behind the U-shield wake)
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
				cell.density = static_cast<uint8_t>(std::min(maxDens, static_cast<int>(cell.density) + 10));
			} else if (cell.density > maxDens) {
				cell.density = static_cast<uint8_t>(maxDens);
			}

			cell.velX = static_cast<int8_t>((cell.velX * 3) / 4);
			cell.velY = static_cast<int8_t>((cell.velY * 3) / 4);
		}
	}

	// 2. Single-tile directional U-shield wake for active Players
	for (const Player &player : Players) {
		if (!player.plractive || !player.isOnActiveLevel())
			continue;
		const bool moving = player.isWalking();
		CarveSingleTileEntityWake(player.position.tile, player._pdir, moving, moving ? 145 : 65);
	}

	// 3. Single-tile directional U-shield wake for active Monsters
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
			CarveSingleTileEntityWake(monster.position.tile, monster.direction, moving, (moving || attacking) ? 130 : 55);
		}
	}

	// 4. Single-tile directional U-shield wake for active Missiles / Spells
	for (const Missile &missile : Missiles) {
		if (missile._miDelFlag)
			continue;
		const Direction missileDir = VelocityToDirection(
		    missile.position.velocity.deltaX,
		    missile.position.velocity.deltaY,
		    missile._mimfnum);
		CarveSingleTileEntityWake(missile.position.tile, missileDir, true, 140);
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

	// Resolve volumetric shades:
	// In Hell (isDarkSmoke), use raw dark charcoal body (PAL16_GRAY + 13) and black soot trough
	// (PAL16_GRAY + 15) directly so they darken bright lava/floors into heavy soot smoke instead
	// of collapsing to index 0, while lighting the glowing ember-orange top crests.
	const uint8_t *lightTbl = LightTables[std::clamp(lightIndex, 0, LightsMax - 1)].data();
	const uint8_t litCrest = lightTbl[cfg.crestShade];
	const uint8_t litCore = cfg.isDarkSmoke ? cfg.coreShade : lightTbl[cfg.coreShade];
	const uint8_t litTrough = cfg.isDarkSmoke ? cfg.troughShade : lightTbl[cfg.troughShade];

	const int32_t ticks = static_cast<int32_t>(SDL_GetTicks());
	const int driftX1 = ticks / cfg.driftDivX;
	const int driftY1 = ticks / cfg.driftDivY;
	const int driftX2 = -ticks / ((cfg.driftDivX * 3) / 2);
	const int driftY2 = ticks / ((cfg.driftDivY * 4) / 3);

	// Optional slow bio-spore breathing pulse for Hive/Nest
	int effectiveMaxDensity = cfg.maxDensity;
	if (cfg.hasPulse) {
		const int phase = (ticks / 28 + tilePosition.x * 19 + tilePosition.y * 31) & 0xFF;
		const int triWave = (phase < 128) ? phase : (255 - phase); // 0..127
		effectiveMaxDensity = std::clamp(static_cast<int>(cfg.maxDensity) - 18 + (triWave >> 2), 0, 255);
	}

	const int uvScale = cfg.uvScale;
	const int baseWorldU = tilePosition.x * uvScale;
	const int baseWorldV = tilePosition.y * uvScale;

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

			int cellDensity = effectiveMaxDensity;
			int wallFade = 256;
			int swirlX = 0;
			int swirlY = 0;
			SampleTileFogState(tilePosition, u256, v256, effectiveMaxDensity, cellDensity, wallFade, swirlX, swirlY);
			if (cellDensity < 18)
				continue;

			const int tileU = (u256 * uvScale) >> 8;
			const int tileV = (v256 * uvScale) >> 8;

			// Sample domain-warp field to curl the mist organically
			const int warpUIdx = (baseWorldU + tileU + driftX2) & 0xFF;
			const int warpVIdx = (baseWorldV + tileV + driftY2) & 0xFF;
			const int warpX = (static_cast<int>(gFogWarpLut[warpVIdx][warpUIdx]) - 128) >> 3;
			const int warpY = (static_cast<int>(gFogWarpLut[(warpVIdx + 73) & 0xFF][(warpUIdx + 41) & 0xFF]) - 128) >> 3;

			// Primary and secondary scrolling FBM coordinates (without dynamic loftOffset UV jitter)
			const int wu1 = (baseWorldU + tileU + driftX1 + warpX + (swirlX >> 2)) & 0xFF;
			const int wv1 = (baseWorldV + tileV + driftY1 + warpY + (swirlY >> 2)) & 0xFF;
			const int wu2 = (baseWorldU + tileU + driftX2 - warpY + (swirlX >> 2)) & 0xFF;
			const int wv2 = (baseWorldV + tileV + driftY2 + warpX + (swirlY >> 2)) & 0xFF;

			const int hCenter = (static_cast<int>(gFogNoiseLut[wv1][wu1]) * 3 + static_cast<int>(gFogNoiseLut[wv2][wu2])) >> 2;
			const int finalIntensity = (hCenter * cellDensity) >> 8;
			if (finalIntensity < cfg.intensityCutoff)
				continue;

			// True 3D volumetric vertical slope derivative (verticalSlope = hBelow - hAbove):
			// - verticalSlope > 8  : top-facing upper dome catching overhead light (litCrest)
			// - verticalSlope < -10: lower underside crease resting in shadow on the floor (litTrough)
			const int hAbove = (static_cast<int>(gFogNoiseLut[(wv1 - 3) & 0xFF][(wu1 - 3) & 0xFF]) * 3
			                       + static_cast<int>(gFogNoiseLut[(wv2 - 3) & 0xFF][(wu2 - 3) & 0xFF]))
			    >> 2;
			const int hBelow = (static_cast<int>(gFogNoiseLut[(wv1 + 3) & 0xFF][(wu1 + 3) & 0xFF]) * 3
			                       + static_cast<int>(gFogNoiseLut[(wv2 + 3) & 0xFF][(wu2 + 3) & 0xFF]))
			    >> 2;
			const int verticalSlope = hBelow - hAbove;

			// 1. Floor base contact & underside shadow crease at sy
			uint8_t &dstBase = out[{ sx, sy }];
			if (verticalSlope < -10) {
				dstBase = (finalIntensity > 140) ? Blend37(litTrough, dstBase) : Blend25(litTrough, dstBase);
			} else if (finalIntensity > 120) {
				dstBase = Blend19(litTrough, dstBase);
			} else {
				dstBase = Blend12(litTrough, dstBase);
			}

			// 2. Physical screen-space vertical Z-loft (lifts upper cloud body and lit top dome
			// up to 9 pixels above the floor plane, scaled by wallFade so it flattens to 0 at walls)
			const int rawLift = ((finalIntensity - cfg.intensityCutoff) * 9) / std::max(1, 255 - static_cast<int>(cfg.intensityCutoff));
			const int zLift = std::clamp((rawLift * wallFade) >> 8, 0, 9);
			const int syLift = sy - zLift;
			if (syLift < 0 || syLift >= outH)
				continue;

			uint8_t puffShade = litCore;
			if (verticalSlope > 8) {
				puffShade = litCrest;
			} else if (verticalSlope < -10) {
				puffShade = litTrough;
			}

			// Fill intermediate mid-column pixel when lifted >= 3px so 3D billows never show scanline gaps
			if (zLift >= 3) {
				const int syMid = sy - (zLift >> 1);
				if (syMid >= 0 && syMid < outH) {
					uint8_t &dstMid = out[{ sx, syMid }];
					dstMid = (finalIntensity > 145) ? Blend25(litCore, dstMid) : Blend19(litCore, dstMid);
				}
			}

			uint8_t &dstTop = out[{ sx, syLift }];
			if (finalIntensity > 180) {
				dstTop = cfg.allowHeavyBlend ? Blend50(puffShade, dstTop) : Blend37(puffShade, dstTop);
			} else if (finalIntensity > 138) {
				dstTop = Blend37(puffShade, dstTop);
			} else if (finalIntensity > 102) {
				dstTop = Blend25(puffShade, dstTop);
			} else if (finalIntensity > 80) {
				dstTop = Blend19(puffShade, dstTop);
			} else {
				dstTop = Blend12(puffShade, dstTop);
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

	const uint8_t *lightTbl = LightTables[std::clamp(lightIndex, 0, LightsMax - 1)].data();
	const uint8_t litCrest = lightTbl[cfg.crestShade];
	const uint8_t litCore = cfg.isDarkSmoke ? cfg.coreShade : lightTbl[cfg.coreShade];

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
			const int wu = (tilePosition.x * cfg.uvScale + dx + driftX) & 0xFF;
			const int wv = (tilePosition.y * cfg.uvScale + dy * 2 + driftY) & 0xFF;
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
