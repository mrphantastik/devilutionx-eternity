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

### 4.3 Additional Hell Diablo ~2% Exclusive Uniques (`Apostate's Crook` & `Loxley's Bend`)
- **File:** `Source/items.h` & `Source/itemdat.h`
  - Added `UITEM_APOSTATESCROOK = 111` and `UITEM_LOXLEYSBEND = 112` to `enum _unique_items`.
  - Added `IPL_MAXDAM` to `enum item_effect_type` for flat maximum damage bonuses on unique weapons.
- **File:** `Source/itemdat.cpp` — `UniqueItems[]`
  - Added **Apostate's Crook** (`UITYPE_WARSTAFF`, War Staff base: Damage `8–16`, Durability `75`):
    - `+2 to All Spell Levels` (`{ IPL_SPLLVLADD, 2, 2 }`)
    - `+40 to Magic` (`{ IPL_MAG, 40, 40 }`)
    - `+80 to Maximum Mana` (`{ IPL_MANA, 80, 80 }`)
    - `Damage Reduced by 8` (`{ IPL_GETHIT, 8, 8 }`)
    - `-50 to Armor Class` (`{ IPL_AC_CURSE, 50, 50 }`)
    - `Resist All: -30%` (`{ IPL_ALLRES, -30, -30 }`)
  - Added **Loxley's Bend** (`UITYPE_WARBOW`, Long War Bow base: Damage `1–14`, Durability `60`):
    - `Knocks Target Back` (`{ IPL_KNOCKBACK }`)
    - `+40 to Dexterity` (`{ IPL_DEX, 40, 40 }`)
    - `+30 to Maximum Damage` (`{ IPL_MAXDAM, 30, 30 }`)
    - `Steals 5% Mana` (`{ IPL_STEALMANA, 5, 5 }`)
    - `-40% Light Radius` (`{ IPL_LIGHT_CURSE, 4, 4 }`)
    - `-50 to Maximum Life` (`{ IPL_LIFE_CURSE, 50, 50 }`)
- **File:** `Source/items.cpp`
  - Handled `IPL_MAXDAM` in `SaveItemPower()` (`item._iMaxDam += r;`) and `PrintItemPower()` (`"{:+d} to maximum damage"`).
  - Registered `UITEM_APOSTATESCROOK` and `UITEM_LOXLEYSBEND` alongside `UITEM_KADISFURY` in `CheckUnique()` (excluded from random world unique drops when `!recreate`), `GetUniqueItem()`, `SetupAllItems()`, `IsUniqueAvailable()`, and `SpawnUnique()`.
- **File:** `Source/missiles.cpp` — `MonsterMHit()`
  - Enabled `StealMana3`, `StealMana5`, `StealLife3`, and `StealLife5` on physical arrow hits (`missileData.isArrow() && damageType == DamageType::Physical`) so `Loxley's Bend` steals 5% mana on bow attacks.
- **File:** `Source/monster.cpp` — `SpawnLoot()`
  - Added independent `GenerateRnd(50) == 0` (`2%`) drop rolls for `UITEM_APOSTATESCROOK` and `UITEM_LOXLEYSBEND` alongside `UITEM_KADISFURY` when `monster.type().type == MT_DIABLO && sgGameInitInfo.nDifficulty == DIFF_HELL`.

---

## Phase 5: Female Barbarian Class (Rogue Visual & Audio Assets)

### 5.1 Character Creation & Female UI Portrait
- **File:** `Source/DiabloUI/hero/selhero.cpp` — `SelheroListSelect()` & `SelheroGenerateName()`
  - Enabled `HeroClass::Barbarian` in character creation whenever `!gbIsSpawn` (without requiring `diablo.ini` test flags or `hfbarb.mpq`).
  - Updated default prefill names for `HeroClass::Barbarian` to female warrior/shieldmaiden names (`"Sonya"`, `"Freya"`, `"Boudica"`, `"Astrid"`, `"Valkyrie"`, etc.).
- **File:** `Source/DiabloUI/diabloui.cpp` — `LoadHeros()`
  - Mapped `ArtHeroPortraitOrder[static_cast<std::size_t>(HeroClass::Barbarian)] = 1` and `ArtHeroOverrides[Barbarian]` to `HeroClass::Rogue` (`1`) so the Barbarian displays the female Rogue portrait in hero selection and save menus.
- **File:** `Source/inv.cpp` — `InitInv()`
  - Mapped `HeroClass::Barbarian` to `data\inv\inv_rog` so the inventory paperdoll displays the female Rogue silhouette.

### 5.2 Rogue Sprites, Animations, Voice & Cinematics with Full Barbarian Mechanics
- **File:** `Source/playerdat.cpp`
  - Preserved all Barbarian stats and scaling (`baseStr=40, maxStr=255, baseMag=0, maxMag=0, baseDex=20, maxDex=55, baseVit=25, maxVit=150, blockBonus=30, itmLife=2.5x`, `SpellID::Rage`), innate resistance/two-handed weapon mastery, and starting equipment.
  - Mapped `PlayersData[HeroClass::Barbarian].classPath` to `"rogue"`, `PlayersSpriteData[HeroClass::Barbarian]` to Rogue sprite widths (`bow=128`), and `PlayersAnimData[HeroClass::Barbarian]` to match the Rogue `.cl2` directional frame counts (`18` sword/mace/unarmed, `22` axe, `12` bow, `8` idle/walk, `4` block, `20` death, `16` cast) while retaining fast Barbarian melee hit-frame timing (`actionFrame = 8..9`).
  - Replaced `herosounds[HeroClass::Barbarian]` with the female Rogue voice table (`PS_ROGUE1..PS_ROGUE102, PS_ROGUE71`).
- **File:** `Source/player.cpp` & `Source/engine/trn.cpp`
  - `GetPlayerSpriteClass(HeroClass::Barbarian)` unconditionally returns `HeroClass::Rogue` so `LoadPlrGFX()` loads `plrgfx\rogue\r*` `.cl2` sheets; `SetPlrAnims()` no longer applies the Warrior-only 15-frame armored death override to Barbarian; `GetClassTRN()` maps `HeroClass::Barbarian` to `plrgfx\rogue.trn`.
