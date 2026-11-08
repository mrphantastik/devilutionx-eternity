/**
 * @file gbuffer.cpp
 *
 * Hybrid CPU G-Buffer, Dynamic Colored Point-Light Collector, and
 * 2D Occlusion Post-Lighting Pipeline for DX:Eternity.
 */
#include "engine/render/gbuffer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include <SDL.h>

#include "control.h"
#include "cursor.h"
#include "diablo.h"
#include "engine/dx.h"
#include "engine/palette.h"
#include "engine/render/dun_render.hpp"
#include "engine/render/scrollrt.h"
#include "engine/render/weather.h"
#include "gmenu.h"
#include "help.h"
#include "inv.h"
#include "levels/gendung.h"
#include "lighting.h"
#include "minitext.h"
#include "misdat.h"
#include "missiles.h"
#include "monster.h"
#include "objects.h"
#include "options.h"
#include "player.h"
#include "qol/stash.h"
#include "quests.h"
#include "stores.h"
#include "utils/display.h"

namespace devilution {

namespace {

constexpr size_t MaxGpuLights = 48;

struct GpuPointLight {
	float x;
	float y;
	float z;
	float radius;
	float invRadiusSq;
	float r;
	float g;
	float b;
	float intensity;
};

int gBufferWidth = 0;
int gBufferHeight = 0;
int gActiveViewportHeight = 0;

std::vector<GBufferPixel> gBuffer;
std::vector<uint8_t> gWorldPalSnapshot;
std::vector<uint32_t> gLitFrameBuffer;
std::array<GpuPointLight, MaxGpuLights> gActiveLights;
size_t gActiveLightCount = 0;
bool gWorldPassCompletedThisFrame = false;

uint32_t gCurrentFrameId = 1;
Point gTileScreenPos[MAXDUNX][MAXDUNY];
uint32_t gTileRenderedFrame[MAXDUNX][MAXDUNY];

inline bool InBufferBounds(int x, int y)
{
	return x >= 0 && x < gBufferWidth && y >= 0 && y < gBufferHeight;
}

inline GBufferPixel &AtGBuffer(int x, int y)
{
	return gBuffer[static_cast<size_t>(y) * static_cast<size_t>(gBufferWidth) + static_cast<size_t>(x)];
}

Point ToFinalViewportSpace(Point p)
{
	if (!*sgOptions.Graphics.zoom)
		return p;

	int viewportOffsetX = 0;
	if (CanPanelsCoverView() && IsLeftPanelOpen()) {
		viewportOffsetX = SidePanelSize.width;
	}
	return Point { viewportOffsetX + p.x * 2, p.y * 2 };
}

void PushLight(float x, float y, float z, float radius, float r, float g, float b, float intensity)
{
	if (gActiveLightCount >= MaxGpuLights || radius <= 1.0F || intensity <= 0.01F)
		return;
	if (*sgOptions.Graphics.zoom) {
		radius *= 1.85F;
		z *= 1.5F;
	}
	if (x + radius < 0.0F || x - radius > static_cast<float>(gBufferWidth)
	    || y + radius < 0.0F || y - radius > static_cast<float>(gActiveViewportHeight)) {
		return;
	}

	// Merge if an identical-category light is within 6 pixels to prevent stacked sub-projectiles from spiking
	for (size_t i = 0; i < gActiveLightCount; ++i) {
		const float dx = gActiveLights[i].x - x;
		const float dy = gActiveLights[i].y - y;
		if (dx * dx + dy * dy < 36.0F) {
			gActiveLights[i].intensity = std::min(1.25F, gActiveLights[i].intensity + intensity * 0.25F);
			gActiveLights[i].radius = std::max(gActiveLights[i].radius, radius);
			gActiveLights[i].invRadiusSq = 1.0F / (gActiveLights[i].radius * gActiveLights[i].radius);
			return;
		}
	}

	gActiveLights[gActiveLightCount++] = GpuPointLight {
		x, y, z, radius, 1.0F / (radius * radius), r, g, b, intensity
	};
}

void ZoomGBuffer(int viewportHeight)
{
	int viewportWidth = gBufferWidth;
	int viewportOffsetX = 0;
	if (CanPanelsCoverView()) {
		if (IsLeftPanelOpen()) {
			viewportWidth -= SidePanelSize.width;
			viewportOffsetX = SidePanelSize.width;
		} else if (IsRightPanelOpen()) {
			viewportWidth -= SidePanelSize.width;
		}
	}
	if (viewportWidth <= 0 || viewportHeight <= 0)
		return;

	const int srcWidth = (viewportWidth + 1) / 2;
	const int doubleableWidth = viewportWidth / 2;
	const int srcHeight = (viewportHeight + 1) / 2;
	const int doubleableHeight = viewportHeight / 2;
	const bool oddViewportWidth = (viewportWidth % 2) == 1;

	for (int hgt = 0; hgt < doubleableHeight; hgt++) {
		const int srcY = srcHeight - 1 - hgt;
		const int dstY = viewportHeight - 1 - hgt * 2;
		if (srcY < 0 || dstY < 1 || dstY >= gBufferHeight)
			continue;

		const GBufferPixel *src = &AtGBuffer(srcWidth - 1, srcY);
		GBufferPixel *dst = &AtGBuffer(viewportOffsetX + viewportWidth - 1, dstY);

		for (int i = 0; i < doubleableWidth; i++) {
			*dst-- = *src;
			*dst-- = *src;
			--src;
		}
		if (oddViewportWidth) {
			*dst-- = *src;
		}

		std::memcpy(
		    &AtGBuffer(viewportOffsetX, dstY - 1),
		    &AtGBuffer(viewportOffsetX, dstY),
		    static_cast<size_t>(viewportWidth) * sizeof(GBufferPixel));
	}
}

bool IsDynamicEntityLightId(int lightId)
{
	if (lightId < 0)
		return false;

	for (const Player &player : Players) {
		if (player.plractive && player.lightId == lightId)
			return true;
	}
	for (const Missile &m : Missiles) {
		if (!m._miDelFlag && m._mlid == lightId)
			return true;
	}
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const Monster &mon = Monsters[ActiveMonsters[i]];
		if (!mon.isInvalid && mon.lightId == lightId)
			return true;
	}
	return false;
}

void CollectStaticDungeonLights()
{
	const uint32_t ticks = SDL_GetTicks();
	const float timeSec = static_cast<float>(ticks % 100000U) * 0.001F;
	const float torchFlicker = 0.97F + 0.03F * std::sin(timeSec * 6.5F);

	// Collect static dungeon & town light sources (torches, braziers, shrines) that were rendered this frame
	for (int i = 0; i < ActiveLightCount && gActiveLightCount < MaxGpuLights; ++i) {
		const int lid = ActiveLights[i];
		if (IsDynamicEntityLightId(lid))
			continue;

		const Light &lt = Lights[lid];
		if (lt.isInvalid || lt.radius == 0 || !InDungeonBounds(lt.position.tile))
			continue;
		if (gTileRenderedFrame[lt.position.tile.x][lt.position.tile.y] != gCurrentFrameId)
			continue;

		const Point basePos = gTileScreenPos[lt.position.tile.x][lt.position.tile.y];
		const Point centerOut {
			basePos.x + TILE_WIDTH / 2 + lt.position.offset.deltaX,
			basePos.y - TILE_HEIGHT / 2 + lt.position.offset.deltaY - 14
		};
		const Point screenPos = ToFinalViewportSpace(centerOut);

		float r = 1.00F;
		float g = 0.72F;
		float b = 0.38F;
		float inten = 0.55F * torchFlicker;
		switch (leveltype) {
		case DTYPE_TOWN:
			r = 1.00F;
			g = 0.76F;
			b = 0.44F;
			inten = 0.48F * torchFlicker;
			break;
		case DTYPE_CATHEDRAL:
		case DTYPE_CATACOMBS:
			r = 1.00F;
			g = 0.70F;
			b = 0.36F;
			break;
		case DTYPE_CAVES:
		case DTYPE_HELL:
			r = 1.00F;
			g = 0.45F;
			b = 0.16F;
			inten = 0.65F * torchFlicker;
			break;
		case DTYPE_NEST:
			r = 0.68F;
			g = 0.92F;
			b = 0.36F;
			inten = 0.50F * torchFlicker;
			break;
		case DTYPE_CRYPT:
			r = 0.80F;
			g = 0.82F;
			b = 1.00F;
			inten = 0.58F * torchFlicker;
			break;
		default:
			break;
		}

		const float radiusPx = std::clamp(static_cast<float>(lt.radius) * 16.0F, 64.0F, 148.0F);
		PushLight(
		    static_cast<float>(screenPos.x),
		    static_cast<float>(screenPos.y),
		    28.0F,
		    radiusPx,
		    r, g, b,
		    inten);
	}
}

void FinalizeGBufferForFrame(SDL_Surface *outputRgbaSurface)
{
	if (PalSurface == nullptr || outputRgbaSurface == nullptr)
		return;

	const int w = std::min<int>({ gBufferWidth, PalSurface->w, outputRgbaSurface->w });
	const int h = std::min<int>({ gBufferHeight, PalSurface->h, outputRgbaSurface->h });
	const int viewH = std::min(h, gActiveViewportHeight);

	const bool leftPanelCovered = CanPanelsCoverView() && IsLeftPanelOpen();
	const bool rightPanelCovered = CanPanelsCoverView() && IsRightPanelOpen();
	const Rectangle leftRect = GetLeftPanel();
	const Rectangle rightRect = GetRightPanel();
	const Rectangle uiRect = GetUIRectangle();
	const bool modalDialogActive = (stextflag != TalkID::None) || qtextflag || gmenu_is_active() || HelpFlag;

	for (int y = 0; y < viewH; ++y) {
		const auto *palRow = static_cast<const uint8_t *>(PalSurface->pixels) + y * PalSurface->pitch;
		const uint8_t *snapRow = &gWorldPalSnapshot[static_cast<size_t>(y) * static_cast<size_t>(gBufferWidth)];
		GBufferPixel *gbRow = &AtGBuffer(0, y);

		for (int x = 0; x < w; ++x) {
			if (palRow[x] != snapRow[x]) {
				// Any pixel overwritten after DrawGame() (Automap, Item Labels, Health Bars,
				// Floating Numbers, Panels, Dialogs, Cursor, FPS) is a 2D UI overlay
				gbRow[x].surfaceType = static_cast<uint8_t>(GBufferSurfaceType::UI);
				gbRow[x].occlusionHeight = 0;
				continue;
			}
			if ((leftPanelCovered && leftRect.contains(Point { x, y }))
			    || (rightPanelCovered && rightRect.contains(Point { x, y }))
			    || (modalDialogActive && uiRect.contains(Point { x, y }))) {
				gbRow[x].surfaceType = static_cast<uint8_t>(GBufferSurfaceType::UI);
				gbRow[x].occlusionHeight = 0;
				continue;
			}
		}
	}

	// Everything at or below gnViewportHeight is the lower HUD panel
	for (int y = viewH; y < h; ++y) {
		GBufferPixel *gbRow = &AtGBuffer(0, y);
		for (int x = 0; x < w; ++x) {
			gbRow[x].surfaceType = static_cast<uint8_t>(GBufferSurfaceType::UI);
			gbRow[x].occlusionHeight = 0;
		}
	}
}

void ExecuteNativeLightingPass(SDL_Surface *outputRgbaSurface, uint32_t *dstPixels, int dstPitchPixels)
{
	if (outputRgbaSurface == nullptr || outputRgbaSurface->format->BytesPerPixel != 4 || dstPixels == nullptr)
		return;

	const int w = std::min<int>(gBufferWidth, outputRgbaSurface->w);
	const int h = std::min<int>(gBufferHeight, outputRgbaSurface->h);
	const int viewH = std::min<int>(h, gActiveViewportHeight);

	// Copy unlit HUD/UI rows below the world viewport verbatim
	for (int y = viewH; y < h; ++y) {
		const auto *srcRow = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(outputRgbaSurface->pixels) + y * outputRgbaSurface->pitch);
		std::memcpy(dstPixels + static_cast<size_t>(y) * static_cast<size_t>(dstPitchPixels), srcRow, static_cast<size_t>(w) * sizeof(uint32_t));
	}

