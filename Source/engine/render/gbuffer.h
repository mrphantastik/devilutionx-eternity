/**
 * @file gbuffer.h
 *
 * Hybrid CPU G-Buffer, Dynamic Colored Point-Light Collector, and
 * 2D Occlusion Post-Lighting Pipeline for DX:Eternity.
 */
#pragma once

#include <cstdint>

#include <SDL.h>

#include "engine/point.hpp"
#include "engine/surface.hpp"
#include "missiles.h"
#include "monster.h"
#include "player.h"

namespace devilution {

/**
 * @brief Surface classification stored in G-Buffer channel R (`surfaceType`).
 */
enum class GBufferSurfaceType : uint8_t {
	/** 2D UI / HUD / Panels / Text / Cursor / Automap — completely unlit by post-processing. */
	UI = 0,
	/** World viewport surface (floor, walls, sprites). */
	Floor = 1,
};

/**
 * @brief Per-pixel 32-bit G-Buffer entry.
 */
struct GBufferPixel {
	uint8_t surfaceType;     // GBufferSurfaceType (0 = UI, 1 = World surface)
	uint8_t occlusionHeight; // 0 = non-occluding floor/sprite, 120..255 = solid dungeon wall footprint
	uint8_t localLightIndex; // Native dLight index (0 = brightest, 15 = darkest)
	uint8_t reserved;        // Alignment padding
};

/**
 * @brief Initialize or resize the CPU G-Buffer and world snapshot buffers to match screen dimensions.
 */
void InitGBuffer(int width, int height);

/**
 * @brief Release G-Buffer memory and auxiliary frame buffers.
 */
void FreeGBuffer();

/**
 * @brief Clear the G-Buffer and dynamic light list at the start of `DrawGame()`.
 */
void GBufferBeginFrame(const Surface &viewportOut);

/**
 * @brief Tag an isometric 64x32 floor diamond (`mt[0..1]`) in the G-Buffer.
 */
void GBufferTagFloorTile(const Surface &out, Point tilePosition, Point targetBufferPosition, uint8_t lightIndex);

/**
 * @brief Tag a dungeon cell (`mt[0..15]`) with isometric floor coverage and solid dungeon wall occlusion.
 */
void GBufferTagDungeonCell(const Surface &out, Point tilePosition, Point targetBufferPosition, uint8_t lightIndex, bool transparent);

/**
 * @brief Emit a player's dynamic light directly at their exact rendered screen position during `DrawPlayer()`.
 */
void GBufferAddPlayerLight(const Player &player, Point targetBufferPosition);

/**
 * @brief Emit a missile/projectile's dynamic colored light directly at its exact rendered screen position during `DrawMissilePrivate()`.
 */
void GBufferAddMissileLight(const Missile &missile, Point missileRenderPosition, int spriteWidth, int spriteHeight);

/**
 * @brief Emit a unique/boss/fiery monster's dynamic light directly at its exact rendered screen position during `DrawMonster()`.
 */
void GBufferAddMonsterLight(const Monster &monster, Point targetBufferPosition, int spriteWidth, int spriteHeight);

/**
 * @brief Snapshot the rendered 8-bit world viewport at the end of `DrawGame()` (after `Zoom()`, before 2D UI overlays)
 *        and collect static dungeon/town light sources (`Lights`).
 */
void GBufferEndWorldPass(const Surface &fullOut, Point startTilePosition, Displacement offset);

/**
 * @brief Finalize the G-Buffer right before `RenderPresent()` by comparing `PalSurface` against the world snapshot
 *        to mask out all 2D UI, HUD, Automap, panel, label, and cursor pixels, and apply the post-lighting pass.
 */
void GBufferApplyLightingPass(SDL_Surface *outputRgbaSurface, SDL_Renderer *sdlRenderer, SDL_Texture *sdlTexture);

} // namespace devilution
