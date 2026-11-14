# DX:Eternity Project Roadmap

- [x] **Phase 1: Quality of Life & Endgame Loop**
  - Open town shortcuts (Catacombs, Caves, Hell) from the start in Single Player
  - Endless Single Player session after killing Diablo (no forced credits cut, full boss loot drop)
- [x] **Phase 2: Custom Unique Weapon (`Kadi's Fury`) & Dungeon FastWalk**
  - `Kadi's Fury` Unique Broad Sword with dungeon-only Faster Walk bonus
  - Classic Diablo save/load compatibility for custom unique items
- [x] **Phase 3: Hive & Crypt Dungeon Branches + Audio/FPS Optimizations**
  - Second down-staircase on Caves Level 12 -> Hive (Levels 17–20)
  - Second down-staircase on Hell Level 14 -> Crypt (Levels 21–24)
  - Full save/load support for Levels 17–24 in classic Diablo mode
  - Audio stream pooling & multi-tick animation SFX fixes
- [x] **Phase 4: Classic Loot Filter & Hell Diablo Exclusive Uniques**
  - Pure classic Diablo loot tables across Hive and Crypt (no oils, runes, or Hellfire-only base items)
  - Suppress Defiler quest pop-up monologues on all Hive levels
  - Hell Diablo exclusive unique weapon drops (`2%` independent chance each on Hell difficulty):
    - `Kadi's Fury` (Broad Sword, `Indestructible`, unused `ITEM_MAGSWORD` inventory graphic)
    - `Apostate's Crook` (War Staff, unused `ITEM_LONGSTAFF` inventory graphic)
    - `Loxley's Bend` (Long War Bow, unused `ITEM_STLSHRTBOW` inventory graphic, with physical arrow life/mana steal support)
- [x] **Phase 5: Female Barbarian Class**
  - Playable Barbarian class using classic Rogue sprites, portraits, inventory silhouette, and voiceovers
  - Full Barbarian stats, innate resistances, two-handed weapon mastery, and Rage skill
- [x] **Phase 6: Hive & Crypt Monster Roster Overhaul**
  - Keep unused vanilla monster models with original palettes (Spider Lords, Torchants, Bone Demons, Devil Kin Brutes, Invisible Lords, Satyr Lords)
  - Replace remaining Hellfire monsters with matched vanilla dark-fantasy counterparts (including Magma Demons in the Hive and Mages in the Crypt) while keeping Hellfire projectiles
  - Fix Na-Krul death freeze and loopback item validation warning
- [x] **Phase 7: OpenAL Soft + EFX Spatial Audio, Reverb, Headphone Mode (HRTF) & Restored Footstep SFX**
  - OpenAL Soft 1.23.1 static audio backend with `ALC_EXT_EFX` (`AL_EFFECT_EAXREVERB`) environmental reverb per dungeon type (Town, Cathedral, Catacombs, Caves, Hell, Hive, Crypt) and 3-state intensity toggle (`Normal`, `High`, `OFF`)
  - EFX Low-Pass (`AL_FILTER_LOWPASS`) line-of-sight wall occlusion muffling in dungeons
  - Full-fidelity 3D positional audio (`BSinc24` resampling, 2D/self stereo direct HRTF bypass, HRIR comb-notch de-coloration, and wide horizontal-plane azimuth + deepened head shadow) with live in-game `Reverb` and `Headphone Mode (HRTF)` menu toggles
  - Restored 4-variation randomized player footstep sounds (`sfx\misc\walk1.wav` – `walk4.wav`) in dungeons and Town
