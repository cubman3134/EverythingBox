// THE RECOMPS SECTION'S ROW MODEL (issue #248, increment a) — the browse surface over the native-port
// catalogue #233 already ships.
//
// WHAT THE SECTION IS. #233 put a *Native port* verb on the one game row a shipped recompilation runs. That
// is the right place for somebody who is already looking at Majora's Mask, and the wrong place for somebody
// who wants to know what recompilations exist at all: the only way to find the feature was to already own the
// one game it applies to. `Games → Recomps` is the browse half — the catalogue, listed, with what each entry
// is and where this machine stands with it.
//
// PURE, AND QtCore ONLY. Everything here is (catalogue entries + facts about this machine) -> rows. It reads
// no file, no store, no setting and no clock; the caller gathers the facts and the caller projects the rows
// onto MediaItems. That is what lets probe_ports drive the whole model headlessly — including the three
// states that need a machine to be in a particular condition, which no live drive could stage on demand.
//
// THE STATE IS DERIVED, NEVER STORED. There is no "port state" record anywhere and there must not be: two
// records of the same fact drift, and the one the row reads is then the stale one. Every state is recomputed
// from four inputs that each have exactly one owner:
//   * `installed`    — EmulatorManager::isInstalled, i.e. does the port's binary resolve. The #233 install
//                      path already owns this and nothing else may answer it;
//   * `libraryMatch` — does anything in the user's library match the entry's `rom_identity`, asked through
//                      NativePorts::matchesRow, which is the SAME gate the game-row verb is offered on. It is
//                      a title/region match today; the `HashVerify` digest gate is increment (b) and lands
//                      behind this same boolean, so nothing above here changes when it does;
//   * `installedTag` — the release EB recorded when it installed (NativePorts::readInstalledTag);
//   * `catalogueTag` — the release the catalogue pins (`release.tag`).
// An UNKNOWN input is never read as a negative. No recorded tag, or no pinned tag, means "nobody knows which
// release this is", and that is `installed` — telling a person their software is out of date on the strength
// of a fact nobody has is worse than saying nothing.
//
// TIER. `pre-built` (a published release binary, #233's tier — every entry this build ships) or
// `self-compiled` (RetComM's tier: the port is compiled on this machine from a named recompiler plus the
// user's own dump). Read off `NativePortBinding::buildEngine`: an entry that names a build engine is the
// second tier. Increment (a) can act on the first only, and a self-compiled row says so rather than offering
// an Install that cannot happen.
//
// TWO STATES ARE RESERVED AND UNUSED. `Building` and `Ready` belong to increment (c), where an install
// becomes a compile that takes minutes and can fail. They are declared here, now, so that increment adds a
// case rather than reshaping an enum every call site switches over.
//
// ---------------------------------------------------------------------------------------------------------
// THE ROM-IDENTITY GATE (issue #248, increment b). Increment (a)'s `libraryMatch` was a TITLE match, and the
// header above promised the digest gate would land behind the same boolean. It has. What changed underneath:
//
//   * a title match ALONE no longer counts where the entry publishes a digest. A recomp is compiled against
//     one exact dump; "Twisted Metal 4" on disk may be the PAL disc, a bad rip or a hack, and offering an
//     install that ends in the port refusing the file is worse than saying which dump is wanted. So an entry
//     that publishes crc32 / md5 / sha1 / sha256 is matched on ONE OF THOSE, against the library's own hash
//     cache, and on nothing else;
//   * an entry that publishes NO digest at all (the entries whose authors never wrote one down) keeps the
//     title match — and the row SAYS SO, because "we matched the name and nobody published a hash to check it
//     against" is a materially weaker claim than "these are the bytes". That is `dumpUnverified`, and it is a
//     fact about the CATALOGUE, not about the dump;
//   * NOTHING HERE HASHES. Not one function in this header opens a file, and that is the mechanism, not a
//     convention: the gate is handed digests the caller already had, and reports `Checking` when a plausible
//     candidate has none cached. The caller then hashes THAT candidate — off the GUI thread, once, into the
//     shared cache — and asks again. A gate that could hash would eventually hash a 660 MB disc image inside
//     a list-population call, which is the whole defect this shape exists to make unreachable.
//
// WHICH FILES ARE PLAUSIBLE, i.e. which ones are worth the one-off hash. Hashing every ROM on the machine to
// answer "do you own Klonoa" is absurd, so a candidate is narrowed FIRST by three facts already in hand
// (platform, extension, byte size) — RetComM's own scan rule, which is what `rom_identity.sizes` is for. The
// two deliberate loosenesses in that narrowing are each load-bearing:
//   * an ARCHIVE is never size-gated. A .7z of a 32 MB ROM is 12 MB, and the cache's digests for an archive
//     are the digests of the EXTRACTED stream (HashVerify keys the record on the archive the user sees and
//     hashes what is inside it), so the archive's own size says nothing about the dump's;
//   * an archive also passes the extension gate, for the same reason: its extension is `.7z`, and the dump's
//     is inside it.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <functional>

