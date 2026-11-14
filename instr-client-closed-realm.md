# DX:Eternity Closed Realm Client Integration Guide (`instr-client-closed-realm.md`)

**Target Server:** `dx-eternity-realm` / `dx-eternity-realm.exe` (`ProtocolMagic = 0x52455844` `"DXER"`, `ProtocolVersion = 1`)  
**Target Client:** DevilutionX: Eternity (`Beta v0.7d+`, DevilutionX 1.5.4 fork)

---

## 1. Shared Protocol Header & Cryptography (`Source/net/realm_client.h/.cpp`)

Copy `Server/protocol/realm_protocol.hpp` and `Server/net/packet_codec.hpp/.cpp` into the client repository (`Source/net/`). Both sides share identical `#pragma pack(push, 1)` binary layouts:
- `RealmPacketHeader` (`32` bytes: `magic`, `version`, `opcode`, `payloadLength`, `sequenceNumber`, `mac[16]`)
- `PkItemStruct` (`19` bytes)
- `PlayerPack` (`1262` bytes)
- `TCmdGItem` (`30` bytes)
- `HeroSummaryInfo` (`48` bytes)

### Handshake, Binary Attestation & Account Login Sequence (`selconn.cpp`)
1. Connect via TCP to the Realm Server (`DefaultRealmPort = 6112`).
2. Send `OP_AUTH_CHALLENGE_REQ` (`0x0101`, `seq = 1`) signed with `GetDefaultHandshakeKey()`.
3. Receive `OP_AUTH_CHALLENGE_RESP` (`0x0102`) containing `serverNonce[32]`.
4. Compute the `.text` segment BLAKE2b-256 digest (`clientTextSectionHash[32]`) of the running `dx-eternity.exe` image and compute `attestationHmac[32]` via `ComputeClientAttestationHmac(REALM_ATTESTATION_KEY, serverNonce, clientNonce, clientTextSectionHash, 0x00000704U)`.
5. Send `OP_ATTEST_HANDSHAKE_REQ` (`0x0106`, `seq = 2`) and verify `OP_ATTEST_HANDSHAKE_RESP` (`statusCode == RealmStatus::OK`).
6. Immediately rotate the session packet MAC key to `DeriveSessionTrafficKey(REALM_ATTESTATION_KEY, serverNonce, clientNonce)`.
7. **Account Login / Registration UI Dialog:**
   - After selecting **"Closed Realm (DX:Eternity)"** in `selconn.cpp`, show a simple DiabloUI dialog with **Username** (`3..24` chars: alphanumeric, `_`, `-`), **Password** (`8..63` chars), and two buttons: **[Login]** and **[Create Account]**:
     - **[Create Account]** sends `OP_AUTH_REGISTER_REQ` (`0x0103`, `AuthCredentialsRequestPayload`). The server hashes the password with `libsodium` `Argon2id`, creates the account + stash rows in SQLite, and returns `OP_AUTH_RESULT` (`0x0105`) with `statusCode == RealmStatus::OK`, `accountId`, and a 256-bit `sessionToken[32]`.
     - **[Login]** sends `OP_AUTH_LOGIN_REQ` (`0x0104`, `AuthCredentialsRequestPayload`) and returns `OP_AUTH_RESULT` (`0x0105`) with `accountId` and `sessionToken[32]`.
     - **Session Resume (Optional):** Cache `(accountId, sessionToken)` in memory while the game is running so returning to the lobby can silently re-authenticate via `OP_AUTH_RESUME_REQ` (`0x0108`) without re-prompting for the password.

---

## 2. Server-Vault Character Persistence & One-Way Local `.sv` Backup Mirror

When `gbIsClosedRealm == true` in `Source/pfile.cpp` and `Source/DiabloUI/selhero.cpp`:
- **Never Read from Local `.sv` Files in Closed Realm:** Closed Realm must **never** read `multi_*.sv` or `stash.sv` from local disk when listing or loading heroes (otherwise offline edits could be uploaded to the realm).
- **One-Way Local `.sv` Backup Mirror (Safe Against Server Shutdown):**
  - Whenever the client receives a verified character snapshot from the server (`OP_HERO_CREATE_RESP`, `OP_HERO_LOAD_RESP`) or completes a verified save (`OP_HERO_SAVE_RESP` with `statusCode == RealmStatus::OK`), the client **should also write a local `.sv` backup copy** to the user's save directory using standard `pfile_write_hero()`.
  - Because Closed Realm is strictly **Read-From-Server / Write-To-Both (Server + Local `.sv` Mirror)**, local `.sv` tampering can never affect Closed Realm, **and** players never lose their characters if they play offline/Open mode or if the Realm Server ever shuts down.
- **Hero Roster (`pfile_ui_set_hero_infos`):**
  - Send `OP_HERO_LIST_REQ` (`0x0201`) -> receive `OP_HERO_LIST_RESP` (`HeroListResponsePayload`).
  - Map each `HeroSummaryInfo` (`charId`, `name`, `heroClass` `0..5` including `HeroClass::Barbarian = 5`, `level`, `diabloKillLevel` `0..4`, `baseStr`, `baseMag`, `baseDex`, `baseVit`) directly to `_uiheroinfo` for the 4-skull difficulty badge and `DIFF_TORMENT` unlock check.
- **Hero Creation (`pfile_ui_save_create`):**
  - Send `OP_HERO_CREATE_REQ` (`0x0203`) -> server generates the authoritative `PlayerPack` and records the starting equipment in SQLite `item_ledger`.
- **Hero Load & Save (`pfile_read_player_from_save` / `pfile_write_hero`):**
  - Load via `OP_HERO_LOAD_REQ` (`0x0207`) -> unpack `HeroLoadResponsePayload::pack` via `UnPackPlayer` (and write one-way local `.sv` backup).
  - Save via `OP_HERO_SAVE_REQ` (`0x0209`) -> pack via `PackPlayer` and transmit `HeroSaveRequestPayload`. When `OP_HERO_SAVE_RESP` returns `RealmStatus::OK`, also update the local `.sv` backup copy.
- **Shared Account Stash (1-Page Limit + Physical `<= 5,000g` Stacks + `300,000g` Hard Cap):**
  - Load via `OP_STASH_LOAD_REQ` (`0x020B`) and save via `OP_STASH_SAVE_REQ` (`0x020D`).
  - **1-Page Stash Limit (`MaxStashItems = 100`):** The server strictly enforces a single `10x10` stash page (`itemCount <= 100`). Any attempt to save `> 100` items (multi-page stash) is rejected with `ERR_SAVE_AUDIT_FAILED`.
  - **Traditional Physical Gold Stacks (`1 .. 5,000g` per `IDI_GOLD` item):** Gold in both Hero Inventory (`40` slots, max `200,000g`) and Stash must be stored physically as `IDI_GOLD` (`PkItemStruct`) stacks with `1 <= wValue <= 5000` (`GOLD_MAX_LIMIT`).
  - **`300,000g` Stash Gold Hard Limit (`MaxSinglePageStashGold = 300000U`):** The sum of all physical `IDI_GOLD` stacks inside the 1-page Stash cannot exceed `300,000` gold (at most `60` stacks of `5,000g`). Virtual/unbacked `StashDataHeader::stashGold` is forbidden (`stashGold` in `StashDataHeader` must be `0` or equal the exact sum of the physical `IDI_GOLD` items in the stash page).

