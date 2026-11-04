# DX:Eternity Development Log (DevilutionX 1.5.4 Fork)

**Base Commit:** `9f30593b4c2e706d6d6a66edfbc3aa6ada8861b4` (DevilutionX 1.5.4)  
**Target Architecture:** Windows 64-bit (`x86_64-w64-mingw32`, static/self-contained `dx-eternity.exe`)

---

## Phase 1: Quality of Life & Endgame Loop (Completed & Verified)

### 1.1 Town Shortcuts Open in Single Player (Nightmare & Hell)
- **File:** `Source/levels/trigs.cpp` — `IsWarpOpen(dungeon_type)`
- **Change:** In Single Player (`!gbIsMultiplayer`), returns `true` unconditionally for `DTYPE_CATACOMBS`, `DTYPE_CAVES`, and `DTYPE_HELL`, opening the town shortcuts to Catacombs (Level 5), Caves (Level 9), and Hell (Level 13) right from the start of a game session.

### 1.2 Endless Single Player Session & Diablo Loot Drop
- **File:** `Source/monster.cpp` — `DiabloDeath(Monster &)` & `MonsterDeath(Monster &, ...)`
- **Change:**
  - `DiabloDeath()`: Changed `gbProcessPlayers = false` to `gbProcessPlayers = !gbIsMultiplayer` so Single Player movement/input is not frozen when Diablo dies.
  - `MonsterDeath()`: At `monster.var1 == 140` in Single Player (`!gbIsMultiplayer`), skips `PrepDoEnding()`, spawns `MissileID::DiabloApocalypseBoom` at Diablo's tile, clears `dMonster[x][y] = 0`, marks `monster.isInvalid = true`, and updates monster relations (`M_UpdateRelations(monster)`).
- **File:** `Source/items.cpp` — `SpawnItem(Monster &, ...)`
- **Change:** Treats `MT_DIABLO` alongside unique monsters (`monster.isUnique() || monster.type().type == MT_DIABLO`) so Diablo drops loot (`uper = 15`, `RndUItem(monster)`) without applying the `-15` qlvl penalty in classic Diablo 1 mode.

---

## Phase 2: Custom Unique Weapon (`Kadi's Fury`) & FastWalk Fix (Completed & Verified)

### 2.1 Item Effect & Unique ID Definitions
- **File:** `Source/itemdat.h`
  - Added `FastWalk = 1 << 7` to `enum class ItemSpecialEffectHf : uint8_t`.
  - Added `IPL_FASTWALK` to `item_effect_type`.
- **File:** `Source/items.h`
  - Added `UITEM_KADISFURY = 110` to `_unique_items`.

### 2.2 `Kadi's Fury` Unique Item Specification
- **File:** `Source/itemdat.cpp` — `UniqueItemList[]`
  - Added `Kadi's Fury` at index `110` (`UITEM_KADISFURY`):
    - Base Type: `UITYPE_BROADSWR` (Broad Sword), `qlevel = 1`, `maxValue = 25000`
    - Affix 1: `IPL_TOHIT` (`+100% To Hit`)
    - Affix 2: `IPL_DMG_PERC` (`+175% Damage`)
    - Affix 3: `IPL_FASTATTACK` (`4` = Fastest Attack)
    - Affix 4: `IPL_LIGHTDAM` (`4` to `16` Lightning Damage)
    - Affix 5: `IPL_FASTWALK` (`faster walk`)
    - Affix 6: `IPL_ALLRES` (`-30%` to All Resistances)

### 2.3 Affix Application, Unique Availability & Save/Load Persistence in Classic D1 Mode
- **File:** `Source/items.cpp`
  - `SaveItemPower()`:
    - Removed `std::max(r, 0)` clamping on `IPL_ALLRES` so negative resistance penalties (`-30%`) apply properly.
    - Added `case IPL_FASTWALK: item._iDamAcFlags |= ItemSpecialEffectHf::FastWalk; break;`.
  - `IsUniqueAvailable(int i)`: Allowed `i == UITEM_KADISFURY` even when `!gbIsHellfire`.
  - `CheckUnique()` & `DebugSpawnUniqueItem()`: Changed `if (!IsUniqueAvailable(j)) break;` to `continue;` so scanning past unavailable Hellfire items reaches `UITEM_KADISFURY` (index 110).
  - `GetUniqueItem()` & `SetupAllItems()`: Stored `uid` in `item._iSeed` for `UITEM_KADISFURY` and allowed `RecreateItem()` (`SetupAllItems`) to restore `GetUniqueItem(item, iseed)` when `recreate && iseed == UITEM_KADISFURY`, preserving the item across `PackItem`/`UnPackItem` in classic Diablo 1 mode.
  - `SpawnUnique()`: Initialized `item._iCreateInfo = curlv` and allowed `uid == UITEM_KADISFURY` to spawn on all difficulties (`DIFF_NORMAL`, `DIFF_NIGHTMARE`, `DIFF_HELL`).
  - `CalcPlrItemVals()`: Ensured `item._iDamAcFlags |= ItemSpecialEffectHf::FastWalk` is set when `item._iUid == UITEM_KADISFURY` or `item._iSeed == UITEM_KADISFURY`.
  - `PrintItemPower()`: Added `case IPL_FASTWALK: return _("faster walk");`.
