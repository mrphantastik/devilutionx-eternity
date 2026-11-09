/**
 * @file weather.cpp
 *
 * Implementation of Tristram weather & environmental particle effects:
 * - Dynamic Light Rain & Heavy Rain states (+35% raindrop intensity, persisted per game session
 *   and only re-rolled when returning to Tristram after >= 1 minute in a dungeon)
 * - Outdoor Wet Ground & Glossy Surface Reflections (`DrawWetGroundGloss`):
 *   - Seamless world-space multi-scale topographic wetness field across walkable outdoor tiles
 *   - Shallow sky-reflecting water puddle basins with damp darkened rims and animated wind-driven
 *     capillary wave specular glints
 *   - Rain-slicked cobblestone/stone micro-specular highlights (`logical_palette` luminance-aware)
 *     with player Fresnel proximity glint and electric sky reflection during lightning flashes
 * - Localized 5-stage isometric puddle splash animations on walkable outdoor ground tiles
 *   (-35% frequency, 50% smaller average radius, and +50% transparency via 50%/25%/12.5% palette
 *   blending), rendered in DrawCell() after mt[0..1] and before mt[2..15]
 * - Precipitation / Steam Fog:
 *   1) Non-overlapping world-space drifting 4-puff steam fog banks (-30% count, +20% size,
 *      +35% transparency via 12.5%/6.25% contiguous LUT blending)
 *   2) Curling rising ground-evaporation steam wisps (-35% size, +50% transparency via
 *      12.5%/6.25% contiguous LUT blending)
 * - Camera-compensated multi-layer falling raindrop streaks with 75%/50%/25% palette blending
 * - Intense full-screen distant lightning palette flashes with multi-strike flicker (every 30-60s)
 * - Speed-of-sound delayed thunder claps timed to lightning strikes:
 *   sfx\misc\thundera.wav (distant, Light Rain) and sfx\misc\thunderb.wav (closer, Heavy Rain)
 * - Seamless looping playback of sfx\misc\RainLightLoop.wav and sfx\misc\RainHeavyLoop.wav
 *   that stops cleanly when entering dungeons or quitting to the main menu
 */
#include "engine/render/weather.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <SDL.h>

#include "diablo.h"
#include "effects.h"
#include "engine/palette.h"
#include "engine/random.hpp"
#include "levels/gendung.h"
#include "options.h"
#include "player.h"
#include "utils/stdcompat/algorithm.hpp"

