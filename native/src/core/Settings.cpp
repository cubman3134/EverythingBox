#include "Settings.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "../theme2/FormFactor.h"   // virtualPadEnabled() resolves "auto" against the form-factor authority
#include "../video/RefreshSync.h"   // videoRefreshSync() default maps the resolved form factor (issue #70)
#include "ShaderPreset.h"           // shaderPreset() seeds its global default from the legacy filter (issue #99)
#include "LanguageCodes.h"          // preferredLanguage() canonicalizes + migrates from the legacy 3-letter key
#include "Scrobble.h"               // the scrobble keys (#192) are built off the prefix the carve-out excludes
#include <QSettings>
#include <QCoreApplication>
#include <QRegularExpression>
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

QString Settings::preferredLanguage()
{
    return LanguageCodes::readPreferred(store());
}

void Settings::setPreferredLanguage(const QString& code)
{
    store().setValue(QStringLiteral("content/language"), LanguageCodes::toCanonical(code));
    store().sync();
}

// Back-compat: the old subtitle-language accessors now read/write the unified content language.
QString Settings::subtitleLanguage() { return preferredLanguage(); }
void Settings::setSubtitleLanguage(const QString& code) { setPreferredLanguage(code); }

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

// Reader typography (issue #135). Each accessor is the ordinary read-default / write-and-sync pair; the setters
// clamp to the SAME bounds ReaderTypography's clamps enforce, so a value that reaches disk is already in range
// and the reader never has to re-clamp on read. readerTypography() assembles them into the pure Settings the
// reader and probe_readertypography share. Size reuses "ebook/fontSize" — the one notion of reading size the
// A+/A− stepper already writes — so the settings surface and the in-reader stepper can never disagree.
ReaderTypography::Settings Settings::readerTypography()
{
    ReaderTypography::Settings s;
    s.fontFamily     = readerFont();
    s.sizePt         = readerFontSize();
    s.lineSpacingPct = readerLineSpacing();
    s.marginPct      = readerMargin();
    s.justify        = readerJustify();
    s.theme          = readerTheme();
    return s;
}

QString Settings::readerFont() { return store().value(QStringLiteral("reader/font")).toString(); }
void Settings::setReaderFont(const QString& family)
{
    store().setValue(QStringLiteral("reader/font"), family); store().sync();
}

int Settings::readerFontSize()
{
    return ReaderTypography::clampSize(store().value(QStringLiteral("ebook/fontSize"), 14).toInt());
}
void Settings::setReaderFontSize(int pt)
{
    store().setValue(QStringLiteral("ebook/fontSize"), ReaderTypography::clampSize(pt)); store().sync();
}

int Settings::readerLineSpacing()
{
    return ReaderTypography::clampSpacing(store().value(QStringLiteral("reader/lineSpacing"), 100).toInt());
}
void Settings::setReaderLineSpacing(int pct)
{
    store().setValue(QStringLiteral("reader/lineSpacing"), ReaderTypography::clampSpacing(pct)); store().sync();
}

int Settings::readerMargin()
{
    return ReaderTypography::clampMargin(store().value(QStringLiteral("reader/margin"), 6).toInt());
}
void Settings::setReaderMargin(int pct)
{
    store().setValue(QStringLiteral("reader/margin"), ReaderTypography::clampMargin(pct)); store().sync();
}

bool Settings::readerJustify() { return store().value(QStringLiteral("reader/justify"), false).toBool(); }
void Settings::setReaderJustify(bool on)
{
    store().setValue(QStringLiteral("reader/justify"), on); store().sync();
}

