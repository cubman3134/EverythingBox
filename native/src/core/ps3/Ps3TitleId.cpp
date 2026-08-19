#include "core/ps3/Ps3TitleId.h"
#include "core/ps3/Ps3Sfo.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace Ps3TitleId {
namespace {

// A title id read here comes from bytes an untrusted .pkg or PARAM.SFO chose, and every consumer
// pastes it straight into a filesystem path: Ps3InstalledVersion::gameDir (which the installer then
// writes into and whose PARAM.SFO restoreSfo rewrites) and the ps3-heal-<titleId> marker, which is
// opened WriteOnly — i.e. truncating. An id of "../../../../Windows/System32/drivers/etc/hosts"
// would turn either into an arbitrary-file-clobber primitive. Real ids are short alphanumerics
// (BCUS98148, NPUB30910), so close the whole class at the boundary instead of hardening each sink:
// anything outside ^[A-Za-z0-9_-]{1,32}$ is not a title id and the caller gets nullopt (the same
// answer as an unreadable rom, which every caller already handles by skipping the update).
bool validId(const QString& id)
{
    if (id.isEmpty() || id.size() > 32) return false;
    for (const QChar c : id)
        if (!(c.isLetter() && c.unicode() < 128) && !(c.isDigit() && c.unicode() < 128)
            && c != QLatin1Char('_') && c != QLatin1Char('-'))
            return false;
    return true;
}

} // namespace

std::optional<QString> titleIdFromPkgHeader(const QByteArray& header)
{
    if (header.size() < 0x30 + 36) return std::nullopt;
    const auto* p = reinterpret_cast<const uchar*>(header.constData());
    if (!(p[0] == 0x7F && p[1] == 'P' && p[2] == 'K' && p[3] == 'G')) return std::nullopt;
    QByteArray cid = header.mid(0x30, 36);
    const int nul = cid.indexOf('\0'); if (nul >= 0) cid.truncate(nul);
    // content_id "XXYYYY-{TITLEID}_00-..." -> take the segment between the first '-' and the first '_'.
    const int dash = cid.indexOf('-'); if (dash < 0) return std::nullopt;
    const int us = cid.indexOf('_', dash); if (us < 0) return std::nullopt;
    const QByteArray tid = cid.mid(dash + 1, us - dash - 1);
    const QString id = QString::fromLatin1(tid);
    if (!validId(id)) return std::nullopt; // path sink — see validId
    return id;
}

namespace {
// Read the SFO at <gameRoot>/PS3_GAME/PARAM.SFO or <gameRoot>/PARAM.SFO.
std::optional<QString> fromGameRoot(const QString& root)
{
    for (const QString& rel : { QStringLiteral("/PS3_GAME/PARAM.SFO"), QStringLiteral("/PARAM.SFO") })
    {
        QFile f(root + rel);
        if (f.open(QIODevice::ReadOnly))
        {
            auto id = Ps3Sfo::titleIdFromSfo(f.readAll());
            // The SFO's bytes are as untrusted as the pkg header's; same path-sink gate. A rejected
            // id keeps looking (the second candidate path, then the walk-up in read()).
            if (id && validId(*id)) return id;
        }
    }
    return std::nullopt;
}
}

std::optional<QString> read(const QString& romPath)
{
    const QFileInfo fi(romPath);
    if (!fi.exists()) return std::nullopt;

    if (fi.isDir()) return fromGameRoot(QDir(romPath).absolutePath());

    if (fi.suffix().compare(QStringLiteral("pkg"), Qt::CaseInsensitive) == 0)
    {
        QFile f(romPath);
        if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
        return titleIdFromPkgHeader(f.read(0x60));
    }

    // A file inside a game tree (e.g. EBOOT.BIN): walk up to the game root (the dir that holds PS3_GAME,
    // or a PS3_GAME dir itself) and read the SFO there.
    QDir d = fi.absoluteDir();
    for (int hops = 0; hops < 6; ++hops)
    {
        if (auto id = fromGameRoot(d.absolutePath())) return id;
        if (d.dirName().compare(QStringLiteral("PS3_GAME"), Qt::CaseInsensitive) == 0)
            if (auto id = fromGameRoot(QFileInfo(d.absolutePath()).absolutePath())) return id;
        if (!d.cdUp()) break;
    }
    return std::nullopt;
}

} // namespace Ps3TitleId
