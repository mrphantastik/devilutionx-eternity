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
std::array<std::array<uint8_t, 256>, 256> gFogNoiseLutB;
std::array<std::array<uint8_t, 256>, 256> gFogCanopyNoiseLut;
std::array<std::array<uint8_t, 256>, 256> gFogCanopyNoiseLutB;
std::array<std::array<uint8_t, 256>, 256> gFogWarpLut;
std::array<uint8_t, 256> gFogFloorCurveLut;
std::array<uint8_t, 256> gFogCanopyCurveLut;
std::array<uint8_t, 15> gCrestByLight;
std::array<uint8_t, 15> gCoreByLight;
std::array<uint8_t, 15> gTroughByLight;
std::array<uint8_t, 15> gLowerDomeByLight;

uint32_t gLastFogSheetUpdateTick = 0;
uint8_t gFogSheetSliceIdx = 0;
dungeon_type gLastFogSheetDtype = DTYPE_NONE;
int gCachedPulseOffset = 0;
std::array<std::array<uint8_t, 256>, 256> gCachedFloorFog;
std::array<std::array<uint8_t, 256>, 256> gCachedFloorFogB;
std::array<std::array<uint8_t, 256>, 256> gCachedCanopyFog;
std::array<std::array<uint8_t, 256>, 256> gCachedCanopyFogB;

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
	switch (leveltype) {
	case DTYPE_CATHEDRAL:
		return {
			/*enabled=*/true,
			/*spawnChancePercent=*/100,
			/*maxDensity=*/232,
			/*intensityCutoff=*/98,
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
			/*intensityCutoff=*/98,
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
			/*intensityCutoff=*/94,
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
			/*intensityCutoff=*/94,
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
			/*intensityCutoff=*/98,
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
			/*intensityCutoff=*/92,
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

	for (int l = 0; l < 15; l++) {
		gCrestByLight[l] = ShadeFogColor(static_cast<uint8_t>(PAL16_GRAY + 3), l);
		gCoreByLight[l] = ShadeFogColor(static_cast<uint8_t>(PAL16_GRAY + 6), l);
		gTroughByLight[l] = ShadeFogColor(static_cast<uint8_t>(PAL16_GRAY + 10), l);
		gLowerDomeByLight[l] = Blend50(gTroughByLight[l], gCoreByLight[l]);
	}

	for (int i = 0; i < 256; i++) {
		const float c = static_cast<float>(i) / 255.0F;
		const float n1 = std::clamp((c - 0.415F) * 5.20F, 0.0F, 1.0F);
		const float m1 = n1 * n1 * (3.0F - 2.0F * n1);
		gFogFloorCurveLut[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(m1 * 255.0F + 0.5F), 0, 255));

		const float n2 = std::clamp((c - 0.398F) * 4.90F, 0.0F, 1.0F);
		const float m2 = n2 * n2 * (3.0F - 2.0F * n2);
		gFogCanopyCurveLut[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(m2 * 255.0F + 0.5F), 0, 255));
	}

	for (int y = 0; y < 256; y++) {
		const float fy = static_cast<float>(y);
		for (int x = 0; x < 256; x++) {
			const float fx = static_cast<float>(x);

			const float wxA = ValueNoiseTileableFloat(fx, fy, 9, 901U);
			const float wyA = ValueNoiseTileableFloat(fx, fy, 11, 902U);
			const float wxB = ValueNoiseTileableFloat(fx, fy, 7, 911U);
			const float wyB = ValueNoiseTileableFloat(fx, fy, 9, 912U);
			gFogWarpLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(wxA * 255.0F + 0.5F), 0, 255));

			const float mxA = fx + (wxA - 0.5F) * 24.0F;
			const float myA = fy + (wyA - 0.5F) * 24.0F;
			const float mA = std::clamp((ValueNoiseTileableFloat(mxA, myA, 3, 771U) * 0.60F
			                                + ValueNoiseTileableFloat(mxA, myA, 7, 772U) * 0.40F - 0.28F)
			        * 2.0F,
			    0.0F, 1.0F);
			const float envA = 0.82F + 0.18F * mA;

			const float mxB = fx - (wyB - 0.5F) * 26.0F;
			const float myB = fy + (wxB - 0.5F) * 26.0F;
			const float mB = std::clamp((ValueNoiseTileableFloat(mxB, myB, 3, 781U) * 0.62F
			                                + ValueNoiseTileableFloat(mxB, myB, 5, 782U) * 0.38F - 0.28F)
			        * 2.0F,
			    0.0F, 1.0F);
			const float envB = 0.80F + 0.20F * mB;

			// 1A. Layer 1 Sheet A (pre-scaled to 0..144 so a1 + b1 requires zero runtime multiplications)
			const float sx1A = fx + (wxA - 0.5F) * 18.0F;
			const float sy1A = fy + (wyA - 0.5F) * 18.0F;
			const float n0A = ValueNoiseTileableFloat(sx1A, sy1A, 5, 101U);
			const float n1A = ValueNoiseTileableFloat(sx1A, sy1A, 11, 202U);
			const float n2A = ValueNoiseTileableFloat(sx1A, sy1A, 21, 303U);
			const float n3A = ValueNoiseTileableFloat(sx1A, sy1A, 37, 404U);
			const float fbm1A = n0A * 0.38F + n1A * 0.32F + n2A * 0.20F + n3A * 0.10F;
			const float r1A = 1.0F - std::fabs(n1A * 2.0F - 1.0F);
			const float rs1A = r1A * r1A * (3.0F - 2.0F * r1A);
			const float raw1A = (fbm1A * 0.72F + rs1A * 0.28F) * envA;
			gFogNoiseLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(raw1A * 144.0F + 0.5F), 0, 144));

			// 1B. Layer 1 Sheet B (pre-scaled to 0..111 so a1 + b1 is directly in 0..255)
			const float sx1B = fx - (wyB - 0.5F) * 20.0F;
			const float sy1B = fy + (wxB - 0.5F) * 20.0F;
			const float n0B = ValueNoiseTileableFloat(sx1B, sy1B, 3, 151U);
			const float n1B = ValueNoiseTileableFloat(sx1B, sy1B, 7, 252U);
			const float n2B = ValueNoiseTileableFloat(sx1B, sy1B, 15, 353U);
			const float n3B = ValueNoiseTileableFloat(sx1B, sy1B, 29, 454U);
			const float fbm1B = n0B * 0.42F + n1B * 0.30F + n2B * 0.18F + n3B * 0.10F;
			const float r1B = 1.0F - std::fabs(n1B * 2.0F - 1.0F);
			const float rs1B = r1B * r1B * (3.0F - 2.0F * r1B);
			const float raw1B = (fbm1B * 0.74F + rs1B * 0.26F) * envB;
			gFogNoiseLutB[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(raw1B * 111.0F + 0.5F), 0, 111));

			// 2A. Layer 2 Sheet A (pre-scaled to 0..144)
			const float sx2A = fx + (wyA - 0.5F) * 20.0F;
			const float sy2A = fy - (wxA - 0.5F) * 20.0F;
			const float c0A = ValueNoiseTileableFloat(sx2A, sy2A, 4, 505U);
			const float c1A = ValueNoiseTileableFloat(sx2A, sy2A, 9, 606U);
			const float c2A = ValueNoiseTileableFloat(sx2A, sy2A, 17, 707U);
			const float c3A = ValueNoiseTileableFloat(sx2A, sy2A, 31, 808U);
			const float raw2A = (c0A * 0.44F + c1A * 0.32F + c2A * 0.16F + c3A * 0.08F) * envA;
			gFogCanopyNoiseLut[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(raw2A * 144.0F + 0.5F), 0, 144));

			// 2B. Layer 2 Sheet B (pre-scaled to 0..111)
			const float sx2B = fx - (wxB - 0.5F) * 22.0F;
			const float sy2B = fy - (wyB - 0.5F) * 22.0F;
			const float c0B = ValueNoiseTileableFloat(sx2B, sy2B, 3, 555U);
			const float c1B = ValueNoiseTileableFloat(sx2B, sy2B, 6, 656U);
			const float c2B = ValueNoiseTileableFloat(sx2B, sy2B, 13, 757U);
			const float c3B = ValueNoiseTileableFloat(sx2B, sy2B, 25, 858U);
			const float raw2B = (c0B * 0.46F + c1B * 0.30F + c2B * 0.16F + c3B * 0.08F) * envB;
			gFogCanopyNoiseLutB[y][x] = static_cast<uint8_t>(std::clamp(static_cast<int>(raw2B * 111.0F + 0.5F), 0, 111));
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
 * Updates the dual world-locked fog sheets (Sheet A and rotated-axis Sheet B) in 8 interleaved 32-row slices.
 * Uses direct integer reads on the smooth low-frequency warp LUT to cut per-frame CPU overhead by 3.5x.
 */
