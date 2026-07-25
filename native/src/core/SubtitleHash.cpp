#include "SubtitleHash.h"

#include <QFile>

namespace {
constexpr int kWindow = 65536;              // 64 KiB, per the OSDb spec
constexpr qint64 kMinSize = 2 * kWindow;    // a file smaller than both windows has no valid hash

void addWords(quint64& h, const QByteArray& b)
{
    for (int i = 0; i + 8 <= b.size(); i += 8)
    {
        quint64 w = 0;
        for (int k = 7; k >= 0; --k) w = (w << 8) | quint8(b.at(i + k));   // little-endian
        h += w;                                                            // wraps at 64 bits, by design
    }
}
}

namespace SubtitleHash
{
QString ofBytes(const QByteArray& head, const QByteArray& tail, qint64 size)
{
    quint64 h = quint64(size);
    addWords(h, head);
    addWords(h, tail);
    return QStringLiteral("%1").arg(h, 16, 16, QLatin1Char('0'));
}

QString ofFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const qint64 size = f.size();
    if (size < kMinSize) return QString();          // too small to hash
    const QByteArray head = f.read(kWindow);
    if (!f.seek(size - kWindow)) return QString();
    const QByteArray tail = f.read(kWindow);
    if (head.size() != kWindow || tail.size() != kWindow) return QString();
    return ofBytes(head, tail, size);
}
}