	const SDL_PixelFormat *fmt = outputRgbaSurface->format;
	const uint8_t rShift = fmt->Rshift;
	const uint8_t gShift = fmt->Gshift;
	const uint8_t bShift = fmt->Bshift;
	const uint32_t aMask = fmt->Amask;

	const float stormFlash = (leveltype == DTYPE_TOWN && !setlevel) ? GetWeatherLightningFlashFactor() : 0.0F;
	const float ambientBase = (leveltype == DTYPE_TOWN && !setlevel) ? 0.96F : 0.92F;
	const bool checkWallOcclusion = (leveltype != DTYPE_TOWN);

	std::array<size_t, MaxGpuLights> rowLightIndices {};

	for (int y = 0; y < viewH; ++y) {
		const auto *srcRow = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(outputRgbaSurface->pixels) + y * outputRgbaSurface->pitch);
		uint32_t *dstRow = dstPixels + static_cast<size_t>(y) * static_cast<size_t>(dstPitchPixels);
		const GBufferPixel *gbRow = &AtGBuffer(0, y);

		const float fy = static_cast<float>(y);
		size_t rowLightCount = 0;
		for (size_t i = 0; i < gActiveLightCount; ++i) {
			if (std::abs(fy - gActiveLights[i].y) < gActiveLights[i].radius) {
				rowLightIndices[rowLightCount++] = i;
			}
		}

