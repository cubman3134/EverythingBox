// Cheat Search — the pure, mutation-tested heart of "scan core memory to CREATE a cheat" (#96).
//
// The existing cheat editor edits codes you already have. This is the RetroArch-style search that MAKES
// them: snapshot system RAM, then narrow a candidate address set by exact value or by how a value CHANGED
// across gameplay (increased / decreased / unchanged / changed), until one address is left — the byte that
// holds health / lives / money — which the caller then freezes as a cheat.
//
// This unit is deliberately dependency-free: QtCore/std only, NO RetroView / LibretroCore. It operates on
// plain byte buffers (a `const uint8_t*` + length is exactly what `core_.memoryData(RETRO_MEMORY_SYSTEM_RAM)`
// hands back), so the probe drives it with hand-authored fixtures and never a running core. Buffers in,
// sorted addresses out.
//
// ENDIANNESS: reads default to LITTLE-ENDIAN, because the classic cheat targets are little-endian consoles
// (NES 6502, SNES 65816, Game Boy LR35902, Genesis is big-endian but its work RAM cheats are the exception,
// not the rule). Endianness is a parameter, never baked in, so a big-endian core can pass littleEndian=false.
//
// ALIGNMENT: the initial scan walks EVERY byte offset, not only width-aligned ones. This is the safe
// superset RetroArch itself uses — a 16-bit value can, and on many systems does, sit at an odd address, and
// an aligned-only scan would miss it. The cost is a larger initial candidate set that the relational steps
// collapse quickly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace cheatsearch {

// Value width in bytes. The enum's numeric value IS the byte count, so widthBytes() is a static_cast.
enum class Width : std::size_t { W8 = 1, W16 = 2, W32 = 4 };

inline std::size_t widthBytes(Width w) { return static_cast<std::size_t>(w); }

// A narrowing predicate. ExactValue compares value(addr) against a caller-supplied target; the four
// relational filters compare value(addr) in the current snapshot against the same address in the previous
// snapshot. "Unchanged"/"Changed" let you find a value you can't read directly (e.g. hunt for the thing
// that DOESN'T change when everything else does).
enum class Filter {
    ExactValue,   // value(cur, addr) == target
    Increased,    // value(cur, addr) >  value(prev, addr)
    Decreased,    // value(cur, addr) <  value(prev, addr)
    Unchanged,    // value(cur, addr) == value(prev, addr)
    Changed       // value(cur, addr) != value(prev, addr)
};

// Read a width-w value at `addr` from ram[0..ramLen). Returns nullopt when the read would run off the end
// (addr + width > ramLen) or ram is null — the bounds guard the whole feature relies on. `isSigned`
// sign-extends the width-w value into the returned int64; otherwise it is zero-extended. Overflow-safe:
// `ramLen - addr` is only evaluated once addr <= ramLen is known.
inline std::optional<std::int64_t> readValue(const std::uint8_t* ram, std::size_t ramLen,
                                             std::size_t addr, Width w, bool isSigned,
                                             bool littleEndian = true)
{
    if (ram == nullptr) return std::nullopt;
    const std::size_t n = widthBytes(w);
    if (addr > ramLen || n > ramLen - addr) return std::nullopt;  // addr + n > ramLen, without overflow

    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::uint8_t b = littleEndian ? ram[addr + i] : ram[addr + n - 1 - i];
        raw |= static_cast<std::uint64_t>(b) << (8 * i);
    }

    if (isSigned)
    {
        const unsigned bits = static_cast<unsigned>(n * 8);
        if (bits < 64)
        {
            const std::uint64_t signBit = std::uint64_t(1) << (bits - 1);
            if (raw & signBit) raw |= ~((std::uint64_t(1) << bits) - 1);  // sign-extend the high bits
        }
        return static_cast<std::int64_t>(raw);
    }
    return static_cast<std::int64_t>(raw);
}

// Does `addr` satisfy the relational filter, comparing current vs previous snapshot? Both reads must be in
// bounds; an out-of-bounds address never survives. ExactValue is NOT a relational filter — it is handled by
// matchesExact — so passing it here always returns false.
inline bool matchesRelational(const std::uint8_t* prev, const std::uint8_t* cur, std::size_t ramLen,
                              std::size_t addr, Width w, bool isSigned, Filter f, bool littleEndian = true)
{
    const std::optional<std::int64_t> a = readValue(prev, ramLen, addr, w, isSigned, littleEndian);
    const std::optional<std::int64_t> b = readValue(cur, ramLen, addr, w, isSigned, littleEndian);
    if (!a || !b) return false;
    switch (f)
    {
        case Filter::Increased: return *b > *a;
        case Filter::Decreased: return *b < *a;
        case Filter::Unchanged: return *b == *a;
        case Filter::Changed:   return *b != *a;
        case Filter::ExactValue: return false;
    }
    return false;
}

// Does `addr` in `snapshot` hold exactly `target` at width w? Out-of-bounds never matches.
inline bool matchesExact(const std::uint8_t* snapshot, std::size_t ramLen, std::size_t addr,
                         Width w, bool isSigned, std::int64_t target, bool littleEndian = true)
{
    const std::optional<std::int64_t> v = readValue(snapshot, ramLen, addr, w, isSigned, littleEndian);
    return v.has_value() && *v == target;
}

// The FIRST scan, over the full RAM. For ExactValue this returns every byte offset whose value == target.
// For a relational filter there is no previous snapshot to compare against, so this seeds the full universe
// of in-bounds addresses (every offset where a width-w read fits) — the "I don't know the value, start from
// everything, then narrow by change" workflow. Output is ascending (naturally, we scan low->high) and each
// address appears once.
inline std::vector<std::size_t> initialCandidates(const std::uint8_t* snapshot, std::size_t ramLen,
                                                  Filter filter, Width w, bool isSigned,
                                                  std::int64_t target, bool littleEndian = true)
{
    std::vector<std::size_t> out;
    if (snapshot == nullptr) return out;
    const std::size_t n = widthBytes(w);
    if (n > ramLen) return out;
    const std::size_t last = ramLen - n;  // highest addr where a width-w read still fits
    for (std::size_t addr = 0; addr <= last; ++addr)
    {
        const bool keep = (filter == Filter::ExactValue)
            ? matchesExact(snapshot, ramLen, addr, w, isSigned, target, littleEndian)
            : true;  // relational: seed everything, the next narrow() step compares against a prev snapshot
        if (keep) out.push_back(addr);
    }
    return out;
}

// A subsequent step. INTERSECTS: it only ever keeps addresses already in `candidates`, so a dropped address
// can never re-enter the set. For ExactValue it re-tests value(cur, addr) == target; for a relational filter
// it compares cur vs prev. `candidates` is assumed sorted ascending (as produced by initialCandidates / a
// prior narrow); the result preserves that order.
inline std::vector<std::size_t> narrow(const std::vector<std::size_t>& candidates,
                                       const std::uint8_t* prev, const std::uint8_t* cur, std::size_t ramLen,
                                       Filter filter, Width w, bool isSigned,
                                       std::int64_t target, bool littleEndian = true)
{
    std::vector<std::size_t> out;
    out.reserve(candidates.size());
    for (std::size_t addr : candidates)
    {
        const bool keep = (filter == Filter::ExactValue)
            ? matchesExact(cur, ramLen, addr, w, isSigned, target, littleEndian)
            : matchesRelational(prev, cur, ramLen, addr, w, isSigned, filter, littleEndian);
        if (keep) out.push_back(addr);
    }
    return out;
}

} // namespace cheatsearch
