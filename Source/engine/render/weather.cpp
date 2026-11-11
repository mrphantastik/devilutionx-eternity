/**
 * @file weather.cpp
 *
 * Implementation of Tristram weather & environmental particle effects:
 * - Equal 1/3 probability New Game weather modes (Rain, Snow, AshStorm) locked per game session,
 *   toggling between Light & Heavy intensity only after >= 1 minute in a dungeon.
 * - Boosted Snow & AshStorm particle density (+35% air & ground, with 80% in visible midground/foreground
 *   planes so roofs/trees don't cull them) and longer-lingering (2200 ms) ground snowflakes.
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

constexpr size_t MaxRainDrops = 900;
constexpr size_t LightRainDropCount = 160;
constexpr size_t HeavyRainDropCount = 420;
constexpr size_t LightSnowAshDropCount = 380; // Visibly boosted Light Snow & Light AshStorm density
constexpr size_t HeavySnowAshDropCount = 850; // Visibly boosted Heavy Blizzard & Heavy AshStorm density

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
	const int layer0Cutoff = (gTownWeatherKind == TownWeatherKind::Rain) ? 42 : 20;
	const int layer1Cutoff = (gTownWeatherKind == TownWeatherKind::Rain) ? 84 : 74;
	if (roll < layer0Cutoff) {
		drop.layer = 0;
		drop.parallaxScale = 0.58F;
		drop.speedScale = 0.55F + static_cast<float>(GenerateRnd(18)) * 0.01F;
		drop.length = static_cast<uint8_t>(9 + GenerateRnd(5));
		drop.colorIndex = static_cast<uint8_t>((GenerateRnd(2) == 0 ? PAL16_BLUE : PAL16_GRAY) + 3 + GenerateRnd(2));
	} else if (roll < layer1Cutoff) {
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
	const bool isEmber = isAshMode && ((variant & 3U) == 0U);
	const uint8_t brightCol = isEmber
	    ? static_cast<uint8_t>(PAL16_ORANGE + 2 + (variant & 1U))
	    : (isAshMode
	            ? static_cast<uint8_t>(PAL16_GRAY + 5 + (variant & 1U))
	            : static_cast<uint8_t>(PAL16_GRAY + 1 + (variant & 1U)));
	const uint8_t softCol = isEmber
	    ? static_cast<uint8_t>(PAL16_RED + 4 + (variant & 1U))
	    : (isAshMode
	            ? static_cast<uint8_t>(PAL16_GRAY + 8 + (variant & 1U))
	            : static_cast<uint8_t>(PAL16_BLUE + 2 + (variant & 1U)));

	switch (stage) {
	case 0:
	case 1:
	case 2:
		PlotBlended75(out, center.x, center.y, brightCol);
		PlotBlended37(out, center.x - 1, center.y, softCol);
		PlotBlended37(out, center.x + 1, center.y, softCol);
		PlotBlended25(out, center.x, center.y - 1, softCol);
		PlotBlended25(out, center.x, center.y + 1, softCol);
		break;
	case 3:
		PlotBlended50(out, center.x, center.y, brightCol);
		PlotBlended25(out, center.x - 1, center.y, softCol);
		PlotBlended25(out, center.x + 1, center.y, softCol);
		break;
	default:
		PlotBlended25(out, center.x, center.y, softCol);
		PlotBlended19(out, center.x + 1, center.y, softCol);
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
		while (bank.worldX < 0.0F)
			bank.worldX += wrapW;
		while (bank.worldX >= wrapW)
			bank.worldX -= wrapW;
		while (bank.worldY < 0.0F)
			bank.worldY += wrapH;
		while (bank.worldY >= wrapH)
			bank.worldY -= wrapH;
	}

	const bool isSnowMode = (gTownWeatherKind == TownWeatherKind::Snow);
	const bool isAshMode = (gTownWeatherKind == TownWeatherKind::AshStorm);
	const bool isBlizzard = isSnowMode && (gHeavyBlend >= 0.5F);
	const float baseFallSpeed = isBlizzard
	    ? 395.0F
	    : (isSnowMode
	            ? 145.0F
	            : (isAshMode
	                    ? (130.0F + 95.0F * gHeavyBlend)
	                    : (460.0F + 190.0F * gHeavyBlend)));
	const int marginX = 144;
	const size_t activeDropCount = GetActiveWeatherDropCount();
	const float simSec = static_cast<float>(gWeatherSimTick) * 0.001F;

	for (size_t i = 0; i < activeDropCount; ++i) {
		RainDrop &drop = gRainDrops[i];
		const float vy = baseFallSpeed * drop.speedScale;
		float vx = vy * GetLayerBaseSlant(drop.layer, gHeavyBlend);
		if (isBlizzard) {
			const float gust = 1.0F + 0.32F * std::sin(simSec * 3.4F + static_cast<float>(i) * 0.19F);
			vx = -vy * (0.68F + 0.14F * static_cast<float>(drop.layer)) * gust;
		} else if (isSnowMode) {
			const float sway = std::sin(simSec * 2.1F + static_cast<float>(i) * 0.37F) * (24.0F * drop.parallaxScale);
			vx = vx * 0.45F + sway;
		} else if (isAshMode) {
			const float swirl = std::sin(simSec * 2.7F + static_cast<float>(i) * 0.53F) * (36.0F * drop.parallaxScale);
			vx = vx * (0.75F + 0.35F * gHeavyBlend) + swirl;
		}

		drop.x += vx * dt - static_cast<float>(camDeltaX) * drop.parallaxScale;
		drop.y += vy * dt - static_cast<float>(camDeltaY) * drop.parallaxScale;

		const float minX = static_cast<float>(-marginX);
		const float maxX = static_cast<float>(viewW + marginX);
		const float spanX = std::max(1.0F, maxX - minX);
		if (drop.x < minX) {
			drop.x += spanX;
		} else if (drop.x >= maxX) {
			drop.x -= spanX;
		}

		const float maxY = static_cast<float>(viewH + 64);
		if (drop.y >= maxY) {
			InitRainDrop(drop, viewW, viewH, /*randomY=*/false);
		} else if (drop.y < -64.0F) {
			drop.y += static_cast<float>(viewH + 96);
		}
	}

	SyncWeatherAudio();
}

