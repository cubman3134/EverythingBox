// Headless pure-logic probe for the PS3 auto-update units. Prints PS3UPDATE-OK on success.
// No display, no network, no process spawns — every external effect is an injected seam.
#include "core/ps3/Ps3Sfo.h"

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QPair>
#include <QtEndian>
#include <cstdio>
#include <optional>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #c, __LINE__); ++g_fail; } } while (0)

// Build a minimal valid PARAM.SFO holding the given UTF-8 string keys, so the parser is exercised
// against real bytes rather than a magic blob. Layout: 20-byte header, N index entries (16 bytes each),
// key table (null-terminated names), data table (null-terminated values padded to max len).
static QByteArray makeSfo(const QVector<QPair<QString, QString>>& kv)
{
    auto u16 = [](quint16 v) { char b[2]; qToLittleEndian(v, b); return QByteArray(b, 2); };
    auto u32 = [](quint32 v) { char b[4]; qToLittleEndian(v, b); return QByteArray(b, 4); };

    QByteArray keyTable, dataTable, index;
    QVector<quint32> keyOffs, dataOffs, dataLens, dataMax;
    for (const auto& p : kv)
    {
        QByteArray k = p.first.toUtf8();  k.append('\0');
        QByteArray d = p.second.toUtf8(); d.append('\0');
        const quint32 maxLen = static_cast<quint32>((d.size() + 15) & ~15); // pad to 16
        keyOffs.append(static_cast<quint32>(keyTable.size()));
        dataOffs.append(static_cast<quint32>(dataTable.size()));
        dataLens.append(static_cast<quint32>(d.size()));
        dataMax.append(maxLen);
        keyTable.append(k);
        dataTable.append(d);
        dataTable.append(QByteArray(static_cast<int>(maxLen) - d.size(), '\0'));
    }
    const quint32 entries = static_cast<quint32>(kv.size());
    const quint32 keyStart = 20 + entries * 16;
    const quint32 dataStart = keyStart + static_cast<quint32>(keyTable.size());
    for (quint32 i = 0; i < entries; ++i)
    {
        index += u16(static_cast<quint16>(keyOffs[i]));
        index += u16(0x0204); // utf8 null-terminated
        index += u32(dataLens[i]);
        index += u32(dataMax[i]);
        index += u32(dataOffs[i]);
    }
    QByteArray out;
    out.append('\0'); out.append("PSF", 3);       // magic \0PSF
    out += u32(0x00000101);                        // version 1.1
    out += u32(keyStart);
    out += u32(dataStart);
    out += u32(entries);
    out += index; out += keyTable; out += dataTable;
    return out;
}

static void testSfo()
{
    const QByteArray sfo = makeSfo({ { "APP_VER", "01.00" }, { "TITLE_ID", "BLUS31156" }, { "TITLE", "GTA V" } });
    auto id = Ps3Sfo::titleIdFromSfo(sfo);
    CHECK(id.has_value());
    CHECK(id.value_or(QString()) == QStringLiteral("BLUS31156"));

    CHECK(!Ps3Sfo::titleIdFromSfo(makeSfo({ { "TITLE", "No id here" } })).has_value());
    CHECK(!Ps3Sfo::titleIdFromSfo(QByteArray("not an sfo")).has_value());
    CHECK(!Ps3Sfo::titleIdFromSfo(QByteArray()).has_value());
}

int main()
{
    testSfo();
    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("PS3UPDATE-OK\n");
    return 0;
}
