# DX:Eternity Closed Realm Server (`dx-eternity-realm`) — Development Log

**Project:** `dx-eternity-realm` (Closed Realm & Anti-Cheat Server for DevilutionX: Eternity)  
**Architecture:** Tier 1.5 Server-Vault + Server-Oracle Loot Dispensing + Real-Time Anomaly Detection  
**Target Binaries:** `misc/dx-eternity-realm` (Linux x86_64 static) & `misc/dx-eternity-realm.exe` (Windows x86_64 static)

---

## Session 1: Repository Initialization, Workspace Cleanup & Multi-Phase Roadmap

- **Workspace Cleanup:**
  - Removed legacy client-only source trees and assets from the active workspace while caching essential `DX:Eternity` shared data table references (`itemdat`, `monstdat`, `pack`, `playerdat`, `gendung`) in `/app/applet/.deps/client_ref/` for 1:1 binary and LCG parity.
  - Saved the architecture specification to `/CLOSED_REALM_ARCHITECTURE.md`.
- **Roadmap Established (`/ROADMAP.md`):**
  - Defined the 7-phase engineering plan covering:
    1. Core C++20 Server Skeleton, Static Build Toolchain (`libsodium`, `sqlite3`, `asio`, `fmt`) & Binary Protocol Framing
    2. Cryptographic Authentication (`Argon2id`) & Official Binary Attestation (`REALM_ATTESTATION_KEY`)
    3. Server-Vault Character & Stash Persistence (`PlayerPack`, 6 classes including Female Barbarian, `DIFF_TORMENT` unlock tracking)
    4. Deterministic 1:1 `DX:Eternity` LCG & `RecreateItem()` Verification Engine (including `CF_TORMENT` maxed item rolls and Hell/Torment Diablo exclusive uniques)
    5. Game Session Registry & Server-Oracle Loot Dispensing (`REQ_MONSTER_DROP`, `REQ_CHEST_DROP`, `REQ_PICKUP_ITEM`, `RealmSecretSalt`, Active Ground & Inventory Ledgers)
    6. Server-Side Anti-Cheat Anomaly Detection (Player DPS/Single-Hit Ceiling, TTK validation, Travel-Distance cadence, Floor Progression Gate, and XP/Stat/Gold/Item Provenance auditing on save)
    7. Automated End-to-End Verification Suite & Client Integration Specification

---

## Phase 1: Core C++20 Server Skeleton, Static Build Toolchain & Binary Protocol Framing (Completed & Verified)

### 1.1 Zero-Dependency Static Build Toolchain (`CMakeLists.txt`, `cmake/mingw-w64-x86_64.cmake`, `.deps/`)
- Cached SQLite 3.45.1 amalgamation (`sqlite3.c`/`sqlite3.h`) and `libsodium` 1.0.20 static archives in `/app/applet/.deps/linux/` and `/app/applet/.deps/win64/`.
- Built both zero-dependency static server executables directly into `/misc/`:
  - `misc/dx-eternity-realm` (`1.6 MB`, Linux x86_64 static)
  - `misc/dx-eternity-realm.exe` (`2.0 MB`, Windows 64-bit static with `-static -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread`)

### 1.2 Shared `DX:Eternity` Binary Structures & Wire Framing (`Server/protocol/realm_protocol.hpp`, `Server/net/packet_codec.hpp/.cpp`)
- Enforced exact 1:1 binary structure layout parity via `static_assert`:
  - `PkItemStruct` (`19` bytes)
  - `PlayerPack` (`1262` bytes, including `uint32_t pDiabloKillLevel`, `uint32_t pDifficulty`, `uint32_t pDamAcFlags`, `uint8_t bReserved2[16]`)
  - `TCmdGItem` (`30` bytes)
  - `RealmPacketHeader` (`32` bytes: `magic = 0x52455844`, `version = 1`, `opcode`, `payloadLength`, monotonic `sequenceNumber`, and 16-byte `libsodium` `crypto_generichash` BLAKE2b MAC).
