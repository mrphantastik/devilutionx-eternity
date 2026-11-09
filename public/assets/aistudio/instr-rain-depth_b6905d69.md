# Implementation Specification: 3D Multi-Plane Depth Rain Upgrade (`weather.cpp`) — Wall/Roof-Safe Edition

## 1. Why the Current Rain Looks Like 1 Flat Layer
In the current `Source/engine/render/weather.cpp`:
1. **Identical Camera Parallax (`1.0x`) on All Drops (`lines 843–844`):**
   Every raindrop—regardless of `drop.layer` (`0, 1, 2`)—subtracts the exact same `1.0f * camDeltaX` and `1.0f * camDeltaY` when the player walks. Because distant drops and near-camera drops scroll at the exact same speed as the ground, the human eye perceives all raindrops as lying on a single flat 2D plane.
2. **No Scene Occlusion (All Drops Drawn Over Roofs/Trees/Buildings):**
   All three rain layers are drawn in `DrawWeatherAir()` *after* `DrawTileContent()`. Even "distant" Layer 0 drops streak straight over the front of Griswold's roof, tree canopies, and character sprites.
3. **Minimal Visual Separation Between Layers (`lines 304–319`):**
   All three layers are 1-pixel-wide lines with nearly identical speeds (`0.80x` vs `0.96x` vs `1.14x`) and a static wind angle.

---

## 2. Preventing the "Rain Hitting Side of Walls / Rooftops" Problem
In isometric engines (like `fallout-ce` and `DevilutionX`), if an air-layer raindrop terminates mid-screen and draws a splash after walls/roofs are rendered, it will visually "hit" the vertical side of house walls, cliffs/rocks, and rooftops.

To **guarantee raindrops and splashes never hit the side of walls, rocks, or rooftops**:
1. **Keep 100% of Ground Impacts/Splashes Inside `DrawWeatherGroundTile()`:**
   - `DrawWeatherGroundTile()` already checks `IsOutdoorWalkableGround(tilePosition)` (`!TileHasAny(piece, TileProperties::Solid)`), so splashes **only** spawn on walkable ground tiles (never on rooftops, rocks, rivers, or walls).
   - Furthermore, `DrawWeatherGroundTile()` runs inside `DrawCell()` right after floor microtiles `mt[0..1]` and *before* upper wall/roof microtiles `mt[2..15]` and tree leaves (`dSpecial`). Any overhanging roof, wall, rock, tree, or character in front of that tile naturally paints over the splash!
2. **Exploit `DrawFloor()` vs. `DrawTileContent()` for Free Building/Roof/Tree Occlusion (`Layer 0`):**
   - In `Source/engine/render/scrollrt.cpp` (`DrawGame`, lines 1108–1111):
     - `DrawFloor()` renders **only non-solid walkable ground tiles** first.
     - `DrawTileContent()` then renders all solid tiles (rocks, walls, houses), overhanging roofs (`mt[2..15]`), trees (`dSpecial`), town NPCs, and players on top.
   - By drawing **Layer 0 (Distant Background Rain)** in `DrawWeatherBackground()` right *between* `DrawFloor()` and `DrawTileContent()`, `DrawTileContent()` automatically paints every solid wall, rock, rooftop, tree, and character **over** the distant rain!
3. **Air Layers (`Layer 1` & `Layer 2`) Are Pure Passing Air Streaks (No Mid-Screen Wall Impacts):**
   - `Layer 1` (Midground, `1.00x` parallax) and `Layer 2` (Near-Camera Lens, `1.38x` parallax) pass continuously through the air volume in front of the camera without terminating or splashing on wall faces.

---

## 3. Multi-Plane Depth Parameters

