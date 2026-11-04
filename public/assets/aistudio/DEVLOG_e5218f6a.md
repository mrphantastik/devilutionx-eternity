# DX:Eternity (DevilutionX 1.5.4) — Engineering & Patch DevLog

**Base Engine:** `diasurgical/devilutionX` @ tag `1.5.4` (`9f30593b4c2e706d6d6a66edfbc3aa6ada8861b4`)  
**Target Architecture:** Windows 64-bit (`x86_64-w64-mingw32` / `dx-eternity.exe`)  
**Workflow Rule:** Pure C++ source tree modifications (`Source/*.cpp`, `Source/*.h`) built via CMake + Ninja (`x86_64-w64-mingw32-g++`) and standard `git diff` unified patches. Zero binary inspection or hex patching.

---

## Verified Baseline (Phases 1–5)
- **Phase 1 — NM/Hell Town Shortcuts & SP Diablo Loot (`Source/levels/trigs.cpp`, `Source/monster.cpp`, `Source/items.cpp`):**
  - Opens Catacombs, Caves, and Hell town warps immediately on all difficulties in Single Player (`IsWarpOpen()`).
  - Keeps Single Player session active when Diablo dies (`gbProcessPlayers = true`), plays his full 140-tick death agony + `DiabloApocalypseBoom` disintegration instead of triggering the ending movie, unlocks next difficulty, and drops boss loot (`uper = 15`).
- **Phase 2 — Kadi's Fury / Claw Unique Broad Sword & Walk Speed (`Source/itemdat.h`, `Source/items.h`, `Source/itemdat.cpp`, `Source/items.cpp`, `Source/player.cpp`, `Source/monster.cpp`):**
  - Adds `ItemSpecialEffectHf::FastWalk`, `IPL_FASTWALK`, and `UITEM_KADISFURY = 110` (`UITYPE_BROADSWR`).
  - Stats: `+100% To Hit`, `+175% Damage`, `Fastest Attack`, `4–16 Lightning Hit Damage`, `Faster Walk` (`skippedFrames += 3` in `StartWalkAnimation()`), `-30% Resist All` (unclamped `IPL_ALLRES`).
- **Phase 3 — Extended Mode & Save/Stash Isolation (`Source/DiabloUI/mainmenu.cpp`, `Source/DiabloUI/selhero.cpp`, `Source/init.cpp`, `Source/pfile.cpp`):**
  - Adds `"Extended"` campaign mode to the Main Menu.
  - Locks character classes to Warrior, Rogue, and Sorcerer while mounting `hellfire.mpq` for Nest/Crypt level assets (`giNumberOfLevels = 25`).
  - Isolates save files (`ext_single_*.esv`) and stash (`stash_ext.esv`).
- **Phase 4 — Nest & Crypt Staircases + Pure Diablo Loot (`Source/levels/drlg_l3.cpp`, `Source/levels/drlg_l4.cpp`, `Source/levels/trigs.cpp`, `Source/items.cpp`, `Source/monster.cpp`):**
  - Connects Caves Level 12 (`dPiece == 548`) to The Nest Level 17 (with return stairs `L17 -> L12`).
  - Connects Hell Level 14 (`dPiece == 421`) to The Crypt Level 21 (with return stairs `L21 -> L14`).
  - Blocks Hellfire oils (`IMISC_OILFIRST..IMISC_OILLAST`), runes (`IMISC_RUNEFIRST..IMISC_RUNELAST`), and Hellfire-only base items (`i > IDI_LASTDIABLO`) in `IsItemAvailable()`.
- **Phase 5 — Discrete 7.1 Surround Sound & Per-Dungeon EAX Reverb (`Source/engine/sound.cpp`, `Source/gamemenu.cpp`, `Source/options.cpp`, `SDL_audiolib`):**
  - 8-channel 7.1 surround output (`FL, FR, FC, LFE, BL, BR, SL, SR`) with radial spatial panning, `<110Hz` LFE subwoofer crossover, and per-dungeon EAX reverb DSP.

---

## Phase 6 — Incremental Step-by-Step Log

### Step 6.1 — Remove 100% Kadi's Fury Drop from The Butcher (`MT_CLEAVER`)
- **Status:** Complete
- **File Modified:** `Source/monster.cpp` (`SpawnLoot()`)
- **Summary:** Removed the `MT_CLEAVER` guaranteed `UITEM_KADISFURY` drop block in `SpawnLoot()`. The Butcher only drops his standard `UITEM_CLEAVER` via `SpawnItem()`.