- Implemented constant-time packet MAC verification (`sodium_memcmp` in `VerifyPacketMac`) and frame encoding (`EncodePacketFrame`).

### 1.3 Embedded SQLite3 WAL Database & Non-Blocking TCP Event Loop (`Server/db/realm_database.hpp/.cpp`, `Server/net/realm_server.hpp/.cpp`, `Server/main.cpp`)
- `RealmDatabase`: Bootstraps SQLite3 in `WAL` journal mode with tables `accounts`, `characters`, `stashes`, `item_ledger` (indexed by owner and item seed tuple), and `security_audit_log`.
- `RealmServer`: Cross-platform non-blocking TCP multiplexer (`poll` on Linux, `select` on Winsock2) with per-session ring buffers (`128 KB` cap), `120 pkt/s` rate limiting, monotonic sequence-number anti-replay checks, and `OP_PING`/`OP_PONG` + `OP_AUTH_CHALLENGE_REQ`/`OP_AUTH_CHALLENGE_RESP` dispatch.
- Verified end-to-end via `./misc/dx-eternity-realm --self-test` (including automatic detection and `security_audit_log` recording of tampered packet MACs).

---

## Phase 2: Cryptographic Authentication & Official Binary Attestation (Completed & Verified)

### 2.1 Official Client Binary Integrity Attestation (`Server/auth/realm_auth.hpp/.cpp`, `Server/net/realm_server.cpp`)
- Added `OP_ATTEST_HANDSHAKE_REQ` (`100` bytes) and `OP_ATTEST_HANDSHAKE_RESP` (`4` bytes) wire payloads to `Server/protocol/realm_protocol.hpp`.
- Implemented `ComputeClientAttestationHmac` and `VerifyAttestationHandshake` using `libsodium` `crypto_generichash` (BLAKE2b-256 keyed by 256-bit `REALM_ATTESTATION_KEY` over `serverNonce[32] || clientNonce[32] || clientTextSectionHash[32] || clientBuildVersionLE[4]`) verified in constant time via `sodium_memcmp`, plus optional exact `.text` hash enforcement.
- Upon successful attestation, both client and server rotate their packet MAC key from the bootstrap handshake key to a unique per-connection key via `DeriveSessionTrafficKey(REALM_ATTESTATION_KEY, serverNonce, clientNonce)`.
- Enforced strict attestation gating in `RealmServer::DispatchPacket`: any `OP_AUTH_REGISTER_REQ`, `OP_AUTH_LOGIN_REQ`, or `OP_AUTH_RESUME_REQ` sent prior to `session.attested == true` is rejected with `ERR_ATTESTATION_FAILED`, logged to `security_audit_log`, and disconnected.

### 2.2 Argon2id Account Persistence & 256-Bit Session Tokens (`Server/db/realm_database.hpp/.cpp`, `Server/auth/realm_auth.hpp/.cpp`)
- Implemented `CreateAccount` (inserting into `accounts` and initializing an empty `stashes` row in a transaction), `FindAccountByUsername` (`COLLATE NOCASE`), `FindAccountById`, `UpdateLastLogin`, and `SetAccountBanStatus` in `RealmDatabase`.
- Implemented `RegisterAccount` and `LoginAccount` in `RealmAuthManager` using `libsodium` `crypto_pwhash_str` / `crypto_pwhash_str_verify` (`Argon2id` interactive profile) and zeroing plaintext password buffers via `sodium_memzero`.
- Implemented 256-bit ephemeral session tokens (`IssueSessionToken`, `ResumeSession`, `RevokeSessionToken`) with 24-hour expiration and immediate ban enforcement.
- Rebuilt both static binaries (`misc/dx-eternity-realm` `1.7 MB` and `misc/dx-eternity-realm.exe` `2.1 MB`) and verified all Phase 1 & Phase 2 flows via `./misc/dx-eternity-realm --self-test`.

---

## Phase 3: Server-Vault Character & Stash Persistence (Completed & Verified)