### Client Fix Note (`Source/items.cpp` line 2486)
In `IsItemAvailable(int i)`, when `!gbIsHellfire`, whitelist `IsAnyOf(i, IDI_SORCERER_DIABLO, IDI_BARBARIAN)` before the trailing `&& i <= 160` check so `RemapItemIdxFromDiablo(5) -> IDI_SORCERER_DIABLO (166)` is not flagged during `UnPackItem` when `bIsHellfire == 0`.

### 2.1 Exporting Existing Single-Player (`single_*.sv`) or Multiplayer (`multi_*.sv`) Heroes & Stash for Admin Import
To let the Realm Admin import an existing Single-Player or Multiplayer character + Stash via the Admin Web Console (`http://127.0.0.1:6113` -> **Admin Character (SP / MP .pack) & Stash (.stash) Importer**):
- In `Source/pfile.cpp` (when loading a local SP or MP hero in offline/open mode, or via a `--dump-packs` launch flag), write the decrypted `1262`-byte `PlayerPack` to `<PrefPath>/<HeroName>.pack`:
  ```cpp
  if (std::FILE *fp = std::fopen((paths::PrefPath() + std::string(pkplr.pName) + ".pack").c_str(), "wb")) {
      std::fwrite(&pkplr, sizeof(PlayerPack), 1, fp);
      std::fclose(fp);
  }
  ```
- In `Source/stash.cpp` (inside `SaveStash()`), write `StashDataHeader` (`8` bytes: `uint16_t statusCode=0`, `uint16_t itemCount`, `uint32_t stashGold`) followed by each `PkItemStruct` (`19` bytes each) to `<PrefPath>/exported.stash`.
- In the Admin Web Console (`http://127.0.0.1:6113`), select `<HeroName>.pack` and `exported.stash` and click **Import SP/MP Hero (.pack)** / **Import Stash (.stash)**. The server automatically strips Single-Player `CF_PREGEN` (`0x8000`) flags, verifies every item via `RecreateServerItem()`, and registers 100% of the items in SQLite `item_ledger`!

### 2.2 Drop-In C++ Implementation for 1-Page Stash, Physical `5,000g` Gold Stacks & `300,000g` Cap (`Source/qol/stash.cpp`)
In Closed Realm mode (`gbIsClosedRealm == true`), disable multi-page stash navigation and virtual gold deposit/withdraw buttons so gold is stored traditionally as physical `IDI_GOLD` (`<= 5,000g`) piles inside the single `10x10` stash page (capped at `300,000g` total):
```cpp
// Inside Source/qol/stash.cpp:
constexpr uint32_t kMaxClosedRealmStashGold = 300000U; // 300k hard cap (max 60 x 5,000g stacks)
constexpr int kMaxClosedRealmStashPages = 1;           // 1 page (10x10 = max 100 1x1 items)

uint32_t GetPhysicalStashGoldTotal()
{
	uint32_t total = 0;
	for (const Item &item : Stash.stashList) {
		if (!item.isEmpty() && item._itype == ItemType::Gold) {
			total += static_cast<uint32_t>(std::clamp(item._ivalue, 1, GOLD_MAX_LIMIT));
		}
	}
	return total;
}

bool CanPlaceItemInClosedRealmStash(const Item &candidateItem)
{
	if (!gbIsClosedRealm)
		return true;

	if (Stash.stashList.size() >= devilution::realm::MaxStashItems) {
		NetSendCmdString(1 << MyPlayerId, "Stash is full (1-page limit: 100 items max).");
		return false;
	}

	if (candidateItem._itype == ItemType::Gold) {
		if (candidateItem._ivalue < 1 || candidateItem._ivalue > GOLD_MAX_LIMIT) {
			return false;
		}
		const uint32_t newGoldTotal = GetPhysicalStashGoldTotal() + static_cast<uint32_t>(candidateItem._ivalue);
		if (newGoldTotal > kMaxClosedRealmStashGold) {
			NetSendCmdString(1 << MyPlayerId, "Stash cannot hold more than 300,000 gold.");
			return false;
		}
	}
	return true;
}
```
When serializing `OP_STASH_SAVE_REQ` (`0x020D`), set `StashDataHeader::stashGold = GetPhysicalStashGoldTotal()` (or `0`; the server verifies and persists the exact sum of physical `IDI_GOLD` stacks `<= 300,000g`).

---

## 3. Game Lobby, Speed Cap (`Default/Max = Fast 30Hz`), Search/Filtering, Static Chat Channels (`#General`, `#Trade`, `#LFG`, `#Torment`), `DIFF_TORMENT` Gate & Floor Progression (`selgame.cpp` / `msg.cpp` / `gendung.cpp`)

- **Closed Realm Game Speed Rules (`Default = Fast`, `Max = Fast`, `Min = Normal`):**
  - On Closed Realm, the **default** and **maximum** game speed is **`Fast` (`30` ticks/sec, `DefaultClosedRealmTickRate = 30`, `MaxClosedRealmTickRate = 30`)**.
  - Players can select a **lower** speed (**`Normal` = `20` ticks/sec, `MinClosedRealmTickRate = 20`**), but **never higher** (`Faster = 40` and `Fastest = 50` are disabled in the UI and rejected by the server with `ERR_MALFORMED_PACKET` / `GAME_SPEED_REJECTED`).
  - In `Source/DiabloUI/selgame.cpp`:
    - When `gbIsClosedRealm == true`, only populate **"Fast (Default)" (`30`)** and **"Normal" (`20`)** in the game speed selector, defaulting to **`Fast` (`30`)**.
    - Set `GameCreateRequestPayload::tickRate = sgGameInitInfo.nTickRate` (`30` or `20`; if `0` is sent, the server automatically defaults to `30` `Fast`).
    - On `OP_GAME_CREATE_RESP` and `OP_GAME_JOIN_RESP`, apply `sgGameInitInfo.nTickRate = resp.tickRate` (`20..30`).
- **Game Creation & Join (`OP_GAME_CREATE_REQ` / `OP_GAME_JOIN_REQ`):**
  - Pass `charId`, `gameName`, `password`, `difficulty` (`DIFF_NORMAL = 0 .. DIFF_TORMENT = 3`), `maxPlayers` (`1..4`), `bIsHellfire = 1`, and `tickRate` (`30` default `Fast`, or `20` `Normal`).
  - Server enforces `diabloKillLevel >= 3` (`ERR_TORMENT_LOCKED_HELL_REQUIRED`) and `level >= 35` in multiplayer (`ERR_TORMENT_LEVEL_35_REQUIRED`).
  - Use `GameCreateOrJoinResponsePayload::terrainInitSeed` to seed dungeon geometry layout (`glSeedTbl`), while monster/chest loot rolls are delegated to the server oracle.
