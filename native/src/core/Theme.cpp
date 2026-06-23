#include "Theme.h"
#include "ProfileStore.h"

#include <QSettings>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

static QSettings& store()
{
    static QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/goliath.ini"),
                       QSettings::IniFormat);
    return s;
}

static QVector<Theme> builtinThemes()
{
    QVector<Theme> v;
    { Theme t; t.name = QStringLiteral("Default"); t.accentFollowsTab = true;
      t.neutralTab = QColor(0x7E, 0x82, 0x8C); v.push_back(t); }                         // each tab its colour
    { Theme t; t.name = QStringLiteral("Sunset");  t.accentFollowsTab = false;
      t.accent = QColor(0xE0, 0x7A, 0x2E); t.neutralTab = QColor(0x9A, 0x8C, 0x82); v.push_back(t); }
    { Theme t; t.name = QStringLiteral("Ocean");   t.accentFollowsTab = false;
      t.accent = QColor(0x2E, 0x8B, 0xC0); t.neutralTab = QColor(0x86, 0x92, 0xA0); v.push_back(t); }
    { Theme t; t.name = QStringLiteral("Grape");   t.accentFollowsTab = false;
      t.accent = QColor(0x8A, 0x5C, 0xC8); t.neutralTab = QColor(0x90, 0x86, 0xA0); v.push_back(t); }
    { Theme t; t.name = QStringLiteral("Slate");   t.accentFollowsTab = false;
      t.accent = QColor(0x5A, 0x62, 0x70); t.neutralTab = QColor(0x88, 0x8C, 0x96); v.push_back(t); }
    { Theme t; t.name = QStringLiteral("Carousel"); t.accentFollowsTab = true; t.layout = QStringLiteral("carousel");
      t.neutralTab = QColor(0x7E, 0x82, 0x8C); v.push_back(t); }
    return v;
}

// Resolve a theme asset path (image/font) relative to the themes folder; absolute/existing paths pass through.
static QString resolveAsset(const QString& dir, const QString& path)
{
    if (path.isEmpty()) return QString();
    QFileInfo fi(path);
    if (fi.isAbsolute() && fi.exists()) return fi.absoluteFilePath();
    const QString p = QDir::cleanPath(dir + QStringLiteral("/") + path);
    return QFile::exists(p) ? p : QString();
}

static Theme parseTheme(const QJsonObject& o, const QString& fallbackName, const QString& dir)
{
    Theme t;
    t.name = o.value(QStringLiteral("name")).toString(fallbackName);
    t.accentFollowsTab = o.value(QStringLiteral("accentFollowsTab")).toBool(true);
    QColor a(o.value(QStringLiteral("accent")).toString());      if (a.isValid()) t.accent = a;
    QColor n(o.value(QStringLiteral("neutralTab")).toString());  if (n.isValid()) t.neutralTab = n;
    const QJsonObject tc = o.value(QStringLiteral("tabColors")).toObject();
    for (auto it = tc.begin(); it != tc.end(); ++it)
    {
        QColor c(it.value().toString());
        if (c.isValid()) t.tabColors.insert(it.key(), c);
    }

    t.fontFamily = o.value(QStringLiteral("fontFamily")).toString();
    t.fontScale  = o.value(QStringLiteral("fontScale")).toDouble(1.0);
    if (o.contains(QStringLiteral("cornerRadius"))) t.cornerRadius = o.value(QStringLiteral("cornerRadius")).toInt(9);
    t.background = resolveAsset(dir, o.value(QStringLiteral("background")).toString());
    if (o.contains(QStringLiteral("backgroundDim"))) t.backgroundDim = o.value(QStringLiteral("backgroundDim")).toDouble(0.55);

    const QJsonObject ic = o.value(QStringLiteral("icons")).toObject();
    for (auto it = ic.begin(); it != ic.end(); ++it)
    {
        const QString p = resolveAsset(dir, it.value().toString());
        if (!p.isEmpty()) t.icons.insert(it.key(), p);
    }

    t.layout = o.value(QStringLiteral("layout")).toString(QStringLiteral("tabs"));

    const QJsonObject d = o.value(QStringLiteral("detail")).toObject();
    if (!d.isEmpty())
    {
        t.detail.image = d.value(QStringLiteral("image")).toString(QStringLiteral("left"));
        if (d.contains(QStringLiteral("imageWidth"))) t.detail.imageWidth = d.value(QStringLiteral("imageWidth")).toInt(170);
        for (const QJsonValue& s : d.value(QStringLiteral("order")).toArray()) t.detail.order << s.toString();
    }
    return t;
}

static QVector<Theme> userThemes()
{
    QVector<Theme> v;
    const QString dirPath = QCoreApplication::applicationDirPath() + QStringLiteral("/themes");
    const QFileInfoList files = QDir(dirPath).entryInfoList(QStringList() << QStringLiteral("*.json"),
                                                            QDir::Files, QDir::Name);
    for (const QFileInfo& fi : files)
    {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        if (o.isEmpty()) continue;
        v.push_back(parseTheme(o, fi.completeBaseName(), dirPath));
    }
    return v;
}

QVector<Theme> ThemeStore::all()
{
    QVector<Theme> v = builtinThemes();
    v += userThemes(); // user files can also override a built-in name (last wins in byName())
    return v;
}

Theme ThemeStore::byName(const QString& name)
{
    const QVector<Theme> themes = all();
    for (int i = themes.size() - 1; i >= 0; --i) // last match wins, so user files can override built-ins
        if (themes[i].name.compare(name, Qt::CaseInsensitive) == 0) return themes[i];
    return builtinThemes().first();
}

QString ThemeStore::currentName()
{
    const QString id = ProfileStore::currentId();
    return store().value(QStringLiteral("theme/") + (id.isEmpty() ? QStringLiteral("default") : id),
                         QStringLiteral("Default")).toString();
}

void ThemeStore::setCurrent(const QString& name)
{
    const QString id = ProfileStore::currentId();
    store().setValue(QStringLiteral("theme/") + (id.isEmpty() ? QStringLiteral("default") : id), name);
    store().sync();
}

Theme ThemeStore::current() { return byName(currentName()); }
