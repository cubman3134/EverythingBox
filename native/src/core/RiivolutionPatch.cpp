#include "RiivolutionPatch.h"
#include <QXmlStreamReader>

namespace
{
    RiivolutionPatch::Parsed refuse(const QString& why)
    {
        RiivolutionPatch::Parsed p;
        p.ok = false;
        p.refusal = why;
        return p;
    }
}

RiivolutionPatch::Parsed RiivolutionPatch::parse(const QByteArray& xml)
{
    Parsed out;
    QXmlStreamReader r(xml);

    int optionCount = 0;
    int maxChoicesInAnOption = 0;
    int choicesInCurrentOption = 0;
    bool sawPatch = false;

    while (!r.atEnd())
    {
        r.readNext();
        if (r.hasError()) break;
        if (!r.isStartElement()) continue;

        const QStringView name = r.name();

        if (name == QLatin1String("option"))
        {
            ++optionCount;
            choicesInCurrentOption = 0;
        }
        else if (name == QLatin1String("choice"))
        {
            ++choicesInCurrentOption;
            if (choicesInCurrentOption > maxChoicesInAnOption)
                maxChoicesInAnOption = choicesInCurrentOption;
        }
        else if (name == QLatin1String("memory"))
        {
            // Refused, not ignored: a RAM patch cannot be baked into a disc image, and a disc composed
            // without it would boot and misbehave with nothing to say why.
            return refuse(QStringLiteral("this mod uses a <memory> patch, which changes the game while it "
                                         "runs and cannot be written into a disc image"));
        }
        else if (name == QLatin1String("savegame"))
        {
            out.savegameIgnored = true;
        }
        else if (name == QLatin1String("patch"))
        {
            // A <patch id=.../> REFERENCE inside a <choice> carries no root; the definition does.
            const auto attrs = r.attributes();
            if (attrs.hasAttribute(QLatin1String("root")))
            {
                sawPatch = true;
                out.root = attrs.value(QLatin1String("root")).toString();
            }
        }
        else if (name == QLatin1String("folder") || name == QLatin1String("file"))
        {
            const auto attrs = r.attributes();
            Op op;
            op.kind = (name == QLatin1String("folder")) ? Op::Folder : Op::File;
            op.discPath = attrs.value(QLatin1String("disc")).toString();
            op.externalPath = attrs.value(QLatin1String("external")).toString();
            op.create = attrs.value(QLatin1String("create")).toString() == QLatin1String("true");
            // Appended in document order and never keyed by either path: the measured document maps SEVEN
            // different disc folders onto one external folder, so a map keyed by external path would drop
            // six of them and the mod would install missing most of its localisations.
            if (!op.discPath.isEmpty() && !op.externalPath.isEmpty()) out.ops.append(op);
        }
    }

    if (r.hasError())
        return refuse(QStringLiteral("this mod's Riivolution file could not be read: %1").arg(r.errorString()));
    if (!sawPatch)
        return refuse(QStringLiteral("this mod's Riivolution file declares no patch to apply"));
    if (optionCount > 1 || maxChoicesInAnOption > 1)
        return refuse(QStringLiteral("this mod offers a choice of options, and there is no way to ask which "
                                     "one you want yet"));

    out.ok = true;
    return out;
}
