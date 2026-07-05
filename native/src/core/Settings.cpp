#include "Settings.h"
#include "AppPaths.h"
#include <QSettings>
#include <QCoreApplication>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

bool Settings::subtitlesOnByDefault()
{
    return store().value(QStringLiteral("subs/onByDefault"), false).toBool();
}

void Settings::setSubtitlesOnByDefault(bool on)
{
    store().setValue(QStringLiteral("subs/onByDefault"), on);
    store().sync();
}

QString Settings::subtitleLanguage()
{
    return store().value(QStringLiteral("subs/language")).toString();
}

void Settings::setSubtitleLanguage(const QString& code)
{
    store().setValue(QStringLiteral("subs/language"), code);
    store().sync();
}

bool Settings::autoplayNextEpisode() { return store().value(QStringLiteral("playback/autoplayNext"), true).toBool(); }
void Settings::setAutoplayNextEpisode(bool on)
{
    store().setValue(QStringLiteral("playback/autoplayNext"), on); store().sync();
}

QString Settings::traktClientId() { return store().value(QStringLiteral("trakt/clientId")).toString(); }
void Settings::setTraktClientId(const QString& v) { store().setValue(QStringLiteral("trakt/clientId"), v.trimmed()); store().sync(); }
QString Settings::traktClientSecret() { return store().value(QStringLiteral("trakt/clientSecret")).toString(); }
void Settings::setTraktClientSecret(const QString& v) { store().setValue(QStringLiteral("trakt/clientSecret"), v.trimmed()); store().sync(); }
QString Settings::traktAccessToken() { return store().value(QStringLiteral("trakt/access")).toString(); }
QString Settings::traktRefreshToken() { return store().value(QStringLiteral("trakt/refresh")).toString(); }
qint64  Settings::traktTokenExpiry() { return store().value(QStringLiteral("trakt/expiry"), 0).toLongLong(); }
void Settings::setTraktTokens(const QString& access, const QString& refresh, qint64 expiryUnix)
{
    store().setValue(QStringLiteral("trakt/access"), access);
    store().setValue(QStringLiteral("trakt/refresh"), refresh);
    store().setValue(QStringLiteral("trakt/expiry"), expiryUnix);
    store().sync();
}
void Settings::clearTraktTokens()
{
    store().remove(QStringLiteral("trakt/access"));
    store().remove(QStringLiteral("trakt/refresh"));
    store().remove(QStringLiteral("trakt/expiry"));
    store().sync();
}

QString Settings::openSubApiKey() { return store().value(QStringLiteral("subs/osApiKey")).toString(); }
void Settings::setOpenSubApiKey(const QString& key)
{
    store().setValue(QStringLiteral("subs/osApiKey"), key.trimmed()); store().sync();
}
QString Settings::openSubUsername() { return store().value(QStringLiteral("subs/osUser")).toString(); }
void Settings::setOpenSubUsername(const QString& user)
{
    store().setValue(QStringLiteral("subs/osUser"), user.trimmed()); store().sync();
}
QString Settings::openSubPassword() { return store().value(QStringLiteral("subs/osPass")).toString(); }
void Settings::setOpenSubPassword(const QString& pass)
{
    store().setValue(QStringLiteral("subs/osPass"), pass); store().sync();
}

QString Settings::videoFilter() { return store().value(QStringLiteral("emu/videoFilter"), QStringLiteral("off")).toString(); }
void Settings::setVideoFilter(const QString& id)
{
    store().setValue(QStringLiteral("emu/videoFilter"), id); store().sync();
}

bool Settings::startFullscreen() { return store().value(QStringLiteral("general/startFullscreen"), false).toBool(); }
void Settings::setStartFullscreen(bool on)
{
    store().setValue(QStringLiteral("general/startFullscreen"), on); store().sync();
}
bool Settings::checkUpdatesOnStartup() { return store().value(QStringLiteral("general/checkUpdatesOnStartup"), true).toBool(); }
void Settings::setCheckUpdatesOnStartup(bool on)
{
    store().setValue(QStringLiteral("general/checkUpdatesOnStartup"), on); store().sync();
}

QString Settings::romsFolder()
{
    const QString p = store().value(QStringLiteral("roms/folder")).toString();
    return p.isEmpty() ? (AppPaths::dataDir() + QStringLiteral("/roms")) : p;
}
void Settings::setRomsFolder(const QString& path)
{
    store().setValue(QStringLiteral("roms/folder"), path); store().sync();
}

bool Settings::bgmEnabled() { return store().value(QStringLiteral("bgm/enabled"), true).toBool(); }
void Settings::setBgmEnabled(bool on) { store().setValue(QStringLiteral("bgm/enabled"), on); store().sync(); }
int  Settings::bgmVolume() { return store().value(QStringLiteral("bgm/volume"), 35).toInt(); }
void Settings::setBgmVolume(int pct)
{
    store().setValue(QStringLiteral("bgm/volume"), qBound(0, pct, 100)); store().sync();
}

QString Settings::coreFor(const QString& systemId)
{
    return store().value(QStringLiteral("cores/") + systemId).toString();
}

void Settings::setCoreFor(const QString& systemId, const QString& core)
{
    store().setValue(QStringLiteral("cores/") + systemId, core);
    store().sync();
}

// Keyed "opt/<core>/<key>". The option key is the core's own (e.g. "mgba_gb_model"); it can't collide
// across cores because <core> namespaces it.
QString Settings::optionValue(const QString& core, const QString& key)
{
    return store().value(QStringLiteral("opt/") + core + QStringLiteral("/") + key).toString();
}

void Settings::setOptionValue(const QString& core, const QString& key, const QString& value)
{
    store().setValue(QStringLiteral("opt/") + core + QStringLiteral("/") + key, value);
    store().sync();
}

int Settings::padBinding(int port, int retroId, int defaultCode)
{
    return store().value(QStringLiteral("pad/%1/%2").arg(port).arg(retroId), defaultCode).toInt();
}

void Settings::setPadBinding(int port, int retroId, int code)
{
    store().setValue(QStringLiteral("pad/%1/%2").arg(port).arg(retroId), code);
    store().sync();
}

int Settings::keyBinding(int port, int retroId, int defaultKey)
{
    return store().value(QStringLiteral("kbd/%1/%2").arg(port).arg(retroId), defaultKey).toInt();
}

void Settings::setKeyBinding(int port, int retroId, int qtKey)
{
    store().setValue(QStringLiteral("kbd/%1/%2").arg(port).arg(retroId), qtKey);
    store().sync();
}

bool Settings::turboButton(int port, int retroId)
{
    return store().value(QStringLiteral("turbo/%1/%2").arg(port).arg(retroId), false).toBool();
}

void Settings::setTurboButton(int port, int retroId, bool on)
{
    store().setValue(QStringLiteral("turbo/%1/%2").arg(port).arg(retroId), on);
    store().sync();
}

int Settings::turboHalfPeriod()
{
    return qBound(1, store().value(QStringLiteral("turbo/halfPeriod"), 3).toInt(), 30);
}

void Settings::setTurboHalfPeriod(int frames)
{
    store().setValue(QStringLiteral("turbo/halfPeriod"), qBound(1, frames, 30));
    store().sync();
}
