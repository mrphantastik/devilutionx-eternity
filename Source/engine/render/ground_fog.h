/**
 * @file ground_fog.h
 *
 * 3D Isometric Volumetric Ground Fog & Interactive Entity Dispersion.
 */
#pragma once

#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

/**
 * @brief Resets the interactive fog density, wall-feather corner weights, and per-floor
 * spawn roll when entering a level.
 */
void ResetGroundFogLevel();

/**
 * @brief Advances the 20 Hz fog dispersion simulation (single-tile player/monster/missile
 * displacement and natural density recovery). Called from GameLogic().
 */
void UpdateGroundFogSimulation();

/**
 * @brief Pass 1: Renders the 3D isometric volumetric ground-hugging fog/smoke blanket onto a
 * passable floor diamond (64x32), with true 4-corner wall fadeout, 3-tier internal shading
 * (crest/body/trough), and single-tile interactive dispersion.
 * Called in DrawCell() right after floor microtiles mt[0..1] and before upper wall/arch
 * microtiles mt[2..15] so walls, pillars, and objects naturally occlude the fog.
 */
void DrawGroundFogTileBase(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex);

/**
 * @brief Pass 2: Renders soft elliptical knee-high volumetric fog wisps over the lower legs/boots
 * of players and monsters standing on a foggy tile so they appear nestled in the volume.
 * Called in DrawDungeon() immediately after DrawPlayerHelper() and DrawMonsterHelper().
 */
void DrawGroundFogEntitySubmersion(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex);

} // namespace devilution