ReaderTypography::Theme Settings::readerTheme()
{
    return ReaderTypography::themeFromInt(store().value(QStringLiteral("reader/theme"), 0).toInt());
}
void Settings::setReaderTheme(ReaderTypography::Theme t)
{
    store().setValue(QStringLiteral("reader/theme"), ReaderTypography::themeToInt(t)); store().sync();
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

// Gapless audio (issue #141). DEFAULT ON, because the gap is a defect rather than a preference: a live album,
// a DJ mix or a segued record is written to run continuously, and the stop-start-per-track path cuts it
// mid-phrase at exactly the moment the artist made seamless. Shipping that off meant nobody heard the fix
// unless they went looking for a setting to explain a problem they could not name.
//
// Safe as a default because of WHICH mode it applies: MpvWidget sets `gapless-audio=weak`, which joins two
// tracks only when their formats already match and falls back to the ordinary path when they do not. On a
// library of mixed formats it changes nothing; on the albums this exists for, it is the whole point. An
// absent key now reads true, so an existing install gets it without touching its settings — and the toggle
// is still there for anyone who wants the gap back.
bool Settings::gaplessAudio() { return store().value(QStringLiteral("playback/gaplessAudio"), true).toBool(); }
void Settings::setGaplessAudio(bool on)
{
    store().setValue(QStringLiteral("playback/gaplessAudio"), on); store().sync();
}

// ReplayGain (issue #141). DEFAULT ALBUM — an absent key resolves through ReplayGain::modeFromId, whose
// unknown-id fallback IS the default, so a missing, empty, hand-mangled or newer-build value all land on the
// shipped behaviour rather than silently on Off. Stored as the id string (not an int) so the ini stays
// readable and an enum reorder cannot repoint existing installs at a different mode.
ReplayGain::Mode Settings::replayGainMode()
{
    return ReplayGain::modeFromId(store().value(QStringLiteral("playback/replayGain")).toString());
}
void Settings::setReplayGainMode(ReplayGain::Mode mode)
{
    store().setValue(QStringLiteral("playback/replayGain"), ReplayGain::idForMode(mode)); store().sync();
}

// Clamp on both read and write (house style, cf. defaultPlaybackSpeed): a value written by an older build, a
// hand-edited ini or corruption is still inside the ±15 dB band, and an absent/non-numeric value reads back as
// 0 dB — the tagged gain and nothing more.
double Settings::replayGainPreamp()
{
    return ReplayGain::clampPreamp(store().value(QStringLiteral("playback/replayGainPreamp"),
                                                 ReplayGain::defaultPreampDb()).toDouble());
}
void Settings::setReplayGainPreamp(double db)
{
    store().setValue(QStringLiteral("playback/replayGainPreamp"), ReplayGain::clampPreamp(db)); store().sync();
}

// Crossfade (issue #141). DEFAULT OFF: an absent key reads 0, so every install that does not ask for it keeps
// the untouched boundary it has today. Clamped on both read AND write (house style, cf. replayGainPreamp), so
// an older build's value, a hand-edited ini or a non-numeric string still lands inside the 1-12 s band — and
// note which way clampSeconds rounds a stray sub-minimum value: UP to 1 s, never silently down to off, so a
// surface that shows the feature as on can never be lying about it.
int Settings::crossfadeSeconds()
{
    return Crossfade::clampSeconds(store().value(QStringLiteral("playback/crossfadeSeconds"),
                                                 Crossfade::defaultSeconds()).toInt());
}
void Settings::setCrossfadeSeconds(int seconds)
{
    store().setValue(QStringLiteral("playback/crossfadeSeconds"), Crossfade::clampSeconds(seconds)); store().sync();
}

// Online lyric lookup (issue #142). DEFAULT ON — the opposite default from gapless above, and for the reason
// ReplayGain is also opt-out: this only ever acts where the user already has nothing. A track with an .lrc
// sidecar or embedded words never reaches it, the lookup is once per track on play and cached forever after,
// and the service needs no key and no account. An absent key therefore reads TRUE, so an existing install
// gets the feature without going looking for a switch.
bool Settings::onlineLyrics() { return store().value(QStringLiteral("playback/onlineLyrics"), true).toBool(); }
void Settings::setOnlineLyrics(bool on)
{
    store().setValue(QStringLiteral("playback/onlineLyrics"), on); store().sync();
}

// The classic player page's lyric panel (issue #142). Remembered so a listener who wants the words gets them
// on the next track too, and defaulted OFF so nobody else grows a third pane in the player splitter.
bool Settings::lyricsPanel() { return store().value(QStringLiteral("playback/lyricsPanel"), false).toBool(); }
void Settings::setLyricsPanel(bool on)
{
    store().setValue(QStringLiteral("playback/lyricsPanel"), on); store().sync();
}

// Clamp on both read and write (house style, cf. virtualPadOpacity): a value written by an older build, a
// hand-edited ini, or corruption is still bounded, and an absent/non-numeric value reads back as 1.0.
double Settings::defaultPlaybackSpeed()
{
    const double v = store().value(QStringLiteral("playback/defaultSpeed"), 1.0).toDouble();
    return qBound(0.5, v > 0.0 ? v : 1.0, 3.5);
}
void Settings::setDefaultPlaybackSpeed(double rate)
{
    store().setValue(QStringLiteral("playback/defaultSpeed"), qBound(0.5, rate, 3.5)); store().sync();
}

// Audio jump interval (issue #140). Clamp on both read and write (house style, cf. defaultPlaybackSpeed): a
// value written by an older build, a hand-edited ini or corruption is still bounded, and an absent/non-numeric
// value reads back as the 30 s default.
int Settings::audioJumpSeconds()
{
    const int v = store().value(QStringLiteral("playback/jumpSeconds"), 30).toInt();
    return qBound(5, v > 0 ? v : 30, 120);
}
void Settings::setAudioJumpSeconds(int seconds)
{
    store().setValue(QStringLiteral("playback/jumpSeconds"), qBound(5, seconds, 120)); store().sync();
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

// ---- Music scrobbling (issue #192) ------------------------------------------------------------------------
// Per profile, through ONE key builder, so the four keys can never end up in different profiles' groups — and
// built off Scrobble::settingsKeyPrefix() so CloudSync's device-local carve-out is written in terms of the
// same string this writer uses rather than a literal that can drift from it.
//
// The active profile is read STRAIGHT OUT OF THE STORE rather than through ProfileStore::currentId(), which is
// the one-line function that does exactly this. Not a duplication for its own sake: Settings.cpp is linked by
// two dozen headless probes that have no reason to know what a profile is, and calling into ProfileStore here
// makes every one of them fail to link — measured, not theorised (probe_navqml went red on this exact symbol).
// The key it reads is the same literal ProfileStore::setCurrent writes, and it is the ONE place in this file
// that needs it.
static QString scrobbleKey(const QString& leaf)
{
    const QString profileId = store().value(QStringLiteral("profiles/current")).toString();
    return Scrobble::settingsKeyPrefix() + Scrobble::profileSlot(profileId)
         + QStringLiteral("/") + leaf;
}

// OFF by default, and deliberately so: this sends what somebody listens to, by name, to a third party. A
// feature with that shape is opted INTO.
bool Settings::scrobbleEnabled() { return store().value(scrobbleKey(QStringLiteral("enabled")), false).toBool(); }
void Settings::setScrobbleEnabled(bool on)
{ store().setValue(scrobbleKey(QStringLiteral("enabled")), on); store().sync(); }

bool Settings::scrobbleSpokenAudio()
{ return store().value(scrobbleKey(QStringLiteral("spoken")), false).toBool(); }
void Settings::setScrobbleSpokenAudio(bool on)
{ store().setValue(scrobbleKey(QStringLiteral("spoken")), on); store().sync(); }

// THE USER'S OWN SECRET. Read by exactly one caller (ListenBrainzClient, at the moment it builds a request)
// and written by exactly one (the two settings builders). Nothing between logs it.
QString Settings::listenBrainzToken()
{ return store().value(scrobbleKey(QStringLiteral("lb/token"))).toString(); }
void Settings::setListenBrainzToken(const QString& token)
{ store().setValue(scrobbleKey(QStringLiteral("lb/token")), token.trimmed()); store().sync(); }

QString Settings::listenBrainzApiUrl()
{ return store().value(scrobbleKey(QStringLiteral("lb/url"))).toString(); }
void Settings::setListenBrainzApiUrl(const QString& url)
{ store().setValue(scrobbleKey(QStringLiteral("lb/url")), url.trimmed()); store().sync(); }

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

QString Settings::shaderPreset()
{
    // Seed from the legacy video filter on FIRST read (#99): until the user picks a shader preset explicitly,
    // the global default mirrors their existing Scanlines/CRT/LCD choice so nothing appears to reset on upgrade.
    // Once written, the stored value wins.
    if (!store().contains(QStringLiteral("emu/shaderPreset")))
        return ShaderPreset::presetIdForLegacyFilter(videoFilter());
    return store().value(QStringLiteral("emu/shaderPreset")).toString();
}
void Settings::setShaderPreset(const QString& id)
{
    store().setValue(QStringLiteral("emu/shaderPreset"), id); store().sync();
}

QString Settings::hwDecode() { return store().value(QStringLiteral("video/hwdec"), QStringLiteral("auto")).toString(); }
void Settings::setHwDecode(const QString& mode)
{
    store().setValue(QStringLiteral("video/hwdec"), mode.trimmed()); store().sync();
}

bool Settings::videoRefreshSync()
{
    const QString sk = QStringLiteral("video/refreshSync");
    if (store().contains(sk)) return store().value(sk).toBool();
    // Absent: the form-factor-dependent default. Resolve against the RESOLVED form-factor mode (the ONE
    // authority — "auto" is already collapsed to a concrete mode here), same pattern as virtualPadEnabled().
    RefreshSync::FormFactor ff;
    switch (FormFactor::instance().mode())
    {
        case FormFactor::Mode::Tv:     ff = RefreshSync::FormFactor::Tv;     break;
        case FormFactor::Mode::Mobile: ff = RefreshSync::FormFactor::Mobile; break;
        default:                       ff = RefreshSync::FormFactor::Desktop; break;
    }
    return RefreshSync::defaultEnabled(ff);
}
void Settings::setVideoRefreshSync(bool on)
{
    store().setValue(QStringLiteral("video/refreshSync"), on); store().sync();
}

// HDR output (issue #68). Stored as the mode id string under "video/hdr"; an absent value resolves to the
// ToneMapSdr default (the common washed-out-on-SDR fix), and any unknown/hand-edited value degrades to it too —
// HdrOutput::modeFromId owns that mapping so Settings, the two builders and probe_hdroutput agree on spelling.
HdrOutput::Mode Settings::hdrOutput()
{
    return HdrOutput::modeFromId(store().value(QStringLiteral("video/hdr"),
                                               HdrOutput::defaultModeId()).toString());
}
void Settings::setHdrOutput(const QString& modeId)
{
    store().setValue(QStringLiteral("video/hdr"), modeId.trimmed()); store().sync();
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

bool Settings::remoteControlEnabled() { return store().value(QStringLiteral("remote/enabled"), false).toBool(); }
void Settings::setRemoteControlEnabled(bool on)
{
    store().setValue(QStringLiteral("remote/enabled"), on); store().sync();
}
int Settings::remoteControlPort() { return store().value(QStringLiteral("remote/port"), 8090).toInt(); }
void Settings::setRemoteControlPort(int port)
{
    if (port < 1) port = 1; else if (port > 65535) port = 65535;   // clamp to a valid TCP port
    store().setValue(QStringLiteral("remote/port"), port); store().sync();
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

QString Settings::musicFolder()
{
    const QString p = store().value(QStringLiteral("music/folder")).toString();
    return p.isEmpty() ? (AppPaths::dataDir() + QStringLiteral("/musiclibrary")) : p;
}
void Settings::setMusicFolder(const QString& path)
{
    store().setValue(QStringLiteral("music/folder"), path); store().sync();
}

// The AUDIOBOOK root (issue #139). Same shape as musicFolder above and deliberately a DIFFERENT key: the
// two libraries classify the same file types, and which library a file belongs to is answered by which of
// these two folders it is under and by nothing else. See Settings.h.
QString Settings::audiobookFolder()
{
    const QString p = store().value(QStringLiteral("audiobooks/folder")).toString();
    return p.isEmpty() ? (AppPaths::dataDir() + QStringLiteral("/audiobooks")) : p;
}
void Settings::setAudiobookFolder(const QString& path)
{
    store().setValue(QStringLiteral("audiobooks/folder"), path); store().sync();
}

// The READING root (issue #134) — books and comics together. Same shape again, its own key again: the same
// .pdf is a book under this folder and a document nobody has classified anywhere else. See Settings.h for
// why books and comics share one root while music and audiobooks cannot.
QString Settings::readingFolder()
{
    const QString p = store().value(QStringLiteral("reading/folder")).toString();
    return p.isEmpty() ? (AppPaths::dataDir() + QStringLiteral("/books")) : p;
}
void Settings::setReadingFolder(const QString& path)
{
    store().setValue(QStringLiteral("reading/folder"), path); store().sync();
}

// The one place the ad-hoc separator DEFAULT is decided (issue #196). AudioTags holds the splitting rule and
// no policy — see Settings.h for why this list is a single semicolon and what it costs to get it wrong.
// contains() rather than a default argument: an empty stored value is "split nothing", which a defaulted
// read could not tell from "never configured".
QString Settings::musicTagSeparators()
{
    const QString key = QStringLiteral("music/tagSeparators");
    return store().contains(key) ? store().value(key).toString() : QStringLiteral(";");
}
void Settings::setMusicTagSeparators(const QString& list)
{
    store().setValue(QStringLiteral("music/tagSeparators"), list.trimmed()); store().sync();
}
QStringList Settings::musicTagSeparatorList()
{
    // Whitespace-separated, so a separator can never itself be whitespace — which is what stops a stray
    // space in the field from splitting every two-word band name in the library.
    return musicTagSeparators().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

QString Settings::musicPreferredSource()
{
    const QString v = store().value(QStringLiteral("music/preferredSource")).toString().trimmed();
    return v.isEmpty() ? QStringLiteral("local") : v;
}
void Settings::setMusicPreferredSource(const QString& v)
{
    store().setValue(QStringLiteral("music/preferredSource"), v.trimmed()); store().sync();
}

QString Settings::photosFolder()
{
    const QString p = store().value(QStringLiteral("photos/folder")).toString();
    return p.isEmpty() ? (AppPaths::dataDir() + QStringLiteral("/photos")) : p;
}
void Settings::setPhotosFolder(const QString& path)
{
    store().setValue(QStringLiteral("photos/folder"), path); store().sync();
}

bool Settings::resolveOnline() { return store().value(QStringLiteral("library/resolveOnline"), true).toBool(); }
void Settings::setResolveOnline(bool on)
{
    store().setValue(QStringLiteral("library/resolveOnline"), on); store().sync();
}

bool Settings::collapseRegionalDuplicates()
{
    return store().value(QStringLiteral("library/collapseRegions"), false).toBool();
}
void Settings::setCollapseRegionalDuplicates(bool on)
{
    store().setValue(QStringLiteral("library/collapseRegions"), on); store().sync();
}

bool Settings::bgmEnabled() { return store().value(QStringLiteral("bgm/enabled"), true).toBool(); }
void Settings::setBgmEnabled(bool on) { store().setValue(QStringLiteral("bgm/enabled"), on); store().sync(); }
int  Settings::bgmVolume() { return store().value(QStringLiteral("bgm/volume"), 35).toInt(); }
void Settings::setBgmVolume(int pct)
{
    store().setValue(QStringLiteral("bgm/volume"), qBound(0, pct, 100)); store().sync();
}

// Video hover previews (issue #55). previewsEnabled defaults to TRUE: the previews are the intended
// browse experience, so an untouched profile gets them. snapVolume defaults to 0 (MUTED): the snap plays
// silently out of the box and the C++ duck never pauses the background music at volume 0 — the user opts
// into audible previews. The volume is clamped to 0..100 on write so a hand-edited ini can't drive mpv's
// volume property out of range.
bool Settings::videoPreviewsEnabled()
{
    return store().value(QStringLiteral("video/previewsEnabled"), true).toBool();
}
void Settings::setVideoPreviewsEnabled(bool on)
{
    store().setValue(QStringLiteral("video/previewsEnabled"), on); store().sync();
}
int  Settings::videoSnapVolume() { return store().value(QStringLiteral("video/snapVolume"), 0).toInt(); }
void Settings::setVideoSnapVolume(int pct)
{
    store().setValue(QStringLiteral("video/snapVolume"), qBound(0, pct, 100)); store().sync();
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

// Per-system standalone-emulator default (Unified Emulation Picker Task 2), a byte-for-byte mirror of coreFor:
// same store, same empty-is-inherit posture. Keyed "emulators/<systemId>" — a per-system PREFERENCE that rides
// the synced settings bundle exactly like "cores/<id>" and "backends/<id>" (no CloudSync carve-out). The
// device-local "emulators/root" / "emulators/fullscreen" keys are matched as EXACT leaves, so a systemId never
// collides with them.
QString Settings::emulatorFor(const QString& systemId)
{
    return store().value(QStringLiteral("emulators/") + systemId).toString();
}

void Settings::setEmulatorFor(const QString& systemId, const QString& emulatorId)
{
    store().setValue(QStringLiteral("emulators/") + systemId, emulatorId);
    store().sync();
}

// Per-system emulation backend (RetroPark Slice 2a), mirroring coreFor but resolving through a global default
// instead of a catalog default. defaultBackend() is Libretro until set, so an app with no backend settings at
// all launches every system on libretro — today's behaviour.
EmuBackend Settings::defaultBackend()
{
    const QString s = store().value(QStringLiteral("backends/_default")).toString();
    if (s.isEmpty()) return EmuBackend::Libretro;
    return backendFromString(s);   // unknown/retired spelling -> Libretro
}

void Settings::setDefaultBackend(EmuBackend backend)
{
    store().setValue(QStringLiteral("backends/_default"), backendToString(backend));
    store().sync();
}

EmuBackend Settings::backendFor(const QString& systemId)
{
    const QString s = store().value(QStringLiteral("backends/") + systemId).toString();
    if (s.isEmpty()) return defaultBackend();   // no per-system choice -> the global default
    return backendFromString(s);                // unknown/retired spelling -> Libretro
}

void Settings::setBackendFor(const QString& systemId, EmuBackend backend)
{
    store().setValue(QStringLiteral("backends/") + systemId, backendToString(backend));
    store().sync();
}

// RetroPark DRIVEN-core host graphics API. Only "opengl" opts onto the OpenGL compositor; every other stored
// value (unset, "d3d11", or a hand-edited/unknown spelling) resolves to the proven D3D11 default, so a driven
// launch is byte-identical to today until the user deliberately picks OpenGL.
QString Settings::retroParkDrivenBackend()
{
    return store().value(QStringLiteral("retropark/driven_backend"),
                         QStringLiteral("d3d11")).toString().trimmed() == QStringLiteral("opengl")
               ? QStringLiteral("opengl") : QStringLiteral("d3d11");
}

void Settings::setRetroParkDrivenBackend(const QString& id)
{
    store().setValue(QStringLiteral("retropark/driven_backend"),
                     id.trimmed() == QStringLiteral("opengl") ? QStringLiteral("opengl") : QStringLiteral("d3d11"));
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

// Keyed "optdesc/<core>": the raw core-options JSON, cached after the first successful launch so the global
// options editor can show a late-declaring core's options before the next launch. Namespaced by <core> like
// opt/<core>/* above; a single value per core (not a per-key subtree), so no key can collide with an option.
QString Settings::coreOptionDescriptors(const QString& core)
{
    return store().value(QStringLiteral("optdesc/") + core).toString();
}

void Settings::setCoreOptionDescriptors(const QString& core, const QString& json)
{
    store().setValue(QStringLiteral("optdesc/") + core, json);
    store().sync();
}

bool Settings::keepScrapedData() { return store().value(QStringLiteral("scrape/keepData"), true).toBool(); }
void Settings::setKeepScrapedData(bool on) { store().setValue(QStringLiteral("scrape/keepData"), on); store().sync(); }

bool Settings::keepDownloadsInRoms() { return store().value(QStringLiteral("roms/keepDownloads"), true).toBool(); }
void Settings::setKeepDownloadsInRoms(bool on) { store().setValue(QStringLiteral("roms/keepDownloads"), on); store().sync(); }

bool Settings::autoApplyRomPatches() { return store().value(QStringLiteral("roms/autoApplyPatches"), true).toBool(); }
void Settings::setAutoApplyRomPatches(bool on) { store().setValue(QStringLiteral("roms/autoApplyPatches"), on); store().sync(); }

bool Settings::ps3AutoUpdate() { return store().value(QStringLiteral("ps3/autoUpdate"), true).toBool(); }
void Settings::setPs3AutoUpdate(bool on) { store().setValue(QStringLiteral("ps3/autoUpdate"), on); store().sync(); }

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

bool Settings::stateAutoIncrement() { return store().value(QStringLiteral("emu/stateAutoIncrement"), false).toBool(); }
void Settings::setStateAutoIncrement(bool on) { store().setValue(QStringLiteral("emu/stateAutoIncrement"), on); store().sync(); }

int Settings::resumeMode()
{
    // Clamp anything a hand-edited ini carries into the known enum, defaulting to Prompt.
    const int v = store().value(QStringLiteral("emu/resumeMode"), int(ResumePrompt)).toInt();
    return (v == ResumeOff || v == ResumePrompt || v == ResumeSilent) ? v : int(ResumePrompt);
}
void Settings::setResumeMode(int mode) { store().setValue(QStringLiteral("emu/resumeMode"), mode); store().sync(); }

bool Settings::hardcoreAchievements() { return store().value(QStringLiteral("ra/hardcore"), false).toBool(); }
void Settings::setHardcoreAchievements(bool on) { store().setValue(QStringLiteral("ra/hardcore"), on); store().sync(); }

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
    // Precedence (issue #95): per-GAME layer -> per-SYSTEM scope -> global -> hard default. The game layer is
    // the highest, so the one quirky game's swapped buttons win over its console's profile, which wins over
    // the global map. Each layer is a delta: a level that has no key for this (port, button) falls through.
    const QString g = inputGameScope();
    if (!g.isEmpty())
    {
        const QString gk = QStringLiteral("padgame/%1/%2/%3").arg(g).arg(port).arg(retroId);
        if (store().contains(gk)) return store().value(gk).toInt();
    }
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
    // Same three-level precedence as padBinding: per-GAME -> per-SYSTEM -> global -> hard default.
    const QString g = inputGameScope();
    if (!g.isEmpty())
    {
        const QString gk = QStringLiteral("kbdgame/%1/%2/%3").arg(g).arg(port).arg(retroId);
        if (store().contains(gk)) return store().value(gk).toInt();
    }
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

// ---- Per-game overrides (issue #95) ------------------------------------------------------------------
// A game's stable identity (PlayStats::identity — its addon item id, else its path) is hashed to a compact,
// ini-safe leaf, exactly as PlayStats/ItemMarks hash their keys: the identity can be a URL or a Windows path
// and must never alias another game nor become a nested ini group by containing '/' or '\'.
QString Settings::gameToken(const QString& gameIdentity)
{
    if (gameIdentity.isEmpty()) return QString();
    return QString::fromLatin1(
        QCryptographicHash::hash(gameIdentity.toUtf8(), QCryptographicHash::Sha1).toHex());
}

// The active per-game INPUT layer, set at game launch and cleared at teardown (RetroView). "" = no game
// layer, which is the state whenever nothing is running — so a stale token can never leak a previous game's
// remap into the settings UI or the next launch.
QString Settings::inputGameScope() { return store().value(QStringLiteral("input/gameScope")).toString(); }
void Settings::setInputGameScope(const QString& gameToken)
{
    store().setValue(QStringLiteral("input/gameScope"), gameToken); store().sync();
}

// Explicit per-game binding writes/reset for the remap dialog's "This game" scope. They target the game
// keyspace directly (padgame/*, kbdgame/*) and NEVER the global pad/* or the per-system padscope/* — the
// no-leak rail for input, the twin of the core-option one below. `token` is a gameToken().
bool Settings::gamePadHasBinding(const QString& token, int port, int retroId)
{
    return !token.isEmpty()
        && store().contains(QStringLiteral("padgame/%1/%2/%3").arg(token).arg(port).arg(retroId));
}
void Settings::setGamePadBinding(const QString& token, int port, int retroId, int code)
{
    if (token.isEmpty()) return;
    store().setValue(QStringLiteral("padgame/%1/%2/%3").arg(token).arg(port).arg(retroId), code);
    store().sync();
}
void Settings::clearGamePadBinding(const QString& token, int port, int retroId)
{
    if (token.isEmpty()) return;
    store().remove(QStringLiteral("padgame/%1/%2/%3").arg(token).arg(port).arg(retroId));
    store().sync();
}
bool Settings::gameKeyHasBinding(const QString& token, int port, int retroId)
{
    return !token.isEmpty()
        && store().contains(QStringLiteral("kbdgame/%1/%2/%3").arg(token).arg(port).arg(retroId));
}
void Settings::setGameKeyBinding(const QString& token, int port, int retroId, int qtKey)
{
    if (token.isEmpty()) return;
    store().setValue(QStringLiteral("kbdgame/%1/%2/%3").arg(token).arg(port).arg(retroId), qtKey);
    store().sync();
}
void Settings::clearGameKeyBinding(const QString& token, int port, int retroId)
{
    if (token.isEmpty()) return;
    store().remove(QStringLiteral("kbdgame/%1/%2/%3").arg(token).arg(port).arg(retroId));
    store().sync();
}

// Per-game CORE-OPTION deltas. Keyed "optgame/<token>/<core>/<key>". These are a SEPARATE keyspace from the
// per-core baseline "opt/<core>/<key>" (optionValue/setOptionValue above), which these never write — so a
// game-scoped option cannot mutate the per-core value and cannot carry to the next game on that core (issue
// #95's #1 rail). Presence is the override; absence inherits the baseline. A reset REMOVES the key.
bool Settings::gameHasOption(const QString& token, const QString& core, const QString& key)
{
    return !token.isEmpty()
        && store().contains(QStringLiteral("optgame/%1/%2/%3").arg(token, core, key));
}
QString Settings::gameOptionValue(const QString& token, const QString& core, const QString& key)
{
    if (token.isEmpty()) return QString();
    return store().value(QStringLiteral("optgame/%1/%2/%3").arg(token, core, key)).toString();
}
void Settings::setGameOptionValue(const QString& token, const QString& core, const QString& key, const QString& value)
{
    if (token.isEmpty()) return;
    store().setValue(QStringLiteral("optgame/%1/%2/%3").arg(token, core, key), value);
    store().sync();
}
void Settings::clearGameOptionValue(const QString& token, const QString& core, const QString& key)
{
    if (token.isEmpty()) return;
    store().remove(QStringLiteral("optgame/%1/%2/%3").arg(token, core, key));
    store().sync();
}
// Every game-scoped core-option override for (token, core), as key -> value. This is the delta the launch
// applies on top of the per-core baseline, and the set the "modified for this game" markers read.
QMap<QString, QString> Settings::gameOptionDelta(const QString& token, const QString& core)
{
    QMap<QString, QString> out;
    if (token.isEmpty()) return out;
    const QString group = QStringLiteral("optgame/%1/%2").arg(token, core);
    store().beginGroup(group);
    const QStringList keys = store().childKeys();
    for (const QString& k : keys) out.insert(k, store().value(k).toString());
    store().endGroup();
    return out;
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
