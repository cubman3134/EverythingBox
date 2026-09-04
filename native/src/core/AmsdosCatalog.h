// AMSDOS DISK CATALOGUE (issue #190) — what an Amstrad CPC .dsk actually holds, and the command that runs it.
//
// A CPC disk is not "insert and go": the machine boots to BASIC and waits for you to type RUN"<something>.
// The something is in the disk's own AMSDOS catalogue, and every CPC user knows to type CAT first. #190's job
// is to stop asking the user to know that — so this header reads the catalogue out of the .dsk bytes and
// derives the command.
//
// WHY IT MIRRORS CAP32 INSTEAD OF INVENTING A RULE. cap32 (the default CPC core) already does this, in
// libretro/dsk/loader.c::_loader_run, and its precedence is a decade of accumulated CPC folklore:
//
//     |CPM when the disk is a CP/M system disk with nothing in the AMSDOS catalogue
//     the first entry whose name starts DISC. / DISC / DISK.        (the near-universal CPC loader names)
//     JEU.BAS, then ELITE.BAS                                       (two specific publishers' conventions)
//     the ONE listed entry, when there is exactly one
//     the ONE hidden entry, when there are no listed ones
//     otherwise the first .BAS, then the first extension-less file, then the first .BIN
//     otherwise give up and type CAT
//
// Re-deriving that from scratch would produce a DIFFERENT command from the one the core is about to type,
// which is worse than useless: the frontend and the core would disagree about what the disk is. So this is a
// faithful re-implementation, cited line by line below, and its purpose is to let the app SAY what is about
// to happen — and, when the chain gives up, to say that too, which is the diagnostic #190 asks for by name
// ("a black screen with no explanation is the current failure mode"). It is the one place the app can tell
// the difference between "this disk boots itself" and "this disk will drop you at a catalogue listing".
//
// Sources, all read (libretro/libretro-cap32, master):
//   libretro/dsk/loader.c:52-64        _loader_launch  — RUN"<name>, or the bare name under CP/M
//   libretro/dsk/loader.c:218-259      _loader_run     — the precedence chain above
//   libretro/dsk/amsdos_catalog.c:49   GET_CHAR(x) = x & 0x7F   (AMSDOS puts attributes in the high bits)
//   libretro/dsk/amsdos_catalog.c:56   CATALOGUE_ATTRIB = 0xA   (extension byte 1, bit 7 = hidden/system)
//   libretro/dsk/amsdos_catalog.c:78   _find_sector    — sector id low nibble 1..4, 512-byte sectors only
//   libretro/dsk/amsdos_catalog.c:115  _is_valid_ext   — BAS / BIN / blank are runnable (COM under CP/M)
//   libretro/dsk/amsdos_catalog.c:352  _probe_track    — user 0, skip catalogue art, hidden -> hidden list
//   libretro/dsk/amsdos_catalog.c:405  _find_entries   — tracks 0..2, side 0
//   libretro/dsk/amsdos_catalog.c:428  _prepare_catalog— format id C0=DATA(t0) 40=SYSTEM(t2) 00=IBM(t1)
//   libretro/dsk/format.h:46-48        FORMAT_ID_DATA/SYSTEM/IBM
//
// EVERYTHING HERE IS PURE: it takes the .dsk BYTES, not a path, so probe_recipes drives it against a
// hand-built fixture image with no filesystem involved.
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace AmsdosCatalog
{
    struct Entry
    {
        QString name;             // "GAME.BAS" — AMSDOS spelling, upper case, one dot
        bool    hidden = false;   // the system/hidden attribute (extension byte 1, bit 7)
        int     track  = 0;       // which of tracks 0..2 it was found on (the chain compares this)
    };

    // What a .dsk turned out to hold. `ok` false means the bytes are not a CPCEMU disk image at all; every
    // other outcome (a readable image with an empty catalogue) is ok=true with no entries, because "this
    // disk has nothing runnable on it" is a real answer and the caller has to be able to say it.
    struct Catalogue
    {
        bool         ok = false;
        QString      error;
        QList<Entry> entries;      // in the order the core adds them: track 0..2, sector 1..4, entry 0..15
        bool         cpm = false;  // the disk is SYSTEM-formatted, so CP/M rules apply
        int          catalogueTrack = 0;   // the track the format id says the catalogue lives on
    };

    // ---- .dsk container ---------------------------------------------------------------------------------
    // One sector lifted out of a track: its id byte (the CHRN "R"), its size code (N) and its data.
    struct RawSector { int id = 0; int sizeCode = 0; QByteArray data; };

    inline quint16 le16(const QByteArray& b, int off)
    {
        if (off < 0 || off + 1 >= b.size()) return 0;
        return quint16(quint8(b.at(off))) | quint16(quint16(quint8(b.at(off + 1))) << 8);
    }

    // Read track `wanted` (side 0) out of a CPCEMU standard or EXTENDED .dsk. Empty on any malformed input:
    // this parser is fed whatever a user dropped in a folder, so every length is checked against the buffer.
    inline QList<RawSector> readTrack(const QByteArray& dsk, int wanted)
    {
        QList<RawSector> out;
        if (dsk.size() < 0x100) return out;
        const bool extended = dsk.startsWith("EXTENDED");
        if (!extended && !dsk.startsWith("MV - CPC")) return out;

        const int tracks = quint8(dsk.at(0x30));
        const int sides  = quint8(dsk.at(0x31));
        if (tracks <= 0 || sides <= 0 || sides > 2) return out;

        // Walk the track blocks in file order, because in an EXTENDED image they are variable length (and a
        // zero-length entry means the track is simply absent — it occupies no bytes at all).
        int offset = 0x100;
        for (int t = 0; t < tracks; ++t)
        {
            for (int s = 0; s < sides; ++s)
            {
                int blockLen = 0;
                if (extended)
                {
                    const int idx = 0x34 + t * sides + s;
                    if (idx >= dsk.size()) return out;
                    blockLen = int(quint8(dsk.at(idx))) * 256;
                }
                else
                {
                    blockLen = int(le16(dsk, 0x32));
                }
                if (blockLen == 0) continue;                 // absent track: no bytes in the file
                if (offset + blockLen > dsk.size()) return out;

                if (t == wanted && s == 0)
                {
                    const QByteArray blk = dsk.mid(offset, blockLen);
                    if (blk.size() < 0x100 || !blk.startsWith("Track-Info")) return out;
                    const int trackSizeCode = quint8(blk.at(0x14));
                    const int sectorCount   = quint8(blk.at(0x15));
                    int dataAt = 0x100;
                    for (int i = 0; i < sectorCount; ++i)
                    {
                        const int si = 0x18 + i * 8;
                        if (si + 7 >= blk.size()) return out;
                        RawSector rs;
                        rs.id       = quint8(blk.at(si + 2));           // R
                        rs.sizeCode = quint8(blk.at(si + 3));           // N
                        // EXTENDED images carry the real length per sector; a standard image has none, so the
                        // size code (of the TRACK, which is what CPCEMU wrote) is the only answer.
                        int len = extended ? int(le16(blk, si + 6)) : 0;
                        if (len <= 0) len = 128 << (extended ? rs.sizeCode : trackSizeCode);
                        if (dataAt + len > blk.size()) return out;
                        rs.data = blk.mid(dataAt, len);
                        dataAt += len;
                        out.push_back(rs);
                    }
                    return out;
                }
                offset += blockLen;
            }
        }
        return out;
    }

    // ---- the catalogue ----------------------------------------------------------------------------------
    inline char amsChar(char c) { return char(quint8(c) & 0x7F); }   // amsdos_catalog.c:49

    // amsdos_catalog.c:115 _is_valid_ext — only these run. Under CP/M it is .COM instead (:145 _is_valid_cpm).
    inline bool runnableExt(const QString& ext, bool cpm)
    {
        if (cpm) return ext == QLatin1String("COM");
        return ext == QLatin1String("BAS") || ext == QLatin1String("BIN") || ext == QLatin1String("   ");
    }

    // amsdos_catalog.c:237 __catalog_build_name — name up to the first space, '.', extension up to the first
    // space; rejected when the result is nothing but the dot.
    inline QString buildName(const QByteArray& raw)
    {
        QString name;
        for (int i = 0; i < 8; ++i)
        {
            const char c = amsChar(raw.at(1 + i));
            if (c == ' ') break;
            name += QLatin1Char(c);
        }
        QString ext;
        for (int i = 0; i < 3; ++i)
        {
            const char c = amsChar(raw.at(9 + i));
            if (c == ' ') break;
            ext += QLatin1Char(c);
        }
        if (name.isEmpty() && ext.isEmpty()) return QString();
        return name + QLatin1Char('.') + ext;
    }

    // amsdos_catalog.c:202 _is_catalogue_art — a "CAT art" entry decorates the catalogue listing with
    // graphics characters instead of naming a file. The tell is a non-printable byte inside the 11 name+ext
    // bytes. Once one is seen the core treats every later entry as hidden, which this mirrors.
    inline bool looksLikeCatArt(const QByteArray& raw)
    {
        int idx = 1;
        for (; idx < raw.size(); ++idx)
        {
            const char c = amsChar(raw.at(idx));
            if (c >= 0x20 && c <= 0x5A) continue;
            break;
        }
        return idx < 12;   // CATALOGUE_ART_SIZE
    }

    // Read the AMSDOS catalogue out of a whole .dsk. Mirrors _prepare_catalog + _find_entries + _probe_track.
    inline Catalogue read(const QByteArray& dsk)
    {
        Catalogue cat;
        const QList<RawSector> t0 = readTrack(dsk, 0);
        if (t0.isEmpty()) { cat.error = QStringLiteral("not a readable CPCEMU .dsk image"); return cat; }
        cat.ok = true;

        // amsdos_catalog.c:428 — the FIRST sector of track 0 declares the format in the top nibble of its id.
        const int formatId = t0.first().id & 0xF0;
        if (formatId == 0x40)      { cat.cpm = true; cat.catalogueTrack = 2; }   // FORMAT_ID_SYSTEM
        else if (formatId == 0x00) { cat.catalogueTrack = 1; }                   // FORMAT_ID_IBM
        else if (formatId == 0xC0) { cat.catalogueTrack = 0; }                   // FORMAT_ID_DATA
        else
        {
            cat.error = QStringLiteral("unsupported disk format id 0x%1")
                            .arg(formatId, 2, 16, QLatin1Char('0'));
            return cat;
        }

        bool sawCatArt = false;
        QStringList seen;
        // amsdos_catalog.c:405 — the core probes tracks 0..2 regardless of which one the format named.
        for (int track = 0; track <= 2; ++track)
        {
            const QList<RawSector> sectors = (track == 0) ? t0 : readTrack(dsk, track);
            for (int wantId = 1; wantId <= 4; ++wantId)
            {
                const RawSector* found = nullptr;
                for (const RawSector& rs : sectors)
                    if ((rs.id & 0x0F) == wantId && rs.sizeCode == 2) { found = &rs; break; }   // :78
                if (!found || found->data.isEmpty()) continue;

                const int total = found->data.size() / 32;
                for (int e = 0; e < total; ++e)
                {
                    const QByteArray raw = found->data.mid(e * 32, 32);
                    if (raw.size() < 32) break;
                    if (quint8(raw.at(0)) != 0) continue;          // user 0 only
                    if (looksLikeCatArt(raw)) { sawCatArt = true; continue; }
                    if (quint8(raw.at(15)) == 0) continue;         // record count 0 -> not a real extent
                    bool quoted = false;
                    for (int i = 1; i <= 11; ++i) if (amsChar(raw.at(i)) == '"') quoted = true;
                    if (quoted) continue;

                    QString ext;
                    for (int i = 0; i < 3; ++i) ext += QLatin1Char(amsChar(raw.at(9 + i)));
                    if (!runnableExt(ext, cat.cpm)) continue;

                    const QString name = buildName(raw);
                    if (name.isEmpty() || seen.contains(name)) continue;
                    seen.push_back(name);

                    Entry en;
                    en.name   = name;
                    en.track  = track;
                    en.hidden = (quint8(raw.at(0x0A)) & 0x80) != 0 || sawCatArt;   // CATALOGUE_ATTRIB
                    cat.entries.push_back(en);
                }
            }
        }
        return cat;
    }

    // ---- the command --------------------------------------------------------------------------------------
    inline QString launch(const Catalogue& cat, const QString& file)
    {
        if (cat.cpm) return file;                                          // loader.c:52-57
        return QStringLiteral("RUN\"%1").arg(file);                        // loader.c:58
    }

    // loader.c:218 _loader_run, step for step. Returns the command the disk boots with, or an EMPTY string
    // where the core gives up and types CAT — which is exactly the case worth telling the user about.
    inline QString bootCommand(const Catalogue& cat)
    {
        if (!cat.ok) return QString();

        int listed = 0, hidden = 0, firstListed = -1, firstHidden = -1;
        int trackListed = -1, trackHidden = -1;
        for (int i = 0; i < cat.entries.size(); ++i)
        {
            const Entry& e = cat.entries.at(i);
            if (e.hidden) { if (firstHidden < 0) { firstHidden = i; trackHidden = e.track; } ++hidden; }
            else          { if (firstListed < 0) { firstListed = i; trackListed = e.track; } ++listed; }
        }

        // _loader_cpm (:186): a CP/M system disk with an empty AMSDOS catalogue boots the CP/M ROM.
        if (cat.cpm && listed == 0 && hidden == 0) return QStringLiteral("|CPM");

        // _loader_find_file (:225-241): the conventional loader names, in this order, matched as PREFIXES.
        static const char* const kConventional[] = { "DISC.", "DISC", "DISK.", "JEU.BAS", "ELITE.BAS" };
        for (const char* const want : kConventional)
        {
            const QString w = QString::fromLatin1(want);
            for (const Entry& e : cat.entries)
                if (e.name.startsWith(w)) return launch(cat, e.name);
        }

        // _loader_one_listed (:148): exactly one entry -> that is the game.
        if (!cat.cpm && listed == 1) return launch(cat, cat.entries.at(firstListed).name);
        if (cat.cpm && (listed == 1 || hidden == 1))
            return launch(cat, cat.entries.at(listed == 1 ? firstListed : firstHidden).name);

        // _loader_hidden (:163): nothing listed and exactly one hidden entry, on the catalogue's own track.
        if (listed == 0 && hidden == 1 && trackHidden == cat.catalogueTrack)
            return launch(cat, cat.entries.at(firstHidden).name);

        // _loader_find (:86): first .BAS, then the first extension-less file, then the first .BIN — but only
        // when the catalogue was actually found where the format said it would be.
        if (trackListed == cat.catalogueTrack || trackHidden == cat.catalogueTrack)
        {
            int firstBas = -1, firstBlank = -1, firstBin = -1;
            for (int i = 0; i < cat.entries.size(); ++i)
            {
                const QString n = cat.entries.at(i).name;
                const int dot = n.lastIndexOf(QLatin1Char('.'));
                const QString ext = dot < 0 ? QString() : n.mid(dot + 1);
                if (ext == QLatin1String("BAS"))      { if (firstBas < 0)   firstBas = i; }
                else if (ext.isEmpty())               { if (firstBlank < 0) firstBlank = i; }
                else if (ext == QLatin1String("BIN")) { if (firstBin < 0)   firstBin = i; }
            }
            const int pick = firstBas >= 0 ? firstBas : (firstBlank >= 0 ? firstBlank : firstBin);
            if (pick >= 0) return launch(cat, cat.entries.at(pick).name);
        }

        return QString();   // _loader_failed (:203): the core types CAT and the user is on their own
    }

    // The listing the user would get by typing CAT, for the log and for the "nothing runnable here" message.
    inline QStringList names(const Catalogue& cat)
    {
        QStringList out;
        for (const Entry& e : cat.entries) out << e.name;
        return out;
    }
}