- **Game Search & Filtered Lobby Listing (`OP_GAME_LIST_REQ = 0x0305` / `OP_GAME_LIST_RESP = 0x0306`):**
  - Send `OP_GAME_LIST_REQ` with either a `0`-byte payload (lists all active games) or a 36-byte `GameListFilterRequestPayload`:
    - `searchQuery[32]`: Case-insensitive substring filter matching either `gameName` OR any player's `heroName` currently inside the game.
    - `difficultyFilter`: `0..3` for a specific `_difficulty` (`Normal`, `Nightmare`, `Hell`, `Torment`), or `0xFF` for all difficulties.
    - `hideFullGames`: `1` to only show joinable games (`currentPlayers < maxPlayers`).
    - `hidePasswordedGames`: `1` to only show public games without passwords.
- **Static Lobby Chat Channels (`#General = 0`, `#Trade = 1`, `#LFG = 2`, `#Torment = 3`):**
  - **Switch Channel (`OP_CHAT_JOIN_CHANNEL_REQ = 0x0401` -> `OP_CHAT_JOIN_CHANNEL_RESP = 0x0402`):**
    - Pass `{ charId, channelId }` (`16` bytes). Returns `ChatChannelFeedResponsePayload` containing the active user count in that channel and the last `16` messages.
  - **Send Message or Slash Command (`OP_CHAT_SEND_REQ = 0x0403` -> `OP_CHAT_SEND_RESP = 0x0404`):**
    - Pass `{ channelId, text[160] }` (`168` bytes). The server automatically stamps the sender's verified `heroName`, `heroClass`, `heroLevel`, and `CHAT_FLAG_TORMENT_CROWN` (`diabloKillLevel >= 3`) from the database so clients can never spoof identity or badges.
    - Built-in server slash commands:
      - `/w <HeroName> <message>` — Sends a private whisper directly to `<HeroName>` across any channel or game.
      - `/games [search]` — Returns an instant `[LOBBY]` summary of open games matching `[search]`.
      - `/who` — Displays active game and hero counts.
  - **Poll Live Channel & Whisper Feed (`OP_CHAT_POLL_REQ = 0x0405` -> `OP_CHAT_POLL_RESP = 0x0406`):**
    - Poll every `500..1000ms` while in the pre-game lobby with `{ afterMessageId, channelId }` to receive any new channel messages and private whispers with `messageId > afterMessageId`.

### 3.1 Drop-In Client C++ Implementation for Game Search & Static Lobby Chat (`Source/net/realm_client.cpp`)
```cpp
// 1. Search / Filter Active Games in the Lobby (`OP_GAME_LIST_REQ = 0x0305`)
bool RealmSearchGames(
    const char *searchQuery,      // "" for all, or substring matching gameName OR player heroName
    uint8_t difficultyFilter,     // 0..3 (Normal..Torment) or 0xFF for Any difficulty
    bool hideFullGames,           // true = only games with open slots
    bool hidePasswordedGames,     // true = only public games
    devilution::realm::GameListResponsePayload &outLobbyList)
{
	using namespace devilution::realm;
	GameListFilterRequestPayload filter {};
	if (searchQuery != nullptr) {
		std::snprintf(filter.searchQuery, sizeof(filter.searchQuery), "%s", searchQuery);
	}
	filter.difficultyFilter = difficultyFilter;
	filter.hideFullGames = hideFullGames ? 1 : 0;
	filter.hidePasswordedGames = hidePasswordedGames ? 1 : 0;

	return SendRealmRpc(
	    RealmOpcode::OP_GAME_LIST_REQ, &filter, sizeof(filter),
	    RealmOpcode::OP_GAME_LIST_RESP, outLobbyList);
}

// 2. Join Static Lobby Chat Channel (`#General=0`, `#Trade=1`, `#LFG=2`, `#Torment=3`)
bool RealmJoinChatChannel(
    uint64_t activeCharId,
    devilution::realm::RealmChatChannel channel,
    uint64_t &inOutLastMessageId,
    devilution::realm::ChatChannelFeedResponsePayload &outFeed)
{
	using namespace devilution::realm;
	ChatJoinChannelRequestPayload req {};
	req.charId = activeCharId;
	req.channelId = static_cast<uint8_t>(channel);

	if (!SendRealmRpc(
	        RealmOpcode::OP_CHAT_JOIN_CHANNEL_REQ, &req, sizeof(req),
	        RealmOpcode::OP_CHAT_JOIN_CHANNEL_RESP, outFeed)) {
		return false;
	}
	inOutLastMessageId = outFeed.latestMessageId;
	return outFeed.statusCode == static_cast<uint16_t>(RealmStatus::OK);
}

// 3. Send Lobby Chat Message or Slash Command (`/w <Hero> <msg>`, `/games [query]`, `/who`)
bool RealmSendChatMessage(
    devilution::realm::RealmChatChannel channel,
    const char *text,
    devilution::realm::ChatSendResponsePayload &outResp)
{
	using namespace devilution::realm;
	ChatSendRequestPayload req {};
	req.channelId = static_cast<uint8_t>(channel);
	std::snprintf(req.text, sizeof(req.text), "%s", text != nullptr ? text : "");

	return SendRealmRpc(
	    RealmOpcode::OP_CHAT_SEND_REQ, &req, sizeof(req),
	    RealmOpcode::OP_CHAT_SEND_RESP, outResp);
}

