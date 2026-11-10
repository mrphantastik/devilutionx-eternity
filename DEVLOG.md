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
- **File:** `Source/engine/load_file.hpp`, `Source/engine/assets.hpp` & `Source/engine/assets.cpp`
  - Updated `LoadFileInMem()` overloads in `Source/engine/load_file.hpp` to reject `size == 0` and verify `handle.read()` succeeds before returning `buf`, ensuring failed reads never pass uninitialized memory to the CLX font or sprite renderers.
  - Updated the 3-argument `LoadFileInMem(path, data, count)` and `LoadOptionalFileInMem(path, data, count)` templates to query `OpenAsset(path, size)` and clamp `bytesToRead = std::min(size, count * sizeof(T))`, with `LoadFileInMem(path, data, count)` reading `bytesToRead` after `ValidateHandle(path, handle)` so variable-length files read into fixed-capacity upper-bound buffers (such as `.sol` tile property files like `nlevels\l5data\l5.sol` at 410 bytes or `town.sol` at 1287 bytes read into `2048`-byte `SOLData`) never trigger false EOF read errors.
  - Added `SDL_ClearError()` in `FindAsset()` (`Source/engine/assets.cpp`) when `FindMpqFile()` succeeds so a missed optional `PrefPath()` file override probe does not leave a stale `"Couldn't open ...\AppData\Roaming\..."` error string in `SDL_GetError()`.
  - Hardened `AssetRef::size()` and `AssetHandle::read()` / `seek()` in `Source/engine/assets.hpp` against null handles and negative `SDL_RWsize` returns.
- **Windows 64-Bit Build (`misc/dx-eternity.exe`):**
  - Compiled `misc/dx-eternity.exe` (`6.7 MB`, Release) dynamically linking `SDL2.dll` (matching the existing `SDL2.dll` in the game directory with zero static/dynamic `SDL_RWops` ABI conflict) while statically linking all other libraries (`libOpenAL32.a` with `BSinc24` + HRIR de-coloration, `libsodium.a`, `libwinpthread.a`, `libstdc++.a`, `libgcc.a`, `SDL2_image`, `SDL_audiolib`, `fmt`, `zlib`, `bzip2`, and `libmpq`) directly into the executable.

---

## Phase 10: Level Palette Randomizer (`DevilutionXRandomizer` Integration in Graphics & In-Game Options)

### 10.1 Graphics Option Default ON (`Source/options.h`, `Source/options.cpp`, `Source/gamemenu.cpp`)
- **File:** `Source/options.h` & `Source/options.cpp`
  - Added `OptionEntryBoolean randomizePalette` (`"Randomize Level Palettes"`, label `"Randomize Level Palettes"`, description `"Randomizes dungeon level color palettes."`, default `true`) at the end of `GraphicsOptions` (`Settings -> Graphics`, persisted in `diablo.ini` under `[Graphics] Randomize Level Palettes`).
  - Registered `OptionRandomizePaletteChanged()` value-changed callback (`if (!gbRunGame || setlevel || leveltype == DTYPE_TOWN) return; LoadRndLvlPal(leveltype); ApplyGamma(logical_palette, orig_palette, 256); SetFadeLevel(256);`) so toggling `Randomize Level Palettes` on/off switches between the floor's vanilla palette and its deterministic randomized palette without rerolling on the spot.
  - Kept the in-game `ESC -> Options` menu (`Source/gamemenu.cpp`) clean at 7 items (`Music`, `Sound`, `Reverb`, `Headphone Mode (HRTF)`, `Gamma`, `Speed`, `Previous Menu`) while `Randomize Level Palettes` remains accessible in `Settings -> Graphics`.

### 10.2 Loose `BasePath()` Lookup, Vanilla Town Palette, `IsMultiHuePalette` Filter & Procedural Split-Complementary Fallback (`Source/engine/assets.cpp`, `Source/engine/palette.h`, `Source/engine/palette.cpp`)
- **File:** `Source/engine/assets.cpp`
  - Added a `paths::BasePath() + relativePath` check inside `FindAsset()` before `FindMpqFile()` so loose folders next to the executable (e.g., `<GameDir>\palette\...`) load directly without requiring an MPQ repack or conflicting with MPQ Editor file locks.
- **File:** `Source/engine/palette.h` & `Source/engine/palette.cpp`
  - **Vanilla Town Palette Preservation:** `DTYPE_TOWN` always loads `"levels\\towndata\\town.pal"` and returns immediately, ensuring Tristram never changes palette when returning from dungeons (`SaveLevel` advancing `glSeedTbl[0]`) or starting a New Game.
  - **100% Deterministic Gameplay RNG Preservation:** Preserved the exact single `GenerateRnd(4) + 1` call for `l != DTYPE_TOWN` in `LoadRndLvlPal(dungeon_type l)` so dungeon layout seeds, monster/item spawns, and multiplayer synchronization are completely unaffected.
  - **Deterministic Per-Floor Murmur3 Seed Hash (`GetLevelPaletteHash`):** Derives a deterministic hash strictly from `glSeedTbl[currlevel]`, `currlevel`, and `dungeon_type l` so each dungeon floor maintains its locked randomized palette across stair transitions and menu toggles.
  - **`IsMultiHuePalette` Monochrome Preset Filter:** Inspects candidate `DevilutionXRandomizer` `.pal` presets (`palette\{folder}\{prefix}_{idx}.pal` across `l1_1..66.pal`, `l2_1..74.pal`, `l3_1..58.pal`, `l4_1..46.pal`, `l5_1..74.pal`, `l6_1..79.pal`) and rejects flat 1-color monochrome presets (requiring `>= 16` saturated level colors across `>= 3` distinct 30° hue buckets and `<= 68%` concentration within any single 40° hue window), stepping forward deterministically until a multi-hue preset is found.
  - **Split-Complementary Procedural HSV Fallback (`ApplyProceduralPaletteShift`):** If external `palette\*.pal` files are not installed, loads the vanilla level palette and applies a split-complementary hue shift (`hueShiftPrimary` and `hueShiftPrimary + 115°`) only to already-colored level entries (`1..127` with `s >= 0.10`), leaving neutral greyscale stone untouched so levels never become a flat 1-color wash.