namespace devilution {

namespace {

// Multi-plane 3D raindrop counts for Light Rain and Heavy Rain
constexpr size_t MaxRainDrops = 448;
constexpr size_t LightRainDropCount = 160;
constexpr size_t HeavyRainDropCount = 420;

// Steam Fog Banks: amount reduced by 30% (13 -> 9 in Light Rain, 20 -> 14 in Heavy Rain),
// arranged on a non-overlapping 5x3 world-space lattice (1500 x 900 px, 300 x 300 px per cell)
constexpr size_t MaxSteamFogBanks = 14;
constexpr size_t LightRainFogBankCount = 9;
constexpr size_t HeavyRainFogBankCount = 14;
constexpr int FogWorldWrapW = 1500;
constexpr int FogWorldWrapH = 900;

// Minimum time player must spend in a dungeon before Tristram rain intensity can change (1 minute)
constexpr uint32_t MinDungeonTimeForWeatherChangeMs = 60000;

struct RainDrop {
	float x = 0.0F;
	float y = 0.0F;
	float speedScale = 1.0F;
	float parallaxScale = 1.0F; // 0.72 (Layer 0 far), 1.00 (Layer 1 mid), 1.38 (Layer 2 near camera)
	uint8_t length = 12;
	uint8_t layer = 0;          // 0 = far/occluded, 1 = mid, 2 = near/DoF blurred
	uint8_t colorIndex = PAL16_BLUE + 4;
};

struct SteamFogBank {
	float worldX = 0.0F;
	float worldY = 0.0F;
	float phase = 0.0F;
	float phaseSpeed = 0.50F;
	int16_t rx = 68; // +20% size
	int16_t ry = 25; // +20% size
	int8_t puffOffX[4] = { -32, -10, 14, 34 };
	int8_t puffOffY[4] = { 5, -5, 4, -4 };
	uint8_t puffScalePct[4] = { 75, 100, 90, 70 };
	uint8_t colorIndex = PAL16_GRAY + 6;
};

struct DrawnFogRect {
	int minX;
	int maxX;
	int minY;
	int maxY;
};

bool gWeatherInitialized = false;
bool gHasGameRainRoll = false;
RainMode gCurrentRainMode = RainMode::Light;
float gHeavyBlend = 0.0F; // 0.0 = Light Rain, 1.0 = Heavy Rain
uint32_t gLastWeatherTick = 0;

// Dungeon time tracking (weather intensity only changes after >= 60s in a dungeon)
bool gInDungeon = false;
uint32_t gDungeonEnterTick = 0;
uint32_t gAccumulatedDungeonMs = 0;

// Distant lightning palette flash state
bool gLightningFlashActiveOnPalette = false;
uint8_t gActiveAppliedStrength = 0;
uint32_t gFlashStartTick = 0;
uint32_t gFlashPeakEndTick = 0;
uint32_t gFlashTotalEndTick = 0;
uint8_t gFlashPeakStrength = 0;
uint32_t gNextLightningTick = 0;
uint32_t gSecondaryFlashTick = 0;
uint8_t gPendingFlashStrength = 0;

// Speed-of-sound delayed thunder audio state
uint32_t gPendingThunderTick = 0;
_sfx_id gPendingThunderSfx = SFX_NONE;
int gPendingThunderVolume = 0;

// Exact camera world-space pixel position (top-left of `out` surface in world pixels)
bool gHasPrevCamPos = false;
Point gCamWorldPixel { 0, 0 };
int gLastViewportW = 0;
int gLastViewportH = 0;

std::array<RainDrop, MaxRainDrops> gRainDrops;
std::array<SteamFogBank, MaxSteamFogBanks> gSteamFogBanks;

// Screen-space mask for Layer 0 (Distant Background Rain):
// Populated in DrawWeatherBackground() and composited onto walkable ground tiles
// inside DrawWeatherGroundTile() (after floor microtiles mt[0..1] and before roofs/walls
// mt[2..15], solid tiles, trees, NPCs, and players) so DrawCell() never overwrites Layer 0
// while roofs, walls, trees, rocks, and characters naturally paint over and occlude it.
std::vector<uint16_t> gBgRainMask;
std::vector<uint8_t> gBgRainBlockGrid;
int gBgRainMaskW = 0;
int gBgRainMaskH = 0;
int gBgRainBlockCols = 0;
int gBgRainBlockRows = 0;

inline float GetLayerBaseSlant(uint8_t layer, float heavyBlend)
{
	if (layer == 0)
		return -(0.12F + 0.05F * heavyBlend); // Distant background: more vertical
	if (layer == 1)
		return -(0.21F + 0.08F * heavyBlend); // Midground: standard wind angle
	return -(0.31F + 0.10F * heavyBlend);     // Near-camera lens: steeper perspective slant
}

inline uint32_t FastHash(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

inline int PositiveMod(int value, int mod)
{
	const int r = value % mod;
	return r < 0 ? r + mod : r;
}

inline uint8_t Blend50(uint8_t fg, uint8_t bg)
{
	return paletteTransparencyLookup[fg][bg];
}

inline uint8_t Blend75(uint8_t fg, uint8_t bg)
{
	const uint8_t mid = paletteTransparencyLookup[fg][bg];
	return paletteTransparencyLookup[fg][mid];
}

inline uint8_t Blend25(uint8_t fg, uint8_t bg)
{
	const uint8_t mid = paletteTransparencyLookup[fg][bg];
	return paletteTransparencyLookup[bg][mid];
}

inline uint8_t Blend37(uint8_t fg, uint8_t bg)
{
	const uint8_t half = paletteTransparencyLookup[fg][bg];
	const uint8_t qtr = paletteTransparencyLookup[bg][half];
	return paletteTransparencyLookup[half][qtr]; // 37.5% fg, 62.5% bg
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

inline void PlotBlended75(const Surface &out, int x, int y, uint8_t fg)
{
	if (x >= 0 && x < out.w() && y >= 0 && y < out.h()) {
		uint8_t &px = out[{ x, y }];
		px = Blend75(fg, px);
	}
}

inline void PlotBlended50(const Surface &out, int x, int y, uint8_t fg)
{
	if (x >= 0 && x < out.w() && y >= 0 && y < out.h()) {
		uint8_t &px = out[{ x, y }];
		px = Blend50(fg, px);
	}
}

inline void PlotBlended37(const Surface &out, int x, int y, uint8_t fg)
{
	if (x >= 0 && x < out.w() && y >= 0 && y < out.h()) {
		uint8_t &px = out[{ x, y }];
		px = Blend37(fg, px);
	}
}

inline void PlotBlended25(const Surface &out, int x, int y, uint8_t fg)
{
	if (x >= 0 && x < out.w() && y >= 0 && y < out.h()) {
		uint8_t &px = out[{ x, y }];
		px = Blend25(fg, px);
	}
}

inline void PlotBlended19(const Surface &out, int x, int y, uint8_t fg)
{
	if (x >= 0 && x < out.w() && y >= 0 && y < out.h()) {
		uint8_t &px = out[{ x, y }];
		px = Blend19(fg, px);
	}
}

inline void PlotBlended12(const Surface &out, int x, int y, uint8_t fg)
{
	if (x >= 0 && x < out.w() && y >= 0 && y < out.h()) {
		uint8_t &px = out[{ x, y }];
		px = Blend12(fg, px);
	}
}

/**
 * @brief Computes the exact world-space pixel coordinates of the top-left corner (0, 0)
 * of the viewport `out` surface.
 */
inline Point CalcCameraWorldPixel(Point startTilePosition, Displacement offset)
{
	const int camX = (startTilePosition.x - startTilePosition.y) * (TILE_WIDTH / 2) - offset.deltaX;
	const int camY = (startTilePosition.x + startTilePosition.y) * (TILE_HEIGHT / 2) - offset.deltaY;
	return Point { camX, camY };
}

bool IsOutdoorWalkableGround(Point tile)
{
	if (!InDungeonBounds(tile))
		return false;
	const int piece = dPiece[tile.x][tile.y];
	if (piece <= 0)
		return false;
	if (TileHasAny(piece, TileProperties::Solid))
		return false;
	return true;
}

void RestoreNormalTownPalette()
{
	if (!gLightningFlashActiveOnPalette)
		return;
	gLightningFlashActiveOnPalette = false;
	gActiveAppliedStrength = 0;
	if (IsPaletteFadedIn()) {
		ApplyGamma(system_palette, logical_palette, 256);
		palette_update(0, 256);
	}
}

void ApplyLightningPaletteBoost(uint8_t strength)
{
	if (!IsPaletteFadedIn() || strength == 0)
		return;
	if (gLightningFlashActiveOnPalette && gActiveAppliedStrength == strength)
		return;

	ApplyGamma(system_palette, logical_palette, 256);

	// Strong cool-white / electric slate-blue lightning illumination across the Town palette
	const int addR = (58 * static_cast<int>(strength)) / 100;
	const int addG = (70 * static_cast<int>(strength)) / 100;
	const int addB = (98 * static_cast<int>(strength)) / 100;
	const int multNum = 256 + static_cast<int>(strength); // up to ~1.65x luminance boost

	for (int i = 1; i < 255; i++) {
		const int r = (static_cast<int>(system_palette[i].r) * multNum) >> 8;
		const int g = (static_cast<int>(system_palette[i].g) * multNum) >> 8;
		const int b = (static_cast<int>(system_palette[i].b) * multNum) >> 8;

		system_palette[i].r = static_cast<Uint8>(std::min(255, r + addR));
		system_palette[i].g = static_cast<Uint8>(std::min(255, g + addG));
		system_palette[i].b = static_cast<Uint8>(std::min(255, b + addB));
	}

	palette_update(0, 256);
	gLightningFlashActiveOnPalette = true;
	gActiveAppliedStrength = strength;
}

void TriggerLightningStrike(uint32_t nowTick, uint8_t strength)
{
	gFlashStartTick = nowTick;
	gFlashPeakEndTick = nowTick + 60;   // 60ms full peak flash (~1 classic Diablo frame)
	gFlashTotalEndTick = nowTick + 115; // 55ms crisp afterglow decay
	gFlashPeakStrength = strength;
	ApplyLightningPaletteBoost(strength);
}

void ScheduleNextLightning(uint32_t nowTick)
{
	// Random interval strictly between 30 and 60 seconds (30,000 .. 60,000 ms)
	gNextLightningTick = nowTick + 30000U + static_cast<uint32_t>(GenerateRnd(30001));
}

void RollRainIntensity()
{
	gCurrentRainMode = (GenerateRnd(100) < 50) ? RainMode::Light : RainMode::Heavy;
	gHeavyBlend = (gCurrentRainMode == RainMode::Heavy) ? 1.0F : 0.0F;
	gHasGameRainRoll = true;
}

void InitRainDrop(RainDrop &drop, int viewW, int viewH, bool randomY)
{
	const int marginX = 144;
	drop.x = static_cast<float>(GenerateRnd(std::max(1, viewW + marginX * 2)) - marginX);
	drop.y = randomY
	    ? static_cast<float>(GenerateRnd(std::max(1, viewH)))
	    : -static_cast<float>(GenerateRnd(64));

	const int roll = GenerateRnd(100);
	if (roll < 42) {
		// Layer 0: Distant background rain (+5% transparency: softer shade + shorter 33% head blend)
		drop.layer = 0;
		drop.parallaxScale = 0.58F;
		drop.speedScale = 0.55F + static_cast<float>(GenerateRnd(18)) * 0.01F;
		drop.length = static_cast<uint8_t>(9 + GenerateRnd(5)); // 9..13 px
		drop.colorIndex = static_cast<uint8_t>((GenerateRnd(2) == 0 ? PAL16_BLUE : PAL16_GRAY) + 3 + GenerateRnd(2));
	} else if (roll < 84) {
		// Layer 1: Midground world-plane air rain (+15% transparency: softer shade + 50%/37.5%/25% blend)
		drop.layer = 1;
		drop.parallaxScale = 1.00F;
		drop.speedScale = 0.95F + static_cast<float>(GenerateRnd(24)) * 0.01F;
		drop.length = static_cast<uint8_t>(17 + GenerateRnd(8)); // 17..24 px
		drop.colorIndex = static_cast<uint8_t>((GenerateRnd(2) == 0 ? PAL16_BLUE : PAL16_GRAY) + 2 + GenerateRnd(2));
	} else {
		// Layer 2: Near-camera foreground "lens" rain (+35% transparency: softer shade + 50%/37.5%/25%/18.75% DoF blur)
		drop.layer = 2;
		drop.parallaxScale = 1.65F;
		drop.speedScale = 1.65F + static_cast<float>(GenerateRnd(50)) * 0.01F;
		drop.length = static_cast<uint8_t>(34 + GenerateRnd(15)); // 34..48 px
		drop.colorIndex = static_cast<uint8_t>((GenerateRnd(2) == 0 ? PAL16_GRAY : PAL16_BLUE) + 2 + GenerateRnd(2));
	}
}

void InitSteamFogBank(SteamFogBank &bank, size_t index)
{
	// Arrange on a strict non-overlapping 5x3 staggered lattice inside 1500x900 world pixels:
	// Each cell is 300x300 px, and each bank's max extent is ~200x76 px with bounded cell jitter,
	// guaranteeing two banks never touch or overlap.
	const int cols = 5;
	const int rows = 3;
	const int cellW = FogWorldWrapW / cols; // 300 px
	const int cellH = FogWorldWrapH / rows; // 300 px
	const int col = static_cast<int>(index % cols);
	const int row = static_cast<int>((index / cols) % rows);
	const int staggerX = (row & 1) != 0 ? (cellW / 2) : 0;

	bank.worldX = static_cast<float>((col * cellW + staggerX + 125 + GenerateRnd(50)) % FogWorldWrapW);
	bank.worldY = static_cast<float>(row * cellH + 115 + GenerateRnd(70));
	bank.phase = static_cast<float>(GenerateRnd(628)) * 0.01F;
	bank.phaseSpeed = 0.40F + static_cast<float>(GenerateRnd(45)) * 0.01F;

	// Size increased by +20% (rx: 46..79, ry: 18..30)
	bank.rx = static_cast<int16_t>(46 + GenerateRnd(34));
	bank.ry = static_cast<int16_t>(18 + GenerateRnd(13));

	// 4 overlapping billowy puffs arranged along an isometric wind-sheared ribbon (+20% scaled offsets)
	bank.puffOffX[0] = static_cast<int8_t>(-(bank.rx * 3) / 5 - GenerateRnd(9));
	bank.puffOffY[0] = static_cast<int8_t>(4 + GenerateRnd(7));
	bank.puffScalePct[0] = static_cast<uint8_t>(68 + GenerateRnd(16));

	bank.puffOffX[1] = static_cast<int8_t>(-(bank.rx / 5) + GenerateRnd(9) - 4);
	bank.puffOffY[1] = static_cast<int8_t>(-3 - GenerateRnd(6));
	bank.puffScalePct[1] = 100;

	bank.puffOffX[2] = static_cast<int8_t>((bank.rx / 4) + GenerateRnd(9));
	bank.puffOffY[2] = static_cast<int8_t>(3 + GenerateRnd(6));
	bank.puffScalePct[2] = static_cast<uint8_t>(82 + GenerateRnd(15));

	bank.puffOffX[3] = static_cast<int8_t>((bank.rx * 3) / 5 + GenerateRnd(9));
	bank.puffOffY[3] = static_cast<int8_t>(-4 - GenerateRnd(6));
	bank.puffScalePct[3] = static_cast<uint8_t>(64 + GenerateRnd(16));

	const bool coolTint = (GenerateRnd(3) == 0);
	bank.colorIndex = static_cast<uint8_t>((coolTint ? PAL16_BLUE : PAL16_GRAY) + 6 + GenerateRnd(2));
}

void SyncWeatherAudio()
{
	if (!gbRunGame || !gbSoundOn || PauseMode != 0 || leveltype != DTYPE_TOWN || setlevel) {
		StopSFX(LS_RAINLIGHT);
		StopSFX(LS_RAINHEAVY);
		return;
	}

	if (gHeavyBlend >= 0.5F) {
		StopSFX(LS_RAINLIGHT);
		PlayLoopingSFX(LS_RAINHEAVY, -200);
	} else {
		StopSFX(LS_RAINHEAVY);
		PlayLoopingSFX(LS_RAINLIGHT, -300);
	}
}

/**
 * @brief Renders a single localized ground puddle splash with:
 * - Average size reduced by 50% (max radius rx=5..6, ry=2..3)
 * - +50% transparency (50% / 25% / 12.5% LUT blending so splashes integrate naturally with ground)
 */
void DrawSinglePuddleSplash(const Surface &out, Point center, uint8_t stage, uint8_t variant)
{
	if (center.x < -10 || center.x >= out.w() + 10 || center.y < -10 || center.y >= out.h() + 10)
		return;

	const uint8_t specCol = static_cast<uint8_t>(PAL16_GRAY + 1 + (variant & 1));
	const uint8_t brightCol = static_cast<uint8_t>(PAL16_BLUE + 1 + (variant & 1));
	const uint8_t midCol = static_cast<uint8_t>(PAL16_BLUE + 3 + (variant & 1));
	const uint8_t ringCol = static_cast<uint8_t>(PAL16_GRAY + 3 + (variant & 1));

	switch (stage) {
	case 0: {
		// Stage 0: Compact semi-transparent impact bead (rx=1, ry=0) + 2 tiny crown droplets
		PlotBlended50(out, center.x, center.y, specCol);
		PlotBlended25(out, center.x - 1, center.y, brightCol);
		PlotBlended25(out, center.x + 1, center.y, brightCol);
		PlotBlended25(out, center.x - 1, center.y - 2, specCol);
		PlotBlended25(out, center.x + 1, center.y - 2, specCol);
		break;
	}
	case 1: {
		// Stage 1: Compact 2:1 isometric inner ring (rx=2, ry=1)
		PlotBlended50(out, center.x - 1, center.y - 1, specCol);
		PlotBlended50(out, center.x, center.y - 1, specCol);
		PlotBlended50(out, center.x + 1, center.y - 1, specCol);

		PlotBlended50(out, center.x - 2, center.y, brightCol);
		PlotBlended50(out, center.x + 2, center.y, brightCol);

		PlotBlended25(out, center.x - 1, center.y + 1, midCol);
		PlotBlended25(out, center.x, center.y + 1, midCol);
		PlotBlended25(out, center.x + 1, center.y + 1, midCol);
		break;
	}
	case 2: {
		// Stage 2: Expanding 2:1 isometric ripple (rx=3..4, ry=1..2)
		const int rx = 3 + static_cast<int>(variant & 1);
		PlotBlended50(out, center.x - 1, center.y - 1, brightCol);
		PlotBlended25(out, center.x, center.y - 2, brightCol);
		PlotBlended50(out, center.x + 1, center.y - 1, brightCol);

		PlotBlended50(out, center.x - rx, center.y, brightCol);
		PlotBlended50(out, center.x + rx, center.y, midCol);
		PlotBlended25(out, center.x - (rx - 1), center.y - 1, midCol);
		PlotBlended25(out, center.x + (rx - 1), center.y - 1, midCol);

		PlotBlended25(out, center.x - (rx - 1), center.y + 1, midCol);
		PlotBlended25(out, center.x + (rx - 1), center.y + 1, midCol);
		PlotBlended25(out, center.x, center.y + 1, ringCol);
		break;
	}
	case 3: {
		// Stage 3: Soft semi-transparent isometric ripple (rx=4..5, ry=2)
		const int rx = 4 + static_cast<int>(variant & 1);
		PlotBlended25(out, center.x - 2, center.y - 2, midCol);
		PlotBlended25(out, center.x, center.y - 2, brightCol);
		PlotBlended25(out, center.x + 2, center.y - 2, midCol);

		PlotBlended25(out, center.x - (rx - 1), center.y - 1, midCol);
		PlotBlended25(out, center.x + (rx - 1), center.y - 1, midCol);

		PlotBlended25(out, center.x - rx, center.y, midCol);
		PlotBlended25(out, center.x + rx, center.y, ringCol);

		PlotBlended25(out, center.x - (rx - 1), center.y + 1, ringCol);
		PlotBlended25(out, center.x + (rx - 1), center.y + 1, ringCol);

		PlotBlended12(out, center.x - 1, center.y + 2, ringCol);
		PlotBlended12(out, center.x + 1, center.y + 2, ringCol);
		break;
	}
	default: {
		// Stage 4: Dissipating outer ring (rx=5..6, ry=2..3) at 12.5%..25% opacity
		const int rx = 5 + static_cast<int>(variant & 1);
		const int ry = 2 + static_cast<int>(variant & 1);
		PlotBlended25(out, center.x - 2, center.y - ry, ringCol);
		PlotBlended25(out, center.x + 2, center.y - ry, ringCol);

		PlotBlended25(out, center.x - rx, center.y, ringCol);
		PlotBlended25(out, center.x + rx, center.y, ringCol);

		PlotBlended12(out, center.x - (rx - 1), center.y - 1, ringCol);
		PlotBlended12(out, center.x + (rx - 1), center.y - 1, ringCol);
		PlotBlended12(out, center.x - (rx - 1), center.y + 1, ringCol);
		PlotBlended12(out, center.x + (rx - 1), center.y + 1, ringCol);

		PlotBlended12(out, center.x, center.y + ry, ringCol);
		break;
	}
	}
}

/**
 * @brief Renders a curling ground-evaporation steam wisp rising off a walkable outdoor tile:
 * - Size reduced by 35% (riseY=18px, rx=1..4px, ry=1..2px)
 * - Transparency increased by 50% (12.5% Blend12 core + 6.25% Blend6 feather halo)
 */
void DrawGroundSteamWisp(const Surface &out, Point basePos, uint32_t progressPermille, uint8_t variant)
{
	if (basePos.x < -24 || basePos.x >= out.w() + 24 || basePos.y < -8 || basePos.y >= out.h() + 24)
		return;

	const uint8_t coreCol = static_cast<uint8_t>(PAL16_GRAY + 2 + (variant & 1));
	const uint8_t haloCol = static_cast<uint8_t>(((variant & 2) != 0 ? PAL16_BLUE : PAL16_GRAY) + 3 + (variant & 1));
	const float phaseBase = static_cast<float>(variant) * 0.85F + static_cast<float>(progressPermille) * 0.00628F;

	// Render 5 staggered vapor puffs along an ascending, wind-curled plume (-35% size)
	for (int p = 0; p < 5; ++p) {
		const int puffProgress = static_cast<int>(progressPermille) - p * 120;
		if (puffProgress <= 0 || puffProgress >= 880)
			continue;

		const float t = static_cast<float>(puffProgress) / 880.0F; // 0.0 -> 1.0 as puff rises
		const float swayX = std::sin(phaseBase + t * 4.2F) * (2.2F + t * 2.9F) - t * 7.1F;
		const float riseY = -t * 18.2F;

		const int cx = basePos.x + static_cast<int>(swayX);
		const int cy = basePos.y + static_cast<int>(riseY);

		const float env = (t < 0.30F) ? (t / 0.30F) : ((1.0F - t) / 0.70F);
		const int rx = std::max(1, static_cast<int>((2.6F + t * 3.9F) * (0.55F + 0.45F * env)));
		const int ry = std::max(1, static_cast<int>((1.3F + t * 2.2F) * (0.55F + 0.45F * env)));

		const int yStart = std::max(0, cy - ry);
		const int yEnd = std::min(out.h() - 1, cy + ry);
		const int xStart = std::max(0, cx - rx);
		const int xEnd = std::min(out.w() - 1, cx + rx);
		const int invRx2 = std::max(1, rx * rx);
		const int invRy2 = std::max(1, ry * ry);
		const bool hasDenseCore = (env > 0.50F);

		for (int y = yStart; y <= yEnd; ++y) {
			const int dy = y - cy;
			const int dyNorm256 = (dy * dy * 256) / invRy2;
			if (dyNorm256 >= 256)
				continue;

			for (int x = xStart; x <= xEnd; ++x) {
				const int dx = x - cx;
				const int d2 = (dx * dx * 256) / invRx2 + dyNorm256;
				if (d2 >= 256)
					continue;

				uint8_t &px = out[{ x, y }];
				if (hasDenseCore && d2 < 92) {
					px = Blend12(coreCol, px);
				} else {
					px = Blend6(haloCol, px);
				}
			}
		}
	}
}

/**
 * @brief Renders a single world-anchored multi-puff Steam Fog Bank at screen center (screenCx, screenCy):
 * - +20% larger size
 * - +35% higher transparency (12.5% Blend12 inner core + 6.25% Blend6 outer feather)
 * - Strictly non-overlapping against all previously drawn banks in `drawnRects`
 */
void DrawSingleSteamFogBank(
    const Surface &out,
    const SteamFogBank &bank,
    int screenCx,
    int screenCy,
    std::array<DrawnFogRect, 32> &drawnRects,
    size_t &drawnCount)
{
	int puffCx[4];
	int puffCy[4];
	int invRx2[4];
	int invRy2[4];
	int rawMinX = out.w();
	int rawMaxX = -1;
	int rawMinY = out.h();
	int rawMaxY = -1;

	for (int i = 0; i < 4; ++i) {
		const float breath = std::sin(bank.phase + static_cast<float>(i) * 1.35F);
		puffCx[i] = screenCx + static_cast<int>(bank.puffOffX[i]) + static_cast<int>(breath * 3.0F);
		puffCy[i] = screenCy + static_cast<int>(bank.puffOffY[i]) + static_cast<int>(std::cos(bank.phase + static_cast<float>(i)) * 2.0F);

		const int prx = std::max(10, (static_cast<int>(bank.rx) * static_cast<int>(bank.puffScalePct[i])) / 100);
		const int pry = std::max(6, (static_cast<int>(bank.ry) * static_cast<int>(bank.puffScalePct[i])) / 100);
		invRx2[i] = prx * prx;
		invRy2[i] = pry * pry;

		rawMinX = std::min(rawMinX, puffCx[i] - prx);
		rawMaxX = std::max(rawMaxX, puffCx[i] + prx);
		rawMinY = std::min(rawMinY, puffCy[i] - pry - 4);
		rawMaxY = std::max(rawMaxY, puffCy[i] + pry + 4);
	}

	const int minX = std::max(0, rawMinX);
	const int maxX = std::min(out.w() - 1, rawMaxX);
	const int minY = std::max(0, rawMinY);
	const int maxY = std::min(out.h() - 1, rawMaxY);
	if (minX > maxX || minY > maxY)
		return;

	// Enforce strict non-overlap against any already-drawn Steam Fog Bank on screen
	for (size_t r = 0; r < drawnCount; ++r) {
		if (rawMaxX >= drawnRects[r].minX - 6 && rawMinX <= drawnRects[r].maxX + 6
		    && rawMaxY >= drawnRects[r].minY - 6 && rawMinY <= drawnRects[r].maxY + 6) {
			return;
		}
	}
	if (drawnCount < drawnRects.size()) {
		drawnRects[drawnCount++] = DrawnFogRect { rawMinX, rawMaxX, rawMinY, rawMaxY };
	}

	const float pulse = 0.85F + 0.15F * std::sin(bank.phase);
	const int coreCutoff = static_cast<int>(54.0F * pulse); // Soft inner core cutoff (out of 256)

	for (int y = minY; y <= maxY; ++y) {
		for (int x = minX; x <= maxX; ++x) {
			const int waveOffsetY = (((x - screenCx) * 5) >> 5) & 1;
			const int wy = y + waveOffsetY;

			int minD2 = 256;
			for (int i = 0; i < 4; ++i) {
				const int dy = wy - puffCy[i];
				const int dyNorm = (dy * dy * 256) / invRy2[i];
				if (dyNorm >= minD2)
					continue;
				const int dx = x - puffCx[i];
				const int d2 = (dx * dx * 256) / invRx2[i] + dyNorm;
				if (d2 < minD2)
					minD2 = d2;
			}

			if (minD2 >= 256)
				continue;

			// +35% increased transparency (contiguous scanline-free 12.5% core + 6.25% feather):
			uint8_t &px = out[{ x, y }];
			if (minD2 < coreCutoff) {
				px = Blend12(bank.colorIndex, px);
			} else {
				px = Blend6(bank.colorIndex, px);
			}
		}
	}
}

} // namespace

void OnWeatherNewGame()
{
	ResetWeatherState();
	gHasGameRainRoll = false;
	gWeatherInitialized = false;
	gInDungeon = false;
	gDungeonEnterTick = 0;
	gAccumulatedDungeonMs = 0;
}

void OnWeatherLevelLoad(bool firstflag, dungeon_type levelType)
{
	const uint32_t nowTick = SDL_GetTicks();

	if (firstflag || !gHasGameRainRoll) {
		RollRainIntensity();
		gInDungeon = false;
		gDungeonEnterTick = 0;
		gAccumulatedDungeonMs = 0;
		ScheduleNextLightning(nowTick);
	}

	if (levelType != DTYPE_TOWN || setlevel) {
		// Entering or changing levels inside a dungeon: stop weather audio & flash immediately
		RestoreNormalTownPalette();
		StopSFX(LS_RAINLIGHT);
		StopSFX(LS_RAINHEAVY);
		StopSFX(LS_THUNDERA);
		StopSFX(LS_THUNDERB);
		gHasPrevCamPos = false;
		gSecondaryFlashTick = 0;
		gPendingThunderTick = 0;

		if (!gInDungeon) {
			gInDungeon = true;
			gDungeonEnterTick = nowTick;
		}
	} else {
		// Returning to Tristram: only allow rain intensity to change if player spent >= 1 minute in a dungeon
		if (gInDungeon) {
			gAccumulatedDungeonMs += (nowTick - gDungeonEnterTick);
			gInDungeon = false;
			if (gAccumulatedDungeonMs >= MinDungeonTimeForWeatherChangeMs) {
				RollRainIntensity();
			}
			gAccumulatedDungeonMs = 0;
		}
		gHasPrevCamPos = false;
		gPendingThunderTick = 0;
		ScheduleNextLightning(nowTick);
	}
}

void ResetWeatherState()
{
	RestoreNormalTownPalette();
	StopSFX(LS_RAINLIGHT);
	StopSFX(LS_RAINHEAVY);
	StopSFX(LS_THUNDERA);
	StopSFX(LS_THUNDERB);
	gHasPrevCamPos = false;
	gSecondaryFlashTick = 0;
	gPendingThunderTick = 0;
	gFlashPeakStrength = 0;
}

void UpdateWeatherState(const Surface &out, Point startTilePosition, Displacement offset)
{
	if (!gbRunGame || leveltype != DTYPE_TOWN || setlevel) {
		ResetWeatherState();
		return;
	}

	const int viewW = out.w();
	const int viewH = out.h();
	const uint32_t nowTick = SDL_GetTicks();

	if (!gWeatherInitialized || gLastViewportW != viewW || gLastViewportH != viewH) {
		if (!gHasGameRainRoll) {
			RollRainIntensity();
		}
		if (!gWeatherInitialized) {
			ScheduleNextLightning(nowTick);
			for (size_t i = 0; i < gSteamFogBanks.size(); ++i) {
				InitSteamFogBank(gSteamFogBanks[i], i);
			}
		}
		gLastViewportW = viewW;
		gLastViewportH = viewH;
		for (auto &drop : gRainDrops) {
			InitRainDrop(drop, viewW, viewH, /*randomY=*/true);
		}
		gWeatherInitialized = true;
		gLastWeatherTick = nowTick;
	}

	// Update active lightning flash envelope (60ms peak flash + 55ms afterglow decay)
	if (gLightningFlashActiveOnPalette) {
		if (nowTick >= gFlashTotalEndTick) {
			RestoreNormalTownPalette();
		} else if (nowTick >= gFlashPeakEndTick) {
			const uint8_t afterglowStrength = static_cast<uint8_t>((static_cast<uint32_t>(gFlashPeakStrength) * 45U) / 100U);
			ApplyLightningPaletteBoost(afterglowStrength);
		}
	}

	uint32_t dtMs = nowTick - gLastWeatherTick;
	gLastWeatherTick = nowTick;
	if (dtMs > 100) {
		dtMs = 33;
	}
	const float dt = static_cast<float>(dtMs) * 0.001F;

	if (PauseMode != 0) {
		SyncWeatherAudio();
		return;
	}

	// Keep gHeavyBlend locked to the session's active gCurrentRainMode
	gHeavyBlend = (gCurrentRainMode == RainMode::Heavy) ? 1.0F : 0.0F;

	// Trigger speed-of-sound delayed thunder clap when its acoustic timer arrives
	if (gPendingThunderTick != 0 && nowTick >= gPendingThunderTick) {
		gPendingThunderTick = 0;
		PlayWeatherSFX(gPendingThunderSfx, gPendingThunderVolume);
	}

	// Trigger distant lightning flashes (every 30-60s) and schedule speed-of-sound delayed thunder
	if (gSecondaryFlashTick != 0 && nowTick >= gSecondaryFlashTick) {
		gSecondaryFlashTick = 0;
		TriggerLightningStrike(nowTick, gPendingFlashStrength);
	} else if (nowTick >= gNextLightningTick) {
		const bool isHeavy = (gHeavyBlend >= 0.5F);
		const uint8_t flashStrength = isHeavy
		    ? static_cast<uint8_t>(135 + GenerateRnd(36))
		    : static_cast<uint8_t>(100 + GenerateRnd(31));
		TriggerLightningStrike(nowTick, flashStrength);

		if (isHeavy) {
			gPendingThunderSfx = LS_THUNDERB;
			gPendingThunderTick = nowTick + 608 + static_cast<uint32_t>(GenerateRnd(608));
			gPendingThunderVolume = -80;
		} else {
			gPendingThunderSfx = LS_THUNDERA;
			gPendingThunderTick = nowTick + 1553 + static_cast<uint32_t>(GenerateRnd(945));
			gPendingThunderVolume = -180;
		}

		const int doubleStrikeChance = isHeavy ? 50 : 25;
		if (GenerateRnd(100) < doubleStrikeChance) {
			gSecondaryFlashTick = nowTick + 125 + static_cast<uint32_t>(GenerateRnd(45));
			gPendingFlashStrength = static_cast<uint8_t>((static_cast<uint32_t>(flashStrength) * 80U) / 100U);
		}
		ScheduleNextLightning(nowTick);
	}

	// Compute exact camera world-space pixel position so world-space fog and rain never move with the camera
	const Point newCamWorldPixel = CalcCameraWorldPixel(startTilePosition, offset);
	int camDeltaX = 0;
	int camDeltaY = 0;
	if (gHasPrevCamPos) {
		camDeltaX = newCamWorldPixel.x - gCamWorldPixel.x;
		camDeltaY = newCamWorldPixel.y - gCamWorldPixel.y;
		if (std::abs(camDeltaX) > 256 || std::abs(camDeltaY) > 256) {
			camDeltaX = 0;
			camDeltaY = 0;
		}
	}
	gCamWorldPixel = newCamWorldPixel;
	gHasPrevCamPos = true;

	// Update world-space drifting precipitation / steam fog banks with uniform wind velocity
	// so their non-overlapping lattice spacing is strictly preserved at all times
	const float fogWindX = -(14.0F + 10.0F * gHeavyBlend);
	const float fogRiseY = -2.5F;
	for (auto &bank : gSteamFogBanks) {
		bank.phase += bank.phaseSpeed * dt;
		if (bank.phase > 6.2831853F) {
			bank.phase -= 6.2831853F;
		}
		bank.worldX += fogWindX * dt;
		bank.worldY += fogRiseY * dt;

		if (bank.worldX < 0.0F) {
			bank.worldX += static_cast<float>(FogWorldWrapW);
		} else if (bank.worldX >= static_cast<float>(FogWorldWrapW)) {
			bank.worldX -= static_cast<float>(FogWorldWrapW);
		}
		if (bank.worldY < 0.0F) {
			bank.worldY += static_cast<float>(FogWorldWrapH);
		} else if (bank.worldY >= static_cast<float>(FogWorldWrapH)) {
			bank.worldY -= static_cast<float>(FogWorldWrapH);
		}
	}

	// Per-layer fall velocity and slant-matched horizontal wind velocity so each depth
	// plane moves strictly along its own perspective streak angle
	const float fallVelY = 510.0F + 230.0F * gHeavyBlend;
	const int marginX = 144;
	const float minX = -static_cast<float>(marginX);
	const float maxX = static_cast<float>(viewW + marginX);
	const float spanX = maxX - minX;

	const size_t activeDropCount = static_cast<size_t>(
	    static_cast<float>(LightRainDropCount)
	    + static_cast<float>(HeavyRainDropCount - LightRainDropCount) * gHeavyBlend);

	for (size_t i = 0; i < activeDropCount; ++i) {
		RainDrop &drop = gRainDrops[i];
		const float pScale = drop.parallaxScale;
		const float layerSlant = GetLayerBaseSlant(drop.layer, gHeavyBlend);
		const float dropFallVel = fallVelY * drop.speedScale;
		const float dropWindVel = dropFallVel * layerSlant;

		drop.x += dropWindVel * dt - static_cast<float>(camDeltaX) * pScale;
		drop.y += dropFallVel * dt - static_cast<float>(camDeltaY) * pScale;

		if (drop.y > static_cast<float>(viewH + 48)) {
			InitRainDrop(drop, viewW, viewH, /*randomY=*/false);
		} else if (drop.y < -80.0F) {
			drop.y += static_cast<float>(viewH + 80);
		}

		while (drop.x < minX) {
			drop.x += spanX;
		}
		while (drop.x >= maxX) {
			drop.x -= spanX;
		}
	}

	SyncWeatherAudio();
}

void DrawWeatherGroundTile(const Surface &out, Point tilePosition, Point targetBufferPosition)
{
	if (!gbRunGame || leveltype != DTYPE_TOWN || setlevel)
		return;
	if (!IsOutdoorWalkableGround(tilePosition))
		return;

	// Center of the 64x32 isometric ground diamond in `out` buffer coordinates
	const int tileCenterX = targetBufferPosition.x + TILE_WIDTH / 2;
	const int tileCenterY = targetBufferPosition.y - TILE_HEIGHT / 2;
	if (tileCenterX < -32 || tileCenterX >= out.w() + 32 || tileCenterY < -16 || tileCenterY >= out.h() + 32)
		return;

	const uint32_t nowTick = gLastWeatherTick != 0 ? gLastWeatherTick : SDL_GetTicks();

	// 1. Render localized puddle splash animations (-35% count, -50% size, +50% transparency)
	const uint32_t tileSeed = FastHash(
	    static_cast<uint32_t>(tilePosition.x) * 73856093U
	    ^ static_cast<uint32_t>(tilePosition.y) * 19349663U);

	const uint32_t spawnChancePerMille = (gHeavyBlend >= 0.5F) ? 299U : 136U;
	constexpr uint32_t CycleDurationMs = 320U;
	constexpr uint32_t SplashAnimDurationMs = 260U;

	for (uint32_t slot = 0; slot < 2; ++slot) {
		const uint32_t slotSeed = FastHash(tileSeed ^ (slot * 0x9e3779b9U));
		const uint32_t phaseOffset = slotSeed % CycleDurationMs;
		const uint32_t shiftedTick = nowTick + phaseOffset;
		const uint32_t epoch = shiftedTick / CycleDurationMs;
		const uint32_t localMs = shiftedTick % CycleDurationMs;

		if (localMs >= SplashAnimDurationMs)
			continue;

		const uint32_t epochHash = FastHash(slotSeed ^ (epoch * 0x85ebca6bU));
		if ((epochHash % 1000U) >= spawnChancePerMille)
			continue;

		const int subX = static_cast<int>((epochHash >> 10) % 33U) - 16;
		const int subY = static_cast<int>((epochHash >> 16) % 15U) - 7;
		const uint8_t variant = static_cast<uint8_t>((epochHash >> 22) & 1U);
		const uint8_t stage = static_cast<uint8_t>(std::min<uint32_t>(4U, (localMs * 5U) / SplashAnimDurationMs));

		DrawSinglePuddleSplash(out, Point { tileCenterX + subX, tileCenterY + subY }, stage, variant);
	}

	// 2. Render curling rising ground-evaporation steam wisps (-40% count, -35% size, +50% transparency)
	if ((tileSeed % 100U) < 19U) {
		constexpr uint32_t SteamCycleMs = 2600U;
		const uint32_t shiftedSteamTick = nowTick + (tileSeed % SteamCycleMs);
		const uint32_t steamEpoch = shiftedSteamTick / SteamCycleMs;
		const uint32_t steamLocalMs = shiftedSteamTick % SteamCycleMs;
		const uint32_t steamHash = FastHash(tileSeed ^ (steamEpoch * 0x27d4eb2dU));
		if ((steamHash % 100U) < 75U) {
			const int wispOffX = static_cast<int>((steamHash >> 8) % 25U) - 12;
			const int wispOffY = static_cast<int>((steamHash >> 16) % 11U) - 5;
			const uint32_t progressPermille = (steamLocalMs * 1000U) / SteamCycleMs;
			DrawGroundSteamWisp(
			    out,
			    Point { tileCenterX + wispOffX, tileCenterY + wispOffY },
			    progressPermille,
			    static_cast<uint8_t>(steamHash >> 24));
		}
	}

	// 3. Composite Layer 0 (Distant Background Rain) onto this walkable ground tile's 64x32
	// isometric diamond right after floor microtiles mt[0..1] (and LeftFoliage/RightFoliage)
	// have finished, and BEFORE upper microtiles mt[2..15] (roofs, walls, arches), solid tiles,
	// trees (dSpecial), objects, NPCs, and players are drawn on top.
	if (gBgRainMaskW == out.w() && gBgRainMaskH == out.h() && !gBgRainMask.empty()) {
		const int boxLeft = std::max(0, targetBufferPosition.x);
		const int boxRight = std::min(gBgRainMaskW - 1, targetBufferPosition.x + TILE_WIDTH - 1);
		const int boxTop = std::max(0, targetBufferPosition.y - (TILE_HEIGHT - 1));
		const int boxBottom = std::min(gBgRainMaskH - 1, targetBufferPosition.y);

		if (boxLeft <= boxRight && boxTop <= boxBottom) {
			const int bColStart = boxLeft >> 5;
			const int bColEnd = boxRight >> 5;
			const int bRowStart = boxTop >> 4;
			const int bRowEnd = boxBottom >> 4;
			bool hasBgRain = false;
			for (int br = bRowStart; br <= bRowEnd && !hasBgRain; ++br) {
				for (int bc = bColStart; bc <= bColEnd; ++bc) {
					if (gBgRainBlockGrid[br * gBgRainBlockCols + bc] != 0) {
						hasBgRain = true;
						break;
					}
				}
			}

			if (hasBgRain) {
				for (int localY = 0; localY < TILE_HEIGHT; ++localY) {
					const int py = targetBufferPosition.y - localY;
					if (py < 0 || py >= gBgRainMaskH)
						continue;

					const int distY = (localY < (TILE_HEIGHT / 2)) ? ((TILE_HEIGHT / 2 - 1) - localY) : (localY - (TILE_HEIGHT / 2));
					const int xStart = std::max(0, targetBufferPosition.x + distY * 2 - 1);
					const int xEnd = std::min(gBgRainMaskW - 1, targetBufferPosition.x + (TILE_WIDTH - distY * 2));

					uint16_t *maskRow = &gBgRainMask[py * gBgRainMaskW];
					for (int px = xStart; px <= xEnd; ++px) {
						const uint16_t packed = maskRow[px];
						if (packed == 0)
							continue;
						maskRow[px] = 0; // Consume so adjacent diamond overlap never double-blends
						const uint8_t col = static_cast<uint8_t>(packed & 0xFFU);
						const uint8_t blendMode = static_cast<uint8_t>(packed >> 8U);
						uint8_t &dstPx = out[{ px, py }];
						dstPx = (blendMode >= 2) ? Blend37(col, dstPx) : Blend25(col, dstPx);
					}
				}
			}
		}
	}
}

void DrawWeatherBackground(const Surface &out, Point /*startTilePosition*/, Displacement /*offset*/)
{
	if (!gbRunGame || leveltype != DTYPE_TOWN || setlevel) {
		return;
	}

	const int viewW = out.w();
	const int viewH = out.h();
	if (viewW <= 0 || viewH <= 0)
		return;

	if (gBgRainMaskW != viewW || gBgRainMaskH != viewH) {
		gBgRainMaskW = viewW;
		gBgRainMaskH = viewH;
		gBgRainBlockCols = (viewW + 31) >> 5;
		gBgRainBlockRows = (viewH + 15) >> 4;
		gBgRainMask.resize(static_cast<size_t>(viewW) * static_cast<size_t>(viewH), 0);
		gBgRainBlockGrid.resize(static_cast<size_t>(gBgRainBlockCols) * static_cast<size_t>(gBgRainBlockRows), 0);
	} else {
		std::fill(gBgRainMask.begin(), gBgRainMask.end(), 0);
		std::fill(gBgRainBlockGrid.begin(), gBgRainBlockGrid.end(), 0);
	}

	const size_t activeDropCount = static_cast<size_t>(
	    static_cast<float>(LightRainDropCount)
	    + static_cast<float>(HeavyRainDropCount - LightRainDropCount) * gHeavyBlend);

	const uint32_t nowTick = gLastWeatherTick != 0 ? gLastWeatherTick : SDL_GetTicks();
	const float gustPhase = static_cast<float>(nowTick) * 0.0025F;
	const float baseSlant = GetLayerBaseSlant(0, gHeavyBlend);
	const int extraLen = static_cast<int>(3.0F * gHeavyBlend);

	// Rasterize Layer 0 (Distant Background Rain) into gBgRainMask so DrawWeatherGroundTile()
	// composites it onto walkable ground after mt[0..1] and before roofs/walls/trees/NPCs.
	for (size_t i = 0; i < activeDropCount; ++i) {
		const RainDrop &drop = gRainDrops[i];
		if (drop.layer != 0)
			continue;

		const int headX = static_cast<int>(drop.x);
		const int headY = static_cast<int>(drop.y);
		const int totalLen = static_cast<int>(drop.length) + extraLen;

		if (headY < 0 || headY - totalLen >= viewH || headX < -24 || headX >= viewW + 24)
			continue;

		const float gust = std::sin(drop.x * 0.006F + gustPhase) * 0.025F;
		const float slantXPerY = baseSlant + gust;

		for (int step = 0; step < totalLen; ++step) {
			const int py = headY - step;
			const int px = headX - static_cast<int>(static_cast<float>(step) * slantXPerY);
			if (px < 0 || px >= viewW || py < 0 || py >= viewH)
				continue;

			const uint16_t blendMode = (step * 3 < totalLen) ? 2U : 1U; // +5% transparency: 37.5% head (top 33%), 25% tail
			gBgRainMask[py * viewW + px] = static_cast<uint16_t>((blendMode << 8U) | drop.colorIndex);
			gBgRainBlockGrid[(py >> 4) * gBgRainBlockCols + (px >> 5)] = 1;
		}
	}
}

void DrawWeatherAir(const Surface &out, Point startTilePosition, Displacement offset)
{
	if (!gbRunGame || leveltype != DTYPE_TOWN || setlevel) {
		return;
	}

	// 1. Render non-overlapping world-anchored drifting precipitation / steam fog banks
	const Point camWorld = CalcCameraWorldPixel(startTilePosition, offset);
	const size_t activeFogBanks = static_cast<size_t>(
	    static_cast<float>(LightRainFogBankCount)
	    + static_cast<float>(HeavyRainFogBankCount - LightRainFogBankCount) * gHeavyBlend);

	std::array<DrawnFogRect, 32> drawnRects;
	size_t drawnCount = 0;

	for (size_t i = 0; i < activeFogBanks; ++i) {
		const SteamFogBank &bank = gSteamFogBanks[i];
		const int relX = PositiveMod(static_cast<int>(bank.worldX) - camWorld.x + 160, FogWorldWrapW) - 160;
		const int relY = PositiveMod(static_cast<int>(bank.worldY) - camWorld.y + 120, FogWorldWrapH) - 120;

		DrawSingleSteamFogBank(out, bank, relX, relY, drawnRects, drawnCount);
		if (relX + FogWorldWrapW - 160 < out.w()) {
			DrawSingleSteamFogBank(out, bank, relX + FogWorldWrapW, relY, drawnRects, drawnCount);
		}
		if (relY + FogWorldWrapH - 120 < out.h()) {
			DrawSingleSteamFogBank(out, bank, relX, relY + FogWorldWrapH, drawnRects, drawnCount);
		}
	}

	// 2. Render Layer 1 (Midground Air Rain) and Layer 2 (Near-Camera DoF Foreground Rain)
	// Note: Layer 0 was already composited onto walkable ground behind roofs/trees/walls
	// in DrawWeatherGroundTile(), and all puddle splashes remain strictly on walkable ground.
	const size_t activeDropCount = static_cast<size_t>(
	    static_cast<float>(LightRainDropCount)
	    + static_cast<float>(HeavyRainDropCount - LightRainDropCount) * gHeavyBlend);

	const uint32_t nowTick = gLastWeatherTick != 0 ? gLastWeatherTick : SDL_GetTicks();
	const float gustPhase = static_cast<float>(nowTick) * 0.0032F;
	const float slant1 = GetLayerBaseSlant(1, gHeavyBlend);
	const float slant2 = GetLayerBaseSlant(2, gHeavyBlend);
	const int extraLen1 = static_cast<int>(6.0F * gHeavyBlend);
	const int extraLen2 = static_cast<int>(10.0F * gHeavyBlend);

	for (size_t i = 0; i < activeDropCount; ++i) {
		const RainDrop &drop = gRainDrops[i];
		if (drop.layer == 0)
			continue; // Drawn on walkable ground behind roofs/rocks/trees in DrawWeatherGroundTile()

		const int headX = static_cast<int>(drop.x);
		const int headY = static_cast<int>(drop.y);
		const int totalLen = static_cast<int>(drop.length) + (drop.layer == 2 ? extraLen2 : extraLen1);

		if (headY < 0 || headY - totalLen >= out.h() || headX < -40 || headX >= out.w() + 40)
			continue;

		const float gust = std::sin(drop.x * 0.007F + gustPhase) * 0.035F;
		const float slantXPerY = (drop.layer == 2 ? slant2 : slant1) + gust;

		if (drop.layer == 1) {
			// Layer 1 (Midground world-plane air streak, +15% transparency):
			// Softened from (75% -> 50% -> 25%) to (50% head -> 37.5% body -> 25% tail)
			for (int step = 0; step < totalLen; ++step) {
				const int py = headY - step;
				const int px = headX - static_cast<int>(static_cast<float>(step) * slantXPerY);
				if (px < 0 || px >= out.w() || py < 0 || py >= out.h())
					continue;

				if (step * 4 < totalLen) {
					PlotBlended50(out, px, py, drop.colorIndex);
				} else if (step * 5 < totalLen * 3) {
					PlotBlended37(out, px, py, drop.colorIndex);
				} else {
					PlotBlended25(out, px, py, drop.colorIndex);
				}
			}
		} else {
			// Layer 2 (Near-Camera Foreground "Lens" Rain, +35% transparency):
			// Softened by ~35% so near-camera drops are translucent and atmospheric rather than bright/glaring:
			// Core uses 50% -> 37.5% -> 25% -> 18.75% and DoF wings use 25% / 18.75% with darker halo shade (+2).
			const uint8_t haloCol = static_cast<uint8_t>(drop.colorIndex + 2);
			for (int step = 0; step < totalLen; ++step) {
				const int py = headY - step;
				const int px = headX - static_cast<int>(static_cast<float>(step) * slantXPerY);
				if (px < 1 || px >= out.w() - 1 || py < 0 || py >= out.h())
					continue;

				if (step * 5 < totalLen) {
					// Leading 20% head: 50% core + 25% right DoF wing + 18.75% left DoF wing
					PlotBlended50(out, px, py, drop.colorIndex);
					PlotBlended25(out, px + 1, py, haloCol);
					PlotBlended19(out, px - 1, py, haloCol);
				} else if (step * 2 < totalLen) {
					// Upper-mid 30%: 37.5% core + 25% right DoF wing + 18.75% left DoF wing
					PlotBlended37(out, px, py, drop.colorIndex);
					PlotBlended25(out, px + 1, py, haloCol);
					PlotBlended19(out, px - 1, py, haloCol);
				} else if (step * 4 < totalLen * 3) {
					// Lower-mid 25%: 25% core + 18.75% right DoF wing
					PlotBlended25(out, px, py, drop.colorIndex);
					PlotBlended19(out, px + 1, py, haloCol);
				} else {
					// Trailing 25% tail: 18.75% core taper
					PlotBlended19(out, px, py, drop.colorIndex);
				}
			}
		}
	}
}

} // namespace devilution