// 4. Poll Incremental Channel Messages & Private Whispers (`OP_CHAT_POLL_REQ = 0x0405`)
bool RealmPollChatFeed(
    devilution::realm::RealmChatChannel channel,
    uint64_t &inOutLastMessageId,
    devilution::realm::ChatChannelFeedResponsePayload &outFeed)
{
	using namespace devilution::realm;
	ChatPollRequestPayload req {};
	req.channelId = static_cast<uint8_t>(channel);
	req.afterMessageId = inOutLastMessageId;

	if (!SendRealmRpc(
	        RealmOpcode::OP_CHAT_POLL_REQ, &req, sizeof(req),
	        RealmOpcode::OP_CHAT_POLL_RESP, outFeed)) {
		return false;
	}
	if (outFeed.latestMessageId > inOutLastMessageId) {
		inOutLastMessageId = outFeed.latestMessageId;
	}
	return outFeed.statusCode == static_cast<uint16_t>(RealmStatus::OK);
}
```

- **Floor Progression (`OP_FLOOR_TRANSITION_REQ`):**
  - Send `OP_FLOOR_TRANSITION_REQ` (`0x0307`) whenever the player traverses stairs, town portals, or branch entrances (`Caves 12 <-> Hive 17`, `Hell 14 <-> Crypt 21`).

---

## 4. Server-Oracle Loot Dispensing & Atomic Pickup (`monster.cpp`, `objects.cpp`, `items.cpp`)

- **Suppress Pre-Spawned Loose Floor Items on Level Load (`InitItems` in `Source/items.cpp`):**
  - When `gbIsClosedRealm == true`, skip random loose floor item generation inside `InitItems()` when a dungeon level loads.
  - *Why:* Any item spawned locally from `glSeedTbl` during level initialization would be visible in RAM immediately upon entering the floor (enabling pre-cog maphacks) and would lack a server-issued `itemUid` in `item_ledger` (causing `OP_ITEM_PICKUP_REQ` and `OP_HERO_SAVE_REQ` provenance checks to reject it). All dungeon loot in Closed Realm mode must come from `OP_MONSTER_DROP_REQ` or `OP_CHEST_DROP_REQ`.
  - Note: Local `Monster[i].mRndSeed` and `Object[i]._oRndSeed` values in client RAM are completely ignored by the server when rolling Closed Realm drops; the server derives all drop seeds on-demand using its secret 256-bit `realmSecretSalt`.
- **Monster Kill (`M_StartKill` in `Source/monster.cpp`):**
  - In Closed Realm mode, suppress local `SpawnItem` / `SpawnUnique`.
  - Send `OP_MONSTER_DROP_REQ` (`0x0310`) with `(gameId, currlevel, tileX, tileY, isUniqueMonster, monsterIndex, monsterTypeOrUid, totalDamageDealt, maxSingleHitDealt, combatDurationMs)`.
  - On `OP_MONSTER_DROP_RESP` (`0x0311`), instantiate each returned `OracleDroppedItemEntry::dropCmd` (`TCmdGItem`, up to 4 items for Hell/Torment Diablo's `UITEM_KADISFURY`, `UITEM_APOSTATESCROOK`, `UITEM_LOXLEYSBEND`) onto the dungeon floor and store `entry.itemUid` on the ground item slot.
  - **Hell Diablo Anti-Boost Safeguard (`MT_DIABLO` on `DIFF_HELL`):**
    - When `MT_DIABLO` dies on `DIFF_HELL` (`gnDifficulty == DIFF_HELL`), check the highest player level in the current game (`highestPartyLevel = max(Players[p]._pLevel)` across active players):
    - If `(highestPartyLevel - MyPlayer->_pLevel) > 10` **and** `MyPlayer->_pLevel < 32`, **do not** increment `MyPlayer->pDiabloKillLevel` to `3` (and display a net message: *"Your level is too far below the party to receive Hell Diablo kill credit"*).
    - The Realm Server enforces this identical rule in `HandleMonsterDrop` and `AuditCharacterSaveConservation` (`OP_HERO_SAVE_REQ` will reject `pDiabloKillLevel >= 3` with `ERR_SAVE_AUDIT_FAILED` if a player below level 32 is boosted by a party member more than 10 levels higher).
- **Chest / Barrel / Weapon Rack / Shrine Open (`Source/objects.cpp`):**
  - Suppress local item spawning in `OperateChest`, `BreakBarrel`, `OperateWeaponRack`, `OperateArmorStand`, `OperateBookcase`, etc.
  - Send `OP_CHEST_DROP_REQ` (`0x0312`) and spawn the returned `OracleDroppedItemEntry` items with their server-assigned `itemUid`.
- **Atomic Ground Item Pickup (`AutoGetItem` / `InvGetItem` in `Source/items.cpp`):**
  - Send `OP_ITEM_PICKUP_REQ` (`0x0314`) with `(gameId, itemUid, currlevel, tileX, tileY, iSeed, idx, iCreateInfo)`.
  - Only place the item into the player's inventory when `OP_ITEM_PICKUP_RESP` (`0x0315`) returns `RealmStatus::OK`.
- **Dropping an Owned Item to the Ground (`InvPutItem` / `DropItemBeforeTrig` in `Source/items.cpp`):**
  - When a player drops an item from their inventory/hand onto the ground, send `OP_ITEM_DROP_REQ` (`0x0316`) with `(gameId, currlevel, tileX, tileY, PkItemStruct)`.
  - On `OP_ITEM_DROP_RESP` (`0x0317`, `RealmStatus::OK`), attach the returned `itemUid` (`ItemDropResponsePayload::itemUid`) to the ground item so you or another player in the game can pick it up via `OP_ITEM_PICKUP_REQ`.

---

## 5. Rolling-XOR Shadow Memory Canaries (`Source/player.h`)

To harden in-memory fields against Cheat Engine memory freezes between server saves, mirror `_pBaseStr`, `_pBaseMag`, `_pBaseDex`, `_pBaseVit`, `_pStatPts`, `_pExperience`, `_pGold`, and `_pHitPoints` with a per-frame `uint32_t sessionXorKey` shadow value (`shadowField = realField ^ sessionXorKey`), verifying parity at the start of `Player::Process()`.

---

## 6. Closed-Realm Client Diagnostic Logger (`dx-eternity-client-realm.log`)

To make debugging Closed Realm issues side-by-side with the server's `dx-eternity-realm.log` effortless, add a dedicated Closed Realm logger in `Source/net/realm_client.cpp` that writes **only when `gbIsClosedRealm == true`**:

### Drop-in Client Logger (`Source/net/realm_client.cpp`)
```cpp
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include "utils/paths.h"

void RealmClientLog(const char *severity, const char *category, const char *fmt, ...)
{
	if (!gbIsClosedRealm)
		return;

	const std::string logPath = paths::PrefPath() + "dx-eternity-client-realm.log";
	std::FILE *fp = std::fopen(logPath.c_str(), "a");
	if (fp == nullptr)
		return;

	const std::time_t nowSec = std::time(nullptr);
	std::tm tmBuf {};
#ifdef _WIN32
	gmtime_s(&tmBuf, &nowSec);
#else
	gmtime_r(&nowSec, &tmBuf);
#endif
	char ts[32] {};
	std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S UTC", &tmBuf);

	std::fprintf(fp, "[%s] [%s] [%s] ", ts, severity, category);
	va_list args;
	va_start(args, fmt);
	std::vfprintf(fp, fmt, args);
	va_end(args);
	std::fprintf(fp, "\n");
	std::fclose(fp);
}
```

### What to Log in `dx-eternity-client-realm.log`
1. **Connection & Attestation (`CONNECT`, `ATTEST`, `AUTH`):**
   - Server IP/port, `OP_ATTEST_HANDSHAKE_RESP` `statusCode`, and `OP_AUTH_RESULT` (`statusCode`, `accountId`).
2. **Hero & Stash Vault (`HERO_LOAD`, `HERO_SAVE`, `STASH_SAVE`):**
   - `charId`, hero level, `pExperience`, `pGold`, `pDiabloKillLevel`, and the returned `statusCode` (if `statusCode != 0`, log all equipped item `(idx, iCreateInfo, iSeed, dwBuff)` tuples so any `RecreateItem` or provenance mismatch can be diagnosed immediately).
3. **Game Session & Floor Transitions (`GAME_JOIN`, `FLOOR_GATE`):**
   - `gameId`, `difficulty`, `fromLevel -> toLevel`, player `(tileX, tileY)`, and `statusCode`.
4. **Oracle Drops & Pickups (`MONSTER_DROP`, `CHEST_DROP`, `ITEM_PICKUP`, `ITEM_DROP`):**
   - Monster/object index, `combatDurationMs`, `maxSingleHitDealt`, `totalDamageDealt`, dispensed `itemUid` / `(idx, iCreateInfo, iSeed, dwBuff)`, and any non-zero `RealmStatus` error code (`ERR_ANOMALY_TTK_VIOLATION`, `ERR_ANOMALY_CADENCE_VIOLATION`, `ERR_ITEM_NOT_ON_GROUND`, etc.).

---

## 7. Built-In Red-Team / Chaos Security Test Hotkeys (`#ifdef REALM_CHAOS_TEST`)