- [x] **Phase 8: Tristram Weather & Environmental Particle System (Rain, Snow, Blizzard, Chaotic Horizontal Whirlwind Ash/Sand Storm, Puddle Splashes, Steam Fog & Thunder)**
  - Hardware-seeded shuffled 4-bag session weather roll with equal **25% spawn probability** for each of the 4 weather types (`Rain` 25%, `Snow` 25%, `Blizzard` / `SnowHeavy` 25%, and chaotic horizontal whirlwind `AshStorm` / Sandstorm 25% with +35% intensity across both Light & Heavy variants and additional +30% larger, +15% more transparent compact 2D tumbling ash/sand flakes across all layers) locked per game session
  - 3D multi-plane depth precipitation (`0.58x` Layer 0 distant background with 40–42% of particles occluded behind buildings/roofs/trees/walls via walkable-ground diamond compositing in `DrawWeatherGroundTile()`, `1.00x` Layer 1 midground world-plane, and `1.65x` Layer 2 near-camera foreground with DoF blur and balanced layer speeds)
  - Precipitation / Steam Fog (`Rain` only, disabled in `Snow` and `AshStorm`): non-overlapping world-space drifting 4-puff vapor clouds with per-bank speed variety and toroidal collision-avoidance solver, plus curling 5-puff rising ground-evaporation wisps
  - Localized compact, semi-transparent 5-stage isometric puddle splashes (`Rain`), pure neutral white (`PAL16_GRAY`) settling ground snowflakes ~74–95% transparent (`66/38/28 -> 44/24 -> 24/14`) (`Snow`/`Blizzard`), and +35% denser, additional +30% larger, +15% more transparent compact skittering sand/ash flakes (`AshStorm`) on walkable outdoor ground tiles rendered in `DrawCell` after `mt[0..1]`
  - Full-screen distant lightning palette flashes (`system_palette`, every 30–60s during `Rain`) with multi-strike flicker and speed-of-sound delayed thunder claps (`sfx\misc\thundera.wav` in Light Rain, `sfx\misc\thunderb.wav` in Heavy Rain)
  - Seamless ambient looping playback of `RainLightLoop.wav`, `RainHeavyLoop.wav`, `SnowLight.wav`, `SnowHeavy.wav`, `AshStormLight.wav`, and `AshStormHeavy.wav` (paused in Single Player `ESC` menu and cleanly stopped on dungeon entry or quitting to the main menu)