		// Fast path if no dynamic lights or thunderstorm flash touch this scanline
		if (rowLightCount == 0 && stormFlash <= 0.001F) {
			for (int x = 0; x < w; ++x) {
				const uint32_t px = srcRow[x];
				if (gbRow[x].surfaceType == 0) {
					dstRow[x] = px;
					continue;
				}
				const uint32_t outR = static_cast<uint32_t>(static_cast<float>((px >> rShift) & 0xFFU) * ambientBase);
				const uint32_t outG = static_cast<uint32_t>(static_cast<float>((px >> gShift) & 0xFFU) * ambientBase);
				const uint32_t outB = static_cast<uint32_t>(static_cast<float>((px >> bShift) & 0xFFU) * ambientBase);
				dstRow[x] = (outR << rShift) | (outG << gShift) | (outB << bShift) | aMask;
			}
			continue;
		}

		for (int x = 0; x < w; ++x) {
			const uint32_t px = srcRow[x];
			if (gbRow[x].surfaceType == 0) {
				dstRow[x] = px;
				continue;
			}

			const float fx = static_cast<float>(x);
			float dynR = 0.0F;
			float dynG = 0.0F;
			float dynB = 0.0F;

			for (size_t idx = 0; idx < rowLightCount; ++idx) {
				const GpuPointLight &lt = gActiveLights[rowLightIndices[idx]];
				const float lx = lt.x - fx;
				if (std::abs(lx) >= lt.radius)
					continue;
				const float ly = lt.y - fy;
				const float distSq = lx * lx + ly * ly;
				const float normDistSq = distSq * lt.invRadiusSq;
				if (normDistSq >= 1.0F)
					continue;

				// Smooth C2 quartic window attenuation (zero hard outer ring)
				const float falloff = 1.0F - normDistSq;
				const float atten = falloff * falloff;

				// Smooth 3D point-light elevation cosine (no noisy Sobel edge filter)
				const float geomCos = lt.z / std::sqrt(distSq + lt.z * lt.z);

				// Soft screen-space occlusion behind solid dungeon walls
				float shadow = 1.0F;
				if (checkWallOcclusion && gbRow[x].occlusionHeight == 0 && distSq > 100.0F) {
					const float stepX = lx * 0.1666F;
					const float stepY = ly * 0.1666F;
					float sx = fx + stepX * 1.5F;
					float sy = fy + stepY * 1.5F;
					int blockers = 0;
					for (int s = 1; s < 6; ++s) {
						const int ix = static_cast<int>(sx);
						const int iy = static_cast<int>(sy);
						if (InBufferBounds(ix, iy) && AtGBuffer(ix, iy).occlusionHeight > 100) {
							++blockers;
						}
						sx += stepX;
						sy += stepY;
					}
					if (blockers > 0) {
						shadow = std::max(0.42F, 1.0F - static_cast<float>(blockers) * 0.14F);
					}
				}

				const float wLight = lt.intensity * atten * (0.35F + 0.65F * geomCos) * shadow;
				dynR += lt.r * wLight;
				dynG += lt.g * wLight;
				dynB += lt.b * wLight;
			}

			if (stormFlash > 0.001F) {
				const float sw = stormFlash * 0.45F;
				dynR += 0.58F * sw;
				dynG += 0.74F * sw;
				dynB += 1.00F * sw;
			}

			// Soft asymptotic tone-mapping compression so multiple overlapping projectiles
			// never overexpose or blow out the player/monsters to white
			const float compR = dynR / (1.0F + dynR * 1.35F);
			const float compG = dynG / (1.0F + dynG * 1.35F);
			const float compB = dynB / (1.0F + dynB * 1.35F);

			// Contrast-preserving multiplicative color modulation (zero milky additive fog)
			const float baseR = static_cast<float>((px >> rShift) & 0xFFU);
			const float baseG = static_cast<float>((px >> gShift) & 0xFFU);
			const float baseB = static_cast<float>((px >> bShift) & 0xFFU);

			const int outR = std::min(255, static_cast<int>(baseR * (ambientBase + compR * 0.82F)));
			const int outG = std::min(255, static_cast<int>(baseG * (ambientBase + compG * 0.82F)));
			const int outB = std::min(255, static_cast<int>(baseB * (ambientBase + compB * 0.82F)));

			dstRow[x] = (static_cast<uint32_t>(outR) << rShift)
			    | (static_cast<uint32_t>(outG) << gShift)
			    | (static_cast<uint32_t>(outB) << bShift)
			    | aMask;
		}
	}
}

} // namespace