### 10.3 3D Multi-Plane Depth Rain Upgrade — Wall/Roof-Safe Edition (`Source/engine/render/weather.h`, `Source/engine/render/weather.cpp`, `Source/engine/render/scrollrt.cpp`)
- **File:** `Source/engine/render/weather.h`, `Source/engine/render/scrollrt.cpp`, `Source/engine/render/weather.cpp`
  - **Multi-Plane Camera Parallax, Perspective Slant Fan & Tuned Per-Layer Transparency (`RainDrop`):**
    - Added fine-grained binary LUT blend helpers `Blend37(fg, bg)` (37.5% fg / 62.5% bg) and `Blend19(fg, bg)` (18.75% fg / 81.25% bg) alongside `Blend50` and `Blend25`.
    - **Layer 0 (Distant Background Rain, 42% of drops, +5% transparency):** `0.58x` camera parallax (moves slower than the ground), `0.55x..0.72x` fall speed, more vertical `-0.12` (`-0.17` Heavy) slant angle, `9..13 px` 1px streaks (`Blend37` top-33% head -> `Blend25` tail using `PAL16_BLUE/GRAY + 3..4`).
    - **Layer 1 (Midground World-Plane Air Rain, 42% of drops, +15% transparency):** `1.00x` camera parallax (locked to the world plane), `0.95x..1.18x` fall speed, `-0.21` (`-0.29` Heavy) slant angle, `17..24 px` crisp 1px streaks (`Blend50` head -> `Blend37` body -> `Blend25` tail using `PAL16_BLUE/GRAY + 2..3`).
    - **Layer 2 (Near-Camera Foreground "Lens" Rain, 16% of drops, +35% transparency):** `1.65x` camera parallax (whips past the camera when walking), `1.65x..2.15x` fall speed, steeper `-0.31` (`-0.41` Heavy) perspective slant angle, `34..48 px` streaks with a translucent **2px–3px Depth-of-Field soft cross-section** (`Blend50` -> `Blend37` -> `Blend25` -> `Blend19` core using `PAL16_GRAY/BLUE + 2..3`, paired with `Blend25` right and `Blend19` left lateral DoF blur wings at shade `+2` so near-camera drops remain soft and atmospheric without glaring).
    - Each layer's horizontal wind velocity (`dropWindVel = dropFallVel * layerSlant`) is locked to its drawn slant angle so every drop shoots strictly along its perspective streak vector.
  - **True Scene Occlusion Behind Buildings, Roofs, Rocks & Trees Without `DrawCell()` Ground Overwrite (`DrawWeatherBackground` & `DrawWeatherGroundTile`):**
    - `DrawCell()` in `scrollrt.cpp` redraws `LeftFoliage` / `RightFoliage` (`TransparentSquare` upper triangles) on every walkable Town tile during `DrawTileContent()`, which previously erased any background rain drawn before `DrawTileContent()`.
    - Fixed by rasterizing Layer 0 into a coarse-grid-accelerated screen-space buffer (`gBgRainMask` + `gBgRainBlockGrid`) in `DrawWeatherBackground()`, and compositing it onto each walkable ground tile's 64x32 isometric diamond inside `DrawWeatherGroundTile()` right *after* `mt[0..1]` (`LeftFoliage`/`RightFoliage`) finish and *before* `mt[2..15]` (roofs, walls, arches), solid tiles, trees (`dSpecial`), objects, NPCs, and players are rendered on top.

### 10.4 Floating Steam Fog Wisps Upgrade: Vertical Distribution, Rounder Cloud Shapes & Variable-Speed Collision Avoidance (`Source/engine/render/weather.cpp`)
- **Full Vertical Distribution & Meandering (`InitSteamFogBank` & `UpdateWeatherState`):**
  - Replaced the rigid 3-row (`y = 115, 415, 715`) lattice that caused fog wisps to travel along 2 visible horizontal lanes on screen with a 14-altitude low-discrepancy distribution across `1560 x 1040` world pixels (`74 px` vertical bands).
  - Added per-bank diagonal vertical drift (`driftVelY = -9.5 .. +6.9 px/s`) and sinusoidal vertical wave undulation (`swayAmpY = 10.0 .. 23.9 px/s`, `swayFreq = 0.26 .. 0.57`) so wisps glide across all vertical heights with organic upward and downward motion.