- **File:** `Source/loadsave.cpp`
  - `LoadItemData()`: Classic Diablo 1 saves (`!gbIsHellfireSaveGame`) do not serialize `_iDamAcFlags` in the 36-byte item struct and zero it on load. Added restoration of `item._iDamAcFlags = ItemSpecialEffectHf::FastWalk` whenever `item._iMagical == ITEM_QUALITY_UNIQUE && (item._iUid == UITEM_KADISFURY || item._iSeed == UITEM_KADISFURY)`.
  - `RemoveInvalidItem()`: Exempted `UITEM_KADISFURY` from the `!gbIsHellfire && item._iDamAcFlags != ItemSpecialEffectHf::None` anti-cheat strip during level transitions and game loads.
- **File:** `Source/player.cpp`
  - `StartWalkAnimation()`: Checked `HasAnyOf(player.pDamAcFlags, ItemSpecialEffectHf::FastWalk)` plus an explicit `player.InvBody` fallback check for `FastWalk` / `UITEM_KADISFURY`, adding `skippedFrames += 3` to walk animations **only outside of Town** (`leveltype != DTYPE_TOWN`) so it does not stack uncontrollably with `Run in Town`.

---

## Phase 3: Nest & Crypt Dungeon Staircase Transitions & Town Walk Fix (Current Build)

*(Note: Extended Mode & isolated saves/stash were scrapped per user direction.)*

### 3.1 Disable `Kadi's Fury` Walk Speed Bonus in Town
- **File:** `Source/player.cpp` — `StartWalkAnimation()`
- **Change:** Gated the `FastWalk` check and `skippedFrames += 3` bonus behind `if (leveltype != DTYPE_TOWN)` so Town movement uses standard walk/Run-in-Town speed without overspeeding.

### 3.2 Remove Temporary Butcher Drop for `Kadi's Fury`
- **File:** `Source/monster.cpp` — `SpawnLoot()`
- **Change:** Removed the temporary `if (monster.type().type == MT_CLEAVER) SpawnUnique(UITEM_KADISFURY, ...)` block; `Kadi's Fury` now drops naturally via standard unique item generation.

### 3.3 Caves L12 <-> Nest L17 & Hell L14 <-> Crypt L21 Staircase Transitions
- **File:** `Source/levels/drlg_l3.cpp` — `PlaceCaveStairs()`
  - Placed a second `L3DOWN` ("stairs down") staircase miniset on Caves Level 12 (`currlevel == 12`), recorded its mega-tile origin in `L3AltStairsPos`, and positioned the player view at `position->megaToWorld() + Displacement { 3, 1 }` when returning up from Nest Level 17 (`entry == ENTRY_TWARPDN || entry == ENTRY_TWARPUP`).
- **File:** `Source/levels/drlg_l4.cpp` — `PlaceStairs()`
  - Placed a second `L4DSTAIRS` ("stairs down") staircase miniset on Hell Level 14 (`currlevel == 14`), recorded its mega-tile origin in `L4AltStairsPos`, and positioned the player view at `position->megaToWorld() + Displacement { 7, 5 }` when returning up from Crypt Level 21 (`entry == ENTRY_TWARPDN || entry == ENTRY_TWARPUP`).
