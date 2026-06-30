#include "Theme.h"
#include "AppPaths.h"
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
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

static QVector<Theme> builtinThemes()
{
    // The themeable front end is the theme2 system (Settings -> Appearance). The classic widgets (the info
    // page, library, settings) just need one neutral dark base so they never look like an "old theme".
    QVector<Theme> v;
    { Theme t; t.name = QStringLiteral("Default"); t.dark = true; t.accentFollowsTab = false;
      t.accent = QColor(0x1B, 0x20, 0x2A); t.neutralTab = QColor(0x2A, 0x2F, 0x3A); v.push_back(t); }
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
    const QString dirPath = AppPaths::dataDir() + QStringLiteral("/themes");
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
    // The classic colour-theme system is retired in favour of the theme2 front end (Settings -> Appearance).
    // Only the single neutral dark base remains, so any old/leftover theme name falls back to it (byName) and
    // the classic widgets never show an "old theme".
    return builtinThemes();
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
