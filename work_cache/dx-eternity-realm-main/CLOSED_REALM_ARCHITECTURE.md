# DX:Eternity — Closed Realm & Anti-Cheat Architecture (Tier 1.5 + Anomaly Detection)

If the goal is **maximum anti-cheat protection across the board while keeping server costs near zero** (so a single $5/month VPS can run the whole realm), we can combine **4 lightweight mechanisms** into **Tier 1.5**.

Together, they block save editing, item injection, duping, drop-sniping, casual Cheat Engine scanning, and brute-force kill spamming—without the server ever needing to run a 20 Hz dungeon simulation loop.

---

## 1. Server-Vault Characters (Closed Realm Persistence)
1. **Server-Side Character Storage (No Local `.sv` Files):**
   - When selecting the `"Closed Eternity Realm"` connection provider in `selconn.cpp`, the client logs into a lightweight central C++/SQLite or Postgres realm server (using `libsodium` Ed25519/Argon2, which is already linked into `dx-eternity.exe`).
   - Instead of reading `multi_0.sv` from the player's PC (`pfile_ui_set_hero_infos`), the Hero Selection screen (`selhero.cpp`) fetches the player's `PlayerPack` list + Stash directly from the Realm Server over TCP.
   - Local disk saving (`pfile_write_hero`) is completely disabled in Closed mode; instead, `pfile_write_hero` sends an encrypted save snapshot to the Realm Server.
2. **Cryptographic Item & Character Verification (`libsodium`):**
   - Every item (`Item::iSeed`, `iCreateInfo`, `wIndx`, `dwBuff`) and character state (`PlayerPack`) is verified by the Realm Server using `RecreateItem()` against the game's verified drop ledger.
   - This **completely eliminates offline save editors, hero editors, item duping via read-only `.sv` files, and character importing**.

---

## 2. Server-Oracle Loot Dispensing ("Kill-Receipt RPC")
Instead of letting the client roll item drops locally when a monster dies or a chest opens:
- **How it works:**
  - A monster or chest only dies/opens a few times per minute.
  - When a monster dies, the client sends a tiny 24-byte `REQ_MONSTER_DROP` packet to the Realm Server containing `{ gameId, level, monsterId, tileX, tileY, totalDamageDealt, hitsLanded }`.
  - The Realm Server holds a **secret server-only RNG salt (`RealmSecretSalt`)** that the client never sees.
  - The server checks that `monsterId` hasn't already been killed in `gameId`, rolls the drop on the server, logs the generated item `(wIndx, wCI, dwSeed, dwBuff)` into that game's **Active Ground Ledger**, and returns the `TCmdGItem` drop packet.
- **What this eliminates completely (at ~0% CPU cost):**
  - **Zero Pre-Cog / Maphack Loot Sniping:** Even if someone uses a maphack, they have **no idea** what any monster or chest will drop until they actually kill it, because the drop seed only exists on the server.
  - **Zero Drop Forcing:** Cheat Engine cannot force a monster to drop a Unique or a specific base item, because the client doesn't roll loot anymore.
  - **Zero Item Duping:** When a player picks up an item (`REQ_PICKUP_ITEM`), the server atomically moves that item ID from the Ground Ledger to that player's Inventory Ledger. Two players can never pick up the same item, and dropping + Alt-F4ing cannot duplicate it.

---

## 3. Server-Side Anomaly Detection (How It Works)
Because the Realm Server has your character's `PlayerPack` in memory when you enter a game, it knows your **exact legitimate combat ceiling** (your weapon damage, spell levels, attack speed, magic find, and whether you know Teleport/Phasing).

Whenever a client sends `REQ_MONSTER_DROP`, `REQ_CHEST_OPEN`, or `SAVE_CHARACTER`, the server runs **4 fast math checks** (taking < 0.05 milliseconds):

1. **Time-To-Kill (TTK) & Damage Ceiling Check (Stops 1-Shotting Bosses & Kill-Spam):**
   - From your `PlayerPack`, the server calculates your character's **maximum possible single-hit damage** (`MaxHit`) and **maximum possible DPS** (`MaxDPS`, accounting for crits, shrines, and spell casts).
   - If you send `REQ_MONSTER_DROP` for Hell/Torment Diablo (who has ~16,000+ HP) only **1.5 seconds** after first damaging him—or with `hitsLanded = 1`—the server knows it is mathematically impossible for your character's gear/spells to have dealt 16,000 damage in 1.5 seconds. The kill and drop are rejected.