- **File:** `Source/levels/trigs.cpp`
  - `InitL3Triggers()`: On Caves Level 12 (`currlevel == 12`), distinguished the second `dPiece == 167` staircase via `WorldTileRectangle { L3AltStairsPos.megaToWorld(), { 6, 6 } }.contains({ i, j })` and set its trigger to `_tmsg = WM_DIABTOWNWARP; _tlvl = 17;` ("Down to Hive").
  - `InitL4Triggers()`: On Hell Level 14 (`currlevel == 14`), distinguished the second `dPiece == 119` staircase via `WorldTileRectangle { L4AltStairsPos.megaToWorld(), { 10, 10 } }.contains({ i, j })` and set its trigger to `_tmsg = WM_DIABTOWNWARP; _tlvl = 21;` ("Down to Crypt").
  - `InitHiveTriggers()`: Set Nest Level 17 top staircase (`dPiece == 79`) trigger to `_tmsg = WM_DIABTWARPUP; _tlvl = 12;` (Up to Caves Level 12).
  - `InitCryptTriggers()`: Set Crypt Level 21 top staircase (`dPiece == 183`) trigger to `_tmsg = WM_DIABTWARPUP; _tlvl = 14;` (Up to Hell Level 14).
  - `ForceL3Trig()`, `ForceL4Trig()`, `ForceHiveTrig()`, `ForceCryptTrig()`: Matched `L3DownList` and `L4DownList` triggers by proximity (`dx < 4 && dy < 4`) so hovering either down-staircase on Caves L12 or Hell L14 displays the corresponding tooltip (`"Down to level 13"` vs `"Down to Hive"`, `"Down to level 15"` vs `"Down to Crypt"`) and snaps the cursor to the right staircase.
  - `CheckTriggers()`: Updated `case WM_DIABTWARPUP:` to pass `trigs[i]._tlvl` instead of hardcoded `0` to `StartNewLvl()`.
- **File:** `Source/player.cpp` — `StartNewLvl()`
  - In `case WM_DIABTWARPUP:`, guarded `MyPlayer->pTownWarps |= 1 << (leveltype - 2);` behind `if (lvl == 0)` so ascending from Nest L17 to Caves L12 or Crypt L21 to Hell L14 does not corrupt town warp flags.
- **File:** `Source/diablo.cpp` & `Source/engine/assets.cpp`
  - Set `giNumberOfLevels = 25` and enabled asset lookup fallback to `hellfire.mpq`/`hf*.mpq` after `diabdat.mpq` so Nest (17–20) and Crypt (21–24) tilesets, monsters, and cutscenes load seamlessly even when playing a classic Diablo campaign.

### 3.4 Hive & Crypt Save/Load Support in Classic Diablo Mode
- **File:** `Source/loadsave.cpp` & `Source/pfile.cpp`
  - Removed the `if (!gbIsHellfire && IsAnyOf(leveltype, DTYPE_NEST, DTYPE_CRYPT)) app_fatal(_("Player is on a Hellfire only level"));` guard in `LoadGame()`.
  - Kept the runtime global `giNumberOfLevels = 25` across `LoadGame()`, `SaveGameData()`, and `pfile_ui_save_create()` so `pfile.cpp` (`RenameTempToPerm`, `pfile_remove_temp_files`, `GetFileName`) properly manages `templ17`–`templ24` and `perml17`–`perml24` level files inside `.sv` save archives.
  - Preserved 100% binary compatibility with existing classic Diablo (`"RETL"`) `.sv` files by reading/writing `numSavedLevels = gbIsHellfire ? 25 : 17` inside the fixed-layout `"game"` stream (`LoadGame`, `SaveGameData`, `LoadPlayer`, `SavePlayer`), while persisting extended level seeds (`glSeedTbl[17..24]`), visited flags (`_pLvlVisited[17..24]`), and `pDungMsgs2` in an auxiliary `"extlevels"` save archive record (`SaveExtLevels` / `LoadExtLevels`), with deterministic seed fallback for pre-existing saves.

### 3.5 Audio Lock Contention, Projectile SFX & Balrog Flame FPS Drop Fix
- **Build Configuration (`CMAKE_BUILD_TYPE=Release`):**
  - Configured the CMake/Ninja Windows 64-bit cross-compile build with `-DCMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG -flto=auto`), enabling full compiler optimization and link-time optimization across `libdevilutionx`, `SDL_audiolib`, `dr_wav`, and `speex_resampler` (which previously defaulted to an unoptimized `-O0` build when `CMAKE_BUILD_TYPE` was unset).
- **File:** `Source/player.cpp`
  - `DoWalk()`, `DoAttack()`, `DoRangeAttack()`: Added `player.AnimInfo.tickCounterOfCurrentFrame == 0` checks to all frame-based sound (`PS_WALK1`, `PS_SWING`) and attack/arrow triggers so multi-tick animation frames (`ticksPerFrame > 1`) only fire once on the initial tick of the action frame.