- **Rounder Cloud Morphologies (`SteamFogBank` & `DrawSingleSteamFogBank`):**
  - Replaced the flat horizontal ribbon puff layout (`rx = 46..79, ry = 18..30`) with 2D 4-lobe cloud clusters (left, top, bottom, and right puffs) supporting rounder cumulus-like clouds (`rx = 38..54, ry = 29..42`, aspect `~1.15:1` to `1.38:1`), tumbled ovals (`rx = 46..64, ry = 26..37`), and full-bodied wisps, blended with a smooth metaball bridge (`secondMinD2`) between adjacent lobes.
- **Per-Bank Speed Variety (`0.64x .. 1.42x`) with Toroidal Collision-Avoidance Solver:**
  - Each fog bank now has its own wind speed multiplier (`speedMultX = 0.64F .. 1.42F`).
  - Added a 2-pass toroidal elliptical collision-avoidance solver in `UpdateWeatherState()` (plus an elliptical screen-space guard in `DrawSingleSteamFogBank()`) that detects when a faster bank approaches a slower bank and smoothly steers them apart vertically and horizontally (`+26 px` X / `+24 px` Y comfort buffer) so wisps never collide or stack.

---

## Phase 11: 3D Isometric Volumetric Ground Fog Overhaul & Interactive Entity Dispersion

### 11.1 Tightened 8-Direction Corner Wall Fadeout, Correct `DrawCell` Isometric Z-Order & True 3D Slope Shading + Vertical Z-Loft (`Source/engine/render/ground_fog.h`, `Source/engine/render/ground_fog.cpp`, `Source/engine/render/scrollrt.cpp`)
- **Tightened 8-Direction Corner Vertex Wall Weights (`gFogCornerWeight`, Margin Cut in Half):**
  - Precomputes a vertex wall weight `gFogCornerWeight[vx][vy]` (`0` or `256`) at every 4-tile grid intersection `(vx, vy)` in `ResetGroundFogLevel()`. If any of the 4 tiles meeting at `(vx, vy)` (`(vx-1, vy-1)`, `(vx, vy-1)`, `(vx-1, vy)`, `(vx, vy)`) is a solid wall, that corner's weight is `0`; otherwise the first interior vertex immediately receives full `256` weight.
  - In `SampleTileFogState()`, uses a 2x steeper ramp (`ramped = std::min(256, rawWallFade * 2)`) with a concave ease-out (`(ramped * (512 - ramped)) >> 8`), cutting the empty margin between walls and fog in half while still feathering cleanly to `0%` at the wall line in all 8 directions.
- **Exact Floor-vs-Wall Isometric Z-Order (`DrawCell` in `Source/engine/render/scrollrt.cpp`):**
  - `DrawGroundFogTileBase(out, tilePosition, targetBufferPosition, LightTableIndex)` is invoked inside `DrawCell()` immediately after floor microtiles `mt[0]` and `mt[1]` (and `DrawWeatherGroundTile()`) finish, and **before** `targetBufferPosition.y -= TILE_HEIGHT` and the upper wall/pillar/arch microtile loop `mt[2..15]` run.
- **True 3D Volumetric Slope Shading + Screen-Space Vertical Z-Loft (`DrawGroundFogTileBase`):**
  - Computes the vertical screen-space heightmap derivative `verticalSlope = hBelow - hAbove` across every cloud puff.
  - Shades the **lower underside crease (`verticalSlope < -10`)** at the floor base `sy` with a dark shadow trough (`litTrough` at `Blend37`/`Blend25`).
  - Physically lifts the **upper cloud body and lit top dome** upward in screen space (`sy - zLift`, up to `9` pixels above the floor plane, scaled by `wallFade` so it flattens to `0` at walls) and shades the top-facing dome (`verticalSlope > 8`) with `litCrest` (`Blend37`/`Blend50`).
  - Slowed fog drift speed by ~3x (`driftDivX = 115..240`, `driftDivY = 120..310`) and removed dynamic `loftOffset` UV jitter so fog glides slowly and heavily across the floor.

### 11.2 Directional U-Shield Entity & Spell Cutout, Per-Floor Spawn Chance, 6 Distinct Biome Presets & Elliptical Boot Submersion
- **Directional U-Shield Cutout Around Entities & Spells (`FogCell::dir`, `VelocityToDirection`, `SampleTileFogState`):**
  - `FogCell` stores `uint8_t dir` (the 8-way `Direction` of the player, monster, or missile, with `VelocityToDirection()` computing missile travel direction from `missile.position.velocity`).
  - Projects local tile offset `(isoX, isoY)` onto the facing vector `(fwd, side)`: behind the entity (`fwd < 0`) is a smooth rounded semicircle (`side^2 + fwd^2`), while in front of the entity (`fwd >= 0`) forms straight parallel side walls capped by a **flat straight leading edge** perpendicular to the facing direction (`std::max(absSide, fwdFlat)`).
  - Increased density recovery rate from `+4` to `+10` per tick so stepped-off tiles close up cleanly behind the U-shield wake.