| Layer | Role | Render Pass | Camera Parallax | Speed Scale | Streak Length | Thickness & Opacity |
| :---: | :--- | :--- | :---: | :---: | :---: | :--- |
| **Layer 0** | Distant / Background Rain | `DrawWeatherBackground()` *(occluded behind roofs/trees/walls/rocks)* | **`0.72x`** *(moves slower than ground)* | `0.62x .. 0.76x` | `7 .. 10 px` | 1px fine (`Blend25` head -> `Blend12` tail) |
| **Layer 1** | Midground Air Rain | `DrawWeatherAir()` *(world-plane air streaks)* | **`1.00x`** *(locked to world plane)* | `0.95x .. 1.12x` | `14 .. 19 px` | 1px crisp (`Blend50` head -> `Blend25` mid -> `Blend12` tail) |
| **Layer 2** | Near-Camera "Lens" Rain | `DrawWeatherAir()` *(close to camera lens)* | **`1.38x`** *(whips past camera when walking)* | `1.45x .. 1.70x` | `26 .. 36 px` | **2px Depth-of-Field Blur** (`Blend75`/`Blend50` core + `Blend12` 1px lateral blur halo) |

---

## 4. Step-by-Step C++ Changes

### Step 1: Declare `DrawWeatherBackground` in `Source/engine/render/weather.h`
In `Source/engine/render/weather.h`, right above `DrawWeatherAir`:

```cpp
/**
 * @brief Renders distant background rain (Layer 0) immediately after DrawFloor()
 * and before DrawTileContent() so buildings, roofs, rocks, trees, and characters
 * naturally occlude distant rain sheets.
 */
void DrawWeatherBackground(const Surface &out, Point startTilePosition, Displacement offset);

/**
 * @brief Renders non-overlapping drifting precipitation/steam fog banks, midground
 * rain streaks (Layer 1), and near-camera out-of-focus foreground rain (Layer 2).
 * Called in DrawGame() after DrawTileContent() and before Zoom().
 */
void DrawWeatherAir(const Surface &out, Point startTilePosition, Displacement offset);
```

---

### Step 2: Hook `DrawWeatherBackground` in `Source/engine/render/scrollrt.cpp`
In `Source/engine/render/scrollrt.cpp` inside `DrawGame()` (around line 1108):

```cpp
	DrawFloor(out, position, Point {} + offset, rows, columns);
	UpdateWeatherState(out, position, offset);
	DrawWeatherBackground(out, position, offset);
	DrawTileContent(out, position, Point {} + offset, rows, columns);
	DrawWeatherAir(out, position, offset);
```

---

### Step 3: Upgrade `RainDrop` Struct & Functions in `Source/engine/render/weather.cpp`

#### 3.1. Update `struct RainDrop` (`Source/engine/render/weather.cpp`, around line 66)
Add `parallaxScale` to `struct RainDrop`:

```cpp
struct RainDrop {
	float x = 0.0F;
	float y = 0.0F;
	float speedScale = 1.0F;
	float parallaxScale = 1.0F; // 0.72 (Layer 0 far), 1.00 (Layer 1 mid), 1.38 (Layer 2 near camera)
	uint8_t length = 12;
	uint8_t layer = 0;          // 0 = far/occluded, 1 = mid, 2 = near/DoF blurred
	uint8_t colorIndex = PAL16_BLUE + 4;
};
```

#### 3.2. Update `InitRainDrop` (`Source/engine/render/weather.cpp`, around line 295)
Replace `InitRainDrop` with true 3-plane depth parameters:

