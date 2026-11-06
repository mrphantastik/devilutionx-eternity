# DX:Eternity

**DX:Eternity** is a curated gameplay, atmospheric, and audiovisual overhaul built on top of the [DevilutionX](https://github.com/diasurgical/devilutionX) engine.

The primary goal of this project is to expand vanilla Diablo with fresh depth, variety, and modern acoustic fidelity while fiercely protecting the classic Diablo I pace, dark-gothic tone, and itemization curve. Rather than bolting on disjointed mechanics, DX:Eternity integrates cut and expansion content seamlessly into the core Tristram descent.

---

## Key Features

### 🎧 3D Spatial Audio, EFX Dungeon Reverb & Binaural HRTF
* **OpenAL Soft + EFX Environmental Reverb:** Every dungeon tier features custom acoustic reverberation profiles (`AL_EFFECT_EAXREVERB`) - from subtle open-air reflections in Tristram to cavernous stone echoes in the Cathedral, suffocating damp crypts, roaring magma caves, booming abyssal halls in Hell, and organic resonance in the Hive.
* **Dynamic Wall Occlusion:** Sound sources originating behind solid dungeon geometry are dynamically muffled using EFX low-pass filtering while preserving open-air corridor travel.
* **Surround Sound & Headphone HRTF:** Native 3D positional audio with full support for multichannel speaker layouts and binaural HRTF profiles (configurable directly from the in-game Audio Settings menu).

---

### 🗺️ Seamless Dungeon Branches: Hive & Crypt
*Hellfire's* dungeons are woven directly into the natural labyrinth descent:
* **The Hive (Levels 17–20):** Accessible via a downward stairway on **Caves Level 12**.
* **The Crypt (Levels 21–24):** Accessible via a descent discovered on **Hell Level 14**.
* **Classic Itemization:** Both branches utilize pure vanilla Diablo I drop and affix tables.
* **Atmospheric Enemy Reskinning:** Out-of-place and cartoonish *Hellfire* sprites have been replaced with gritty, dark-fantasy D1 counterparts (including Magma Demons, Counselors, Advocates, Drakes, and Blood Knights) while retaining their aggressive projectile and elemental hazard behaviors.

---

### ⚔️ New Class Archetype: The Female Barbarian
Pairs the Rogue’s visuals, portraits, and voice lines with brutal frontline melee mechanics:
* **Attributes:** High Strength and Vitality caps with zero Magic affinity.
* **Mastery & Traits:** Full two-handed weapon proficiency (wielding two-handed swords, clubs, and axes with one-handed shield blocking mechanics), innate stun/knockback advantages, and the active **Rage** skill.

---

### 🏆 Hell Difficulty Endgame Drops
Slaying Diablo on **Hell difficulty** yields an independent **2% (1-in-50) drop chance** for each of three build-defining, class-tailored unique artifacts:

| Item | Base | Stats & Mechanics |
| :--- | :--- | :--- |
| **Kadi's Fury** | Broad Sword | `+100% To Hit`, `+175% Damage`, `Fastest Attack`, `+4–16 Lightning Damage`, `Faster Walk`, `Resist All: -30%` |
| **Apostate's Crook** | War Staff | `+2 to All Spell Levels`, `+40 to Magic`, `+80 to Maximum Mana`, `Damage Reduced by 8`, `-50 to Armor Class`, `Resist All: -30%` |
| **Loxley's Bend** | Long War Bow | `Knocks Target Back`, `+40 to Dexterity`, `+30 to Maximum Damage`, `Steals 5% Mana (works on ranged hits)`, `-40% Light Radius`, `-50 to Maximum Life` |

---

## 🛠️ Other Gameplay & Polish Changes

* **Restored Footstep Sounds:** Restores randomized walking sound effects originally commented out in the 1996 Blizzard North source code.
* **Streamlined Hive Pacing:** Silences the repetitive Defiler monologue pop-ups upon entering the Hive.
* **Persistent Sessions:** Slaying Diablo drops full boss loot and leaves the game session open, allowing you to clean up unfinished quests or explore secondary dungeon branches without forcing an immediate game over.
* **Single-Player Town Shortcuts:** Town entrance portals to the Catacombs, Caves, and Hell are unlocked from the start of a Single Player campaign for fluid session routing.

---

## 📋 Roadmap / To-Do

* [ ] **Dynamic Weather & Biome Visual FX:** Area-specific atmospheric effects (such as rain, ambient fog, drifting ash, and cavern embers).
* [ ] **Torment Difficulty:** An endgame difficulty tier scaled beyond Hell for max-level characters and high-end equipment progression.
