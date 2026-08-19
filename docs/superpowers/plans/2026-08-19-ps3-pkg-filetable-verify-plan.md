# PS3 Update Install Verification — PKG File-Table Check Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A partial PS3 update install can never be recorded as success — the PKG's own decrypted
entry table (every file's name and size) is the success criterion, and a poisoned tree already on
disk heals itself on the next launch.

**Architecture:** A new `Ps3Pkg` module parses a retail pkg's header and AES-128-CTR-decrypts its
entry table with the fixed retail GPKG key (validated live against the 13 sha1-verified LBP pkgs in
`C:\Users\cubma\rpcs3-bisect\pkgs` on 2026-08-19 — clean UTF-8 paths, and entry 8 of A0130.pkg is
exactly the file that was 0 bytes on hardware: `USRDIR/patch.sdat`, expected 910064 bytes).
`Ps3Pkg::verifyInstalled` checks the installed tree against that table and is ANDed into every
success decision in EmulatorManager's `--installpkg` lambda. When the table can't be parsed, the
defensible fallback is "no 0-byte regular file under USRDIR". A new optional `InstallIntact` seam on
`Ps3UpdateCoordinator` re-runs the chain once (marker-guarded) when the state file claims current
but the tree holds a 0-byte file — this is what heals the poisoned machine (and any user machine
poisoned by the pre-fix build) automatically.

**Tech Stack:** C++17/Qt6::Core only (probe links Qt6::Core), a self-contained AES-128
encrypt-block implementation inside Ps3Pkg.cpp (no new dependencies — the repo has none for crypto;
QCryptographicHash has no AES).

## Global Constraints

- No AI attribution anywhere: no `Co-Authored-By`, no "Generated with" — commits are ordinary project prose.
- Conventional commit prefixes (`fix:`, `docs:` …) per CONTRIBUTING.md.
- probe_ps3update is an EXISTING gated probe (already in run-headless-probes.sh's mandatory list and
  CMakeLists) — no new probe registration; but the new `Ps3Pkg.cpp` source must be added to BOTH the
  main executable source list AND the probe_ps3update source list in native/CMakeLists.txt.
- The probe must stay headless-deterministic with **no args**; the new `--dump` / `--verify` argv
  modes are dev tooling only and must not change no-arg behavior.
- Build config is **Release** (`cmake --build build --config Release --parallel`).
- The gate is `bash native/tools/run-headless-probes.sh` → must end `ALL HEADLESS PROBES PASSED`.
- Mutation sanity: extend `native/tools/mutate-ps3pkgverify.json`; run
  `python native/tools/mutate.py native/tools/mutate-ps3pkgverify.json` — every mutant `expect: killed`.
- Comments follow the repo's idiom: constraints and hardware ground truth (dated), never narration.

## Verified ground truth this plan builds on (do not re-derive)

- Retail PS3 pkg layout (all offsets big-endian, validated live 2026-08-19 against A0130.pkg):
  magic `0x7F504B47` @0x00 (u32); pkg type @0x06 (u16, `0x0001` = PS3); item_count @0x14 (u32);
  data_offset @0x20 (u64, A0130: 0x190); data_size @0x28 (u64); content id @0x30 (0x30 bytes);
  riv (CTR counter base) @0x70 (16 bytes).
- Data area (starting at data_offset) is AES-128-CTR encrypted: keystream block N =
  AES-ECB-Encrypt(key, riv + N) where riv+N is 128-bit big-endian addition. Retail GPKG key:
  `2E 7B 71 D7 C9 C9 A1 4E A3 22 1F 18 88 28 B8 F8`.
- Entry table = first item_count × 32 bytes of the data area. Entry: name_offset (u32, relative to
  data area start), name_size (u32), data_offset (u64), data_size (u64), type (u32), pad (u32).
- Type semantics (from RPCS3 `Crypto/unpkg.cpp`, read 2026-08-19 at
  `C:\Users\cubma\source\repos\RetroPark\external\rpcs3\rpcs3\Crypto\unpkg.cpp:926-1075`):
  `type & 0xFF` == 0x04 or 0x12 → directory; every other value RPCS3 extracts as a file of exactly
  data_size bytes. `type & 0x80000000` (PKG_FILE_ENTRY_OVERWRITE): when CLEAR and the file already
  exists, RPCS3 **skips** it ("Didn't overwrite") — so a non-overwrite entry's on-disk size may
  legitimately differ from the table.
- AES-CTR known-answer vectors (independent reference: .NET AES-128-ECB, generated 2026-08-19):
  - riv=00×16, blockOffset 0, pt=00×32 → `06b0681ba85d3e959861d07991838548c54505a018180ec6c1bc89151d6392d1`
  - riv=ff×16, blockOffset 1, pt=00×16 → `06b0681ba85d3e959861d07991838548` (counter wraps to zero — pins the carry)
  - riv=`4309f292bd44abd6788293457fd4a8a4` (A0130's real riv), blockOffset 3, pt=00×16 → `a4a93c5f9890b4c6d0663ebc25cf1efc`
- The hardware failure (2026-08-19, LBP BCUS98148): an install left `USRDIR/patch.sdat` as a 0-byte
  file and 6 files missing while PARAM.SFO claimed APP_VER=01.30; version+quiet-fingerprint passed;
  the game then crashed deterministically ~14s after boot. The machine's ps3-updates.json also
  RECORDED the success, so the coordinator's `needsUpdate` gate skips the chain forever — healing
  needs a hook that fires even when state says current.

---

### Task 1: Ps3Pkg module skeleton + AES-128-CTR (`gpkgCrypt`) with known-answer tests

**Files:**
- Create: `native/src/core/ps3/Ps3Pkg.h`
- Create: `native/src/core/ps3/Ps3Pkg.cpp`
- Modify: `native/CMakeLists.txt` (two places: main source list near line 458, probe_ps3update sources near line 1116)
- Test: `native/tools/probe_ps3update.cpp`

**Interfaces:**
- Produces: `Ps3Pkg::gpkgCrypt(const QByteArray& data, const QByteArray& riv, qint64 blockOffset = 0) -> QByteArray`
  (AES-128-CTR transform with the retail GPKG key baked in; encrypt == decrypt; empty on riv.size() != 16).
- Produces (declared now, implemented in Tasks 2–3): `Ps3Pkg::Entry { QString path; qint64 size; bool isDir; bool overwrite; }`,
  `Ps3Pkg::entries(const QString& pkgPath) -> std::optional<QVector<Entry>>`,
  `Ps3Pkg::verifyInstalled(const QString& gameDir, const QVector<Entry>&) -> bool`.

- [ ] **Step 1: Write the header**

`native/src/core/ps3/Ps3Pkg.h`:

```cpp
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <optional>

// Retail PS3 .pkg entry-table access — the airtight "what must this install produce" list. A pkg's
// header names an item count and a data area; the data area begins with item_count 32-byte entries
// (name offset/size, data offset/size, type flags), everything AES-128-CTR encrypted with the fixed
// retail GPKG key. Hardware ground truth 2026-08-19 (LBP BCUS98148): an --installpkg run left
// PARAM.SFO claiming the target APP_VER=01.30 over a 0-byte USRDIR/patch.sdat and 6 missing files —
// PARAM.SFO extracts early, so only the table itself can say what a COMPLETE install looks like.
namespace Ps3Pkg {

struct Entry {
    QString path;              // relative to the game dir, e.g. "USRDIR/patch.sdat"
    qint64  size = 0;          // bytes RPCS3 writes to disk (pkg data is CTR: same size decrypted)
    bool    isDir = false;     // type & 0xFF == 0x04/0x12 (unpkg.cpp's folder cases)
    bool    overwrite = false; // PKG_FILE_ENTRY_OVERWRITE (0x80000000): when CLEAR, RPCS3 keeps an
                               // existing file untouched ("Didn't overwrite"), so its on-disk size
                               // may legitimately differ from this entry's.
};

// AES-128-CTR keystream transform with the retail PS3 GPKG key (encrypt == decrypt). `riv` is the
// 16-byte counter base from the pkg header at 0x70; `blockOffset` positions `data` within the pkg's
// data area in 16-byte blocks (keystream block N is AES(key, riv + blockOffset + N), the addition
// 128-bit big-endian). Exposed so the probe can build encrypted fixtures with the very transform
// the parser undoes — and pin the transform itself against independent known-answer vectors, since
// a fixture round-trip alone would let a broken AES cancel itself out. Empty when riv is not 16 bytes.
QByteArray gpkgCrypt(const QByteArray& data, const QByteArray& riv, qint64 blockOffset = 0);

// Parse pkgPath's entry table. nullopt when the file is not a PS3 pkg or the decrypted table fails
// sanity (counts/offsets out of range, names not clean relative paths) — a table this key
// demonstrably did not decrypt must not drive verification, so the caller falls back to the
// 0-byte-file heuristic instead.
std::optional<QVector<Entry>> entries(const QString& pkgPath);

// After an --installpkg run: does gameDir hold everything the table names? Directories must exist;
// files must exist at exactly the expected size — except a non-overwrite entry may keep a
// pre-existing file of a different size (see Entry::overwrite), accepted only when non-empty.
// A 0-byte file where the table expects bytes is always a failed install (the 2026-08-19 poison).
// Sizes are path-based (fresh QFileInfo): every caller runs after the installer's handles closed
// (self-exit, post-kill) or inside the quiet window where a stale directory-entry size can only
// FAIL the check and keep us waiting — the safe direction.
bool verifyInstalled(const QString& gameDir, const QVector<Entry>& entries);

} // namespace Ps3Pkg
```

- [ ] **Step 2: Write the failing probe tests**

In `native/tools/probe_ps3update.cpp`, add `#include "core/ps3/Ps3Pkg.h"` to the includes, add this
function, and call `testPkgCrypt();` in `main()` after `testCoordinator();`:

```cpp
// AES-128-CTR with the retail GPKG key, pinned against an INDEPENDENT implementation (.NET
// AES-128-ECB keystream, generated 2026-08-19) — the fixture tests below encrypt with the same
// function the parser decrypts with, so only known-answer vectors can catch a broken AES/key/counter.
static void testPkgCrypt()
{
    const QByteArray z16(16, '\0'), z32(32, '\0');
    CHECK(Ps3Pkg::gpkgCrypt(z32, z16).toHex()
          == "06b0681ba85d3e959861d07991838548c54505a018180ec6c1bc89151d6392d1");
    // riv=ff*16 + blockOffset 1 wraps the 128-bit counter to zero — the carry math, pinned: the
    // wrapped keystream must equal the riv=0 vector's first block.
    CHECK(Ps3Pkg::gpkgCrypt(z16, QByteArray(16, char(0xFF)), 1).toHex()
          == "06b0681ba85d3e959861d07991838548");
    // The real A0130.pkg riv at block 3 (the live parse that validated key+format, 2026-08-19).
    const QByteArray riv = QByteArray::fromHex("4309f292bd44abd6788293457fd4a8a4");
    CHECK(Ps3Pkg::gpkgCrypt(z16, riv, 3).toHex() == "a4a93c5f9890b4c6d0663ebc25cf1efc");
    // CTR is its own inverse at any block offset, and actually transforms.
    const QByteArray msg("USRDIR/patch.sdat sized 910064");
    CHECK(Ps3Pkg::gpkgCrypt(Ps3Pkg::gpkgCrypt(msg, riv, 7), riv, 7) == msg);
    CHECK(Ps3Pkg::gpkgCrypt(msg, riv, 7) != msg);
    // Non-block-multiple lengths keep the tail (partial last block).
    CHECK(Ps3Pkg::gpkgCrypt(QByteArray(17, '\0'), z16).toHex()
          == "06b0681ba85d3e959861d07991838548c5");
    CHECK(Ps3Pkg::gpkgCrypt(msg, QByteArray(15, '\0')).isEmpty()); // riv must be 16 bytes
}
```

- [ ] **Step 3: Register sources in CMake and verify the test fails**

In `native/CMakeLists.txt`, add to the main source list (beside the other ps3 files near line 458):

```
        src/core/ps3/Ps3Pkg.cpp               src/core/ps3/Ps3Pkg.h
```

and add `src/core/ps3/Ps3Pkg.cpp` to the `add_executable(probe_ps3update ...)` source list (near
line 1116). Create `Ps3Pkg.cpp` with just the namespace and a stub
`QByteArray gpkgCrypt(const QByteArray&, const QByteArray&, qint64) { return {}; }` plus stubs
returning `std::nullopt` / `false` for `entries` / `verifyInstalled` so it links.

Run: `cmake --build build --config Release --target probe_ps3update --parallel && build/Release/probe_ps3update.exe`
Expected: `CHECK failed` lines from testPkgCrypt, non-zero exit.

- [ ] **Step 4: Implement gpkgCrypt**

In `native/src/core/ps3/Ps3Pkg.cpp` (this full file body; `entries`/`verifyInstalled` stay stubs
until Tasks 2–3):

```cpp
#include "core/ps3/Ps3Pkg.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <cstring>

namespace {

// Self-contained AES-128 block ENCRYPTION (all CTR ever needs — decryption is the same XOR). FIPS-197
// verbatim; pinned against independent known-answer vectors in probe_ps3update's testPkgCrypt, so a
// transcription slip here goes red rather than silently rejecting every genuine pkg into the
// fallback path. Qt has no AES and the repo takes no crypto dependency for one fixed-key CTR.
const quint8 kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

quint8 xtime(quint8 x) { return quint8((x << 1) ^ ((x >> 7) * 0x1b)); }

struct Aes128 {
    quint8 rk[176]; // 11 round keys
    explicit Aes128(const quint8 key[16])
    {
        std::memcpy(rk, key, 16);
        quint8 rcon = 1;
        for (int i = 16; i < 176; i += 4)
        {
            quint8 t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
            if (i % 16 == 0)
            {
                const quint8 tmp = t[0];
                t[0] = quint8(kSbox[t[1]] ^ rcon); t[1] = kSbox[t[2]];
                t[2] = kSbox[t[3]];                t[3] = kSbox[tmp];
                rcon = xtime(rcon);
            }
            for (int j = 0; j < 4; ++j) rk[i + j] = quint8(rk[i - 16 + j] ^ t[j]);
        }
    }
    void encryptBlock(const quint8 in[16], quint8 out[16]) const
    {
        quint8 s[16];
        for (int i = 0; i < 16; ++i) s[i] = quint8(in[i] ^ rk[i]);
        for (int round = 1; round <= 10; ++round)
        {
            for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]]; // SubBytes
            // ShiftRows (state is column-major: byte r + 4c)
            quint8 t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;
            t = s[2];  s[2]  = s[10]; s[10] = t;  t = s[6]; s[6] = s[14]; s[14] = t;
            t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
            if (round != 10)
                for (int c = 0; c < 16; c += 4) // MixColumns
                {
                    const quint8 a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];
                    const quint8 x = quint8(a0 ^ a1 ^ a2 ^ a3);
                    s[c]     = quint8(a0 ^ x ^ xtime(quint8(a0 ^ a1)));
                    s[c + 1] = quint8(a1 ^ x ^ xtime(quint8(a1 ^ a2)));
                    s[c + 2] = quint8(a2 ^ x ^ xtime(quint8(a2 ^ a3)));
                    s[c + 3] = quint8(a3 ^ x ^ xtime(quint8(a3 ^ a0)));
                }
            for (int i = 0; i < 16; ++i) s[i] = quint8(s[i] ^ rk[round * 16 + i]);
        }
        std::memcpy(out, s, 16);
    }
};

// The retail PS3 GPKG data key. Validated live 2026-08-19: decrypts the entry tables of all 13
// sha1-verified LBP update pkgs into clean UTF-8 paths (rpcs3-bisect/pkgs).
const quint8 kGpkgKey[16] = { 0x2E,0x7B,0x71,0xD7,0xC9,0xC9,0xA1,0x4E,
                              0xA3,0x22,0x1F,0x18,0x88,0x28,0xB8,0xF8 };

} // namespace

namespace Ps3Pkg {

QByteArray gpkgCrypt(const QByteArray& data, const QByteArray& riv, qint64 blockOffset)
{
    if (riv.size() != 16) return {};
    const Aes128 aes(kGpkgKey);
    quint8 ctr[16];
    std::memcpy(ctr, riv.constData(), 16);
    // Position the 128-bit big-endian counter at riv + blockOffset (with carry).
    quint64 carry = quint64(blockOffset);
    for (int i = 15; i >= 0 && carry; --i)
    {
        const quint64 sum = quint64(ctr[i]) + (carry & 0xFF);
        ctr[i] = quint8(sum);
        carry = (carry >> 8) + (sum >> 8);
    }
    QByteArray out(data.size(), Qt::Uninitialized);
    quint8 pad[16];
    for (qint64 base = 0; base < data.size(); base += 16)
    {
        aes.encryptBlock(ctr, pad);
        const qint64 n = qMin<qint64>(16, data.size() - base);
        for (qint64 i = 0; i < n; ++i)
            out[int(base + i)] = char(quint8(data[int(base + i)]) ^ pad[i]);
        for (int i = 15; i >= 0; --i) if (++ctr[i]) break; // big-endian increment
    }
    return out;
}

std::optional<QVector<Entry>> entries(const QString&) { return std::nullopt; } // Task 2

bool verifyInstalled(const QString&, const QVector<Entry>&) { return false; }  // Task 3

} // namespace Ps3Pkg
```

- [ ] **Step 5: Run the tests, verify they pass**

Run: `cmake --build build --config Release --target probe_ps3update --parallel && build/Release/probe_ps3update.exe`
Expected: `PS3UPDATE-OK` (all existing tests plus testPkgCrypt green). If a KAT fails, the AES
transcription is wrong — fix the implementation, never the vector (the vectors come from .NET AES).

Also build the main app to prove the CMake registration compiles there too:
`cmake --build build --config Release --parallel`

- [ ] **Step 6: Commit**

```bash
git add native/src/core/ps3/Ps3Pkg.h native/src/core/ps3/Ps3Pkg.cpp native/CMakeLists.txt native/tools/probe_ps3update.cpp
git commit -m "feat: Ps3Pkg module — AES-128-CTR with the retail GPKG key, KAT-pinned"
```

---

### Task 2: `Ps3Pkg::entries()` — header parse + entry-table decrypt with synthetic-pkg fixtures

**Files:**
- Modify: `native/src/core/ps3/Ps3Pkg.cpp` (replace the `entries` stub)
- Test: `native/tools/probe_ps3update.cpp`

**Interfaces:**
- Consumes: `Ps3Pkg::gpkgCrypt` (Task 1).
- Produces: `Ps3Pkg::entries(const QString& pkgPath) -> std::optional<QVector<Ps3Pkg::Entry>>` — order
  preserved from the table; nullopt on any sanity failure. Task 3's `verifyInstalled` and Task 6's
  EmulatorManager wiring consume the `QVector<Entry>`.

- [ ] **Step 1: Write the failing tests**

Add to `native/tools/probe_ps3update.cpp` (and call `testPkgEntries();` in `main()` after
`testPkgCrypt();`):

```cpp
// Assemble a retail-shaped pkg: header + AES-CTR-encrypted data area (entry table, then the name
// blob, then payload bytes). Field layout is the one the live A0130.pkg parse validated 2026-08-19.
// dataOffset is deliberately NOT 0x80 (real pkgs use 0x190) so a parser that assumes the data area
// abuts the header goes red.
struct PkgFixtureEntry { QByteArray name; QByteArray data; quint32 type; };
static QByteArray makePkg(const QVector<PkgFixtureEntry>& items, const QByteArray& riv,
                          quint32 itemCountOverride = 0xFFFFFFFF)
{
    auto be32 = [](quint32 v) { char b[4]; qToBigEndian(v, b); return QByteArray(b, 4); };
    auto be64 = [](quint64 v) { char b[8]; qToBigEndian(v, b); return QByteArray(b, 8); };

    const qint64 tableSize = qint64(items.size()) * 32;
    QByteArray names, blobs;
    QVector<quint32> nameOffs;
    QVector<quint64> dataOffs;
    for (const auto& it : items) { nameOffs << quint32(tableSize + names.size()); names += it.name; }
    const qint64 blobBase = tableSize + names.size();
    for (const auto& it : items) { dataOffs << quint64(blobBase + blobs.size()); blobs += it.data; }

    QByteArray table;
    for (int i = 0; i < items.size(); ++i)
    {
        table += be32(nameOffs[i]);
        table += be32(quint32(items[i].name.size()));
        table += be64(dataOffs[i]);
        table += be64(quint64(items[i].data.size()));
        table += be32(items[i].type);
        table += be32(0);
    }
    const QByteArray data = table + names + blobs;

    const quint64 dataOffset = 0x90; // header 0x80 + 0x10 slack: the parser must READ the field
    QByteArray hdr(int(dataOffset), '\xAA');
    hdr[0] = 0x7F; hdr[1] = 'P'; hdr[2] = 'K'; hdr[3] = 'G';
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 1; // pkg type 0x0001 = PS3
    const quint32 itemCount =
        itemCountOverride != 0xFFFFFFFF ? itemCountOverride : quint32(items.size());
    hdr.replace(0x14, 4, be32(itemCount));
    hdr.replace(0x20, 8, be64(dataOffset));
    hdr.replace(0x28, 8, be64(quint64(data.size())));
    const QByteArray cid = QByteArray("UP9000-BCUS98148_00-GLBPPATCH0000001").leftJustified(0x30, '\0');
    hdr.replace(0x30, 0x30, cid);
    hdr.replace(0x70, 16, riv);
    return hdr + Ps3Pkg::gpkgCrypt(data, riv, 0);
}

static QVector<PkgFixtureEntry> lbpShapedItems()
{
    // The A0130.pkg shape in miniature: overwrite files, a non-overwrite icon, dirs, NPDRM EBOOT,
    // SDAT — including the very entry that was 0 bytes on hardware.
    return {
        { "PARAM.SFO",         QByteArray(12, 'S'),  0x80000003u },
        { "ICON0.PNG",         QByteArray(5, 'I'),   0x00000003u }, // no overwrite bit
        { "USRDIR",            {},                    0x80000004u }, // dir
        { "USRDIR/output",     {},                    0x80000004u }, // dir, no file inside
        { "USRDIR/EBOOT.BIN",  QByteArray(100, 'E'), 0x80000101u }, // NPDRM (type & 0xFF == 1)
        { "USRDIR/patch.sdat", QByteArray(33, 'D'),  0x80000009u }, // SDAT
        { "USRDIR/empty.bin",  {},                    0x80000003u }, // legit 0-byte payload
    };
}

static void testPkgEntries()
{
    QTemporaryDir tmp; CHECK(tmp.isValid());
    const QByteArray riv = QByteArray::fromHex("4309f292bd44abd6788293457fd4a8a4");
    auto writePkg = [&](const QString& name, const QByteArray& bytes) {
        const QString p = tmp.path() + '/' + name;
        QFile f(p); CHECK(f.open(QIODevice::WriteOnly)); f.write(bytes); return p;
    };

    const QString good = writePkg("good.pkg", makePkg(lbpShapedItems(), riv));
    const auto got = Ps3Pkg::entries(good);
    CHECK(got.has_value());
    if (got)
    {
        CHECK(got->size() == 7);
        CHECK((*got)[0].path == QStringLiteral("PARAM.SFO"));
        CHECK((*got)[0].size == 12);
        CHECK((*got)[0].overwrite); CHECK(!(*got)[0].isDir);
        CHECK(!(*got)[1].overwrite);                       // ICON0.PNG carries no overwrite bit
        CHECK((*got)[2].isDir); CHECK((*got)[3].isDir);
        CHECK((*got)[4].path == QStringLiteral("USRDIR/EBOOT.BIN"));
        CHECK((*got)[4].size == 100); CHECK(!(*got)[4].isDir); // NPDRM low byte 0x01 is a FILE
        CHECK((*got)[5].path == QStringLiteral("USRDIR/patch.sdat"));
        CHECK((*got)[5].size == 33);                        // SDAT low byte 0x09 is a FILE
        CHECK((*got)[6].size == 0);
    }

    // Not a pkg / torn pkg → nullopt (fallback path), never a crash.
    CHECK(!Ps3Pkg::entries(writePkg("nomagic.pkg", QByteArray("garbage bytes"))).has_value());
    CHECK(!Ps3Pkg::entries(tmp.path() + QStringLiteral("/absent.pkg")).has_value());
    QByteArray truncated = makePkg(lbpShapedItems(), riv);
    truncated.truncate(0x60);
    CHECK(!Ps3Pkg::entries(writePkg("trunc.pkg", truncated)).has_value());

    // A non-PS3 pkg type must not be decrypted with the PS3 key.
    QByteArray psp = makePkg(lbpShapedItems(), riv);
    psp[7] = 2; // pkg type 0x0002 (PSP)
    CHECK(!Ps3Pkg::entries(writePkg("psp.pkg", psp)).has_value());

    // Wrong riv in the header = the table decrypts to garbage = names fail sanity → nullopt.
    // This is the self-guard that routes debug/foreign pkgs into the fallback instead of
    // "verifying" against noise.
    QByteArray wrongRiv = makePkg(lbpShapedItems(), riv);
    wrongRiv.replace(0x70, 16, QByteArray(16, '\x42'));
    CHECK(!Ps3Pkg::entries(writePkg("wrongriv.pkg", wrongRiv)).has_value());

    // An item count pointing past the data area is garbage, not a huge loop.
    CHECK(!Ps3Pkg::entries(writePkg("count.pkg",
        makePkg(lbpShapedItems(), riv, 0x00FFFFFF))).has_value());

    // Escaping names must poison the whole table: a verifier must never stat outside gameDir.
    for (const char* evil : { "../evil.bin", "/abs.bin", "USR\\DIR.bin" })
    {
        auto items = lbpShapedItems();
        items[5].name = evil;
        CHECK(!Ps3Pkg::entries(writePkg(QStringLiteral("evil.pkg"), makePkg(items, riv))).has_value());
    }
}
```

- [ ] **Step 2: Run to verify the new tests fail**

Run: `cmake --build build --config Release --target probe_ps3update --parallel && build/Release/probe_ps3update.exe`
Expected: FAIL — `got.has_value()` red (stub returns nullopt).

- [ ] **Step 3: Implement entries()**

Replace the stub in `native/src/core/ps3/Ps3Pkg.cpp`:

```cpp
namespace { // add to the existing anonymous namespace

quint16 be16(const QByteArray& b, int off) { return qFromBigEndian<quint16>(b.constData() + off); }
quint32 be32(const QByteArray& b, int off) { return qFromBigEndian<quint32>(b.constData() + off); }
quint64 be64(const QByteArray& b, int off) { return qFromBigEndian<quint64>(b.constData() + off); }

// Decrypt [off, off+len) of the data area: read the covering 16-byte blocks, transform with the
// counter positioned at the FIRST covered block, slice out the requested window.
QByteArray decryptRegion(QFile& f, quint64 dataOffset, const QByteArray& riv, quint64 off, quint64 len)
{
    const quint64 blockFirst = off / 16;
    const quint64 padded = (off + len + 15) / 16 * 16 - blockFirst * 16;
    if (!f.seek(qint64(dataOffset + blockFirst * 16))) return {};
    const QByteArray ct = f.read(qint64(padded));
    if (quint64(ct.size()) < padded) return {};
    const QByteArray pt = Ps3Pkg::gpkgCrypt(ct, riv, qint64(blockFirst));
    return pt.mid(int(off - blockFirst * 16), int(len));
}

} // namespace

std::optional<QVector<Entry>> entries(const QString& pkgPath)
{
    QFile f(pkgPath);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray hdr = f.read(0x80);
    if (hdr.size() < 0x80) return std::nullopt;
    if (be32(hdr, 0x00) != 0x7F504B47u) return std::nullopt; // "\x7FPKG"
    if (be16(hdr, 0x06) != 0x0001) return std::nullopt;      // PS3 — the GPKG key is only theirs
    const quint32 itemCount  = be32(hdr, 0x14);
    const quint64 dataOffset = be64(hdr, 0x20);
    const quint64 dataSize   = be64(hdr, 0x28);
    const QByteArray riv = hdr.mid(0x70, 16);
    // A real update pkg names tens of entries; 100k is far past any genuine table and caps the
    // work a corrupt count can demand.
    if (itemCount == 0 || itemCount > 100000) return std::nullopt;
    if (dataOffset < 0x80 || dataOffset + dataSize > quint64(f.size())) return std::nullopt;
    if (quint64(itemCount) * 32 > dataSize) return std::nullopt;

    const QByteArray table = decryptRegion(f, dataOffset, riv, 0, quint64(itemCount) * 32);
    if (quint64(table.size()) != quint64(itemCount) * 32) return std::nullopt;

    QVector<Entry> out;
    out.reserve(int(itemCount));
    for (quint32 i = 0; i < itemCount; ++i)
    {
        const int o = int(i) * 32;
        const quint32 nameOff  = be32(table, o);
        const quint32 nameSize = be32(table, o + 4);
        const quint64 fileSize = be64(table, o + 16);
        const quint32 type     = be32(table, o + 24);
        if (nameSize == 0 || nameSize > 4096) return std::nullopt;
        if (quint64(nameOff) + nameSize > dataSize) return std::nullopt;
        const QByteArray nameBytes = decryptRegion(f, dataOffset, riv, nameOff, nameSize);
        if (quint32(nameBytes.size()) != nameSize) return std::nullopt;

        Entry e;
        // Trailing NULs are padding; anything else must be a clean RELATIVE path. Garbage here means
        // the key did not decrypt this table (debug pkg, foreign platform, corruption) — poison the
        // whole parse rather than verify against noise. '..'/'\\'/leading-'/' would also let a
        // hostile table walk the verifier outside gameDir.
        QByteArray name = nameBytes;
        while (name.endsWith('\0')) name.chop(1);
        if (name.isEmpty() || name.startsWith('/') || name.contains('\\')) return std::nullopt;
        for (const char c : name) if (quint8(c) < 0x20) return std::nullopt;
        const QString path = QString::fromUtf8(name);
        if (path.contains(QStringLiteral("../")) || path == QStringLiteral("..")
            || path.contains(QChar(0xFFFD))) return std::nullopt; // 0xFFFD: not valid UTF-8
        e.path = path;
        e.size = qint64(fileSize);
        const quint8 low = quint8(type & 0xFF);
        e.isDir = (low == 0x04 || low == 0x12); // unpkg.cpp's two folder cases
        e.overwrite = (type & 0x80000000u) != 0;
        if (!e.isDir && quint64(be64(table, o + 8)) + fileSize > dataSize) return std::nullopt;
        out.append(e);
    }
    return out;
}
```

- [ ] **Step 4: Run the tests, verify they pass**

Run: `cmake --build build --config Release --target probe_ps3update --parallel && build/Release/probe_ps3update.exe`
Expected: `PS3UPDATE-OK`

- [ ] **Step 5: Commit**

```bash
git add native/src/core/ps3/Ps3Pkg.cpp native/tools/probe_ps3update.cpp
git commit -m "feat: parse a retail pkg's encrypted entry table (names, sizes, dir/overwrite flags)"
```

---

### Task 3: `Ps3Pkg::verifyInstalled` + `Ps3InstalledVersion::hasZeroByteFile` — the poisoned-install case

**Files:**
- Modify: `native/src/core/ps3/Ps3Pkg.cpp` (replace the `verifyInstalled` stub)
- Modify: `native/src/core/ps3/Ps3InstalledVersion.h` / `.cpp` (add `hasZeroByteFile`)
- Test: `native/tools/probe_ps3update.cpp`

**Interfaces:**
- Consumes: `Ps3Pkg::Entry`, `Ps3Pkg::entries` (Task 2); `Ps3InstalledVersion::reachedTarget`,
  `restoreSfo`, `snapshotSfo` (existing).
- Produces: `Ps3Pkg::verifyInstalled(const QString& gameDir, const QVector<Entry>&) -> bool`;
  `Ps3InstalledVersion::hasZeroByteFile(const QString& dir) -> bool` (recursive; false for a
  missing/empty dir). Task 6 wires both.

- [ ] **Step 1: Write the failing tests**

Add to `native/tools/probe_ps3update.cpp` (call `testVerifyInstalled();` and
`testHasZeroByteFile();` in `main()` after `testPkgEntries();`):

```cpp
static void materialize(const QString& gameDir, const QVector<PkgFixtureEntry>& items)
{
    for (const auto& it : items)
    {
        const QString p = gameDir + '/' + QString::fromUtf8(it.name);
        if ((it.type & 0xFF) == 0x04) { QDir().mkpath(p); continue; }
        QDir().mkpath(QFileInfo(p).path());
        QFile f(p); f.open(QIODevice::WriteOnly); f.write(it.data);
    }
}

// The verification a success verdict now owes the tree: every table entry present at its expected
// size. The hardware failure this pins (2026-08-19, BCUS98148): PARAM.SFO claimed APP_VER=01.30
// while USRDIR/patch.sdat sat at 0 bytes and 6 files were missing entirely — version+quiescence
// passed and the game crashed ~14s after boot when the EBOOT loaded the empty sdat.
static void testVerifyInstalled()
{
    QTemporaryDir tmp; CHECK(tmp.isValid());
    const auto items = lbpShapedItems();
    const QByteArray riv = QByteArray::fromHex("4309f292bd44abd6788293457fd4a8a4");
    const QString pkgPath = tmp.path() + QStringLiteral("/u.pkg");
    { QFile f(pkgPath); CHECK(f.open(QIODevice::WriteOnly)); f.write(makePkg(items, riv)); }
    const auto table = Ps3Pkg::entries(pkgPath);
    CHECK(table.has_value());
    if (!table) return;

    const QString g = tmp.path() + QStringLiteral("/game");
    materialize(g, items);
    CHECK(Ps3Pkg::verifyInstalled(g, *table)); // complete install verifies

    // Extra files RPCS3 didn't write (runtime game data, older update leftovers) are none of the
    // table's business.
    { QFile f(g + QStringLiteral("/USRDIR/leftover.bin"));
      CHECK(f.open(QIODevice::WriteOnly)); f.write("x"); }
    CHECK(Ps3Pkg::verifyInstalled(g, *table));

    // THE poisoned-install case: the expected-910064-byte sdat truncated to 0 bytes while
    // PARAM.SFO still claims the target — must FAIL even though every path exists.
    { QFile f(g + QStringLiteral("/USRDIR/patch.sdat"));
      CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); }
    CHECK(!Ps3Pkg::verifyInstalled(g, *table));
    { QFile f(g + QStringLiteral("/USRDIR/patch.sdat"));
      CHECK(f.open(QIODevice::WriteOnly)); f.write(QByteArray(33, 'D')); }
    CHECK(Ps3Pkg::verifyInstalled(g, *table)); // healed

    // A missing file — the other half of the hardware poison.
    CHECK(QFile::remove(g + QStringLiteral("/USRDIR/EBOOT.BIN")));
    CHECK(!Ps3Pkg::verifyInstalled(g, *table));
    { QFile f(g + QStringLiteral("/USRDIR/EBOOT.BIN"));
      CHECK(f.open(QIODevice::WriteOnly)); f.write(QByteArray(100, 'E')); }
    CHECK(Ps3Pkg::verifyInstalled(g, *table));

    // An overwrite entry at the WRONG size (torn mid-write, then killed) fails too.
    { QFile f(g + QStringLiteral("/USRDIR/EBOOT.BIN"));
      CHECK(f.open(QIODevice::Append)); f.write("tail"); }
    CHECK(!Ps3Pkg::verifyInstalled(g, *table));
    { QFile f(g + QStringLiteral("/USRDIR/EBOOT.BIN"));
      CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write(QByteArray(100, 'E')); }

    // A NON-overwrite entry may keep a pre-existing file of a different size — RPCS3's
    // "Didn't overwrite" path (unpkg.cpp) skips it, so a size mismatch there is legitimate…
    { QFile f(g + QStringLiteral("/ICON0.PNG"));
      CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write(QByteArray(999, 'O')); }
    CHECK(Ps3Pkg::verifyInstalled(g, *table));
    // …but 0 bytes where the table expects content is never legitimate, overwrite bit or not.
    { QFile f(g + QStringLiteral("/ICON0.PNG"));
      CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); }
    CHECK(!Ps3Pkg::verifyInstalled(g, *table));
    { QFile f(g + QStringLiteral("/ICON0.PNG"));
      CHECK(f.open(QIODevice::WriteOnly)); f.write(QByteArray(5, 'I')); }

    // A 0-byte file the table EXPECTS at 0 bytes is fine (some updates ship empty markers).
    CHECK(Ps3Pkg::verifyInstalled(g, *table)); // USRDIR/empty.bin is 0 bytes by design

    // A directory entry the install never produced.
    CHECK(QDir(g + QStringLiteral("/USRDIR/output")).removeRecursively());
    CHECK(!Ps3Pkg::verifyInstalled(g, *table));
    QDir().mkpath(g + QStringLiteral("/USRDIR/output"));
    CHECK(Ps3Pkg::verifyInstalled(g, *table));

    // The wiring rule the installer lambda applies (Task 6), composed here where a probe can reach
    // it: version-at-target alone said "done", the table says otherwise, and the restore then
    // un-tells the lie so the next launch re-runs the chain.
    const QByteArray priorSfo = makeSfo({ { "APP_VER", "01.02" }, { "TITLE_ID", "BCUS98148" } });
    { QFile f(g + QStringLiteral("/PARAM.SFO"));
      CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write(priorSfo); }
    const QByteArray entrySnap = Ps3InstalledVersion::snapshotSfo(g);
    { QFile f(g + QStringLiteral("/PARAM.SFO")); CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
      f.write(makeSfo({ { "APP_VER", "01.30" }, { "TITLE_ID", "BCUS98148" } })); }
    { QFile f(g + QStringLiteral("/USRDIR/patch.sdat"));
      CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); } // the 0-byte poison
    CHECK(Ps3InstalledVersion::reachedTarget(g, QStringLiteral("01.30"))); // the old check passes…
    CHECK(!Ps3Pkg::verifyInstalled(g, *table));                            // …the table does not
    Ps3InstalledVersion::restoreSfo(g, entrySnap);
    CHECK(!Ps3InstalledVersion::reachedTarget(g, QStringLiteral("01.30"))); // lie undone
}

static void testHasZeroByteFile()
{
    QTemporaryDir tmp; CHECK(tmp.isValid());
    // Missing or empty dirs hold no poison — this must never make a fresh install look poisoned.
    CHECK(!Ps3InstalledVersion::hasZeroByteFile(tmp.path() + QStringLiteral("/absent")));
    const QString d = tmp.path() + QStringLiteral("/usr");
    QDir().mkpath(d + QStringLiteral("/deep"));
    CHECK(!Ps3InstalledVersion::hasZeroByteFile(d));
    { QFile f(d + QStringLiteral("/deep/ok.bin")); CHECK(f.open(QIODevice::WriteOnly)); f.write("x"); }
    CHECK(!Ps3InstalledVersion::hasZeroByteFile(d));
    { QFile f(d + QStringLiteral("/deep/poison.sdat")); CHECK(f.open(QIODevice::WriteOnly)); }
    CHECK(Ps3InstalledVersion::hasZeroByteFile(d));
}
```

- [ ] **Step 2: Run to verify the new tests fail**

Run: `cmake --build build --config Release --target probe_ps3update --parallel && build/Release/probe_ps3update.exe`
Expected: FAIL (verifyInstalled stub returns false; hasZeroByteFile doesn't exist → compile error
first — add the declaration, then watch asserts fail).

- [ ] **Step 3: Implement**

Replace the `verifyInstalled` stub in `native/src/core/ps3/Ps3Pkg.cpp`:

```cpp
bool verifyInstalled(const QString& gameDir, const QVector<Entry>& entries)
{
    const QDir root(gameDir);
    for (const Entry& e : entries)
    {
        const QFileInfo fi(root.filePath(e.path));
        if (e.isDir)
        {
            if (!fi.isDir()) return false;
            continue;
        }
        if (!fi.isFile()) return false;
        const qint64 sz = fi.size();
        if (sz == e.size) continue;
        // RPCS3 skips a non-overwrite entry when the file pre-exists ("Didn't overwrite"), so a
        // size mismatch there can be a legitimately kept older file — but only a NON-EMPTY one.
        // 0 bytes where the table expects content is the 2026-08-19 poison, never a kept file.
        if (!e.overwrite && sz > 0) continue;
        return false;
    }
    return true;
}
```

In `native/src/core/ps3/Ps3InstalledVersion.h`, add after the `dirFingerprint` declaration:

```cpp
// true iff some regular file under dir (recursive) is exactly 0 bytes. The pkg-less poison
// heuristic: when there is no entry table to verify against (package not downloaded yet, or a pkg
// the parser rejected), a 0-byte file under the update's USRDIR is the one partial-install shape
// that is cheap to see and never legitimate for a file an update wrote (hardware 2026-08-19:
// patch.sdat at 0 bytes crashed the game 14s after boot). A missing/empty dir has no poison.
bool hasZeroByteFile(const QString& dir);
```

In `native/src/core/ps3/Ps3InstalledVersion.cpp`, add after `dirFingerprint`:

```cpp
bool hasZeroByteFile(const QString& dir)
{
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        if (it.fileInfo().size() == 0) return true;
    }
    return false;
}
```

- [ ] **Step 4: Run the tests, verify they pass**

Run: `cmake --build build --config Release --target probe_ps3update --parallel && build/Release/probe_ps3update.exe`
Expected: `PS3UPDATE-OK`

- [ ] **Step 5: Commit**

```bash
git add native/src/core/ps3/Ps3Pkg.cpp native/src/core/ps3/Ps3InstalledVersion.h native/src/core/ps3/Ps3InstalledVersion.cpp native/tools/probe_ps3update.cpp
git commit -m "feat: verify an installed PS3 update against its pkg entry table; 0-byte poison heuristic"
```

---

### Task 4: `Ps3UpdateCoordinator` — optional `InstallIntact` seam (heal when state claims current)

**Files:**
- Modify: `native/src/core/ps3/Ps3UpdateCoordinator.h`
- Modify: `native/src/core/ps3/Ps3UpdateCoordinator.cpp`
- Test: `native/tools/probe_ps3update.cpp` (extend `testCoordinator`)

**Interfaces:**
- Produces: 6th optional ctor param `Ps3UpdateCoordinator::InstallIntact =
  std::function<bool(const QString& titleId)>` — consulted ONLY when the state file says no update
  is needed; returning false forces the chain to run anyway. Task 6 wires it (marker-guarded).
  All existing 5-arg call sites keep compiling (defaulted param).

- [ ] **Step 1: Write the failing test**

Extend `testCoordinator()` in `native/tools/probe_ps3update.cpp` — append at the end of the function:

```cpp
    // The heal seam: the state file can RECORD a success the tree does not hold (the pre-file-table
    // builds did exactly that on 2026-08-19 — ps3-updates.json said 01.30 while patch.sdat sat at
    // 0 bytes, so needsUpdate skipped the chain forever). When state says current but installIntact
    // says the tree is poisoned, the chain must run anyway; when intact, it must not.
    {
        int intactAsked = 0;
        Ps3UpdateCoordinator c(
            [](const QString&) { return std::optional<QString>(QStringLiteral("BLUS31156")); },
            [&](const QString&) { return std::optional<QByteArray>(feed); },
            &state, &installer, progress,
            [&](const QString& titleId) { ++intactAsked; CHECK(titleId == QStringLiteral("BLUS31156")); return false; });
        CHECK(c.maybeUpdate(QStringLiteral("/any/rom"))); // state said current, poison forced the run
        CHECK(intactAsked == 1);
    }
    CHECK(installs == 2); // the heal chain actually installed
    {
        Ps3UpdateCoordinator c(
            [](const QString&) { return std::optional<QString>(QStringLiteral("BLUS31156")); },
            [&](const QString&) { return std::optional<QByteArray>(feed); },
            &state, &installer, progress,
            [](const QString&) { return true; });
        CHECK(!c.maybeUpdate(QStringLiteral("/any/rom"))); // intact + current: nothing to do
    }
    CHECK(installs == 2);
    // And an update that IS needed never asks intact (the chain runs regardless).
    {
        Ps3UpdateState fresh(dir.path() + QStringLiteral("/state2.json"));
        int intactAsked = 0;
        Ps3UpdateCoordinator c(
            [](const QString&) { return std::optional<QString>(QStringLiteral("BLUS31156")); },
            [&](const QString&) { return std::optional<QByteArray>(feed); },
            &fresh, &installer, progress,
            [&](const QString&) { ++intactAsked; return true; });
        CHECK(c.maybeUpdate(QStringLiteral("/any/rom")));
        CHECK(intactAsked == 0);
    }
    CHECK(installs == 3);
```

(Note: `installs`, `state`, `feed`, `installer`, `progress`, `dir` already exist in the function.
The first coordinator block earlier in the test already left `state` marked current at 01.11 with
`installs == 1`.)

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release --target probe_ps3update --parallel`
Expected: compile FAIL (no 6-arg ctor).

- [ ] **Step 3: Implement the seam**

`native/src/core/ps3/Ps3UpdateCoordinator.h` — add the alias, the ctor param, and the member:

```cpp
    using TitleIdReader = std::function<std::optional<QString>(const QString& romPath)>;
    using FeedFetcher   = std::function<std::optional<QByteArray>(const QString& titleId)>;
    using Progress      = std::function<void(const QString& message)>;
    // Consulted ONLY when the state file says the title is current: false means the on-disk install
    // is visibly poisoned (e.g. a 0-byte file under its USRDIR) and the chain must run anyway. The
    // state file records what an installer CLAIMED; the pre-file-table builds (≤2026-08-19) could
    // claim success over a truncated tree, and that record otherwise gates the heal out forever.
    // Absent = trust the state file (the pre-heal behavior).
    using InstallIntact = std::function<bool(const QString& titleId)>;

    Ps3UpdateCoordinator(TitleIdReader readId, FeedFetcher fetch,
                         Ps3UpdateState* state, Ps3UpdateInstaller* installer, Progress progress,
                         InstallIntact intact = {});
```

and the member `InstallIntact intact_;` beside the others.

`native/src/core/ps3/Ps3UpdateCoordinator.cpp`:

```cpp
Ps3UpdateCoordinator::Ps3UpdateCoordinator(TitleIdReader readId, FeedFetcher fetch,
                                           Ps3UpdateState* state, Ps3UpdateInstaller* installer, Progress progress,
                                           InstallIntact intact)
    : readId_(std::move(readId)), fetch_(std::move(fetch)), state_(state), installer_(installer),
      progress_(std::move(progress)), intact_(std::move(intact)) {}
```

and replace line 21's gate:

```cpp
    if (!state_) return false;
    // State current is only half the verdict: the record says what an installer CLAIMED. When the
    // tree is visibly poisoned (intact_ says so), run the chain anyway — the per-package
    // already-applied and file-table checks downstream then drive a real heal, and pkg entries
    // overwrite in place.
    if (!state_->needsUpdate(*titleId, latest) && (!intact_ || intact_(*titleId))) return false;
```

- [ ] **Step 4: Run the tests, verify they pass**

Run: `cmake --build build --config Release --target probe_ps3update --parallel && build/Release/probe_ps3update.exe`
Expected: `PS3UPDATE-OK`

- [ ] **Step 5: Commit**

```bash
git add native/src/core/ps3/Ps3UpdateCoordinator.h native/src/core/ps3/Ps3UpdateCoordinator.cpp native/tools/probe_ps3update.cpp
git commit -m "feat: coordinator heal seam — a poisoned tree re-runs the chain past a lying state file"
```

---

### Task 5: probe dev modes `--dump` / `--verify` + validation against the 13 real LBP pkgs

**Files:**
- Modify: `native/tools/probe_ps3update.cpp` (main() argv handling only)

**Interfaces:**
- Consumes: `Ps3Pkg::entries`, `Ps3Pkg::verifyInstalled`.
- Produces: `probe_ps3update --dump <pkg>...` prints each pkg's entry table (or `PARSE-FAIL <path>`);
  `probe_ps3update --verify <pkg> <gameDir>` prints `VERIFY-OK` / `VERIFY-FAIL` and exits 0/1.
  **No-arg behavior is unchanged** (the gate runs it with no args).

- [ ] **Step 1: Implement the argv modes**

Replace `int main()` signature and prepend to its body:

```cpp
int main(int argc, char** argv)
{
    // Dev modes against REAL Sony packages (the gate runs argless and never enters these): --dump
    // prints a pkg's decrypted entry table for eyeballing against ground truth; --verify checks an
    // installed tree against a pkg's table — the exact predicate the installer applies. Hardware
    // truth lives in C:\Users\cubma\rpcs3-bisect\pkgs (13 sha1-verified LBP update pkgs).
    if (argc >= 3 && qstrcmp(argv[1], "--dump") == 0)
    {
        int rc = 0;
        for (int a = 2; a < argc; ++a)
        {
            const auto t = Ps3Pkg::entries(QString::fromLocal8Bit(argv[a]));
            if (!t) { std::printf("PARSE-FAIL %s\n", argv[a]); rc = 1; continue; }
            std::printf("%s: %d entries\n", argv[a], int(t->size()));
            for (const auto& e : *t)
                std::printf("  %s%s size=%lld%s\n", qPrintable(e.path), e.isDir ? "/" : "",
                            static_cast<long long>(e.size), e.overwrite ? "" : " [no-overwrite]");
        }
        return rc;
    }
    if (argc == 4 && qstrcmp(argv[1], "--verify") == 0)
    {
        const auto t = Ps3Pkg::entries(QString::fromLocal8Bit(argv[2]));
        if (!t) { std::printf("PARSE-FAIL %s\n", argv[2]); return 2; }
        const bool ok = Ps3Pkg::verifyInstalled(QString::fromLocal8Bit(argv[3]), *t);
        std::printf(ok ? "VERIFY-OK\n" : "VERIFY-FAIL\n");
        return ok ? 0 : 1;
    }
    // ... existing test calls unchanged ...
```

- [ ] **Step 2: Build and validate against all 13 real pkgs**

```bash
cmake --build build --config Release --target probe_ps3update --parallel
build/Release/probe_ps3update.exe --dump C:/Users/cubma/rpcs3-bisect/pkgs/*.pkg
```
(expand the glob in the shell: `build/Release/probe_ps3update.exe --dump C:/Users/cubma/rpcs3-bisect/pkgs/A0102.pkg ... A0130.pkg` — all 13.)

Expected: every pkg parses (exit 0, no `PARSE-FAIL`); A0130.pkg prints exactly 10 entries matching
the 2026-08-19 PowerShell ground-truth parse, including `USRDIR/patch.sdat size=910064` and
`ICON0.PNG … [no-overwrite]`. **If any real pkg fails to parse, STOP — the format assumption is
wrong for that pkg; investigate before continuing.**

- [ ] **Step 3: Validate against the poisoned hardware tree**

Locate the live RPCS3 game dir (the poisoned one): find `dev_hdd0/game/BCUS98148` under the RPCS3
root that `Ps3Firmware::devFlashRoot(binDir)` resolves for the deployed app (search
`C:\EverythingBox-app` and the emulators dir for `dev_hdd0`). Then:

```bash
build/Release/probe_ps3update.exe --verify C:/Users/cubma/rpcs3-bisect/pkgs/A0130.pkg "<that gameDir>"
```

Expected: `VERIFY-FAIL` (exit 1) — the real 2026-08-19 poison (0-byte patch.sdat, 6 missing files)
must fail the real check. Record which entries are the missing 6 (compare `--dump` output against
`dir`) in the task notes. If it prints VERIFY-OK the predicate is broken — stop and debug.

- [ ] **Step 4: Run the argless probe (gate behavior unchanged)**

Run: `build/Release/probe_ps3update.exe`
Expected: `PS3UPDATE-OK`

- [ ] **Step 5: Commit**

```bash
git add native/tools/probe_ps3update.cpp
git commit -m "feat: probe_ps3update --dump/--verify dev modes against real Sony pkgs"
```

---

### Task 6: EmulatorManager wiring — every success verdict requires the file table

**Files:**
- Modify: `native/src/core/EmulatorManager.cpp` (the `--installpkg` installer lambda ~lines 1642–1756,
  the AlreadyApplied lambda ~1753, the coordinator construction ~1757, plus the include block ~line 42)

**Interfaces:**
- Consumes: `Ps3Pkg::entries`, `Ps3Pkg::verifyInstalled`, `Ps3InstalledVersion::hasZeroByteFile`,
  `Ps3UpdateCoordinator::InstallIntact` (Tasks 1–4).
- Produces: no new interfaces — behavior only.

- [ ] **Step 1: Add the include**

Beside the other ps3 includes (~line 42): `#include "core/ps3/Ps3Pkg.h"`.

- [ ] **Step 2: Rewire the installer lambda**

Inside the `[binDir](const QString& exe, const QString& pkg, ...)` installer lambda, right after
`gameDir` is computed (line ~1659), insert:

```cpp
                // The airtight success criterion (hardware 2026-08-19, BCUS98148): the pkg's own
                // entry table names every file this install must produce, with sizes. An install
                // left PARAM.SFO claiming APP_VER=01.30 over a 0-byte USRDIR/patch.sdat and 6
                // missing files — the version check and the quiet-window fingerprint both passed,
                // and the game crashed ~14s after boot. Version-at-target is now only the fast
                // pre-filter; every success verdict below also requires the tree to match the
                // table. When the table can't be parsed (foreign/debug pkg — retail updates all
                // parse), the defensible fallback is "no 0-byte file under USRDIR".
                const std::optional<QVector<Ps3Pkg::Entry>> expected = Ps3Pkg::entries(pkg);
                const QString usrDir = gameDir + QStringLiteral("/USRDIR");
                const auto installVerified = [&gameDir, &usrDir, &expected] {
                    return expected ? Ps3Pkg::verifyInstalled(gameDir, *expected)
                                    : !Ps3InstalledVersion::hasZeroByteFile(usrDir);
                };
```

Then change each decision site:

**(a) Pre-spawn skip (line ~1664)** — from
`if (Ps3InstalledVersion::reachedTarget(gameDir, version)) return 0;` to:

```cpp
                // Already on disk: the disk state IS the result, so don't spawn at all — but only
                // when the tree actually matches this pkg's table. A version at target over a
                // hole-y tree is the 2026-08-19 poison; running the installer anyway is the heal
                // (pkg entries overwrite in place). The strict check can also fire on a
                // hand-installed NEWER update (its sizes differ from this older table) — that
                // reinstall converges to the feed's newest and is the price of airtight.
                if (Ps3InstalledVersion::reachedTarget(gameDir, version) && installVerified())
                    return 0;
```

**(b) Self-exit branch (line ~1691)** — replace the return (and its "No rollback here,
deliberately" comment) with:

```cpp
                    if (proc.waitForFinished(500)) // it exits on its own: the headless path
                    {
                        // A self-exit is no longer trusted on version alone: hardware 2026-08-19
                        // exited "cleanly" leaving a 0-byte patch.sdat and 6 missing files behind
                        // PARAM.SFO's target claim. Verified => done. Not verified => nothing this
                        // run wrote is trustworthy — put the entry-state PARAM.SFO back so the
                        // next launch re-runs the chain instead of recording the lie as applied.
                        if (Ps3InstalledVersion::reachedTarget(gameDir, version) && installVerified())
                            return 0;
                        Ps3InstalledVersion::restoreSfo(gameDir, priorSfo);
                        return proc.exitCode() == 0 ? -1 : proc.exitCode();
                    }
```

**(c) Kill branch (line ~1714)** — the `completedDespiteKill` acceptance also requires the table:

```cpp
                        QDeadlineTimer verifyBudget(3000);
                        if (Ps3InstalledVersion::completedDespiteKill(
                                gameDir, version, lastPrint,
                                [&verifyBudget] { return verifyBudget.hasExpired(); })
                            && installVerified())
                            return 0;
```

**(d) Quiet-window kill (line ~1742)** — require the table before the kill is taken as success:

```cpp
                    if (stable.isValid() && stable.elapsed() >= 3000)
                    {
                        // Quiet is necessary, not sufficient: a wedged GUI over a hole-y tree is
                        // quiet too. Only a tree that matches the table earns the kill-as-success;
                        // otherwise keep waiting — the deadline branch restores and fails.
                        if (!installVerified()) continue;
                        proc.waitForFinished(2000); // settle: let the installer close its handles
                        proc.kill();
                        proc.waitForFinished(5000);
                        return 0;
                    }
```

- [ ] **Step 3: Harden the AlreadyApplied lambda (line ~1753)**

```cpp
            // Asked BEFORE each package is downloaded: a retry after a partial chain, or after a
            // lost ps3-updates.json, must not re-pay hundreds of megabytes for an update already
            // on disk. With no pkg on disk yet there is no entry table to check against, so the
            // pre-filter is version + the 0-byte poison heuristic; the full table check runs in
            // the installer seam once the bytes exist.
            [binDir](const QString& titleId, const QString& version) {
                const QString g =
                    Ps3InstalledVersion::gameDir(Ps3Firmware::devFlashRoot(binDir), titleId);
                return Ps3InstalledVersion::reachedTarget(g, version)
                       && !Ps3InstalledVersion::hasZeroByteFile(g + QStringLiteral("/USRDIR"));
            });
```

- [ ] **Step 4: Wire the heal seam into the coordinator (line ~1757)**

```cpp
        // One-shot heal: when ps3-updates.json claims current but the tree holds a 0-byte file
        // under USRDIR (what the pre-file-table builds could record as success), force the chain
        // once. The marker makes it once-EVER per title: a 0-byte file a GAME writes into its own
        // dir at runtime would otherwise re-run the whole multi-hundred-MB chain on every launch.
        // Armed only after the attempt actually ran to completion (not on app-quit interruption —
        // same pattern as the fw-install-failed marker above), so an innocent quit retries.
        QString healAttempted;
        Ps3UpdateCoordinator coord(
            [](const QString& p) { return Ps3TitleId::read(p); },
            [](const QString& titleId) { return fetchPs3VerXml(titleId); },
            &state, &installer, note,
            [binDir, tmpDir, &healAttempted](const QString& titleId) {
                if (QFile::exists(QDir(tmpDir).filePath(QStringLiteral("ps3-heal-") + titleId)))
                    return true; // one heal attempt per title, ever
                const QString g =
                    Ps3InstalledVersion::gameDir(Ps3Firmware::devFlashRoot(binDir), titleId);
                if (!Ps3InstalledVersion::hasZeroByteFile(g + QStringLiteral("/USRDIR")))
                    return true;
                healAttempted = titleId;
                return false;
            });
        coord.maybeUpdate(rom); // result ignored — always fall through to a boot
        if (!healAttempted.isEmpty() && !QThread::currentThread()->isInterruptionRequested())
        {
            QFile m(QDir(tmpDir).filePath(QStringLiteral("ps3-heal-") + healAttempted));
            m.open(QIODevice::WriteOnly);
        }
```

- [ ] **Step 5: Build everything + run the probe suite**

Run: `cmake --build build --config Release --parallel`
Expected: clean build (this task compiles EmulatorManager.cpp — the probes don't link it).
Run: `build/Release/probe_ps3update.exe` → `PS3UPDATE-OK`.

- [ ] **Step 6: Commit**

```bash
git add native/src/core/EmulatorManager.cpp
git commit -m "fix: every PS3 update success verdict now requires the pkg's file table on disk"
```

---

### Task 7: Mutation sanity — extend mutate-ps3pkgverify.json

**Files:**
- Modify: `native/tools/mutate-ps3pkgverify.json`

**Interfaces:** none (tooling config).

- [ ] **Step 1: Add the mutants**

Append to the `mutants` array (adjust `find` strings to the exact committed source — they must match
byte-for-byte; `count: 1` enforces uniqueness):

```json
    { "name": "verify-size-check-dropped",
      "file": "native/src/core/ps3/Ps3Pkg.cpp",
      "find": "        if (sz == e.size) continue;",
      "replace": "        continue; // mutated: any existing file passes",
      "count": 1, "expect": "killed" },

    { "name": "verify-existence-dropped",
      "file": "native/src/core/ps3/Ps3Pkg.cpp",
      "find": "        if (!fi.isFile()) return false;",
      "replace": "        if (!fi.isFile()) continue; // mutated: missing files pass",
      "count": 1, "expect": "killed" },

    { "name": "verify-zero-byte-accepted",
      "file": "native/src/core/ps3/Ps3Pkg.cpp",
      "find": "        if (!e.overwrite && sz > 0) continue;",
      "replace": "        if (!e.overwrite) continue; // mutated: 0-byte non-overwrite passes",
      "count": 1, "expect": "killed" },

    { "name": "verify-dirs-ignored",
      "file": "native/src/core/ps3/Ps3Pkg.cpp",
      "find": "            if (!fi.isDir()) return false;",
      "replace": "            { } // mutated: missing dirs pass",
      "count": 1, "expect": "killed" },

    { "name": "crypt-counter-not-positioned",
      "file": "native/src/core/ps3/Ps3Pkg.cpp",
      "find": "    quint64 carry = quint64(blockOffset);",
      "replace": "    quint64 carry = 0; // mutated: blockOffset ignored",
      "count": 1, "expect": "killed" },

    { "name": "entries-name-sanity-dropped",
      "file": "native/src/core/ps3/Ps3Pkg.cpp",
      "find": "        if (name.isEmpty() || name.startsWith('/') || name.contains('\\\\')) return std::nullopt;",
      "replace": "        { } // mutated: any name accepted",
      "count": 1, "expect": "killed" },

    { "name": "haszero-always-clean",
      "file": "native/src/core/ps3/Ps3InstalledVersion.cpp",
      "find": "        if (it.fileInfo().size() == 0) return true;",
      "replace": "        ; // mutated: 0-byte files invisible",
      "count": 1, "expect": "killed" },

    { "name": "coordinator-heal-seam-ignored",
      "file": "native/src/core/ps3/Ps3UpdateCoordinator.cpp",
      "find": "    if (!state_->needsUpdate(*titleId, latest) && (!intact_ || intact_(*titleId))) return false;",
      "replace": "    if (!state_->needsUpdate(*titleId, latest)) return false; // mutated: state trusted blindly",
      "count": 1, "expect": "killed" }
```

(If an exact `find` doesn't match the committed code, fix the JSON to the real bytes — never bend
the code to the JSON. Mind JSON escaping: the `'\\\\'` above is source `'\\'`.)

- [ ] **Step 2: Run the mutation driver**

Run: `python native/tools/mutate.py native/tools/mutate-ps3pkgverify.json`
Expected: every mutant (the 5 pre-existing + 8 new) reports **KILLED**. A SURVIVED means a probe
gap — add the missing CHECK, don't weaken the mutant. Remember the harness lesson: a NOT-BUILT or
baseline failure is a config problem, not a kill.

- [ ] **Step 3: Commit**

```bash
git add native/tools/mutate-ps3pkgverify.json
git commit -m "test: mutation-sanity for the pkg file-table verification"
```

---

### Task 8: Full gate, review, merge, deploy, live heal verification

- [ ] **Step 1: Full Release build + headless gate**

```bash
cmake --build build --config Release --parallel
bash native/tools/run-headless-probes.sh
```
Expected: `ALL HEADLESS PROBES PASSED`. (Post-merge convention: rebuild-all-and-grep-errors.)

- [ ] **Step 2: Fable review** — dispatch the final code review (Fable, per repo convention) over
the full diff vs origin/main; fix anything Critical/Important, re-run the gate.

- [ ] **Step 3: Merge + push** (standing autonomy; no finish-branch menu):

```bash
git fetch origin && git merge origin/main   # reconcile if main moved; bash -n any merged gate scripts
bash native/tools/run-headless-probes.sh    # gate again if a merge happened
git checkout main && git pull && git merge --no-ff <branch> && git push origin main
```

- [ ] **Step 4: Deploy** Release `EverythingBox.exe` to `C:\EverythingBox-app` (close the running
app first — standing autonomy; Release config, never Debug).

- [ ] **Step 5: Live heal verification on the poisoned hardware state**
  - Confirm pre-state: `dev_hdd0/game/BCUS98148/USRDIR/patch.sdat` is 0 bytes; ps3-updates.json
    claims BCUS98148 current at 01.30.
  - Launch LBP through EverythingBox (or trigger the update worker); the intact seam must fire
    (0-byte under USRDIR + no marker) and re-run the chain.
  - After the chain: `probe_ps3update.exe --verify C:/Users/cubma/rpcs3-bisect/pkgs/A0130.pkg <gameDir>`
    → `VERIFY-OK`; patch.sdat is 910064 bytes; APP_VER=01.30; the heal marker file exists in tmpDir.
  - Boot the game past the ~14s crash point (the EBOOT's patch.sdat load).
  - Update the ps3-auto-update memory file with the outcome.

## Self-Review notes (spec ↔ plan)

- "Parse the PKG's own file table (AES-CTR, known PS3 GPKG key)" → Tasks 1–2 (key validated live).
- "Fallback: reject 0-byte files under USRDIR + REMOVE the restore-skip in that case" → Task 3
  (`hasZeroByteFile`), Task 6b (self-exit path now restores on any unverified success claim — the
  restore-skip is gone for both table-fail and fallback-fail).
- "Verify each expected file exists with expected size after --installpkg; only then record
  success/markInstalled" → Task 6 (all four verdict sites); markInstalled is downstream of
  installAll's per-pkg 0-return, so it's gated automatically.
- "Keep APP_VER/VERSION as a fast pre-filter" → reachedTarget still guards every site first.
- "probe_ps3update poisoned-install case (0-byte + version claims target → FAIL and restore)" →
  Task 3's final block.
- "Mutation-sanity per repo conventions" → Task 7.
- The poisoned MACHINE (state file already recorded success) heals via Task 4's seam + Task 6's
  marker-guarded wiring — without it the spec's own hardware case would never re-run the chain.
- total-bytes-floor variant of the fallback: NOT implemented — the 0-byte rule + full table check
  supersede it (the floor adds guesswork the table makes unnecessary).
