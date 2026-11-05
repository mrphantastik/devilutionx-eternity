# DX:Eternity

**DX:Eternity** is a curated gameplay and atmosphere overhaul built on top of [DevilutionX](https://github.com/diasurgical/DevilutionX).

The primary purpose of this project is to **add audio-visual and gameplay variety to vanilla DevilutionX**—expanding classic *Diablo* with new dungeon branches, class archetypes, boss-exclusive unique weapons, and restored unused dark-fantasy monsters without breaking the classic *Diablo I* tone and itemization.

---

## Features

### Seamless Endgame & Dungeon Exploration
- **Endless Single Player Loop:** Defeating Diablo no longer forces the game session to end; he drops full boss loot on death so you can continue exploring and farming.
- **Open Town Shortcuts:** Catacombs, Caves, and Hell town entrances are open from the start of a Single Player game.
- **Integrated Hive & Crypt Branches:** Discover a second downward staircase on **Caves Level 12** leading into the 4-level **Hive** (Levels 17–20) and a second downward staircase on **Hell Level 14** leading into the 4-level **Crypt** (Levels 21–24), all using pure classic *Diablo* loot tables.

### Female Barbarian Class
- Play as a **Female Barbarian** in character creation, pairing the Rogue's classic visual sprites, portraits, and voice lines with the Barbarian's heavy melee mechanics:
  - High Strength and Vitality scaling with zero Magic affinity.
  - Innate two-handed weapon mastery (wielding two-handed axes, clubs, and swords with one-handed block bonuses), innate stun/knockback advantages, and the **Rage** skill.

### Restored Dark-Fantasy Bestiary
- **Unused Vanilla Monsters Restored:** Brings back unused original *Diablo* monster models and palettes across the Hive and Crypt—including **Spider Lords**, **Torchants**, **Bone Demons**, **Devil Kin Brutes**, **Invisible Lords**, and **Satyr Lords**.
- **Tone-Consistent Monster Roster:** Replaces jarring insectoid and out-of-place *Hellfire* sprites in the Hive and Crypt with classic dark-fantasy counterparts (such as Magma Demons, Counselors, Advocates, Drakes, and Blood Knights) while preserving dangerous ranged projectiles and elemental hazards.
- **Clean Immersion:** Removes repetitive Defiler pop-up monologues in the Hive.

### Hell Diablo Boss-Exclusive Unique Weapons
Defeating **Diablo on Hell difficulty** grants an independent **2% (1-in-50)** chance to drop each of three build-defining unique weapons:

- **Kadi's Fury** (*Broad Sword*)
  - `+100% To Hit`, `+175% Damage`, `Fastest Attack`, `4–16 Lightning Damage`, `Faster Walk` (in dungeons), `Resist All: -30%`
- **Apostate's Crook** (*War Staff*)
  - `+2 to All Spell Levels`, `+40 to Magic`, `+80 to Maximum Mana`, `Damage Reduced by 8`, `-50 to Armor Class`, `Resist All: -30%`
- **Loxley's Bend** (*Long War Bow*)
  - `Knocks Target Back`, `+40 to Dexterity`, `+30 to Maximum Damage`, `Steals 5% Mana` (works on bow attacks), `-40% Light Radius`, `-50 to Maximum Life`

### 3D Spatial Audio, EFX Dungeon Reverb & Binaural HRTF
- **OpenAL Soft + EFX Environmental Reverb:** Every area features tailored acoustic reverberation (`AL_EFFECT_EAXREVERB`)—from subtle open-air reflections in Tristram to Gothic stone echoes in the Cathedral, claustrophobic tombs in the Catacombs, cavernous Caves, booming abyssal halls in Hell, damp organic Hive tunnels, and resonant Crypt vaults.
- **Dynamic Wall Occlusion:** Sounds originating behind solid dungeon walls are dynamically muffled via EFX low-pass filtering while preserving distant corridor reverberation.
- **Surround Sound & Binaural HRTF:** Full 3D positional audio supporting multichannel surround speakers and binaural headphone HRTF (configurable in the in-game Audio Settings menu).

---

## Installation

1. Copy `dx-eternity.exe` into your DevilutionX / *Diablo + Hellfire* folder containing `DIABDAT.MPQ` and the expansion `.mpq` archives (`hellfire.mpq`, `hfmonk.mpq`, `hfbard.mpq`, `hfbarb.mpq`, `hfmusic.mpq`, `hfvoice.mpq`).
2. Run `dx-eternity.exe`.
