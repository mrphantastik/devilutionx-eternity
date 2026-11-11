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
std::array<std::array<uint8_t, 256>, 256> gFogWarpLut;

uint32_t gLastFogSheetUpdateTick = 0;
dungeon_type gLastFogSheetDtype = DTYPE_NONE;
int gCachedPulseOffset = 0;
std::array<std::array<uint8_t, 256>, 256> gCachedFloorFog;
std::array<std::array<uint8_t, 256>, 256> gCachedCanopyFog;
std::array<std::array<int8_t, 256>, 256> gCachedCanopySlope;

/**
 * Direct RGB-interpolated 6-tier software alpha LUT:
 * Avoids recursive paletteTransparencyLookup chaining (which snaps dark pixels
 * to 0 Pure Black and gets stuck in quantization fixed-points, causing black blotches
 * and contour rings).
 * Tiers 0..5 correspond to:
 *   0: ~6.25%  (1/16 fg + 15/16 bg)
 *   1: ~12.5%  (2/16 fg + 14/16 bg)
 *   2: ~18.75% (3/16 fg + 13/16 bg)
 *   3: ~25.0%  (4/16 fg + 12/16 bg)
 *   4: ~31.25% (5/16 fg + 11/16 bg)
 *   5: ~37.5%  (6/16 fg + 10/16 bg)
 */
uint8_t gFogAlphaLut[6][256][256];
uint32_t gCachedFogLutPaletteVersion = 0xFFFFFFFFU;
dungeon_type gCachedFogLutDtype = DTYPE_NONE;

/**
 * 4x4 Ordered Bayer Dither Matrix (-14..+14):
 * Smoothly feathers transitions across the 6 alpha blend tiers so contour edges
 * never exhibit rectilinear texel staircase blocks.
 */
constexpr int8_t Bayer4x4[4][4] = {
	{ -14,   2, -10,   6 },
	{  10,  -6,  14,  -2 },
	{  -8,   8, -12,   4 },
	{  12,  -4,   8,  -8 },
};

std::array<std::array<FogHalfCell, HalfGridH>, HalfGridW> gFogHalfGrid;
std::array<std::array<bool, MAXDUNY>, MAXDUNX> gFogTilePassable;

LevelFogConfig GetLevelFogConfig()
{
	// NOTE: Uses pure neutral PAL16_GRAY (240..254) for neutral mist/smoke across dungeons
	// instead of PAL16_BEIGE (192..207), which is a salmon/reddish-tan ramp in Diablo 1's palette!
	switch (leveltype) {
	case DTYPE_CATHEDRAL:
		// Cold spectral crypt mist (Layer 1) with bright 3D cloud puffs & clear drop-shadow (Layer 2)
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/214,
			/*intensityCutoff=*/74,
			/*uvScale=*/44,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 2),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 11),
			/*allowHeavyBlend=*/false,
			/*hasPulse=*/false,
			/*driftDivX=*/560,
			/*driftDivY=*/820
		};
	case DTYPE_CATACOMBS:
		// Pale neutral bone-ash haze (Layer 1) with ghostly drifting crypt billows (Layer 2)
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/212,
			/*intensityCutoff=*/74,
			/*uvScale=*/44,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 2),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 11),
			/*allowHeavyBlend=*/false,
			/*hasPulse=*/false,
			/*driftDivX=*/600,
			/*driftDivY=*/860
		};
	case DTYPE_CAVES:
		// Rising volcanic steam bed (Layer 1) with silver-gray thermal puffs (Layer 2)
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/214,
			/*intensityCutoff=*/72,
			/*uvScale=*/46,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 2),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 11),
			/*allowHeavyBlend=*/false,
			/*hasPulse=*/false,
			/*driftDivX=*/460,
			/*driftDivY=*/-520
		};
	case DTYPE_HELL:
		// Rolling abyssal brimstone smoke: high-contrast smoky silver-ash crests (PAL16_GRAY + 2)
		// over medium-dark charcoal smoke body (PAL16_GRAY + 7) and deep soot troughs (PAL16_GRAY + 12)
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/222,
			/*intensityCutoff=*/72,
			/*uvScale=*/44,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 2),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 7),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 12),
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/false,
			/*driftDivX=*/460,
			/*driftDivY=*/-500
		};
	case DTYPE_NEST:
		// Pulsing toxic bio-spore miasma
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/214,
			/*intensityCutoff=*/74,
			/*uvScale=*/48,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 2),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 11),
			/*allowHeavyBlend=*/false,
			/*hasPulse=*/true,
			/*driftDivX=*/520,
			/*driftDivY=*/-600
		};
	case DTYPE_CRYPT:
		// Thick ancient tomb-dust & ectoplasmic floor blanket
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/224,
			/*intensityCutoff=*/70,
			/*uvScale=*/42,
			/*crestShade=*/static_cast<uint8_t>(PAL16_GRAY + 2),
			/*coreShade=*/static_cast<uint8_t>(PAL16_GRAY + 6),
			/*troughShade=*/static_cast<uint8_t>(PAL16_GRAY + 11),
			/*allowHeavyBlend=*/true,
			/*hasPulse=*/false,
			/*driftDivX=*/680,
			/*driftDivY=*/900
		};
	default:
		return { false, 0, 0, 255, 44, 0, 0, 0, false, false, 600, 600 };
	}
}