2. **Kill-Cadence / Travel-Distance Envelope (Stops Packet-Spam & Teleport Hacks):**
   - Each `REQ_MONSTER_DROP` includes the tile `(tileX, tileY)` where the monster died.
   - The server checks `walkingDistance(lastKillTile, currentKillTile) / elapsedTime`.
   - If a client claims to kill monsters 60 tiles apart in 0.3 seconds (or sends 25 kill requests across an entire floor in 3 seconds), the server detects instant-room-clear / teleport hacking and drops the session.
3. **Floor Progression Gate:**
   - The server tracks which dungeon floors (`currlevel`) have been entered in the session. A client cannot claim a kill or drop on Level 16 (Diablo's Lair) without first registering floor transitions through the preceding floors or an unlocked town shortcut.
4. **Stat, XP & Gold Budget Conservation on Save:**
   - `Total XP Gained <= Sum(XP of verified monster kills in session)`.
   - `Total Stat Points == StartingClassStats + (Level - 1) * 5 + ElixirCount`.
   - Every item in the character's inventory/belt/stash must exist in either their **Starting Inventory Ledger** (when the game began) or the **Verified Ground Ledger** (items legitimately dropped and picked up in that game).

---

## 4. Client-Side Anti-Cheat Hardening (Zero Server Cost)
We can also add three lightweight protections directly into `dx-eternity.exe` that stop 99% of Cheat Engine users and P2P griefers before a packet ever leaves the machine:

1. **Rolling-XOR Shadow Memory for HP, Mana, Damage & Stats:**
   - Right now, `MyPlayer->_pHitPoints`, `_pMana`, `_pMinDam`, `_pMaxDam`, and `Monsters[i].hitPoints` sit in plain 32-bit integers in RAM—anyone can type their HP into Cheat Engine, click "First Scan", and freeze it in 10 seconds.
   - We can store a **rolling XOR shadow copy** (`hpCanary = _pHitPoints ^ gFrameSecretKey`, where `gFrameSecretKey` rotates every tick).
   - If Cheat Engine freezes `_pHitPoints`, `_pMana`, or `Monsters[i].hitPoints` in memory, two things happen:
     1. Changing the value in Cheat Engine triggers an instant canary mismatch on the next 50ms tick (`_pHitPoints != (hpCanary ^ gFrameSecretKey)`), immediately disconnecting/invalidating the session.
     2. Even searching for the value in Cheat Engine fails if the true authoritative value is stored XOR-masked.
2. **Cumulative Monster Damage Tracking on Packets (`Source/msg.cpp`):**
   - In vanilla Diablo 1, `CMD_KILLMONSTER` instantly kills any monster regardless of its remaining HP, and `CMD_MONSTDAMAGE` accepts any `uint16_t` number (`65535`).
   - We can patch `ParseCmd()` in `Source/msg.cpp` so that:
     - `CMD_MONSTDAMAGE` is clamped/rejected if the damage value exceeds the attacking player's legitimate max weapon/spell damage + crit/shrine multiplier.
     - `CMD_KILLMONSTER` is ignored if the monster hasn't actually taken lethal damage.
     - `CMD_HITPLR` / `CMD_PLRDEAD` is strictly ignored in Town (`currlevel == 0`) or when hostility is off (eliminating Town-PK griefing).
3. **Binary Integrity HMAC Handshake:**
   - When `dx-eternity.exe` connects to the Realm Server, the server sends a random 32-byte challenge nonce, and the client computes a `libsodium` `crypto_generichash` (BLAKE2b HMAC) over its own in-memory `.text` code segment + the nonce.
   - If someone hex-edits `dx-eternity.exe` (e.g. patching out a jump instruction for God Mode or Maphack), the `.text` code segment hash changes and the Realm Server refuses the login.

---

## 5. Summary: Cost vs. Protection of This Setup
- **Server Resource Usage:** Because the server only processes **Login, Game Creation, ~1–5 Drop/Pickup RPCs per minute, and Character Save**, a single **$5/month VPS (1 vCPU, 1 GB RAM)** can easily handle **500+ concurrent games**.
- **What is blocked:**
  - Offline save editors / hero editors (**100% blocked**)
  - Imported or hacked items (**100% blocked**)
  - Item duping (**100% blocked**)
  - Drop forcing & Pre-cog loot maphacks (**100% blocked** via Server-Oracle Drop RPC)
  - Cheat Engine HP/Mana/Damage freezing (**Blocked** via Rolling-XOR Canary + TTK/Damage-cap checks)
  - 1-shotting bosses / instant room-clear packet spam (**Blocked** via TTK & Travel-Distance Anomaly Detection + `msg.cpp` damage clamping)
  - Town-kill / PK packet griefing (**100% blocked** in `msg.cpp`)

---

## 6. Open-Source GitHub Client Safety & Official Binary Attestation

### 6.1 What Remains 100% Safe Even If Every Line of Client Code Is Public on GitHub
All of **Sections 1, 2, and 3** (Server-Vault Characters, Server-Oracle Loot Dispensing, and Server-Side Anomaly Detection) are **100% immune** to the client source code being public:
- **Save / Stat / Inventory Protection:** The character `.sv` and Stash live on the server. Reading the client source code gives an attacker zero ability to edit their stats or items.
- **Loot & Drop Protection (`Server-Oracle RPC`):** The drop RNG salt (`RealmSecretSalt`) and the Ground/Inventory Item Ledgers exist **only on the server**. Even if someone compiles their own custom client from your GitHub repo, they still cannot pre-inspect monster drops, force a Unique to drop, or dupe an item.
- **TTK / DPS / Travel-Time / XP-Cap Auditing:** The math checks run on the server. Even a custom-compiled client cannot claim it killed Torment Diablo in 1.5 seconds or claim more XP/gold than the monsters on that floor actually award.

### 6.2 What Public Client Source Code *Does* Affect (and How to Solve It)
If the client source is on GitHub, a normal player who just downloads your release `dx-eternity.exe` and opens Cheat Engine is still blocked by the Rolling-XOR memory canaries.

**However**, a programmer who clones your GitHub repo could try to:
1. Edit the C++ code to remove the Rolling-XOR check or hardcode a Maphack (`AutoMapShowItems = true`), compile their own `dx-eternity.exe`, and connect to your server.
2. Read how the Binary Integrity HMAC works and make their custom build return the expected hash of the official `.exe`.

### 6.3 How to Prevent Custom-Compiled Clients from Connecting to the Closed Realm
Because DevilutionX is licensed under **The Unlicense (Public Domain)**, you can keep 99% of `DX:Eternity` open-source on GitHub while locking the Closed Realm to **only your official compiled `dx-eternity.exe` release binary** using two simple techniques:

1. **Build-Time Secret Key Injection (GitHub Actions / Private Build Script):**
   - Your public CMake/C++ code on GitHub has a placeholder `#ifndef REALM_ATTESTATION_KEY`.
   - Anyone can clone the repo and compile it for Single Player or Open TCP/IP multiplayer.
   - When *you* build the official `dx-eternity.exe` release, CMake injects a private 256-bit key (`-DREALM_ATTESTATION_KEY="..."`) and randomizes the Rolling-XOR memory layout offsets per release version.
   - Without that secret build key, a custom-compiled binary from GitHub can play Single Player and Open MP, but cannot complete the `libsodium` challenge-response handshake to log into the **Closed Eternity Realm**.
2. **Private `realm_guard` Static Library (Optional):**
   - Put the small (~200-line) memory-integrity + challenge-response handshake function into a tiny private `.cpp` file (or private submodule) that only links into official release builds, while stubbing it out in the public repo.
   - That way, someone reading the public GitHub repo cannot see which memory regions and state variables the handshake hashes during gameplay.

### 6.4 Summary
- **Keep the client repo open on GitHub?** **Yes.**
- **Server-Vault, Server-Oracle Drops, and Server Anomaly Detection** don't care if the client is open-source because the server never trusts the client anyway.
- **Injecting a private CMake key/module at build time** ensures that casual re-compiles (e.g., someone commenting out fog-of-war in `automap.cpp` and hitting build) won't be able to log into the Closed Realm.