void InitGBuffer(int width, int height)
{
	if (width <= 0 || height <= 0)
		return;
	if (gBufferWidth == width && gBufferHeight == height && !gBuffer.empty())
		return;

	gBufferWidth = width;
	gBufferHeight = height;
	const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
	gBuffer.assign(count, GBufferPixel { 0, 0, 0, 0 });
	gWorldPalSnapshot.assign(count, 0);
	gLitFrameBuffer.assign(count, 0);
}

void FreeGBuffer()
{
	gBuffer.clear();
	gWorldPalSnapshot.clear();
	gLitFrameBuffer.clear();
	gBufferWidth = 0;
	gBufferHeight = 0;
}

void GBufferBeginFrame(const Surface &viewportOut)
{
	if (HeadlessMode || !*sgOptions.Graphics.dynamicLighting)
		return;

	InitGBuffer(gnScreenWidth, gnScreenHeight);
	gActiveViewportHeight = gnViewportHeight;
	gActiveLightCount = 0;
	gWorldPassCompletedThisFrame = false;
	++gCurrentFrameId;

	const int clearH = std::min<int>(gBufferHeight, viewportOut.h());
	// Initialize the entire world viewport to GBufferSurfaceType::Floor (1) with 0 occlusion
	// so there are never any rectangular sprite or micro-tile discontinuities.
	for (int y = 0; y < clearH; ++y) {
		GBufferPixel *row = &AtGBuffer(0, y);
		for (int x = 0; x < gBufferWidth; ++x) {
			row[x] = GBufferPixel { static_cast<uint8_t>(GBufferSurfaceType::Floor), 0, 0, 0 };
		}
	}
}