- **File:** `Source/effects.cpp`, `Source/objects.cpp` & `Source/monster.cpp`
  - `sound_init()` preloads `sfx_ROGUE` for `HeroClass::Barbarian` in Single Player; dungeon lore books and quest tomes (`AddCryptObject`, `OperateSChambBk`, `InitObjects`) use the Rogue's female voiceovers (`TEXT_RBOOKA..C`, `TEXT_RBONER`, `TEXT_RBLINDING`, `TEXT_RBLOODY`, `TEXT_RBLOODWAR`); defeating Diablo plays the female Rogue victory cinematic (`gendata\diabvic3.smk`).

---

## Phase 6: Na-Krul Death Freeze Fix, Defiler Monologue Suppression & Nest/Crypt Monster Roster Swap

### 6.1 Na-Krul Death Freeze & Item Packet Validation Fix (Crypt Level 24 Boss)
- **Root Cause (Freeze):** When `MT_NAKRUL` died (`Source/monster.cpp` — `SpawnLoot()`), it called `CreateSpellBook(monster.position.tile, SpellID::Apocalypse, sendmsg, false)`. In classic Diablo mode (`!gbIsHellfire`), `GetSpellBookLevel(SpellID::Apocalypse)` returns `-1`, meaning `SetupAllItems()` can never roll `SpellID::Apocalypse` on a generated book. Because `CreateSpellBook()` only checked `GetSpellBookLevel(ispell)` inside `if (gbIsHellfire)`, it spun in an infinite `while (true)` loop when `!gbIsHellfire`.
- **Root Cause (Invalid Packet Chat Message):** When `CreateMagicWeapon` and `SpawnItem` dropped items on Crypt Level 24 (`ItemsGetCurrlevel() == 17`, `ilvl == 34`, or `MT_NAKRUL` `mlvl == 40`), `NetSendCmdPItem` validated the loopback `CMD_DROPITEM` packet via `IsPItemValid()` -> `IsDungeonItemValid()` / `IsUniqueMonsterItemValid()` (`Source/items/validation.cpp`). Because `!gbIsHellfire` (`isHellfireItem == false`), `IsDungeonItemValid()` capped valid dungeon `ilvl` at `15 * 2 = 30` and skipped `MT_NAKRUL` (`MonsterAvailability::Never`), printing `"Player '<Name>' sent an invalid packet."` in the chat log.
- **File:** `Source/items.cpp` — `CreateSpellBook()` & `SpawnItem()`
  - Moved the `int bookLvl = GetSpellBookLevel(ispell); if (bookLvl < 0) return;` guard outside `if (gbIsHellfire)` so any unrollable spell book ID returns immediately instead of hanging in an infinite loop.
  - Included `MT_NAKRUL` alongside `MT_DIABLO` in `SpawnItem()` (`RndUItem(&monster)` and `uper = 15`).
- **File:** `Source/items/validation.cpp` — `IsUniqueMonsterItemValid()` & `IsDungeonItemValid()`
  - Allowed `MT_DIABLO`, `MT_NAKRUL`, `MT_DEFILER`, and `MT_HORKDMN` item levels in both `IsUniqueMonsterItemValid()` and `IsDungeonItemValid()`, and raised the dungeon container/weapon `ilvl` ceiling in `IsDungeonItemValid()` to `(24 - 7) * 2 = 34` unconditionally.
- **File:** `Source/monster.cpp` — `SpawnLoot()`
  - Updated `case MT_NAKRUL:` so when `!gbIsHellfire`, Na-Krul drops a standard boss item via `SpawnItem(monster, monster.position.tile, sendmsg)` instead of attempting to spawn Hellfire-only `SpellID::Apocalypse` and `Uber` items.

### 6.2 Suppress Defiler Quest Monologues on Hive Levels
- **File:** `Source/player.cpp` — `PlayDungMsgs()`
  - Removed the `currlevel == 17` (`USFX_DEFILER1` / `TEXT_DEFILER1`) and `currlevel == 19` (`USFX_DEFILER3` / `TEXT_DEFILER3`) branches so the Defiler's quest pop-up monologues never trigger on any Hive level.

### 6.3 Preserved Unused Vanilla Monster Families & Original Palettes
- **File:** `Source/monstdat.cpp` & `Source/monster.cpp`
  - **Spider Lord (`MT_ARACHNON`, `MT_SPIDLORD`):** Kept both variants in the Nest/Crypt roster and restored `MT_ARACHNON`'s sprite and palette from Hellfire's `wspidr\wspidr` back to the unused vanilla `bspidr\bspidr` (`MonsterAIID::Acid`, spitting acid projectiles and leaving an `AcidPuddle` on death).
  - **Torchant (`MT_TORCHANT`):** Kept in Crypt (`dLvl 21–23`) with its original unused vanilla `fireman\fireman` assets (`trnFile = nullptr`) and `Fireball` ranged attack.
  - **Bone Demon (`MT_SKLWING`, `MT_BONEDEMN`):** Kept both variants in Crypt (`dLvl 21–24`) and restored `MT_SKLWING`'s `trnFile` from `"monsters\\sklwing\\sklwx.trn"` to `nullptr` so both use their original vanilla `sklwing\sklwing` palette while keeping their `BoneDemon` projectile.
  - **Devil Kin Brute (`MT_BIGFALL`):** Kept in Crypt (`dLvl 21–22`) with its original unused vanilla `bigfall\bigfall` assets (`trnFile = nullptr`).
  - **Invisible Lord (`MT_FELLTWIN`, `MT_INVILORD`):** Kept `MT_FELLTWIN` (`dLvl 18–19`) and enabled `MT_INVILORD` (`MonsterAvailability::Retail`, `dLvl 22–23`, `soundSuffix = "newsfx\\ftwin"`) with their original unused vanilla `ftwin\ftwin` assets (`trnFile = nullptr`).
  - **Satyr Lord (`MT_LRDSAYTR`):** Kept in Crypt (`dLvl 22–24`) with its original unused vanilla `goatlrd\goatlrd` assets (`trnFile = nullptr`).