- **6 Visually Distinct Dungeon Biome Presets (`GetLevelFogConfig`) & Flat Liquid/Lava/Acid Pool Support (`IsFogPassableTile`):**
  - **Cathedral (`DTYPE_CATHEDRAL`):** Sparse cold spectral mist (`maxDensity = 165`, `intensityCutoff = 86`, `uvScale = 22`, `PAL16_BLUE + 2` crest, `driftDivX = 210, driftDivY = 290`).
  - **Catacombs (`DTYPE_CATACOMBS`):** Medium warm dusty bone-beige haze (`maxDensity = 190`, `intensityCutoff = 76`, `uvScale = 24`, `PAL16_BEIGE + 2` / `PAL16_BEIGE + 6`, `driftDivX = 185, driftDivY = 260`).
  - **Caves (`DTYPE_CAVES`):** Upward-rising geothermal steam (`maxDensity = 200`, `intensityCutoff = 72`, `uvScale = 26`, `driftDivX = 140, driftDivY = -155`, `PAL16_GRAY + 2` / `PAL16_BEIGE + 4`).
  - **Hell (`DTYPE_HELL`):** Dark charcoal/black rising soot smoke (`isDarkSmoke = true`, raw dark charcoal body `PAL16_GRAY + 13` and black soot trough `PAL16_GRAY + 15` with glowing ember-orange top crests `PAL16_ORANGE + 4`, `driftDivX = 115, driftDivY = -120`).
  - **Crypt (`DTYPE_CRYPT`):** Heavy, dense tomb fog blanket (`maxDensity = 250`, `intensityCutoff = 54`, `allowHeavyBlend = true`, large `uvScale = 20` banks, `driftDivX = 240, driftDivY = 310`).
  - **Hive / Nest (`DTYPE_NEST`):** Slowly pulsing yellowish-green bio-spore clusters (`hasPulse = true`, tighter `uvScale = 34`, `PAL16_YELLOW + 2` / `PAL16_YELLOW + 6`, `driftDivX = 165, driftDivY = -210`).
- **Elliptical Boot Submersion (`DrawGroundFogEntitySubmersion`):**
  - Renders a tight `28x16` (`RadiusX = 14, RadiusY = 8`) radially attenuated ellipse over player/monster boots.

### 11.3 Application Branding (`DevilutionX: Eternity`), Main Menu Version (`DX-Eternity Beta v0.7d`) & Discord Rich Presence (`Source/diablo.cpp`, `Source/CMakeLists.txt`, `Source/discord/discord.cpp`)
- **Main Menu Bottom-Left Version String (`Source/diablo.cpp`):**
  - Updated `gszProductName` (`"DX-Eternity Beta v0.7d"`), `gszVersionNumber` (`"Beta v0.7d"`), and `SetApplicationVersions()` so `UiMainMenuDialog(gszProductName, ...)` renders `DX-Eternity Beta v0.7d` in the bottom-left corner of the Main Menu.
- **Application & Window Title (`Source/CMakeLists.txt`):**
  - Updated the generated `config.h` block in `Source/CMakeLists.txt` to `#define PROJECT_NAME "DevilutionX: Eternity"` and `#define PROJECT_VERSION "Beta v0.7d"`, updating the SDL2 window title (`SpawnWindow(PROJECT_NAME)`) and dialog boxes without altering user save/config directory paths (`"diasurgical"` / `"devilution"` in `Source/utils/paths.cpp`).
- **Discord Rich Presence (`-DDISCORD_INTEGRATION=ON`, `Source/discord/discord.cpp`):**
  - Enabled `-DDISCORD_INTEGRATION=ON` in the MinGW-w64 CMake build (linking against `discord_game_sdk.dll`).
  - Updated `UpdateMenu()` and `UpdateGame()` in `Source/discord/discord.cpp` so both keep `activity.SetName("DevilutionX: Eternity")` and `activity.SetDetails("DevilutionX: Eternity")` permanently visible, while `UpdateGame()` combines location, difficulty, level, and class into `activity.SetState(...)` (`"{Location} ({Difficulty}) - Level {N} {Class}"`) and sets `gszProductName` (`"DX-Eternity Beta v0.7d"`) as the icon hover tooltip text.

### 11.4 Ground Fog Overhaul v2: Hell Black Smoke Fix, 3-Altitude Volumetric Ray-Slicing + Drop-Shadows, 60+ Hz Sub-Texel Bilinear Drift, ~1.5px Wall Margin & Continuous Half-Tile Wake (`Source/engine/render/ground_fog.cpp`)
- **Hell Black Smoke Fix (`PAL16_GRAY + 15` = System White `255` Bug Fixed):**
  - In Diablo 1's 256-color palette (`Source/lighting.cpp:325`), index `255` (`PAL16_GRAY + 15`) is reserved for system Pure White `(255, 255, 255)`, whereas index `0` is Pure Black `(0, 0, 0)` and `PAL16_GRAY + 14` (`254`) is the darkest charcoal ramp color.
  - Updated `DTYPE_HELL` in `GetLevelFogConfig()` to use `troughShade = 0` (`Pure Black`), `coreShade = PAL16_GRAY + 14` (`254`, darkest charcoal soot), and `crestShade = PAL16_GRAY + 12` (`252`, dark ash-charcoal) with `isDarkSmoke = true` on all three shades, eliminating bright white/orange stripe artifacts in Hell smoke.