void GBufferTagFloorTile(const Surface & /*out*/, Point tilePosition, Point targetBufferPosition, uint8_t /*lightIndex*/)
{
	if (HeadlessMode || !*sgOptions.Graphics.dynamicLighting || gBuffer.empty())
		return;

	if (InDungeonBounds(tilePosition)) {
		gTileScreenPos[tilePosition.x][tilePosition.y] = targetBufferPosition;
		gTileRenderedFrame[tilePosition.x][tilePosition.y] = gCurrentFrameId;
	}
}

void GBufferTagDungeonCell(const Surface &out, Point tilePosition, Point targetBufferPosition, uint8_t lightIndex, bool transparent)
{
	if (HeadlessMode || !*sgOptions.Graphics.dynamicLighting || gBuffer.empty())
		return;

	if (InDungeonBounds(tilePosition)) {
		gTileScreenPos[tilePosition.x][tilePosition.y] = targetBufferPosition;
		gTileRenderedFrame[tilePosition.x][tilePosition.y] = gCurrentFrameId;
	}

	// Only tag solid indoor dungeon walls with an isometric diamond occlusion footprint
	// (never tag rectangular 32x32 micro-tile boxes or outdoor Town rocks/trees)
	if (leveltype == DTYPE_TOWN || transparent)
		return;

	const uint16_t levelPieceId = dPiece[tilePosition.x][tilePosition.y];
	const bool isSolidWall = TileHasAny(levelPieceId, TileProperties::Solid) && TileHasAny(levelPieceId, TileProperties::Trap);
	if (!isSolidWall)
		return;

	const int maxW = std::min(gBufferWidth, out.w());
	const int maxH = std::min(gBufferHeight, out.h());
	const int baseX = targetBufferPosition.x;
	const int baseY = targetBufferPosition.y - 12;

	for (int dy = 4; dy < TILE_HEIGHT - 4; ++dy) {
		const int py = baseY - dy;
		if (py < 0 || py >= maxH)
			continue;
		const int halfSpan = (dy < TILE_HEIGHT / 2) ? (dy * 2 - 4) : ((TILE_HEIGHT - 1 - dy) * 2 - 4);
		if (halfSpan <= 0)
			continue;
		const int x0 = std::max(0, baseX + TILE_WIDTH / 2 - halfSpan);
		const int x1 = std::min(maxW, baseX + TILE_WIDTH / 2 + halfSpan);
		GBufferPixel *row = &AtGBuffer(0, py);
		for (int px = x0; px < x1; ++px) {
			row[px].occlusionHeight = 180;
			row[px].localLightIndex = lightIndex;
		}
	}
}