### 6.4 Matched Vanilla Monster Replacements & Preserved Hellfire Projectiles
- **File:** `Source/monstdat.cpp` & `Source/monster.cpp`
  - **Hive Casters -> Vanilla Magma Demons (`AiRangedAvoidance` with Hellfire Flare Projectiles):**
    - Replaced `MT_PSYCHORB` (`dLvl 17–18`) and `MT_NECRMORB` (`dLvl 19–20`) in the Hive with vanilla `magma\magma` `"Blood Stone"` (`magma\yellow.trn`) and `"Hell Stone"` (`magma\blue.trn`) models while preserving their `MonsterAIID::Psychorb` (`BlueFlare`) and `MonsterAIID::Necromorb` (`RedFlare`) projectiles via `AiRangedAvoidance`.
  - **Crypt Casters -> Vanilla Mages (`CounselorAi` with Hellfire Flare Projectiles):**
    - Replaced `MT_LICH` (*Lich*) and `MT_ARCHLICH` (*Arch-Lich*) in the Crypt with vanilla `mage\mage` Counselor/Advocate models and palettes (`nullptr`, `mage\cnselbk.trn`) while routing `Lich` and `ArchLich` AIs through `CounselorAi` (`Source/monster.cpp`) with `GetMissileType(monster.ai)` (`OrangeFlare`, `YellowFlare`).
  - **Hellfire Bats -> Vanilla Bats with Firebolt/Fireball Projectiles:**
    - Replaced `MT_FIREBAT` and `MT_HELLBAT` with vanilla `bat\bat` (`Blink` / `Gloom` palettes) while keeping `MonsterAIID::Firebat` (`Firebolt`) and `MonsterAIID::Hellbat` (`Fireball`).
  - **Remaining Nest & Crypt Monster Replacements:**
    - `MT_UNRAV` -> `"Illusion Weaver"` (`sneak\sneak`, `sneakv1.trn`, `MonsterAIID::Sneak`)
    - `MT_HELLBOAR` -> `"Obsidian Lord"` (`rhino\rhino`, `rhinob.trn`, `MonsterAIID::Rhino`)
    - `MT_STINGER` -> `"Fire Drake"` (`snake\snake`, `snakr.trn`, `MonsterAIID::Snake`)
    - `MT_HORKSPWN` -> `"Lava Maw"` (`acid\acid`, `acidr.trn`, `MonsterAIID::Acid`)
    - `MT_VENMTAIL` -> `"Azure Drake"` (`snake\snake`, `snakb.trn`, `MonsterAIID::Snake`)
    - `MT_LASHWORM` -> `"Maelstrom"` (`thin\thin`, `thinv1.trn`, `MonsterAIID::Storm`)
    - `MT_HORKDMN` -> `"Balrog"` (`mega\mega`, `balr.trn`, `MonsterAIID::SkeletonMelee`)
    - `MT_GRAVEDIG` -> `"Bone Gasher"` (`scav\scav`, `scavw.trn`, `MonsterAIID::Scavenger`)
    - `MT_TOMBRAT` -> `"Horror Captain"` (`skelsd\sklsr`, `black.trn`, `MonsterAIID::SkeletonMelee`)
    - `MT_CRYPTDMN` -> `"Lava Lord"` (`magma\magma`, `wierd.trn`, `MonsterAIID::Magma`)
    - `MT_BICLOPS` -> `"Flayed One"` (`fat\fat`, `fatf.trn`, `MonsterAIID::SkeletonMelee`)
    - `MT_FLESTHNG` -> `"Black Death"` (`zombie\zombie`, `yellow.trn`, `MonsterAIID::SkeletonMelee`)
    - `MT_REAPER` -> `"Blood Knight"` (`black\black`, `blkkntbe.trn`, `MonsterAIID::SkeletonMelee`)
  - Updated `AddMonsterType()` in `Source/monster.cpp` to preload `MagmaBall`, `ThinLightning`, and `Acid`/`AcidPuddle` missile graphics whenever `monsterData.ai` is `Magma`, `Storm`, or `Acid`.

---

## Phase 7: OpenAL Soft + EFX Extension, 3D Surround, Binaural HRTF & Wall Occlusion

### 7.1 Static OpenAL Soft 1.23.1 Backend & Embedded HRTF Dataset
- **File:** `Source/CMakeLists.txt`
  - Statically linked `OpenAL::OpenAL` (`libOpenAL32.a` built with `ALSOFT_EMBED_HRTF_DATA=ON`, `ole32`, `uuid`, `winmm`) into `libdevilutionx` so `dx-eternity.exe` remains a single self-contained executable with embedded HRTF filters and full WASAPI/DirectSound/WinMM support.
- **File:** `Source/options.h` & `Source/options.cpp`
  - Raised `DEFAULT_AUDIO_SAMPLE_RATE` from `22050` to `48000` Hz and added in-game Audio Options toggles for **EFX Reverb** (`efxReverb`, default `ON`) and **Binaural HRTF** (`hrtf`, default `ON`).