### 3.1 Server-Vault Protocol & SQLite Persistence (`Server/protocol/realm_protocol.hpp`, `Server/db/realm_database.hpp/.cpp`)
- Added wire payloads for `OP_HERO_LIST_REQ`/`RESP` (`HeroSummaryInfo` 48B `_uiheroinfo` parity, up to `MaxHeroesPerAccount = 12`), `OP_HERO_CREATE_REQ`/`RESP`, `OP_HERO_DELETE_REQ`/`RESP`, `OP_HERO_LOAD_REQ`/`RESP`, `OP_HERO_SAVE_REQ`/`RESP`, and `OP_STASH_LOAD_REQ`/`RESP` + `OP_STASH_SAVE_REQ`/`RESP` (up to `MaxStashItems = 400`).
- Implemented `ListCharactersForAccount`, `CreateCharacter` (enforcing case-insensitive global hero name uniqueness and slot assignment), `DeleteCharacter` (cascading cleanup of `item_ledger`), `LoadCharacter`, `SaveCharacter`, `LoadStash`, `SaveStash`, and `RecordLedgerItem` / `CountLedgerItemsForCharacter`.

### 3.2 Authoritative `CreatePlayer` Parity & Save Snapshot Validation (`Server/vault/realm_vault.hpp/.cpp`, `Server/net/realm_server.cpp`)
- Embedded the exact `DX:Eternity` `PlayersData[6]` (`Warrior`, `Rogue`, `Sorcerer`, `Monk`, `Bard`, and Classic Female `Barbarian` with `Str=40, Mag=0, Dex=20, Vit=25, MaxStr=255, MaxVit=150`) and `ExpLvlsTbl[50]` tables.
- Implemented `BuildInitialPlayerPack` matching `CreatePlayer()` + `CreatePlrItems()` in `DX:Eternity` (`IDI_BARBARIAN = 139`, `IDI_WARRSHLD = 2`, `IDI_WARRCLUB = 3`, `IDI_SORCERER = 5` with `Firebolt` level 2, starting `100` gold at `InvGrid[30]`, and automatic registration of all starting equipment in `item_ledger`).
- Implemented `ValidatePlayerPackSnapshot` on `OP_HERO_SAVE_REQ` enforcing class attribute caps, stat-point conservation, `ExpLvlsTbl` thresholds, base HP/Mana formula ceilings (`CalculateHeroBaseLife` / `CalculateHeroBaseMana`), and `pDiabloKillLevel` (`0..4`) progression gates (requiring `existing.diabloKillLevel >= 3` before saving a Torment Diablo kill).
- Rebuilt both static binaries (`misc/dx-eternity-realm` `1.7 MB` and `misc/dx-eternity-realm.exe` `2.1 MB`) and verified all Phase 1–3 checks via `./misc/dx-eternity-realm --self-test`.

---

## Phase 4: Deterministic 1:1 `DX:Eternity` LCG & `RecreateItem()` Verification Engine (Completed & Verified)

### 4.1 Exact DevilutionX 1.5.4 / `DX:Eternity` Borland C++ LCG (`Server/game/lcg.hpp`)
- Implemented `LcgEngine` (`kMultiplier = 0x015A4E35U`, `kIncrement = 1U`, `SetRndSeed`, `GetLCGEngineState`, `GenerateSeed`, `AdvanceRndSeed`, `GenerateRnd`, `FlipCoin`, `DiscardRandomValues`) with bit-for-bit state transition parity against `Source/engine/random.cpp`.

### 4.2 Verbatim `DX:Eternity` Item, Affix, Spell & Monster Data Tables (`Server/game/item_tables.hpp/.cpp`)
- Embedded 1:1 `AllItemsList` (`170` entries), `ItemPrefixes`, `ItemSuffixes`, `UniqueItems` (`113` entries including `UITEM_KADISFURY = 110`, `UITEM_APOSTATESCROOK = 111`, and `UITEM_LOXLEYSBEND = 112`), `SpellsData` (`52` spells), `ServerMonstersData` (`138` monster types), and `ServerUniqueMonstersData` (`101` unique bosses).

