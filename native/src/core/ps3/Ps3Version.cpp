#include "core/ps3/Ps3Version.h"
#include <QStringList>
namespace Ps3Version {
bool less(const QString& a, const QString& b)
{
    const auto pa = a.split(QLatin1Char('.')); const auto pb = b.split(QLatin1Char('.'));
    const int amaj = pa.value(0).toInt(), amin = pa.value(1).toInt();
    const int bmaj = pb.value(0).toInt(), bmin = pb.value(1).toInt();
    if (amaj != bmaj) return amaj < bmaj;
    return amin < bmin;
}
}