### 7.2 EFX Environmental Reverb Presets (`ALC_EXT_EFX` / `AL_EFFECT_EAXREVERB`) & Wall Occlusion
- **File:** `Source/engine/sound.h` & `Source/engine/sound.cpp`
  - Initialized an OpenAL Soft context (`ALC_STEREO_MODE_SOFT = ALC_EXTENDED_SOFT`, `ALC_HRTF_SOFT`, `ALC_MAX_AUXILIARY_SENDS = 2`) and EFX Auxiliary Effect Slot (`gEfxSlot`) backed by `AL_EFFECT_EAXREVERB`.
  - Implemented `UpdateSoundEnvironment(dungeon_type)` using `<AL/efx-presets.h>` to dynamically reconfigure the EFX reverb tank per area:
    - **Town (`DTYPE_TOWN`):** `EFX_REVERB_PRESET_OUTDOORS_BACKYARD` (subtle open-air reflections)
    - **Cathedral (`DTYPE_CATHEDRAL`):** `EFX_REVERB_PRESET_CHAPEL` (Gothic stone arches & vaulted ceiling)
    - **Catacombs (`DTYPE_CATACOMBS`):** `EFX_REVERB_PRESET_STONEROOM` (enclosed subterranean tombs)
    - **Caves (`DTYPE_CAVES`):** `EFX_REVERB_PRESET_CAVE` (echoing natural rock caverns)
    - **Hell (`DTYPE_HELL`):** `EFX_REVERB_PRESET_CATHEDRAL` (`flDecayTime = 3.5s`, `flDecayLFRatio = 1.25` — deep abyssal hall)
    - **Hive (`DTYPE_NEST`):** `EFX_REVERB_PRESET_CAVE` (`flDecayTime = 1.5s`, `flDecayHFRatio = 0.35` — dampened organic tunnels)
    - **Crypt (`DTYPE_CRYPT`):** `EFX_REVERB_PRESET_AUDITORIUM` (`flDecayTime = 3.1s`, `flDecayHFRatio = 0.92` — resonant stone crypt)
  - Created EFX Low-Pass Filters (`gFilterOccludedDirect`: `AL_LOWPASS_GAIN = 0.58f, AL_LOWPASS_GAINHF = 0.22f`; `gFilterOccludedSend`: `AL_LOWPASS_GAIN = 0.85f, AL_LOWPASS_GAINHF = 0.45f`) applied whenever a dungeon sound's line-of-sight to the player (`LineClear(IsTileNotSolid, ...)`) is blocked by solid walls.

### 7.3 Shared `ALuint` Buffers & Isometric 3D Spatialization (`AL_SOFT_source_spatialize`)
- **File:** `Source/utils/soundsample.h`, `Source/utils/soundsample.cpp` & `Source/effects.cpp`
  - Decoded WAV/MP3 sound effects into shared `OpenAlBuffer` (`ALuint`) objects so `DuplicateFrom()` shares the decoded PCM buffer in `O(1)` without re-decoding or allocating resamplers.
  - Mapped isometric tile offsets `(deltaX, deltaY)` into listener-relative 3D space with an elevated listener depth offset (`z = isoY * 0.75f - 2.2f`) and enabled `AL_SOURCE_SPATIALIZE_SOFT = AL_TRUE` so both mono and stereo SFX spatialize smoothly across 5.1/7.1 surround speakers and binaural HRTF headphones without harsh 180° front/back flips in melee range.

### 7.4 Direct `dr_wav` / `dr_mp3` PCM Decoding (SFX Pitch Fix), Boosted EFX Reverb & Live In-Game Toggles
- **Root Cause (Lowered SFX Pitch):** When `Aulib::init()` is initialized with 2 output channels (stereo), `Aulib::DecoderDrwav::doDecoding()` automatically duplicates every mono sample `[s0, s0, s1, s1, ...]` so its output buffer matches `Aulib::channelCount() == 2`, while `decoder->getChannels()` still returns `1` from the WAV header. Uploading that stereo-duplicated buffer to `alBufferData` with `AL_FORMAT_MONO16` at `22050 Hz` caused OpenAL to play twice as many mono samples at 22050 Hz, halving playback speed and lowering pitch by one full octave.
- **File:** `Source/utils/soundsample.cpp` — `DecodeMemoryToOpenAlBuffer()` & `AcquireSource()`
  - Replaced `Aulib::Decoder` in `DecodeMemoryToOpenAlBuffer()` with direct `drwav_init_memory` + `drwav_read_pcm_frames_s16` and `drmp3_init_memory` + `drmp3_read_pcm_frames_s16` decoding so mono and stereo WAV/MP3 assets are uploaded at their exact native channel count and sample rate without stereo sample duplication, and enforced `alSourcef(source, AL_PITCH, 1.0F)`.
- **File:** `Source/engine/sound.h` & `Source/engine/sound.cpp`
  - Added `ApplyOpenAlReverbOption()` (`alAuxiliaryEffectSlotf(gEfxSlot, AL_EFFECTSLOT_GAIN, enabled ? 1.0F : 0.0F)`) and `ApplyOpenAlHrtfOption()` (`alcResetDeviceSOFT(gAlDevice, attrs)` with `ALC_HRTF_SOFT`) so toggling Reverb or HRTF in the menus takes effect immediately without restarting audio or dropping active buffers.
  - Set `alDopplerFactor(0.0F)` in `InitOpenAl()` and increased `AL_EAXREVERB_GAIN` (`0.55F` in Town, `0.90F–1.00F` across Cathedral/Catacombs/Caves/Hell/Hive/Crypt) and `flLateReverbGain` (`1.55F–1.65F` scale) in `LoadEaxReverbPreset()` so environmental reverb is enabled by default and distinctly audible.
- **File:** `Source/options.cpp`, `Source/gamemenu.cpp` & `Source/gmenu.cpp`
  - Placed **Reverb** (`efxReverb`, default `Normal`) and **HRTF** (`hrtf`, default `ON`) at the top of `Settings -> Audio` (`AudioOptions::GetEntries()`) with live callbacks (`ApplyOpenAlReverbOption`, `ApplyOpenAlHrtfOption`) and hid manual speaker channel configuration (`OptionEntryFlags::Invisible`) since OpenAL Soft autodetects Stereo vs. Surround output hardware.
  - Added **Reverb: Normal/High/OFF** (`GamemenuReverb`) and **HRTF: ON/OFF** (`GamemenuHrtf`) directly to the in-game `ESC -> Options` menu (`sgOptionsMenu[]`) with compact 7-row vertical spacing in `Source/gmenu.cpp` so all items fit cleanly above the HUD panel and respond to mouse clicks, `Enter`, and `Left`/`Right` arrow keys.