void DrawWeatherGroundTile(const Surface &out, Point tilePosition, Point targetBufferPosition)
{
	if (!gbRunGame || leveltype != DTYPE_TOWN || setlevel || !gWeatherInitialized)
		return;
	if (!IsOutdoorWalkableGround(tilePosition))
		return;

	const uint32_t tileSeed = FastHash(static_cast<uint32_t>(tilePosition.x * 73856093 ^ tilePosition.y * 19349663));
	const uint32_t nowTick = gWeatherSimTick;

	if (gTownWeatherKind == TownWeatherKind::Rain && (tileSeed & 7U) == 0U) {
		const uint8_t wispVariant = static_cast<uint8_t>((tileSeed >> 4) & 3U);
		const uint32_t wispPeriodMs = 3000U + ((tileSeed >> 8) % 900U);
		const uint32_t wispPhaseMs = (nowTick + (tileSeed % wispPeriodMs)) % wispPeriodMs;
		const uint32_t activeWindowMs = (wispPeriodMs * 72U) / 100U;
		if (wispPhaseMs < activeWindowMs) {
			const uint32_t progressPermille = (wispPhaseMs * 1000U) / activeWindowMs;
			const int localX = 24 + static_cast<int>((tileSeed >> 12) % 17U);
			const int localY = -12 - static_cast<int>((tileSeed >> 17) % 9U);
			const Point basePos { targetBufferPosition.x + localX, targetBufferPosition.y + localY };
			DrawGroundSteamWisp(out, basePos, progressPermille, wispVariant);
		}
	}

	const bool isHeavy = (gHeavyBlend >= 0.5F);
	const bool isSnowOrAsh = (gTownWeatherKind == TownWeatherKind::Snow || gTownWeatherKind == TownWeatherKind::AshStorm);
	// Boosted ground particle slots (5 slots in Heavy, 3 in Light) and spawn threshold (520‰ Heavy / 280‰ Light)
	const int maxSlots = isSnowOrAsh ? (isHeavy ? 5 : 3) : (isHeavy ? 2 : 1);
	const uint32_t spawnThresholdPermille = isSnowOrAsh
	    ? (isHeavy ? 520U : 280U)
	    : (isHeavy ? 260U : 120U);

	// Longer-lingering ground snowflakes (2200 ms active out of 2500 ms cycle)
	const uint32_t cycleDurationMs = (gTownWeatherKind == TownWeatherKind::Snow)
	    ? 2500U
	    : ((gTownWeatherKind == TownWeatherKind::AshStorm) ? 1100U : 520U);
	const uint32_t animDurationMs = (gTownWeatherKind == TownWeatherKind::Snow)
	    ? 2200U
	    : ((gTownWeatherKind == TownWeatherKind::AshStorm) ? 850U : 260U);

	for (int slot = 0; slot < maxSlots; ++slot) {
		const uint32_t slotSeed = FastHash(tileSeed ^ static_cast<uint32_t>(slot * 0x9e3779b9U));
		const uint32_t cycleIdx = (nowTick + (slotSeed % cycleDurationMs)) / cycleDurationMs;
		const uint32_t cycleRoll = FastHash(slotSeed ^ (cycleIdx * 0x85ebca6bU));

		if ((cycleRoll % 1000U) >= spawnThresholdPermille)
			continue;

		const uint32_t elapsedInCycle = (nowTick + (slotSeed % cycleDurationMs)) % cycleDurationMs;
		if (elapsedInCycle >= animDurationMs)
			continue;

		const uint8_t stage = static_cast<uint8_t>((elapsedInCycle * 5U) / animDurationMs);
		const uint8_t variant = static_cast<uint8_t>((cycleRoll >> 10) & 3U);

		const int localX = 20 + static_cast<int>((cycleRoll >> 14) % 25U);
		const int localY = -10 - static_cast<int>((cycleRoll >> 20) % 13U);

		const Point splashCenter {
			targetBufferPosition.x + localX,
			targetBufferPosition.y + localY
		};
		if (gTownWeatherKind == TownWeatherKind::Rain) {
			DrawSinglePuddleSplash(out, splashCenter, stage, variant);
		} else {
			DrawSingleSettlingSnowFlake(out, splashCenter, stage, static_cast<uint8_t>((cycleRoll >> 8) & 0xFFU));
		}
	}
}