### 4.3 Deterministic `RecreateServerItem()`, `CF_TORMENT` Verification & Vault Integration (`Server/game/item_verifier.hpp/.cpp`, `Server/vault/realm_vault.cpp`)
- Implemented the full `DX:Eternity` item generation and recreation pipeline (`RemapItemIdxFromDiablo`, `RemapItemIdxToDiablo`, `InitializeServerItem`, `GetItemAttrs`, `SaveItemPower`, `GetItemBonus`, `CheckUnique`, `GetUniqueItem`, `ItemRndDur`, `SetupAllItems`, `SetupAllUseful`, `SpawnUniqueServerItem`, `RecreateServerItem`, `RecreateTownItem`, `UnPackServerItem`, and `PackServerItem`).
- Enforced `DIFF_TORMENT` (`CF_TORMENT = 1U << 1` in `PkItemStruct::dwBuff`): rolls maximum possible values in `RndPL`, `iMaxAC`, `sStaffMax`, and `_iMaxDur` while still consuming every `lcg.GenerateRnd(...)` call so LCG state and prefix/suffix selection never desynchronize from normal rolls.
- Implemented `VerifyPackedItemIntegrity` (checking `IsCreationFlagComboValid`, `IsTownItemValid`, `IsShopPriceValid`, `IsUniqueMonsterItemValid`, `IsDungeonItemValid`, `IsHellfireSpellBookValid`, `IsServerItemValid`, quality bit `bId` parity, `bMDur == _iMaxDur`, `bMCh == _iMaxCharges`, and gold limits) and integrated it into `RealmVaultManager::ValidatePlayerPackSnapshot` (`InvBody`, `InvList`, `SpdList`, and inventory gold sum == `pGold`) and `RealmVaultManager::SaveStash`.
- Implemented `CalculatePlayerEquippedStats` (`ComputedPlayerCombatStats`) including Female Barbarian 1H two-handed sword/mace mastery and class damage modifiers for Phase 6 anti-cheat DPS auditing.
- Rebuilt both static binaries (`misc/dx-eternity-realm` and `misc/dx-eternity-realm.exe`) and verified all Phase 1–4 self-tests via `./misc/dx-eternity-realm --self-test`.

---

## Phase 5: Game Session Registry & Server-Oracle Loot Dispensing (Completed & Verified)

### 5.1 Closed Realm Game Lobby, `DIFF_TORMENT` Entry Gate & Floor Progression (`Server/session/realm_session.hpp/.cpp`, `Server/protocol/realm_protocol.hpp`)
- Added packed wire payloads for `OP_GAME_CREATE_REQ`/`RESP`, `OP_GAME_JOIN_REQ`/`RESP`, `OP_GAME_LIST_REQ`/`RESP`, `OP_FLOOR_TRANSITION_REQ`/`RESP`, `OP_MONSTER_DROP_REQ`/`RESP`, `OP_CHEST_DROP_REQ`/`RESP`, `OP_ITEM_PICKUP_REQ`/`RESP`, and `OP_ITEM_DROP_REQ`/`RESP`.
- Enforced server-side `DIFF_TORMENT` entry gate (`CheckDifficultyEntryGate`): requires `diabloKillLevel >= 3` (`ERR_TORMENT_LOCKED_HELL_REQUIRED`) and `heroLevel >= 35` in multiplayer (`ERR_TORMENT_LEVEL_35_REQUIRED`).
- Generated a 256-bit secret `realmSecretSalt` per `ActiveGameSession` via `libsodium` `randombytes_buf` that never leaves the server, paired with a public `terrainInitSeed` for dungeon layout geometry.
- Implemented `IsFloorTransitionValid` across all 25 floors (`0..24`), including `DX:Eternity` seamless dungeon branches (`Caves 12 <-> Hive 17` and `Hell 14 <-> Crypt 21`).