void GBufferAddPlayerLight(const Player &player, Point targetBufferPosition)
{
	if (HeadlessMode || !*sgOptions.Graphics.dynamicLighting || gBuffer.empty())
		return;

	// Anchor light directly to the rendered player torso center (100% locked to sprite when moving)
	const Point centerOut { targetBufferPosition.x, targetBufferPosition.y - 28 };
	const Point screenPos = ToFinalViewportSpace(centerOut);

	const float baseRadius = (leveltype == DTYPE_TOWN)
	    ? 145.0F
	    : std::max(120.0F, static_cast<float>(std::max<int>(player._pLightRad, 4)) * 20.0F);

	if (leveltype == DTYPE_TOWN) {
		PushLight(
		    static_cast<float>(screenPos.x),
		    static_cast<float>(screenPos.y),
		    32.0F,
		    baseRadius,
		    1.00F, 0.80F, 0.54F,
		    0.42F);
	} else {
		PushLight(
		    static_cast<float>(screenPos.x),
		    static_cast<float>(screenPos.y),
		    32.0F,
		    baseRadius,
		    1.00F, 0.76F, 0.44F,
		    0.72F);
	}

	// Elemental weapon swing glow during melee/ranged attack frames
	if (player._pmode == PM_ATTACK || player._pmode == PM_RATTACK) {
		if (HasAnyOf(player._pIFlags, ItemSpecialEffect::LightningDamage) || player._pILMaxDam > 0) {
			PushLight(
			    static_cast<float>(screenPos.x),
			    static_cast<float>(screenPos.y),
			    26.0F,
			    90.0F,
			    0.35F, 0.76F, 1.00F,
			    0.55F);
		} else if (HasAnyOf(player._pIFlags, ItemSpecialEffect::FireDamage) || player._pIFMaxDam > 0) {
			PushLight(
			    static_cast<float>(screenPos.x),
			    static_cast<float>(screenPos.y),
			    26.0F,
			    90.0F,
			    1.00F, 0.46F, 0.16F,
			    0.55F);
		}
	}
}