### 7.5 Full-Fidelity HRTF Mode (2D/Self Stereo Direct Bypass, `BSinc24` Resampler & HRIR Comb-Notch De-Coloration), Wide Melee 3D Azimuth & 3-State Reverb (`Normal` / `High` / `OFF`)
- **Root Cause (Lo-Fi / Muffled UI & Game Audio When HRTF Was Enabled):**
  1. In OpenAL Soft (`alc/alu.cpp`), mono buffers (`AL_FORMAT_MONO16`) hardcode `DirectChannels = DirectMode::Off; /* Mono buffers are never played direct. */`. Because Diablo 1 WAV assets are 1-channel mono, OpenAL Soft ignored `AL_DIRECT_CHANNELS_SOFT = AL_TRUE` on mono buffers and forced every 2D UI click (`titlemov.wav`, `titlslct.wav`), inventory sound, town speech, and player-centered footstep/weapon swing (`deltaX == 0 && deltaY == 0`) through `RenderMode::Hrtf` at `0°` azimuth.
  2. OpenAL Soft's embedded `Default HRTF.mhr` (`MinPHR03` KEMAR 64-tap minimum-phase HRIR dataset) has a strong pinna reflection tap at `n = 4` that creates a `-21.4 dB` comb-filter notch at `8 kHz` and a `-7.1 dB` dip at `1 kHz` for frontal (`0°`) sources, making mono sounds filtered through it sound bandwidth-limited (`~22050 Hz` lo-fi).
  3. OpenAL Soft defaulted to a 4-point `Cubic` B-spline resampler (`Resampler::Cubic`), which rolls off high frequencies when upsampling `22050 Hz` SFX to `48000 Hz`.
- **File:** `Source/utils/soundsample.cpp` & Static `libOpenAL32.a` (`core/voice.cpp`, `core/hrtf.cpp`)
  - **Dual Mono/Stereo OpenAL Buffers & True HRTF Bypass for 2D/Self Sounds:** `DecodeMemoryToOpenAlBuffer()` now creates both `monoId` (`AL_FORMAT_MONO16` for 3D positional world sounds) and `stereoId` (`AL_FORMAT_STEREO16` for 2D UI/menu sounds and `0,0`-offset player footsteps, weapon swings, bow shots, and spell casts). Non-positional (`!isPositional`) and player-centered (`deltaX == 0 && deltaY == 0`) sounds bind `stereoId` with `AL_DIRECT_CHANNELS_SOFT = AL_REMIX_UNMATCHED_SOFT` and `AL_SOURCE_SPATIALIZE_SOFT = AL_FALSE`, bypassing `RenderMode::Hrtf` completely for 100% uncolored studio fidelity while still feeding dungeon EFX reverb sends.
  - **`BSinc24` Band-Limited Sinc Resampling & HRIR De-Coloration:** Switched OpenAL Soft's default and per-source resampler (`AL_SOURCE_RESAMPLER_SOFT`) from `Cubic` to `BSinc24` (24-point band-limited sinc interpolation) and damped late pinna comb-notch reflections (`t >= 2`, `0.72^(t-1)` with RMS passband preservation) in `CreateHrtfStore()`, flattening near-ear frequency response to `±1.1 dB` across `500 Hz – 10 kHz` while preserving 100% of interaural time delay (`ITD`) and far-ear head-shadow cues.
- **File:** `Source/monster.cpp` — 3D Spatialization, EFX Reverb & Wall Occlusion for All Monster Sounds
  - **Root Cause (Mono/Dry Monster Sounds):** `PlayEffect(Monster &monster, MonsterSound mode)` in `Source/monster.cpp` previously called `snd_play_snd(snd, lVolume, lPan)` (which forwarded `deltaX = 0, deltaY = 0, isPositional = false, applyReverb = false, isOccluded = false`), causing all monster attack, hit, pain, death, and special sounds to bypass 3D spatialization (playing centered mono) and bypass EFX dungeon reverb.
  - Updated `PlayEffect()`, `DiabloDeath()`, and unique boss activation taunts (`MT_CLEAVER`, `MT_NAKRUL`, `MT_DEFILER`) in `Source/monster.cpp` to pass monster tile offsets `(delta.deltaX, delta.deltaY)`, `isPositional = true`, `applyReverb = (currlevel != 0)`, and wall occlusion (`isOccluded`) to `snd_play_snd_ex()` / `PlaySfxLoc()`, and allowed overlapping monster `Attack`/`Hit`/`Death` sounds to play concurrently via the `DuplicateSound` pool.
- **File:** `Source/utils/soundsample.cpp`, `Source/engine/sound.cpp` & Static `libOpenAL32.a` (`core/hrtf.cpp`) — Wide Horizontal-Plane 3D Azimuth & Deepened HRTF Head Shadow
  - Mapped lateral isometric tile offsets (`horiz = deltaX - deltaY != 0`) onto the horizontal ear plane (`posY = 0.0F`) with a fixed small front depth offset (`posZ = -0.28F`, `posX = copysign(1.20F + |horiz| * 0.90F, horiz)`). This prevents vertical screen distance (`deltaX + deltaY`) from pulling off-center barrels or monsters toward `0°`/`180°` mono center and places melee monsters (`|horiz| = 1..2`) at `±82°–85°` azimuth and distant monsters/barrels at `±85°–88°` azimuth.
  - Deepened far-ear head-shadow attenuation (`1.0 - 0.52 * lat`, `-6.4 dB` extra opposite-ear rejection, bringing total lateral ILD to `~21.5 dB`) in `CreateHrtfStore()` for off-axis HRIRs (`delays[0] != delays[1]`) while keeping `0°` uncolored.
  - Added `gFilterStereoSend` (`AL_LOWPASS_GAIN = 0.50F`) on 2-channel `stereoId` player-centered sounds so their EFX reverb send matches 1-channel `monoId` 3D monster/world sounds without a `+6 dB` dual-channel sum boost.
