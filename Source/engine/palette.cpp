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

struct RawPalColor {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

/**
 * @brief Checks if a custom palette asset exists in MPQs or loose game/save/assets paths.
 */
bool AssetPaletteExists(const char *pszFileName)
{
	AssetRef ref = FindAsset(pszFileName);
	return ref.ok() && ref.size() >= 768;
}

/**
 * @brief Rejects monochrome / single-hue "colorized" .pal presets in DevilutionXRandomizer
 * where >68% of all saturated level colors (1..127) are crammed into a single 40-degree hue window
 * or fewer than 3 distinct 30-degree hue buckets are populated.
 */
bool IsMultiHuePalette(const std::array<RawPalColor, 256> &palData)
{
	float hues[128];
	int hueCount = 0;
	int bucketCounts[12] = {};

	for (int i = 1; i < 128; i++) {
		const int r = palData[i].r;
		const int g = palData[i].g;
		const int b = palData[i].b;
		const int cmax = std::max({ r, g, b });
		const int cmin = std::min({ r, g, b });
		const int delta = cmax - cmin;

		// Require non-trivial brightness (cmax >= 28) and saturation (delta / cmax >= ~0.12)
		if (cmax >= 28 && delta * 255 >= cmax * 30) {
			float h = 0.0F;
			if (cmax == r)
				h = 60.0F * std::fmod(static_cast<float>(g - b) / static_cast<float>(delta), 6.0F);
			else if (cmax == g)
				h = 60.0F * ((static_cast<float>(b - r) / static_cast<float>(delta)) + 2.0F);
			else
				h = 60.0F * ((static_cast<float>(r - g) / static_cast<float>(delta)) + 4.0F);
			if (h < 0.0F)
				h += 360.0F;

			hues[hueCount++] = h;
			const int bucket = static_cast<int>(h / 30.0F) % 12;
			bucketCounts[bucket]++;
		}
	}

	if (hueCount < 16)
		return false;

	int activeBuckets = 0;
	for (int count : bucketCounts) {
		if (count >= 3)
			activeBuckets++;
	}
	if (activeBuckets < 3)
		return false;

	int maxIn40Deg = 0;
	for (int center = 0; center < 360; center += 5) {
		int cnt = 0;
		for (int i = 0; i < hueCount; i++) {
			float diff = std::fabs(hues[i] - static_cast<float>(center));
			if (diff > 180.0F)
				diff = 360.0F - diff;
			if (diff <= 20.0F)
				cnt++;
		}
		if (cnt > maxIn40Deg)
			maxIn40Deg = cnt;
	}

	// Reject if more than 68% of colored entries fall within a single 40-degree hue cone
	return (maxIn40Deg * 100) <= (hueCount * 68);
}

/**
 * @brief Deterministic Murmur3-style hash from the current level's seed and depth
 * so palette selection is fixed per floor and never perturbs gameplay RNG (GenerateRnd).
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
 * @brief Fallback procedural split-complementary hue/saturation shift on already-colored
 * level palette entries (indices 1..127) when external DevilutionXRandomizer .pal files
 * are not present. Neutral greyscale stone (s < 0.10) is left untouched so levels never
 * turn into a flat 1-color monochrome wash.
 */
/**
 * @brief Applies one of 4 deterministic lava color variants to Caves animated lava
 * palette indices (1..31) while preserving the per-index brightness wave so
 * CycleColors(1, 31) animates flowing lava ripples across all 4 variants:
 *   Variant 0: Classic Molten Magma (Orange-Amber)
 *   Variant 1: Sulfuric Brimstone (Golden-Yellow)
 *   Variant 2: Poison / Toxic Acid-Lava (Venomous Green)
 *   Variant 3: Subterranean Phosphene (Bioluminescent Cyan-Teal, no red)
 */
void ApplyCavesLavaVariant(uint32_t hash)
{
	const uint32_t variant = (hash >> 4) % 3U;

	for (int i = 1; i <= 31; i++) {
		const float r0 = orig_palette[i].r / 255.0F;
		const float g0 = orig_palette[i].g / 255.0F;
		const float b0 = orig_palette[i].b / 255.0F;
		const float cmax = std::max({ r0, g0, b0 });
		const float luma = 0.299F * r0 + 0.587F * g0 + 0.114F * b0;
		const float t = clamp(cmax * 0.65F + luma * 0.35F, 0.04F, 1.0F);
		const float t2 = t * t;

		float rp = r0;
		float gp = g0;
		float bp = b0;

		switch (variant) {
		case 0: // Classic Molten Magma (Orange-Amber)
			rp = clamp(t * 1.25F, 0.0F, 1.0F);
			gp = clamp(t2 * 0.82F + t * 0.12F, 0.0F, 1.0F);
			bp = clamp(t2 * t * 0.30F, 0.0F, 1.0F);
			break;
		case 1: // Sulfuric Brimstone (Golden-Yellow)
			rp = clamp(t * 1.18F, 0.0F, 1.0F);
			gp = clamp(t * 0.96F + t2 * 0.12F, 0.0F, 1.0F);
			bp = clamp(t2 * 0.26F, 0.0F, 1.0F);
			break;
		case 2: // Poison / Toxic Acid-Lava (Venomous Green)
		default:
			rp = clamp(t2 * 0.50F + t * 0.08F, 0.0F, 1.0F);
			gp = clamp(t * 1.22F, 0.0F, 1.0F);
			bp = clamp(t2 * 0.24F + t * 0.05F, 0.0F, 1.0F);
			break;
		}

		orig_palette[i].r = static_cast<Uint8>(clamp(static_cast<int>(rp * 255.0F + 0.5F), 0, 255));
		orig_palette[i].g = static_cast<Uint8>(clamp(static_cast<int>(gp * 255.0F + 0.5F), 0, 255));
		orig_palette[i].b = static_cast<Uint8>(clamp(static_cast<int>(bp * 255.0F + 0.5F), 0, 255));
	}
}

void ApplyLevelPaletteVariation(uint32_t hash)
{
	if (leveltype == DTYPE_CAVES) {
		ApplyCavesLavaVariant(hash);
		GenerateBlendedLookupTable(orig_palette, 1, 31);
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

	// Town (Tristram) always uses its original vanilla palette so returning to Town
	// or starting a New Game never recolors Tristram.
	if (l == DTYPE_TOWN) {
		LoadPalette("levels\\towndata\\town.pal");
		return;
	}

	// Preserve exact vanilla GenerateRnd(4) call count for dungeon levels
	// so dungeon generation seeds and multiplayer sync remain 100% identical.
	int rv = GenerateRnd(4) + 1;

	if (*sgOptions.Graphics.randomizePalette) {
		const uint32_t hash = GetLevelPaletteHash(l);
		char szRndFileName[64];

		struct RndPalInfo {
			const char *folder;
			const char *prefix;
			uint32_t count;
		};

		RndPalInfo info = { "l1data", "l1", 66 };
		switch (l) {
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

		const uint32_t startIdx = hash % info.count;
		for (uint32_t step = 0; step < info.count; step++) {
			const uint32_t rndIdx = ((startIdx + step) % info.count) + 1;
			*fmt::format_to(szRndFileName, R"(palette\{0}\{1}_{2}.pal)", info.folder, info.prefix, rndIdx) = '\0';
			if (!AssetPaletteExists(szRndFileName)) {
				if (step == 0)
					break; // Palette folder for this dungeon is not installed
				continue;
			}

			std::array<RawPalColor, 256> palData;
			LoadFileInMem(szRndFileName, palData);
			if (!IsMultiHuePalette(palData))
				continue; // Reject flat 1-color monochrome preset and try next preset

			// Load the vanilla base palette first so animated/special ramps (like Hell's
			// red blood/lava ramp 1..31) have their authentic vanilla shades ready
			if (l == DTYPE_CRYPT) {
				LoadPalette("nlevels\\l5data\\l5base.pal", /*blend=*/false);
			} else if (l == DTYPE_NEST) {
				const int nestRv = (!*sgOptions.Graphics.alternateNestArt) ? (rv + 1) : rv;
				char szBase[32];
				*fmt::format_to(szBase, R"(nlevels\l6data\l6base{0}.pal)", nestRv) = '\0';
				LoadPalette(szBase, /*blend=*/false);
			} else {
				char szBase[32];
				*fmt::format_to(szBase, R"(levels\l{0}data\l{0}_{1}.pal)", static_cast<int>(l), rv) = '\0';
				LoadPalette(szBase, /*blend=*/false);
			}

			for (unsigned i = 0; i < palData.size(); i++) {
				if (l == DTYPE_HELL) {
					// In Hell, indices 1..31 are the animated red blood/lava ramp, and strongly red
					// vanilla entries in 32..127 are Hell's blood/flesh/lava — always keep them red!
					if (i >= 1 && i <= 31)
						continue;
					if (i >= 32 && i < 128) {
						const int vr = orig_palette[i].r;
						const int vg = orig_palette[i].g;
						const int vb = orig_palette[i].b;
						if (vr > 36 && vr > (vg * 3) / 2 && vr > (vb * 3) / 2) {
							// Keep red-dominant Hell blood/lava/flesh in the red family
							// (scale brightness by palData luminance while preserving vanilla red ratio)
							const int vLum = std::max(1, vr * 2 + vg * 3 + vb);
							const int pLum = static_cast<int>(palData[i].r) * 2 + static_cast<int>(palData[i].g) * 3 + static_cast<int>(palData[i].b);
							orig_palette[i].r = static_cast<Uint8>(clamp((vr * pLum) / vLum, 0, 255));
							orig_palette[i].g = static_cast<Uint8>(clamp((vg * pLum) / vLum, 0, 255));
							orig_palette[i].b = static_cast<Uint8>(clamp((vb * pLum) / vLum, 0, 255));
							continue;
						}
					}
				}
				orig_palette[i].r = palData[i].r;
				orig_palette[i].g = palData[i].g;
				orig_palette[i].b = palData[i].b;
#ifndef USE_SDL1
				orig_palette[i].a = SDL_ALPHA_OPAQUE;
#endif
			}
			if (leveltype == DTYPE_CAVES) {
				ApplyCavesLavaVariant(hash);
				GenerateBlendedLookupTable(orig_palette, 1, 31);
			} else if (leveltype == DTYPE_CRYPT) {
				GenerateBlendedLookupTable(orig_palette, 1, 31);
			} else if (leveltype == DTYPE_NEST) {
				GenerateBlendedLookupTable(orig_palette, 1, 15);
			} else {
				GenerateBlendedLookupTable(orig_palette, -1, -1);
			}
			return;
		}
	}

	if (l == DTYPE_CRYPT) {
		LoadPalette("nlevels\\l5data\\l5base.pal");
		if (*sgOptions.Graphics.randomizePalette)
			ApplyLevelPaletteVariation(GetLevelPaletteHash(l));
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
		ApplyLevelPaletteVariation(GetLevelPaletteHash(l));
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
