#include "SubtitleHash.h"

#include <QFile>
#include <QFileInfo>
#include <QDateTime>

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
    // Documented precondition, now enforced: a short window would silently yield a hash the server can never
    // match. ofFile already checks this; the guard covers every other caller (buildQueries/cacheIdentifier).
    if (head.size() != kWindow || tail.size() != kWindow) return QString();
    quint64 h = quint64(size);
    addWords(h, head);
    addWords(h, tail);
    return QStringLiteral("%1").arg(h, 16, 16, QLatin1Char('0'));
}

// A ONE-ENTRY memo: the same file is hashed twice per open (once for the download-cache key, once to build
// the moviehash query), and each pass is real disk I/O — an open + two 64 KiB reads with a seek between them,
// which is a visible hitch on a NAS/SMB share or a spun-down drive. The key carries size + mtime, so a file
// rewritten behind our back re-hashes. GUI-thread use only: this is deliberately unsynchronised.
QString ofFile(const QString& path)
{
    static QString lastKey, lastHash;
    const QFileInfo fi(path);
    if (!fi.exists()) return QString();
    const QString key = path + QLatin1Char('|') + QString::number(fi.size())
                      + QLatin1Char('|') + QString::number(fi.lastModified().toSecsSinceEpoch());
    if (key == lastKey) return lastHash;

    QString out;                                     // "" is a legitimate result (too small / short read) —
    QFile f(path);                                   // memoize it too, so an unhashable file isn't re-read
    if (f.open(QIODevice::ReadOnly))
    {
        const qint64 size = f.size();
        if (size >= kMinSize)                        // a file smaller than both windows has no valid hash
        {
            const QByteArray head = f.read(kWindow);
            if (f.seek(size - kWindow))
            {
                const QByteArray tail = f.read(kWindow);
                if (head.size() == kWindow && tail.size() == kWindow)   // short read ⇒ no hash
                    out = ofBytes(head, tail, size);
            }
        }
    }
    lastKey = key; lastHash = out;
    return out;
}
}
