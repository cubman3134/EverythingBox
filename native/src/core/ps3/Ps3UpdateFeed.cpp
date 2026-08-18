#include "core/ps3/Ps3UpdateFeed.h"
#include "core/ps3/Ps3Version.h"
#include <QXmlStreamReader>
#include <algorithm>

namespace Ps3UpdateFeed {

QVector<Ps3UpdatePackage> parseVerXml(const QByteArray& xml)
{
    QVector<Ps3UpdatePackage> out;
    if (xml.trimmed().isEmpty()) return out;

    QXmlStreamReader r(xml);
    while (!r.atEnd())
    {
        if (r.readNext() == QXmlStreamReader::StartElement && r.name() == QLatin1String("package"))
        {
            const auto a = r.attributes();
            Ps3UpdatePackage p;
            p.version      = a.value(QLatin1String("version")).toString();
            p.size         = a.value(QLatin1String("size")).toLongLong();
            p.sha1         = a.value(QLatin1String("sha1sum")).toString();
            p.url          = a.value(QLatin1String("url")).toString();
            p.ps3SystemVer = a.value(QLatin1String("ps3_system_ver")).toString();
            if (!p.url.isEmpty()) out.append(p);
        }
    }
    if (r.hasError()) return {}; // malformed -> no updates, never fatal

    std::sort(out.begin(), out.end(),
              [](const Ps3UpdatePackage& x, const Ps3UpdatePackage& y) { return Ps3Version::less(x.version, y.version); });
    return out;
}

} // namespace Ps3UpdateFeed