- **File:** `Source/monster.cpp`
  - `MonsterAttack()`, `MonsterRangedAttack()`, `MonsterSpecialAttack()`, `MonsterSpecialStand()`: Added `monster.animInfo.tickCounterOfCurrentFrame == 0` guards to all frame-based attack, ranged projectile, and special sound triggers (matching `MonsterRangedSpecialAttack()`), preventing multi-tick sound and projectile spam.
- **File:** `Source/missiles.cpp`
  - `ProcessInfernoControl()`: Passed `missile.var3 == 0 ? std::optional<_sfx_id> {} : std::optional<_sfx_id> { SFX_NONE }` to `AddMissile(..., MissileID::Inferno, ...)` so Balrog flame breath (`MissileID::InfernoControl`) only plays `LS_SPOUTSTR` once on the initial parent flame spawn (`var3 == 0`) rather than spawning overlapping 2-second loop streams on every child flame segment.
  - `SpawnLightning()`: Throttled `MissileID::Lightning` / `ThinLightning` child segments so `LS_LNING1` plays only on the initial segment spawn (`isFirstSegment`) rather than on every tile crossed by `LightningControl`.
- **File:** `Source/effects.cpp`, `Source/engine/sound.cpp` & `Source/utils/soundsample.h`
  - `PlaySfxLoc()`: Removed the `pSnd->start_tc = 0` reset for `psfx` `0..3` (`PS_WALK1`, `PS_BFIRE`, `PS_TMAG`, `PS_SWING`) that bypassed `snd_play_snd()`'s 80ms debounce window.
  - `PrivSoundInit()` & `sound_file_load()`: Preloaded Hellfire sound effects when present even in classic Diablo mode so Nest/Crypt combat SFX do not lazy-load from MPQ archives mid-combat.
  - `DuplicateSound()` & `SoundSample::MatchesData()`: Pooled finished duplicate `SoundSample` streams by shared audio buffer (`file_data_.get()`) so overlapping projectile SFX reuse already-opened `Aulib::Stream` and `ResamplerSpeex` instances without heap reallocation or `SDL_LockAudioDevice` stream-open/close thrashing; removed the SDL audio callback's `SetFinishCallback` mutex lock; added a 15ms global duplicate spawn interval and capped active duplicate streams at `MaxActiveDuplicateSounds = 8`.

---

## Phase 4: Pure Classic Diablo Loot Filter & Hell Diablo ~2% `Kadi's Fury` Drop

### 4.1 Hell Diablo ~2% `Kadi's Fury` Boss Drop
- **File:** `Source/monster.cpp` — `SpawnLoot()`
  - Added a 1-in-50 (`GenerateRnd(50) == 0`, `2%`) drop roll when `monster.type().type == MT_DIABLO && sgGameInitInfo.nDifficulty == DIFF_HELL` to spawn `UITEM_KADISFURY` alongside Diablo's normal loot drop.
- **File:** `Source/items.cpp` — `CheckUnique()`
  - Excluded `UITEM_KADISFURY` from random world unique rolls (`if (!recreate && j == UITEM_KADISFURY) continue;`) so `Griswold's Edge` remains the standard unique Broad Sword world drop while `Kadi's Fury` is exclusive to Hell Diablo (and still recreates cleanly on save/load when `recreate == true`).

### 4.2 Pure Classic Diablo Loot in Nest & Crypt
- **File:** `Source/items.cpp` — `IsItemAvailable()` & `InitItems()`
  - Blocked all Hellfire oils (`IMISC_OILFIRST..IMISC_OILLAST`), runes (`IMISC_RUNEFIRST..IMISC_RUNELAST`), and Hellfire-only base items (`i > 160`) when `!gbIsHellfire`.
  - Enabled floor potion initialization (`AddInitItems()`) on Nest and Crypt levels (`currlevel > 0 && currlevel != 16`) and gated `SpawnNote()` behind `gbIsHellfire`.
- **File:** `Source/monster.cpp` — `SpawnLoot()`
  - Updated `UniqueMonsterType::Defiler` to drop a standard boss item (`SpawnItem(monster, ...)`) instead of the Hellfire-only `Cathedral Map` (`SpawnMapOfDoom`) when `!gbIsHellfire`.
