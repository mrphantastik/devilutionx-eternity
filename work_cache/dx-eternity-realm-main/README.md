# DX:Eternity Closed Realm Server (`dx-eternity-realm`)

Official C++20 Closed Realm, Server-Vault & Anti-Cheat Server for **DevilutionX: Eternity** (`Beta v0.7d+`).

## Architecture Overview (Tier 1.5 Server-Vault + Server-Oracle Loot RPC)

- **Zero-Dependency Static Binaries:** Compiles to standalone single-file executables (`misc/dx-eternity-realm` for Linux x86_64 VPS and `misc/dx-eternity-realm.exe` for Windows 64-bit) with `libsodium` and `sqlite3` statically linked.
- **Official Binary Integrity Attestation & Argon2id Auth (`Server/auth/`):** Challenge-response handshake (`REALM_ATTESTATION_KEY` BLAKE2b-256 HMAC) with per-session traffic MAC key derivation and `Argon2id` password hashing.
- **Server-Vault Persistence (`Server/vault/`):** Server-side `PlayerPack` (`1262`B) and Account Stash (`PkItemStruct` `19`B) storage in SQLite3 WAL mode — Closed Realm clients never read or write local `.sv` files.
- **Deterministic 1:1 `DX:Eternity` LCG & `RecreateItem()` Verification (`Server/game/`):** Exact Borland C++ LCG parity (`0x015A4E35`), full `AllItemsList` / `ItemPrefixes` / `ItemSuffixes` / `UniqueItems` tables (including Hell/Torment Diablo exclusives `UITEM_KADISFURY`, `UITEM_APOSTATESCROOK`, `UITEM_LOXLEYSBEND`), and `DIFF_TORMENT` (`CF_TORMENT`) maxed roll verification.
- **Server-Oracle Loot Dispensing & Real-Time Anomaly Detection (`ROADMAP.md`):** Server-held `RealmSecretSalt` drop rolling, atomic Ground-to-Inventory item ledger, DPS/TTK ceiling auditing, and Embedded C++20 Admin Web Control Interface.

## Documentation

- [`CLOSED_REALM_ARCHITECTURE.md`](CLOSED_REALM_ARCHITECTURE.md) — Complete Closed Realm & Anti-Cheat Architecture Specification
- [`ROADMAP.md`](ROADMAP.md) — Multi-Phase Engineering Roadmap & Status
- [`DEVLOG.md`](DEVLOG.md) — Detailed Development & Verification Log

## Building & Running Self-Tests

```bash
# Build Linux x86_64 static binary
cmake --build .deps/build-linux -j4

# Cross-compile Windows 64-bit static binary
cmake --build .deps/build-win64 -j4

# Run automated end-to-end verification suite
./misc/dx-eternity-realm --self-test
```