/**
 * Shades a fog palette index smoothly by dungeon lightIndex (0 = bright, 14 = dark)
 * WITHOUT allowing PAL16_GRAY (240..254) to snap to 0 (Pure Black) at lightIndex >= 6,
 * which previously caused Hell and Cathedral fog troughs/drop-shadows to collapse into solid black!
 */
inline uint8_t ShadeFogColor(uint8_t baseColor, int lightIndex)
{
	const int clampedLight = std::clamp(lightIndex, 0, 14);
	if (baseColor >= PAL16_GRAY && baseColor < PAL16_GRAY + 15) {
		const int baseStep = static_cast<int>(baseColor - PAL16_GRAY);
		const int shadedStep = std::clamp(baseStep + ((clampedLight * (14 - baseStep)) / 15), 1, 14);
		return static_cast<uint8_t>(PAL16_GRAY + shadedStep);
	}
	const uint8_t lit = LightTables[clampedLight][baseColor];
	return (lit == 0) ? static_cast<uint8_t>(PAL16_GRAY + 14) : lit;
}

/**
 * Rebuilds the direct RGB 6-tier alpha LUT whenever the dungeon palette changes
 * (ignoring color-cycling ticks on 1..31 by skipping 1..31 in the best-match search,
 * and never snapping non-zero floor pixels to index 0 Pure Black or index 255 Pure White).
 */
void EnsureFogAlphaLutUpdated()
{
	if (gCachedFogLutDtype == leveltype && gCachedFogLutPaletteVersion == pal_surface_palette_version)
		return;

	// If only color cycling rotated 1..31 on the same leveltype, we only need to rebuild once per level load
	// Check whether static palette entries (32..254) actually changed:
	static uint32_t sLastStaticPalHash = 0;
	uint32_t staticHash = 2166136261U;
	for (int i = 32; i < 255; i += 3) {
		staticHash = (staticHash ^ orig_palette[i].r) * 16777619U;
		staticHash = (staticHash ^ orig_palette[i].g) * 16777619U;
		staticHash = (staticHash ^ orig_palette[i].b) * 16777619U;
	}
	gCachedFogLutPaletteVersion = pal_surface_palette_version;
	if (gCachedFogLutDtype == leveltype && staticHash == sLastStaticPalHash)
		return;

	gCachedFogLutDtype = leveltype;
	sLastStaticPalHash = staticHash;

	// Search only static non-cycling indices (32..254) so fog never maps to index 0 (Pure Black),
	// index 255 (Pure White), or indices 1..31 (animated lava/water cycle).
	for (int bg = 0; bg < 256; bg++) {
		const int bgR = orig_palette[bg].r;
		const int bgG = orig_palette[bg].g;
		const int bgB = orig_palette[bg].b;

		for (int fg = 0; fg < 256; fg++) {
			if (bg == 0) {
				for (int t = 0; t < 6; t++)
					gFogAlphaLut[t][bg][fg] = 0;
				continue;
			}
			const int fgR = orig_palette[fg].r;
			const int fgG = orig_palette[fg].g;
			const int fgB = orig_palette[fg].b;

			for (int t = 0; t < 6; t++) {
				const int w = t + 1; // 1..6 out of 16 (6.25% .. 37.5%)
				const int targetR = (bgR * (16 - w) + fgR * w + 8) >> 4;
				const int targetG = (bgG * (16 - w) + fgG * w + 8) >> 4;
				const int targetB = (bgB * (16 - w) + fgB * w + 8) >> 4;

				uint8_t bestIdx = static_cast<uint8_t>(bg);
				int bestDist = 0x7FFFFFFF;
				for (int i = 32; i < 255; i++) {
					const int dr = static_cast<int>(orig_palette[i].r) - targetR;
					const int dg = static_cast<int>(orig_palette[i].g) - targetG;
					const int db = static_cast<int>(orig_palette[i].b) - targetB;
					const int dist = dr * dr * 3 + dg * dg * 4 + db * db * 2;
					if (dist < bestDist) {
						bestDist = dist;
						bestIdx = static_cast<uint8_t>(i);
						if (dist == 0)
							break;
					}
				}
				gFogAlphaLut[t][bg][fg] = bestIdx;
			}
		}
	}
}