- **File:** `Source/options.h`, `Source/options.cpp`, `Source/gamemenu.cpp` & `Source/engine/sound.cpp` — Distinct 3-State Reverb (`Normal` / `High` / `OFF`)
  - Replaced the boolean Reverb toggle with `enum class ReverbLevel : uint8_t { Off = 0, Normal = 1, High = 2 }`.
  - Updated `LoadEaxReverbPreset()` and `ApplyOpenAlReverbOption()` so switching between **`Normal`** (`AL_EFFECTSLOT_GAIN = 0.45F`, `effectGain * 0.68F`, `lateReverbGain * 0.50F`, `reflectionsGain * 0.75F`, `decayTime * 0.75F`), **`High`** (`AL_EFFECTSLOT_GAIN = 1.00F`, `effectGain * 1.00F`, `lateReverbGain * 1.25F`, `reflectionsGain * 1.50F`, `decayTime * 1.20F`), and **`OFF`** (`0.00F`) immediately reloads the EAX reverb effect into `gEfxSlot` with a clear perceptual difference.

### 7.6 Restored Randomized Footstep Sounds (`walk1.wav` – `walk4.wav`) & Town Footsteps
- **File:** `Source/effects.h` & `Source/effects.cpp`
  - Restored `PS_WALK2`, `PS_WALK3`, and `PS_WALK4` in `enum _sfx_id` and `sgSFX[]` (`"sfx\\misc\\walk1.wav"` through `"sfx\\misc\\walk4.wav"`, all preloaded via `sfx_MISC`).
  - Enabled `case PS_WALK1:` in `RndSFX(_sfx_id psfx)` to randomize across all 4 footstep variations (`PS_WALK1..PS_WALK4`) with consecutive-repeat prevention (`lastWalkRand`) so two identical footstep samples never play back-to-back.
- **File:** `Source/player.cpp` — `DoWalk()`
  - Removed the `(leveltype != DTYPE_TOWN || sgGameInitInfo.bRunInTown == 0)` restriction in `DoWalk()` so player footsteps play in Town (`currentFrame == 4` once per 4-tick tile when `Run in Town` is active, matching the 4-tick footstep cadence of normal walking).

---

## Phase 8: Tristram Weather & Environmental Particle System (Rain, Puddle Splashes, Distant Lightning & Ambient Loops)

### 8.1 Dynamic Light Rain & Heavy Rain Simulation (`Source/engine/render/weather.h`, `Source/engine/render/weather.cpp`)
- **Session-Persisted Rain Intensity + 1-Minute Dungeon Rule (`OnWeatherNewGame`, `OnWeatherLevelLoad`):**
  - Tristram (`leveltype == DTYPE_TOWN && !setlevel`) always has active rain in either `Light` or `Heavy` mode, rolled randomly upon starting/loading a game (`OnWeatherNewGame()` / `OnWeatherLevelLoad(true, ...)`).
  - The rolled rain intensity **persists indefinitely while in Tristram** and can only re-roll when the player returns to Tristram after spending **at least 1 minute (`60,000 ms`) in a dungeon** (`gAccumulatedDungeonMs >= MinDungeonTimeForWeatherChangeMs`).
- **Camera-Compensated Multi-Layer Raindrop Streaks (+35% Intensity, `DrawWeatherAir`):**
  - Rendered in `DrawGame()` (`Source/engine/render/scrollrt.cpp`) after `DrawTileContent()` and before `Zoom()` so raindrops scale naturally with viewport zoom and never draw over the bottom HUD or open UI panels.
  - Tracks camera world-space pixel movement (`startTilePosition.worldToScreen() - offset`) so walking or running through Tristram moves the viewport naturally through the falling rain sheet rather than dragging raindrops with the screen.
  - Simulates 3 depth layers (`far/fine`, `mid`, `near/bright`) of slanted raindrop streaks (`132` active drops in Light Rain, `360` in Heavy Rain — increased by +35%) using 75%, 50%, and 25% hardware-free 8-bit palette blending (`paletteTransparencyLookup`).

### 8.2 Localized 5-Stage Isometric Puddle Splash Animations (`DrawWeatherGroundTile`)
- **Root Cause & Fix for Missing Ground Splashes (`Source/engine/render/scrollrt.cpp`):**
  - In DevilutionX's renderer, `DrawTileContent()` -> `DrawDungeon()` -> `DrawCell()` re-renders the ground microtiles (`mt[0]` and `mt[1]`) for every `TransparentSquare` tile in Town, which overwrote anything drawn between `DrawFloor()` and `DrawTileContent()`.
  - Moved ground splash rendering into `DrawDungeon()` via `DrawWeatherGroundTile(out, tilePosition, targetBufferPosition)` **immediately after `DrawCell()`**. This draws splashes directly onto each walkable outdoor tile's freshly rendered floor pixels while still allowing objects, items, players, NPCs, tree canopies (`dSpecial`), and all closer isometric rows (`i + 1..rows`) to naturally occlude splashes behind them.
- **Deterministic Per-Tile Spawning & Compact 5-Stage Semi-Transparent Splash Animation:**
  - Uses a fast spatial+temporal hash per walkable outdoor Town tile (`!TileHasAny(dPiece[x][y], TileProperties::Solid)`) with tuned spawn density (`136‰` per slot in Light Rain, `299‰` per slot in Heavy Rain — reduced by 35% from initial pass).
  - Reduced splash radii by 50% (`rx = 1..6`, `ry = 0..3`) and added +50% transparency (`PlotBlended50`, `PlotBlended25`, and `PlotBlended12` using `PAL16_GRAY + 2..6`, `PAL16_BLUE + 3..6`) across 5 stages (`260 ms` lifecycle):
    - **Stage 0:** Compact 50%/25%-blended impact water bead + 2 tiny crown droplets.
    - **Stage 1:** Compact 2:1 isometric inner ring (`rx = 2, ry = 1`).
    - **Stage 2:** Expanding 2:1 isometric ripple (`rx = 3..4, ry = 1..2`).
    - **Stage 3:** Soft 25%/12.5%-blended isometric ripple (`rx = 4..5, ry = 2`).
    - **Stage 4:** Dissipating outer ring (`rx = 5..6, ry = 2..3`) at 25%/12.5% opacity.

