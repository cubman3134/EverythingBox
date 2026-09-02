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

    // The four inputs, gathered by the caller from their one owner each (see the header note).
    struct Facts
    {
        bool    installed    = false;
        bool    libraryMatch = false;
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
        return f.libraryMatch ? State::NotInstalled : State::NeedsRom;
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
        Tier    tier = Tier::PreBuilt;
        State   state = State::NotInstalled;
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
                r.tier = tierOf(e);
                r.state = deriveState(facts ? facts(e) : Facts{});
                out.push_back(r);
            }
        }
        return out;
    }

    // ---- the facts, gathered ---------------------------------------------------------------------------
    // One library candidate, reduced to what the match reads. A caller builds these from RomLibrary::scan()
    // and the Downloaded list; keeping the type local is what lets this header stay free of both.
    struct LibraryRom
    {
        QString systemId;
        QString title;
        QString path;
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
}