```cpp
void InitRainDrop(RainDrop &drop, int viewW, int viewH, bool randomY)
{
	const int marginX = 128;
	drop.x = static_cast<float>(GenerateRnd(std::max(1, viewW + marginX * 2)) - marginX);
	drop.y = randomY
	    ? static_cast<float>(GenerateRnd(std::max(1, viewH)))
	    : -static_cast<float>(GenerateRnd(64));

	const int roll = GenerateRnd(100);
	if (roll < 38) {
		// Layer 0: Distant background rain (drawn behind roofs, rocks, trees, and walls)
		drop.layer = 0;
		drop.parallaxScale = 0.72F;
		drop.speedScale = 0.62F + static_cast<float>(GenerateRnd(15)) * 0.01F;
		drop.length = static_cast<uint8_t>(7 + GenerateRnd(4)); // 7..10 px
		drop.colorIndex = static_cast<uint8_t>(PAL16_BLUE + 6 + GenerateRnd(3));
	} else if (roll < 82) {
		// Layer 1: Midground world-plane air rain
		drop.layer = 1;
		drop.parallaxScale = 1.00F;
		drop.speedScale = 0.95F + static_cast<float>(GenerateRnd(18)) * 0.01F;
		drop.length = static_cast<uint8_t>(14 + GenerateRnd(6)); // 14..19 px
		drop.colorIndex = static_cast<uint8_t>((GenerateRnd(2) == 0 ? PAL16_BLUE : PAL16_GRAY) + 3 + GenerateRnd(3));
	} else {
		// Layer 2: Near-camera foreground rain (whips past lens with 2px DoF softness)
		drop.layer = 2;
		drop.parallaxScale = 1.38F;
		drop.speedScale = 1.45F + static_cast<float>(GenerateRnd(26)) * 0.01F;
		drop.length = static_cast<uint8_t>(26 + GenerateRnd(11)); // 26..36 px
		drop.colorIndex = static_cast<uint8_t>((GenerateRnd(2) == 0 ? PAL16_BLUE : PAL16_GRAY) + 2 + GenerateRnd(2));
	}
}
```

#### 3.3. Update Raindrop Motion in `UpdateWeatherState` (`Source/engine/render/weather.cpp`, around line 841)
Apply `drop.parallaxScale` to `camDeltaX` and `camDeltaY`:

```cpp
	for (size_t i = 0; i < activeDropCount; ++i) {
		RainDrop &drop = gRainDrops[i];
		const float pScale = drop.parallaxScale;
		drop.x += windVelX * drop.speedScale * dt - static_cast<float>(camDeltaX) * pScale;
		drop.y += fallVelY * drop.speedScale * dt - static_cast<float>(camDeltaY) * pScale;

		if (drop.y > static_cast<float>(viewH + 36)) {
			InitRainDrop(drop, viewW, viewH, /*randomY=*/false);
		} else if (drop.y < -72.0F) {
			drop.y += static_cast<float>(viewH + 72);
		}

		while (drop.x < minX) {
			drop.x += spanX;
		}
		while (drop.x >= maxX) {
			drop.x -= spanX;
		}
	}
```

#### 3.4. Implement `DrawWeatherBackground` & Upgraded `DrawWeatherAir` (`Source/engine/render/weather.cpp`, around line 929)
Replace `DrawWeatherAir` with `DrawWeatherBackground` (for Layer 0 behind roofs/rocks/trees) and the upgraded `DrawWeatherAir` (for Steam Fog Banks + Layer 1 midground + Layer 2 2px DoF foreground, while `DrawWeatherGroundTile` continues handling all walkable ground puddle splashes):

