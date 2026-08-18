#include "core/ps3/Ps3Sfo.h"
#include <QtEndian>

namespace Ps3Sfo {

std::optional<QString> titleIdFromSfo(const QByteArray& sfo)
{
    if (sfo.size() < 20) return std::nullopt;
    const auto* p = reinterpret_cast<const uchar*>(sfo.constData());
    // magic "\0PSF"
    if (!(p[0] == 0x00 && p[1] == 'P' && p[2] == 'S' && p[3] == 'F')) return std::nullopt;

    const quint32 keyStart  = qFromLittleEndian<quint32>(p + 8);
    const quint32 dataStart = qFromLittleEndian<quint32>(p + 12);
    const quint32 entries   = qFromLittleEndian<quint32>(p + 16);
    const int size = sfo.size();
    if (keyStart > static_cast<quint32>(size) || dataStart > static_cast<quint32>(size)) return std::nullopt;
    // guard the index table
    if (20 + static_cast<qint64>(entries) * 16 > size) return std::nullopt;

    for (quint32 i = 0; i < entries; ++i)
    {
        const uchar* e = p + 20 + i * 16;
        const quint16 keyOff  = qFromLittleEndian<quint16>(e + 0);
        const quint32 dataLen = qFromLittleEndian<quint32>(e + 4);
        const quint32 dataOff = qFromLittleEndian<quint32>(e + 12);

        // read the null-terminated key name
        const quint32 kpos = keyStart + keyOff;
        if (kpos >= static_cast<quint32>(size)) continue;
        int kend = static_cast<int>(kpos);
        while (kend < size && sfo[kend] != '\0') ++kend;
        const QByteArray key = sfo.mid(static_cast<int>(kpos), kend - static_cast<int>(kpos));
        if (key != "TITLE_ID") continue;

        const quint32 dpos = dataStart + dataOff;
        if (dpos > static_cast<quint32>(size)) return std::nullopt;
        int avail = size - static_cast<int>(dpos);
        int len = static_cast<int>(qMin<quint32>(dataLen, static_cast<quint32>(avail)));
        QByteArray val = sfo.mid(static_cast<int>(dpos), len);
        const int nul = val.indexOf('\0');
        if (nul >= 0) val.truncate(nul); // strings are null-terminated
        if (val.isEmpty()) return std::nullopt;
        return QString::fromLatin1(val); // Title IDs are ASCII
    }
    return std::nullopt;
}

} // namespace Ps3Sfo