void GBufferAddMissileLight(const Missile &m, Point missileRenderPosition, int spriteWidth, int spriteHeight)
{
	if (HeadlessMode || !*sgOptions.Graphics.dynamicLighting || gBuffer.empty() || m._miDelFlag)
		return;

	float r = 0.0F;
	float g = 0.0F;
	float b = 0.0F;
	float radius = 0.0F;
	float intensity = 0.0F;

	switch (m._mitype) {
	case MissileID::Firebolt:
	case MissileID::Fireball:
	case MissileID::FireballBow:
	case MissileID::FireWall:
	case MissileID::FlameWave:
	case MissileID::Inferno:
	case MissileID::Elemental:
	case MissileID::Guardian:
	case MissileID::MagmaBall:
	case MissileID::MagmaBallExplosion:
	case MissileID::Immolation:
	case MissileID::RingOfFire:
	case MissileID::RuneOfFire:
	case MissileID::RuneOfImmolation:
	case MissileID::ApocalypseBoom:
	case MissileID::DiabloApocalypseBoom:
	case MissileID::BigExplosion:
	case MissileID::WeaponExplosion:
		r = 1.00F;
		g = 0.46F;
		b = 0.14F;
		radius = (m._mitype == MissileID::Fireball || m._mitype == MissileID::ApocalypseBoom || m._mitype == MissileID::DiabloApocalypseBoom) ? 135.0F : 98.0F;
		intensity = 0.78F;
		break;
	case MissileID::Lightning:
	case MissileID::ThinLightning:
	case MissileID::ChainLightning:
	case MissileID::LightningWall:
	case MissileID::Nova:
	case MissileID::NovaBall:
	case MissileID::RuneOfNova:
	case MissileID::ChargedBolt:
	case MissileID::ChargedBoltBow:
	case MissileID::LightningBow:
	case MissileID::BlueFlare:
	case MissileID::BlueFlare2:
	case MissileID::BlueExplosion:
	case MissileID::BlueExplosion2:
		r = 0.35F;
		g = 0.76F;
		b = 1.00F;
		radius = 105.0F;
		intensity = 0.72F;
		break;
	case MissileID::HolyBolt:
	case MissileID::HolyBoltBow:
	case MissileID::FlashBottom:
	case MissileID::FlashTop:
	case MissileID::TownPortal:
	case MissileID::RedPortal:
	case MissileID::Teleport:
	case MissileID::Phasing:
	case MissileID::ManaShield:
	case MissileID::ResurrectBeam:
	case MissileID::RuneOfLight:
	case MissileID::SpectralArrow:
	case MissileID::Warp:
		r = (m._mitype == MissileID::RedPortal) ? 1.00F : 0.48F;
		g = (m._mitype == MissileID::RedPortal) ? 0.24F : 0.78F;
		b = (m._mitype == MissileID::RedPortal) ? 0.26F : 1.00F;
		radius = 115.0F;
		intensity = 0.68F;
		break;
	case MissileID::YellowFlare:
	case MissileID::YellowExplosion:
	case MissileID::OrangeFlare:
	case MissileID::OrangeExplosion:
		r = 1.00F;
		g = 0.78F;
		b = 0.26F;
		radius = 95.0F;
		intensity = 0.70F;
		break;
	case MissileID::BloodStar:
	case MissileID::BloodStarExplosion:
	case MissileID::RedFlare:
	case MissileID::RedExplosion:
	case MissileID::BoneSpirit:
		r = 1.00F;
		g = 0.20F;
		b = 0.36F;
		radius = 98.0F;
		intensity = 0.72F;
		break;
	case MissileID::Acid:
	case MissileID::AcidPuddle:
	case MissileID::AcidSplat:
		r = 0.30F;
		g = 0.94F;
		b = 0.26F;
		radius = 80.0F;
		intensity = 0.55F;
		break;
	case MissileID::FireArrow:
		r = 1.00F;
		g = 0.52F;
		b = 0.18F;
		radius = 78.0F;
		intensity = 0.60F;
		break;
	case MissileID::LightningArrow:
		r = 0.38F;
		g = 0.78F;
		b = 1.00F;
		radius = 78.0F;
		intensity = 0.60F;
		break;
	default:
		return;
	}

	const Point centerOut {
		missileRenderPosition.x + spriteWidth / 2,
		missileRenderPosition.y - std::min(spriteHeight / 2, 24)
	};
	const Point screenPos = ToFinalViewportSpace(centerOut);
	PushLight(
	    static_cast<float>(screenPos.x),
	    static_cast<float>(screenPos.y),
	    26.0F,
	    radius,
	    r, g, b,
	    intensity);
}

void GBufferAddMonsterLight(const Monster &mon, Point targetBufferPosition, int spriteWidth, int spriteHeight)
{
	if (HeadlessMode || !*sgOptions.Graphics.dynamicLighting || gBuffer.empty() || leveltype == DTYPE_TOWN)
		return;
	if (mon.isInvalid || (mon.flags & MFLAG_HIDDEN) != 0 || mon.hitPoints <= 0)
		return;

	const _monster_id mtype = mon.type().type;
	const bool isBossOrFiery = mon.isUnique() || mtype == MT_DIABLO || mtype == MT_NAKRUL
	    || mtype == MT_TORCHANT || mtype == MT_FIREBAT || mtype == MT_HELLBAT
	    || (mtype >= MT_NMAGMA && mtype <= MT_WMAGMA)
	    || (mtype >= MT_INCIN && mtype <= MT_HELLBURN);
	if (!isBossOrFiery)
		return;

	float r = 1.00F;
	float g = 0.50F;
	float b = 0.18F;
	float rad = 90.0F;
	float inten = 0.55F;
	if (mtype == MT_DIABLO || mtype == MT_NAKRUL) {
		r = 1.00F;
		g = 0.26F;
		b = 0.18F;
		rad = 128.0F;
		inten = 0.78F;
	} else if (mon.isUnique()) {
		r = 0.85F;
		g = 0.64F;
		b = 1.00F;
		rad = 92.0F;
		inten = 0.52F;
	}

	const Point centerOut {
		targetBufferPosition.x + spriteWidth / 2,
		targetBufferPosition.y - std::min(spriteHeight / 2, 28)
	};
	const Point screenPos = ToFinalViewportSpace(centerOut);
	PushLight(
	    static_cast<float>(screenPos.x),
	    static_cast<float>(screenPos.y),
	    28.0F,
	    rad,
	    r, g, b,
	    inten);
}