void UpdateCachedFogSheets30Hz(uint32_t ticks, const LevelFogConfig &cfg)
{
	const bool forceFullRebuild = (gLastFogSheetDtype != leveltype);
	if (!forceFullRebuild && (ticks - gLastFogSheetUpdateTick) < 8U)
		return;

	gLastFogSheetUpdateTick = ticks;
	gLastFogSheetDtype = leveltype;

	const int32_t sTicks = static_cast<int32_t>(ticks);
	// Step 1: 40% slower screen-space travel speeds, scaled to the 11.1-tile and 15.1-tile world sheets
	const int driftX1A = (sTicks * 256) / ((cfg.driftDivX * 100) / 121);
	const int driftY1A = (sTicks * 256) / ((cfg.driftDivY * 100) / 60);
	const int driftX1B = (sTicks * 256) / ((cfg.driftDivX * 100) / 89);
	const int driftY1B = -(sTicks * 256) / ((cfg.driftDivY * 100) / 52);

	const int driftX2A = -((sTicks * 256) / ((cfg.driftDivX * 100) / 199));
	const int driftY2A = (sTicks * 256) / ((cfg.driftDivY * 100) / 127);
	const int driftX2B = -((sTicks * 256) / ((cfg.driftDivX * 100) / 145));
	const int driftY2B = -(sTicks * 256) / ((cfg.driftDivY * 100) / 96);

	// Step 1: 40% slower counter-rotating internal swirl velocities
	const int warpDriftX1 = -(sTicks * 256) / 280;
	const int warpDriftY1 = (sTicks * 256) / 360;
	const int warpDriftX2 = (sTicks * 256) / 245;
	const int warpDriftY2 = -(sTicks * 256) / 310;

	if (cfg.hasPulse) {
		const int phase = (sTicks / 153) & 0xFF;
		const int triWave = (phase < 128) ? phase : (255 - phase);
		gCachedPulseOffset = (triWave >> 2) - 16;
	} else {
		gCachedPulseOffset = 0;
	}

	const int yStart = forceFullRebuild ? 0 : (static_cast<int>(gFogSheetSliceIdx) * 32);
	const int yEnd = forceFullRebuild ? 256 : (yStart + 32);
	gFogSheetSliceIdx = (gFogSheetSliceIdx + 1U) & 7U;

	const int wOffX0 = (warpDriftX1 >> 8);
	const int wOffY0 = (warpDriftY1 >> 8);
	const int wOffX1 = 41 + (warpDriftY1 >> 8);
	const int wOffY1 = 73 - (warpDriftX1 >> 8);
	const int wOffX2 = 59 + (warpDriftX2 >> 8);
	const int wOffY2 = 131 + (warpDriftY2 >> 8);

	for (int y = yStart; y < yEnd; y++) {
		const int baseV = (y << 8);
		const uint8_t *warpRow0 = gFogWarpLut[(y + wOffY0) & 0xFF].data();
		const uint8_t *warpRow1 = gFogWarpLut[(y + wOffY1) & 0xFF].data();
		const uint8_t *warpRow2 = gFogWarpLut[(y + wOffY2) & 0xFF].data();

		for (int x = 0; x < 256; x++) {
			const int baseU = (x << 8);
			const int wA = static_cast<int>(warpRow0[(x + wOffX0) & 0xFF]) - 128;
			const int wB = static_cast<int>(warpRow1[(x + wOffX1) & 0xFF]) - 128;
			const int wC = static_cast<int>(warpRow2[(x + wOffX2) & 0xFF]) - 128;

			// 1. Layer 1 Dual Sheets (A & B swirl in counter-rotating directions)
			const int warpX1A = wA * 10 + wC * 6;
			const int warpY1A = wB * 8 - wC * 5;
			gCachedFloorFog[y][x] = static_cast<uint8_t>(SampleLutBilinear(gFogNoiseLut, baseU + warpX1A + driftX1A, baseV + warpY1A + driftY1A));

			const int warpX1B = -wB * 9 + wA * 5;
			const int warpY1B = -wC * 8 - wA * 5;
			gCachedFloorFogB[y][x] = static_cast<uint8_t>(SampleLutBilinear(gFogNoiseLutB, baseU + warpX1B + driftX1B + (67 << 8), baseV + warpY1B + driftY1B + (149 << 8)));

			// 2. Layer 2 Dual Sheets (Elevated 3D Cloud Puffs A & B)
			const int warpX2A = -wB * 11 + wC * 6;
			const int warpY2A = wA * 9 + wC * 5;
			gCachedCanopyFog[y][x] = static_cast<uint8_t>(SampleLutBilinear(gFogCanopyNoiseLut, baseU + warpX2A + driftX2A + (137 << 8), baseV + warpY2A + driftY2A + (89 << 8)));

			const int warpX2B = wC * 10 - wA * 6;
			const int warpY2B = -wB * 8 + wA * 5;
			gCachedCanopyFogB[y][x] = static_cast<uint8_t>(SampleLutBilinear(gFogCanopyNoiseLutB, baseU + warpX2B + driftX2B + (193 << 8), baseV + warpY2B + driftY2B + (53 << 8)));
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

	// Average entity-wake swirl offset across the tile (scaled to 24.8 fixed-point for the 11.1-tile sheet)
	const int swirlFixedX = ((sx00 + sx10 + sx01 + sx11) * 12);
	const int swirlFixedY = ((sy00 + sy10 + sy01 + sy11) * 12);

	// Dual incommensurate coprime UV scales (eliminates the 4x4-tile repeating clone stamp):
	//   Layer 1 Sheet A: UvScale1A = 23 (~11.13x11.13 tiles per 256-wrap)
	//   Layer 1 Sheet B: Rotated axes (15, 8) (~15.06x15.06 tiles per 256-wrap, rotated 28 deg)
	//   Layer 2 Sheet A: uvScale2A = 19 (~13.47x13.47 tiles per 256-wrap)
	//   Layer 2 Sheet B: Rotated axes (12, 7) (~18.42x18.42 tiles per 256-wrap, rotated 30 deg)
	constexpr int UvScale1A = 23;
	constexpr int UvScale1BMain = 15;
	constexpr int UvScale1BCross = 8;
	const int uvScale2A = std::max(15, (static_cast<int>(cfg.uvScale) * 19) / 44);
	constexpr int UvScale2BMain = 12;
	constexpr int UvScale2BCross = 7;
	const int shadowOff2A = 29 * uvScale2A;
	constexpr int ShadowOff2BU = 29 * (UvScale2BMain + UvScale2BCross);
	constexpr int ShadowOff2BV = 29 * (UvScale2BMain - UvScale2BCross);

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

		// Seamless world-space isometric coordinates (worldU, worldV in 24.8 tile units):
		// Each dx++ step advances worldU by +4 and worldV by -4.
		const int worldU = (tx << 8) + uLeft;
		const int worldV = (ty << 8) + vLeft;

		int u1AFixed = worldU * UvScale1A + swirlFixedX;
		int v1AFixed = worldV * UvScale1A + swirlFixedY;
		constexpr int U1AStep = 4 * UvScale1A;
		constexpr int V1AStep = -4 * UvScale1A;

		int u1BFixed = worldU * UvScale1BMain + worldV * UvScale1BCross - swirlFixedY;
		int v1BFixed = worldV * UvScale1BMain - worldU * UvScale1BCross + swirlFixedX;
		constexpr int U1BStep = 4 * (UvScale1BMain - UvScale1BCross);
		constexpr int V1BStep = -4 * (UvScale1BMain + UvScale1BCross);

		int u2AFixed = worldU * uvScale2A + (swirlFixedX >> 1);
		int v2AFixed = worldV * uvScale2A + (swirlFixedY >> 1);
		const int u2AStep = 4 * uvScale2A;
		const int v2AStep = -4 * uvScale2A;

		int u2BFixed = worldU * UvScale2BMain + worldV * UvScale2BCross - (swirlFixedY >> 1);
		int v2BFixed = worldV * UvScale2BMain - worldU * UvScale2BCross + (swirlFixedX >> 1);
		constexpr int U2BStep = 4 * (UvScale2BMain - UvScale2BCross);
		constexpr int V2BStep = -4 * (UvScale2BMain + UvScale2BCross);

		const int8_t *bayerRow = Bayer4x4[screenY & 3];
		uint8_t *dstRow = &out[{ 0, screenY }];

		for (int dx = xStart; dx < xEnd; dx++, wFixed += wStep, lFixed += lStep,
		         u1AFixed += U1AStep, v1AFixed += V1AStep,
		         u1BFixed += U1BStep, v1BFixed += V1BStep,
		         u2AFixed += u2AStep, v2AFixed += v2AStep,
		         u2BFixed += U2BStep, v2BFixed += V2BStep) {
			const int screenX = leftX + dx;
			if (screenX < 0 || screenX >= clipW)
				continue;

			const int weight = wFixed >> 8;
			if (weight <= 6)
				continue;

			// Fast 1-tap coarse bound check (skips ~55% bare-floor pixels with zero bilinear lerps or multiplications)
			const int u1A0 = (u1AFixed >> 8) & 0xFF;
			const int v1A0 = (v1AFixed >> 8) & 0xFF;
			const int u1B0 = (u1BFixed >> 8) & 0xFF;
			const int v1B0 = (v1BFixed >> 8) & 0xFF;
			const int coarse1 = gCachedFloorFog[v1A0][u1A0] + gCachedFloorFogB[v1B0][u1B0];

			const int u2A0 = (u2AFixed >> 8) & 0xFF;
			const int v2A0 = (v2AFixed >> 8) & 0xFF;
			const int u2B0 = (u2BFixed >> 8) & 0xFF;
			const int v2B0 = (v2BFixed >> 8) & 0xFF;
			const int coarse2 = gCachedCanopyFog[v2A0][u2A0] + gCachedCanopyFogB[v2B0][u2B0];

			const int su2A0 = ((u2AFixed - shadowOff2A) >> 8) & 0xFF;
			const int sv2A0 = ((v2AFixed - shadowOff2A) >> 8) & 0xFF;
			const int su2B0 = ((u2BFixed - ShadowOff2BU) >> 8) & 0xFF;
			const int sv2B0 = ((v2BFixed - ShadowOff2BV) >> 8) & 0xFF;
			const int coarseS2 = gCachedCanopyFog[sv2A0][su2A0] + gCachedCanopyFogB[sv2B0][su2B0];

			const bool hasL1 = (gFogFloorCurveLut[std::min(255, coarse1 + 8)] > floorCutoff);
			const bool hasL2 = (gFogCanopyCurveLut[std::min(255, coarse2 + 8)] > canopyCutoff);
			const bool hasS2 = (gFogCanopyCurveLut[std::min(255, coarseS2 + 8)] > canopyCutoff);
			if (!hasL1 && !hasL2 && !hasS2)
				continue;

			uint8_t outPx = dstRow[screenX];
			// Do not apply static paletteTransparencyLookup over animated Caves lava pixels (1..31)
			if (leveltype == DTYPE_CAVES && outPx >= 1 && outPx <= 31)
				continue;

			int rawDensity1 = 0;
			if (hasL1) {
				const int u1A1 = (u1A0 + ((u1AFixed >> 7) & 1)) & 0xFF;
				const int v1A1 = (v1A0 + ((v1AFixed >> 7) & 1)) & 0xFF;
				const int u1B1 = (u1B0 + ((u1BFixed >> 7) & 1)) & 0xFF;
				const int v1B1 = (v1B0 + ((v1BFixed >> 7) & 1)) & 0xFF;
				const int fine1 = (coarse1 + gCachedFloorFog[v1A1][u1A1] + gCachedFloorFogB[v1B1][u1B1]) >> 1;
				rawDensity1 = gFogFloorCurveLut[fine1];
			}

			int rawDensity2 = 0;
			if (hasL2) {
				const int sum2 = SampleLutBilinear(gCachedCanopyFog, u2AFixed, v2AFixed)
				    + SampleLutBilinear(gCachedCanopyFogB, u2BFixed, v2BFixed);
				rawDensity2 = gFogCanopyCurveLut[std::min(255, sum2)];
			}

			int rawShadow2 = 0;
			if (hasS2) {
				const int su2A1 = (su2A0 + (((u2AFixed - shadowOff2A) >> 7) & 1)) & 0xFF;
				const int sv2A1 = (sv2A0 + (((v2AFixed - shadowOff2A) >> 7) & 1)) & 0xFF;
				const int su2B1 = (su2B0 + (((u2BFixed - ShadowOff2BU) >> 7) & 1)) & 0xFF;
				const int sv2B1 = (sv2B0 + (((v2BFixed - ShadowOff2BV) >> 7) & 1)) & 0xFF;
				const int fineS2 = (coarseS2 + gCachedCanopyFog[sv2A1][su2A1] + gCachedCanopyFogB[sv2B1][su2B1]) >> 1;
				rawShadow2 = gFogCanopyCurveLut[fineS2];
			}

			if (rawDensity1 <= floorCutoff && rawDensity2 <= canopyCutoff && rawShadow2 <= canopyCutoff)
				continue;

			const int dither = bayerRow[screenX & 3];
			const int smoothLightIdx = std::clamp(((lFixed >> 8) + dither * 8) >> 8, 0, 14);
			const uint8_t litCrest = gCrestByLight[smoothLightIdx];
			const uint8_t litCore = gCoreByLight[smoothLightIdx];
			const uint8_t litTrough = gTroughByLight[smoothLightIdx];
			const uint8_t litLowerDome = gLowerDomeByLight[smoothLightIdx];

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
					const int slope = (rawDensity2 - rawShadow2) >> 2;
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
