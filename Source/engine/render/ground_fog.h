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

void DrawCavesLavaBubblesTile(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex);

void DrawGroundFogTileSpan(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex, int dyStart, int dyEnd);

void DrawGroundFogTileBase(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex);

void DrawGroundFogEntitySubmersion(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightIndex);

} // namespace devilution