#include "NativePorts.h"

namespace recomps
{
    // Where this machine stands with one catalogue entry. Ordered as the derivation reads them, not as a UI
    // would sort them.
    enum class State
    {
        NotInstalled,     // the user has the game; the port has not been installed yet
        NeedsRom,         // nothing in the library matches the entry's rom_identity — installing is premature
        CheckingDumps,    // ...or nothing YET: a plausible dump is on this machine and has not been hashed
        Installed,        // the port's binary resolves, and nothing says a newer release exists
        UpdateAvailable,  // ...and the recorded release differs from the one the catalogue pins
        // ---- reserved for increment (c) (self-compiled tier). Never produced by deriveState().
        Building,         // a local compile is running for this entry
        Ready,            // a local compile finished and its binary has not been launched yet
    };

    enum class Tier
    {
        PreBuilt,      // a published release binary — #233's tier, and every entry this build ships
        SelfCompiled,  // compiled here from the recompiler the entry names — increment (c)
    };

    // The inputs, gathered by the caller from their one owner each (see the header note).
    struct Facts
    {
        bool    installed    = false;
        bool    libraryMatch = false;
        // ...and HOW the library matched, which is a property of the catalogue ENTRY rather than of the dump:
        // true means the entry published no digest to check, so the name is all that was compared.
        bool    dumpUnverified = false;
        // No match YET: a plausible candidate is on this machine but its digests are not in the cache. Only
        // ever read when libraryMatch is false — a match already found is not made provisional by a second
        // candidate nobody has hashed.
        bool    checkingDumps = false;
        QString installedTag;   // "" = EB never recorded which release this install is
        QString catalogueTag;   // "" = the catalogue pins no release
    };

    // THE DERIVATION. Read top to bottom; each clause is the reason the next one is reachable.
    inline State deriveState(const Facts& f)
    {
        if (f.installed)
        {
            // Both tags must be known before a difference means anything — see the header note on unknowns.
            const QString a = f.installedTag.trimmed();
            const QString b = f.catalogueTag.trimmed();
            if (!a.isEmpty() && !b.isEmpty() && a.compare(b, Qt::CaseInsensitive) != 0)
                return State::UpdateAvailable;
            return State::Installed;
        }
        // NOT installed. Whether that is worth doing anything about depends on whether the user has the game:
        // a port is not a game, it is a way of running one you already own, and "install" on a machine with no
        // matching dump ends in the port's own "give me the ROM" screen with nothing to give it.
        if (f.libraryMatch) return State::NotInstalled;
        // "We have not looked yet" is not "you do not own it". A plausible dump whose digests are still being
        // computed gets its own state rather than a `needs ROM` that flips a few seconds later — a row that
        // tells somebody to go and find a game they already have, and then silently corrects itself, is worse
        // than a row that says it is still working.
        return f.checkingDumps ? State::CheckingDumps : State::NeedsRom;
    }

    inline Tier tierOf(const ExternalEmulator& port)
    {
        return port.port.buildEngine.trimmed().isEmpty() ? Tier::PreBuilt : Tier::SelfCompiled;
    }