void GBufferEndWorldPass(const Surface & /*fullOut*/, Point /*startTilePosition*/, Displacement /*offset*/)
{
	if (HeadlessMode || !*sgOptions.Graphics.dynamicLighting || gBuffer.empty() || PalSurface == nullptr)
		return;

	if (*sgOptions.Graphics.zoom) {
		ZoomGBuffer(gnViewportHeight);
	}

	// Snapshot the pure 8-bit world viewport before any 2D UI, Automap, Labels, or Panels are drawn
	const int w = std::min<int>(gBufferWidth, PalSurface->w);
	const int viewH = std::min<int>({ gBufferHeight, PalSurface->h, static_cast<int>(gnViewportHeight) });
	for (int y = 0; y < viewH; ++y) {
		const auto *palRow = static_cast<const uint8_t *>(PalSurface->pixels) + y * PalSurface->pitch;
		std::memcpy(&gWorldPalSnapshot[static_cast<size_t>(y) * static_cast<size_t>(gBufferWidth)], palRow, static_cast<size_t>(w));
	}

	CollectStaticDungeonLights();
	gWorldPassCompletedThisFrame = true;
}

void GBufferApplyLightingPass(SDL_Surface *outputRgbaSurface, SDL_Renderer *sdlRenderer, SDL_Texture *sdlTexture)
{
	if (HeadlessMode || !gbRunGame || !*sgOptions.Graphics.dynamicLighting || !gWorldPassCompletedThisFrame || gBuffer.empty()) {
		if (sdlRenderer != nullptr && sdlTexture != nullptr && outputRgbaSurface != nullptr) {
			SDL_UpdateTexture(sdlTexture, nullptr, outputRgbaSurface->pixels, outputRgbaSurface->pitch);
			SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
			SDL_RenderClear(sdlRenderer);
			SDL_RenderCopy(sdlRenderer, sdlTexture, nullptr, nullptr);
		}
		return;
	}

	FinalizeGBufferForFrame(outputRgbaSurface);

	if (sdlRenderer != nullptr && sdlTexture != nullptr && outputRgbaSurface != nullptr) {
		// Compute the lit frame at exact 1:1 native pixel resolution into gLitFrameBuffer
		// (without mutating outputRgbaSurface) and let SDL2's hardware scaler present it crisply.
		ExecuteNativeLightingPass(outputRgbaSurface, gLitFrameBuffer.data(), gBufferWidth);
		SDL_UpdateTexture(sdlTexture, nullptr, gLitFrameBuffer.data(), gBufferWidth * static_cast<int>(sizeof(uint32_t)));
		SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
		SDL_RenderClear(sdlRenderer);
		SDL_RenderCopy(sdlRenderer, sdlTexture, nullptr, nullptr);
		return;
	}

	if (outputRgbaSurface != nullptr) {
		ExecuteNativeLightingPass(outputRgbaSurface, gLitFrameBuffer.data(), gBufferWidth);
		const int w = std::min<int>(gBufferWidth, outputRgbaSurface->w);
		const int viewH = std::min<int>({ gBufferHeight, outputRgbaSurface->h, gActiveViewportHeight });
		for (int y = 0; y < viewH; ++y) {
			auto *dstRow = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(outputRgbaSurface->pixels) + y * outputRgbaSurface->pitch);
			const uint32_t *srcRow = gLitFrameBuffer.data() + static_cast<size_t>(y) * static_cast<size_t>(gBufferWidth);
			std::memcpy(dstRow, srcRow, static_cast<size_t>(w) * sizeof(uint32_t));
		}
	}
}

} // namespace devilution