- **3-Altitude Volumetric Ray-Slicing + Overhead Drop-Shadows + `2x2` Bayer Dithering:**
  - Replaced `std::fabs` ridge-fold creases in `EnsureFogNoiseInitialized()` with quintic C2-continuous (`t^3 * (t * (t * 6 - 15) + 10)`) 4-octave cumulus domes (`s = 128, 64, 32, 16`) with smooth rounded tops (`norm * (2.0 - norm)`).
  - Each screen pixel `(sx, sy)` is written at most once and ray-marched across 3 isometric altitudes:
    - **Altitude 0 (`z = 0 px`, `0.81x` drift):** Ground-hugging mist (`hBase`).
    - **Altitude 1 (`z = +3.5 px`, `1.00x` drift):** Mid cloud body on decorrelated slice `(+97, +151)` (`hMid`).
    - **Altitude 2 (`z = +7.0 px`, `1.19x` wind shear):** Upper 3D billow domes on decorrelated slice `(+193, +53)` (`hTop`).
  - Samples overhead cloud density `hOverhead` (`z = +12.0 px`) to cast dark underside drop-shadows (`hOverhead > hTop + 26`) onto lower cloud layers and floor, and applies a `2x2` Bayer dither matrix (`Bayer2x2[sy & 1][sx & 1]`) across blend thresholds (`Blend50`, `Blend37`, `Blend31`, `Blend25`, `Blend19`, `Blend12`, `Blend6`) to eliminate contour banding.
- **60+ Hz Sub-Texel Bilinear Drift (`SampleLutBilinear`):**
  - Replaced integer-stepped `ticks / driftDiv` LUT indexing with 24.8 fixed-point sub-texel coordinates (`driftXFixed1 = (ticks * 256) / cfg.driftDivX`) and 2D bilinear interpolation (`SampleLutBilinear`), eliminating 3–5 Hz quantization stutter.
- **~1.5px Wall Margin + Smooth 6-Step Fade-Out to `6.25%` (`Blend6`):**
  - Decoupled cloud shape evaluation (`cloudIntensity >= cfg.intensityCutoff`) from `wallFade` and computed exact fractional 8-direction wall distance (`minDist` in `0..255` across N/S/W/E edges and NW/NE/SW/SE corners) in `SampleTileFogState()`.
  - Leaves a tight `~1.5px` gap (`minDist <= 12 -> wallFade = 0`) next to solid walls and ramps opacity smoothly over the next ~14 pixels through 6 blending tiers (`Blend37 -> Blend31 -> Blend25 -> Blend19 -> Blend12 -> Blend6 -> 0%`) without carving rigid V-trough corridors or abrupt cutoffs.
- **Continuous `+0.5 Tile on Each Side` Entity & Missile Wake (`gFogHalfGrid`):**
  - Replaced the single-tile `gFogGrid` with a continuous half-tile resolution grid (`gFogHalfGrid[MAXDUNX * 2 + 3][MAXDUNY * 2 + 3]`) bilinearly interpolated across every tile's `(u256, v256)` coordinates.
  - `CarveContinuousEntityWake()` converts each player's/monster's/missile's sub-tile walking/flight offset into continuous half-tile space and carves a radial wake of radius `2.0` half-tiles (`1.0` full tile in every direction = `0.5` center + `0.5` on each side), smoothly pushing fog outward across tile boundaries.
- **+10% Transparency on Denser Layers:**
  - Added `Blend31` (`31.25%`) and `Blend6` (`6.25%`) and shifted dense tiers down by `+10%` transparency (`Blend37 -> Blend31 -> Blend25 -> Blend19 -> Blend12 -> Blend6`, with `Blend50` reserved only for the very peak of Crypt heavy tomb fog).

### 11.5 Batch 2: Caves Deterministic Lava Color Variants & 3D Boiling Lava Bubbles (`Source/engine/palette.cpp`, `Source/effects.h`, `Source/effects.cpp`, `Source/engine/render/ground_fog.cpp`)
- **4 Deterministic Caves Lava Color Variants (`ApplyCavesLavaVariant` in `Source/engine/palette.cpp`):**
  - In `DTYPE_CAVES`, palette indices `1..31` are the animated lava indices cycled every 50 ms by `palette_update_caves()` (`CycleColors(1, 31)`).
  - Hooked `ApplyCavesLavaVariant(hash)` into both `LoadRndLvlPal()` and `ApplyProceduralPaletteShift()` when `*sgOptions.Graphics.randomizePalette` is enabled on `DTYPE_CAVES`, selecting one of 4 deterministic lava variants from the floor's palette hash (`(hash >> 11) & 3`) while preserving each index's brightness wave for `CycleColors(1, 31)`:
    - **Variant 0 — Classic Molten Magma (Orange-Amber):** Deep crimson crust -> fiery orange -> incandescent golden-white core.
    - **Variant 1 — Sulfuric Brimstone Lava (Golden-Yellow):** Dark bronze crust -> intense sulfuric yellow -> blinding lemon-white core.
    - **Variant 2 — Poison / Toxic Acid-Lava (Venomous Green):** Dark forest-sludge crust -> neon toxic emerald -> radioactive chartreuse-white core.
    - **Variant 3 — Subterranean Phosphene Lava (Bioluminescent Cyan-Teal):** Deep abyssal teal crust -> electric cyan -> glowing ice-cyan core.