### 5.2 Server-Oracle Monster & Chest Loot Dispensing (`HandleMonsterDrop`, `HandleChestDrop`)
- Tracked per-floor killed monsters (`std::bitset<256>`) and opened containers (`std::bitset<128>`) per `(gameId, currlevel)`, rejecting replay/duplicate requests with `ERR_DUPLICATE_KILL_OR_CHEST`.
- Implemented `CalculateEffectiveMonsterStats` with `DIFF_TORMENT` `130%` of `DIFF_HELL` scaling (`* 130 / 100` for HP, damage, AC, To-Hit, level, and XP) and single-hit / TTK ceiling verification against `player->combatStats.maxSingleHitCeiling`.
- Implemented `RollOracleDungeonItem` using `BLAKE2b(realmSecretSalt, gameId, currlevel, entityIndex, tag)` and `LcgEngine`, enforcing classic Diablo loot filter rules (`IsClassicDroppableItemAvailable`), `CF_TORMENT` maxed item rolls, `+30%` max gold on `DIFF_TORMENT`, and independent `2%` rolls on Hell/Torment `MT_DIABLO` for `UITEM_KADISFURY`, `UITEM_APOSTATESCROOK`, and `UITEM_LOXLEYSBEND`.

### 5.3 Atomic Ground-to-Inventory Item Ledger (`TransitionLedgerItemState`, `HandleItemPickup`, `HandleItemDrop`)
- Added `TransitionLedgerItemState` and `HasCharacterLedgerProvenance` to `RealmDatabase`.
- `HandleItemPickup` atomically transitions dispensed items from `LedgerItemState::OnGround` to `LedgerItemState::InInventoryOrStash` and removes them from `ActiveGameSession::groundLedger`, rejecting race-condition or replayed pickup attempts with `ERR_ITEM_NOT_ON_GROUND`.

---

## Phase 6: Server-Side Anti-Cheat Anomaly Detection & Save Auditing (Completed & Verified)

### 6.1 Authoritative Player DPS, Spell & Single-Hit Ceiling Calculator (`Server/anticheat/anomaly_detector.hpp/.cpp`)
- Implemented `CalculateAuthoritativeCombatCeiling` combining `CalculatePlayerEquippedStats` physical/elemental weapon damage, attack speed flags (`QuickAttack`, `FastAttack`, `FasterAttack`, `FastestAttack`), `FastWalk` (`UITEM_KADISFURY`), and `EstimateMaxSpellHit` across `pSplLvl[1..36]`, `pSplLvl2[0..9]`, and `+spellLvlAdd` (`UITEM_APOSTATESCROOK`).

### 6.2 Multi-Hit Time-To-Kill (TTK) Floor & Travel-Distance Cadence Auditing (`AuditMonsterKillAnomaly`)
- Enforced single-hit ceiling (`req.maxSingleHitDealt <= ceiling.effectiveMaxSingleHit`) and multi-hit boss TTK floor (`minHitsRequired >= 3` requires `combatDurationMs >= (minHitsRequired - 1) * minAttackIntervalMs`), blocking instant 1-packet kills on high-HP monsters and bosses (`ERR_ANOMALY_TTK_VIOLATION`).
- Enforced 2-second sliding-window kill-rate cap (`kMaxKillsPerWindow = 24`) and Chebyshev tile travel-distance cadence checks (blocking cross-map teleport kill jumps when `!ceiling.hasTeleportAccess` with `ERR_ANOMALY_CADENCE_VIOLATION`).

### 6.3 Character Save Conservation & 100% Item Ledger Provenance / Anti-Clone Audit (`AuditCharacterSaveConservation`)
- Added `CountCharacterLedgerCopies` to `RealmDatabase`.
- Integrated `AuditCharacterSaveConservation` into `RealmVaultManager::SaveHero` (`OP_HERO_SAVE_REQ`), enforcing:
  1. Active-session **XP Conservation** (`xpGained <= verifiedExpGained + kQuestAndShrineXpAllowance`).
  2. **Stat Point Conservation** (`Str + Mag + Dex + Vit + pStatPts` vs level + elixir caps).
  3. Active-session **Gold Conservation** (`goldDelta <= verifiedGoldDropped + kVendorSaleAndStartGoldAllowance`).
  4. **100% Item Provenance & Anti-Clone Verification**: every dungeon equipment item in `InvBody` and `InvList` must pass `VerifyPackedItemIntegrity` (`RecreateServerItem()`) and its copy count in the incoming `PlayerPack` must not exceed `CountCharacterLedgerCopies` in `item_ledger`.