- [x] **Phase 9: Highest-Priority `dx-eternity.mpq` Custom Asset Archive & MPQ/SDL2 Hardening**
  - Highest-priority `dx-eternity.mpq` (`DX-ETERNITY.MPQ` / unpacked `dx-eternity`) loaded in `LoadCoreArchives()` and `LoadGameArchives()` and queried first in `FindMpqFile()` / `FindUnpackedMpqFile()`
  - Guard `MpqArchive` (`GetFileNumber`, `GetUnpackedFileSize`, `GetNumFiles`, `ReadFile`, `ReadBlock`, `OpenBlockOffsetTable`, `HasFile`) and `FindMpqFile()` against empty MPQ archives (`files == 0`, preventing `libmpq`'s `files - 1` unsigned underflow) and zero-byte file entries (`GetUnpackedFileSize(fileNumber) == 0`), cleanly falling back to `devilutionx.mpq` / `diabdat.mpq`
  - Verify `handle.read()` in `LoadFileInMem()` (`Source/engine/load_file.hpp`) before returning the buffer so failed reads never pass uninitialized memory to CLX font/sprite renderers
  - Disable `SDL_DYNAMIC_API` (`SDL_DYNAMIC_API=0`) in `Source/CMakeLists.txt` so static SDL2 builds never load an older `SDL2.dll` from the game directory, and keep `SetChunkStream()` at the 16 MB threshold so `dtowne.wav` (~31 MB) streams asynchronously
- [x] **Phase 10: Level Palette Randomizer (`DevilutionXRandomizer` Integration) & Caves-Only Lava Variants**
  - Enabled by default (`[Graphics] Randomize Level Palettes` in `diablo.ini`, description `"Randomizes dungeon level color palettes."`) with live callback (`OptionRandomizePaletteChanged`) that switches between the floor's vanilla palette and its locked deterministic randomized palette without rerolling on the spot
  - Preserves vanilla Town (`levels\towndata\town.pal`) unconditionally and supports direct loose folder lookup (`paths::BasePath() + relativePath`) in `FindAsset()` (`Source/engine/assets.cpp`)
  - Deterministic per-floor Murmur3 seed hashing (`glSeedTbl[currlevel]`, `currlevel`, `leveltype`) that never perturbs the gameplay `GenerateRnd` LCG
  - Multi-hue quality filter (`IsMultiHuePalette`) across `DevilutionXRandomizer` dungeon `.pal` presets (`l1_1..66.pal`, `l2_1..74.pal`, `l3_1..58.pal`, `l4_1..46.pal`, `l5_1..74.pal`, `l6_1..79.pal`) while preserving Hell's authentic red blood/lava/flesh entries (`1..31` and red-dominant `32..127` from `l4_1..4.pal`) and restricting new lava color rolls (`ApplyCavesLavaVariant`: Molten Magma Orange-Amber, Sulfuric Brimstone Golden-Yellow, and Poison Green) strictly to `DTYPE_CAVES` (`1..31`)
- [ ] **Phase 11: 3D Isometric Volumetric Ground Fog Overhaul & Caves Boiling Lava Bubbles (In Progress — Paused at Step 2C)**
  - [x] **Step 1 (Done):** Restored `nice-fog2.png` (`Ground Fog v6`) smoky visuals (`paletteTransparencyLookup` `Blend6..Blend50` + `Bayer4x4` sub-pixel dithering + `PAL16_GRAY + 3 / + 6 / + 10` shades + `Pure Black (0)` cast drop-shadows) with 40% slower travel and internal swirl (`UpdateCachedFogSheets30Hz`)
  - [x] **Step 2A (Done):** Carve +35% more open bare-stone floor ("free") space (~55% open floor clearings / ~45% fog coverage)
  - [x] **Step 2B (Done):** Eliminated the 4x4-tile repeating clone pattern via dual independent non-harmonic coprime world sheets (`gCachedFloorFog` + `gCachedFloorFogB` and `gCachedCanopyFog` + `gCachedCanopyFogB`) sampled at incommensurate coprime scales and rotated isometric axes (`UvScale1A = 23` + rotated `(15, 8)`; `uvScale2A = 19` + rotated `(12, 7)`) with counter-rotating swirl and static `libpthread.a` / `libwinpthread.a` linking
  - [ ] **Step 2C (In Progress — Resume Here):** Further optimize dual-sheet Ground Fog FPS (improved via pre-scaled sheets + 1-tap coarse bounds, but still needs additional speedup to eliminate remaining frame-rate hit)
  - [ ] **Step 3 (Pending):** Eliminate the hard diagonal cutoff near dungeon walls
  - [ ] **Step 4 (Pending):** Submerge entities and objects into the fog up to roughly player character calf/knee height (`DrawGroundFogEntitySubmersion`) instead of standing on top of the fog like a solid surface
  - [x] Smoothly swelling Caves Lava Bubbles (`DrawCavesLavaBubblesTile`) rendered strictly below the ground fog pass using live cycled lava palette indices (`1..31`), with smooth 15-tile quadratic distance-attenuated `LS_LAVABUBBLES` audio (`MaxHearRadius = 15` tiles)
- [x] **Phase 12: Torment Difficulty (`DIFF_TORMENT`) & Client-Server Item Validation Sync**
  - `DIFF_TORMENT` difficulty tier (`DIFF_LAST = DIFF_TORMENT`, 4th hero portrait skull indicator, Create Game UI menu, Automap readout, Discord Rich Presence, locked in Single Player and Multiplayer until the character has defeated Hell Diablo (`heroRank >= 3`), plus multiplayer level 35 entry requirement)
  - Blanket `+30%` over Hell monster stats (`maxHitPoints`, `uint16_t` `minDamage`/`maxDamage`/`minDamageSpecial`/`maxDamageSpecial`/`armorClass`, `toHit`, `toHitSpecial`, `level`, `exp`, fixed-damage monster missiles, and Hell Diablo unique drops) with unclamped `uint16_t` restoration on save/load
  - Maxed-out item stat rolls (`CF_TORMENT` flag on `Item::dwBuff`, max `RndPL` rolls for all magic prefixes/suffixes/To-Hit bonuses/unique powers, max base `iMaxAC`, max staff spell charges `sStaffMax`, `100%` starting durability `_iMaxDur`, and `+30%` max gold piles) while preserving exact `GenerateRnd` LCG call parity
  - Whitelist `IDI_SORCERER_DIABLO` (`166`) and `IDI_BARBARIAN` (`132`) in `IsItemAvailable(int i)` when `!gbIsHellfire` for 1:1 `UnPackItem` / `RecreateItem` parity with the Closed Realm Server-Vault
- [x] **Phase 13: Closed Realm Client — Step 1 (Bare Minimum Account, Auth, Character Vault, Game Lobby & Gameplay Ledger Hooks)**
  - [x] Configurable Realm Server IP & Port in `diablo.ini` (`[Network] Realm Host` default `"127.0.0.1"`, `[Network] Realm Port` default `6112`, `[Network] Realm Username`) + in-game `"Change Server IP"` UI dialog
  - [x] Closed Realm connection provider (`"Closed Realm (DX:Eternity)"`, `SELCONN_REALM`) with `Log In` and `Create Account` dialogs (`OP_AUTH_CHALLENGE_REQ`, `OP_ATTEST_HANDSHAKE_REQ`, `OP_AUTH_REGISTER_REQ`, `OP_AUTH_LOGIN_REQ`)
  - [x] Server-authoritative Character Vault (`OP_HERO_LIST_REQ`, `OP_HERO_CREATE_REQ`, `OP_HERO_LOAD_REQ`, `OP_HERO_SAVE_REQ`, `OP_HERO_DELETE_REQ`, plus `OP_STASH_LOAD_REQ` / `OP_STASH_SAVE_REQ`) with `RealmPlayerPack` (`1262B`) conversion
  - [x] Closed Realm Game Lobby (`OP_GAME_LIST_REQ`, `OP_GAME_CREATE_REQ`, `OP_GAME_JOIN_REQ`) with blank default Game Name input, Automap top-left `Game: <GameName>` readout (instead of `"Offline Game"`), and `Fast (30Hz)` / `Normal (20Hz)` speed cap
  - [x] Server-authoritative gameplay & loot ledger hooks (`OP_FLOOR_TRANSITION_REQ`, `OP_MONSTER_DROP_REQ`, `OP_CHEST_DROP_REQ`, `OP_ITEM_PICKUP_REQ`, `OP_ITEM_DROP_REQ`)
  - [x] Built-In Red-Team / Chaos Security Test Hotkeys (`#ifdef REALM_CHAOS_TEST`, `Ctrl+Alt+F5..F10` in `RunRealmChaosExploitTest`) & `RealmClientLog` (`dx-eternity-client-realm.log`)

---

## Active Development Backlog (Segregated into Small Development Chunks)

### Category A: Closed Realm (`instr-client-closed-realm.md` & `dx-eternity-realm`)
- **A1. Login UX — Password Masking & Save Password (`Source/DiabloUI/multi/selconn.cpp`):**
  - [ ] Mask password text input with standard `*******` by default during Closed Realm Login / Account Creation, with an in-dialog `"Show / Hide Password"` toggle.
  - [ ] Add a `"Save Password"` checkbox/toggle in the Closed Realm Login dialog that persists credentials to a simple plaintext `.txt` file in the game directory, accompanied by a warning (`"Warning: Only save your password on a private machine you trust."`).
- **A2. One-Way Local `.sv` Backup Mirror & Admin `.pack` / `.stash` Exporter (`Section 2 & 2.1`, `Source/pfile.cpp`, `Source/qol/stash.cpp`):**
  - [ ] Write a standard local `.sv` backup copy via `pfile_write_hero()` whenever a verified snapshot is received from the server (`OP_HERO_CREATE_RESP`, `OP_HERO_LOAD_RESP`, or `OP_HERO_SAVE_RESP` with `RealmStatus::OK`), while strictly never reading local `.sv` files in Closed Realm mode.
  - [ ] Support exporting decrypted `<HeroName>.pack` (`1262`B `PlayerPack`) and `exported.stash` (`StashDataHeader + N*PkItemStruct`) for Admin Web Console (`127.0.0.1:6113`) SP/MP character & stash import.
- **A3. 1-Page Shared Account Stash (`10x10`, `<= 100` Items), Physical `<= 5,000g` Gold Stacks & `300,000g` Cap (`Section 2.2`, `Source/qol/stash.cpp`):**
  - [ ] In Closed Realm mode (`gbIsClosedRealm == true`), disable multi-page stash navigation (`kMaxClosedRealmStashPages = 1`) and virtual gold deposit/withdraw buttons so gold is stored traditionally as physical `IDI_GOLD` (`1..5,000g`) piles inside the single `10x10` stash page.
  - [ ] Enforce `MaxStashItems = 100` and `kMaxClosedRealmStashGold = 300000U` (`CanPlaceItemInClosedRealmStash` / `GetPhysicalStashGoldTotal`) when placing items in Stash and serializing `OP_STASH_SAVE_REQ`.
- **A4. Lobby Game Search / Filtering (`Section 3 & 3.1`, `Source/DiabloUI/multi/selgame.cpp`, `Source/net/realm_client.cpp`):**
  - [ ] Implement `RealmSearchGames` (`OP_GAME_LIST_REQ` with `GameListFilterRequestPayload`: case-insensitive substring `searchQuery` matching `gameName` or player `heroName`, `difficultyFilter`, `hideFullGames`, and `hidePasswordedGames`) and expose search/filter controls in the Closed Realm lobby UI.
- **A5. Static Lobby Chat Channels (`#General`, `#Trade`, `#LFG`, `#Torment`) & Slash Commands (`Section 3 & 3.1`, `Source/net/realm_client.cpp`, `Source/DiabloUI/multi/selgame.cpp`):**
  - [ ] Implement `RealmJoinChatChannel` (`OP_CHAT_JOIN_CHANNEL_REQ`), `RealmSendChatMessage` (`OP_CHAT_SEND_REQ`, supporting `/w <HeroName> <msg>`, `/games [search]`, `/who`), and incremental `RealmPollChatFeed` (`OP_CHAT_POLL_REQ` every `500..1000ms`), displaying server-verified sender badges (`heroName`, `heroClass`, `heroLevel`, `CHAT_FLAG_TORMENT_CROWN`).
- **A6. Server-Oracle Loose Floor Item Suppression, Hell Diablo Anti-Boost Safeguard & Atomic Pickup/Drop Verification (`Section 4`, `Source/items.cpp`, `Source/monster.cpp`):**
  - [ ] Suppress local `InitItems()` random loose dungeon floor item generation when `gbIsClosedRealm == true` so all dungeon loot originates from server-oracle drops with valid `itemUid` provenance.
  - [ ] Implement the Hell Diablo Party Anti-Boost Safeguard on `MT_DIABLO` death on `DIFF_HELL`: if `(highestPartyLevel - MyPlayer->_pLevel) > 10` and `MyPlayer->_pLevel < 32`, do not increment `MyPlayer->pDiabloKillLevel` to `3` and display *"Your level is too far below the party to receive Hell Diablo kill credit"*.
  - [ ] Gate placing picked-up ground items into inventory on `OP_ITEM_PICKUP_RESP == RealmStatus::OK`, and attach returned `ItemDropResponsePayload::dropped.itemUid` to dropped ground items.
- **A7. Rolling-XOR Shadow Memory Canaries (`Section 5`, `Source/player.h`, `Source/player.cpp`):**
  - [ ] Mirror `_pBaseStr`, `_pBaseMag`, `_pBaseDex`, `_pBaseVit`, `_pStatPts`, `_pExperience`, `_pGold`, and `_pHitPoints` with a per-frame `uint32_t sessionXorKey` shadow value (`shadowField = realField ^ sessionXorKey`), verifying parity at the start of `Player::Process()`.
- **A8. Full Event Coverage in Closed-Realm Client Diagnostic Logger (`Section 6`, `Source/net/realm_client.cpp`):**
  - [ ] Expand `RealmClientLog` calls across `CONNECT`, `ATTEST`, `AUTH`, `HERO_LOAD`, `HERO_SAVE` (logging all equipped `(idx, iCreateInfo, iSeed, dwBuff)` tuples if `statusCode != 0`), `STASH_SAVE`, `GAME_JOIN`, `FLOOR_GATE`, `MONSTER_DROP`, `CHEST_DROP`, `ITEM_PICKUP`, and `ITEM_DROP`.
- **A9. Eternity Warden (`0x05xx`) & Private Guard Isolation (`dx-eternity-guard`, `Section 8`):**
  - [ ] Add `include/dx_eternity_guard.h` (5-function C API: `DxGuard_BuildAttestationHandshake`, `DxGuard_InitSessionCanary`, `DxGuard_SealPlayerStats`, `DxGuard_VerifyPlayerStatsCanary`, `DxGuard_AnswerWardenChallenge`), `Source/realm/dx_eternity_guard_stub.cpp`, and `CMakeLists.txt` `DX_ETERNITY_WITH_PRIVATE_GUARD` option.
  - [ ] Handle `OP_WARDEN_CHALLENGE_REQ` (`0x0501`) -> `OP_WARDEN_CHALLENGE_RESP` (`0x0502`) computing live RAM slice digests (`AllItemsList`, `UniqueItems`, `SpellsData`, `MonstersData`), Cheat Engine / debugger / breakpoint / `.text` / XOR canary anomaly flags, and `SDL_GetTicks64()` clock-skew timestamps.
- **A10. "Adjust for High Ping" Mode — Non-Blocking Async Oracle, Optimistic Pickup & Adaptive Turn-Buffer (`Section 9`, `Source/realm/realm_async_oracle.cpp`):**
  - [ ] Implement `RealmAsyncOracleQueue` (`QueueAsyncMonsterKill`, `QueueAsyncChestOpen`, `RequestOptimisticItemPickup` with immediate `IS_IGRAB` + `_iDelFlag` and rollback on race loss, `PollAsyncRealmResponses`, and `ComputeRecommendedTurnDelayTicks` scaling lockstep turn delay `1..6` ticks).

### Category B: Torment Difficulty (`DIFF_TORMENT`) Progression, Bosses & Loot (`Source/levels/trigs.cpp`, `Source/player.cpp`, `Source/monster.cpp`, `Source/items.cpp`, `Source/spells.cpp`)
- **B1. Traditional Town Shortcut Progression & Stairway Tooltip Fix:**
  - [ ] **Disable Town Shortcuts on Torment:** Require players on `DIFF_TORMENT` to unlock Catacombs, Caves, and Hell town shortcuts traditionally by reaching those floors in the dungeon (instead of starting unlocked in Single Player).
  - [ ] **Stairway Hover Tooltip Fix:** Fix hovering over the stairway to Level 14 (and any other dungeon/town warp triggers) displaying `"Difficulty: Hell"` (or misusing trigger/difficulty strings) when playing on Torment/other difficulties.
- **B2. Level Cap 60 (Post-50 XP Exclusive to Torment) & Monster `mlvl` Audit:**
  - [ ] **Level Cap 60:** Extend character maximum level from `50` (`MAXCHARLEVEL = 51`) to `60` (`ExpLvlsTbl` extended through Level 60), gating all experience gain past Level 50 strictly to `DIFF_TORMENT` (and syncing with Closed Realm Server-Vault validation).
  - [ ] **Torment Monster Level (`mlvl`) Audit:** Verify `monster.level(sgGameInitInfo.nDifficulty)` (`+30%` over Hell) is used consistently across all hit/dodge, block, XP reward, and item drop `mlvl` calculations on `DIFF_TORMENT` (no raw `MonstersData[].level` bypasses).
- **B3. Book of Apocalypse on Torment:**
  - [ ] Allow `Book of Apocalypse` (and learning/leveling `SPL_APOCALYPSE` via books) to drop/work exclusively on `DIFF_TORMENT`.
- **B4. `Shade of Diablo` Boss (`DIFF_TORMENT` Level 16):**
  - [ ] **Boss Entity & Visuals:** Create `Shade of Diablo` — identical stats/AI/animations to Diablo (`MT_DIABLO`), rendered with a dark grey palette/tint and **35% transparency**, with an independent **1% chance** (`1%` each) to drop Diablo's exclusive unique weapons (`Kadi's Fury`, `Apostate's Crook`, `Loxley's Bend`).
  - [ ] **25% Co-Spawn on Level 16 (Torment Only):** On `DIFF_TORMENT` Level 16 (`currlevel == 16`), roll a **25% chance** for `Shade of Diablo` to spawn alongside normal Diablo.

### Category C: Audio Engine — Dynamic 3D Tracking, EFX Reverb & HRTF (`Source/engine/sound.cpp`, `Source/effects.cpp`, `Source/engine/render/ground_fog.cpp`)
- **C1. Live Positional 3D Sound Tracking for Active/Looping Sounds:**
  - [ ] Fix active sounds losing 3D positional updates once started: continuously update OpenAL `AL_POSITION` / listener-relative pan & distance attenuation for playing sound sources (`effects.cpp` / `sound.cpp`) as the player walks away or changes direction.
- **C2. Wall Occlusion & EFX Reverb for `lavabubbles.wav` (`LS_LAVABUBBLES`):**
  - [ ] Apply line-of-sight wall occlusion (`AL_FILTER_LOWPASS`) and EFX environmental reverb to `lavabubbles.wav` (reusable for other non-panned ambient SFX in the future) **without** 3D stereo panning, so boiling lava is muffled through solid dungeon walls.
- **C3. Hell Reverb Intensity Rebalance:**
  - [ ] Reduce `DTYPE_HELL` EFX reverb **High** preset intensity by **25%**, and increase `DTYPE_HELL` **Normal** preset intensity by **20%**.
- **C4. OpenAL HRTF (`Headphone Mode`) 3D Sound & Positional Accuracy Fix:**
  - [ ] Fix enabling `Headphone Mode (HRTF)` disabling 3D positional sound, and tune HRTF spatial/azimuth accuracy.

### Category D: Visuals, Palettes & Class Identity (`Source/player.cpp`, `Source/engine/palette.cpp`, `Source/engine/render/weather.cpp`)
- **D1. Female Barbarian Distinct Color Palette (Black Hair, Grey Armor):**
  - [ ] Remap the Female Barbarian's in-game sprite/TRN color ramps (`Source/player.cpp` / palette translation) so she has **black hair** and **grey armor**, making her immediately distinguishable from the Rogue at a glance.
- **D2. Hell "Lava Wall" Very Dark Grey / Near-Black Palette Variant:**
  - [ ] Allow Hell (`DTYPE_HELL`) randomized palettes (`Source/engine/palette.cpp`) to roll a very dark charcoal-grey / near-black variant for the Hell lava-wall rock ramps (while keeping molten lava/blood river indices red) if not already in the preset pool.
- **D3. Tristram Weather Session Lock & Heavy Snow Speed Tuning (`Source/engine/render/weather.cpp`):**
  - [ ] **Lock Weather Type Per Game Session:** Fix weather type changing mid-game when re-entering Town — once a game session rolls its weather type (`Rain`, `Snow`, or `AshStorm`), lock that weather type for the entire session and only allow Light/Heavy intensity variants within that same weather family to transition.
  - [ ] **Slow Down Heavy Snow by 30%:** Reduce particle fall/drift velocity in Heavy Snow (`Blizzard` / `SnowHeavy`) by **30%**.
- **D4. Tristram Nighttime (`DTYPE_TOWN`):**
  - [ ] Selective Palette Manipulation + Enable Dungeon Lighting in Town (`DTYPE_TOWN`) for authentic nighttime Tristram illumination.

### Category E: 3D Isometric Volumetric Ground Fog (Paused at Step 2C — `Source/engine/render/ground_fog.cpp`)
- **E1. Step 2C (In Progress — Resume Point):**
  - [ ] Further optimize dual-sheet Ground Fog inner-loop FPS to eliminate the remaining frame-rate hit.
- **E2. Step 3:**
  - [ ] Eliminate the hard diagonal fog cutoff near dungeon walls.
- **E3. Step 4:**
  - [ ] Submerge entities and objects into the fog up to roughly player calf/knee height (`DrawGroundFogEntitySubmersion`) instead of standing on top of the fog like a solid floor.

### Category F: Final Phase (Deferred)
- **F1. Cut Content Restoration & Dynamic Lighting Pipeline**



