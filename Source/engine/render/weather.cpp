/**
 * @file weather.cpp
 *
 * Implementation of Tristram weather & environmental particle effects:
 * - Equal 1/3 probability New Game weather modes (Rain, Snow, AshStorm) locked per game session,
 *   toggling between Light & Heavy intensity only after >= 1 minute in a dungeon.
 * - SnowHeavy acts as a true wind-driven snowstorm/blizzard with gale-force lateral shear & diagonal flurry streaks.
 * - AshStorm mixes swirling dark ash flakes with glowing orange-red volcanic ember cinders.
 * - Pauses particle simulation and stops weather audio loops when the in-game ESC menu is open.
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
#include "gmenu.h"
#include "levels/gendung.h"
#include "options.h"
#include "player.h"
#include "utils/stdcompat/algorithm.hpp"

namespace devilution {

namespace {

constexpr size_t MaxRainDrops = 576;
constexpr size_t LightRainDropCount = 160;
constexpr size_t HeavyRainDropCount = 420;
constexpr size_t LightSnowAshDropCount = 216; // +35% intensity for Light Snow & Light AshStorm
constexpr size_t HeavySnowAshDropCount = 567; // +35% intensity for Heavy Snow & Heavy AshStorm

constexpr size_t MaxSteamFogBanks = 14;
constexpr size_t LightRainFogBankCount = 9;
constexpr size_t HeavyRainFogBankCount = 14;
constexpr int FogWorldWrapW = 1560;
constexpr int FogWorldWrapH = 1040;

constexpr uint32_t MinDungeonTimeForWeatherChangeMs = 60000;

struct RainDrop {
	float x = 0.0F;
	float y = 0.0F;
	float speedScale = 1.0F;
	float parallaxScale = 1.0F; // 0.58 (Layer 0 far), 1.00 (Layer 1 mid), 1.65 (Layer 2 near camera)
	uint8_t length = 12;
	uint8_t layer = 0;          // 0 = far/occluded, 1 = mid, 2 = near/DoF blurred
	uint8_t colorIndex = PAL16_BLUE + 4;
};

struct SteamFogBank {
	float worldX = 0.0F;
	float worldY = 0.0F;
	float phase = 0.0F;
	float phaseSpeed = 0.50F;
	float speedMultX = 1.0F;
	float driftVelY = -4.0F;
	float swayAmpY = 14.0F;
	float swayFreq = 0.35F;
	int16_t boundRx = 88;
	int16_t boundRy = 56;
	int16_t puffRx[4] = { 42, 54, 48, 40 };
	int16_t puffRy[4] = { 30, 38, 34, 28 };
	int8_t puffOffX[4] = { -22, -4, 14, 24 };
	int8_t puffOffY[4] = { 8, -10, 10, -6 };
	uint8_t colorIndex = PAL16_GRAY + 6;
};

struct DrawnFogRect {
	int cx;
	int cy;
	int rx;
	int ry;
};

enum class TownWeatherKind : uint8_t {
	Rain,
	Snow,
	AshStorm,
};

bool gWeatherInitialized = false;
bool gHasGameRainRoll = false;
TownWeatherKind gTownWeatherKind = TownWeatherKind::Rain;
RainMode gCurrentRainMode = RainMode::Light;
float gHeavyBlend = 0.0F; // 0.0 = Light, 1.0 = Heavy
uint32_t gLastWeatherTick = 0;
uint32_t gWeatherSimTick = 0;

bool gInDungeon = false;
uint32_t gDungeonEnterTick = 0;
uint32_t gAccumulatedDungeonMs = 0;

bool gLightningFlashActiveOnPalette = false;
uint8_t gActiveAppliedStrength = 0;
uint32_t gFlashStartTick = 0;
uint32_t gFlashPeakEndTick = 0;
uint32_t gFlashTotalEndTick = 0;
uint8_t gFlashPeakStrength = 0;
uint32_t gNextLightningTick = 0;
uint32_t gSecondaryFlashTick = 0;
uint8_t gPendingFlashStrength = 0;

uint32_t gPendingThunderTick = 0;
_sfx_id gPendingThunderSfx = SFX_NONE;
int gPendingThunderVolume = 0;

bool gHasPrevCamPos = false;
Point gCamWorldPixel { 0, 0 };
int gLastViewportW = 0;
int gLastViewportH = 0;

std::array<RainDrop, MaxRainDrops> gRainDrops;
std::array<SteamFogBank, MaxSteamFogBanks> gSteamFogBanks;

std::vector<uint16_t> gBgRainMask;
std::vector<uint8_t> gBgRainBlockGrid;
int gBgRainMaskW = 0;
int gBgRainMaskH = 0;
int gBgRainBlockCols = 0;
int gBgRainBlockRows = 0;

inline size_t GetActiveWeatherDropCount()
{
	const bool isSnowOrAsh = (gTownWeatherKind == TownWeatherKind::Snow || gTownWeatherKind == TownWeatherKind::AshStorm);
	const float lightCount = static_cast<float>(isSnowOrAsh ? LightSnowAshDropCount : LightRainDropCount);
	const float heavyCount = static_cast<float>(isSnowOrAsh ? HeavySnowAshDropCount : HeavyRainDropCount);
	return std::min<size_t>(MaxRainDrops, static_cast<size_t>(lightCount + (heavyCount - lightCount) * gHeavyBlend));
}

inline float GetLayerBaseSlant(uint8_t layer, float heavyBlend)
{
	if (layer == 0)
		return -(0.12F + 0.05F * heavyBlend);
	if (layer == 1)
		return -(0.21F + 0.08F * heavyBlend);
	return -(0.31F + 0.10F * heavyBlend);
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
	return paletteTransparencyLookup[half][qtr];
}

inline uint8_t Blend19(uint8_t fg, uint8_t bg)
{
	const uint8_t qtr = Blend25(fg, bg);
	const uint8_t eighth = paletteTransparencyLookup[bg][qtr];
	return paletteTransparencyLookup[qtr][eighth];
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

	const int addR = (58 * static_cast<int>(strength)) / 100;
	const int addG = (70 * static_cast<int>(strength)) / 100;
	const int addB = (98 * static_cast<int>(strength)) / 100;
	const int multNum = 256 + static_cast<int>(strength);

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
	gFlashPeakEndTick = nowTick + 60;
	gFlashTotalEndTick = nowTick + 115;
	gFlashPeakStrength = strength;
	ApplyLightningPaletteBoost(strength);
}

void ScheduleNextLightning(uint32_t nowTick)
{
	gNextLightningTick = nowTick + 30000U + static_cast<uint32_t>(GenerateRnd(30001));
}

void RollRainIntensity()
{
	// On the first roll of a New Game session (!gHasGameRainRoll), roll with equal probability
	// (1/3 each) between Rain, Snow, and Ash Storm. Whichever rolls stays locked for the entire
	// game session; subsequent calls (after >= 60s in a dungeon) only roll Light vs Heavy intensity!
	if (!gHasGameRainRoll) {
		const int kindRoll = GenerateRnd(3);
		if (kindRoll == 0) {
			gTownWeatherKind = TownWeatherKind::Rain;
		} else if (kindRoll == 1) {
			gTownWeatherKind = TownWeatherKind::Snow;
		} else {
			gTownWeatherKind = TownWeatherKind::AshStorm;
		}
	}
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
		drop.layer = 0;
		drop.parallaxScale = 0.58F;
		drop.speedScale = 0.55F + static_cast<float>(GenerateRnd(18)) * 0.01F;
		drop.length = static_cast<uint8_t>(9 + GenerateRnd(5));
		drop.colorIndex = static_cast<uint8_t>((GenerateRnd(2) == 0 ? PAL16_BLUE : PAL16_GRAY) + 3 + GenerateRnd(2));
	} else if (roll < 84) {
		drop.layer = 1;
		drop.parallaxScale = 1.00F;
		drop.speedScale = 0.95F + static_cast<float>(GenerateRnd(24)) * 0.01F;
		drop.length = static_cast<uint8_t>(17 + GenerateRnd(8));
		drop.colorIndex = static_cast<uint8_t>((GenerateRnd(2) == 0 ? PAL16_BLUE : PAL16_GRAY) + 2 + GenerateRnd(2));
	} else {
		drop.layer = 2;
		drop.parallaxScale = 1.65F;
		drop.speedScale = 1.65F + static_cast<float>(GenerateRnd(50)) * 0.01F;
		drop.length = static_cast<uint8_t>(34 + GenerateRnd(15));
		drop.colorIndex = static_cast<uint8_t>((GenerateRnd(2) == 0 ? PAL16_GRAY : PAL16_BLUE) + 2 + GenerateRnd(2));
	}
}

void InitSteamFogBank(SteamFogBank &bank, size_t index)
{
	const int slotX = static_cast<int>((index * 5U + 2U) % MaxSteamFogBanks);
	const int slotY = static_cast<int>((index * 9U + 3U) % MaxSteamFogBanks);
	const int stepX = FogWorldWrapW / static_cast<int>(MaxSteamFogBanks);
	const int stepY = FogWorldWrapH / static_cast<int>(MaxSteamFogBanks);

	bank.worldX = static_cast<float>((slotX * stepX + GenerateRnd(std::max(1, stepX))) % FogWorldWrapW);
	bank.worldY = static_cast<float>((slotY * stepY + GenerateRnd(std::max(1, stepY))) % FogWorldWrapH);
	bank.phase = static_cast<float>(GenerateRnd(628)) * 0.01F;
	bank.phaseSpeed = 0.42F + static_cast<float>(GenerateRnd(48)) * 0.01F;

	bank.speedMultX = 0.64F + static_cast<float>(GenerateRnd(79)) * 0.01F;
	bank.driftVelY = -9.5F + static_cast<float>(GenerateRnd(165)) * 0.1F;
	bank.swayAmpY = 10.0F + static_cast<float>(GenerateRnd(140)) * 0.1F;
	bank.swayFreq = 0.26F + static_cast<float>(GenerateRnd(32)) * 0.01F;

	const int shapeRoll = GenerateRnd(100);
	int baseRx = 48;
	int baseRy = 34;
	if (shapeRoll < 48) {
		baseRx = 38 + GenerateRnd(17);
		baseRy = 29 + GenerateRnd(14);
	} else if (shapeRoll < 82) {
		baseRx = 46 + GenerateRnd(19);
		baseRy = 26 + GenerateRnd(12);
	} else {
		baseRx = 54 + GenerateRnd(20);
		baseRy = 22 + GenerateRnd(10);
	}

	const int spreadX = (baseRx * 42) / 100;
	const int spreadY = (baseRy * 38) / 100;

	bank.puffOffX[0] = static_cast<int8_t>(-spreadX - GenerateRnd(7));
	bank.puffOffY[0] = static_cast<int8_t>(GenerateRnd(spreadY + 1) - (spreadY / 2));

	bank.puffOffX[1] = static_cast<int8_t>(-(spreadX / 4) + GenerateRnd(9) - 4);
	bank.puffOffY[1] = static_cast<int8_t>(-spreadY - GenerateRnd(5));

	bank.puffOffX[2] = static_cast<int8_t>((spreadX / 4) + GenerateRnd(9) - 4);
	bank.puffOffY[2] = static_cast<int8_t>(spreadY + GenerateRnd(5));

	bank.puffOffX[3] = static_cast<int8_t>(spreadX + GenerateRnd(7));
	bank.puffOffY[3] = static_cast<int8_t>(GenerateRnd(spreadY + 1) - (spreadY / 2));

	int maxExtentX = 24;
	int maxExtentY = 20;
	for (int i = 0; i < 4; ++i) {
		const int pctX = (i == 1 || i == 2) ? (88 + GenerateRnd(20)) : (74 + GenerateRnd(22));
		const int pctY = (i == 1 || i == 2) ? (92 + GenerateRnd(22)) : (78 + GenerateRnd(22));
		bank.puffRx[i] = static_cast<int16_t>(std::max(14, (baseRx * pctX) / 100));
		bank.puffRy[i] = static_cast<int16_t>(std::max(12, (baseRy * pctY) / 100));

		maxExtentX = std::max(maxExtentX, std::abs(static_cast<int>(bank.puffOffX[i])) + static_cast<int>(bank.puffRx[i]) + 8);
		maxExtentY = std::max(maxExtentY, std::abs(static_cast<int>(bank.puffOffY[i])) + static_cast<int>(bank.puffRy[i]) + 8);
	}
	bank.boundRx = static_cast<int16_t>(maxExtentX);
	bank.boundRy = static_cast<int16_t>(maxExtentY);

	const bool coolTint = (GenerateRnd(3) == 0);
	bank.colorIndex = static_cast<uint8_t>((coolTint ? PAL16_BLUE : PAL16_GRAY) + 6 + GenerateRnd(2));
}

void StopAllTownWeatherLoops()
{
	StopSFX(LS_RAINLIGHT);
	StopSFX(LS_RAINHEAVY);
	StopSFX(LS_SNOWLIGHT);
	StopSFX(LS_SNOWHEAVY);
	StopSFX(LS_ASHSTORMLIGHT);
	StopSFX(LS_ASHSTORMHEAVY);
}

void SyncWeatherAudio()
{
	const bool isMenuPaused = (PauseMode != 0 || (!gbIsMultiplayer && gmenu_is_active()));
	if (!gbRunGame || !gbSoundOn || isMenuPaused || leveltype != DTYPE_TOWN || setlevel) {
		StopAllTownWeatherLoops();
		return;
	}

	const bool isHeavy = (gHeavyBlend >= 0.5F);
	if (gTownWeatherKind == TownWeatherKind::Rain) {
		StopSFX(LS_SNOWLIGHT);
		StopSFX(LS_SNOWHEAVY);
		StopSFX(LS_ASHSTORMLIGHT);
		StopSFX(LS_ASHSTORMHEAVY);
		if (isHeavy) {
			StopSFX(LS_RAINLIGHT);
			PlayLoopingSFX(LS_RAINHEAVY, -200);
		} else {
			StopSFX(LS_RAINHEAVY);
			PlayLoopingSFX(LS_RAINLIGHT, -300);
		}
	} else if (gTownWeatherKind == TownWeatherKind::Snow) {
		StopSFX(LS_RAINLIGHT);
		StopSFX(LS_RAINHEAVY);
		StopSFX(LS_ASHSTORMLIGHT);
		StopSFX(LS_ASHSTORMHEAVY);
		if (isHeavy) {
			StopSFX(LS_SNOWLIGHT);
			PlayLoopingSFX(LS_SNOWHEAVY, -200);
		} else {
			StopSFX(LS_SNOWHEAVY);
			PlayLoopingSFX(LS_SNOWLIGHT, -300);
		}
	} else {
		StopSFX(LS_RAINLIGHT);
		StopSFX(LS_RAINHEAVY);
		StopSFX(LS_SNOWLIGHT);
		StopSFX(LS_SNOWHEAVY);
		if (isHeavy) {
			StopSFX(LS_ASHSTORMLIGHT);
			PlayLoopingSFX(LS_ASHSTORMHEAVY, -200);
		} else {
			StopSFX(LS_ASHSTORMHEAVY);
			PlayLoopingSFX(LS_ASHSTORMLIGHT, -300);
		}
	}
}

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
		PlotBlended50(out, center.x, center.y, specCol);
		PlotBlended25(out, center.x - 1, center.y, brightCol);
		PlotBlended25(out, center.x + 1, center.y, brightCol);
		PlotBlended25(out, center.x - 1, center.y - 2, specCol);
		PlotBlended25(out, center.x + 1, center.y - 2, specCol);
		break;
	}
	case 1: {
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

void DrawSingleSettlingSnowFlake(const Surface &out, Point center, uint8_t stage, uint8_t variant)
{
	if (center.x < -6 || center.x >= out.w() + 6 || center.y < -6 || center.y >= out.h() + 6)
		return;

	const bool isAshMode = (gTownWeatherKind == TownWeatherKind::AshStorm);
	const bool isEmber = isAshMode && ((variant & 7U) == 0U);
	const uint8_t brightCol = isEmber
	    ? static_cast<uint8_t>(PAL16_ORANGE + 3 + (variant & 1U))
	    : (isAshMode
	            ? static_cast<uint8_t>(PAL16_GRAY + 6 + (variant & 1U))
	            : static_cast<uint8_t>(PAL16_GRAY + 1 + (variant & 1U)));
	const uint8_t softCol = isEmber
	    ? static_cast<uint8_t>(PAL16_RED + 5 + (variant & 1U))
	    : (isAshMode
	            ? static_cast<uint8_t>(PAL16_GRAY + 9 + (variant & 1U))
	            : static_cast<uint8_t>(PAL16_BLUE + 3 + (variant & 1U)));

	switch (stage) {
	case 0:
	case 1:
	case 2:
		PlotBlended50(out, center.x, center.y, brightCol);
		PlotBlended25(out, center.x - 1, center.y, softCol);
		PlotBlended25(out, center.x + 1, center.y, softCol);
		PlotBlended19(out, center.x, center.y - 1, softCol);
		break;
	case 3:
		PlotBlended37(out, center.x, center.y, brightCol);
		PlotBlended19(out, center.x - 1, center.y, softCol);
		PlotBlended19(out, center.x + 1, center.y, softCol);
		break;
	default:
		PlotBlended19(out, center.x, center.y, softCol);
		PlotBlended12(out, center.x + 1, center.y, softCol);
		break;
	}
}

void DrawGroundSteamWisp(const Surface &out, Point basePos, uint32_t progressPermille, uint8_t variant)
{
	if (basePos.x < -24 || basePos.x >= out.w() + 24 || basePos.y < -8 || basePos.y >= out.h() + 24)
		return;

	const uint8_t coreCol = static_cast<uint8_t>(PAL16_GRAY + 2 + (variant & 1));
	const uint8_t haloCol = static_cast<uint8_t>(((variant & 2) != 0 ? PAL16_BLUE : PAL16_GRAY) + 3 + (variant & 1));
	const float phaseBase = static_cast<float>(variant) * 0.85F + static_cast<float>(progressPermille) * 0.00628F;

	for (int p = 0; p < 5; ++p) {
		const int puffProgress = static_cast<int>(progressPermille) - p * 120;
		if (puffProgress <= 0 || puffProgress >= 880)
			continue;

		const float t = static_cast<float>(puffProgress) / 880.0F;
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
		const float angle = bank.phase + static_cast<float>(i) * 1.5707963F;
		const float breathX = std::sin(angle);
		const float breathY = std::cos(angle * 0.85F);
		puffCx[i] = screenCx + static_cast<int>(bank.puffOffX[i]) + static_cast<int>(breathX * 5.0F);
		puffCy[i] = screenCy + static_cast<int>(bank.puffOffY[i]) + static_cast<int>(breathY * 4.5F);

		const int prx = std::max(12, static_cast<int>(bank.puffRx[i]) + static_cast<int>(breathY * 3.0F));
		const int pry = std::max(10, static_cast<int>(bank.puffRy[i]) + static_cast<int>(breathX * 2.5F));
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

	const int bankCx = (rawMinX + rawMaxX) / 2;
	const int bankCy = (rawMinY + rawMaxY) / 2;
	const int bankRx = std::max(16, (rawMaxX - rawMinX) / 2);
	const int bankRy = std::max(14, (rawMaxY - rawMinY) / 2);

	for (size_t r = 0; r < drawnCount; ++r) {
		const float dx = static_cast<float>(bankCx - drawnRects[r].cx);
		const float dy = static_cast<float>(bankCy - drawnRects[r].cy);
		const float sumRx = static_cast<float>(bankRx + drawnRects[r].rx + 8);
		const float sumRy = static_cast<float>(bankRy + drawnRects[r].ry + 8);
		if ((dx * dx) / (sumRx * sumRx) + (dy * dy) / (sumRy * sumRy) < 1.0F) {
			return;
		}
	}
	if (drawnCount < drawnRects.size()) {
		drawnRects[drawnCount++] = DrawnFogRect { bankCx, bankCy, bankRx, bankRy };
	}

	const float pulse = 0.85F + 0.15F * std::sin(bank.phase);
	const int coreCutoff = static_cast<int>(64.0F * pulse);

	for (int y = minY; y <= maxY; ++y) {
		for (int x = minX; x <= maxX; ++x) {
			const int waveOffsetY = (((x - screenCx) * 3 + (y - screenCy) * 2) >> 5) & 1;
			const int wy = y + waveOffsetY;

			int minD2 = 256;
			int secondMinD2 = 256;
			for (int i = 0; i < 4; ++i) {
				const int dy = wy - puffCy[i];
				const int dyNorm = (dy * dy * 256) / invRy2[i];
				if (dyNorm >= secondMinD2)
					continue;
				const int dx = x - puffCx[i];
				const int d2 = (dx * dx * 256) / invRx2[i] + dyNorm;
				if (d2 < minD2) {
					secondMinD2 = minD2;
					minD2 = d2;
				} else if (d2 < secondMinD2) {
					secondMinD2 = d2;
				}
			}

			if (secondMinD2 < 256) {
				minD2 = std::max(0, minD2 - ((256 - secondMinD2) >> 3));
			}

			if (minD2 >= 256)
				continue;

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
	StopSFX(LS_LAVABUBBLES);
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
		// Entering or changing levels inside a dungeon: stop Town weather audio & flash immediately
		RestoreNormalTownPalette();
		StopAllTownWeatherLoops();
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
		// Returning to Tristram: only allow intensity (Light vs Heavy) to change if player spent >= 1 minute in a dungeon
		StopSFX(LS_LAVABUBBLES);
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
	// NOTE: UpdateWeatherState() calls ResetWeatherState() on EVERY FRAME when in a dungeon (leveltype != DTYPE_TOWN)!
	// Never stop dungeon loops (LS_LAVABUBBLES) here, or they will be killed every frame!
	RestoreNormalTownPalette();
	StopAllTownWeatherLoops();
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
		if (gWeatherSimTick == 0) {
			gWeatherSimTick = nowTick;
		}
	}

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

	if (PauseMode != 0 || (!gbIsMultiplayer && gmenu_is_active())) {
		SyncWeatherAudio();
		return;
	}

	gWeatherSimTick += dtMs;
	const float dt = static_cast<float>(dtMs) * 0.001F;

	gHeavyBlend = (gCurrentRainMode == RainMode::Heavy) ? 1.0F : 0.0F;

	if (gTownWeatherKind == TownWeatherKind::Rain) {
		if (gPendingThunderTick != 0 && nowTick >= gPendingThunderTick) {
			gPendingThunderTick = 0;
			PlayWeatherSFX(gPendingThunderSfx, gPendingThunderVolume);
		}

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
	}

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

	const float baseFogWindX = -(15.0F + 11.0F * gHeavyBlend);
	const float wrapW = static_cast<float>(FogWorldWrapW);
	const float wrapH = static_cast<float>(FogWorldWrapH);
	const float halfW = wrapW * 0.5F;
	const float halfH = wrapH * 0.5F;

	for (size_t i = 0; i < gSteamFogBanks.size(); ++i) {
		SteamFogBank &bank = gSteamFogBanks[i];
		bank.phase += bank.phaseSpeed * dt;
		if (bank.phase > 6.2831853F) {
			bank.phase -= 6.2831853F;
		}

		const float waveVelY = std::sin(bank.phase * bank.swayFreq + static_cast<float>(i) * 1.1F) * bank.swayAmpY;
		const float waveVelX = std::cos(bank.phase * 0.5F + static_cast<float>(i) * 0.7F) * 3.5F;

		bank.worldX += (baseFogWindX * bank.speedMultX + waveVelX) * dt;
		bank.worldY += (bank.driftVelY + waveVelY) * dt;
	}

	for (int pass = 0; pass < 2; ++pass) {
		for (size_t i = 0; i < gSteamFogBanks.size(); ++i) {
			for (size_t j = i + 1; j < gSteamFogBanks.size(); ++j) {
				SteamFogBank &a = gSteamFogBanks[i];
				SteamFogBank &b = gSteamFogBanks[j];

				float dx = a.worldX - b.worldX;
				if (dx > halfW)
					dx -= wrapW;
				else if (dx < -halfW)
					dx += wrapW;

				float dy = a.worldY - b.worldY;
				if (dy > halfH)
					dy -= wrapH;
				else if (dy < -halfH)
					dy += wrapH;

				const float minSepX = static_cast<float>(a.boundRx + b.boundRx + 26);
				const float minSepY = static_cast<float>(a.boundRy + b.boundRy + 24);
				const float nx = dx / minSepX;
				const float ny = dy / minSepY;
				const float distSq = nx * nx + ny * ny;

				if (distSq < 1.0F) {
					const float dist = std::sqrt(std::max(0.0001F, distSq));
					const float overlap = 1.0F - dist;
					float dirX = nx / dist;
					float dirY = ny / dist;
					if (std::abs(dirY) < 0.35F) {
						dirY = (dirY >= 0.0F) ? 0.55F : -0.55F;
					}
					const float pushX = dirX * overlap * minSepX * 0.52F;
					const float pushY = dirY * overlap * minSepY * 0.58F;
					a.worldX += pushX;
					a.worldY += pushY;
					b.worldX -= pushX;
					b.worldY -= pushY;
				}
			}
		}
	}

	for (auto &bank : gSteamFogBanks) {
		while (bank.worldX < 0.0F) {
			bank.worldX += wrapW;
		}
		while (bank.worldX >= wrapW) {
			bank.worldX -= wrapW;
		}
		while (bank.worldY < 0.0F) {
			bank.worldY += wrapH;
		}
		while (bank.worldY >= wrapH) {
			bank.worldY -= wrapH;
		}
	}

	const bool isSnow = (gTownWeatherKind == TownWeatherKind::Snow);
	const bool isAsh = (gTownWeatherKind == TownWeatherKind::AshStorm);
	float fallVelY = 510.0F + 230.0F * gHeavyBlend;
	if (isSnow) {
		fallVelY = 74.0F + 142.0F * gHeavyBlend; // 74 px/s in SnowLight -> 216 px/s in SnowHeavy blizzard!
	} else if (isAsh) {
		fallVelY = 68.0F + 88.0F * gHeavyBlend;  // 68 px/s in AshStormLight -> 156 px/s in AshStormHeavy
	}

	const float simSec = static_cast<float>(gWeatherSimTick) * 0.001F;
	const float stormGust = std::sin(simSec * 1.45F) * 0.5F + std::cos(simSec * 0.62F) * 0.5F;
	const int marginX = 144;
	const float minX = -static_cast<float>(marginX);
	const float maxX = static_cast<float>(viewW + marginX);
	const float spanX = maxX - minX;

	const size_t activeDropCount = GetActiveWeatherDropCount();

	for (size_t i = 0; i < activeDropCount; ++i) {
		RainDrop &drop = gRainDrops[i];
		const float pScale = drop.parallaxScale;
		const float dropFallVel = fallVelY * drop.speedScale;
		float dropWindVel = 0.0F;
		if (isSnow) {
			const float flutterAmp = (16.0F + 20.0F * gHeavyBlend) * pScale;
			const float flutter = std::sin(simSec * (2.2F + drop.speedScale * (1.0F + gHeavyBlend)) + static_cast<float>(i) * 0.77F) * flutterAmp;
			const float baseSnowWind = -(18.0F + 108.0F * gHeavyBlend + stormGust * 28.0F * gHeavyBlend) * pScale;
			dropWindVel = baseSnowWind + flutter;
		} else if (isAsh) {
			const float eddy = std::sin(simSec * (1.8F + drop.speedScale) + static_cast<float>(i) * 0.91F) * (22.0F + 18.0F * gHeavyBlend) * pScale;
			const float baseAshWind = -(24.0F + 72.0F * gHeavyBlend + stormGust * 22.0F * gHeavyBlend) * pScale;
			dropWindVel = baseAshWind + eddy;
		} else {
			const float layerSlant = GetLayerBaseSlant(drop.layer, gHeavyBlend);
			dropWindVel = dropFallVel * layerSlant;
		}

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

	const int tileCenterX = targetBufferPosition.x + TILE_WIDTH / 2;
	const int tileCenterY = targetBufferPosition.y - TILE_HEIGHT / 2;
	if (tileCenterX < -32 || tileCenterX >= out.w() + 32 || tileCenterY < -16 || tileCenterY >= out.h() + 32)
		return;

	const uint32_t nowTick = gWeatherSimTick != 0 ? gWeatherSimTick : SDL_GetTicks();

	const uint32_t tileSeed = FastHash(
	    static_cast<uint32_t>(tilePosition.x) * 73856093U
	    ^ static_cast<uint32_t>(tilePosition.y) * 19349663U);

	const bool isSnowOrAsh = (gTownWeatherKind != TownWeatherKind::Rain);
	const uint32_t spawnChancePerMille = isSnowOrAsh
	    ? ((gHeavyBlend >= 0.5F) ? 404U : 184U) // +35% ground particle density for Snow & AshStorm
	    : ((gHeavyBlend >= 0.5F) ? 299U : 136U);
	// Snowflakes linger ~6.3x longer on the ground (1650 ms vs 260 ms splash) before melting away
	const uint32_t cycleDurationMs = isSnowOrAsh ? 1900U : 320U;
	const uint32_t animDurationMs = isSnowOrAsh ? 1650U : 260U;
	const uint32_t slotCount = isSnowOrAsh ? 3U : 2U;

	for (uint32_t slot = 0; slot < slotCount; ++slot) {
		const uint32_t slotSeed = FastHash(tileSeed ^ (slot * 0x9e3779b9U));
		const uint32_t phaseOffset = slotSeed % cycleDurationMs;
		const uint32_t shiftedTick = nowTick + phaseOffset;
		const uint32_t epoch = shiftedTick / cycleDurationMs;
		const uint32_t localMs = shiftedTick % cycleDurationMs;

		if (localMs >= animDurationMs)
			continue;

		const uint32_t epochHash = FastHash(slotSeed ^ (epoch * 0x85ebca6bU));
		if ((epochHash % 1000U) >= spawnChancePerMille)
			continue;

		const int subX = static_cast<int>((epochHash >> 10) % 33U) - 16;
		const int subY = static_cast<int>((epochHash >> 16) % 15U) - 7;
		const uint8_t variant = static_cast<uint8_t>((epochHash >> 22) & 7U);
		const uint8_t stage = static_cast<uint8_t>(std::min<uint32_t>(4U, (localMs * 5U) / animDurationMs));

		if (isSnowOrAsh) {
			DrawSingleSettlingSnowFlake(out, Point { tileCenterX + subX, tileCenterY + subY }, stage, variant);
		} else {
			DrawSinglePuddleSplash(out, Point { tileCenterX + subX, tileCenterY + subY }, stage, variant & 1U);
		}
	}

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
						maskRow[px] = 0;
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

	const size_t activeDropCount = GetActiveWeatherDropCount();

	const uint32_t nowTick = gWeatherSimTick != 0 ? gWeatherSimTick : SDL_GetTicks();
	const bool isSnow = (gTownWeatherKind == TownWeatherKind::Snow);
	const bool isAsh = (gTownWeatherKind == TownWeatherKind::AshStorm);
	const float gustPhase = static_cast<float>(nowTick) * 0.0025F;
	const float baseSlant = GetLayerBaseSlant(0, gHeavyBlend);
	const int extraLen = static_cast<int>(3.0F * gHeavyBlend);

	for (size_t i = 0; i < activeDropCount; ++i) {
		const RainDrop &drop = gRainDrops[i];
		if (drop.layer != 0)
			continue;

		const int headX = static_cast<int>(drop.x);
		const int headY = static_cast<int>(drop.y);

		if (isSnow || isAsh) {
			if (headY < 2 || headY >= viewH - 2 || headX < 2 || headX >= viewW - 2)
				continue;
			const bool isEmber = isAsh && ((i & 15U) == 0U);
			const uint8_t flakeCol = isEmber
			    ? static_cast<uint8_t>(PAL16_ORANGE + 3 + (i & 1U))
			    : (isAsh
			            ? static_cast<uint8_t>(PAL16_GRAY + 6 + (i & 3U))
			            : static_cast<uint8_t>(PAL16_GRAY + 1 + (i & 1U)));
			gBgRainMask[headY * viewW + headX] = static_cast<uint16_t>((2U << 8U) | flakeCol);
			gBgRainBlockGrid[(headY >> 4) * gBgRainBlockCols + (headX >> 5)] = 1;
			if (isSnow && gHeavyBlend >= 0.5F && headX + 1 < viewW && headY - 1 >= 0) {
				gBgRainMask[(headY - 1) * viewW + (headX + 1)] = static_cast<uint16_t>((1U << 8U) | flakeCol);
			} else if (((i + (nowTick >> 8)) & 1U) == 0U) {
				gBgRainMask[headY * viewW + (headX + 1)] = static_cast<uint16_t>((1U << 8U) | flakeCol);
			}
			continue;
		}

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

			const uint16_t blendMode = (step * 3 < totalLen) ? 2U : 1U;
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

	const size_t activeDropCount = GetActiveWeatherDropCount();

	const uint32_t nowTick = gWeatherSimTick != 0 ? gWeatherSimTick : SDL_GetTicks();
	const bool isSnow = (gTownWeatherKind == TownWeatherKind::Snow);
	const bool isAsh = (gTownWeatherKind == TownWeatherKind::AshStorm);
	const bool isHeavy = (gHeavyBlend >= 0.5F);
	const float gustPhase = static_cast<float>(nowTick) * 0.0032F;
	const float slant1 = GetLayerBaseSlant(1, gHeavyBlend);
	const float slant2 = GetLayerBaseSlant(2, gHeavyBlend);
	const int extraLen1 = static_cast<int>(6.0F * gHeavyBlend);
	const int extraLen2 = static_cast<int>(10.0F * gHeavyBlend);

	for (size_t i = 0; i < activeDropCount; ++i) {
		const RainDrop &drop = gRainDrops[i];
		if (drop.layer == 0)
			continue;

		const int headX = static_cast<int>(drop.x);
		const int headY = static_cast<int>(drop.y);

		if (isSnow || isAsh) {
			if (headY < 3 || headY >= out.h() - 3 || headX < 3 || headX >= out.w() - 4)
				continue;

			const bool isEmber = isAsh && ((i & 11U) == 0U);
			const uint8_t coreCol = isEmber
			    ? static_cast<uint8_t>(PAL16_ORANGE + 2 + (i & 1U))
			    : (isAsh
			            ? static_cast<uint8_t>(PAL16_GRAY + 5 + (i & 3U))
			            : static_cast<uint8_t>(PAL16_GRAY + 1 + (i & 1U)));
			const uint8_t rimCol = isEmber
			    ? static_cast<uint8_t>(PAL16_RED + 4 + (i & 1U))
			    : (isAsh
			            ? static_cast<uint8_t>(PAL16_BEIGE + 7 + (i & 1U))
			            : static_cast<uint8_t>(PAL16_BLUE + 2 + (i & 1U)));
			const uint32_t tumble = (static_cast<uint32_t>(i) + (nowTick >> 7)) % 3U;

			if (drop.layer == 1) {
				PlotBlended50(out, headX, headY, coreCol);
				if (isSnow && isHeavy) {
					// SnowHeavy (Snowstorm / Blizzard): wind-driven diagonal snow flurry streak + crystal head
					PlotBlended37(out, headX + 1, headY - 1, coreCol);
					PlotBlended25(out, headX + 2, headY - 1, rimCol);
					PlotBlended19(out, headX + 3, headY - 2, rimCol);
					PlotBlended25(out, headX, headY + 1, rimCol);
				} else if (tumble == 0U) {
					PlotBlended25(out, headX - 1, headY, rimCol);
					PlotBlended25(out, headX + 1, headY, rimCol);
					PlotBlended19(out, headX, headY - 1, rimCol);
					PlotBlended19(out, headX, headY + 1, rimCol);
				} else if (tumble == 1U) {
					PlotBlended25(out, headX - 1, headY - 1, rimCol);
					PlotBlended25(out, headX + 1, headY + 1, rimCol);
					PlotBlended19(out, headX + 1, headY, rimCol);
				} else {
					PlotBlended37(out, headX + 1, headY, coreCol);
					PlotBlended19(out, headX, headY - 1, rimCol);
					PlotBlended19(out, headX - 1, headY + 1, rimCol);
				}
			} else {
				PlotBlended37(out, headX, headY, coreCol);
				PlotBlended25(out, headX - 1, headY, coreCol);
				PlotBlended25(out, headX + 1, headY, coreCol);
				PlotBlended25(out, headX, headY - 1, coreCol);
				PlotBlended25(out, headX, headY + 1, coreCol);
				PlotBlended12(out, headX - 1, headY - 1, rimCol);
				PlotBlended12(out, headX + 1, headY - 1, rimCol);
				PlotBlended12(out, headX - 1, headY + 1, rimCol);
				PlotBlended12(out, headX + 1, headY + 1, rimCol);
				if (isSnow && isHeavy) {
					PlotBlended19(out, headX + 2, headY - 1, coreCol);
					PlotBlended12(out, headX + 3, headY - 2, rimCol);
				} else if (tumble != 2U) {
					PlotBlended12(out, headX - 2, headY, rimCol);
					PlotBlended12(out, headX + 2, headY, rimCol);
				}
			}
			continue;
		}

		const int totalLen = static_cast<int>(drop.length) + (drop.layer == 2 ? extraLen2 : extraLen1);

		if (headY < 0 || headY - totalLen >= out.h() || headX < -40 || headX >= out.w() + 40)
			continue;

		const float gust = std::sin(drop.x * 0.007F + gustPhase) * 0.035F;
		const float slantXPerY = (drop.layer == 2 ? slant2 : slant1) + gust;

		if (drop.layer == 1) {
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
			const uint8_t haloCol = static_cast<uint8_t>(drop.colorIndex + 2);
			for (int step = 0; step < totalLen; ++step) {
				const int py = headY - step;
				const int px = headX - static_cast<int>(static_cast<float>(step) * slantXPerY);
				if (px < 1 || px >= out.w() - 1 || py < 0 || py >= out.h())
					continue;

				if (step * 5 < totalLen) {
					PlotBlended50(out, px, py, drop.colorIndex);
					PlotBlended25(out, px + 1, py, haloCol);
					PlotBlended19(out, px - 1, py, haloCol);
				} else if (step * 2 < totalLen) {
					PlotBlended37(out, px, py, drop.colorIndex);
					PlotBlended25(out, px + 1, py, haloCol);
					PlotBlended19(out, px - 1, py, haloCol);
				} else if (step * 4 < totalLen * 3) {
					PlotBlended25(out, px, py, drop.colorIndex);
					PlotBlended19(out, px + 1, py, haloCol);
				} else {
					PlotBlended19(out, px, py, drop.colorIndex);
				}
			}
		}
	}
}

} // namespace devilution