void DrawWeatherBackground(const Surface &out, Point /*startTilePosition*/, Displacement /*offset*/)
{
	if (!gbRunGame || leveltype != DTYPE_TOWN || setlevel || !gWeatherInitialized)
		return;

	const int viewW = out.w();
	const int viewH = out.h();
	if (viewW <= 0 || viewH <= 0)
		return;

	const size_t pixelCount = static_cast<size_t>(viewW) * static_cast<size_t>(viewH);
	if (gBgRainMask.size() != pixelCount || gBgRainMaskW != viewW || gBgRainMaskH != viewH) {
		gBgRainMaskW = viewW;
		gBgRainMaskH = viewH;
		gBgRainMask.assign(pixelCount, 0xFFFFU);
	} else {
		std::fill(gBgRainMask.begin(), gBgRainMask.end(), static_cast<uint16_t>(0xFFFFU));
	}

	gBgRainBlockCols = (viewW + 15) >> 4;
	gBgRainBlockRows = (viewH + 15) >> 4;
	const size_t blockCount = static_cast<size_t>(gBgRainBlockCols) * static_cast<size_t>(gBgRainBlockRows);
	if (gBgRainBlockGrid.size() != blockCount) {
		gBgRainBlockGrid.assign(blockCount, 0U);
	} else {
		std::fill(gBgRainBlockGrid.begin(), gBgRainBlockGrid.end(), 0U);
	}

	const bool isSnowMode = (gTownWeatherKind == TownWeatherKind::Snow);
	const bool isAshMode = (gTownWeatherKind == TownWeatherKind::AshStorm);
	const bool isBlizzard = isSnowMode && (gHeavyBlend >= 0.5F);
	const float slant0 = isBlizzard ? -0.68F : GetLayerBaseSlant(0, gHeavyBlend);
	const size_t activeDropCount = GetActiveWeatherDropCount();

	for (size_t idx = 0; idx < activeDropCount; ++idx) {
		const RainDrop &drop = gRainDrops[idx];
		if (drop.layer != 0)
			continue;

		const int x0 = static_cast<int>(drop.x);
		const int y0 = static_cast<int>(drop.y);
		if (isSnowMode || isAshMode) {
			if (x0 >= 0 && x0 < viewW && y0 >= 0 && y0 < viewH) {
				const bool isEmber = isAshMode && ((idx & 7U) == 0U);
				const uint8_t flakeCol = isEmber
				    ? static_cast<uint8_t>(PAL16_ORANGE + 3 + (idx & 1U))
				    : (isAshMode
				            ? static_cast<uint8_t>(PAL16_GRAY + 7 + (idx & 1U))
				            : static_cast<uint8_t>(PAL16_GRAY + 2 + (idx & 1U)));
				const uint8_t blended = isEmber ? Blend50(flakeCol, out[{ x0, y0 }]) : Blend37(flakeCol, out[{ x0, y0 }]);
				out[{ x0, y0 }] = blended;
				gBgRainMask[static_cast<size_t>(y0) * static_cast<size_t>(viewW) + static_cast<size_t>(x0)]
				    = static_cast<uint16_t>((static_cast<uint16_t>(out[{ x0, y0 }]) << 8) | blended);
				gBgRainBlockGrid[static_cast<size_t>(y0 >> 4) * static_cast<size_t>(gBgRainBlockCols) + static_cast<size_t>(x0 >> 4)] = 1U;
			}
			continue;
		}

		const int len = static_cast<int>(drop.length);
		for (int i = 0; i < len; ++i) {
			const int px = x0 + static_cast<int>(static_cast<float>(i) * slant0);
			const int py = y0 + i;
			if (px < 0 || px >= viewW || py < 0 || py >= viewH)
				continue;

			const uint8_t origPx = out[{ px, py }];
			const uint8_t blendedPx = (i * 3 < len)
			    ? Blend12(drop.colorIndex, origPx)
			    : Blend25(drop.colorIndex, origPx);

			gBgRainMask[static_cast<size_t>(py) * static_cast<size_t>(viewW) + static_cast<size_t>(px)]
			    = static_cast<uint16_t>((static_cast<uint16_t>(origPx) << 8) | blendedPx);
			gBgRainBlockGrid[static_cast<size_t>(py >> 4) * static_cast<size_t>(gBgRainBlockCols) + static_cast<size_t>(px >> 4)] = 1U;
		}
	}
}