inline uint8_t ApplySoftFogBlend(uint8_t shade, uint8_t dst, int alpha, bool allowHeavy)
{
	if (alpha > 192) {
		return gFogAlphaLut[allowHeavy ? 5 : 4][dst][shade];
	}
	if (alpha > 152) {
		return gFogAlphaLut[4][dst][shade];
	}
	if (alpha > 114) {
		return gFogAlphaLut[3][dst][shade];
	}
	if (alpha > 78) {
		return gFogAlphaLut[2][dst][shade];
	}
	if (alpha > 44) {
		return gFogAlphaLut[1][dst][shade];
	}
	if (alpha > 14) {
		return gFogAlphaLut[0][dst][shade];
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
		for (int x = 0; x < 256; x++) {
			const float fx = static_cast<float>(x);
			const float fy = static_cast<float>(y);

			const float wx = ValueNoiseTileableFloat(fx, fy, 3, 901U) * 0.62F
			    + ValueNoiseTileableFloat(fx, fy, 6, 902U) * 0.38F;
			const float wy = ValueNoiseTileableFloat(fx, fy, 3, 903U) * 0.62F
			    + ValueNoiseTileableFloat(fx, fy, 6, 904U) * 0.38F;
			gFogWarpLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(wx * 255.0F + 0.5F), 0, 255));

			// Continuous floating-point curl-warped coordinates (zero integer truncation!)
			const float sx = fx + (wx - 0.5F) * 54.0F;
			const float sy = fy + (wy - 0.5F) * 54.0F;

			const float n1 = ValueNoiseTileableFloat(sx, sy, 3, 101U);
			const float n2 = ValueNoiseTileableFloat(sx, sy, 6, 202U);
			const float n3 = ValueNoiseTileableFloat(sx, sy, 12, 303U);
			const float n4 = ValueNoiseTileableFloat(sx, sy, 24, 404U);

			// Smooth multi-octave FBM clouds + gentle wispy billows
			const float fbm = n1 * 0.48F + n2 * 0.30F + n3 * 0.15F + n4 * 0.07F;
			const float ridge = 1.0F - std::fabs(n2 * 2.0F - 1.0F);
			const float ridgeSmooth = ridge * ridge * (3.0F - 2.0F * ridge);
			const float raw = fbm * 0.78F + ridgeSmooth * 0.22F;

			const float norm = std::clamp((raw - 0.35F) * 2.10F, 0.0F, 1.0F);
			const float billow = norm * norm * (3.0F - 2.0F * norm);
			gFogNoiseLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(billow * 255.0F + 0.5F), 0, 255));
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
	// Layer 1 (Low Ground Mist Bed): gentle creeping drift along primary thermal vector
	const int driftX1 = (sTicks * 256) / cfg.driftDivX;
	const int driftY1 = (sTicks * 256) / cfg.driftDivY;

	// Layer 2 (Elevated 3D Cloud Puffs): drifts at a ~65-degree cross-wind angle
	const int driftX2 = ((driftX1 * 5) >> 3) + driftY1;
	const int driftY2 = ((driftY1 * 5) >> 3) - driftX1;

	// Decoupled lively internal counter-rotating swirl velocities!
	// Even though overall fog travel is gentle, internal wisps continuously curl and swirl
	const int warpDriftX1 = -(sTicks * 256) / 92;
	const int warpDriftY1 = (sTicks * 256) / 110;
	const int warpDriftX2 = (sTicks * 256) / 78;
	const int warpDriftY2 = -(sTicks * 256) / 88;

	if (cfg.hasPulse) {
		const int phase = (sTicks / 65) & 0xFF;
		const int triWave = (phase < 128) ? phase : (255 - phase);
		gCachedPulseOffset = (triWave >> 2) - 16;
	} else {
		gCachedPulseOffset = 0;
	}

	for (int y = 0; y < 256; y++) {
		const int baseV = (y << 8);
		const int vWarpFixed0 = baseV + warpDriftY1;
		const int vWarpFixed1 = baseV + (73 << 8) - warpDriftX1;
		const int vWarpFixed2 = ((y * 2 + 131) << 8) + warpDriftY2;

		for (int x = 0; x < 256; x++) {
			const int baseU = (x << 8);
			const int uWarpFixed0 = baseU + warpDriftX1;
			const int uWarpFixed1 = baseU + (41 << 8) + warpDriftY1;
			const int uWarpFixed2 = ((x * 2 + 59) << 8) + warpDriftX2;

			// Dual counter-rotating sub-texel bilinear domain warps for lively internal swirliness
			const int wA = SampleLutBilinear(gFogWarpLut, uWarpFixed0, vWarpFixed0) - 128;
			const int wB = SampleLutBilinear(gFogWarpLut, uWarpFixed1, vWarpFixed1) - 128;
			const int wC = SampleLutBilinear(gFogWarpLut, uWarpFixed2, vWarpFixed2) - 128;

			const int warpX1 = wA * 28 + wC * 16;
			const int warpY1 = wB * 28 - wC * 16;

			// 1. Layer 1: Low Ground-Hugging Mist Bed
			const int u1 = baseU + warpX1 + driftX1;
			const int v1 = baseV + warpY1 + driftY1;
			gCachedFloorFog[y][x] = static_cast<uint8_t>(SampleLutBilinear(gFogNoiseLut, u1, v1));

			// 2. Layer 2: Elevated 3D Cloud Puffs (independent counter-curling swirl)
			const int warpX2 = -wB * 30 + wC * 18;
			const int warpY2 = wA * 30 + wC * 18;
			const int u2 = baseU + warpX2 + driftX2 + (137 << 8);
			const int v2 = baseV + warpY2 + driftY2 + (89 << 8);
			gCachedCanopyFog[y][x] = static_cast<uint8_t>(SampleLutBilinear(gFogNoiseLut, u2, v2));
		}
	}

	// Precompute 3D NW->SE dome slope on Layer 2 at 30 Hz so per-pixel rendering needs zero extra bilinear calls
	for (int y = 0; y < 256; y++) {
		const int yNW = (y - 3) & 0xFF;
		const int ySE = (y + 3) & 0xFF;
		for (int x = 0; x < 256; x++) {
			const int xNW = (x - 3) & 0xFF;
			const int xSE = (x + 3) & 0xFF;
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

	const MICROS &micros = DPieceMicros[piece];
	if (micros.mt[0] == 0 && micros.mt[1] == 0)
		return false;

	if (!TileHasAny(piece, TileProperties::BlockLight))
		return true;

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

// ============================================================================
// Caves Boiling Lava Bubbles (Rendered Below Ground Fog + 6-Tile Proximity Audio)
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
	// Open lava river & pool pieces in l3.til
	if ((piece >= 55 && piece <= 146) || (piece >= 153 && piece <= 160) || piece == 149 || piece == 151)
		return true;
	return false;
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

/**
 * Plays LS_LAVABUBBLES ONLY when the player is relatively close (<= 6 tiles) to
 * visible, illuminated lava tiles within the player's light radius.
 */
void SyncLavaBubbleAudio()
{
	const bool isMenuPaused = (PauseMode != 0 || (!gbIsMultiplayer && gmenu_is_active()));
	if (!gbRunGame || !gbSoundOn || isMenuPaused || leveltype != DTYPE_CAVES || setlevel) {
		StopSFX(LS_LAVABUBBLES);
		return;
	}

	const Point center = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : ViewPosition;
	constexpr int MaxHearRadius = 6;
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
			// Require the lava tile to be visible inside the player's active light radius
			if (dLight[x][y] >= LightsMax)
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
	const int lavaVolume = -150 - ((clamped256 * 1750) >> 8);
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

	SyncLavaBubbleAudio();
}

/**
 * Fast O(1) 8-point shoreline margin check directly against the Surface buffer
 * (replaces the 129,000-iteration per-tile safeLavaPx erosion scan that dropped Caves to 30 FPS!).
 */
inline bool IsSafeLavaFootprint(const Surface &out, int cx, int cy, int radX, int radY)
{
	const int rx = radX + 4;
	const int ry = radY + 3;
	if (cx - rx < 0 || cx + rx >= out.w() || cy - ry < 0 || cy + ry >= out.h())
		return false;

	auto isLavaPx = [&](int px, int py) {
		const uint8_t c = out[{ px, py }];
		return c >= 1 && c <= 31;
	};

	if (!isLavaPx(cx, cy) || !isLavaPx(cx - rx, cy) || !isLavaPx(cx + rx, cy)
	    || !isLavaPx(cx, cy - ry) || !isLavaPx(cx, cy + ry))
		return false;

	const int dxDiag = (rx * 3) >> 2;
	const int dyDiag = (ry * 3) >> 2;
	return isLavaPx(cx - dxDiag, cy - dyDiag) && isLavaPx(cx + dxDiag, cy - dyDiag)
	    && isLavaPx(cx - dxDiag, cy + dyDiag) && isLavaPx(cx + dxDiag, cy + dyDiag);
}

void DrawCavesLavaBubblesOnTile(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
{
	if (leveltype != DTYPE_CAVES || lightIndex >= LightsMax)
		return;
	if (!InDungeonBounds(tilePosition) || gBubbleCountOnTile[tilePosition.x][tilePosition.y] == 0)
		return;

	EnsureLiveLavaBubbleShades();

	const uint8_t *lightTbl = LightTables[std::clamp(lightIndex, 0, LightsMax - 1)].data();
	const uint8_t crustCol = lightTbl != nullptr ? lightTbl[gLavaCrustIdx] : gLavaCrustIdx;
	const uint8_t bodyCol = lightTbl != nullptr ? lightTbl[gLavaBodyIdx] : gLavaBodyIdx;
	const uint8_t rimCol = lightTbl != nullptr ? lightTbl[gLavaRimIdx] : gLavaRimIdx;
	const uint8_t specCol = lightTbl != nullptr ? lightTbl[gLavaSpecIdx] : gLavaSpecIdx;

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
					const int candOffX = -14 + ((b.seed * 7 + tryIdx * 11) % 29);
					const int candOffY = -6 + ((b.seed * 5 + tryIdx * 7) % 13);
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

		if (!IsSafeLavaFootprint(out, cx, cy, maxRx, maxRy))
			continue;

		if (b.age < b.swellTicks) {
			const int progress256 = (static_cast<int>(b.age) * 256) / std::max<int>(1, b.swellTicks);
			const int ease256 = 256 - (((256 - progress256) * (256 - progress256)) >> 8);
			const int rx = std::max(2, (maxRx * ease256 + 128) >> 8);
			const int ry = std::max(2, (maxRy * ease256 + 128) >> 8);
			const int domeLift = (5 * ease256) >> 8;

			const int rxSq = std::max(1, rx * rx);
			const int rySq = std::max(1, ry * ry);

			for (int dy = -ry - domeLift - 1; dy <= ry + 1; dy++) {
				for (int dx = -rx - 1; dx <= rx + 1; dx++) {
					const int baseNorm256 = (dx * dx * 256) / rxSq + (dy * dy * 256) / rySq;
					if (baseNorm256 >= 196 && baseNorm256 <= 284 && dy >= -1) {
						plotLavaOnly(cx + dx, cy + dy, crustCol);
					}

					const int domeDy = dy + domeLift;
					const int domeNorm256 = (dx * dx * 256) / rxSq + (domeDy * domeDy * 256) / rySq;
					if (domeNorm256 > 256)
						continue;

					const int specDx = dx + std::max(1, rx / 3);
					const int specDy = domeDy + std::max(1, ry / 3);
					const int specNorm256 = (specDx * specDx * 256) / rxSq + (specDy * specDy * 256) / rySq;

					uint8_t col = bodyCol;
					if (domeNorm256 >= 196) {
						col = crustCol;
					} else if (specNorm256 <= 42) {
						col = specCol;
					} else if (specNorm256 <= 115) {
						col = rimCol;
					}
					plotLavaOnly(cx + dx, cy + dy, col);
				}
			}
		} else if (b.age < b.swellTicks + b.burstTicks) {
			const int burstAge = static_cast<int>(b.age - b.swellTicks);
			const int burst256 = (burstAge * 256) / std::max<int>(1, b.burstTicks);
			const int rx = maxRx + ((burst256 * 2) >> 8);
			const int ry = maxRy + ((burst256 * 1) >> 8);
			const int rxSq = std::max(1, rx * rx);
			const int rySq = std::max(1, ry * ry);

			for (int dy = -ry - 1; dy <= ry + 1; dy++) {
				for (int dx = -rx - 1; dx <= rx + 1; dx++) {
					const int norm256 = (dx * dx * 256) / rxSq + (dy * dy * 256) / rySq;
					if (norm256 < 110 || norm256 > 268)
						continue;

					const int angleHash = ((dx * 7 + dy * 13 + static_cast<int>(b.seed)) & 7);
					if (burst256 > 128 && angleHash < 2)
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
			const int rx = maxRx + 2 + ((ripple256 * 2) >> 8);
			const int ry = maxRy + 1 + ((ripple256 * 1) >> 8);
			const int rxSq = std::max(1, rx * rx);
			const int rySq = std::max(1, ry * ry);

			for (int dy = -ry; dy <= ry; dy++) {
				for (int dx = -rx; dx <= rx; dx++) {
					const int norm256 = (dx * dx * 256) / rxSq + (dy * dy * 256) / rySq;
					if (norm256 >= 190 && norm256 <= 256) {
						if (((dx + dy + rippleAge) & 1) == 0) {
							plotLavaOnly(cx + dx, cy + dy, (ripple256 < 128) ? bodyCol : crustCol);
						}
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
		for (int hy = 0; hy < HalfGridH; hy++) {
			const int tx = std::clamp((hx - 1) / 2, 0, MAXDUNX - 1);
			const int ty = std::clamp((hy - 1) / 2, 0, MAXDUNY - 1);
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
		const int tx = std::clamp((hx - 1) / 2, 0, MAXDUNX - 1);
		for (int hy = 0; hy < HalfGridH; hy++) {
			const int ty = std::clamp((hy - 1) / 2, 0, MAXDUNY - 1);
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

		const int dirX = std::clamp(fut.x - pos.x, -1, 1);
		const int dirY = std::clamp(fut.y - pos.y, -1, 1);
		const int pushVx = -dirX * 36 + dirY * 14;
		const int pushVy = -dirY * 36 - dirX * 14;
		const int carve = isMoving ? 58 : (isSwinging ? 52 : 22);

		CarveHalfTileEntityWake(pos, { 0, 0 }, carve, pushVx, pushVy);
		if (isMoving && InDungeonBounds(fut) && IsFogPassableTile(fut)) {
			CarveHalfTileEntityWake(fut, { 0, 0 }, 36, pushVx, pushVy);
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
		const int carve = isMoving ? 46 : 34;

		CarveHalfTileEntityWake(pos, { 0, 0 }, carve, -dirX * 28, -dirY * 28);
	}

	for (const Missile &missile : Missiles) {
		if (missile._miDelFlag)
			continue;

		const Point pos = missile.position.tile;
		const int pushVx = std::clamp(-missile.position.velocity.deltaX / 512, -48, 48);
		const int pushVy = std::clamp(-missile.position.velocity.deltaY / 512, -48, 48);
		CarveHalfTileEntityWake(pos, missile.position.offset, 64, pushVx, pushVy);
	}
}

void DrawGroundFogTileBase(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
{
	if (!InDungeonBounds(tilePosition) || lightIndex >= LightsMax)
		return;

	// 1. Render Caves boiling lava bubbles FIRST so they sit naturally BELOW the ground fog!
	if (leveltype == DTYPE_CAVES) {
		DrawCavesLavaBubblesOnTile(out, tilePosition, targetBufferPosition, lightIndex);
	}

	if (!*sgOptions.Graphics.groundFog || !gLevelHasFog)
		return;

	const int tx = tilePosition.x;
	const int ty = tilePosition.y;
	if (!gFogTilePassable[tx][ty])
		return;

	const LevelFogConfig cfg = GetLevelFogConfig();
	if (!cfg.enabled)
		return;

	EnsureFogNoiseInitialized();
	EnsureFogAlphaLutUpdated();

	const uint32_t ticks = SDL_GetTicks();
	UpdateCachedFogSheets30Hz(ticks, cfg);

	const bool nW = IsFogPassableTile({ tx - 1, ty });
	const bool nE = IsFogPassableTile({ tx + 1, ty });
	const bool nN = IsFogPassableTile({ tx, ty - 1 });
	const bool nS = IsFogPassableTile({ tx, ty + 1 });
	const bool nNW = IsFogPassableTile({ tx - 1, ty - 1 });
	const bool nNE = IsFogPassableTile({ tx + 1, ty - 1 });
	const bool nSW = IsFogPassableTile({ tx - 1, ty + 1 });
	const bool nSE = IsFogPassableTile({ tx + 1, ty + 1 });

	// Evaluate tile fog state at the 4 corners of the tile (4 calls per tile instead of 1,024!)
	int d00 = cfg.maxDensity, wf00 = 256, sx00 = 0, sy00 = 0;
	int d10 = cfg.maxDensity, wf10 = 256, sx10 = 0, sy10 = 0;
	int d01 = cfg.maxDensity, wf01 = 256, sx01 = 0, sy01 = 0;
	int d11 = cfg.maxDensity, wf11 = 256, sx11 = 0, sy11 = 0;

	SampleTileFogState(tilePosition, 16, 16, nW, nE, nN, nS, nNW, nNE, nSW, nSE, d00, wf00, sx00, sy00);
	SampleTileFogState(tilePosition, 240, 16, nW, nE, nN, nS, nNW, nNE, nSW, nSE, d10, wf10, sx10, sy10);
	SampleTileFogState(tilePosition, 16, 240, nW, nE, nN, nS, nNW, nNE, nSW, nSE, d01, wf01, sx01, sy01);
	SampleTileFogState(tilePosition, 240, 240, nW, nE, nN, nS, nNW, nNE, nSW, nSE, d11, wf11, sx11, sy11);

	if ((d00 | d10 | d01 | d11) < 6 || (wf00 | wf10 | wf01 | wf11) == 0)
		return;

	const bool doLight = (lightIndex > 0);
	const int lIdx = std::clamp(lightIndex, 0, LightsMax - 1);

	// Use ShadeFogColor so PAL16_GRAY never snaps to 0 (Pure Black) at lightIndex >= 6 in Hell!
	const uint8_t litCrest = ShadeFogColor(cfg.crestShade, lIdx);
	const uint8_t litCore = ShadeFogColor(cfg.coreShade, lIdx);
	const uint8_t litTrough = ShadeFogColor(cfg.troughShade, lIdx);
	// Darkest charcoal shade (PAL16_GRAY + 14) for Pass 2 drop-shadows so floor never snaps to 0!
	const uint8_t shadowShade = static_cast<uint8_t>(PAL16_GRAY + 14);

	int cLight00 = 256;
	int cLight10 = 256;
	int cLight01 = 256;
	int cLight11 = 256;
	if (doLight && lIdx >= 8) {
		const int vC = LightIndexToVisibility256(lIdx);
		const int vW = GetTileLightVisibility256(tx - 1, ty);
		const int vE = GetTileLightVisibility256(tx + 1, ty);
		const int vN = GetTileLightVisibility256(tx, ty - 1);
		const int vS = GetTileLightVisibility256(tx, ty + 1);
		const int vNW = GetTileLightVisibility256(tx - 1, ty - 1);
		const int vNE = GetTileLightVisibility256(tx + 1, ty - 1);
		const int vSW = GetTileLightVisibility256(tx - 1, ty + 1);
		const int vSE = GetTileLightVisibility256(tx + 1, ty + 1);

		cLight00 = (vC + vW + vN + vNW) >> 2;
		cLight10 = (vC + vE + vN + vNE) >> 2;
		cLight01 = (vC + vW + vS + vSW) >> 2;
		cLight11 = (vC + vE + vS + vSE) >> 2;
	}

	// Fold light visibility into wf00..wf11 once per tile!
	wf00 = (wf00 * cLight00) >> 8;
	wf10 = (wf10 * cLight10) >> 8;
	wf01 = (wf01 * cLight01) >> 8;
	wf11 = (wf11 * cLight11) >> 8;

	// Layer 1 (Low Ground Mist) & Layer 2 (Elevated 3D Cloud Puffs)
	const int uvScale1 = static_cast<int>(cfg.uvScale);
	const int uvScale2 = std::max(28, (uvScale1 * 2) / 3);
	const int baseWorldU1 = tx * uvScale1;
	const int baseWorldV1 = ty * uvScale1;
	const int baseWorldU2 = tx * uvScale2 + 37;
	const int baseWorldV2 = ty * uvScale2 + 73;

	const int floorCutoff = static_cast<int>(cfg.intensityCutoff) + gCachedPulseOffset;
	// Lower canopyCutoff (+22) so Layer 2 3D cloud puffs and their drop-shadows
	// are richly visible in Cathedral and Hell just like in Caves and Catacombs!
	const int canopyCutoff = static_cast<int>(cfg.intensityCutoff) + 22 + gCachedPulseOffset;
	const int floorScaleRecip = (256 * 256) / std::max(64, 255 - floorCutoff);
	const int canopyScaleRecip = (256 * 256) / std::max(64, 255 - canopyCutoff);

	const int topY = targetBufferPosition.y - 31;
	const int leftX = targetBufferPosition.x;
	const int clipW = out.w();
	const int clipH = out.h();

	// Exact 31-scanline non-overlapping isometric diamond partition (dy = 1..31)
	for (int dy = 1; dy < 32; dy++) {
		const int screenY = topY + dy;
		if (screenY < 0 || screenY >= clipH)
			continue;

		const int halfSpan = (dy <= 16) ? (dy * 2) : ((32 - dy) * 2);
		const int xStart = 32 - halfSpan;
		const int xEnd = 32 + halfSpan;
		const int relY = dy - 16;
		const int8_t *bayerRow = Bayer4x4[screenY & 3];

		for (int dx = xStart; dx < xEnd; dx++) {
			const int screenX = leftX + dx;
			if (screenX < 0 || screenX >= clipW)
				continue;

			const int relX = dx - 32;
			const int u256 = std::clamp((relY * 2 - relX) * 4 + 128, 0, 255);
			const int v256 = std::clamp((relY * 2 + relX) * 4 + 128, 0, 255);

			// Fast 4-corner bilinear interpolation of tile density, wall*light fade, and swirl
			const int dTop = (d00 << 8) + (d10 - d00) * u256;
			const int dBot = (d01 << 8) + (d11 - d01) * u256;
			const int localDensity = (dTop + (((dBot - dTop) * v256) >> 8)) >> 8;
			if (localDensity < 6)
				continue;

			const int wfTop = (wf00 << 8) + (wf10 - wf00) * u256;
			const int wfBot = (wf01 << 8) + (wf11 - wf01) * u256;
			const int combinedFade = (wfTop + (((wfBot - wfTop) * v256) >> 8)) >> 8;
			if (combinedFade <= 4)
				continue;

			const int sxTop = (sx00 << 8) + (sx10 - sx00) * u256;
			const int sxBot = (sx01 << 8) + (sx11 - sx01) * u256;
			const int swirlX = (sxTop + (((sxBot - sxTop) * v256) >> 8)) >> 8;

			const int syTop = (sy00 << 8) + (sy10 - sy00) * u256;
			const int syBot = (sy01 << 8) + (sy11 - sy01) * u256;
			const int swirlY = (syTop + (((syBot - syTop) * v256) >> 8)) >> 8;

			// ================================================================
			// 1. Layer 1: Low Ground-Hugging Mist Bed
			// ================================================================
			const int u1Fixed = (baseWorldU1 << 8) + u256 * uvScale1 + (swirlX << 5);
			const int v1Fixed = (baseWorldV1 << 8) + v256 * uvScale1 + (swirlY << 5);
			const int rawDensity1 = SampleLutBilinear(gCachedFloorFog, u1Fixed, v1Fixed);

			// ================================================================
			// 2. Layer 2: Elevated 3D Cloud Puffs + Offset Cast Drop-Shadow
			// ================================================================
			const int u2Fixed = (baseWorldU2 << 8) + u256 * uvScale2 + (swirlX << 4);
			const int v2Fixed = (baseWorldV2 << 8) + v256 * uvScale2 + (swirlY << 4);
			const int rawDensity2 = SampleLutBilinear(gCachedCanopyFog, u2Fixed, v2Fixed);

			// Direct O(1) lookup for shadow & 3D slope (saving 2 bilinear calls per pixel!)
			const int u2Idx = (u2Fixed >> 8) & 0xFF;
			const int v2Idx = (v2Fixed >> 8) & 0xFF;
			const int rawShadow2 = gCachedCanopyFog[(v2Idx + 6) & 0xFF][(u2Idx + 6) & 0xFF];

			uint8_t &dst = out[{ screenX, screenY }];
			uint8_t outPx = dst;
			const int dither = bayerRow[screenX & 3];

			// Pass 1: Composite Layer 1 (Low Ground Mist Bed)
			if (rawDensity1 > floorCutoff) {
				const int norm1 = ((rawDensity1 - floorCutoff) * floorScaleRecip) >> 8;
				const int smooth1 = (norm1 * (384 - norm1)) >> 8;
				const int alpha1 = (((smooth1 * localDensity) >> 8) * combinedFade >> 8) + dither;
				if (alpha1 > 14) {
					const uint8_t shade1 = (norm1 > 165) ? litCore : gFogAlphaLut[3][litTrough][litCore];
					outPx = ApplySoftFogBlend(shade1, outPx, alpha1, false);
				}
			}

			// Pass 2: Composite Layer 2 Cast Drop-Shadow (darkens both bare floor and Layer 1 mist!)
			const int canopyDensity = (localDensity + static_cast<int>(cfg.maxDensity)) >> 1;
			if (rawShadow2 > canopyCutoff && rawShadow2 > rawDensity2 - 12) {
				const int normS = ((rawShadow2 - canopyCutoff) * canopyScaleRecip) >> 8;
				const int shadowAlpha = (((normS * canopyDensity) >> 8) * combinedFade >> 8) + dither;
				if (shadowAlpha > 96) {
					outPx = gFogAlphaLut[2][outPx][shadowShade]; // 18.75% charcoal shadow
				} else if (shadowAlpha > 48) {
					outPx = gFogAlphaLut[1][outPx][shadowShade]; // 12.5% charcoal shadow
				} else if (shadowAlpha > 18) {
					outPx = gFogAlphaLut[0][outPx][shadowShade]; // 6.25% charcoal shadow
				}
			}

			// Pass 3: Composite Layer 2 (Elevated 3D Cloud Puffs with Top-Left Dome Highlights)
			if (rawDensity2 > canopyCutoff) {
				const int norm2 = ((rawDensity2 - canopyCutoff) * canopyScaleRecip) >> 8;
				const int smooth2 = (norm2 * (384 - norm2)) >> 8;
				const int alpha2 = ((((smooth2 * canopyDensity) >> 8) * combinedFade) >> 8) + 28 + dither;
				if (alpha2 > 14) {
					const int slope = gCachedCanopySlope[v2Idx][u2Idx];
					uint8_t shade2;
					if (slope > 6 || norm2 > 140) {
						shade2 = litCrest;
					} else if (slope >= -8) {
						shade2 = litCore;
					} else {
						shade2 = litTrough;
					}
					outPx = ApplySoftFogBlend(shade2, outPx, alpha2, cfg.allowHeavyBlend);
				}
			}

			dst = outPx;
		}
	}
}

void DrawGroundFogEntitySubmersion(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex)
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

	const int hx = tx * 2 + 1;
	const int hy = ty * 2 + 1;
	const int neighborDensity = std::max({
	    static_cast<int>(gFogHalfGrid[hx - 1][hy].density),
	    static_cast<int>(gFogHalfGrid[hx + 1][hy].density),
	    static_cast<int>(gFogHalfGrid[hx][hy - 1].density),
	    static_cast<int>(gFogHalfGrid[hx][hy + 1].density),
	});
	if (neighborDensity < 40)
		return;

	EnsureFogNoiseInitialized();
	EnsureFogAlphaLutUpdated();

	const uint32_t ticks = SDL_GetTicks();
	UpdateCachedFogSheets30Hz(ticks, cfg);

	const int lIdx = std::clamp(lightIndex, 0, LightsMax - 1);

	const uint8_t litCrest = ShadeFogColor(cfg.crestShade, lIdx);
	const uint8_t litCore = ShadeFogColor(cfg.coreShade, lIdx);

	const int uvScale2 = std::max(28, (static_cast<int>(cfg.uvScale) * 2) / 3);
	const int baseWorldU = tx * uvScale2 + 37;
	const int baseWorldV = ty * uvScale2 + 73;

	const int footCenterX = targetBufferPosition.x + 32;
	const int footCenterY = targetBufferPosition.y - 12;
	constexpr int Rx = 14;
	constexpr int Ry = 8;

	const int clipW = out.w();
	const int clipH = out.h();

	for (int dy = -Ry; dy <= Ry; dy++) {
		const int screenY = footCenterY + dy;
		if (screenY < 0 || screenY >= clipH)
			continue;

		const int dyNorm256 = (dy * dy * 256) / (Ry * Ry);
		for (int dx = -Rx; dx <= Rx; dx++) {
			const int screenX = footCenterX + dx;
			if (screenX < 0 || screenX >= clipW)
				continue;

			const int d2 = (dx * dx * 256) / (Rx * Rx) + dyNorm256;
			if (d2 >= 256)
				continue;

			const int u256 = std::clamp(((dy + 4) * 2 - dx) * 4 + 128, 0, 255);
			const int v256 = std::clamp(((dy + 4) * 2 + dx) * 4 + 128, 0, 255);

			const int uFixed = (baseWorldU << 8) + u256 * uvScale2;
			const int vFixed = (baseWorldV << 8) + v256 * uvScale2;
			const int rawDensity = SampleLutBilinear(gCachedCanopyFog, uFixed, vFixed);
			if (rawDensity <= cfg.intensityCutoff + 20)
				continue;

			uint8_t &dst = out[{ screenX, screenY }];
			if (d2 < 96 && rawDensity > 175) {
				dst = gFogAlphaLut[2][dst][litCrest];
			} else if (d2 < 180) {
				dst = gFogAlphaLut[1][dst][litCore];
			} else {
				dst = gFogAlphaLut[0][dst][litCore];
			}
		}
	}
}

} // namespace devilution