- **Caves 3D Boiling Lava Bubbles & Positional `LS_LAVABUBBLES` SFX (`Source/effects.h`, `Source/effects.cpp`, `Source/engine/render/ground_fog.cpp`):**
  - Registered `LS_LAVABUBBLES` (`"sfx\\misc\\LavaBubbles.wav"`) in `enum _sfx_id` and `sgSFX[]`.
  - Detects Caves lava tiles (`IsCavesLavaTile`: bright animated magma tiles plus `DPieceMicros` checks for palette indices `1..31`) and maintains up to `MaxLavaBubbles = 64` active bubbles within `±15` tiles of the player (`ResetCavesLavaBubbles()`, `UpdateCavesLavaBubbles()`), pre-seeded on level entry so lava is already boiling immediately.
  - Captures a pre-fog liquid magma pixel mask (`wasLavaPx[32][64]`, where `px >= 1 && px <= 31`) before `DrawGroundFogTileBase()` blends steam over the tile, and renders `DrawCavesLavaBubblesOnTile()` **after** ground fog so rising geothermal steam never washes out the incandescent bubbles:
    - Snaps each bubble center onto actual liquid magma pixels (`wasLavaPx`) and clips against solid stone crusts (`!wasLavaPx`).
    - Samples `hotIdx`, `midIdx`, and `rimIdx` dynamically from `system_palette[1..31]` by luminance so bubbles automatically match all 4 Caves lava color variants (Orange, Golden-Yellow, Poison Green, and Cyan-Teal) in real time as `CycleColors(1, 31)` animates.
    - **Phase 1 (`0%–72%` life):** Swelling 3D hemispherical lava blister (`maxRadX = 6..9 px`, `maxRadY = 4..6 px`, `domeLift = 5 px`) with dark cooled magma base skirt, glowing mid-body, and off-center incandescent top-left specular highlight.
    - **Phase 2 (`72%–100%` life):** Popping 2:1 isometric crater rim + 4 arcing molten splatter droplets (`dx, dy - arcLift`), triggering positional `PlaySfxLoc(LS_LAVABUBBLES, b.tile, false)` (with `220 ms` cooldown within 13 tiles of the player).

### 11.6 Ground Fog v3 (30 Hz Pre-Composited LUT Sheets, 2-Layer Isometric Camera Parallax, 4-Corner Light-Radius Feathering) & Lava Bubble Fixes (`Source/diablo.cpp`, `Source/engine/render/weather.cpp`, `Source/engine/render/ground_fog.cpp`)
- **Massive Performance Optimization (~5x–8x Faster, 30 Hz Pre-Composited LUT Sheets + Interior Tile Fast-Path):**
  - Added `UpdateCachedFogSheets30Hz(ticks, cfg)`, which updates two `256x256` L1-cache-resident pre-warped, sub-texel bilinearly drifted sheets (`gCachedFloorFog[256][256]`, `gCachedCanopyFog[256][256]`, and `gCachedCanopyLight[256][256]`) once every `33 ms` (`30 Hz`).
  - Inside `DrawGroundFogTileBase()`, each screen pixel performs only 2 direct `O(1)` byte array reads (`gCachedFloorFog[v1Idx][u1Idx]` and `gCachedCanopyFog[v2Idx][u2Idx]`) with zero per-pixel `SampleLutBilinear()` calls.
  - Added `fastInteriorTile` fast-path that skips `SampleTileFogState()` completely on interior floor tiles with no adjacent walls and no active entity wake, plus `gBubbleCountOnTile[MAXDUNX][MAXDUNY]` so `DrawGroundFogTileBase()` never scans `32x64` pixels on empty Caves lava tiles.
- **True 2-Layer Isometric Camera Parallax + Cross-Wind Drift Separation:**
  - **Layer 1 (Low Ground-Hugging Mist — `1.00x` Floor-Locked):** Locked to floor tiles `(tx * uvScale1, ty * uvScale1)`, pools in dark troughs, parts `100%` around player/monster/spell wakes, and feathers at walls.
  - **Layer 2 (Elevated Floating Cloud Canopy — `1.35x` Isometric Camera Parallax + Cross-Wind Shear):** Shifts across the floor by `+35%` of live camera movement (`camParU, camParV`), uses `1.5x` larger billowy cloud domes (`uvScale2 = (uvScale1 * 2) / 3`), drifts at a ~40° cross-wind angle relative to Layer 1, casts dark underside shadows (`litTrough`) onto Layer 1 below, and parts slightly less over ground wakes.
- **Slower, Lingering Entity & Spell Dispersion Recovery:**
  - Reduced `gFogHalfGrid` wake regrowth in `UpdateGroundFogSimulation()` to `+1` per 50ms tick when strongly parted (`density < 110`) and `+2` per tick when closing, so cleared trails behind the player and spells linger organically for ~6–7 seconds.
- **4-Corner Bilinear Light-Radius Feathering:**
  - Attenuates `crestShade` and `coreShade` through `lightTbl` on all dungeon types (including Hell `isDarkSmoke`) and computes 4-corner bilinear light-radius feathering (`cLight00, cLight10, cLight01, cLight11`) across the 3x3 tile neighborhood (`dLight`) for `lightIndex >= 8`, smoothly fading fog opacity (`wallFade * lightFade`) down to `0%` (`Blend6 -> 0`) at the edge of the light radius.