### 8.3 Precipitation / Steam Fog (`DrawWeatherGroundTile`, `DrawWeatherAir`)
- **Non-Overlapping World-Anchored Drifting Steam Fog Banks (`SteamFogBank`):**
  - Reduced bank count by 30% (`9` in Light Rain, `14` in Heavy Rain), increased size by +20% (`rx = 46..79, ry = 18..30`), and increased transparency by +35% (`Blend12` 12.5% opaque inner core + `Blend6` 6.25% opaque outer feather).
  - Arranged on a strict `5x3` staggered world-space lattice (`FogWorldWrapW = 1500, FogWorldWrapH = 900`, `300x300 px` per cell) with uniform drift velocity plus a screen-space bounding-box guard (`DrawnFogRect`) so Steam Fog Banks **never overlap**.
- **Curling Rising Ground-Evaporation Wisps (`DrawGroundSteamWisp`):**
  - Reduced wisp count by an additional 40% (`19%` outdoor tile density), reduced size by 35% (`riseY = 18 px`, `rx = 1..4 px`, `ry = 1..2 px`), and increased transparency by 50% (`Blend12` 12.5% core + `Blend6` 6.25% halo) across 5 ascending S-curling vapor puffs, rendered inside `DrawCell()` after `mt[0..1]` and before `mt[2..15]` so fences and archways properly occlude them.

### 8.4 Full-Screen Distant Lightning Flashes (30–60s Interval) & Delayed Thunder (`Source/engine/palette.h`, `Source/engine/palette.cpp`, `Source/engine/render/weather.cpp`)
- **High-Visibility `system_palette` Flash Envelope (30–60s Random Interval):**
  - Exposed `IsPaletteFadedIn()` in `Source/engine/palette.h` / `Source/engine/palette.cpp`.
  - Strikes randomly every **30 to 60 seconds** (`30000 + GenerateRnd(30001)` ms), triggering a full-screen cool-white / electric slate-blue illumination flash across `system_palette[1..254]` via `palette_update(0, 256)` (`60 ms` peak flash followed by a `55 ms` afterglow decay) with up to `1.65x` luminance boost + additive cool RGB shift (`+98 R, +119 G, +166 B` in Heavy Rain) and a 50% (Heavy) / 25% (Light) chance of a secondary double-strike flash `125–170 ms` later.
  - Automatically restores the exact gamma-corrected `logical_palette` after the flash or immediately upon leaving Town without mutating `logical_palette` or `orig_palette`.
- **Speed-of-Sound Delayed Thunder Audio (`LS_THUNDERA` & `LS_THUNDERB`, +35% Delay):**
  - Added `LS_THUNDERA` (`"sfx\\misc\\thundera.wav"`) and `LS_THUNDERB` (`"sfx\\misc\\thunderb.wav"`) to `_sfx_id` and `sgSFX[]`, played via `PlayWeatherSFX()` (`applyReverb = false` since reverb is baked into the WAV assets).
  - Primary lightning strikes schedule an acoustic speed-of-sound delayed thunder clap (+35% propagation delay): `thundera.wav` (`1553–2498 ms` delay, distant rumble) during Light Rain and `thunderb.wav` (`608–1215 ms` delay, closer thunderclap) during Heavy Rain.

### 8.5 Ambient Rain Audio Loops (`sfx\misc\RainLightLoop.wav` & `sfx\misc\RainHeavyLoop.wav`)
- **File:** `Source/effects.h`, `Source/effects.cpp`, `Source/diablo.cpp`, `Source/utils/soundsample.h`, `Source/utils/soundsample.cpp`
  - Added `LS_RAINLIGHT` (`"sfx\\misc\\RainLightLoop.wav"`) and `LS_RAINHEAVY` (`"sfx\\misc\\RainHeavyLoop.wav"`) to `_sfx_id` and `sgSFX[]`, with `AL_LOOPING = AL_TRUE` support in `SoundSample::PlayWith3D()` (`applyReverb = false`).
  - Hooked `ResetWeatherState()` and `sound_stop()` into `FreeGame()` and `StartGame()` in `Source/diablo.cpp`, `OnWeatherLevelLoad()` into `LoadGameLevel()`, and non-game stop guards into `sound_update()` and `effects_play_sound()` in `Source/effects.cpp` so rain and thunder sounds stop immediately when quitting to the Main Menu or entering a dungeon.

### 8.6 Custom Unique Weapon Unused `objcurs.cel` Graphics (`ITEM_MAGSWORD`, `ITEM_LONGSTAFF`, `ITEM_STLSHRTBOW`), `Kadi's Fury` Indestructibility & `Headphone Mode (HRTF)` Label
- **File:** `Source/itemdat.h`, `Source/itemdat.cpp`, `Source/items.cpp`, `Source/loadsave.cpp`, `Source/pack.cpp`
  - Defined the unused `objcurs.cel` cursor graphics from the Hellfire source code `ITEM_IDS` enum (`ITEM_MAGSWORD = 73`, `ITEM_STLSHRTBOW = 120`, `ITEM_LONGSTAFF = 125`) in `enum item_cursor_graphic` and expanded `UniqueItem::powers` to 8 slots.
  - **Kadi's Fury (`UITEM_KADISFURY`):** Added `{ IPL_INDESTRUCTIBLE }` (`Indestructible`) and `{ IPL_INVCURS, ITEM_MAGSWORD }` (`objcurs.cel` magic sword graphic `73`), and updated `LoadItemData()` / `UnPackItem()` so existing saved copies automatically upgrade to `DUR_INDESTRUCTIBLE` and `ITEM_MAGSWORD`.
  - **Apostate's Crook (`UITEM_APOSTATESCROOK`):** Assigned `{ IPL_INVCURS, ITEM_LONGSTAFF }` (`objcurs.cel` unused staff graphic `125`) on creation and save load.
  - **Loxley's Bend (`UITEM_LOXLEYSBEND`):** Assigned `{ IPL_INVCURS, ITEM_STLSHRTBOW }` (`objcurs.cel` steel short bow graphic `120`) on creation and save load.