```cpp
void DrawWeatherBackground(const Surface &out, Point /*startTilePosition*/, Displacement /*offset*/)
{
	if (!gbRunGame || leveltype != DTYPE_TOWN || setlevel) {
		return;
	}

	const size_t activeDropCount = static_cast<size_t>(
	    static_cast<float>(LightRainDropCount)
	    + static_cast<float>(HeavyRainDropCount - LightRainDropCount) * gHeavyBlend);

	const uint32_t nowTick = gLastWeatherTick != 0 ? gLastWeatherTick : SDL_GetTicks();
	const float gustPhase = static_cast<float>(nowTick) * 0.0025F;
	const float baseSlant = -(0.17F + 0.06F * gHeavyBlend);
	const int extraLen = static_cast<int>(3.0F * gHeavyBlend);

	// Render Layer 0 (Distant Background Rain) onto the ground buffer BEFORE DrawTileContent()
	// so buildings, roofs, rocks, trees, and characters naturally paint over and occlude it.
	for (size_t i = 0; i < activeDropCount; ++i) {
		const RainDrop &drop = gRainDrops[i];
		if (drop.layer != 0)
			continue;

		const int headX = static_cast<int>(drop.x);
		const int headY = static_cast<int>(drop.y);
		const int totalLen = static_cast<int>(drop.length) + extraLen;

		if (headY < 0 || headY - totalLen >= out.h() || headX < -24 || headX >= out.w() + 24)
			continue;

		const float gust = std::sin(drop.x * 0.006F + gustPhase) * 0.03F;
		const float slantXPerY = baseSlant + gust;

		for (int step = 0; step < totalLen; ++step) {
			const int py = headY - step;
			const int px = headX - static_cast<int>(static_cast<float>(step) * slantXPerY);
			if (px < 0 || px >= out.w() || py < 0 || py >= out.h())
				continue;

			if (step * 3 < totalLen) {
				PlotBlended25(out, px, py, drop.colorIndex);
			} else {
				PlotBlended12(out, px, py, drop.colorIndex);
			}
		}
	}
}

void DrawWeatherAir(const Surface &out, Point startTilePosition, Displacement offset)
{
	if (!gbRunGame || leveltype != DTYPE_TOWN || setlevel) {
		return;
	}

	// 1. Render non-overlapping world-anchored drifting precipitation / steam fog banks
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

	// 2. Render Layer 1 (Midground Air Rain) and Layer 2 (Near-Camera DoF Foreground Rain)
	// Note: All ground impact splashes remain strictly inside DrawWeatherGroundTile() so rain
	// never splashes on the vertical sides of walls, rocks, or rooftops.
	const size_t activeDropCount = static_cast<size_t>(
	    static_cast<float>(LightRainDropCount)
	    + static_cast<float>(HeavyRainDropCount - LightRainDropCount) * gHeavyBlend);

	const uint32_t nowTick = gLastWeatherTick != 0 ? gLastWeatherTick : SDL_GetTicks();
	const float gustPhase = static_cast<float>(nowTick) * 0.0032F;
	const float baseSlant = -(0.20F + 0.08F * gHeavyBlend);
	const int extraLen = static_cast<int>(6.0F * gHeavyBlend);

	for (size_t i = 0; i < activeDropCount; ++i) {
		const RainDrop &drop = gRainDrops[i];
		if (drop.layer == 0)
			continue; // Already drawn behind roofs/rocks/trees in DrawWeatherBackground()

		const int headX = static_cast<int>(drop.x);
		const int headY = static_cast<int>(drop.y);
		const int totalLen = static_cast<int>(drop.length) + extraLen;

		if (headY < 0 || headY - totalLen >= out.h() || headX < -32 || headX >= out.w() + 32)
			continue;

		const float gust = std::sin(drop.x * 0.007F + gustPhase) * 0.045F;
		const float slantXPerY = baseSlant * (drop.layer == 2 ? 1.15F : 1.0F) + gust;

		if (drop.layer == 1) {
			// Midground air streak: crisp 3-stage vertical fade (50% -> 25% -> 12.5%)
			for (int step = 0; step < totalLen; ++step) {
				const int py = headY - step;
				const int px = headX - static_cast<int>(static_cast<float>(step) * slantXPerY);
				if (px < 0 || px >= out.w() || py < 0 || py >= out.h())
					continue;

				if (step * 3 < totalLen) {
					PlotBlended50(out, px, py, drop.colorIndex);
				} else if (step * 3 < totalLen * 2) {
					PlotBlended25(out, px, py, drop.colorIndex);
				} else {
					PlotBlended12(out, px, py, drop.colorIndex);
				}
			}
		} else {
			// Layer 2 (Near-Camera Foreground): fast, long streak with 2px soft Depth-of-Field blur
			for (int step = 0; step < totalLen; ++step) {
				const int py = headY - step;
				const int px = headX - static_cast<int>(static_cast<float>(step) * slantXPerY);
				if (px < 1 || px >= out.w() - 1 || py < 0 || py >= out.h())
					continue;

				if (step * 4 < totalLen) {
					PlotBlended75(out, px, py, drop.colorIndex);
					PlotBlended12(out, px + 1, py, drop.colorIndex);
				} else if (step * 2 < totalLen) {
					PlotBlended50(out, px, py, drop.colorIndex);
					PlotBlended12(out, px + 1, py, drop.colorIndex);
				} else {
					PlotBlended25(out, px, py, drop.colorIndex);
				}
			}
		}
	}
}
```
