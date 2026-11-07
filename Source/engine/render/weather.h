/**
 * @file weather.h
 *
 * Interface of Tristram weather & environmental particle effects (Light/Heavy Rain,
 * outdoor wet ground & glossy puddle reflections, localized semi-transparent ground
 * puddle splashes, non-overlapping precipitation/steam fog banks, curling ground-evaporation
 * wisps, distant lightning palette flashes with delayed thunder audio, and ambient rain loops).
 */
#pragma once

#include "engine/point.hpp"
#include "engine/surface.hpp"
#include "levels/gendung.h"

namespace devilution {

enum class RainMode : uint8_t {
	Light,
	Heavy,
};

/**
 * @brief Called when starting a new game session so initial Tristram rain intensity
 * is freshly rolled and dungeon timer state is reset.
 */
void OnWeatherNewGame();

/**
 * @brief Called during LoadGameLevel() to track transitions between Town and dungeons
 * (recording dungeon entry time and only allowing rain intensity to change if the player
 * spent at least 60 seconds in a dungeon) and immediately stop weather sounds outside Town.
 */
void OnWeatherLevelLoad(bool firstflag, dungeon_type levelType);

/**
 * @brief Updates the weather simulation (raindrops, precipitation/steam fog banks,
 * wet ground shimmer, puddle splashes, lightning flashes, delayed thunder claps,
 * and ambient rain audio loops) once per rendered frame in Town.
 */
void UpdateWeatherState(const Surface &out, Point startTilePosition, Displacement offset);

/**
 * @brief Renders outdoor wet/glossy ground reflections, localized puddle splash animations,
 * and rising ground-steam wisps on a single walkable outdoor ground tile.
 * Called in DrawCell() immediately after floor microtiles mt[0..1] and before upper
 * microtiles mt[2..15] so overhanging roofs/trees, objects, players, and NPCs naturally
 * occlude the wet ground.
 */
void DrawWeatherGroundTile(const Surface &out, Point tilePosition, Point targetBufferPosition);

/**
 * @brief Renders non-overlapping drifting precipitation/steam fog banks and falling
 * raindrop streaks across the viewport. Called in DrawGame() after DrawTileContent()
 * and before Zoom().
 */
void DrawWeatherAir(const Surface &out, Point startTilePosition, Displacement offset);

/**
 * @brief Stops any active weather audio loops/thunder and restores the palette if a lightning
 * flash was active when leaving Town or exiting to the Main Menu.
 */
void ResetWeatherState();

} // namespace devilution
