/**
 * @file palette.cpp
 *
 * Implementation of functions for handling the engines color palette.
 */
#include "engine/palette.h"

#include <cmath>
#include <cstdint>

#include <fmt/core.h>

#include "engine/assets.hpp"
#include "engine/backbuffer_state.hpp"
#include "engine/demomode.h"
#include "engine/dx.h"
#include "engine/load_file.hpp"
#include "engine/random.hpp"
#include "hwcursor.hpp"
#include "levels/gendung.h"
#include "options.h"
#include "utils/display.h"
#include "utils/sdl_compat.h"

namespace devilution {

std::array<SDL_Color, 256> logical_palette;
std::array<SDL_Color, 256> system_palette;
std::array<SDL_Color, 256> orig_palette;
std::array<std::array<Uint8, 256>, 256> paletteTransparencyLookup;

#if DEVILUTIONX_PALETTE_TRANSPARENCY_BLACK_16_LUT
uint16_t paletteTransparencyLookupBlack16[65536];
#endif

namespace {

/** Specifies whether the palette has max brightness. */
bool sgbFadedIn = true;

void LoadGamma()
{
	int gammaValue = *sgOptions.Graphics.gammaCorrection;
	gammaValue = clamp(gammaValue, 30, 100);
	sgOptions.Graphics.gammaCorrection.SetValue(gammaValue - gammaValue % 5);
}

Uint8 FindBestMatchForColor(std::array<SDL_Color, 256> &palette, SDL_Color color, int skipFrom, int skipTo)
{
	Uint8 best;
	Uint32 bestDiff = SDL_MAX_UINT32;
	for (int i = 0; i < 256; i++) {
		if (i >= skipFrom && i <= skipTo)
			continue;
		int diffr = palette[i].r - color.r;
		int diffg = palette[i].g - color.g;
		int diffb = palette[i].b - color.b;
		Uint32 diff = diffr * diffr + diffg * diffg + diffb * diffb;

		if (bestDiff > diff) {
			best = i;
			bestDiff = diff;
		}
	}
	return best;
}

/**
 * @brief Generate lookup table for transparency
 *
 * This is based of the same technique found in Quake2.
 *
 * To mimic 50% transparency we figure out what colors in the existing palette are the best match for the combination of any 2 colors.
 * We save this into a lookup table for use during rendering.
 *
 * @param palette The colors to operate on
 * @param skipFrom Do not use colors between this index and skipTo
 * @param skipTo Do not use colors between skipFrom and this index
 * @param toUpdate Only update the first n colors
 */
void GenerateBlendedLookupTable(std::array<SDL_Color, 256> &palette, int skipFrom, int skipTo, int toUpdate = 256)
{
	for (int i = 0; i < 256; i++) {
		for (int j = 0; j < 256; j++) {
			if (i == j) { // No need to calculate transparency between 2 identical colors
				paletteTransparencyLookup[i][j] = j;
				continue;
			}
			if (i > j) { // Half the blends will be mirror identical ([i][j] is the same as [j][i]), so simply copy the existing combination.
				paletteTransparencyLookup[i][j] = paletteTransparencyLookup[j][i];
				continue;
			}
			if (i > toUpdate && j > toUpdate) {
				continue;
			}

			SDL_Color blendedColor;
			blendedColor.r = ((int)palette[i].r + (int)palette[j].r) / 2;
			blendedColor.g = ((int)palette[i].g + (int)palette[j].g) / 2;
			blendedColor.b = ((int)palette[i].b + (int)palette[j].b) / 2;
			Uint8 best = FindBestMatchForColor(palette, blendedColor, skipFrom, skipTo);
			paletteTransparencyLookup[i][j] = best;
		}
	}

#if DEVILUTIONX_PALETTE_TRANSPARENCY_BLACK_16_LUT
	for (unsigned i = 0; i < 256; ++i) {
		for (unsigned j = 0; j < 256; ++j) {
			const std::uint16_t index = i | (j << 8);
			paletteTransparencyLookupBlack16[index] = paletteTransparencyLookup[0][i] | (paletteTransparencyLookup[0][j] << 8);
		}
	}
#endif
}

/**
 * @brief Cycle the given range of colors in the palette
 * @param from First color index of the range
 * @param to First color index of the range
 */
void CycleColors(int from, int to)
{
	std::rotate(system_palette.begin() + from, system_palette.begin() + from + 1, system_palette.begin() + to + 1);

	for (auto &palette : paletteTransparencyLookup) {
		std::rotate(palette.begin() + from, palette.begin() + from + 1, palette.begin() + to + 1);
	}

	std::rotate(paletteTransparencyLookup.begin() + from, paletteTransparencyLookup.begin() + from + 1, paletteTransparencyLookup.begin() + to + 1);
}

/**
 * @brief Cycle the given range of colors in the palette in reverse direction
 * @param from First color index of the range
 * @param to Last color index of the range
 */
void CycleColorsReverse(int from, int to)
{
	std::rotate(system_palette.begin() + from, system_palette.begin() + to, system_palette.begin() + to + 1);

	for (auto &palette : paletteTransparencyLookup) {
		std::rotate(palette.begin() + from, palette.begin() + to, palette.begin() + to + 1);
	}

	std::rotate(paletteTransparencyLookup.begin() + from, paletteTransparencyLookup.begin() + to, paletteTransparencyLookup.begin() + to + 1);
}

/**
 * @brief Checks if a custom palette asset exists in MPQs or save/data paths.
 */
bool AssetPaletteExists(const char *pszFileName)
{
	AssetRef ref = FindAsset(pszFileName);
	return ref.ok() && ref.size() >= 768;
}

/**
 * @brief Deterministic Murmur3-style hash from the current level's seed and depth
 * so palette selection never perturbs the gameplay RNG (GenerateRnd).
 */
uint32_t GetLevelPaletteHash(dungeon_type l)
{
	uint32_t h = (currlevel < NUMLEVELS ? static_cast<uint32_t>(glSeedTbl[currlevel]) : 0U);
	if (h == 0 && NUMLEVELS > 1)
		h = static_cast<uint32_t>(glSeedTbl[1]);
	h ^= static_cast<uint32_t>(currlevel) * 0x9E3779B9U;
	h ^= static_cast<uint32_t>(l) * 0x85EBCA6BU;
	h ^= h >> 16;
	h *= 0x85EBCA6BU;
	h ^= h >> 13;
	h *= 0xC2B2AE35U;
	h ^= h >> 16;
	return h;
}

/**
 * @brief Fallback procedural hue/saturation/contrast shift on level-specific
 * palette entries (indices 1..127) when external DevilutionXRandomizer .pal
 * files are not present, preserving global UI/item/monster indices 0 and 128..255.
 */
void ApplyProceduralPaletteShift(uint32_t hash)
{
	// Derive hue rotation (in degrees, avoiding near-0 no-op shifts) and saturation scale
	const float hueShiftDeg = 25.0F + static_cast<float>(hash % 310U);
	const float satScale = 0.80F + static_cast<float>((hash >> 9) % 55U) * 0.01F; // 0.80 .. 1.34

	for (int i = 1; i < 128; i++) {
		float r = orig_palette[i].r / 255.0F;
		float g = orig_palette[i].g / 255.0F;
		float b = orig_palette[i].b / 255.0F;

		float cmax = std::max({ r, g, b });
		float cmin = std::min({ r, g, b });
		float delta = cmax - cmin;

		float h = 0.0F;
		if (delta > 0.0001F) {
			if (cmax == r)
				h = 60.0F * std::fmod(((g - b) / delta), 6.0F);
			else if (cmax == g)
				h = 60.0F * (((b - r) / delta) + 2.0F);
			else
				h = 60.0F * (((r - g) / delta) + 4.0F);
			if (h < 0.0F)
				h += 360.0F;
		}
		float s = (cmax <= 0.0001F) ? 0.0F : (delta / cmax);
		float v = cmax;

		// If the base tile color is nearly pure greyscale (e.g. Cathedral/Crypt stone),
		// inject a subtle atmospheric tint so the randomized palette is still distinct.
		if (s < 0.12F && v > 0.05F) {
			s = std::min(1.0F, s + 0.18F);
		}

		h = std::fmod(h + hueShiftDeg, 360.0F);
		s = clamp(s * satScale, 0.0F, 1.0F);

		float c = v * s;
		float x = c * (1.0F - std::fabs(std::fmod(h / 60.0F, 2.0F) - 1.0F));
		float m = v - c;

		float rp = 0.0F;
		float gp = 0.0F;
		float bp = 0.0F;
		if (h < 60.0F) {
			rp = c;
			gp = x;
		} else if (h < 120.0F) {
			rp = x;
			gp = c;
		} else if (h < 180.0F) {
			gp = c;
			bp = x;
		} else if (h < 240.0F) {
			gp = x;
			bp = c;
		} else if (h < 300.0F) {
			rp = x;
			bp = c;
		} else {
			rp = c;
			bp = x;
		}

		orig_palette[i].r = static_cast<Uint8>(clamp(static_cast<int>((rp + m) * 255.0F + 0.5F), 0, 255));
		orig_palette[i].g = static_cast<Uint8>(clamp(static_cast<int>((gp + m) * 255.0F + 0.5F), 0, 255));
		orig_palette[i].b = static_cast<Uint8>(clamp(static_cast<int>((bp + m) * 255.0F + 0.5F), 0, 255));
	}

	if (leveltype == DTYPE_CAVES || leveltype == DTYPE_CRYPT) {
		GenerateBlendedLookupTable(orig_palette, 1, 31);
	} else if (leveltype == DTYPE_NEST) {
		GenerateBlendedLookupTable(orig_palette, 1, 15);
	} else {
		GenerateBlendedLookupTable(orig_palette, -1, -1);
	}
}

} // namespace

void palette_update(int first, int ncolor)
{
	if (HeadlessMode)
		return;

	assert(Palette);
	if (SDLC_SetSurfaceAndPaletteColors(PalSurface, Palette.get(), system_palette.data(), first, ncolor) < 0) {
		ErrSdl();
	}
	pal_surface_palette_version++;
}

void ApplyGamma(std::array<SDL_Color, 256> &dst, const std::array<SDL_Color, 256> &src, int n)
{
	double g = *sgOptions.Graphics.gammaCorrection / 100.0;

	for (int i = 0; i < n; i++) {
		dst[i].r = static_cast<Uint8>(pow(src[i].r / 256.0, g) * 256.0);
		dst[i].g = static_cast<Uint8>(pow(src[i].g / 256.0, g) * 256.0);
		dst[i].b = static_cast<Uint8>(pow(src[i].b / 256.0, g) * 256.0);
	}
	RedrawEverything();
}

void palette_init()
{
	LoadGamma();
	system_palette = orig_palette;
	InitPalette();
}

void LoadPalette(const char *pszFileName, bool blend /*= true*/)
{
	assert(pszFileName);

	if (HeadlessMode)
		return;

	struct Color {
		uint8_t r;
		uint8_t g;
		uint8_t b;
	};

	std::array<Color, 256> palData;

	LoadFileInMem(pszFileName, palData);

	for (unsigned i = 0; i < palData.size(); i++) {
		orig_palette[i].r = palData[i].r;
		orig_palette[i].g = palData[i].g;
		orig_palette[i].b = palData[i].b;
#ifndef USE_SDL1
		orig_palette[i].a = SDL_ALPHA_OPAQUE;
#endif
	}

	if (blend) {
		if (leveltype == DTYPE_CAVES || leveltype == DTYPE_CRYPT) {
			GenerateBlendedLookupTable(orig_palette, 1, 31);
		} else if (leveltype == DTYPE_NEST) {
			GenerateBlendedLookupTable(orig_palette, 1, 15);
		} else {
			GenerateBlendedLookupTable(orig_palette, -1, -1);
		}
	}
}

void LoadRndLvlPal(dungeon_type l)
{
	if (HeadlessMode)
		return;

	// Preserve exact vanilla GenerateRnd(4) call count for non-town levels
	// so dungeon generation seeds and multiplayer sync remain 100% identical.
	int rv = 1;
	if (l != DTYPE_TOWN) {
		rv = GenerateRnd(4) + 1;
	}

	if (*sgOptions.Graphics.randomizePalette) {
		const uint32_t hash = GetLevelPaletteHash(l);
		char szRndFileName[64];

		struct RndPalInfo {
			const char *folder;
			const char *prefix;
			uint32_t count;
		};

		RndPalInfo info = { "towndata", "town", 5 };
		switch (l) {
		case DTYPE_TOWN:
			info = { "towndata", "town", 5 };
			break;
		case DTYPE_CATHEDRAL:
			info = { "l1data", "l1", 66 };
			break;
		case DTYPE_CATACOMBS:
			info = { "l2data", "l2", 74 };
			break;
		case DTYPE_CAVES:
			info = { "l3data", "l3", 58 };
			break;
		case DTYPE_HELL:
			info = { "l4data", "l4", 46 };
			break;
		case DTYPE_NEST:
			info = { "l6data", "l6", 79 };
			break;
		case DTYPE_CRYPT:
			info = { "l5data", "l5", 74 };
			break;
		default:
			break;
		}

		const uint32_t rndIdx = (hash % info.count) + 1;
		*fmt::format_to(szRndFileName, R"(palette\{0}\{1}_{2}.pal)", info.folder, info.prefix, rndIdx) = '\0';

		if (AssetPaletteExists(szRndFileName)) {
			LoadPalette(szRndFileName);
			return;
		}
	}

	if (l == DTYPE_TOWN) {
		LoadPalette("levels\\towndata\\town.pal");
		if (*sgOptions.Graphics.randomizePalette)
			ApplyProceduralPaletteShift(GetLevelPaletteHash(l));
		return;
	}

	if (l == DTYPE_CRYPT) {
		LoadPalette("nlevels\\l5data\\l5base.pal");
		if (*sgOptions.Graphics.randomizePalette)
			ApplyProceduralPaletteShift(GetLevelPaletteHash(l));
		return;
	}

	char szFileName[27];
	if (l == DTYPE_NEST) {
		if (!*sgOptions.Graphics.alternateNestArt) {
			rv++;
		}
		*fmt::format_to(szFileName, R"(nlevels\l{0}data\l{0}base{1}.pal)", 6, rv) = '\0';
	} else {
		*fmt::format_to(szFileName, R"(levels\l{0}data\l{0}_{1}.pal)", static_cast<int>(l), rv) = '\0';
	}
	LoadPalette(szFileName);
	if (*sgOptions.Graphics.randomizePalette)
		ApplyProceduralPaletteShift(GetLevelPaletteHash(l));
}

void IncreaseGamma()
{
	int gammaValue = *sgOptions.Graphics.gammaCorrection;
	if (gammaValue < 100) {
		sgOptions.Graphics.gammaCorrection.SetValue(std::min(gammaValue + 5, 100));
		ApplyGamma(system_palette, logical_palette, 256);
		palette_update();
	}
}

void DecreaseGamma()
{
	int gammaValue = *sgOptions.Graphics.gammaCorrection;
	if (gammaValue > 30) {
		sgOptions.Graphics.gammaCorrection.SetValue(std::max(gammaValue - 5, 30));
		ApplyGamma(system_palette, logical_palette, 256);
		palette_update();
	}
}

int UpdateGamma(int gamma)
{
	if (gamma > 0) {
		sgOptions.Graphics.gammaCorrection.SetValue(130 - gamma);
		ApplyGamma(system_palette, logical_palette, 256);
		palette_update();
	}
	return 130 - *sgOptions.Graphics.gammaCorrection;
}

void SetFadeLevel(int fadeval, bool updateHardwareCursor)
{
	if (HeadlessMode)
		return;

	for (int i = 0; i < 256; i++) {
		system_palette[i].r = (fadeval * logical_palette[i].r) / 256;
		system_palette[i].g = (fadeval * logical_palette[i].g) / 256;
		system_palette[i].b = (fadeval * logical_palette[i].b) / 256;
	}
	palette_update();
	if (updateHardwareCursor && IsHardwareCursor()) {
		ReinitializeHardwareCursor();
	}
}

void BlackPalette()
{
	// With fade level 0 updating the hardware cursor may be redundant
	// since everything is black. The caller should update the cursor
	// when needed instead.
	SetFadeLevel(0, /*updateHardwareCursor=*/false);
}

void PaletteFadeIn(int fr)
{
	if (HeadlessMode)
		return;
	if (demo::IsRunning())
		fr = 0;

	ApplyGamma(logical_palette, orig_palette, 256);

	if (fr > 0) {
		const uint32_t tc = SDL_GetTicks();
		fr *= 3;
		uint32_t prevFadeValue = 255;
		for (uint32_t i = 0; i < 256; i = fr * (SDL_GetTicks() - tc) / 50) {
			if (i != prevFadeValue) {
				// We can skip hardware cursor update for fade level 0 (everything is black).
				SetFadeLevel(i, /*updateHardwareCursor=*/i != 0u);
				prevFadeValue = i;
			}
			BltFast(nullptr, nullptr);
			RenderPresent();
		}
		SetFadeLevel(256);
	} else {
		SetFadeLevel(256);
		BltFast(nullptr, nullptr);
		RenderPresent();
	}

	logical_palette = orig_palette;

	sgbFadedIn = true;
}

void PaletteFadeOut(int fr)
{
	if (!sgbFadedIn || HeadlessMode)
		return;
	if (demo::IsRunning())
		fr = 0;

	if (fr > 0) {
		const uint32_t tc = SDL_GetTicks();
		fr *= 3;
		uint32_t prevFadeValue = 0;
		for (uint32_t i = 0; i < 256; i = fr * (SDL_GetTicks() - tc) / 50) {
			if (i != prevFadeValue) {
				SetFadeLevel(256 - i);
				prevFadeValue = i;
			}
			BltFast(nullptr, nullptr);
			RenderPresent();
		}
		SetFadeLevel(0);
	} else {
		SetFadeLevel(0);
		BltFast(nullptr, nullptr);
		RenderPresent();
	}

	sgbFadedIn = false;
}

bool IsPaletteFadedIn()
{
	return sgbFadedIn && !HeadlessMode;
}

void palette_update_caves()
{
	CycleColors(1, 31);
	palette_update(0, 31);
}

/**
 * @brief Cycle the lava every other frame, and glow every frame
 * Lava has 15 colors and the glow 16, so the full animation has 240 frames before it loops
 */
void palette_update_crypt()
{
	static bool delayLava = false;

	if (!delayLava) {
		CycleColorsReverse(1, 15);
		delayLava = false;
	}

	CycleColorsReverse(16, 31);
	palette_update(0, 31);
	delayLava = !delayLava;
}

/**
 * @brief Cycle the pond waves and bubles colors every 3rd frame
 * Bubles have 8 colors and waves 7, so the full animation has 56 frames before it loops
 */
void palette_update_hive()
{
	static uint8_t delay = 0;

	if (delay != 2) {
		delay++;
		return;
	}

	CycleColorsReverse(1, 8);
	CycleColorsReverse(9, 15);
	palette_update(0, 15);
	delay = 0;
}

void palette_update_quest_palette(int n)
{
	int i = 32 - n;
	logical_palette[i] = orig_palette[i];
	ApplyGamma(system_palette, logical_palette, 32);
	palette_update(0, 31);
	// Update blended transparency, but only for the color that was updated
	for (int j = 0; j < 256; j++) {
		if (i == j) { // No need to calculate transparency between 2 identical colors
			paletteTransparencyLookup[i][j] = j;
			continue;
		}
		SDL_Color blendedColor;
		blendedColor.r = ((int)logical_palette[i].r + (int)logical_palette[j].r) / 2;
		blendedColor.g = ((int)logical_palette[i].g + (int)logical_palette[j].g) / 2;
		blendedColor.b = ((int)logical_palette[i].b + (int)logical_palette[j].b) / 2;
		Uint8 best = FindBestMatchForColor(logical_palette, blendedColor, 1, 31);
		paletteTransparencyLookup[i][j] = paletteTransparencyLookup[j][i] = best;
	}
}

} // namespace devilution