    // ---- rows ------------------------------------------------------------------------------------------
    // The section is a flat list with section headers, the shape browse::liveTvChannelsCatalog established:
    // a header row per system, then that system's ports. A flat list is what both layouts render, and the
    // header carries the grouping without needing a second level to drill through for one row.
    struct Row
    {
        enum class Kind
        {
            SystemHeader,  // a section label ("Nintendo 64"). Not activatable.
            Port,          // a catalogue entry
            Error,         // the catalogue could not be read. #174: never an empty section.
        };

        Kind    kind = Kind::Port;
        QString portId;        // Port rows: the NativePorts catalogue id (the one thing activation needs)
        QString systemId;      // Port + SystemHeader rows
        QString title;         // the GAME's name (Port), the system's name (SystemHeader), the message (Error)
        QString creditedName;  // the port project, by its OWN name — never the recompiler's brand (see #233)
        QString license;       // "" when the catalogue does not say
        // The recompiler this entry is built with ("psxrecomp" / "snesrecomp" / "gbarecomp"), empty for a
        // pre-built entry. On the row because a self-compiled port is the ENGINE's program as much as the
        // port author's, and because the licence shown beside it is then the engine's, not the port's.
        QString engine;
        Tier    tier = Tier::PreBuilt;
        State   state = State::NotInstalled;
        // The entry published no digest, so `not installed` rests on the dump's NAME. Surfaced because the
        // difference between "these are the bytes" and "this is what it is called" is the user's to judge.
        bool    dumpUnverified = false;
    };

    // The system label a header shows. Resolved by the caller (SystemCatalog is not QtCore-only), falling back
    // to the raw id so a catalogue naming a system this build does not emulate still groups under something
    // readable rather than under an empty header.
    using SystemNamer = std::function<QString(const QString&)>;
    using FactFinder  = std::function<Facts(const ExternalEmulator&)>;

    // The message an unreadable/empty catalogue puts on the one Error row. Named so the probe and the UI
    // cannot disagree about whether the section is empty or broken.
    inline QString emptyCatalogueMessage()
    {
        return QStringLiteral("The recomp catalogue could not be read. Nothing has been installed or removed.");
    }

    // THE BUILDER. Grouped by system (systems in ascending id order, so the list is stable across runs rather
    // than in catalogue-file order), titles sorted case-insensitively within a system.
    //
    // The id order is deliberate and is not a placeholder for "sorted by console name": the header's NAME
    // comes from a resolver that may return the same string for two ids, and sorting on it would make the
    // grouping depend on the display language. Ids are stable, so the order is.
    inline QVector<Row> buildRows(const QList<ExternalEmulator>& ports,
                                  const FactFinder& facts,
                                  const SystemNamer& systemName = {})
    {
        QVector<Row> out;

        // Only real ports. A catalogue file can carry an entry with no game binding (an id and nothing else);
        // it matches no game, it cannot be installed for one, and listing it would be listing a typo.
        QList<ExternalEmulator> usable;
        for (const ExternalEmulator& e : ports)
            if (e.isNativePort() && !e.port.platform.trimmed().isEmpty()) usable << e;

        if (usable.isEmpty())
        {
            Row err;
            err.kind = Row::Kind::Error;
            err.title = emptyCatalogueMessage();
            out.push_back(err);
            return out;
        }

        QStringList systems;
        for (const ExternalEmulator& e : usable)
        {
            const QString sys = e.port.platform.trimmed().toLower();
            if (!systems.contains(sys)) systems << sys;
        }
        std::sort(systems.begin(), systems.end());

        for (const QString& sys : systems)
        {
            QList<ExternalEmulator> group;
            for (const ExternalEmulator& e : usable)
                if (e.port.platform.trimmed().toLower() == sys) group << e;
            std::sort(group.begin(), group.end(), [](const ExternalEmulator& a, const ExternalEmulator& b) {
                const int c = QString::compare(a.port.name, b.port.name, Qt::CaseInsensitive);
                return c != 0 ? c < 0 : a.id < b.id;   // a stable tiebreak: two ports of one game are possible
            });

            Row hdr;
            hdr.kind = Row::Kind::SystemHeader;
            hdr.systemId = sys;
            const QString name = systemName ? systemName(sys).trimmed() : QString();
            hdr.title = name.isEmpty() ? sys : name;
            out.push_back(hdr);

            for (const ExternalEmulator& e : group)
            {
                Row r;
                r.kind = Row::Kind::Port;
                r.portId = e.id;
                r.systemId = sys;
                r.title = e.port.name;
                r.creditedName = e.displayName;
                r.license = e.port.license;
                r.engine = e.port.buildEngine.trimmed();
                r.tier = tierOf(e);
                const Facts f = facts ? facts(e) : Facts{};
                r.state = deriveState(f);
                // Only ever true on a row that actually rests on the title match — an installed port, or one
                // with no match at all, is not "unverified", it is simply not the question there.
                r.dumpUnverified = (r.state == State::NotInstalled) && f.dumpUnverified;
                out.push_back(r);
            }
        }
        return out;
    }