- **File:** `Source/options.cpp` & `Source/gamemenu.cpp`
  - Renamed the HRTF option label to **`Headphone Mode (HRTF)`** in both `Settings -> Audio` and the in-game `ESC -> Options` menu (`Headphone Mode (HRTF): ON/OFF`).

---

## Phase 9: Highest-Priority `dx-eternity.mpq` Custom Asset Override & Hardened MPQ/SDL2 Loading

### 9.1 Pre-Dynamic-Lighting Rollback Verification
- Verified that all experimental dynamic lighting files and hooks are cleanly rolled back (deferred to the final phase of development):
  - `Source/utils/display.cpp` is unmodified (no forced `SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl")` in `ReinitializeRenderer()`).
  - `SoundSample::SetChunkStream()` in `Source/utils/soundsample.cpp` retains the `16 MB` in-memory OpenAL decode threshold (`ref.size() <= 16 * 1024 * 1024`), ensuring large music tracks like `dtowne.wav` (`~31 MB`) stream asynchronously via `Aulib::Stream` rather than synchronously decompressing into RAM on Town entry.
  - Moved prebuilt third-party Windows binary folders (`libsodium-win32`, `libsodium-win64`) and non-repository files into `/misc/`.

### 9.2 Highest-Priority `dx-eternity.mpq` Custom Asset Archive (`Source/init.h`, `Source/init.cpp`, `Source/engine/assets.cpp`)
- **File:** `Source/init.h` & `Source/init.cpp`
  - Declared and defined `dx_eternity_mpq` (and `dx_eternity_data_path` for `UNPACKED_MPQS`), cleaned up in `init_cleanup()`.
  - Loaded `dx-eternity.mpq` (with uppercase fallback `DX-ETERNITY.MPQ`) in `LoadCoreArchives()` and `LoadGameArchives()` so custom UI, font, audio, monster, item, and level assets are available from initial startup through gameplay.
- **File:** `Source/engine/assets.cpp`
  - Placed `at(dx_eternity_mpq)` (and `at(dx_eternity_data_path)`) at the front of the lookup chain in `FindMpqFile()` / `FindUnpackedMpqFile()`, giving `dx-eternity.mpq` highest priority over `font_mpq`, `lang_mpq`, `devilutionx_mpq`, `diabdat_mpq`, and `hellfire_mpq`.

### 9.3 Fix Empty MPQ Underflow, Zero-Byte Entry Fallback, Uninitialized Buffer Reads & `SDL_DYNAMIC_API` Conflict
- **File:** `Source/mpq/mpq_reader.hpp` & `Source/mpq/mpq_reader.cpp`
  - Added `MpqArchive::GetNumFiles() const` and `MpqArchive::GetUnpackedFileSize(uint32_t fileNumber)`.
  - Guarded `GetFileNumber()`, `GetUnpackedFileSize()`, `ReadFile()`, `ReadBlock()`, `GetNumBlocks()`, `OpenBlockOffsetTable()`, `CloseBlockOffsetTable()`, `GetBlockSize()`, and `HasFile()` against empty MPQ archives (`files == 0`) and out-of-range `fileNumber >= files`. This prevents `libmpq`'s `CHECK_FILE_NUM` macro (`file_number > mpq_archive->files - 1`) from underflowing unsigned `0U - 1U` to `4294967295` when `dx-eternity.mpq` is empty.
  - Updated `MpqArchive::GetFileNumber()` and `FindMpqFile()` (`Source/engine/assets.cpp`) to return `false` if a matched file entry has an uncompressed size of `0` (`GetUnpackedFileSize(fileNumber) == 0`), cleanly falling back to `devilutionx_mpq` and `diabdat_mpq`.
- **File:** `Source/engine/load_file.hpp` & `Source/engine/assets.hpp`
  - Updated `LoadFileInMem()` overloads in `Source/engine/load_file.hpp` to reject `size == 0` and verify `handle.read()` succeeds before returning `buf`, ensuring failed reads never pass uninitialized memory to the CLX font or sprite renderers.
  - Updated the 3-argument `LoadFileInMem(path, data, count)` and `LoadOptionalFileInMem(path, data, count)` templates to query `OpenAsset(path, size)` and clamp `bytesToRead = std::min(size, count * sizeof(T))`, allowing variable-length files read into fixed-capacity upper-bound buffers (such as `.sol` tile property files like `nlevels\l5data\l5.sol` at 410 bytes or `town.sol` at 1287 bytes read into `2048`-byte `SOLData`) to read their exact unpacked byte count without triggering false EOF read errors.
  - Hardened `AssetRef::size()` and `AssetHandle::read()` / `seek()` in `Source/engine/assets.hpp` against null handles and negative `SDL_RWsize` returns.
- **Windows 64-Bit Build (`misc/dx-eternity.exe`):**
  - Compiled `misc/dx-eternity.exe` (`6.7 MB`, Release) dynamically linking `SDL2.dll` (matching the existing `SDL2.dll` in the game directory with zero static/dynamic `SDL_RWops` ABI conflict) while statically linking all other libraries (`libOpenAL32.a` with `BSinc24` + HRIR de-coloration, `libsodium.a`, `libwinpthread.a`, `libstdc++.a`, `libgcc.a`, `SDL2_image`, `SDL_audiolib`, `fmt`, `zlib`, `bzip2`, and `libmpq`) directly into the executable.
