#include "Settings.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "../theme2/FormFactor.h"   // virtualPadEnabled() resolves "auto" against the form-factor authority
#include <QSettings>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QUuid>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QString Settings::deviceId()
{
    // Write-once: an already-minted id is returned verbatim and NEVER regenerated (a fresh UUID on every
    // read would give this device a new identity each launch and defeat per-device namespacing). Only the
    // very first call — when the key is absent/empty — mints and persists.
    const QString existing = store().value(QStringLiteral("device/id")).toString();
    if (!existing.isEmpty()) return existing;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    store().setValue(QStringLiteral("device/id"), id);
    store().sync();
    return id;
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

// Subtitle appearance (issue #71). Each accessor is the ordinary read-default / write-and-sync pair; the
// defaults deliberately mirror mpv's own (scale 1.0, border 3, sans-serif, bottom, no box) so a fresh install
// renders as mpv would with no options set. subtitleStyle() assembles them into the pure Style the player and
// probe_substyle map to mpv options — one place gathers the group so a new field is added in one obvious spot.
SubtitleStyle::Style Settings::subtitleStyle()
{
    SubtitleStyle::Style s;
    s.fontFamily        = subtitleFont();
    s.sizePercent       = subtitleSizePercent();
    s.textColor         = subtitleColor();
    s.borderSize        = subtitleBorderSize();
    s.borderColor       = subtitleBorderColor();
    s.boxEnabled        = subtitleBox();
    s.boxOpacityPercent = subtitleBoxOpacity();
    s.position          = subtitlePosition();
    s.bold              = subtitleBold();
    s.overrideStyled    = subtitleOverrideStyled();
    return s;
}

QString Settings::subtitleFont() { return store().value(QStringLiteral("subs/font")).toString(); }
void Settings::setSubtitleFont(const QString& family)
{
    store().setValue(QStringLiteral("subs/font"), family); store().sync();
}

int Settings::subtitleSizePercent() { return store().value(QStringLiteral("subs/sizePercent"), 100).toInt(); }
void Settings::setSubtitleSizePercent(int pct)
{
    store().setValue(QStringLiteral("subs/sizePercent"), qBound(10, pct, 1000)); store().sync();
}

QString Settings::subtitleColor()
{
    return store().value(QStringLiteral("subs/color"), QStringLiteral("#FFFFFF")).toString();
}
void Settings::setSubtitleColor(const QString& hex)
{
    store().setValue(QStringLiteral("subs/color"), hex); store().sync();
}

int Settings::subtitleBorderSize() { return store().value(QStringLiteral("subs/borderSize"), 3).toInt(); }
void Settings::setSubtitleBorderSize(int px)
{
    store().setValue(QStringLiteral("subs/borderSize"), qBound(0, px, 20)); store().sync();
}

QString Settings::subtitleBorderColor()
{
    return store().value(QStringLiteral("subs/borderColor"), QStringLiteral("#000000")).toString();
}
void Settings::setSubtitleBorderColor(const QString& hex)
{
    store().setValue(QStringLiteral("subs/borderColor"), hex); store().sync();
}

bool Settings::subtitleBox() { return store().value(QStringLiteral("subs/box"), false).toBool(); }
void Settings::setSubtitleBox(bool on)
{
    store().setValue(QStringLiteral("subs/box"), on); store().sync();
}

int Settings::subtitleBoxOpacity() { return store().value(QStringLiteral("subs/boxOpacity"), 75).toInt(); }
void Settings::setSubtitleBoxOpacity(int pct)
{
    store().setValue(QStringLiteral("subs/boxOpacity"), qBound(0, pct, 100)); store().sync();
}

int Settings::subtitlePosition() { return store().value(QStringLiteral("subs/pos"), 100).toInt(); }
void Settings::setSubtitlePosition(int pos)
{
    store().setValue(QStringLiteral("subs/pos"), qBound(0, pos, 150)); store().sync();
}

bool Settings::subtitleBold() { return store().value(QStringLiteral("subs/bold"), false).toBool(); }
void Settings::setSubtitleBold(bool on)
{
    store().setValue(QStringLiteral("subs/bold"), on); store().sync();
}

bool Settings::subtitleOverrideStyled() { return store().value(QStringLiteral("subs/assOverride"), false).toBool(); }
void Settings::setSubtitleOverrideStyled(bool on)
{
    store().setValue(QStringLiteral("subs/assOverride"), on); store().sync();
}

// Audio output (issue #69). Defaults mirror mpv's own no-options-set behaviour (Auto device, no passthrough,
// shared mode) so a fresh install outputs as mpv would. The "audio/" group is device-local (CloudSync's
// carve-out excludes it): an audio-device id names a sound card on THIS machine and would point another
// device at the wrong output if synced. audioOutput() assembles the group into the pure Output struct the
// player and probe_audioout map to mpv options — one place gathers it so a new field is added in one spot.
AudioOutput::Output Settings::audioOutput()
{
    AudioOutput::Output o;
    o.device      = audioDevice();
    o.passthrough = audioPassthrough();
    o.exclusive   = audioExclusive();
    return o;
}

QString Settings::audioDevice() { return store().value(QStringLiteral("audio/device")).toString(); }
void Settings::setAudioDevice(const QString& id)
{
    store().setValue(QStringLiteral("audio/device"), id.trimmed()); store().sync();
}

bool Settings::audioPassthrough() { return store().value(QStringLiteral("audio/passthrough"), false).toBool(); }
void Settings::setAudioPassthrough(bool on)
{
    store().setValue(QStringLiteral("audio/passthrough"), on); store().sync();
}

bool Settings::audioExclusive() { return store().value(QStringLiteral("audio/exclusive"), false).toBool(); }
void Settings::setAudioExclusive(bool on)
{
    store().setValue(QStringLiteral("audio/exclusive"), on); store().sync();
}

bool Settings::autoplayNextEpisode() { return store().value(QStringLiteral("playback/autoplayNext"), true).toBool(); }
void Settings::setAutoplayNextEpisode(bool on)
{
    store().setValue(QStringLiteral("playback/autoplayNext"), on); store().sync();
}

// Read-and-write only, so single-line — but they sync() like every other setter in this file, so a crash
// before the next flush cannot lose the user's choice.
bool Settings::skipSegments() { return store().value(QStringLiteral("playback/skipSegments"), true).toBool(); }
void Settings::setSkipSegments(bool on) { store().setValue(QStringLiteral("playback/skipSegments"), on); store().sync(); }
bool Settings::skipSegmentsAuto() { return store().value(QStringLiteral("playback/skipSegmentsAuto"), false).toBool(); }
void Settings::setSkipSegmentsAuto(bool on) { store().setValue(QStringLiteral("playback/skipSegmentsAuto"), on); store().sync(); }

// The PIN is stored as SHA-256(salt + pin) — enough to keep it out of the ini in the clear and deter a
// casual child; it isn't a cryptographic secret store.
//
// The salt is AppBrand::Legacy::kParentalPinSalt and must STAY legacy: it is an input to a hash already
// written to every existing user's ini, so "renaming" it silently redefines the function and no PIN a user
// previously set ever matches again. The rebrand's prose sweep did rename it, which is why it now lives as a
// named legacy constant rather than a literal a future sweep can walk over. See AppBrand.h.
static QString pinHash(const QString& pin)
{
    const QByteArray in = QByteArray(AppBrand::Legacy::kParentalPinSalt) + pin.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(in, QCryptographicHash::Sha256).toHex());
}
bool Settings::hasParentalPin() { return !store().value(QStringLiteral("parental/pinHash")).toString().isEmpty(); }
void Settings::setParentalPin(const QString& pin)
{
    if (pin.isEmpty()) store().remove(QStringLiteral("parental/pinHash"));
    else store().setValue(QStringLiteral("parental/pinHash"), pinHash(pin));
    store().sync();
}
bool Settings::checkParentalPin(const QString& pin)
{
    const QString h = store().value(QStringLiteral("parental/pinHash")).toString();
    return !h.isEmpty() && h == pinHash(pin);
}