Instead of using an external memory-injection trainer `.exe` (which only mutates client RAM and triggers antivirus false positives), compile your internal test builds of `dx-eternity.exe` with `-DREALM_CHAOS_TEST=1` to simulate both **Memory-Trainer Cheats** AND **Network Packet Exploits** at the press of a key, verifying that `dx-eternity-realm` rejects and logs every single one:

```cpp
#ifdef REALM_CHAOS_TEST
// Call from game key handler when Ctrl+Alt+F5..F10 are pressed in Closed Realm mode:
void RunRealmChaosExploitTest(int exploitId)
{
	auto &plr = Players[MyPlayerId];
	switch (exploitId) {
	case 1: // [Ctrl+Alt+F5] Simulate Memory Trainer: +500,000 Gold & +50 Base Strength before Save
		plr._pGold += 500000;
		plr._pBaseStr = 250;
		RealmClientLog("WARN", "CHAOS_TEST", "Triggered Trainer Gold/Str mutation -> sending OP_HERO_SAVE_REQ (expect ERR_SAVE_AUDIT_FAILED=19)");
		break;

	case 2: // [Ctrl+Alt+F6] Simulate Item Dupe Trainer: Clone Equipped Weapon (InvBody[4]) into InvList[0]
		plr.InvList[0] = plr.InvBody[INVLOC_HAND_LEFT];
		if (plr._pNumInv < 1)
			plr._pNumInv = 1;
		RealmClientLog("WARN", "CHAOS_TEST", "Triggered Equipped Item Clone -> sending OP_HERO_SAVE_REQ (expect ERR_SAVE_AUDIT_FAILED=19)");
		break;

	case 3: // [Ctrl+Alt+F7] Simulate 1-Shot God-Mode Boss Kill Packet (combatDurationMs=10ms, maxSingleHitDealt=99999)
	{
		devilution::realm::MonsterDropRequestPayload fakeKill {};
		fakeKill.gameId = gActiveRealmGameId;
		fakeKill.currlevel = currlevel;
		fakeKill.tileX = plr.position.tile.x;
		fakeKill.tileY = plr.position.tile.y;
		fakeKill.isUniqueMonster = 0;
		fakeKill.monsterIndex = 0;
		fakeKill.monsterTypeOrUid = MT_DIABLO;
		fakeKill.totalDamageDealt = 99999;
		fakeKill.maxSingleHitDealt = 99999;
		fakeKill.combatDurationMs = 10; // Impossible 10ms kill!
		devilution::realm::MonsterOrChestDropResponsePayload resp {};
		SendRealmRpc(devilution::realm::RealmOpcode::OP_MONSTER_DROP_REQ, &fakeKill, sizeof(fakeKill),
		    devilution::realm::RealmOpcode::OP_MONSTER_DROP_RESP, resp);
		RealmClientLog("WARN", "CHAOS_TEST", "Sent 1-Shot Diablo Kill RPC -> server returned statusCode=%u (expect ERR_ANOMALY_TTK_VIOLATION=16)", resp.statusCode);
		break;
	}

	case 4: // [Ctrl+Alt+F8] Simulate Maphack / Town-to-Hell-16 Floor Warp Packet
	{
		devilution::realm::FloorTransitionRequestPayload fakeWarp { gActiveRealmGameId, currlevel, 16, 40, 40 };
		devilution::realm::FloorTransitionResponsePayload resp {};
		SendRealmRpc(devilution::realm::RealmOpcode::OP_FLOOR_TRANSITION_REQ, &fakeWarp, sizeof(fakeWarp),
		    devilution::realm::RealmOpcode::OP_FLOOR_TRANSITION_RESP, resp);
		RealmClientLog("WARN", "CHAOS_TEST", "Sent illegal Floor Warp %u->16 -> server returned statusCode=%u (expect ERR_ANOMALY_FLOOR_GATE_VIOLATION=18)", currlevel, resp.statusCode);
		break;
	}
	}
}
#endif
```

---

## 8. Eternity Warden (`0x05xx`) & Public Client Source Isolation (`dx-eternity-guard`)

### 8.1 How Anti-Cheat Works When the Game Client Source Is Public
If the main `DX:Eternity` client repository is public (GPLv3 / open-source fork of DevilutionX), putting your secret `REALM_ATTESTATION_KEY` or your Warden memory-scanner implementation directly in the public git repo would allow anyone to read the key or compile a stripped client that fakes the responses.

To solve this cleanly, use the **Two-Tier Open-Core / Private-Guard Pattern** (the same pattern used by open-source competitive ports and custom private realms):

1. **Layer 1 — Server-Authoritative Rules (100% Immune to Modified Clients):**
   - Even if an attacker has full source code and builds a completely custom client, **they still cannot**:
     - Forge items or edit item affixes (`RecreateServerItem()` + `item_ledger` SQLite provenance rejects them).
     - Pre-cog or force monster/chest drops (`RealmSecretSalt` never leaves `dx-eternity-realm`).
     - Dupe items via packet replay or drop-and-save rollback (`TransitionLedgerItemState` atomic ground/inventory ledger).
     - Edit XP, Gold, Base Stats, or 1-shot bosses (`AuditMonsterKillAnomaly` + `AuditCharacterSaveConservation`).
     - Unlock Torment via high-level Hell party boosting (`highestPartyLevel - playerLevel > 10` below level 32 is blocked).
2. **Layer 2 — Separate Private Guard Module (`dx-eternity-guard.dll` / `libdx_eternity_guard.a`):**
   - Keep a tiny **private** C++ repository (`mrphantastik/dx-eternity-guard`) that compiles either:
     - A static library (`dx_eternity_guard.lib` / `libdx_eternity_guard.a`) linked **only** during your official CI/CD release build, **OR**
     - A signed companion module (`dx-eternity-guard.dll`) shipped alongside official `dx-eternity.exe` releases.
   - The public client source only sees a minimal 5-function C header (`include/dx_eternity_guard.h`):
     ```cpp
     // Public header in open-source client repo (no secrets, no scanner logic exposed):
     #pragma once
     #include <cstdint>
     #include <cstddef>

     extern "C" {
     // 1. Computes OP_ATTEST_HANDSHAKE_REQ (100B) + derives sessionTrafficKey (32B) using private key & .text hash
     bool DxGuard_BuildAttestationHandshake(
         const uint8_t serverNonce[32],
         void *outHandshakePayload100B,
         uint8_t outSessionTrafficKey[32]);

     // 2. Initializes per-session Rolling-XOR shadow canary key
     void DxGuard_InitSessionCanary();

     // 3. Seals authoritative player stats into shadow XOR storage (call after legitimate stat/gold/XP change)
     void DxGuard_SealPlayerStats(
         int32_t str, int32_t mag, int32_t dex, int32_t vit,
         int32_t statPts, int32_t level, int32_t exp, int32_t gold, int32_t hpBase);

     // 4. Verifies live player stats against shadow XOR storage (call at start of Player::Process())
     bool DxGuard_VerifyPlayerStatsCanary(
         int32_t str, int32_t mag, int32_t dex, int32_t vit,
         int32_t statPts, int32_t level, int32_t exp, int32_t gold, int32_t hpBase);

     // 5. Answers OP_WARDEN_CHALLENGE_REQ (0x0501, 28B) -> OP_WARDEN_CHALLENGE_RESP (0x0502, 52B)
     bool DxGuard_AnswerWardenChallenge(
         const void *challengePayload28B,
         void *outResponsePayload52B);
     }
     ```
   - **CMake Integration in `devilutionx-eternity/CMakeLists.txt`:**
     ```cmake
     option(DX_ETERNITY_WITH_PRIVATE_GUARD "Link private dx-eternity-guard static library for Closed Realm" OFF)
     if(DX_ETERNITY_WITH_PRIVATE_GUARD)
       add_subdirectory(external/dx-eternity-guard)
       target_link_libraries(devilutionx PRIVATE dx_eternity_guard)
     else()
       target_sources(devilutionx PRIVATE Source/realm/dx_eternity_guard_stub.cpp)
     endif()
     ```
   - Public contributors compiling from source get `dx_eternity_guard_stub.cpp` that works for Single-Player and open P2P/ZeroTier games, while **Official Closed Realm (`dx-eternity-realm`)** requires the genuine `dx_eternity_guard` static library linked during your CI/release build (`-DDX_ETERNITY_WITH_PRIVATE_GUARD=ON`).