- **Caves Lava Bubble Shoreline Rock Margin (`±4px` X / `±3px` Y), ESC Menu Pause & Single-Instance Proximity Audio Loop (`SyncLavaBubbleAudio`):**
  - Eroded `safeLavaPx[32][64]` by `4px` horizontally and `3px` vertically from cave rock pixels (`rawLava`) and checked `bubbleFootprintFits()` so lava bubbles never touch or clip against shoreline rocks.
  - Paused color cycling (`diablo_color_cyc_logic()` in `Source/diablo.cpp`), rain/puddle splashes (`gWeatherSimTick` in `Source/engine/render/weather.cpp`), and lava bubbles (`UpdateCavesLavaBubblesSimulation()` / `EnsureLiveLavaBubbleShades()`) when the Single Player `ESC` menu (`gmenu_is_active()`) is open.
  - Replaced positional `PlaySfxLoc(LS_LAVABUBBLES, ...)` calls with `SyncLavaBubbleAudio()` using single-instance non-positional `PlayLoopingSFX(LS_LAVABUBBLES, lavaVolume)` (smoothly scaling volume from `-1750` at 15 tiles to `-150` adjacent to lava) and `StopSFX(LS_LAVABUBBLES)` immediately when opening the `ESC` menu, pausing, or leaving Caves.

### 11.7 Ground Fog v4 (2-Layer World-Locked Separation, Slow Drift & High-Contrast Hell Smoke), `LavaBubbles.wav` Loop Fix & Final Ambience Batch (`Source/effects.h`, `Source/effects.cpp`, `Source/engine/render/weather.cpp`, `Source/engine/render/ground_fog.cpp`)
- **Fix `LavaBubbles.wav` Continuously Restarting & Cutting Off (`Source/engine/render/weather.cpp` & `Source/effects.cpp`):**
  - **Root Cause:** `UpdateWeatherState()` is called every frame in `scrollrt.cpp` and invokes `ResetWeatherState()` whenever `leveltype != DTYPE_TOWN`. Having `StopSFX(LS_LAVABUBBLES)` inside `ResetWeatherState()` killed `LavaBubbles.wav` on every single dungeon frame right after `SyncLavaBubbleAudio()` started it.
  - **Fix:** Removed `StopSFX(LS_LAVABUBBLES)` and `StopSFX(LS_INSECTBUZZ)` from `ResetWeatherState()` so dungeon frames never interrupt active dungeon ambience loops; they are stopped only on `OnWeatherNewGame()`, on returning to Town (`OnWeatherLevelLoad`), inside `SyncLavaBubbleAudio()` / `SyncInsectSwarmAudio()`, and in `sound_update()` / `effects_play_sound()` when `!gbRunGame`.
- **Ground Fog v4 — 100% World-Locked Coordinates, 3.5x–4x Slower Sub-Texel Drift, Distinct 2-Layer Separation & Hell Smoke Visibility (`Source/engine/render/ground_fog.cpp`):**
  - **Zero Camera-Move Shift:** Removed `camWorldPxX` / `camParU` / `camParV` so both Layer 1 (`baseWorldU1 = tx * uvScale1`) and Layer 2 (`baseWorldU2 = tx * uvScale2 + 37`) are 100% world-locked to dungeon floor tiles and never slide in reaction to player walking.
  - **3.5x–4x Slower Sub-Texel Drift:** Increased `driftDivX` and `driftDivY` to `460..980` across all 6 dungeon presets and replaced integer `(warpDriftX >> 8)` indexing with sub-texel `SampleLutBilinear(gFogWarpLut, ...)` inside `UpdateCachedFogSheets30Hz()`.
  - **Distinct 2-Layer Volumetric Separation + SE Drop-Shadows:**
    - **Layer 1 (Low Ground Mist Bed):** Finer spatial scale (`uvScale1 = cfg.uvScale`), wider floor coverage (`floorCutoff = cfg.intensityCutoff`), slow creeping drift, shaded with `litCore` / `litTrough`.
    - **Layer 2 (Elevated 3D Cloud Puffs):** `2x` larger scale (`uvScale2 = cfg.uvScale / 2`), higher threshold (`canopyCutoff = cfg.intensityCutoff + 24`) forming distinct floating 3D cloud islands gliding at a ~65° cross-wind angle across Layer 1, highlighted with bright `litCrest` domes (`+36` opacity boost) and casting a dark `litTrough` drop-shadow (`+2, +2` texels SE) onto Layer 1 underneath.
  - **High-Contrast Hell Smoke (`DTYPE_HELL`):** Uses `crestShade = PAL16_GRAY + 5` (smoky ash-gray upper billow crests), `coreShade = PAL16_GRAY + 9` (dark smoldering charcoal smoke body), and `troughShade = 0` (Pure Black shadow troughs), making both layers clearly visible over Hell's floor while darkening smoothly to black at the edge of the light radius.
- **Hive & Catacombs 3D Insect Swarms (`Source/effects.h`, `Source/effects.cpp`, `Source/engine/render/ground_fog.cpp`):**
  - Spawns up to `MaxInsectSwarms = 18` 3D orbiting insect swarms in open room tiles (`IsInsectSwarmRoomTile`) on `DTYPE_CATACOMBS` and `DTYPE_NEST` across 3 visual variants (Carrion Flies, Plague Locusts, Blood Wasps/Mites).
  - Features slow autonomous room drift, single-instance proximity `LS_INSECTBUZZ` loop (`sfx\misc\InsectBuzz.wav` via `SyncInsectSwarmAudio()`), and permanent walk-through dispersal with outward radial scatter and one-shot `LS_INSECTDISPERSE` (`sfx\misc\InsectBuzzDisperse.wav`).