void DrawWeatherAir(const Surface &out, Point startTilePosition, Displacement offset)
{
	if (!gbRunGame || leveltype != DTYPE_TOWN || setlevel || !gWeatherInitialized)
		return;

	const int viewW = out.w();
	const int viewH = out.h();
	if (viewW <= 0 || viewH <= 0)
		return;

	// 1. Apply deferred Layer 0 background drops only on pixels where out[{x, y}] still equals origPx
	if (gBgRainMaskW == viewW && gBgRainMaskH == viewH && gBgRainMask.size() == static_cast<size_t>(viewW) * static_cast<size_t>(viewH)) {
		for (int by = 0; by < gBgRainBlockRows; ++by) {
			const size_t blockRowOffset = static_cast<size_t>(by) * static_cast<size_t>(gBgRainBlockCols);
			for (int bx = 0; bx < gBgRainBlockCols; ++bx) {
				if (gBgRainBlockGrid[blockRowOffset + static_cast<size_t>(bx)] == 0U)
					continue;

				const int yStart = by << 4;
				const int yEnd = std::min(viewH, yStart + 16);
				const int xStart = bx << 4;
				const int xEnd = std::min(viewW, xStart + 16);

				for (int y = yStart; y < yEnd; ++y) {
					const size_t rowOffset = static_cast<size_t>(y) * static_cast<size_t>(viewW);
					for (int x = xStart; x < xEnd; ++x) {
						const uint16_t entry = gBgRainMask[rowOffset + static_cast<size_t>(x)];
						if (entry == 0xFFFFU)
							continue;
						const uint8_t origPx = static_cast<uint8_t>(entry >> 8);
						uint8_t &curPx = out[{ x, y }];
						if (curPx == origPx) {
							curPx = static_cast<uint8_t>(entry & 0xFFU);
						}
					}
				}
			}
		}
	}

	// 2. Non-overlapping drifting precipitation/steam fog banks
	const Point camWorld = CalcCameraWorldPixel(startTilePosition, offset);
	const size_t activeFogBanks = std::min<size_t>(
	    MaxSteamFogBanks,
	    static_cast<size_t>(
	        static_cast<float>(LightRainFogBankCount)
	        + static_cast<float>(HeavyRainFogBankCount - LightRainFogBankCount) * gHeavyBlend + 0.5F));

	std::array<DrawnFogRect, 32> drawnFogRects {};
	size_t drawnFogCount = 0;

	for (size_t i = 0; i < activeFogBanks; ++i) {
		const SteamFogBank &bank = gSteamFogBanks[i];
		const int baseScreenX = PositiveMod(static_cast<int>(bank.worldX) - camWorld.x, FogWorldWrapW);
		const int baseScreenY = PositiveMod(static_cast<int>(bank.worldY) - camWorld.y, FogWorldWrapH);

		for (int ox = -FogWorldWrapW; ox <= FogWorldWrapW; ox += FogWorldWrapW) {
			const int sx = baseScreenX + ox;
			if (sx + bank.boundRx < 0 || sx - bank.boundRx >= viewW)
				continue;

			for (int oy = -FogWorldWrapH; oy <= FogWorldWrapH; oy += FogWorldWrapH) {
				const int sy = baseScreenY + oy;
				if (sy + bank.boundRy < 0 || sy - bank.boundRy >= viewH)
					continue;

				DrawSingleSteamFogBank(out, bank, sx, sy, drawnFogRects, drawnFogCount);
			}
		}
	}

	// 3. Midground (Layer 1) and Near-Camera DoF Blurred Foreground (Layer 2)
	const bool isSnowMode = (gTownWeatherKind == TownWeatherKind::Snow);
	const bool isAshMode = (gTownWeatherKind == TownWeatherKind::AshStorm);
	const bool isBlizzard = isSnowMode && (gHeavyBlend >= 0.5F);
	const float slant1 = isBlizzard ? -0.78F : GetLayerBaseSlant(1, gHeavyBlend);
	const float slant2 = isBlizzard ? -0.92F : GetLayerBaseSlant(2, gHeavyBlend);
	const size_t activeDropCount = GetActiveWeatherDropCount();

	for (size_t idx = 0; idx < activeDropCount; ++idx) {
		const RainDrop &drop = gRainDrops[idx];
		if (drop.layer == 0)
			continue;

		const int x0 = static_cast<int>(drop.x);
		const int y0 = static_cast<int>(drop.y);
		const int len = static_cast<int>(drop.length);

		if (isSnowMode || isAshMode) {
			if (x0 < -4 || x0 >= viewW + 4 || y0 < -4 || y0 >= viewH + 4)
				continue;
			const bool isEmber = isAshMode && ((idx % 5U) == 0U);
			const uint8_t coreCol = isEmber
			    ? static_cast<uint8_t>(PAL16_ORANGE + 1 + (idx & 1U))
			    : (isAshMode
			            ? static_cast<uint8_t>(PAL16_GRAY + 5 + (idx & 1U))
			            : static_cast<uint8_t>(PAL16_GRAY + 1 + (idx & 1U)));
			const uint8_t haloCol = isEmber
			    ? static_cast<uint8_t>(PAL16_RED + 3 + (idx & 1U))
			    : (isAshMode
			            ? static_cast<uint8_t>(PAL16_GRAY + 8 + (idx & 1U))
			            : static_cast<uint8_t>(PAL16_BLUE + 2 + (idx & 1U)));

			if (drop.layer == 1) {
				if (isBlizzard) {
					PlotBlended75(out, x0, y0, coreCol);
					PlotBlended50(out, x0 - 1, y0, coreCol);
					PlotBlended37(out, x0 + 1, y0 - 1, haloCol);
					PlotBlended25(out, x0 + 2, y0 - 2, haloCol);
				} else if (isEmber) {
					PlotBlended75(out, x0, y0, coreCol);
					PlotBlended50(out, x0 + 1, y0, haloCol);
					PlotBlended37(out, x0, y0 - 1, haloCol);
				} else {
					PlotBlended75(out, x0, y0, coreCol);
					PlotBlended37(out, x0 - 1, y0, haloCol);
					PlotBlended37(out, x0 + 1, y0, haloCol);
					PlotBlended25(out, x0, y0 - 1, haloCol);
					PlotBlended25(out, x0, y0 + 1, haloCol);
				}
			} else {
				if (isBlizzard) {
					PlotBlended75(out, x0, y0, coreCol);
					PlotBlended50(out, x0 - 1, y0, coreCol);
					PlotBlended50(out, x0, y0 - 1, coreCol);
					PlotBlended37(out, x0 + 1, y0 - 1, haloCol);
					PlotBlended25(out, x0 + 2, y0 - 2, haloCol);
					PlotBlended19(out, x0 + 3, y0 - 3, haloCol);
				} else {
					PlotBlended75(out, x0, y0, coreCol);
					PlotBlended50(out, x0 - 1, y0, coreCol);
					PlotBlended50(out, x0 + 1, y0, coreCol);
					PlotBlended50(out, x0, y0 - 1, coreCol);
					PlotBlended50(out, x0, y0 + 1, coreCol);
					PlotBlended25(out, x0 - 1, y0 - 1, haloCol);
					PlotBlended25(out, x0 + 1, y0 + 1, haloCol);
				}
			}
			continue;
		}

		if (drop.layer == 1) {
			for (int i = 0; i < len; ++i) {
				const int px = x0 + static_cast<int>(static_cast<float>(i) * slant1);
				const int py = y0 + i;
				if (i * 3 < len) {
					PlotBlended25(out, px, py, drop.colorIndex);
				} else {
					PlotBlended50(out, px, py, drop.colorIndex);
				}
			}
		} else {
			for (int i = 0; i < len; ++i) {
				const int px = x0 + static_cast<int>(static_cast<float>(i) * slant2);
				const int py = y0 + i;

				if (i * 4 < len || i * 4 > len * 3) {
					PlotBlended12(out, px, py, drop.colorIndex);
					PlotBlended12(out, px + 1, py, drop.colorIndex);
				} else {
					PlotBlended25(out, px, py, drop.colorIndex);
					PlotBlended12(out, px - 1, py, drop.colorIndex);
					PlotBlended12(out, px + 1, py, drop.colorIndex);
				}
			}
		}
	}
}

} // namespace devilution