---

### 8.2 Eternity Warden Client Responder Implementation (Inside Private `dx-eternity-guard`)
When the client receives `OP_WARDEN_CHALLENGE_REQ` (`0x0501`, `WardenChallengePayload`, 28 bytes), the guard module:
1. Checks the Windows process environment for Cheat Engine / debugger hooks (`WardenClientAnomalyFlags`):
   - `IsDebuggerPresent()` / PEB `BeingDebugged` -> `WARDEN_FLAG_DEBUGGER_PRESENT` (`1 << 0`)
   - Thread context hardware debug registers `DR0..DR3 != 0` -> `WARDEN_FLAG_HARDWARE_BREAKPOINT` (`1 << 1`)
   - `VirtualQuery` on `.text` section (`PAGE_EXECUTE_READWRITE` / `PAGE_WRITECOPY`) -> `WARDEN_FLAG_TEXT_SECTION_MODIFIED` (`1 << 4`)
   - Loaded modules (`speedhack-x86_64.dll`, `vehdebug-x86_64.dll`, `ced3d11hook64.dll`, `luaclient-x86_64.dll`) -> `WARDEN_FLAG_SUSPICIOUS_DLL` (`1 << 2`)
   - Rolling-XOR shadow canary mismatch on `Players[MyPlayerId]` -> `WARDEN_FLAG_XOR_CANARY_MISMATCH` (`1 << 3`)
2. Computes the canonical live RAM table slice digest via `ComputeWardenSliceDigest(challenge.tableId, challenge.sliceIndex, challenge.wardenNonce, digest)` over the live in-memory `AllItemsList`, `UniqueItems`, `SpellsData`, or `MonstersData` table.
3. Stamps `clientMonotonicMs = SDL_GetTicks64()` (or `QueryPerformanceCounter`) so the server can detect `Speedhack.dll` time-dilation (`ERR_WARDEN_SPEEDHACK_DETECTED = 30` if client clock runs `> 1.20x` server wall clock) and replies with `OP_WARDEN_CHALLENGE_RESP` (`0x0502`).

---

## 9. "Adjust for High Ping" Mode (Zero-Stall Async Oracle Queue + Optimistic Pickup + Adaptive Turn-Buffer)

To make `150–250ms` transatlantic connections feel as responsive as `0ms` Single-Player—even at Closed Realm's default **`Fast` (`30` Hz, `33.3ms`/tick)** game speed—**never block the game loop waiting for `OP_MONSTER_DROP_RESP`, `OP_CHEST_DROP_RESP`, or `OP_ITEM_PICKUP_RESP`**. Instead, drop this non-blocking `RealmAsyncOracleQueue` into your client networking layer:

### 9.1 Non-Blocking Async Oracle & Optimistic Item Pickup (`Source/realm/realm_async_oracle.cpp`)
```cpp
#include <deque>
#include <unordered_map>
#include "protocol/realm_protocol.hpp"
#include "dx_eternity_guard.h"

namespace devilution::realm_client {

struct OptimisticPickupEntry {
	uint64_t itemUid = 0;
	int groundItemIdx = -1;
	uint8_t tileX = 0;
	uint8_t tileY = 0;
	PkItemStruct backupPacked {};
	uint64_t requestedAtMs = 0;
};

class RealmAsyncOracleQueue {
public:
	// 1. Call from M_StartKill() when a monster dies locally (0ms frame stall!):
	//    The monster death animation plays immediately for ~350ms while the packet travels to/from the server.
	void QueueAsyncMonsterKill(const devilution::realm::MonsterDropRequestPayload &req)
	{
		SendNonBlockingRealmFrame(devilution::realm::RealmOpcode::OP_MONSTER_DROP_REQ, &req, sizeof(req));
	}

	// 2. Call from OperateChest() / OperateBarrel() when a container opens locally (0ms frame stall!):
	void QueueAsyncChestOpen(const devilution::realm::ChestDropRequestPayload &req)
	{
		SendNonBlockingRealmFrame(devilution::realm::RealmOpcode::OP_CHEST_DROP_REQ, &req, sizeof(req));
	}

	// 3. Optimistic Item Pickup Prediction:
	//    Immediately hides the ground item & plays IS_IGRAB on Frame 0 so the player feels 0ms lag,
	//    then confirms or rolls back when OP_ITEM_PICKUP_RESP arrives.
	void RequestOptimisticItemPickup(
	    uint64_t gameId,
	    uint8_t currlevel,
	    int groundItemIndex,
	    uint64_t serverItemUid,
	    const devilution::realm::PkItemStruct &packed)
	{
		OptimisticPickupEntry pending {};
		pending.itemUid = serverItemUid;
		pending.groundItemIdx = groundItemIndex;
		pending.tileX = Items[groundItemIndex].position.x;
		pending.tileY = Items[groundItemIndex].position.y;
		pending.backupPacked = packed;
		pending.requestedAtMs = SDL_GetTicks64();
		pendingPickups_[serverItemUid] = pending;

		// Optimistically play sound & hide ground sprite on Frame 0
		PlaySFX(IS_IGRAB);
		Items[groundItemIndex]._iDelFlag = true;

		devilution::realm::ItemPickupRequestPayload req {};
		req.gameId = gameId;
		req.itemUid = serverItemUid;
		req.currlevel = currlevel;
		req.tileX = pending.tileX;
		req.tileY = pending.tileY;
		req.iSeed = packed.iSeed;
		req.idx = packed.idx;
		req.iCreateInfo = packed.iCreateInfo;
		SendNonBlockingRealmFrame(devilution::realm::RealmOpcode::OP_ITEM_PICKUP_REQ, &req, sizeof(req));
	}

	// 4. Call once per game tick (or render frame) to drain incoming Realm Server responses without blocking:
	void PollAsyncRealmResponses()
	{
		devilution::realm::RealmPacketHeader hdr {};
		std::vector<uint8_t> payload;
		while (TryReadNonBlockingRealmFrame(hdr, payload)) {
			const auto opcode = static_cast<devilution::realm::RealmOpcode>(hdr.opcode);
			switch (opcode) {
			case devilution::realm::RealmOpcode::OP_MONSTER_DROP_RESP:
			case devilution::realm::RealmOpcode::OP_CHEST_DROP_RESP: {
				if (payload.size() == sizeof(devilution::realm::MonsterOrChestDropResponsePayload)) {
					devilution::realm::MonsterOrChestDropResponsePayload resp {};
					std::memcpy(&resp, payload.data(), sizeof(resp));
					if (resp.statusCode == static_cast<uint16_t>(devilution::realm::RealmStatus::OK)) {
						for (uint8_t i = 0; i < resp.dropCount; ++i) {
							SpawnAuthoritativeOracleDrop(resp.drops[i]);
						}
					}
				}
				break;
			}
			case devilution::realm::RealmOpcode::OP_ITEM_PICKUP_RESP: {
				if (payload.size() == sizeof(devilution::realm::ItemPickupResponsePayload)) {
					devilution::realm::ItemPickupResponsePayload resp {};
					std::memcpy(&resp, payload.data(), sizeof(resp));
					auto it = pendingPickups_.find(resp.itemUid);
					if (it != pendingPickups_.end()) {
						if (resp.statusCode == static_cast<uint16_t>(devilution::realm::RealmStatus::OK)) {
							// Authoritative ownership granted -> materialize in player inventory & reseal XOR canary
							UnPackPlayerItemToInventory(MyPlayerId, resp.item);
							SealCurrentPlayerStatsToGuard();
						} else {
							// Another player won the pickup race by a few ms -> restore ground sprite cleanly
							RestoreOptimisticGroundItem(it->second);
						}
						pendingPickups_.erase(it);
					}
				}
				break;
			}
			case devilution::realm::RealmOpcode::OP_WARDEN_CHALLENGE_REQ: {
				if (payload.size() == sizeof(devilution::realm::WardenChallengePayload)) {
					devilution::realm::WardenResponsePayload wResp {};
					if (DxGuard_AnswerWardenChallenge(payload.data(), &wResp)) {
						SendNonBlockingRealmFrame(
						    devilution::realm::RealmOpcode::OP_WARDEN_CHALLENGE_RESP, &wResp, sizeof(wResp));
					}
				}
				break;
			}
			case devilution::realm::RealmOpcode::OP_PONG: {
				if (payload.size() == sizeof(devilution::realm::PingPongPayload)) {
					devilution::realm::PingPongPayload pong {};
					std::memcpy(&pong, payload.data(), sizeof(pong));
					const uint64_t rtt = SDL_GetTicks64() - pong.clientTimestampMs;
					smoothedRttMs_ = (smoothedRttMs_ * 3 + static_cast<uint32_t>(rtt)) / 4;
				}
				break;
			}
			default:
				break;
			}
		}
	}

	// 5. Adaptive Lockstep Turn-Buffer Depth for High-Ping Mode (`dvlnet`):
	//    - Solo in Closed Realm (`activePartyPlayers <= 1`): returns 1 tick (0ms extra input delay!)
	//    - 2..4 Player Co-Op with "Adjust for High Ping" enabled: scales turn buffer smoothly (2..6 ticks)
	//      accounting for game tickRate (30Hz 'Fast' = 33.3ms/tick, 20Hz 'Normal' = 50ms/tick)
	//      so 150-220ms connections never hitch waiting for remote turn packets.
	uint32_t ComputeRecommendedTurnDelayTicks(int activePartyPlayers, uint8_t tickRate, bool adjustForHighPingEnabled) const
	{
		if (activePartyPlayers <= 1 || !adjustForHighPingEnabled)
			return 1U;
		const uint32_t tickDurationMs = (tickRate >= 30U) ? 33U : 50U;
		const uint32_t neededTicks = (smoothedRttMs_ + tickDurationMs - 1U) / tickDurationMs + 1U;
		return std::clamp<uint32_t>(neededTicks, 2U, 6U);
	}

private:
	uint32_t smoothedRttMs_ = 50;
	std::unordered_map<uint64_t, OptimisticPickupEntry> pendingPickups_;
};

} // namespace devilution::realm_client
```

---

## 10. Complete Wire Protocol Opcode, Payload Size & `RealmStatus` Reference (`Server/protocol/realm_protocol.hpp`)

### 10.1 Master Opcode & Struct Size Table
Every TCP frame begins with `RealmPacketHeader` (`32` bytes: `magic=0x52455844`, `version=1`, `opcode`, `payloadLength`, `sequenceNumber`, `mac[16]`).