- **Tristram Snow, Blizzard (`SnowHeavy`) & Ash Storm (`Source/effects.h`, `Source/effects.cpp`, `Source/engine/render/weather.cpp`):**
  - Rolls with equal `1/3` probability per New Game session (`TownWeatherKind::Rain`, `Snow`, `AshStorm`) locked for the entire game session, toggling only between `Light` and `Heavy` after `>= 1` minute in a dungeon.
  - `SnowHeavy` renders a wind-driven blizzard with gale-force lateral shear and diagonal flurry streaks (`SnowLight.wav` / `SnowHeavy.wav`), while `AshStorm` mixes swirling dark ash flakes with glowing orange-red volcanic ember cinders (`AshStormLight.wav` / `AshStormHeavy.wav`) and settling ground particles (`DrawSingleSettlingSnowFlake`).

---

## 12. Torment Difficulty (`DIFF_TORMENT`): Blanket Hell + 30% Monster Stats & Maxed-Out Item Drops

### 12.1 `DIFF_TORMENT` Enum, UI, Hell Diablo Unlock Requirement, Portrait Skulls & Save/Load Persistence (`Source/levels/gendung.h`, `Source/DiabloUI/hero/selhero.cpp`, `Source/DiabloUI/multi/selgame.cpp`, `Source/automap.cpp`, `Source/discord/discord.cpp`, `Source/loadsave.cpp`)
- Added `DIFF_TORMENT` after `DIFF_HELL` in `enum _difficulty : uint8_t` (`Source/levels/gendung.h`) and updated `DIFF_LAST = DIFF_TORMENT`, automatically enabling the 4th difficulty skull indicator on hero selection portraits (`selhero.cpp`) when Diablo is slain on Torment.
- Added `"Torment"` to the Create Game difficulty selection list (`selgame_GameSelection_Select`, shifting list Y from `+ 282` to `+ 279` so all 4 difficulty tiers fit cleanly), game info panel (`selgame_GameSelection_Focus`), difficulty description (`selgame_Diff_Focus`), multiplayer level requirement (`heroLevel >= 35` in `IsDifficultyAllowed`), automap difficulty readout (`Source/automap.cpp`), and Discord Rich Presence (`DifficultyStrs` in `Source/discord/discord.cpp`).
- **Locked Until Hell Diablo Is Defeated (`heroRank >= 3`):** Tracked `heroRank = pInfo->herorank` (`player.pDiabloKillLevel`) in `UpdateHeroLevel` (`Source/DiabloUI/multi/selgame.cpp`) and synced `gSaveNumber = selhero_heroInfo.saveNumber` before Single Player difficulty selection (`Source/DiabloUI/hero/selhero.cpp`). Characters that have not yet slain Diablo on Hell difficulty (`heroRank < 3`) see `"Torment"` grayed out (`UiFlags::ColorUiSilver`) with a locked requirement note in the description panel, and attempting to start or join a Torment game in Single Player or Multiplayer displays `"Your character must defeat Diablo on Hell difficulty before you can enter a game of Torment difficulty."`.
- Updated `LoadGame()` (`Source/loadsave.cpp`) to validate `sgGameInitInfo.nDifficulty <= DIFF_LAST`.

### 12.2 Blanket Hell + 30% Monster Offensive & Defensive Stats (`Source/monster.h`, `Source/monster.cpp`, `Source/missiles.cpp`, `Source/loadsave.cpp`)
- Promoted `Monster::minDamage`, `maxDamage`, `minDamageSpecial`, `maxDamageSpecial`, and `armorClass` from `uint8_t` to `uint16_t` (`Source/monster.h`) so high-tier monsters and bosses do not overflow `255` when scaled by `+30%` over Hell (`* 130 / 100`).
- Scaled monster `maxHitPoints`/`hitPoints`, `minDamage`/`maxDamage`, `minDamageSpecial`/`maxDamageSpecial`, `armorClass`, `toHit`, `toHitSpecial`, `level` (which drives hit/block/stun/regen/drop-tier formulas), `exp`, monster kill history (`PrintMonstHistory`), and fixed-damage monster missiles (`AddChargedBolt`, `AddChargedBoltBow`, `AddFireWall`, `ProcessAcidSplate`) to **130% of their `DIFF_HELL` values**, while using `resistanceHell` and enabling Hell Diablo's exclusive unique loot drops (`SpawnLoot`).
- Preserved binary save file compatibility in `SaveMonster` (clamping `uint16_t` damage/AC fields to `uint8_t`) while recalculating unclamped `uint16_t` Torment monster damage and AC in `LoadMonster`.

### 12.3 Maxed-Out Item Stat Rolls on `DIFF_TORMENT` (`Source/items.h`, `Source/items.cpp`, `Source/pack.cpp`, `Source/msg.cpp`)
- Added `CF_TORMENT = 1 << 1` to `enum icreateinfo_flag2` (`Item::dwBuff`) and preserved it across `InitializeItem`, `GetItemAttrs`, `UnPackItem` (`Source/pack.cpp`), and network `RecreateItem` (`Source/msg.cpp`) so items generated in Torment retain their maxed-out stats across save/load and character unpacking.
- Every rolled item stat (`RndPL` via `SaveItemPower` for all magic prefixes, suffixes, To-Hit bonuses, and unique item variable powers; `GetItemAttrs` for base armor class `iMaxAC` and `+30%` max gold piles; `GetStaffSpell` for `sStaffMax` charges; `ItemRndDur` for full `100%` `item._iMaxDur` starting durability) still consumes its exact `GenerateRnd(...)` call so LCG state and `GetTranslatedItemNameMagical` never desync, and returns the **maximum possible roll**.
