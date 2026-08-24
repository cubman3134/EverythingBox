# xdelta3 (VCDIFF) Soft-Patching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Teach `RomPatch` to apply xdelta3 patches, so the translation-patch scene's dominant format works through the existing soft-patch seam.

**Architecture:** Additive to `native/src/core/RomPatch.{h,cpp}` in the shape of the three formats already there — one `applyXdelta()` in the anonymous namespace, one more `Format` arm, one more magic in `detectFormat()`. `resolvePatchedRom()`, `writePatched()`, the content-addressed cache and the launch seam are already format-agnostic and need **no changes**.

**Tech Stack:** C++/Qt6 (`QByteArray`, `QString`), the repo's probe harness (`native/tools/probe_softpatch.cpp`).

**Issue:** [#199](https://github.com/cubman3134/EverythingBox/issues/199). **Branch:** `feat/199-xdelta`, worktree `C:\Users\cubma\goliath-wt-xdelta`.

## Global Constraints

- **Never mutate the source.** `apply()` takes `const QByteArray& source` and must leave it untouched. Same for every file on disk.
- **Refuse rather than corrupt.** A malformed patch, an out-of-range address, a truncated stream, a size that disagrees with the header — all return `false` with a readable `*error`, never a partial or plausible-looking output.
- **Format is decided by MAGIC, never by extension.** This is existing module discipline; keep it.
- **Deterministic:** same source + same patch always yields identical output.
- **No new dependency.** In-tree, like IPS/UPS/BPS.
- **No AI attribution in commits.** Conventional prefixes (`feat:`, `fix:`, `test:`).
- Build with the worktree recipe (below), run probes with the full PATH recipe, and run them **synchronously** — read the result before reporting.

### Build and probe recipe for this worktree

```bash
cd /c/Users/cubma/goliath-wt-xdelta
git submodule update --init external/RetroPark    # once; no network, objects are shared
cmake -S native -B build -G "Visual Studio 18 2026" -A x64 -DEVERYTHINGBOX_BUILD_APP=ON \
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 \
  -DMPV_INCLUDE_DIR=C:/mpv-dev/include -DMPV_LIBRARY=C:/mpv-dev/libmpv.lib
cmake --build build --config Release --target probe_softpatch --parallel
```

Run it with `PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH"` and `QT_QPA_PLATFORM=offscreen`. **Exit 127 is a missing DLL, not a test failure.** Keep `EVERYTHINGBOX_BUILD_APP=ON` even though only a probe is wanted — with `=OFF` the configure silently generates 2 probe targets instead of 51.

## The format, stated precisely

VCDIFF (RFC 3284) as xdelta3 emits it. **Measured against real patches in the wild** — every one sampled had `hdr_indicator = 0x04`, i.e. an application header and nothing else:

- **No `VCD_DECOMPRESS` (0x01)** — no secondary compressor. DJW and LZMA are out of scope; a patch that sets this bit is **refused with a message naming secondary compression**, not half-parsed.
- **No `VCD_CODETABLE` (0x02)** — the default code table only. A patch that sets this bit is **refused**.

### Two incompatible formats are called "xdelta"

| magic | what it is | what we do |
|---|---|---|
| `D6 C3 C4 00` | VCDIFF / xdelta3 | apply |
| `25 58 44 5A` (`%XDZ`) | **xdelta1**, a different container entirely | **refuse by name** |

xdelta1 is not readable by a VCDIFF decoder. Recognising it specifically matters because "this is xdelta1, which we do not support" and "this patch is corrupt" are different facts and the person holding the file needs the right one.

### VCDIFF integers are NOT the varint already in this file

`readVarint()` in `RomPatch.cpp` is byuu's format, used by BPS/UPS: **little-endian-ish, continuation bit clear means continue, with a `+= shift` bias.** VCDIFF's is different and unrelated:

> A VCDIFF integer is base-128 **big-endian**, most-significant group first, with the **high bit set on every byte except the last**.

They are not interchangeable and reusing the wrong one produces garbage that still parses. Write a separate `readVcdInt()` and say so in a comment.

## File Structure

| file | change |
|---|---|
| **Modify** `native/src/core/RomPatch.h` | `Format::Xdelta`, doc-comment updates, the `xdelta1`/secondary-compression refusals stated in the contract. |
| **Modify** `native/src/core/RomPatch.cpp` | `readVcdInt()`, the default code table, the address cache, `applyXdelta()`, arms in `detectFormat()`/`apply()`, extensions in `isPatchExtension()` and `sidecarPatchFor()`. |
| **Modify** `native/tools/probe_softpatch.cpp` | Hand-built VCDIFF fixtures + refusal cases. |

---

### Task 1: Detection, refusals, and extensions — no decoder yet

Small and safe: teach the module to *recognise* xdelta3 and to *refuse* xdelta1 and secondary compression, before anything can decode. Every one of these paths is reachable and testable on its own.

**Files:**
- Modify: `native/src/core/RomPatch.h`
- Modify: `native/src/core/RomPatch.cpp`
- Test: `native/tools/probe_softpatch.cpp`

**Interfaces:**
- Produces: `RomPatch::Format::Xdelta`; `detectFormat()` returning it for `D6 C3 C4 00`; `apply()` refusing xdelta1 and secondary compression with distinct messages; `isPatchExtension()` accepting `xdelta`/`xdelta3`/`vcdiff`.

- [ ] **Step 1: Write the failing checks**

In `probe_softpatch.cpp`, following the file's existing check style (read it first and match it — do not invent a new assertion helper):

```cpp
// ---- xdelta3 detection and refusals -------------------------------------------------------------------
// A minimal VCDIFF header: magic D6 C3 C4, version 00, then hdr_indicator.
static QByteArray vcdHeader(quint8 indicator)
{
    QByteArray h;
    h.append(char(0xD6)); h.append(char(0xC3)); h.append(char(0xC4)); h.append(char(0x00));
    h.append(char(indicator));
    return h;
}

// detectFormat says Xdelta on the real magic.
CHECK(RomPatch::detectFormat(vcdHeader(0x00)) == RomPatch::Format::Xdelta, "vcdiff magic detected");

// xdelta1 is a DIFFERENT format. It must be refused by name, not mis-parsed and not reported as corrupt.
{
    QByteArray x1("%XDZ004%");
    x1.append(QByteArray(32, '\0'));
    QString err;
    QByteArray out;
    CHECK(!RomPatch::apply(QByteArray(64, 'A'), x1, out, &err), "xdelta1 refused");
    CHECK(err.contains("xdelta1", Qt::CaseInsensitive), "xdelta1 refusal names the format, got: " + err);
    CHECK(out.isEmpty(), "xdelta1 refusal writes nothing");
}

// A patch asking for a secondary compressor is refused with a message that says so — we implement
// neither DJW nor LZMA, and half-parsing one would produce garbage that looks like a patch.
{
    QByteArray p = vcdHeader(0x01);          // VCD_DECOMPRESS
    p.append(char(0x01));                    // a compressor id
    QString err;
    QByteArray out;
    CHECK(!RomPatch::apply(QByteArray(64, 'A'), p, out, &err), "secondary compression refused");
    CHECK(err.contains("secondary", Qt::CaseInsensitive), "refusal names secondary compression, got: " + err);
}

// A custom code table is likewise out of scope and refused rather than guessed at.
{
    QByteArray p = vcdHeader(0x02);          // VCD_CODETABLE
    QString err;
    QByteArray out;
    CHECK(!RomPatch::apply(QByteArray(64, 'A'), p, out, &err), "custom code table refused");
    CHECK(err.contains("code table", Qt::CaseInsensitive), "refusal names the code table, got: " + err);
}

// Sidecar extensions.
CHECK(RomPatch::isPatchExtension(QStringLiteral("xdelta")), "xdelta is a patch extension");
CHECK(RomPatch::isPatchExtension(QStringLiteral("vcdiff")), "vcdiff is a patch extension");
CHECK(!RomPatch::isPatchExtension(QStringLiteral("zip")), "zip is not a patch extension");
```

- [ ] **Step 2: Build and run to verify they fail**

```bash
cmake --build build --config Release --target probe_softpatch --parallel
```
Expected: **compile error** — `Format::Xdelta` does not exist. That is the failure for this step; fix only by adding the enum arm, then re-run and expect the *checks* to fail (detectFormat returns None).

- [ ] **Step 3: Implement detection and refusals**

In `RomPatch.h`, extend the enum and the contract comment:

```cpp
    enum class Format { None, Ips, Bps, Ups, Xdelta };
```

In `RomPatch.cpp`:

```cpp
// ---- xdelta3 / VCDIFF (RFC 3284) ----------------------------------------------------------------------
// Header indicator bits we refuse rather than attempt.
constexpr quint8 kVcdDecompress = 0x01;   // a secondary compressor (DJW / LZMA) — not implemented
constexpr quint8 kVcdCodetable  = 0x02;   // a custom instruction code table  — not implemented
constexpr quint8 kVcdAppHeader  = 0x04;   // an application header — present on every real patch; skipped
```

Extend `detectFormat()`:

```cpp
Format detectFormat(const QByteArray& patch)
{
    if (magicIs(patch, "PATCH", 5)) return Format::Ips;
    if (magicIs(patch, "UPS1", 4))  return Format::Ups;
    if (magicIs(patch, "BPS1", 4))  return Format::Bps;
    // VCDIFF: D6 C3 C4 then a version byte. Only version 0 exists.
    if (patch.size() >= 4 && quint8(patch[0]) == 0xD6 && quint8(patch[1]) == 0xC3
        && quint8(patch[2]) == 0xC4 && quint8(patch[3]) == 0x00)
        return Format::Xdelta;
    return Format::None;
}
```

Extend `apply()` — note the xdelta1 arm lives HERE rather than in `detectFormat()`, because it is a refusal with a *reason*, not a format we can represent:

```cpp
bool apply(const QByteArray& source, const QByteArray& patch, QByteArray& out, QString* error)
{
    switch (detectFormat(patch))
    {
    case Format::Ips:    return applyIps(source, patch, out, error);
    case Format::Ups:    return applyUps(source, patch, out, error);
    case Format::Bps:    return applyBps(source, patch, out, error);
    case Format::Xdelta: return applyXdelta(source, patch, out, error);
    case Format::None:
        // xdelta1 (%XDZ) is a different container from VCDIFF and cannot be read by the decoder above.
        // Naming it is the point: "we do not support xdelta1" and "this file is corrupt" are different
        // facts, and only one of them tells the holder what to do next.
        if (magicIs(patch, "%XDZ", 4))
        {
            if (error) *error = QObject::tr("this is an xdelta1 patch, which is not supported "
                                            "(only xdelta3 / VCDIFF patches can be applied)");
            return false;
        }
        if (error) *error = QStringLiteral("not a recognised ROM patch (no IPS/UPS/BPS/VCDIFF magic)");
        return false;
    }
    return false;
}
```

Add a stub `applyXdelta()` in the anonymous namespace that parses only the header and enforces the two refusals — Task 2 fills in the body:

```cpp
bool applyXdelta(const QByteArray& source, const QByteArray& patch, QByteArray& out, QString* error)
{
    Q_UNUSED(source);
    out.clear();
    if (patch.size() < 5) { if (error) *error = QStringLiteral("truncated VCDIFF header"); return false; }
    const quint8 indicator = quint8(patch[4]);
    if (indicator & kVcdDecompress)
    {
        if (error) *error = QObject::tr("this patch uses VCDIFF secondary compression, which is not supported");
        return false;
    }
    if (indicator & kVcdCodetable)
    {
        if (error) *error = QObject::tr("this patch uses a custom VCDIFF code table, which is not supported");
        return false;
    }
    if (error) *error = QStringLiteral("VCDIFF decoding not implemented yet");
    return false;
}
```

Extend `isPatchExtension()` and `sidecarPatchFor()`'s extension list with `xdelta`, `xdelta3`, `vcdiff` — **both**; they are two separate lists and updating only one is the classic miss here.

- [ ] **Step 4: Build and run to verify the checks pass**

```bash
cmake --build build --config Release --target probe_softpatch --parallel
PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" QT_QPA_PLATFORM=offscreen ./build/Release/probe_softpatch.exe
```
Expected: all checks pass, including the pre-existing IPS/BPS/UPS ones.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/RomPatch.h native/src/core/RomPatch.cpp native/tools/probe_softpatch.cpp
git commit -m "feat: recognise xdelta3 patches, and refuse xdelta1 by name"
```

---

### Task 2: The VCDIFF decoder

The substance. Read RFC 3284 sections 4–6 before starting; the three sub-parts below are where a from-spec implementation goes wrong, so they are specified exactly.

**Files:**
- Modify: `native/src/core/RomPatch.cpp`
- Test: `native/tools/probe_softpatch.cpp`

**Interfaces:**
- Produces: a working `applyXdelta()`.

#### 2a. The integer format

```cpp
// A VCDIFF integer (RFC 3284 §2): base-128, BIG-endian, most-significant group first, high bit set on
// every byte EXCEPT the last.
//
// This is NOT readVarint() above. That one is byuu's format for BPS/UPS: little-endian-first, continuation
// signalled by the bit being CLEAR, and with a running += shift bias. The two are unrelated, and feeding a
// VCDIFF stream to the byuu reader yields plausible-looking garbage rather than an error. Keep them separate.
bool readVcdInt(const QByteArray& b, int& pos, quint64& out)
{
    quint64 value = 0;
    for (int i = 0; i < 10; ++i)          // 10 * 7 bits > 64; refuse anything longer
    {
        if (pos >= b.size()) return false;
        const quint8 x = quint8(b.at(pos++));
        if (value > (std::numeric_limits<quint64>::max() >> 7)) return false;   // overflow
        value = (value << 7) | (x & 0x7F);
        if ((x & 0x80) == 0) { out = value; return true; }
    }
    return false;
}
```

#### 2b. The default code table

RFC 3284 §5.4 defines 256 instruction pairs. **Build it algorithmically** — a transcribed 256-row literal is exactly the kind of thing that acquires a typo nobody finds:

```cpp
enum : quint8 { kInstNoop = 0, kInstAdd = 1, kInstRun = 2, kInstCopy = 3 };

struct VcdInst { quint8 type; quint8 size; quint8 mode; };
struct VcdCode { VcdInst first; VcdInst second; };

// RFC 3284 §5.4's default code table, generated rather than transcribed.
const VcdCode* defaultCodeTable()
{
    static VcdCode t[256];
    static bool built = false;
    if (built) return t;

    int i = 0;
    auto put = [&](quint8 t1, quint8 s1, quint8 m1, quint8 t2 = kInstNoop, quint8 s2 = 0, quint8 m2 = 0) {
        t[i].first  = { t1, s1, m1 };
        t[i].second = { t2, s2, m2 };
        ++i;
    };

    put(kInstRun, 0, 0);                                         // 0
    for (quint8 s = 0; s <= 17; ++s) put(kInstAdd, s, 0);        // 1..18  (s==0 => size follows)
    for (quint8 mode = 0; mode <= 8; ++mode)                     // 19..162
    {
        put(kInstCopy, 0, mode);
        for (quint8 s = 4; s <= 18; ++s) put(kInstCopy, s, mode);
    }
    for (quint8 mode = 0; mode <= 5; ++mode)                     // 163..234
        for (quint8 addSize = 1; addSize <= 4; ++addSize)
            for (quint8 copySize = 4; copySize <= 6; ++copySize)
                put(kInstAdd, addSize, 0, kInstCopy, copySize, mode);
    for (quint8 mode = 6; mode <= 8; ++mode)                     // 235..246
        for (quint8 addSize = 1; addSize <= 4; ++addSize)
            put(kInstAdd, addSize, 0, kInstCopy, 4, mode);
    for (quint8 mode = 0; mode <= 8; ++mode)                     // 247..255
        put(kInstCopy, 4, mode, kInstAdd, 1, 0);

    Q_ASSERT(i == 256);
    built = true;
    return t;
}
```

**Verify `i == 256` at runtime in the probe**, not only with `Q_ASSERT` — a Release build compiles the assert away and an off-by-one here would silently mis-decode every patch.

#### 2c. The address cache

RFC 3284 §5.3. COPY addresses are coded relative to a cache, and getting this wrong produces addresses that are *in range but wrong* — output that looks like a plausible ROM. This is the single most important part to get right.

```cpp
// RFC 3284 §5.3. near[] is a round-robin of recent addresses; same[] is a 256-way direct map.
// Sizes are fixed by the default code table's mode count: 4 near, 3 same.
struct VcdAddrCache
{
    static constexpr int kNear = 4;
    static constexpr int kSame = 3;
    quint64 near_[kNear] {};
    int nextNear = 0;
    quint64 same_[kSame * 256] {};

    void reset() { for (auto& n : near_) n = 0; nextNear = 0; for (auto& s : same_) s = 0; }

    void update(quint64 addr)
    {
        near_[nextNear] = addr;
        nextNear = (nextNear + 1) % kNear;
        same_[addr % (kSame * 256)] = addr;
    }

    // Decode one address for mode `mode`, reading from the addresses section.
    bool decode(const QByteArray& addrSec, int& pos, quint64 here, quint8 mode, quint64& out)
    {
        quint64 v = 0;
        if (mode == 0) { if (!readVcdInt(addrSec, pos, v)) return false; out = v; }
        else if (mode == 1) { if (!readVcdInt(addrSec, pos, v)) return false; if (v > here) return false; out = here - v; }
        else if (mode < 2 + kNear)
        {
            if (!readVcdInt(addrSec, pos, v)) return false;
            out = near_[mode - 2] + v;
        }
        else
        {
            if (pos >= addrSec.size()) return false;
            const quint8 b = quint8(addrSec.at(pos++));
            const int m = mode - (2 + kNear);
            out = same_[m * 256 + b];
        }
        update(out);
        return true;
    }
};
```

#### 2d. The window loop

Per window: `win_indicator`; if it has `VCD_SOURCE (0x01)` or `VCD_TARGET (0x02)`, a source segment length and position; then `delta_encoding_length`, `target_window_length`, `delta_indicator` (**must be 0** — non-zero means per-section secondary compression, refuse), then the three section lengths (data, instructions, addresses) and the three sections back to back.

Then run instructions until the target window is full:

- **ADD**: copy `size` bytes from the data section.
- **RUN**: one byte from the data section, repeated `size` times.
- **COPY**: `size` bytes from `addr`, where addr < sourceSegmentLen means "from the source segment", otherwise from the target window already produced. **COPY may overlap its own output** — copy byte-by-byte, never `memcpy`, because a COPY reaching into bytes it is currently writing is legal and is how RLE-ish runs are encoded.

A `size` of 0 in the code table means the size follows as a VCDIFF integer in the instruction stream.

**Every** read must be bounds-checked, and the produced window must be exactly `target_window_length` — short or long is a refusal.

- [ ] **Step 1: Write the failing checks**

Hand-built fixtures. Be explicit that these are hand-built, and why that is a limitation:

```cpp
// ---- xdelta3 decoding ------------------------------------------------------------------------------
// These fixtures are HAND-BUILT VCDIFF byte streams, not the output of a real encoder (none is available
// on this machine). They pin the decoder against the SPEC. They cannot, on their own, prove the decoder
// agrees with what xdelta3 actually emits — that check is the real-patch validation in Task 3, and it is
// the one that would catch a wrong integer format or a mis-generated code table.
static void appendVcdInt(QByteArray& b, quint64 v)
{
    QByteArray tmp;
    tmp.append(char(v & 0x7F));
    v >>= 7;
    while (v) { tmp.append(char((v & 0x7F) | 0x80)); v >>= 7; }
    std::reverse(tmp.begin(), tmp.end());
    b.append(tmp);
}

// One window that ADDs 4 literal bytes and COPYs 4 from the source at offset 0.
{
    const QByteArray source("ABCDEFGH");
    QByteArray data("WXYZ");                       // the ADD payload
    QByteArray insts;
    insts.append(char(5));                          // code 5 = ADD size 4  (1 + size)
    insts.append(char(19 + 0));                     // code 19 = COPY size 0 mode 0 => size follows
    appendVcdInt(insts, 4);
    QByteArray addrs;
    appendVcdInt(addrs, 0);                         // mode 0: absolute address 0

    QByteArray p = vcdHeader(0x00);
    p.append(char(0x01));                           // win_indicator = VCD_SOURCE
    appendVcdInt(p, quint64(source.size()));        // source segment length
    appendVcdInt(p, 0);                             // source segment position
    QByteArray delta;
    appendVcdInt(delta, 8);                         // target window length
    delta.append(char(0x00));                       // delta_indicator = 0
    appendVcdInt(delta, quint64(data.size()));
    appendVcdInt(delta, quint64(insts.size()));
    appendVcdInt(delta, quint64(addrs.size()));
    delta.append(data); delta.append(insts); delta.append(addrs);
    appendVcdInt(p, quint64(delta.size()));         // delta_encoding_length
    p.append(delta);

    QByteArray out; QString err;
    CHECK(RomPatch::apply(source, p, out, &err), "vcdiff add+copy applies: " + err);
    CHECK(out == QByteArray("WXYZABCD"), "vcdiff add+copy output, got: " + QString::fromLatin1(out));
}
```

Add, in the same shape and each with its own `CHECK`:

- a **RUN** instruction producing a repeated byte;
- a **self-overlapping COPY** (address inside the target window already produced) — proves byte-by-byte copying;
- a **truncated** patch (cut the last 3 bytes) → refused, `out` empty;
- an **out-of-range COPY** address → refused;
- a **target length mismatch** (declare 9, produce 8) → refused;
- `defaultCodeTable()` populating exactly 256 entries — a real runtime check, since `Q_ASSERT` vanishes in Release.

- [ ] **Step 2: Build and run to verify they fail**

Expected: the new checks fail with "VCDIFF decoding not implemented yet" from Task 1's stub. Confirm that exact message — if anything *passes*, stop and report it.

- [ ] **Step 3: Implement the decoder**

Fill in `applyXdelta()` per 2a–2d. Keep the two Task-1 refusals at the top. Skip the application header when `kVcdAppHeader` is set: read its length as a VCDIFF integer, then skip that many bytes.

- [ ] **Step 4: Build and run to verify they pass**

All checks green, including the pre-existing IPS/BPS/UPS ones.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/RomPatch.cpp native/tools/probe_softpatch.cpp
git commit -m "feat: decode VCDIFF windows, instructions and copy addresses"
```

---

### Task 3: Real-patch validation, sidecar wiring, and the gate

**Files:**
- Modify: `native/src/core/RomPatch.cpp` (only if validation finds a bug)
- Modify: `native/tools/probe_softpatch.cpp`
- Check: `.github/workflows/ci.yml` and the probe gate script — confirm `probe_softpatch` is already registered (it should be, from #128); if so **change nothing**.

- [ ] **Step 1: Validate against REAL xdelta3 patches**

This is the step the hand-built fixtures cannot replace. Three real patches are already downloaded and their headers decoded (`hdr_indicator=0x04`, no secondary compression):

| patch | size |
|---|---|
| `Chou Soujuu Mecha MG [EN v1.1].xdelta` | 3,352,545 b |
| `2ch-ct-unofficial01a.xdelta` | 2,616,723 b |
| `DGrayManPatch.xdelta` | 1,354,899 b |

Re-fetch them if the temp copies are gone (they came from public homebrew-archive translation pages; the download needs a `Referer` of the item's own page).

We do **not** have the base ROMs, so output content cannot be checked. What can be checked is far from nothing, and it is exactly where a from-spec decoder fails:

1. **The whole stream parses** — every window consumed, ending exactly at end-of-patch with no trailing bytes.
2. **Every instruction is in range** against a synthetic source of the declared source-segment length.
3. **Each window produces exactly its declared `target_window_length`.**

A wrong integer format, a mis-generated code table, or a broken address cache will fail at least one of these within the first window. Write this as a **throwaway harness** (a scratch `main()` or a temporary probe), **not** a committed test — the patches are multi-megabyte and are not ours to vendor.

**Report the actual result.** If it fails, fix the decoder and say what was wrong. If you cannot obtain the patches, say so plainly — do not silently skip this and report Task 3 as done.

- [ ] **Step 2: Confirm the sidecar path**

Verify by inspection that `resolvePatchedRom()` finds `Game.xdelta` beside `Game.nds` — this works only if **both** `isPatchExtension()` and `sidecarPatchFor()`'s literal list gained the new extensions in Task 1. Add a probe check that writes a temp ROM + sidecar `.xdelta` and asserts the resolver returns a patched path.

- [ ] **Step 3: Update the header contract**

`RomPatch.h`'s comment still says *"xdelta is deliberately out of scope (bigger format, disc-image streaming — see issue #128)."* Replace it with what is now true, including the honest limitation:

> xdelta3 / VCDIFF is supported (RFC 3284, default code table, no secondary compression — patches using either are refused by name). **Unlike BPS and UPS, VCDIFF embeds no source checksum**, so a patch built for a different dump cannot be refused on its own evidence: it applies if the source is merely long enough. Where a source states the target dump, check it with `RomPatch::crc32()` before applying. xdelta1 (`%XDZ`) is a different container and is refused.

- [ ] **Step 4: Run the full probe gate**

Not just `probe_softpatch` — the whole CI probe list, since `RomPatch` is linked into the app target.

- [ ] **Step 5: Commit**

```bash
git add -A native/
git commit -m "test: validate the VCDIFF decoder against real xdelta3 patches"
```

---

## Self-Review

**Issue coverage.** #199's scope list maps task-for-task: `Format::Xdelta` and `detectFormat` (1), `applyXdelta` (2), `isPatchExtension` + sidecar (1 and 3), probe fixtures including the xdelta1 refusal and a corrupt patch (1–3), and the header-comment correction (3). `resolvePatchedRom`/`writePatched`/cache are untouched by design, as the issue states.

**The no-checksum asymmetry** is carried into the header contract in Task 3 rather than left implicit — it is the one behavioural difference from BPS/UPS and the one most likely to surprise.

**Deliberately not here:** secondary compression (DJW/LZMA), custom code tables, xdelta1 — all refused by name, per the issue. Nested-archive recursion and extension-agnostic patch discovery belong to an install flow, not to `RomPatch`.

**Known weakness, stated:** the shipped fixtures are hand-built and prove conformance to the spec as I read it. Task 3 step 1 is what tests conformance to what xdelta3 actually emits, and it is not a committed test. If that step is skipped, the decoder is spec-verified and reality-unverified — which is precisely the gap that has bitten this project before.