| Opcode | Hex | Direction | Payload Struct | Exact Bytes | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `OP_PING` / `OP_PONG` | `0x0001` / `0x0002` | C↔S | `PingPongPayload` | `16 B` | RTT measurement & keepalive |
| `OP_ERROR_RESPONSE` | `0x00FF` | S→C | `ErrorResponsePayload` | `128 B` | Generic protocol/auth error |
| `OP_AUTH_CHALLENGE_REQ` | `0x0101` | C→S | *(empty)* | `0 B` | Signed with `GetDefaultHandshakeKey()` |
| `OP_AUTH_CHALLENGE_RESP` | `0x0102` | S→C | `AuthChallengeResponsePayload` | `36 B` | Returns `serverNonce[32]` |
| `OP_AUTH_REGISTER_REQ` | `0x0103` | C→S | `AuthCredentialsRequestPayload` | `96 B` | Requires `attested == true` |
| `OP_AUTH_LOGIN_REQ` | `0x0104` | C→S | `AuthCredentialsRequestPayload` | `96 B` | Requires `attested == true` |
| `OP_AUTH_RESULT` | `0x0105` | S→C | `AuthResultPayload` | `108 B` | Returns `accountId` + `sessionToken[32]` |
| `OP_ATTEST_HANDSHAKE_REQ` | `0x0106` | C→S | `AttestHandshakeRequestPayload` | `100 B` | Built by `DxGuard_BuildAttestationHandshake` |
| `OP_ATTEST_HANDSHAKE_RESP` | `0x0107` | S→C | `AttestHandshakeResponsePayload` | `4 B` | Rotates MAC key to `sessionTrafficKey` after send |
| `OP_AUTH_RESUME_REQ` | `0x0108` | C→S | `AuthResumeRequestPayload` | `40 B` | Resumes session via `sessionToken[32]` |
| `OP_HERO_LIST_REQ` | `0x0201` | C→S | *(empty)* | `0 B` | Requests account hero roster |
| `OP_HERO_LIST_RESP` | `0x0202` | S→C | `HeroListResponsePayload` | `580 B` | Up to `12` × `HeroSummaryInfo` (`48 B`) |
| `OP_HERO_CREATE_REQ` | `0x0203` | C→S | `HeroCreateRequestPayload` | `36 B` | `heroClass = 0..5` (`Barbarian = 5`) |
| `OP_HERO_CREATE_RESP` | `0x0204` | S→C | `HeroCreateResponsePayload` | `1314 B` | Returns `HeroSummaryInfo` + `PlayerPack` (`1262 B`) |
| `OP_HERO_DELETE_REQ` | `0x0205` | C→S | `HeroDeleteRequestPayload` | `8 B` | Deletes hero & cascades `item_ledger` |
| `OP_HERO_DELETE_RESP` | `0x0206` | S→C | `HeroDeleteResponsePayload` | `12 B` | Confirm deletion |
| `OP_HERO_LOAD_REQ` | `0x0207` | C→S | `HeroLoadRequestPayload` | `8 B` | Loads authoritative `PlayerPack` |
| `OP_HERO_LOAD_RESP` | `0x0208` | S→C | `HeroLoadResponsePayload` | `1274 B` | Write one-way local `.sv` backup on `OK` |
| `OP_HERO_SAVE_REQ` | `0x0209` | C→S | `HeroSaveRequestPayload` | `1270 B` | Audited for XP/Stat/Gold (`<=200k`) & `item_ledger` |
| `OP_HERO_SAVE_RESP` | `0x020A` | S→C | `HeroSaveResponsePayload` | `12 B` | `OK` or `ERR_SAVE_AUDIT_FAILED` |
| `OP_STASH_LOAD_REQ` | `0x020B` | C→S | *(empty)* | `0 B` | Loads 1-page shared account stash |
| `OP_STASH_LOAD_RESP` | `0x020C` | S→C | `StashDataHeader` + `N*PkItemStruct` | `8 + 19*N B` | `N <= 100` (1 page), `stashGold <= 300,000` |
| `OP_STASH_SAVE_REQ` | `0x020D` | C→S | `StashDataHeader` + `N*PkItemStruct` | `8 + 19*N B` | `N <= 100`, physical `<=5,000g` stacks, `<=300k` total |
| `OP_STASH_SAVE_RESP` | `0x020E` | S→C | `StashSaveResponsePayload` | `8 B` | Returns verified physical `stashGold` |
| `OP_GAME_CREATE_REQ` | `0x0301` | C→S | `GameCreateRequestPayload` | `76 B` | `tickRate`: `0` or `30` (`Fast` default/max), `20` (`Normal`) |
| `OP_GAME_CREATE_RESP` | `0x0302` | S→C | `GameCreateOrJoinResponsePayload` | `20 B` | Returns `gameId`, `terrainInitSeed`, `tickRate` |
| `OP_GAME_JOIN_REQ` | `0x0303` | C→S | `GameJoinRequestPayload` | `72 B` | Enforces Torment (`kill>=3`, `lvl>=35` MP) gates |
| `OP_GAME_JOIN_RESP` | `0x0304` | S→C | `GameCreateOrJoinResponsePayload` | `20 B` | Returns `playerSlot`, `terrainInitSeed`, `tickRate` |
| `OP_GAME_LIST_REQ` | `0x0305` | C→S | Empty or `GameListFilterRequestPayload` | `0 B` or `36 B` | Filter by query, difficulty, full, or password |
| `OP_GAME_LIST_RESP` | `0x0306` | S→C | `GameListResponsePayload` | `1412 B` | Up to `32` × `GameLobbyInfo` (`44 B`) |
| `OP_FLOOR_TRANSITION_REQ` | `0x0307` | C→S | `FloorTransitionRequestPayload` | `12 B` | Includes Caves 12↔Hive 17 & Hell 14↔Crypt 21 |
| `OP_FLOOR_TRANSITION_RESP` | `0x0308` | S→C | `FloorTransitionResponsePayload` | `4 B` | `OK` or `ERR_ANOMALY_FLOOR_GATE_VIOLATION` |
| `OP_MONSTER_DROP_REQ` | `0x0310` | C→S | `MonsterDropRequestPayload` | `28 B` | Audited for DPS/TTK, cadence & floor gate |
| `OP_MONSTER_DROP_RESP` | `0x0311` | S→C | `MonsterOrChestDropResponsePayload` | `240 B` | Up to `4` drops (`OracleDroppedItemEntry` `58 B`) |
| `OP_CHEST_DROP_REQ` | `0x0312` | C→S | `ChestDropRequestPayload` | `16 B` | Server-Oracle container loot roll |
| `OP_CHEST_DROP_RESP` | `0x0313` | S→C | `MonsterOrChestDropResponsePayload` | `240 B` | Up to `4` drops |
| `OP_ITEM_PICKUP_REQ` | `0x0314` | C→S | `ItemPickupRequestPayload` | `28 B` | Atomic `OnGround -> InInventoryOrStash` transfer |
| `OP_ITEM_PICKUP_RESP` | `0x0315` | S→C | `ItemPickupResponsePayload` | `31 B` | `OK` or `ERR_ITEM_NOT_ON_GROUND` |
| `OP_ITEM_DROP_REQ` | `0x0316` | C→S | `ItemDropRequestPayload` | `31 B` | Atomic `InInventoryOrStash -> OnGround` transfer |
| `OP_ITEM_DROP_RESP` | `0x0317` | S→C | `ItemDropResponsePayload` | `62 B` | Returns `OracleDroppedItemEntry` with `itemUid` |
| `OP_CHAT_JOIN_CHANNEL_REQ` | `0x0401` | C→S | `ChatJoinChannelRequestPayload` | `16 B` | `#General=0`, `#Trade=1`, `#LFG=2`, `#Torment=3` |
| `OP_CHAT_JOIN_CHANNEL_RESP` | `0x0402` | S→C | `ChatChannelFeedResponsePayload` | `3472 B` | Up to `16` × `ChatMessageEntry` (`216 B`) |
| `OP_CHAT_SEND_REQ` | `0x0403` | C→S | `ChatSendRequestPayload` | `168 B` | Supports `/w <hero> <msg>`, `/games`, `/who` |
| `OP_CHAT_SEND_RESP` | `0x0404` | S→C | `ChatSendResponsePayload` | `16 B` | `OK` or `ERR_CHAT_RATE_LIMITED` |
| `OP_CHAT_POLL_REQ` | `0x0405` | C→S | `ChatPollRequestPayload` | `16 B` | Polls messages with `messageId > afterMessageId` |
| `OP_CHAT_POLL_RESP` | `0x0406` | S→C | `ChatChannelFeedResponsePayload` | `3472 B` | Channel feed + private whispers |
| `OP_WARDEN_CHALLENGE_REQ` | `0x0501` | S→C | `WardenChallengePayload` | `28 B` | Issued periodically (`45s`) or on demand |
| `OP_WARDEN_CHALLENGE_RESP` | `0x0502` | C→S | `WardenResponsePayload` | `52 B` | Built by `DxGuard_AnswerWardenChallenge` |
| `OP_WARDEN_VERDICT_RESP` | `0x0503` | S→C | `WardenVerdictPayload` | `16 B` | `OK`, `ERR_WARDEN_INTEGRITY_VIOLATION` (`29`), or `ERR_WARDEN_SPEEDHACK_DETECTED` (`30`) |