---

## Phase 7: Embedded C++20 Admin HTTP Control Interface, Verification Suite & Windows 64-Bit Static Build (Completed & Verified)

### 7.1 Embedded C++20 Admin HTTP Control Interface (`Server/admin/realm_admin_server.hpp/.cpp`)
- Implemented `RealmAdminServer` directly inside the native C++20 `dx-eternity-realm` binary on default `127.0.0.1:6113` (`--admin-port`, `--admin-bind`, `--admin-token`).
- Enforced constant-time `sodium_memcmp` Bearer token authentication, `HttpOnly; SameSite=Strict` BLAKE2b-signed session cookies (`dx_admin_session`), and `X-CSRF-Token` validation on state-mutating endpoints.
- Added administrative endpoints (`GET /`, `POST /api/admin/login`, `GET /api/admin/status`, `GET /api/admin/accounts`, `GET /api/admin/characters`, `GET /api/admin/audit`, `POST /api/admin/ban`, `POST /api/admin/unban`) backed by `RealmDatabase` queries (`ListAllAccounts`, `ListAllCharacters`, `ListRecentAuditLogs`, `GetDatabaseSummaryStats`).

### 7.2 Full-Stack Self-Tests & Client Integration Guide (`Server/main.cpp`, `instr-client-closed-realm.md`)
- Extended `--self-test` in `Server/main.cpp` to verify Phases 1 through 7 end-to-end: `DIFF_TORMENT` entry gate enforcement (`diabloKillLevel >= 3`, `level >= 35` in MP), seamless floor transitions (`Caves 12 -> Hive 17`, `Hell 14 -> Crypt 21`), Server-Oracle `OP_MONSTER_DROP_REQ` + `OP_ITEM_PICKUP_REQ` atomic ledger state transitions, anti-cheat rejection of 1-shot Torment Diablo kills and cloned inventory items on save, and live Admin HTTP API authentication and account ban/unban operations.
- Authored `instr-client-closed-realm.md` detailing the client-side integration hooks (`selconn.cpp`, `selhero.cpp`, `pfile.cpp`, `monster.cpp`, `objects.cpp`, `items.cpp`, and Rolling-XOR shadow memory canaries in `player.h`/`player.cpp`).
- Compiled and linked the zero-dependency Windows 64-bit static server executable (`misc/dx-eternity-realm.exe`) via `x86_64-w64-mingw32-g++` (`-static -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread`).

