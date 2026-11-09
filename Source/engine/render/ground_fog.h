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
 * @brief Resets the interactive fog density & velocity wake grid when entering a level.
 */
void ResetGroundFogLevel();

/**
 * @brief Advances the 20 Hz fog dispersion simulation (player/monster/missile wakes,
 * directional swirl advection, and natural density recovery). Called from GameLogic().
 */
void UpdateGroundFogSimulation();

/**
 * @brief Pass 1: Renders the 3D isometric ground-hugging fog/smoke blanket onto a walkable
 * floor diamond (64x32), respecting local tile lighting (dLight), wall boundaries,
 * and interactive dispersion wakes.
 * Called in DrawDungeon() right after DrawCell() and before items/corpses/entities.
 */
void DrawGroundFogTileBase(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex);

/**
 * @brief Pass 2: Renders knee-high volumetric fog/smoke wisps over the lower legs/boots
 * of players and monsters standing on a foggy tile so they appear submerged in the volume.
 * Called in DrawDungeon() immediately after DrawPlayerHelper() and DrawMonsterHelper().
 */
void DrawGroundFogEntitySubmersion(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex);

} // namespace devilution