// Default FALSE = always show the picker (issue #30). The old behaviour — a one-profile install jumping
// straight in — is now the opt-in, not the default: "who's using this?" is the question a shared TV box has
// to ask, and the previous code answered it silently whenever the answer looked obvious.
bool Settings::skipProfilePickerWhenSingle()
{
    return store().value(QStringLiteral("profiles/skipPickerWhenSingle"), false).toBool();
}
void Settings::setSkipProfilePickerWhenSingle(bool on)
{
    store().setValue(QStringLiteral("profiles/skipPickerWhenSingle"), on); store().sync();
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

QString Settings::steamWebApiKey() { return store().value(QStringLiteral("steam/apikey")).toString(); }
void Settings::setSteamWebApiKey(const QString& key)
{
    store().setValue(QStringLiteral("steam/apikey"), key.trimmed()); store().sync();
}
QString Settings::steamId() { return store().value(QStringLiteral("steam/steamid")).toString(); }
void Settings::setSteamId(const QString& id)
{
    store().setValue(QStringLiteral("steam/steamid"), id.trimmed()); store().sync();
}

QString Settings::videoFilter() { return store().value(QStringLiteral("emu/videoFilter"), QStringLiteral("off")).toString(); }
void Settings::setVideoFilter(const QString& id)
{
    store().setValue(QStringLiteral("emu/videoFilter"), id); store().sync();
}

QString Settings::hwDecode() { return store().value(QStringLiteral("video/hwdec"), QStringLiteral("auto")).toString(); }
void Settings::setHwDecode(const QString& mode)
{
    store().setValue(QStringLiteral("video/hwdec"), mode.trimmed()); store().sync();
}

QString Settings::netplayRelay() { return store().value(QStringLiteral("netplay/relay")).toString(); }
void Settings::setNetplayRelay(const QString& hostPort)
{
    store().setValue(QStringLiteral("netplay/relay"), hostPort.trimmed()); store().sync();
}

QString Settings::externalPlayer() { return store().value(QStringLiteral("player/external"), QStringLiteral("builtin")).toString(); }
void Settings::setExternalPlayer(const QString& id)
{
    store().setValue(QStringLiteral("player/external"), id.trimmed()); store().sync();
}
QString Settings::externalPlayerPath() { return store().value(QStringLiteral("player/externalPath")).toString(); }
void Settings::setExternalPlayerPath(const QString& path)
{
    store().setValue(QStringLiteral("player/externalPath"), path); store().sync();
}

bool Settings::startFullscreen() { return store().value(QStringLiteral("general/startFullscreen"), false).toBool(); }
void Settings::setStartFullscreen(bool on)
{
    store().setValue(QStringLiteral("general/startFullscreen"), on); store().sync();
}
QString Settings::displayMode() { return store().value(QStringLiteral("display/mode"), QStringLiteral("auto")).toString(); }
void Settings::setDisplayMode(const QString& mode)
{
    store().setValue(QStringLiteral("display/mode"), mode); store().sync();
}
QString Settings::virtualPad() { return store().value(QStringLiteral("emu/virtualPad"), QStringLiteral("auto")).toString(); }
void Settings::setVirtualPad(const QString& mode)
{
    store().setValue(QStringLiteral("emu/virtualPad"), mode); store().sync();
}
bool Settings::virtualPadEnabled()
{
    const QString v = virtualPad();
    if (v == QStringLiteral("on"))  return true;
    if (v == QStringLiteral("off")) return false;
    // "auto": on for the touch (Mobile) form factor. Consult the FormFactor authority (the RESOLVED mode),
    // not the raw display/mode string — so a mobile device under stored "auto" (Phase 2 resolveAuto()->Mobile)
    // gets the pad too. This is the ONE visibility resolver; RetroView::virtualPadShouldShow() delegates here.
    return FormFactor::instance().mode() == FormFactor::Mode::Mobile;
}
int Settings::virtualPadOpacity()
{
    return qBound(0, store().value(QStringLiteral("emu/virtualPadOpacity"), 45).toInt(), 100);
}
void Settings::setVirtualPadOpacity(int pct)
{
    store().setValue(QStringLiteral("emu/virtualPadOpacity"), qBound(0, pct, 100)); store().sync();
}
bool Settings::tvPromptDone() { return store().value(QStringLiteral("display/tvPromptDone"), false).toBool(); }
void Settings::setTvPromptDone(bool done)
{
    store().setValue(QStringLiteral("display/tvPromptDone"), done); store().sync();
}

bool Settings::onboardingDone() { return store().value(QStringLiteral("onboarding/done"), false).toBool(); }
void Settings::setOnboardingDone(bool done)
{
    store().setValue(QStringLiteral("onboarding/done"), done); store().sync();
}

bool Settings::checkUpdatesOnStartup() { return store().value(QStringLiteral("general/checkUpdatesOnStartup"), true).toBool(); }
void Settings::setCheckUpdatesOnStartup(bool on)
{
    store().setValue(QStringLiteral("general/checkUpdatesOnStartup"), on); store().sync();
}
bool Settings::uiTestChannel() { return store().value(QStringLiteral("debug/uiTestChannel"), false).toBool(); }
void Settings::setUiTestChannel(bool on)
{
    store().setValue(QStringLiteral("debug/uiTestChannel"), on); store().sync();
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

QString Settings::libraryFolder()
{
    const QString p = store().value(QStringLiteral("library/folder")).toString();
    return p.isEmpty() ? (AppPaths::dataDir() + QStringLiteral("/library")) : p;
}
void Settings::setLibraryFolder(const QString& path)
{
    store().setValue(QStringLiteral("library/folder"), path); store().sync();
}

bool Settings::resolveOnline() { return store().value(QStringLiteral("library/resolveOnline"), true).toBool(); }
void Settings::setResolveOnline(bool on)
{
    store().setValue(QStringLiteral("library/resolveOnline"), on); store().sync();
}

bool Settings::bgmEnabled() { return store().value(QStringLiteral("bgm/enabled"), true).toBool(); }
void Settings::setBgmEnabled(bool on) { store().setValue(QStringLiteral("bgm/enabled"), on); store().sync(); }
int  Settings::bgmVolume() { return store().value(QStringLiteral("bgm/volume"), 35).toInt(); }
void Settings::setBgmVolume(int pct)
{
    store().setValue(QStringLiteral("bgm/volume"), qBound(0, pct, 100)); store().sync();
}

// Attract mode (issue #54). OFF by default: a screensaver that starts on its own is a surprise, so the user
// opts in. The timeout is stored in whole minutes (what the setting shows) and clamped to 1..120 on write so
// a hand-edited ini can neither disable it by setting 0 nor pin it to something absurd.
bool Settings::attractEnabled() { return store().value(QStringLiteral("attract/enabled"), false).toBool(); }
void Settings::setAttractEnabled(bool on) { store().setValue(QStringLiteral("attract/enabled"), on); store().sync(); }
int  Settings::attractTimeoutMinutes() { return store().value(QStringLiteral("attract/timeoutMin"), 10).toInt(); }
void Settings::setAttractTimeoutMinutes(int minutes)
{
    store().setValue(QStringLiteral("attract/timeoutMin"), qBound(1, minutes, 120)); store().sync();
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

bool Settings::keepScrapedData() { return store().value(QStringLiteral("scrape/keepData"), true).toBool(); }
void Settings::setKeepScrapedData(bool on) { store().setValue(QStringLiteral("scrape/keepData"), on); store().sync(); }

bool Settings::autoApplyRomPatches() { return store().value(QStringLiteral("roms/autoApplyPatches"), true).toBool(); }
void Settings::setAutoApplyRomPatches(bool on) { store().setValue(QStringLiteral("roms/autoApplyPatches"), on); store().sync(); }

bool Settings::verifyRoms() { return store().value(QStringLiteral("roms/verifyDats"), true).toBool(); }
void Settings::setVerifyRoms(bool on) { store().setValue(QStringLiteral("roms/verifyDats"), on); store().sync(); }

// Under "device/" on purpose — CloudSync's carve-out makes that whole prefix device-local, and a synced
// "already swept" flag would tell every OTHER device that its own loose save files had been dealt with.
bool Settings::savesStraysSwept() { return store().value(QStringLiteral("device/savesStraysSwept"), false).toBool(); }
void Settings::setSavesStraysSwept(bool done)
{
    store().setValue(QStringLiteral("device/savesStraysSwept"), done); store().sync();
}

bool Settings::bezelEnabled() { return store().value(QStringLiteral("emu/bezel"), false).toBool(); }
void Settings::setBezelEnabled(bool on) { store().setValue(QStringLiteral("emu/bezel"), on); store().sync(); }

QString Settings::inputScope() { return store().value(QStringLiteral("input/scope")).toString(); }
void Settings::setInputScope(const QString& systemId)
{
    store().setValue(QStringLiteral("input/scope"), systemId); store().sync();
}

// Bindings are scope-aware: a non-empty input scope (a system id) reads/writes a per-system override that
// falls back to the global binding, which falls back to the hard-coded default. This lets each console keep
// its own control layout while games with no override use the global one.
int Settings::padBinding(int port, int retroId, int defaultCode)
{
    const QString base = QStringLiteral("pad/%1/%2").arg(port).arg(retroId);
    const QString sc = inputScope();
    if (!sc.isEmpty())
    {
        const QString sk = QStringLiteral("padscope/%1/%2/%3").arg(sc).arg(port).arg(retroId);
        if (store().contains(sk)) return store().value(sk).toInt();
    }
    return store().value(base, defaultCode).toInt();
}

void Settings::setPadBinding(int port, int retroId, int code)
{
    const QString sc = inputScope();
    const QString key = sc.isEmpty() ? QStringLiteral("pad/%1/%2").arg(port).arg(retroId)
                                      : QStringLiteral("padscope/%1/%2/%3").arg(sc).arg(port).arg(retroId);
    store().setValue(key, code);
    store().sync();
}

int Settings::keyBinding(int port, int retroId, int defaultKey)
{
    const QString base = QStringLiteral("kbd/%1/%2").arg(port).arg(retroId);
    const QString sc = inputScope();
    if (!sc.isEmpty())
    {
        const QString sk = QStringLiteral("kbdscope/%1/%2/%3").arg(sc).arg(port).arg(retroId);
        if (store().contains(sk)) return store().value(sk).toInt();
    }
    return store().value(base, defaultKey).toInt();
}

void Settings::setKeyBinding(int port, int retroId, int qtKey)
{
    const QString sc = inputScope();
    const QString key = sc.isEmpty() ? QStringLiteral("kbd/%1/%2").arg(port).arg(retroId)
                                      : QStringLiteral("kbdscope/%1/%2/%3").arg(sc).arg(port).arg(retroId);
    store().setValue(key, qtKey);
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