### 7.3 Post-Build Security & Vulnerability Audit Hardening
- **Admin Web Console Stored XSS & Clickjacking Prevention (`Server/admin/realm_admin_server.cpp`):** Added client-side HTML entity escaping (`esc()`) for all dynamic fields rendered in `.innerHTML` (`username`, `banReason`, `heroName`, `severity`, `category`, `details`) and added `Content-Security-Policy` + `X-Frame-Options: DENY` headers.
- **Admin HTTP Socket Timeout Protection (`Server/admin/realm_admin_server.cpp`):** Configured `250 ms` `SO_RCVTIMEO` and `SO_SNDTIMEO` socket timeouts on accepted admin HTTP connections so an idle/Slowloris TCP connection on port `6113` cannot stall the single-threaded game loop.
- **Drop-and-Pickup Ledger Cloning Fix (`Server/db/realm_database.hpp/.cpp`, `Server/session/realm_session.cpp`):** Added `RealmDatabase::TransitionOwnedItemToGround` and updated `HandleItemDrop` (`OP_ITEM_DROP_REQ`) so dropping an owned dungeon/starting item transitions its existing `InInventoryOrStash` row back to `OnGround` instead of inserting a duplicate row, and rejects unowned dungeon item drops.
- **Immediate Active Session Eviction on Ban (`Server/net/realm_server.hpp/.cpp`, `Server/admin/realm_admin_server.cpp`):** Added `RealmServer::DisconnectAccountSessions` and invoked it on `POST /api/admin/ban` so banned accounts are immediately evicted from active TCP sessions and games.
- **Per-Session Send Buffer & FD-Set Caps (`Server/net/realm_server.cpp`):** Enforced `MaxSendBufferBytes = 256 KB` and `MaxConcurrentSessions = 60` (matching Winsock `FD_SETSIZE` safe bounds).
- **Second-Pass Adversarial Game-Security Hardening (`Server/vault/realm_vault.cpp`, `Server/anticheat/anomaly_detector.cpp`, `Server/net/realm_server.cpp`, `Server/db/realm_database.cpp`):**
  1. **Stash Anti-Clone & Stash Gold Conservation:** Added `RealmDatabase::CountAccountLedgerCopies` and updated `SaveStash` (`OP_STASH_SAVE_REQ`) to enforce 100% `item_ledger` copy-count provenance across the account's stash and characters, plus stash gold delta conservation.
  2. **Drop + Save Rollback Dupe Fix:** Removed the legacy `priorSavedCount` fallback in `AuditCharacterSaveConservation` so dropping an item onto the ground (`OP_ITEM_DROP_REQ`) immediately revokes `InInventoryOrStash` save permission for that item instance.
  3. **Spell Level (`pSplLvl` / `pSplLvl2`), `pMemSpells` & Hand-Slot Mutual Exclusion:** Validated spell levels (`0..15`), blocked zero-magic `Barbarian` characters from learning spellbooks/Teleport, enforced two-handed (`ILOC_TWOHAND`) mutual exclusion across left/right hand slots, and restricted dual-wielding two non-shield weapons to `Bard` and `Barbarian`.
  4. **0-Kill Reconnect XP/Gold Allowance Farming Fix:** Tied active-session quest/shrine XP and vendor gold allowances in `AuditCharacterSaveConservation` to verified dungeon floor activity (`verifiedExpGained > 0`).
  5. **10-Second Pre-Auth Socket Timeout, Oldest-Unauth Eviction & Argon2id Rate-Limiting:** Enforced `PreAuthTimeoutMs = 10000 ms`, oldest-unauthenticated socket eviction when `MaxConcurrentSessions` is reached, one-shot challenge/attestation state transitions, non-zero `clientNonce` verification, and `MaxAuthAttemptsPerSession = 3` Argon2id rate limiting.
  6. **Continuous Travel-Distance Velocity Ceiling & Exact `IDI_GOLD` Stack Cap:** Replaced the fixed `elapsedMs < 200` teleport check in `AuditMonsterKillAnomaly` with a continuous velocity bound (`maxReachableTiles = 24 + (elapsedMs * ceiling.maxTilesPerSecond) / 1000`) and enforced `5,000` (`6,500` on `CF_TORMENT`) per-slot `IDI_GOLD` stack ceilings.
  7. **SQLite `item_ledger` Primary Key (`item_uid`) Fix:** Fixed `TransitionLedgerItemState` and `TransitionOwnedItemToGround` in `Server/db/realm_database.cpp` to query `WHERE item_uid = ?` matching the `item_ledger` primary key column, verified end-to-end by adversarial tests `6D`, `6E`, and `6F` (`security_audit_log rows=24`).
  8. **Hell Diablo Party Anti-Boost Safeguard (`Server/session/realm_session.hpp/.cpp`, `Server/anticheat/anomaly_detector.cpp`):** When `MT_DIABLO` is slain on `DIFF_HELL`, any party member whose level is more than `10` levels below the highest-level player in the game (`highestPartyLevel - playerLevel > 10`) does not receive Hell Diablo kill credit (`pDiabloKillLevel = 3`) unless their character level is `>= 32`. Enforced both on `OP_MONSTER_DROP_REQ` (`HELL_DIABLO_BOOST_BLOCKED`) and on `OP_HERO_SAVE_REQ` (`AuditCharacterSaveConservation`).
  9. **Real-Time Server & Client Plain-Text Diagnostic Logging (`dx-eternity-realm.log` & `dx-eternity-client-realm.log`):** Added `RealmDatabase::SetLogFilePath` (defaulting to `dx-eternity-realm.log` or `--log <path>`) so every server event, save rejection reason, and anti-cheat alert is written as a timestamped plain-text line alongside SQLite `security_audit_log`, and added a drop-in Closed Realm client logger (`RealmClientLog` in `instr-client-closed-realm.md` Section 6).
  10. **Semi-Automatic Linux Self-Updater (`Server/admin/realm_updater.hpp/.cpp`, `POST /api/admin/update`, `--update`) & `DXER_ATTESTATION_KEY_HEX` Override:** Added POSIX atomic self-update targeting `mrphantastik/dx-eternity-realm` (supporting `"git"` source pull+build mode, private/public GitHub release binary downloads with `GITHUB_TOKEN`, BLAKE2b-256 verification, pre-flight `--self-test` gate, `.bak` backup, atomic `rename()`, and in-place `execv()` restart) plus `DXER_ATTESTATION_KEY_HEX` environment variable override in `GetDefaultAttestationKey()`.
  11. **Admin Torment Unlocker & Single-Player / Multiplayer Character + Stash Importer (`Server/vault/realm_vault.hpp/.cpp`, `Server/admin/realm_admin_server.cpp`):** Added `AdminPromoteHeroToTorment` (`POST /api/admin/promote-torment`), `AdminImportCharacterPack` (`POST /api/admin/import-hero`), and `AdminImportStash` (`POST /api/admin/import-stash`) with automatic Single-Player `CF_PREGEN` (`0x8000`) flag conversion, `RecreateServerItem()` verification, and 100% SQLite `item_ledger` registration, verified by self-test `7D`.
  12. **Static Lobby Chat Channels (`#General`, `#Trade`, `#LFG`, `#Torment`), Whispers & Game Search (`Server/protocol/realm_protocol.hpp`, `Server/session/realm_session.cpp`, `Server/net/realm_server.cpp`, `Server/admin/realm_admin_server.cpp`):** Added `OP_CHAT_JOIN_CHANNEL_REQ`/`RESP` (`0x0401`/`0x0402`), `OP_CHAT_SEND_REQ`/`RESP` (`0x0403`/`0x0404`), and `OP_CHAT_POLL_REQ`/`RESP` (`0x0405`/`0x0406`) with server-verified sender badges (`heroName`, `heroClass`, `heroLevel`, `CHAT_FLAG_TORMENT_CROWN`), per-session flood rate-limiting, built-in `/w <hero> <msg>`, `/games [query]`, and `/who` slash commands, Admin Web Console live chat feed + `[REALM ADMIN]` broadcast (`GET/POST /api/admin/chat`), and filtered `OP_GAME_LIST_REQ` (`GameListFilterRequestPayload` matching `gameName`, player `heroName`, difficulty, full status, and password status), verified by self-test `7E`.
  13. **Eternity Warden (`0x05xx`) Runtime Memory-Table Slice Challenge, Cheat Engine / Breakpoint Flags & Speedhack Clock-Skew Auditor (`Server/protocol/realm_protocol.hpp`, `Server/anticheat/anomaly_detector.hpp/.cpp`, `Server/net/realm_server.hpp/.cpp`):** Added `OP_WARDEN_CHALLENGE_REQ` (`0x0501`), `OP_WARDEN_CHALLENGE_RESP` (`0x0502`), and `OP_WARDEN_VERDICT_RESP` (`0x0503`), `GenerateWardenChallenge`, cross-platform `ComputeWardenSliceDigest` (keyed BLAKE2b-256 over `wardenNonce[16]` + live `AllItemsList`, `UniqueItems`, `SpellsData`, or `ServerMonstersData` entries), and `VerifyWardenResponse` (blocking tampered RAM tables, Cheat Engine DLL / `DR0..DR3` hardware breakpoint / Rolling-XOR canary flags with `ERR_WARDEN_INTEGRITY_VIOLATION = 29`, and `>1.20x` Speedhack clock-skew with `ERR_WARDEN_SPEEDHACK_DETECTED = 30`), verified by self-test `7F`.