    // ---- the facts, gathered ---------------------------------------------------------------------------
    // One library candidate, reduced to what the match reads. A caller builds these from RomLibrary::scan()
    // and the Downloaded list; keeping the type local is what lets this header stay free of both.
    // The digests of ONE dump, as the library's hash cache holds them (HashVerify::cachedHashes). Lower-case
    // hex; any field may be empty, and an empty field means "not in the cache", never "does not match".
    //
    // Restated as its own type rather than including HashVerify.h because that unit is a .cpp-backed library
    // and this header is a pure one every probe compiles in isolation. The conversion is one struct literal at
    // the single call site, and the price of it is that the gate below cannot hash even by accident.
    struct CachedHashes
    {
        QString crc;     // CRC32, 8 hex chars
        QString md5;     // 32
        QString sha1;    // 40
        QString sha256;  // 64
        bool isEmpty() const
        { return crc.isEmpty() && md5.isEmpty() && sha1.isEmpty() && sha256.isEmpty(); }
    };

    struct LibraryRom
    {
        QString systemId;
        QString title;
        QString path;
        // For the cheap narrowing that decides whether this file is worth hashing at all. -1 = the caller
        // does not know, which is read as "do not refuse it on size" — an unknown never becomes a negative.
        qint64  size    = -1;
        bool    archive = false;      // a .zip/.7z holding the dump; see the header note on why it is exempt
        CachedHashes hashes;          // empty = never hashed; the gate reports Checking rather than No
    };

    // Does anything the user has match this entry's bound game? Asked through NativePorts::matchesRow — the
    // SAME function the game-row verb is gated on, so the section and the verb can never disagree about
    // whether a port applies to this machine.
    inline bool libraryMatches(const ExternalEmulator& port, const QVector<LibraryRom>& library)
    {
        for (const LibraryRom& r : library)
            if (NativePorts::matchesRow(port, r.systemId, r.title, r.path)) return true;
        return false;
    }

    // ---- the ROM-identity gate (issue #248, increment b) ------------------------------------------------
    // Does the entry publish anything to check bytes against? `disc_serials` is deliberately NOT counted:
    // RetComM treats it as an identity source because its own scanner reads the serial out of a disc image,
    // and this build has no reader for that. Counting it would make an entry look gated when the gate could
    // never fire, which is worse than the title match it would replace.
    inline bool publishesHash(const ExternalEmulator& port)
    {
        return !port.port.crc32.isEmpty() || !port.port.md5.isEmpty()
            || !port.port.sha1.isEmpty()  || !port.port.sha256.isEmpty();
    }

    // A case-insensitive membership test over a published digest list. Catalogue authors write hex both ways
    // and a capital letter is not a different dump.
    inline bool listHas(const QStringList& published, const QString& value)
    {
        if (value.isEmpty()) return false;
        for (const QString& p : published)
            if (p.trimmed().compare(value.trimmed(), Qt::CaseInsensitive) == 0) return true;
        return false;
    }

    // Do these cached digests satisfy the entry? ANY published kind matching is a match — the schema says so
    // in as many words ("authors may publish only the algorithm their gate uses"), and requiring agreement
    // across kinds would refuse a dump over a digest nobody computed.
    inline bool hashMatches(const ExternalEmulator& port, const CachedHashes& h)
    {
        return listHas(port.port.crc32,  h.crc)
            || listHas(port.port.md5,    h.md5)
            || listHas(port.port.sha1,   h.sha1)
            || listHas(port.port.sha256, h.sha256);
    }