### Step 6.2 — Replace `MT_UNRAV` ("The Shredded") in `Source/monstdat.cpp`
- **Status:** Complete & Compiled into `DX-Eternity-Phase6-Win64.zip`
- **File Modified:** `Source/monstdat.cpp` (line 130)
- **Root Cause & Fix:**
  - All Nest monster replacements at lines 145–156 (`MT_HELLBOAR` through `MT_HORKDMN`, preserving `MT_DEFILER` on Level 20) were already in place, except `MT_UNRAV` (*The Shredded*, `"unrav\\unrav"`, `"newsfx\\shred"`, `dLvl 17–18`), which sits earlier in `MonstersData[]` at **line 130** (between `MT_RBLACK` and `MT_HOLOWONE`).
  - Replaced `MT_UNRAV` at line 130 with the original Diablo 1 `"Illusion Weaver"` (`"sneak\\sneak"`, `"sneak\\sneakv1"`, `width = 128`, `image = 992`, `hasSpecial = true`, `hasTrans = false`, `frames = { 16, 8, 12, 8, 24, 15 }`, `rate = { 2, 1, 1, 1, 1, 1 }`, `MonsterAIID::Sneak`, `MFLAG_HIDDEN | MFLAG_SEARCH`) while retaining its Nest spawn levels (`minDLvl = 17, maxDLvl = 18`).

```diff
diff --git a/Source/monstdat.cpp b/Source/monstdat.cpp
--- a/Source/monstdat.cpp
+++ b/Source/monstdat.cpp
@@ -127,7 +127,7 @@ const MonsterData MonstersData[] = {
 	/* MT_RTBLACK */ { P_("monster", "Doom Guard"),              "black\\black",     nullptr,          "black\\blkkntrt",   MonsterAvailability::Retail,    160,   2120, false,       false,            {  8,  8, 16,  4, 24,  0 }, { 2, 1, 1, 1, 1, 1 },         13,         15,     26,               165,               165, MonsterAIID::SkeletonMelee,  MFLAG_SEARCH,                                                     0,    130,             8,         18,         25,             0,                    0,                 0,                 0,          75, MonsterClass::Demon,  RESIST_MAGIC | RESIST_FIRE,                                  RESIST_MAGIC | IMMUNE_FIRE,                                               7, 0,                    3650 },
 	/* MT_BTBLACK */ { P_("monster", "Steel Lord"),              "black\\black",     nullptr,          "black\\blkkntbt",   MonsterAvailability::Retail,    160,   2120, false,       false,            {  8,  8, 16,  4, 24,  0 }, { 2, 1, 1, 1, 1, 1 },         14,         16,     28,               180,               180, MonsterAIID::SkeletonMelee,  MFLAG_SEARCH,                                                     1,    120,             8,         20,         30,             0,                    0,                 0,                 0,          80, MonsterClass::Demon,  RESIST_MAGIC | IMMUNE_FIRE | RESIST_LIGHTNING,               IMMUNE_MAGIC | IMMUNE_FIRE | RESIST_LIGHTNING,                            7, 0,                    4252 },
 	/* MT_RBLACK  */ { P_("monster", "Blood Knight"),            "black\\black",     nullptr,          "black\\blkkntbe",   MonsterAvailability::Retail,    160,   2120, false,       false,            {  8,  8, 16,  4, 24,  0 }, { 2, 1, 1, 1, 1, 1 },         13,         14,     30,               200,               200, MonsterAIID::SkeletonMelee,  MFLAG_SEARCH,                                                     1,    130,             8,         25,         35,             0,                    0,                 0,                 0,          85, MonsterClass::Demon,  IMMUNE_MAGIC | RESIST_FIRE | IMMUNE_LIGHTNING,               IMMUNE_MAGIC | RESIST_FIRE | IMMUNE_LIGHTNING,                            7, 0,                    5130 },
-	/* MT_UNRAV   */ { P_("monster", "The Shredded"),            "unrav\\unrav",     "newsfx\\shred",  nullptr,             MonsterAvailability::Retail,     96,    484, false,       false,            { 10, 10, 12,  5, 16,  0 }, { 1, 1, 1, 1, 1, 1 },         17,         18,     23,                70,                90, MonsterAIID::SkeletonMelee,  0,                                                                0,     75,             7,          4,         12,             0,                    0,                 0,                 0,          65, MonsterClass::Undead, RESIST_FIRE | RESIST_LIGHTNING,                              RESIST_FIRE | RESIST_LIGHTNING,                                           3, 0,                     900 },
+	/* MT_UNRAV   */ { P_("monster", "Illusion Weaver"),         "sneak\\sneak",     nullptr,          "sneak\\sneakv1",    MonsterAvailability::Retail,    128,    992, true,        false,            { 16,  8, 12,  8, 24, 15 }, { 2, 1, 1, 1, 1, 1 },         17,         18,     23,                70,                90, MonsterAIID::Sneak,          MFLAG_HIDDEN |                   MFLAG_SEARCH,                    3,     75,             8,         16,         24,             0,                    0,                 0,                 0,          65, MonsterClass::Demon,  RESIST_FIRE | RESIST_LIGHTNING,                              RESIST_FIRE | RESIST_LIGHTNING,                                           3, 0,                     900 },
```
