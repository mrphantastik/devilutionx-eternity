# DX:Eternity — Feature & Implementation Roadmap

**Base Engine:** DevilutionX 1.5.4 (`x86_64-w64-mingw32`, `-O3 -DNDEBUG -flto` Release)

---

## Completed Phases

- [x] **Phase 1 — Quality of Life & Endless Single Player Loop**
  - [x] Catacombs (L5), Caves (L9), and Hell (L13) town shortcuts open from session start in Single Player (`Source/levels/trigs.cpp`).
  - [x] Endless Single Player session after Diablo's death (no ending cutscene freeze) + Diablo boss loot drop (`Source/monster.cpp`, `Source/items.cpp`).
- [x] **Phase 2 — Custom Unique Weapon (`Kadi's Fury`)**
  - [x] `UITEM_KADISFURY = 110` (`UITYPE_BROADSWR`): `+100% To Hit`, `+175% Damage`, `Fastest Attack`, `4–16 Lightning Damage`, `Faster Walk` (dungeons only), `-30% All Resistances` (`Source/itemdat.h`, `Source/itemdat.cpp`, `Source/items.h`, `Source/items.cpp`).
  - [x] Full classic Diablo `.sv` save/load persistence for `Kadi's Fury` (`Source/loadsave.cpp`).
  - [x] Disabled `FastWalk` bonus in Town so `Run in Town` remains controllable (`Source/player.cpp`).
- [x] **Phase 3 — Nest & Crypt Dungeon Integration, Save/Load & Audio Optimization**
  - [x] Secondary down-staircase on Caves L12 -> Nest L17 and return up-staircase L17 -> L12 (`Source/levels/drlg_l3.cpp`, `Source/levels/trigs.cpp`).
  - [x] Secondary down-staircase on Hell L14 -> Crypt L21 and return up-staircase L21 -> L14 (`Source/levels/drlg_l4.cpp`, `Source/levels/trigs.cpp`).
  - [x] Save/load persistence inside Nest (L17–20) and Crypt (L21–24) for classic `.sv` saves (`Source/loadsave.cpp`, `Source/pfile.cpp`).
  - [x] Multi-tick animation SFX fix, Balrog Inferno & Lightning segment audio throttling, `DuplicateSound` stream pooling, and `-O3 -DNDEBUG -flto` Release build (`Source/player.cpp`, `Source/monster.cpp`, `Source/missiles.cpp`, `Source/effects.cpp`, `Source/engine/sound.cpp`).
- [x] **Phase 4 — Pure Classic Diablo Loot Filter & Hell Diablo ~2% `Kadi's Fury` Drop**
  - [x] ~2% (`1-in-50`) `Kadi's Fury` drop roll from Hell Diablo (`MT_DIABLO` on `DIFF_HELL`), excluded from normal world unique rolls (`Source/monster.cpp`, `Source/items.cpp`).
  - [x] Blocked Hellfire oils, runes, Torn Notes, Cathedral Map, and Hellfire-only base items in classic mode (`Source/items.cpp`, `Source/monster.cpp`).

---

## Upcoming Phases

- [ ] **Phase 5 — Female Barbarian Class (Rogue Visual & Audio Assets)**
  - [ ] Unlock `HeroClass::Barbarian` in hero creation/selection (`Source/DiabloUI/selhero.cpp`, `Source/pfile.cpp`).
  - [ ] Retain Barbarian stats, combat traits, and innate resistance mechanics while mapping sprites (`"Rogue"`), UI portrait, and voice/SFX to Rogue's female assets (`Source/player.cpp`, `Source/pfile.cpp`).
- [ ] **Phase 6 — Nest (Hive) Monster Roster Swap & Defiler Monologue Suppression**
  - [ ] Suppress the Defiler's quest pop-up monologues (`USFX_DEFILER1..4` / `TEXT_DEFILER1..4`) on Hive levels 17 and 19 (`Source/player.cpp` — `PlayDungMsgs()`).
  - [ ] Replace `MT_UNRAV` (*The Shredded*) with `"Illusion Weaver"` (`sneak\sneak`, `MonsterAIID::Sneak`, `dLvl 17–18`) and replace Nest monster slots (`MT_HELLBOAR` through `MT_HORKDMN` on `dLvl 17–20`, keeping `MT_DEFILER` as Level 20 boss) with classic Diablo 1 monsters (`Source/monstdat.cpp`).
- [ ] **Phase 7 — Discrete 7.1 Surround Sound & Per-Dungeon EAX Reverb**
  - [ ] 8-channel 7.1 surround output (`FL, FR, FC, LFE, BL, BR, SL, SR`) with radial spatial panning, `<110Hz` LFE subwoofer crossover, and per-dungeon EAX reverb DSP integrated into the `-O3` pooled audio pipeline (`Source/engine/sound.cpp`, `Source/options.cpp`, `Source/gamemenu.cpp`, `SDL_audiolib`).