    // Does the cache hold ANY of the kinds this entry publishes? False means the file has never been hashed
    // for this purpose (or was stamped by an older build that only kept sha1), so the honest answer about it
    // is "not yet", not "no".
    inline bool hashesCoverEntry(const ExternalEmulator& port, const CachedHashes& h)
    {
        if (!port.port.crc32.isEmpty()  && !h.crc.isEmpty())    return true;
        if (!port.port.md5.isEmpty()    && !h.md5.isEmpty())    return true;
        if (!port.port.sha1.isEmpty()   && !h.sha1.isEmpty())   return true;
        if (!port.port.sha256.isEmpty() && !h.sha256.isEmpty()) return true;
        return false;
    }

    // The file's extension, lower-cased and WITH the leading dot — the spelling `rom_extensions` uses.
    inline QString dottedExtension(const QString& path)
    {
        const int dot = path.lastIndexOf(QLatin1Char('.'));
        const int sep = qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
        if (dot <= sep) return QString();
        return path.mid(dot).toLower();
    }

    // Is this file worth the one-off hash for this entry? Three cheap facts, none of which opens the file.
    // See the header note for why an archive skips two of them.
    inline bool worthHashing(const ExternalEmulator& port, const LibraryRom& rom)
    {
        if (!port.isNativePort()) return false;
        const QString platform = port.port.platform.trimmed();
        if (platform.isEmpty() || rom.systemId.trimmed().compare(platform, Qt::CaseInsensitive) != 0)
            return false;
        if (rom.path.trimmed().isEmpty()) return false;
        if (!rom.archive)
        {
            if (!port.port.romExtensions.isEmpty()
                && !port.port.romExtensions.contains(dottedExtension(rom.path)))
                return false;
            // An unknown size is not a wrong size. Only a size the caller KNOWS, against a list the entry
            // actually published, refuses anything.
            if (!port.port.sizes.isEmpty() && rom.size >= 0 && !port.port.sizes.contains(rom.size))
                return false;
        }
        return true;
    }

    // The verdict for one entry against the whole library.
    enum class DumpMatch
    {
        None,       // nothing here is this dump
        Hashed,     // a digest the entry published matches a dump in the library
        TitleOnly,  // the entry published NO digest and a dump's NAME matched — "dump not verified"
        Checking,   // a plausible candidate exists whose digests are not cached yet
    };

    // Precedence, and it is the order of the loop rather than a sort: a Hashed match anywhere ends the
    // question immediately, and Checking is only reported once every candidate has failed to be one.
    inline DumpMatch dumpMatch(const ExternalEmulator& port, const QVector<LibraryRom>& library)
    {
        if (!port.isNativePort()) return DumpMatch::None;

        // NO PUBLISHED DIGEST. The title match is the whole gate, exactly as it was in increment (a) — and
        // the row is labelled so nobody reads it as a byte-level claim.
        if (!publishesHash(port))
            return libraryMatches(port, library) ? DumpMatch::TitleOnly : DumpMatch::None;

        bool checking = false;
        for (const LibraryRom& r : library)
        {
            if (!worthHashing(port, r)) continue;
            if (hashMatches(port, r.hashes)) return DumpMatch::Hashed;
            if (!hashesCoverEntry(port, r.hashes)) checking = true;   // nothing to compare yet — ask again
        }
        return checking ? DumpMatch::Checking : DumpMatch::None;
    }

    // The files the caller must hash — off the GUI thread, once each — before asking again. EMPTY whenever
    // the question is already answered, which is the property that stops an opened section re-hashing a disc
    // image on every redraw: a warm cache yields no work, and so does a match already found.
    //
    // This function is the ONLY thing in this header that names files, and it still opens none of them.
    inline QStringList pathsNeedingHash(const ExternalEmulator& port, const QVector<LibraryRom>& library)
    {
        QStringList out;
        if (!port.isNativePort() || !publishesHash(port)) return out;
        for (const LibraryRom& r : library)
        {
            if (!worthHashing(port, r)) continue;
            if (hashMatches(port, r.hashes)) return {};                 // answered; hash nothing further
            if (!hashesCoverEntry(port, r.hashes) && !out.contains(r.path)) out << r.path;
        }
        return out;
    }
}
