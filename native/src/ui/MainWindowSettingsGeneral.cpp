// SETTINGS > GENERAL (issue #186, increment 1): MainWindow::openGeneralSettings(), in its own translation unit.
//
// WHY IT MOVED. MainWindow.cpp had reached ~30,000 lines: it is the build's single longest compile (a TU
// compiles on one core however many /MP gives the rest), it already needs /bigobj, and in September its
// General handler hit MSVC's nesting limit (C1061). This one function was ~5,000 lines of it -- BOTH
// settings builders' General page, the themed PanelRow list and the classic QWidget page -- so it is the
// first thing out.
//
// A PURE MOVE. The function below is byte-for-byte what it was in MainWindow.cpp; so are the file-statics
// above it, which nothing else in MainWindow.cpp used and so moved with it unchanged. The two helpers it
// shares with the rest of MainWindow.cpp (store(), panelRow()) are defined once in MainWindowInternal.h.
// Both builders still live in this one function, so the rule that a user-facing setting goes in BOTH of
// them (and in the GS_TWINS parity table) is unchanged -- only the file name is new.
#include "MainWindow.h"
#include "MainWindowInternal.h"   // store(), panelRow(): shared with MainWindow.cpp, defined once

#include "HomeView.h"
#include "RegistryBrowser.h"
#include "nav/Nav.h"
#include "nav/NavOverlay.h"
#include "nav/Osk.h"

#include "../core/Achievements.h"
#include "../core/AniListTracker.h"
#include "../core/AppPaths.h"
#include "../core/AppUpdater.h"
#include "../core/BackgroundMusic.h"
#include "../core/CatalogResolver.h"
#include "../core/CloudSync.h"
#include "../core/ExternalPlayer.h"
#include "../core/FollowPlan.h"
#include "../core/FollowScheduler.h"
#include "../core/HomeRows.h"
#include "../core/IptvSourceStore.h"
#include "../core/JellyfinDownload.h"
#include "../core/JellyfinServerStore.h"
#include "../core/JellyseerrStore.h"
#include "../core/KitsuTracker.h"
#include "../core/LastFmClient.h"
#include "../core/LaunchRecipe.h"
#include "../core/LocalLibrary.h"
#include "../core/MetaOverrides.h"
#include "../core/MusicId.h"
#include "../core/MusicLibrary.h"
#include "../core/MyAnimeListTracker.h"
#include "../core/PresenceController.h"
#include "../core/ProfileStore.h"
#include "../core/RemoteServer.h"
#include "../core/RomLibrary.h"
#include "../core/Scrobbler.h"
#include "../core/ServerMusicClient.h"
#include "../core/Settings.h"
#include "../core/ShaderPreset.h"
#include "../core/Subsonic.h"
#include "../core/SubsonicServerStore.h"
#include "../core/SubtitleFetcher.h"
#include "../core/Theme.h"
#include "../core/TraktClient.h"
#include "../input/InputMode.h"
#include "../theme2/FormFactor.h"
#include "../theme2/PanelModel.h"
#include "../theme2/VideoPreviewBridge.h"
#include "../video/MpvWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#ifdef EB_HAVE_QML
#include "../theme2/ThemeEngine.h"
#include "../theme2/ThemedPanelHost.h"
#endif

// The community server. Permanent, non-expiring invite — see the Discord design spec.
static constexpr const char* kDiscordInvite = "https://discord.gg/bW7KMVhgwH";

// The funding page. The app is free and whole either way; this row is a pointer, not a gate.
static constexpr const char* kPatreonUrl = "https://www.patreon.com/c/TheEverythingBox";

// WHICH COPY PLAYS when a record is on this disk AND on a music server (issue #194). ONE list, built here,
// consumed by BOTH settings builders and by the handler that writes the value back - so the two surfaces
// cannot offer different options, and nothing but a listed value is ever stored. The trailing rows are the
// configured servers, by name, which is what "prefer a specific server" means in practice.
static QList<QPair<QString, QString>> musicSourcePrefPairs()
{
    QList<QPair<QString, QString>> out;
    out << qMakePair(QObject::tr("My own files"),   QString::fromLatin1(MusicId::kPreferLocal));
    out << qMakePair(QObject::tr("A music server"), QString::fromLatin1(MusicId::kPreferServer));
    for (const SubsonicServer& srv : SubsonicServerStore::list())
        out << qMakePair(srv.name.trimmed().isEmpty() ? srv.url : srv.name, srv.id);
    // (#194 increment 3) The two new suppliers, in the SAME order MusicMerge feeds them to
    // pickAutoSource — servers as they were added, then the shelves as their sources load — so "the third
    // one down" means the same thing in this list and in the fallback the pick actually applies.
    //
    // ENABLED Jellyfin servers only: `enabled` means "get this library out of the way", and offering a
    // switched-off server as the one to play from would be offering a preference that can never be met.
    for (const JellyfinServer& srv : JellyfinServerStore::enabled())
        out << qMakePair(srv.name.trimmed().isEmpty() ? srv.url : srv.name, srv.id);
    for (const ServerMusicClient::Shelf& sh : ServerMusicClient::instance().shelves())
        out << qMakePair(sh.name.trimmed().isEmpty() ? sh.id : sh.name, sh.id);
    return out;
}

// SERVER STREAMING QUALITY (#193). ONE list, from the protocol's own choices, read by BOTH settings builders
// and by the themed handler that writes the value back — so the two surfaces cannot offer different options.
static QList<QPair<QString, int>> subsonicStreamQualityPairs()
{
    QList<QPair<QString, int>> out;
    for (int kbps : Subsonic::streamBitRateChoices())
        out << qMakePair(kbps == 0 ? QObject::tr("Original") : QObject::tr("%1 kbps").arg(kbps), kbps);
    return out;
}
static QString subsonicStreamQualityHint()
{
    return QObject::tr("The most a music server is asked to stream at, on this device only. Original plays the "
                       "file the server holds; a cap asks the server to convert on the fly, which saves data "
                       "on a slow or metered connection. Downloads are always the original file. This is not "
                       "switched automatically on mobile data.");
}

// The display string for what is stored right now. Falls back to the FIRST row rather than to nothing: an
// unrecognised stored value (a server id from another device - this key syncs, the servers do not) is read as
// "my own files" by MusicId::pickAutoSource too, so the row and the behaviour agree.
static QString musicSourcePrefDisplay()
{
    const QString cur = Settings::musicPreferredSource();
    const QList<QPair<QString, QString>> pairs = musicSourcePrefPairs();
    for (const auto& pr : pairs) if (pr.second == cur) return pr.first;
    return pairs.first().first;
}

// How many manual "these are / are not the same" verdicts the user has recorded (#194). Both kinds, because
// the row that resets them resets both and a count that named only one would understate what it is about.
static int musicMatchOverrideCount()
{
    return int(MusicId::albumOverrides().size()) + int(MusicId::artistOverrides().size());
}

static void clearMusicMatchOverrides()
{
    // The verdicts are walked as STORED KEYS and removed by key. Re-deriving them from a display name would
    // normalise twice and remove a pair nobody ever wrote - the exact silent no-op PcGameId.cpp documents.
    const QVector<MusicId::Verdict> albums  = MusicId::albumOverrides();
    const QVector<MusicId::Verdict> artists = MusicId::artistOverrides();
    for (const MusicId::Verdict& v : albums)  MusicId::clearAlbumOverrideKeys(v.a, v.b);
    for (const MusicId::Verdict& v : artists) MusicId::clearArtistOverrideKeys(v.a, v.b);
}

void MainWindow::openGeneralSettings()
{
#ifdef EB_HAVE_QML
    // Themed mode: render General as a flat PanelRow descriptor list on the Nav Contract (ThemedPanelHost),
    // instead of the classic QWidget builder. Every row writes the SAME Settings key via the SAME setter the
    // classic handler used. This is a NESTED present() (the hub is already up at level 1) — NO reset(), so Back
    // is a graph-level pop that renderTop(restore)s the hub to the row we entered from (the first live exercise
    // of Task 1's pop-restore path). Section headers -> Separator rows; the subtitle-language combo -> a Choice;
    // credentials + the ROMs path/actions -> TextField/Action rows; the parental PIN keeps Osk::getText nesting.
    if (themedHomeEnabled() && themedPanelHost_)
    {
        // Drop any Trakt live-status hookups from a previous presentation (the host persists — classic's child
        // labels auto-disconnected on teardown; we manage ours). Re-added after present() below.
        for (const QMetaObject::Connection& c : genSettingsConns_) disconnect(c);
        genSettingsConns_.clear();

        if (bgm_) bgm_->reload();                          // rescan music, exactly as the classic builder does
        themedPanelHost_->setStyle(settingsPanelStyle());  // active theme's settingsPanel block (hard fallbacks)

        // Preferred content language table (display <-> canonical 2-letter code). The Choice cycles the display
        // names; the handler maps the picked display back to its code via this same pair list (so a prior
        // "(custom)" code round-trips exact).
        const QList<QPair<QString, QString>> langs = {
            { tr("Any / no preference"), QString() }, { QStringLiteral("English"), QStringLiteral("en") },
            { QStringLiteral("Spanish"), QStringLiteral("es") }, { QStringLiteral("French"), QStringLiteral("fr") },
            { QStringLiteral("German"), QStringLiteral("de") }, { QStringLiteral("Italian"), QStringLiteral("it") },
            { QStringLiteral("Portuguese"), QStringLiteral("pt") }, { QStringLiteral("Dutch"), QStringLiteral("nl") },
            { QStringLiteral("Russian"), QStringLiteral("ru") }, { QStringLiteral("Japanese"), QStringLiteral("ja") },
            { QStringLiteral("Korean"), QStringLiteral("ko") }, { QStringLiteral("Chinese"), QStringLiteral("zh") },
            { QStringLiteral("Arabic"), QStringLiteral("ar") },
        };
        const QString curLang = Settings::preferredLanguage();
        QList<QPair<QString, QString>> langOptPairs = langs;   // captured by the handler for display->code mapping
        QString curLangDisp;
        for (const auto& l : langs) if (l.second == curLang) { curLangDisp = l.first; break; }
        if (curLangDisp.isEmpty() && !curLang.isEmpty()) {     // keep a previously-set code the list doesn't carry
            curLangDisp = tr("%1 (custom)").arg(curLang);
            langOptPairs << qMakePair(curLangDisp, curLang);
        }
        if (curLangDisp.isEmpty()) curLangDisp = langs.first().first;
        QStringList langOpts;
        for (const auto& p : langOptPairs) langOpts << p.first;

        // Background-music volume: the contract has no Slider row, so the continuous 0..100 slider becomes a
        // discrete Choice in 10% steps (documented gap — see report). Same write path (setBgmVolume/setVolume).
        QStringList volOpts;
        for (int p = 0; p <= 100; p += 10) volOpts << QStringLiteral("%1%").arg(p);
        const QString curVolDisp = QStringLiteral("%1%").arg(int(qRound(Settings::bgmVolume() / 10.0) * 10));
        // Video-preview snap volume: same discrete-Choice treatment as the BGM volume (no Slider row in the
        // themed contract). 0% == muted, the default. Same write path (setVideoSnapVolume) as the classic slider.
        const QString curSnapVolDisp = QStringLiteral("%1%").arg(int(qRound(Settings::videoSnapVolume() / 10.0) * 10));

        // External-player choice: Built-in + each detected desktop player (VLC/MPC) + Custom. (Android build:
        // Built-in + "Ask another app…".) The Choice delivers the display string; this pair list maps it back
        // to the stored key so nothing but builtin/vlc/mpc/custom is ever written on desktop — the "android"
        // key never appears in a desktop options list, so a desktop ini can't acquire it here.
        QList<QPair<QString, QString>> playerOptPairs;
        playerOptPairs << qMakePair(tr("Built-in player"), QStringLiteral("builtin"));
#ifdef Q_OS_ANDROID
        playerOptPairs << qMakePair(tr("Ask another app…"), QStringLiteral("android"));
#else
        for (const ExternalPlayer::Detected& d : ExternalPlayer::detect())
        {
            if      (d.kind == ExternalPlayer::Kind::Vlc) playerOptPairs << qMakePair(d.display, QStringLiteral("vlc"));
            else if (d.kind == ExternalPlayer::Kind::Mpc) playerOptPairs << qMakePair(d.display, QStringLiteral("mpc"));
        }
        playerOptPairs << qMakePair(tr("Custom…"), QStringLiteral("custom"));
#endif
        const QString curPlayerKey = Settings::externalPlayer();
        QString curPlayerDisp;
        for (const auto& p : playerOptPairs) if (p.second == curPlayerKey) { curPlayerDisp = p.first; break; }
        // A configured kind whose player isn't currently detected (e.g. VLC uninstalled) — keep it shown so the
        // setting is visible and changeable rather than silently snapping to Built-in.
        if (curPlayerDisp.isEmpty())
        {
            if (curPlayerKey == QStringLiteral("vlc"))      curPlayerDisp = tr("VLC media player");
            else if (curPlayerKey == QStringLiteral("mpc")) curPlayerDisp = tr("MPC-HC");
            if (!curPlayerDisp.isEmpty()) playerOptPairs << qMakePair(curPlayerDisp, curPlayerKey);
            else curPlayerDisp = playerOptPairs.first().first; // builtin/unknown -> Built-in
        }
        QStringList playerOpts;
        for (const auto& p : playerOptPairs) playerOpts << p.first;

        // Hardware-decoding choice (issue #67). Display <-> stored id ("off"/"auto"/"on"); the handler maps the
        // picked display back through this same list. Default (auto) always matches, so no undetected fallback.
        const QList<QPair<QString, QString>> hwdecPairs = {
            { tr("Off (software only)"),      QStringLiteral("off")  },
            { tr("Auto (recommended)"),       QStringLiteral("auto") },
            { tr("On (full hardware)"),       QStringLiteral("on")   },
        };
        const QString curHwdec = Settings::hwDecode();
        QString curHwdecDisp = hwdecPairs.at(1).first;               // "auto" default if a stored value is odd
        for (const auto& p : hwdecPairs) if (p.second == curHwdec) { curHwdecDisp = p.first; break; }
        QStringList hwdecOpts;
        for (const auto& p : hwdecPairs) hwdecOpts << p.first;

        // Seek previews (issue #85). ONE control, and it is a SIZE: "Off" is 0 MB, because the honest
        // question about a background job that writes to your disk is how much it may cost you, and a
        // separate on/off switch beside a size would be two ways to say no that can disagree. Display <->
        // stored megabytes; the handler maps the picked display back through this same list.
        const QList<QPair<QString, int>> previewCachePairs = {
            { tr("Off (never make previews)"), 0    },
            { tr("Up to 256 MB"),              256  },
            { tr("Up to 512 MB"),              512  },
            { tr("Up to 1 GB"),                1024 },
            { tr("Up to 2 GB"),                2048 },
            { tr("Up to 5 GB"),                5120 },
        };
        const int curPreviewMb = Settings::previewCacheMb();
        QString curPreviewDisp = previewCachePairs.at(2).first;   // 512 MB default if a stored value is odd
        for (const auto& p : previewCachePairs) if (p.second == curPreviewMb) { curPreviewDisp = p.first; break; }
        QStringList previewCacheOpts;
        for (const auto& p : previewCachePairs) previewCacheOpts << p.first;

        // HDR output choice (issue #68). Display <-> stored id ("tonemap"/"passthrough"); the handler maps the
        // picked display back through this same list. Default (tonemap) always matches, so no undetected fallback.
        // The classic twin below builds the same two-option list under the same "video/hdr" key.
        const QList<QPair<QString, QString>> hdrPairs = {
            { tr("Tone-map to SDR (default)"),            QStringLiteral("tonemap")     },
            { tr("Passthrough when display supports it"), QStringLiteral("passthrough") },
        };
        const QString curHdrId = HdrOutput::idForMode(Settings::hdrOutput());
        QString curHdrDisp = hdrPairs.first().first;                 // "tonemap" default if a stored value is odd
        for (const auto& p : hdrPairs) if (p.second == curHdrId) { curHdrDisp = p.first; break; }
        QStringList hdrOpts;
        for (const auto& p : hdrPairs) hdrOpts << p.first;

        // Default audiobook/podcast speed (issue #140). Display <-> the numeric rate; the handler maps the picked
        // display back through this same list. Music is unaffected (SpeedStore::speedForItem forces it to 1x),
        // and a book with a remembered per-item speed overrides this. The classic twin builds the same list.
        const QList<QPair<QString, double>> defSpeedPairs = {
            { QStringLiteral("0.75×"), 0.75 }, { QStringLiteral("1× (normal)"), 1.0 }, { QStringLiteral("1.25×"), 1.25 },
            { QStringLiteral("1.5×"), 1.5 }, { QStringLiteral("1.75×"), 1.75 }, { QStringLiteral("2×"), 2.0 },
            { QStringLiteral("2.5×"), 2.5 }, { QStringLiteral("3×"), 3.0 },
        };
        const double curDefSpeed = Settings::defaultPlaybackSpeed();
        QString curDefSpeedDisp = defSpeedPairs.at(1).first;         // "1× (normal)" if a stored value is odd
        for (const auto& p : defSpeedPairs) if (qAbs(p.second - curDefSpeed) < 1e-6) { curDefSpeedDisp = p.first; break; }
        QStringList defSpeedOpts;
        for (const auto& p : defSpeedPairs) defSpeedOpts << p.first;

        // Audio jump interval (issue #140). Display <-> the second count; the handler maps the picked display back
        // through this same list, so only a listed value is ever written. The classic twin builds the same list.
        const QList<QPair<QString, int>> jumpPairs = {
            { tr("10 seconds"), 10 }, { tr("15 seconds"), 15 }, { tr("30 seconds"), 30 },
            { tr("45 seconds"), 45 }, { tr("60 seconds"), 60 },
        };
        const int curJump = Settings::audioJumpSeconds();
        QString curJumpDisp = jumpPairs.at(2).first;                 // "30 seconds" if a stored value is odd
        for (const auto& p : jumpPairs) if (p.second == curJump) { curJumpDisp = p.first; break; }
        QStringList jumpOpts;
        for (const auto& p : jumpPairs) jumpOpts << p.first;

        // Touch-gesture edge inset (issue #162). Display <-> pixels; the band along each window edge in which
        // a touch is ignored outright, because the OS's own back / notification swipes start there. The
        // handler maps the picked display back through this same list, and the classic twin builds it too.
        const QList<QPair<QString, int>> gestEdgePairs = {
            { tr("Off"), 0 }, { tr("16 px"), 16 }, { tr("24 px (default)"), 24 },
            { tr("32 px"), 32 }, { tr("48 px"), 48 }, { tr("64 px"), 64 },
        };
        const int curGestEdge = Settings::gestureEdgeInset();
        QString curGestEdgeDisp = gestEdgePairs.at(2).first;         // "24 px" if a stored value is odd
        for (const auto& p : gestEdgePairs) if (p.second == curGestEdge) { curGestEdgeDisp = p.first; break; }
        QStringList gestEdgeOpts;
        for (const auto& p : gestEdgePairs) gestEdgeOpts << p.first;

        // Preferred music source (issue #194). Display <-> the stored value, built by the one shared list so
        // the classic twin below cannot offer a different set of rows.
        const QList<QPair<QString, QString>> musicSrcPairs = musicSourcePrefPairs();
        QStringList musicSourcePrefOpts;
        for (const auto& pr : musicSrcPairs) musicSourcePrefOpts << pr.first;
        const QString musicSourcePrefCur = musicSourcePrefDisplay();

        // ReplayGain mode (issue #141). Display <-> the stored id ("off"/"track"/"album"); the handler maps the
        // picked display back through this same list, so only a listed value is ever written. The default
        // (album) always matches, so there is no undetected fallback. The classic twin builds the same list.
        const QList<QPair<QString, ReplayGain::Mode>> rgPairs = {
            { tr("Off (play as mastered)"),      ReplayGain::Mode::Off   },
            { tr("Per track (best for shuffle)"), ReplayGain::Mode::Track },
            { tr("Per album (default)"),         ReplayGain::Mode::Album },
        };
        const ReplayGain::Mode curRg = Settings::replayGainMode();
        QString curRgDisp = rgPairs.at(2).first;                     // "Per album" if a stored value is odd
        for (const auto& p : rgPairs) if (p.second == curRg) { curRgDisp = p.first; break; }
        QStringList rgOpts;
        for (const auto& p : rgPairs) rgOpts << p.first;

        // ReplayGain preamp (issue #141). Display <-> the dB offset; the contract has no numeric spinner, so
        // the band becomes a Choice in 1 dB steps over ReplayGain's clamp range. The same values back the
        // classic builder's QComboBox, and the handler maps the picked display back through this list.
        QList<QPair<QString, double>> rgPreampPairs;
        for (int db = int(ReplayGain::minPreampDb()); db <= int(ReplayGain::maxPreampDb()); ++db)
            rgPreampPairs << qMakePair(db == 0 ? tr("0 dB (none)")
                                               : QStringLiteral("%1%2 dB").arg(db > 0 ? QStringLiteral("+") : QString()).arg(db),
                                       double(db));
        const double curRgPre = Settings::replayGainPreamp();
        QString curRgPreDisp = tr("0 dB (none)");                    // 0 dB if a stored value is odd
        for (const auto& p : rgPreampPairs) if (qAbs(p.second - curRgPre) < 1e-6) { curRgPreDisp = p.first; break; }
        QStringList rgPreampOpts;
        for (const auto& p : rgPreampPairs) rgPreampOpts << p.first;

        // Crossfade (issue #141). Display <-> the second count; 0 is Off and is the default, then the 1-12 s
        // band #141 names. The handler maps the picked display back through this same list, so only a listed
        // value is ever written. The classic twin builds the same list.
        QList<QPair<QString, int>> xfPairs;
        xfPairs << qMakePair(tr("Off"), Crossfade::offSeconds());
        // Spelled with an explicit singular rather than tr("%n second(s)"): with no translator loaded (the
        // shipped English build) Qt cannot choose a plural form and renders the source string verbatim, so
        // the row read "5 second(s)". The audio-jump row above lists its labels literally for the same reason.
        for (int sec = Crossfade::minSeconds(); sec <= Crossfade::maxSeconds(); ++sec)
            xfPairs << qMakePair(sec == 1 ? tr("1 second") : tr("%1 seconds").arg(sec), sec);
        const int curXf = Settings::crossfadeSeconds();
        QString curXfDisp = xfPairs.at(0).first;                     // "Off" if a stored value is odd
        for (const auto& p : xfPairs) if (p.second == curXf) { curXfDisp = p.first; break; }
        QStringList xfOpts;
        for (const auto& p : xfPairs) xfOpts << p.first;

        // Watch together (issue #86): what a HOST's room does when a guest's stream stalls. Two answers, and
        // WHICH one is right is a social question, not a technical one -- so it is asked, not guessed. The
        // ids are WatchTogether::policyId's, so the setting, the wire and the room state machine all spell
        // the same two words. The classic twin below builds the same list and the same setter.
        const QList<QPair<QString, QString>> wtPairs = {
            qMakePair(tr("Pause for everyone until they catch up"), QStringLiteral("wait")),
            qMakePair(tr("Keep going and show who's behind"),       QStringLiteral("keepgoing")),
        };
        QString curWtDisp = wtPairs.at(0).first;
        for (const auto& p : wtPairs) if (p.second == Settings::watchTogetherPolicy()) { curWtDisp = p.first; break; }
        QStringList wtOpts;
        for (const auto& p : wtPairs) wtOpts << p.first;

        // Attract-mode idle timeout (issue #54). The contract has no numeric spinner, so the minutes become a
        // Choice; the same minute values back the classic builder's QComboBox. The handler maps the picked
        // display back to minutes through this same list, so nothing but a listed value is ever written.
        const QList<QPair<QString, int>> attractTimeouts = {
            { tr("1 minute"), 1 }, { tr("2 minutes"), 2 }, { tr("3 minutes"), 3 }, { tr("5 minutes"), 5 },
            { tr("10 minutes"), 10 }, { tr("15 minutes"), 15 }, { tr("20 minutes"), 20 },
            { tr("30 minutes"), 30 }, { tr("45 minutes"), 45 }, { tr("60 minutes"), 60 },
        };
        QList<QPair<QString, int>> attractTimeoutPairs = attractTimeouts;  // captured by the handler for display->minutes
        const int curAttractMin = Settings::attractTimeoutMinutes();
        QString curAttractDisp;
        for (const auto& a : attractTimeouts) if (a.second == curAttractMin) { curAttractDisp = a.first; break; }
        if (curAttractDisp.isEmpty()) {   // a hand-edited ini value the list doesn't carry: keep it shown/changeable
            curAttractDisp = tr("%n minute(s)", nullptr, curAttractMin);
            attractTimeoutPairs << qMakePair(curAttractDisp, curAttractMin);
        }
        QStringList attractTimeoutOpts;
        for (const auto& a : attractTimeoutPairs) attractTimeoutOpts << a.first;

        // Save-state resume mode (#93). A Choice, mapped display<->Settings::ResumeMode by this list; the same
        // three options back the classic builder's QComboBox. Default Prompt.
        const QList<QPair<QString, int>> resumeModePairs = {
            { tr("Ask me"),        Settings::ResumePrompt },
            { tr("Automatically"), Settings::ResumeSilent },
            { tr("Off"),           Settings::ResumeOff },
        };
        QStringList resumeModeOpts;
        for (const auto& r : resumeModePairs) resumeModeOpts << r.first;
        QString curResumeDisp = resumeModeOpts.first();
        for (const auto& r : resumeModePairs) if (r.second == Settings::resumeMode()) { curResumeDisp = r.first; break; }

        // Runahead frames (#100). The GLOBAL DEFAULT only — the value that actually matters is per game, because
        // the right N is the game's own internal input lag, and that lives in the per-game override store and is
        // reachable from the emulator's pause menu. The same four options back the classic builder's QComboBox.
        const QList<QPair<QString, int>> runaheadPairs = {
            { tr("Off"),      0 },
            { tr("1 frame"),  1 },
            { tr("2 frames"), 2 },
            { tr("3 frames"), 3 },
        };
        QStringList runaheadOpts;
        for (const auto& r : runaheadPairs) runaheadOpts << r.first;
        QString curRunaheadDisp = runaheadOpts.first();
        for (const auto& r : runaheadPairs) if (r.second == Settings::runaheadFrames()) { curRunaheadDisp = r.first; break; }

        // Global-default shader preset (#99). A Choice over the curated ShaderPreset registry, display<->id by
        // this list; the same list backs the classic builder's QComboBox. The current value SEEDS from the legacy
        // video filter on first read (Settings::shaderPreset). A stored id not in the registry (a "custom:<path>"
        // or a hand-edited ini) is appended so it stays shown and changeable rather than snapping silently to Off.
        // This slice ships the GLOBAL DEFAULT only; the per-system / per-game picker + live preview are later.
        QList<QPair<QString, QString>> shaderPresetPairs;   // display -> id
        for (const ShaderPreset::Entry& e : ShaderPreset::registry()) shaderPresetPairs << qMakePair(e.displayName, e.id);
        const QString curShaderId = Settings::shaderPreset();
        QString curShaderDisp;
        for (const auto& p : shaderPresetPairs) if (p.second == curShaderId) { curShaderDisp = p.first; break; }
        if (curShaderDisp.isEmpty()) {   // an id the registry doesn't carry (custom / hand-edited): keep it selectable
            curShaderDisp = ShaderPreset::isCustomId(curShaderId) ? ShaderPreset::customPath(curShaderId) : curShaderId;
            shaderPresetPairs << qMakePair(curShaderDisp, curShaderId);
        }
        QStringList shaderPresetOpts;
        for (const auto& p : shaderPresetPairs) shaderPresetOpts << p.first;

#ifdef EB_HAVE_RETROPARK
        // RetroPark DRIVEN-core host backend (OpenGL host runtime task): a Choice mapping display<->stored id
        // ("d3d11"|"opengl"), the same two options backing the classic builder's QComboBox below. D3D11 is the
        // proven default; OpenGL is the additive opt-in host compositor. Presenting cores (Dolphin/GC) are
        // unaffected — they always run on Vulkan.
        const QList<QPair<QString, QString>> rpDrivenBackendPairs = {
            { tr("Direct3D 11 (default)"), QStringLiteral("d3d11") },
            { tr("OpenGL"),                QStringLiteral("opengl") },
        };
        QStringList rpDrivenBackendOpts;
        for (const auto& p : rpDrivenBackendPairs) rpDrivenBackendOpts << p.first;
        QString curRpDrivenDisp = rpDrivenBackendOpts.first();
        for (const auto& p : rpDrivenBackendPairs)
            if (p.second == Settings::retroParkDrivenBackend()) { curRpDrivenDisp = p.first; break; }
#endif

        // MS-DOS MIDI device (issue #191). The choices come from the msdos LAUNCH RECIPE's `midi` block, not
        // from a list in C++: which devices exist, what each is called and which files each needs are DATA
        // (native/systems/recipes/msdos.json, overridable from <data>/systems/recipes), so adding a device is
        // a file rather than a rebuild. "Default" is always first and stores the EMPTY value — the pre-#191
        // launch, in which the core decides. The same pairs back the classic builder's QComboBox below.
        //
        // NOTHING HERE IS EVER DOWNLOADED. Picking a device does not fetch its ROMs or a soundfont; the launch
        // checks the system folder and, when a file is missing, says which file and which folder.
        QList<QPair<QString, QString>> dosMidiPairs = { { tr("Default (the core decides)"), QString() } };
        {
            const LaunchRecipe& dosRecipe = LaunchRecipes::forSystem(QStringLiteral("msdos"));
            const RecipeCore* dosCore = dosRecipe.isNull()
                ? nullptr : LaunchRecipes::coreFor(dosRecipe, QStringLiteral("dosbox_pure"));
            if (dosCore)
                for (const DosConf::MidiDevice& d : dosCore->midi.devices)
                    dosMidiPairs.push_back({ d.label.isEmpty() ? d.id : d.label, d.id });
        }
        QStringList dosMidiOpts;
        for (const auto& p : dosMidiPairs) dosMidiOpts << p.first;
        QString curDosMidiDisp = dosMidiOpts.first();
        for (const auto& p : dosMidiPairs)
            if (p.second == Settings::dosMidiDevice()) { curDosMidiDisp = p.first; break; }


        // --- Subtitle appearance (issue #71). Display<->value tables for the Choice rows; the same value sets
        // back the classic builder's QComboBoxes. The handler maps each picked display back to its stored value
        // through these lists, so nothing but a listed value is ever written. An out-of-list stored value (a
        // hand-edited ini) is appended so it stays shown and changeable rather than snapping silently. ---
        QStringList subFontOpts; subFontOpts << tr("Default");
        for (const QString& fam : QFontDatabase::families()) subFontOpts << fam;
        const QString curSubFont = Settings::subtitleFont();
        QString curSubFontDisp = curSubFont.isEmpty() ? tr("Default") : curSubFont;
        if (!curSubFont.isEmpty() && !subFontOpts.contains(curSubFont)) subFontOpts << curSubFont;

        QStringList subSizeOpts;
        for (int p : { 50, 75, 90, 100, 110, 125, 150, 175, 200, 250, 300 }) subSizeOpts << QStringLiteral("%1%").arg(p);
        QString curSubSizeDisp = QStringLiteral("%1%").arg(Settings::subtitleSizePercent());
        if (!subSizeOpts.contains(curSubSizeDisp)) subSizeOpts << curSubSizeDisp;

        // One palette backs both text and outline colour; the handler maps display->hex through it.
        const QList<QPair<QString, QString>> subColorPairs = {
            { tr("White"), QStringLiteral("#FFFFFF") },   { tr("Yellow"), QStringLiteral("#FFFF00") },
            { tr("Black"), QStringLiteral("#000000") },   { tr("Red"),    QStringLiteral("#FF0000") },
            { tr("Green"), QStringLiteral("#00FF00") },   { tr("Cyan"),   QStringLiteral("#00FFFF") },
            { tr("Magenta"), QStringLiteral("#FF00FF") }, { tr("Grey"),   QStringLiteral("#808080") },
        };
        QStringList subColorBaseOpts;
        for (const auto& c : subColorPairs) subColorBaseOpts << c.first;
        auto colorDispFor = [&subColorPairs](const QString& hex) -> QString {
            for (const auto& c : subColorPairs) if (c.second.compare(hex, Qt::CaseInsensitive) == 0) return c.first;
            return hex;   // an unlisted stored hex is shown as itself and round-trips (handler treats "#…" as a hex)
        };
        QStringList subTextColorOpts = subColorBaseOpts;
        const QString curSubColorDisp = colorDispFor(Settings::subtitleColor());
        if (!subTextColorOpts.contains(curSubColorDisp)) subTextColorOpts << curSubColorDisp;
        QStringList subBorderColorOpts = subColorBaseOpts;
        const QString curSubBorderColorDisp = colorDispFor(Settings::subtitleBorderColor());
        if (!subBorderColorOpts.contains(curSubBorderColorDisp)) subBorderColorOpts << curSubBorderColorDisp;

        QStringList subBorderOpts;
        for (int p : { 0, 1, 2, 3, 4, 5, 6 }) subBorderOpts << QString::number(p);
        QString curSubBorderDisp = QString::number(Settings::subtitleBorderSize());
        if (!subBorderOpts.contains(curSubBorderDisp)) subBorderOpts << curSubBorderDisp;

        QStringList subOpacityOpts;
        for (int p : { 25, 50, 75, 90, 100 }) subOpacityOpts << QStringLiteral("%1%").arg(p);
        QString curSubOpacityDisp = QStringLiteral("%1%").arg(Settings::subtitleBoxOpacity());
        if (!subOpacityOpts.contains(curSubOpacityDisp)) subOpacityOpts << curSubOpacityDisp;

        QList<QPair<QString, int>> subPosOptPairs = {
            { tr("Top"), 0 }, { tr("Upper"), 25 }, { tr("Middle"), 50 }, { tr("Lower"), 75 }, { tr("Bottom"), 100 },
        };
        const int curSubPos = Settings::subtitlePosition();
        QString curSubPosDisp;
        for (const auto& p : subPosOptPairs) if (p.second == curSubPos) { curSubPosDisp = p.first; break; }
        if (curSubPosDisp.isEmpty()) { curSubPosDisp = QStringLiteral("%1").arg(curSubPos);
                                       subPosOptPairs << qMakePair(curSubPosDisp, curSubPos); }
        QStringList subPosOpts;
        for (const auto& p : subPosOptPairs) subPosOpts << p.first;

        // --- Reader typography (issue #135). Display<->value tables for the Reading Choice rows; the same values
        // back the classic builder's QComboBoxes below. An out-of-list stored value (a hand-edited ini) is
        // appended so it stays shown and changeable rather than snapping silently. ---
        QStringList readerFontOpts; readerFontOpts << tr("Default");
        for (const QString& fam : QFontDatabase::families()) readerFontOpts << fam;
        const QString curReaderFont = Settings::readerFont();
        QString curReaderFontDisp = curReaderFont.isEmpty() ? tr("Default") : curReaderFont;
        if (!curReaderFont.isEmpty() && !readerFontOpts.contains(curReaderFont)) readerFontOpts << curReaderFont;

        QStringList readerSizeOpts;
        for (int p : { 8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40 })
            readerSizeOpts << QStringLiteral("%1 pt").arg(p);
        QString curReaderSizeDisp = QStringLiteral("%1 pt").arg(Settings::readerFontSize());
        if (!readerSizeOpts.contains(curReaderSizeDisp)) readerSizeOpts << curReaderSizeDisp;

        QStringList readerSpacingOpts;
        for (int p : { 100, 115, 130, 150, 175, 200, 250 }) readerSpacingOpts << QStringLiteral("%1%").arg(p);
        QString curReaderSpacingDisp = QStringLiteral("%1%").arg(Settings::readerLineSpacing());
        if (!readerSpacingOpts.contains(curReaderSpacingDisp)) readerSpacingOpts << curReaderSpacingDisp;

        QStringList readerMarginOpts;
        for (int p : { 0, 3, 6, 9, 12, 15, 20, 25 }) readerMarginOpts << QStringLiteral("%1%").arg(p);
        QString curReaderMarginDisp = QStringLiteral("%1%").arg(Settings::readerMargin());
        if (!readerMarginOpts.contains(curReaderMarginDisp)) readerMarginOpts << curReaderMarginDisp;

        const QList<QPair<QString, ReaderTypography::Theme>> readerThemePairs = {
            { tr("Light"),      ReaderTypography::Theme::Light },
            { tr("Sepia"),      ReaderTypography::Theme::Sepia },
            { tr("Dark"),       ReaderTypography::Theme::Dark },
            { tr("True black"), ReaderTypography::Theme::TrueBlack },
        };
        QStringList readerThemeOpts;
        for (const auto& t : readerThemePairs) readerThemeOpts << t.first;
        QString curReaderThemeDisp = readerThemeOpts.first();
        for (const auto& t : readerThemePairs) if (t.second == Settings::readerTheme()) { curReaderThemeDisp = t.first; break; }

        // --- Touch reading (issue #147). The tap-zone PRESET, three of them and no matrix. The same three
        // display<->value pairs back the classic builder's QComboBox below, so one list decides what the
        // rows offer and what they store. The strings say what the zones DO rather than naming a grip:
        // "left turns back" is checkable against the page in front of you; "for a left-thumb grip" is a
        // claim about the reader. ---
        const QList<QPair<QString, int>> readerZonePairs = {
            { tr("Left goes back, right goes forward"), 0 },
            { tr("Left goes forward, right goes back"), 1 },
            { tr("Every tap opens the menu (swipe to turn pages)"), 2 },
        };
        QStringList readerZoneOpts;
        for (const auto& z : readerZonePairs) readerZoneOpts << z.first;
        QString curReaderZoneDisp = readerZoneOpts.first();
        for (const auto& z : readerZonePairs) if (z.second == Settings::readerTapZones()) { curReaderZoneDisp = z.first; break; }

        // --- Audio output (issue #69). The device picker enumerates mpv's audio-device-list from the live
        // player; "Auto" (stored as an empty id) is always the first entry and the default. The handler maps the
        // picked display back to the stored device id through this same list, so nothing but a listed id is ever
        // written. A stored id the current machine no longer enumerates (a receiver unplugged since) is appended
        // so it stays shown and changeable rather than snapping silently to Auto. These are DEVICE-LOCAL keys
        // (audio/*, in CloudSync's carve-out) — the classic twin below builds the same list. ---
        QList<QPair<QString, QString>> audioDevPairs;                 // display -> stored id ("" = Auto)
        audioDevPairs << qMakePair(tr("Auto (system default)"), QString());
        if (player_)
            for (const MpvWidget::AudioDevice& d : player_->availableAudioDevices())
                audioDevPairs << qMakePair(d.description.isEmpty() ? d.name : d.description, d.name);
        const QString curAudioDev = Settings::audioDevice();
        QString curAudioDevDisp = audioDevPairs.first().first;       // Auto unless a stored id matches below
        bool audioDevListed = false;
        for (const auto& p : audioDevPairs) if (p.second == curAudioDev) { curAudioDevDisp = p.first; audioDevListed = true; break; }
        if (!audioDevListed && !curAudioDev.isEmpty()) {             // keep an unenumerated stored device shown
            audioDevPairs << qMakePair(curAudioDev, curAudioDev);
            curAudioDevDisp = curAudioDev;
        }
        QStringList audioDevOpts;
        // --- Following (issue #155): how often the background pass runs. Display -> stored HOURS, built from
        // follow::intervalChoicesHours() rather than from a second hand-written list, so the offered set, the
        // Settings clamp and probe_follow's assertions cannot drift. 0 is MANUAL and is a real choice. The
        // classic twin below builds its combo from the SAME pairs. ---
        QList<QPair<QString, int>> followIntervalPairs;
        for (int h : follow::intervalChoicesHours())
            followIntervalPairs << qMakePair(followIntervalLabel(h), h);
        QStringList followIntervalOpts;
        for (const auto& f : followIntervalPairs) followIntervalOpts << f.first;
        const QString curFollowDisp = followIntervalLabel(Settings::followIntervalHours());

        for (const auto& p : audioDevPairs) audioDevOpts << p.first;

        QVector<PanelRow> rows;
        auto sep    = [&rows](const QString& t) { PanelRow r; r.kind = PanelRow::Separator; r.label = t; rows << r; };
        auto info   = [&rows](const QString& id, const QString& label, const QString& value) {
            PanelRow r; r.kind = PanelRow::Info; r.id = id; r.label = label; r.value = value; rows << r; };
        auto toggle = [&rows](const QString& id, const QString& label, bool on) {
            PanelRow r; r.kind = PanelRow::Toggle; r.id = id; r.label = label; r.checked = on; rows << r; };
        auto action = [&rows](const QString& id, const QString& label) {
            PanelRow r; r.kind = PanelRow::Action; r.id = id; r.label = label; rows << r; };
        auto textf  = [&rows](const QString& id, const QString& label, const QString& value, bool masked = false) {
            PanelRow r; r.kind = PanelRow::TextField; r.id = id; r.label = label; r.value = value; r.masked = masked; rows << r; };
        // #81: over a client-id / secret / API-key row whose BUILT-IN value is the one in use, the row says so
        // instead of "—". Display only (PanelRow::placeholder): the row still holds the user's own, empty, value,
        // stays editable, and whatever the user types there wins. Twin: the classic rows' placeholder text.
        auto builtinHint = [&rows](bool active) {
            if (active && !rows.isEmpty()) rows.last().placeholder = tr("Built in (you can use your own)"); };
        auto choice = [&rows](const QString& id, const QString& label, const QStringList& opts, const QString& cur) {
            PanelRow r; r.kind = PanelRow::Choice; r.id = id; r.label = label; r.options = opts; r.value = cur; rows << r; };

        // #110: the download storage cap. A short list rather than a typed number, for the reason every
        // other Choice row here is one — this is a control somebody reaches with a d-pad. "No limit" is the
        // default and is a real option, not an absence: a cap that cannot be switched off would eventually
        // nag somebody who deliberately keeps a large library on a big disk.
        const QList<QPair<QString, int>> dlCapPairs = {
            { tr("No limit"),  0   }, { tr("10 GB"), 10  }, { tr("25 GB"),  25  },
            { tr("50 GB"),     50  }, { tr("100 GB"), 100 }, { tr("250 GB"), 250 } };
        QStringList dlCapOpts;
        for (const auto& c : dlCapPairs) dlCapOpts << c.first;
        QString dlCapCur = dlCapOpts.value(0);
        for (const auto& c : dlCapPairs) if (c.second == JellyfinDownload::capGb()) { dlCapCur = c.first; break; }

        // --- Display ---
        sep(tr("Display"));
        toggle(QStringLiteral("disp.fullscreen"), tr("Open in full screen on startup"), Settings::startFullscreen());
        // --- Language --- (general preferred content language; governs subtitle + audio track + Accept-Language)
        sep(tr("Language"));
        choice(QStringLiteral("content.lang"), tr("Preferred content language"), langOpts, curLangDisp);
        // --- Attract mode (idle screensaver, issue #54). Its classic twins are in the QWidget builder below. ---
        sep(tr("Attract mode"));
        toggle(QStringLiteral("attract.enabled"), tr("Play a screensaver slideshow when idle"), Settings::attractEnabled());
        choice(QStringLiteral("attract.timeout"), tr("Start after"), attractTimeoutOpts, curAttractDisp);
        info(QStringLiteral("attract.hint"),
             tr("After this long with no input on a menu, drift through your library's artwork. Any button wakes it."),
             QString());
        // --- Home screen (issue #161). Its classic twin is in the QWidget builder below (GS_TWINS). One
        // action row rather than a panel of toggles: the arrangement is a LIST, and a list wants its own
        // screen with move/hide/cap per row, not a settings page that would have to grow a row per producer. ---
        sep(tr("Home screen"));
        action(QStringLiteral("home.rows"), tr("Choose home rows…"));
        info(QStringLiteral("home.rows.state"), tr("Rows"),
             HomeRowStore::isCustomised() ? tr("Customised") : tr("Default"));
        info(QStringLiteral("home.rows.hint"),
             tr("Pick which rows your home shows, put them in the order you want, and cap how many items "
                "each one holds. Syncs with the rest of this profile."),
             QString());
        // --- Library ---
        sep(tr("Library"));
        // Global (not per-profile) override: reveal items any profile has marked hidden from the detail view.
        toggle(QStringLiteral("lib.showhidden"), tr("Show hidden items"),
               store().value(QStringLiteral("library/showHidden"), false).toBool());
        // --- Following (issue #155). The classic twins are in the QWidget builder below (GS_TWINS). ---
        sep(tr("Following"));
        choice(QStringLiteral("following.interval"), tr("Check followed series"), followIntervalOpts, curFollowDisp);
        toggle(QStringLiteral("following.metered"), tr("Check on metered connections"), Settings::followOnMetered());
        toggle(QStringLiteral("following.notify"), tr("Notify me about new episodes"), Settings::followNotify());
        action(QStringLiteral("following.check"), tr("Check for new items now"));
        // The scheduler's own status, not a sentence this builder picks (#420): a panel rebuilt mid-check says
        // "Checking…", one rebuilt after it says how it went, and before any check it is the old hint.
        info(QStringLiteral("following.hint"), tr("Following"), followStatusLine());
        // --- Updates ---
        sep(tr("Updates"));
        info(QStringLiteral("update.version"), tr("Version"), AppUpdater::currentVersion());
        toggle(QStringLiteral("update.autocheck"), tr("Check for updates on startup"), Settings::checkUpdatesOnStartup());
        action(QStringLiteral("update.check"), tr("Check for updates now"));
        action(QStringLiteral("update.install"), (updater_ && updater_->updatePending())
                   ? tr("Install %1 and restart").arg(updater_->latestVersion()) : tr("Install update"));
        info(QStringLiteral("update.status"), tr("Status"), QString());
        // --- Remote control (issue #76). The classic twins are in the QWidget builder below. The URL info row
        // shows the LAN address to open on a phone while it is on; the toggle starts/stops the server live. ---
        sep(tr("Remote control"));
        toggle(QStringLiteral("remote.enabled"), tr("Control from a phone on your network"),
               Settings::remoteControlEnabled());
        info(QStringLiteral("remote.url"), tr("Open on your phone"),
             Settings::remoteControlEnabled()
                 ? RemoteServer::lanUrl(static_cast<quint16>(Settings::remoteControlPort()))
                 : tr("Turn on to get a URL"));
        info(QStringLiteral("remote.hint"),
             tr("A tiny local web control (play/pause, seek, D-pad). Off by default; no accounts, LAN only."),
             QString());
        // #115: the LAN file drop. Classic twin in the QWidget builder below (GS_TWINS). Upload only, into fixed
        // folders, token-gated by pairing; defined in MainWindowFileDrop.cpp.
        toggle(QStringLiteral("remote.filedrop"), tr("Receive files over the network (file drop)"),
               Settings::fileDropEnabled());
        info(QStringLiteral("remote.filedrop.url"), tr("Open in a browser"), fileDropStatusText());
        // #80: everythingbox:// links. Classic twin in the QWidget builder below (GS_TWINS). Off by default; the
        // OS registration is per user, and every link still asks before installing anything (MainWindowDeepLink.cpp).
        sep(tr("Add-on links"));
        toggle(QStringLiteral("deeplink.enabled"), tr("Open everythingbox:// links"), Settings::deepLinksEnabled());
        info(QStringLiteral("deeplink.hint"),
             tr("Lets an add-on's website hand its install link straight to EverythingBox. Every link still shows "
                "what it would install and where it comes from, and nothing is installed until you press Install."),
             QString());
        // --- Play on device (issue #143). Twins in the QWidget builder below (GS_TWINS). The name row is what
        // OTHER boxes show in their picker; the picker row is the way in when nothing is playing (during
        // playback the same targets are on the cast button). ---
        sep(tr("Play on device"));
        info(QStringLiteral("playon.name"), tr("This device is called"), Settings::deviceName());
        action(QStringLiteral("playon.rename"), tr("Rename this device…"));
        action(QStringLiteral("playon.pick"), tr("Play on another device…"));
        // #127: the same peers, the same pairing, a different payload. Twin below in the QWidget builder.
        action(QStringLiteral("playon.sendlib"), tr("Send library to device…"));
        info(QStringLiteral("playon.sendhint"),
             tr("Warms another box's artwork cache from this one so it doesn't re-scrape thousands of images. "
                "Only what's missing or newer is sent, so running it twice sends nothing the second time. It "
                "moves artwork and metadata — never your marks, favourites or resume points, and never the "
                "games or videos themselves."),
             QString());
        info(QStringLiteral("playon.hint"),
             tr("Other EverythingBoxes on your network appear beside Chromecast and DLNA in the cast picker. "
                "A hand-off sends what to play and where you are in it — never the video itself — so the other "
                "device fetches its own stream. Needs remote control on at BOTH ends."),
             QString());
        // --- Watch together (issue #86). Twins in the QWidget builder below (GS_TWINS). ---
        sep(tr("Watch together"));
        action(QStringLiteral("wt.open"), tr("Watch together…"));
        choice(QStringLiteral("wt.buffering"), tr("When someone's stream stalls"), wtOpts, curWtDisp);
        info(QStringLiteral("wt.hint"),
             tr("Host a room, give someone the code, and the two of you watch the same thing at the same "
                "point — play, pause and seek stay in step. The room shares WHAT to play, never the video "
                "and never your account: each of you fetches your own stream with your own addons. Somebody "
                "who can't get it says so and stays in the room. Over the internet it uses the same relay "
                "as online netplay."),
             QString());
        // --- Live TV. The home shelf hides itself until a source exists, so this is the way in for the first
        // one (and the only way in when the last one is removed). ---
        sep(tr("Live TV"));
        info(QStringLiteral("livetv.count"), tr("Sources"),
             tr("%n saved", "", int(IptvSourceStore::list().size())));
        action(QStringLiteral("livetv.add"), tr("Add a Live TV source…"));
        info(QStringLiteral("livetv.hint"),
             tr("An M3U playlist — a URL or a local file. Once one is saved, Live TV appears under Video, "
                "where you can browse its channels and remove it."),
             QString());
        // --- Channels (issue #179 increment 2): the GLOBAL bumper folder. Its classic twin is in the QWidget
        // builder below (GS_TWINS) - same Setting, same picker, one write path. Empty means no bumpers, which
        // is the default and is not an error; a channel may name its own folder in its editor instead. ---
        sep(tr("Channels"));
        info(QStringLiteral("chan.bumperpath"),
             Settings::interstitialFolder().isEmpty() ? tr("No bumper folder set")
                                                      : Settings::interstitialFolder(),
             QString());
        action(QStringLiteral("chan.bumperchange"), tr("Choose bumper folder…"));
        action(QStringLiteral("chan.bumperclear"), tr("Clear bumper folder"));
        info(QStringLiteral("chan.bumperhint"),
             tr("Short idents and bumpers your channels play BETWEEN programmes. They fill the gap a "
                "channel's break grid leaves and never delay a programme: if there is no room for one, none "
                "airs. A channel whose programmes run back to back has no gaps and so plays none. Each file "
                "has to have been played once before its length is known."),
             QString());
        // --- Game ROMs ---
        sep(tr("Game ROMs"));
        info(QStringLiteral("roms.path"), Settings::romsFolder(), QString());
        action(QStringLiteral("roms.change"), tr("Change ROMs folder…"));
        action(QStringLiteral("roms.open"), tr("Open ROMs folder"));
        toggle(QStringLiteral("roms.keepscrape"), tr("Keep scraped data in the ROMs folder (gamelist.xml)"),
               Settings::keepScrapedData());
        toggle(QStringLiteral("roms.softpatch"), tr("Auto-apply ROM patches (translations, romhacks)"),
               Settings::autoApplyRomPatches());
        toggle(QStringLiteral("roms.verify"), tr("Verify ROMs against DAT files (No-Intro / Redump)"),
               Settings::verifyRoms());
        // 1G1R region collapsing (issue #50): show one entry per game when a folder holds region/revision
        // variants of the same title (USA/Europe/Japan). Off by default; the losers stay reachable from the
        // game's detail view ("Other versions"). The ordered region-priority editor is a follow-up — for now
        // the order defaults sensibly per app language. Twin below in the QWidget builder.
        toggle(QStringLiteral("roms.collapseregions"), tr("Collapse regional duplicates"),
               Settings::collapseRegionalDuplicates());
        // Persist a downloaded online game into the ROMs folder so the next play finds it locally and doesn't
        // re-download (the transient url-hash cache misses every time — the debrid url rotates). Default on.
        // Twin below in the QWidget builder.
        toggle(QStringLiteral("roms.keepdownloads"), tr("Keep downloaded games in the ROMs folder"),
               Settings::keepDownloadsInRoms());
        // Auto-install official Sony updates for a PS3 game before RPCS3 boots it. Default on; a failed update
        // never blocks the launch. Twin below in the QWidget builder.
        toggle(QStringLiteral("ps3.autoupdate"), tr("Auto-install PS3 game updates"),
               Settings::ps3AutoUpdate());
        // Install a game's own update/DLC packages — whatever sits in the `updates/` and `dlc/` folders beside
        // the game — into the target emulator before it boots (issue #189). Default on; a failed install is
        // reported and the base game still launches. Twin below in the QWidget builder.
        toggle(QStringLiteral("content.autoinstall"), tr("Install game updates and DLC before launch"),
               Settings::installGameContent());
        // --- Save states (#93) ---
        sep(tr("Save states"));
        toggle(QStringLiteral("emu.autoinc"), tr("Quick-save to the next free slot (keep a history)"),
               Settings::stateAutoIncrement());
        choice(QStringLiteral("emu.resume"), tr("Resume where you left off"), resumeModeOpts, curResumeDisp);
        info(QStringLiteral("emu.resumehint"),
             tr("Closing a game saves your spot to a reserved slot (never one of your numbered slots), so you can "
                "pick up where you left off next time."), QString());
        // --- Hardcore RetroAchievements (#94). Opt-in; enabling asks for consent (the handler runs NavConfirm)
        // and resets the current achievement session. The classic twin below builds the same setter + consent. ---
        toggle(QStringLiteral("emu.hardcore"), tr("Hardcore RetroAchievements (no save states, rewind or cheats)"),
               Settings::hardcoreAchievements());
        info(QStringLiteral("emu.hardcorehint"),
             tr("Hardcore mode earns full RetroAchievements prestige (softcore unlocks are second-class on the "
                "site, and leaderboards only count in hardcore), but disables save states, rewind, fast-forward "
                "and cheats while you play. Enabling resets your current achievement session. Softcore stays the "
                "default and fully supported."), QString());
        // --- Runahead (#100). The global DEFAULT number of frames to run ahead; per-game values (the ones that
        // matter) are set from the emulator's pause menu. Twin below in the QWidget builder. ---
        choice(QStringLiteral("emu.runahead"), tr("Runahead (default)"), runaheadOpts, curRunaheadDisp);
        info(QStringLiteral("emu.runaheadhint"),
             tr("Many classic games read the controller a frame or three before they draw the response. Runahead "
                "hides that delay by emulating those frames in advance, so a button press shows up immediately. "
                "It costs several times the emulation work per frame, so it is refused — with a reason — on a "
                "game or a device that can't afford it, and it is off during netplay and split screen. Set it "
                "per game from the pause menu; the right number is that game's own lag, not a device setting."),
             QString());
        // --- Shader preset (#99). The global default slang-shader preset; per-system/per-game overrides and a
        // live preview are a later slice, and the shader chain itself does not render yet (librashader is not
        // vendored). Twin below in the QWidget builder. ---
        choice(QStringLiteral("emu.shaderpreset"), tr("Shader preset (experimental)"), shaderPresetOpts, curShaderDisp);
        info(QStringLiteral("emu.shaderpresthint"),
             tr("Slang shaders reproduce the look of a CRT, an LCD grid or a crisp upscale. This sets the default "
                "for every game; heavy presets can slow weak GPUs. Loading custom .slangp files and per-game "
                "overrides arrive in a later update."), QString());
#ifdef EB_HAVE_RETROPARK
        // RetroPark driven backend (OpenGL host runtime task): the host graphics API RetroPark's DRIVEN cores (the
        // in-process NES shim / reference core) run on. D3D11 is the proven default; OpenGL is an opt-in. Presenting
        // cores (Dolphin / GameCube) always run on Vulkan and are unaffected. Classic twin in the QWidget builder.
        choice(QStringLiteral("emu.rpdriven"), tr("RetroPark driven backend"), rpDrivenBackendOpts, curRpDrivenDisp);
        info(QStringLiteral("emu.rpdrivenhint"),
             tr("Which graphics API RetroPark uses to run its in-process (NES / reference) cores. Direct3D 11 is "
                "the proven default; OpenGL is an experimental opt-in. GameCube (Dolphin) is unaffected — it "
                "always uses Vulkan. Takes effect the next time you launch a game."), QString());
#endif

        // MS-DOS MIDI device (issue #191). Classic twin in the QWidget builder below (GS_TWINS).
        choice(QStringLiteral("emu.dosmidi"), tr("MS-DOS MIDI device"), dosMidiOpts, curDosMidiDisp);
        info(QStringLiteral("emu.dosmidihint"),
             tr("Many DOS games sound far better through a Roland MT-32 or General MIDI than through Adlib. "
                "The ROMs and the soundfont are yours to supply - EverythingBox never downloads them. Put "
                "MT32_CONTROL.ROM and MT32_PCM.ROM, or a soundfont named DOSBOX.SF2, in the system folder; "
                "if a file is missing the game still plays, on its default audio, and says which file it "
                "wanted."), QString());
        // --- Local Library (movies + TV) ---
        sep(tr("Local Library"));
        info(QStringLiteral("library.path"), Settings::libraryFolder(), QString());
        action(QStringLiteral("library.change"), tr("Change Local Library folder…"));
        action(QStringLiteral("library.rescan"), tr("Rescan Local Library"));
        toggle(QStringLiteral("library.resolveonline"), tr("Match local files to online catalogs"),
               Settings::resolveOnline());
        action(QStringLiteral("library.rematch"), tr("Re-match Local Library online"));
        // The per-item metadata editor's library-wide escape hatch (issue #24). It belongs NEXT to re-match
        // because that is what a user reaches for when the library looks wrong, and this is the counterpart:
        // re-match discards what the SCRAPER decided, this discards what YOU decided. The editor itself lives
        // on each item's detail card — this is only the bulk undo, and it says how many items are affected so
        // it is never a blind "reset everything".
        action(QStringLiteral("library.clearmetaedits"),
               tr("Reset my metadata edits (%n item(s))", nullptr, MetaOverrides::count()));
        // --- Jellyfin (#160): the connected servers. ONE row, because add / enable / remove are three
        // verbs about the same list and splitting them across three settings rows would put the list itself
        // nowhere. The classic twin is below; a setting in one builder only is unreachable in the other
        // mode. The info line under it is the ONE place that says whether anything is connected at all,
        // and — see jellyfinServerStatusLine — it names no user, no address and no token.
        sep(tr("Jellyfin"));
        action(QStringLiteral("jellyfin.servers"), tr("Jellyfin servers…"));
        info(QStringLiteral("jellyfin.serverstatus"), tr("Jellyfin"), jellyfinServerStatusLine());
        // #160 increment 2: one Continue Watching section across the servers, or one per server. DEVICE-
        // LOCAL, under the "jellyfin/" prefix CloudSync::isDeviceLocalKey already carves out — which box
        // you want your half-watched rows grouped by is a fact about this screen, not about the library.
        // The classic twin is in the QWidget builder below (GS_TWINS).
        toggle(QStringLiteral("jellyfin.continuemerge"), tr("Merge Continue Watching across servers"),
               JellyfinServerStore::continueMerged());
        // --- Requests (#109): the service that goes and gets things you do not have. ONE row, because set
        // up / replace the key / forget it are three verbs about one credential. The classic twin is in the
        // QWidget builder below; a setting in one builder only is unreachable in the other mode. The info
        // line under it is the ONE place that says whether anything is set up at all, and — see
        // JellyseerrStore::statusLine — it names no address, no account and no key.
        sep(tr("Requests"));
        action(QStringLiteral("requests.service"), tr("Request service…"));
        info(QStringLiteral("requests.status"), tr("Requests"), JellyseerrStore::statusLine());
        // --- Downloads (#110): the offline-viewing hygiene pair. Both DEVICE-LOCAL by nature — they are
        // about the files on THIS disk — and both under the "downloads" prefix CloudSync::isDeviceLocalKey
        // already carves out of the synced bundle. Twins live in the QWidget builder below.
        sep(tr("Downloads"));
        choice(QStringLiteral("downloads.cap"), tr("Storage limit for downloads"), dlCapOpts, dlCapCur);
        info(QStringLiteral("downloads.caphint"),
             tr("When downloads go over the limit you are shown the least recently watched ones and can "
                "remove them. Nothing is ever deleted for you."), QString());
        toggle(QStringLiteral("downloads.removewatched"), tr("Offer to remove downloads once watched"),
               JellyfinDownload::removeAfterWatched());
        // --- Photos (#102) ---
        sep(tr("Photos"));
        info(QStringLiteral("photos.path"), Settings::photosFolder(), QString());
        action(QStringLiteral("photos.change"), tr("Change Photos folder…"));
        // --- Music (#74): the local music library's root + rescan. Twins live in the QWidget builder below.
        // NOT the same folder as "Open music folder" under Background Music — that one holds the interface's
        // own ambient loops (BackgroundMusic::musicDir()); this one is the user's record collection.
        sep(tr("Music"));
        info(QStringLiteral("music.path"), Settings::musicFolder(), QString());
        action(QStringLiteral("music.change"), tr("Change Music folder…"));
        action(QStringLiteral("music.rescan"), tr("Rescan Music"));
        // Music SERVERS (#193, increment 5). The classic twin is below; a setting in one builder only is
        // unreachable in the other mode. The servers themselves are managed from the Music category's own
        // "Music Servers" shelf (which is where removing and re-entering one belongs, beside the thing it
        // is about) — this row is the doorway for somebody who is already in Settings, and the info line
        // below it is the ONE place that says whether any are set up at all.
        action(QStringLiteral("music.addserver"), tr("Add a music server…"));
        info(QStringLiteral("music.serverstatus"), tr("Music servers"), musicServerStatusLine());
        // Multi-value artist/genre separators (#196). Editing it re-tags the library, because a cached entry
        // is never re-opened otherwise and the change would appear to do nothing. Classic twin below.
        // ONE LIBRARY ACROSS SOURCES (#194). Which copy plays when a record is both on this disk and on a
        // server, and the reset for the manual "these are / are not the same" corrections. Classic twins below;
        // a setting in one builder only is unreachable in the other mode.
        choice(QStringLiteral("music.prefsource"), tr("Play music from"), musicSourcePrefOpts,
               musicSourcePrefCur);
        // SERVER STREAMING QUALITY (#193). Per device, never synced (Settings.h). Classic twin below (GS_TWINS).
        {
            QStringList ssqOpts;
            QString ssqCur;
            for (const auto& pr : subsonicStreamQualityPairs())
            {
                ssqOpts << pr.first;
                if (pr.second == Settings::subsonicStreamMaxBitRate()) ssqCur = pr.first;
            }
            choice(QStringLiteral("music.streamquality"), tr("Server streaming quality"), ssqOpts,
                   ssqCur.isEmpty() ? ssqOpts.value(0) : ssqCur);
            info(QStringLiteral("music.streamqualityhint"), subsonicStreamQualityHint(), QString());
        }
        info(QStringLiteral("music.prefsourcehint"),
             tr("When the same album is on this device and on a music server, this is the copy that plays. "
                "The others stay one press away on the album itself. Matching is deliberately cautious: two "
                "copies it is not sure about are left as two rows rather than merged into one, because a "
                "wrong match hides music."), QString());
        action(QStringLiteral("music.clearmatches"),
               tr("Reset my music match corrections (%n)", nullptr, musicMatchOverrideCount()));
        textf(QStringLiteral("music.separators"), tr("Artist / genre separators"),
              Settings::musicTagSeparators());
        info(QStringLiteral("music.separatorshint"),
             tr("Characters that separate several artists or genres inside ONE tag, spaced apart — \"; /\" is "
                "two of them. Files that store their values properly (FLAC, and mp3s tagged as ID3v2.4) are "
                "already split correctly and are never affected by this. Album artist is never split, so "
                "albums stay whole. Only \";\" by default: \"/\" would turn AC/DC into two bands. Leave it "
                "empty to split nothing."), QString());
        // --- Audiobooks (#139): the local audiobook library's root + rescan. Classic twins below; a
        // setting in one builder only is unreachable in the other mode.
        //
        // A SEPARATE FOLDER FROM MUSIC, and the hint says why in the user's own terms: this app does not
        // sniff a file to decide whether it is a book, because every rule for doing that is silently wrong
        // about somebody's collection. Which folder a file is in IS the answer.
        sep(tr("Audiobooks"));
        info(QStringLiteral("audiobooks.path"), Settings::audiobookFolder(), QString());
        action(QStringLiteral("audiobooks.change"), tr("Change Audiobooks folder…"));
        action(QStringLiteral("audiobooks.rescan"), tr("Rescan Audiobooks"));
        info(QStringLiteral("audiobooks.hint"),
             tr("Point this at a folder of your own audiobooks and they browse by author, narrator and "
                "series. A folder of numbered files is treated as ONE book that plays straight through and "
                "remembers where you were. This is kept apart from your Music folder on purpose: nothing is "
                "guessed from the file, so an mp3 in here is a book and the same mp3 in your music folder "
                "is music."), QString());
        // Audiobook SERVERS (#197). The classic twin is below; a setting in one builder only is unreachable
        // in the other mode. The servers themselves are managed from the Audiobooks category's own
        // "Audiobook Servers" shelf — which is where removing one belongs, beside the thing it is about —
        // and this row is the doorway for somebody who is already in Settings. The info line below it is
        // the ONE place that says whether any are set up at all.
        action(QStringLiteral("audiobooks.addserver"), tr("Add an audiobook server…"));
        info(QStringLiteral("audiobooks.serverstatus"), tr("Audiobook servers"),
             audiobookServerStatusLine());
        // --- Books (#134): the local reading library's root + rescan. Classic twins below; a setting in
        // one builder only is unreachable in the other mode.
        //
        // ONE FOLDER FOR BOOKS AND COMICS, and the hint says why in the user's own terms: an .epub and a
        // .cbz are different KINDS of file and the app can see that for itself, so it does not need to be
        // told twice. What it cannot see is whether a pile of PDFs is a library, and that is the question
        // this folder answers.
        sep(tr("Books"));
        info(QStringLiteral("books.path"), Settings::readingFolder(), QString());
        action(QStringLiteral("books.change"), tr("Change Books folder…"));
        action(QStringLiteral("books.rescan"), tr("Rescan Books"));
        // Online blank-filling (#134 increment 2), DEFAULT OFF. Classic twin below (GS_TWINS). It is worded
        // as what it does rather than as what it is — "missing" is the whole scope, and a book whose file
        // already carries a cover and an author is never asked about however long this stays on.
        toggle(QStringLiteral("books.enrich"), tr("Fill in missing book covers and authors online"),
               Settings::booksEnrichOnline());
        info(QStringLiteral("books.hint"),
             tr("Point this at a folder of your own books and comics and they browse by author and series. "
                "EPUB books bring their own title, author and cover; comics are grouped by what their files "
                "are called. Anything with no information at all still shows up, under its file name."),
             QString());
        // --- Playback ---
        sep(tr("Playback"));
        toggle(QStringLiteral("pb.autonext"), tr("Auto-play the next episode"), Settings::autoplayNextEpisode());
        // Gapless playback (#141): default off. On, an audio queue (album/folder) plays with no seam between
        // tracks — mpv flows continuously across the boundary. The classic twin below builds the same setter.
        toggle(QStringLiteral("pb.gapless"), tr("Gapless playback"), Settings::gaplessAudio());
        // ReplayGain (#141): default Per album. Album mode keeps the loudness relationships INSIDE a record —
        // the quiet interlude stays quieter than the single — which is the point for anyone who cares about the
        // record; per track is the right answer for a shuffled queue, where the tracks have no relationship to
        // preserve. Music only: audiobooks and podcasts are never levelled. The classic twins below build the
        // same lists + setters, and both re-apply live via applyReplayGainLive().
        choice(QStringLiteral("pb.replaygain"), tr("Volume levelling (ReplayGain)"), rgOpts, curRgDisp);
        choice(QStringLiteral("pb.rgpreamp"), tr("Levelling preamp"), rgPreampOpts, curRgPreDisp);
        info(QStringLiteral("pb.replaygainhint"),
             tr("Plays tracks at a matched volume using the ReplayGain tags they already carry — nothing is "
                "analysed and no file is ever modified. Per album keeps the loudness differences within a "
                "record; per track is better for shuffle. Untagged files, audiobooks and podcasts are left "
                "alone. The preamp shifts everything levelling touches up or down."), QString());
        // Crossfade (#141): default Off, then 1-12 s. Deliberately opt-in where ReplayGain is opt-out - a
        // crossfade rewrites every boundary it is allowed near, so it waits to be asked for. Music only, and
        // never between two tracks of the same album: a live record's seams are part of the record. The
        // classic twin below builds the same list + setter.
        choice(QStringLiteral("pb.crossfade"), tr("Crossfade between tracks"), xfOpts, curXfDisp);
        info(QStringLiteral("pb.crossfadehint"),
             tr("Overlaps the end of one track with the start of the next by this much. Applies to music "
                "queues only - audiobooks, podcasts and video are never crossfaded, and neither are two "
                "tracks from the same album, so a live or continuous record still plays with its own seams. "
                "Takes effect on the next queue you start."), QString());
        // Online lyric lookup (#142, source 3): default ON. Only ever consulted for a track that has NO .lrc
        // sidecar and NO embedded lyrics, once, while it is actually playing, and the answer is cached beside
        // the item so it never asks twice. The classic twin below builds the same setter.
        toggle(QStringLiteral("pb.onlinelyrics"), tr("Look up lyrics online"), Settings::onlineLyrics());
        info(QStringLiteral("pb.onlinelyricshint"),
             tr("Fetches synced lyrics from LRCLIB, a free community database that needs no account and no "
                "key, for tracks that do not already have them. A lyrics file you saved next to the song, or "
                "lyrics stored inside the file's own tags, are always used first and are never sent anywhere. "
                "Each lookup happens once, while the track plays, and is saved so it works offline after."),
             QString());
        // Default audiobook/podcast speed (issue #140). Each book then remembers the speed you last chose;
        // music always plays at 1x unless you change it. The classic twin below builds the same list + setter.
        choice(QStringLiteral("pb.defaultspeed"), tr("Default audiobook speed"), defSpeedOpts, curDefSpeedDisp);
        info(QStringLiteral("pb.defaultspeedhint"),
             tr("Applied to audiobooks and podcasts with no remembered speed. Each book remembers the speed you "
                "last chose; music always plays at 1× unless you change it."), QString());
        // Audio jump interval (issue #140): how far the skip-back / skip-forward controls jump while playing
        // audio. The classic twin below builds the same list + setter. Video seeking is unaffected.
        choice(QStringLiteral("pb.jump"), tr("Audio jump interval"), jumpOpts, curJumpDisp);
        info(QStringLiteral("pb.jumphint"),
             tr("How far the skip-back and skip-forward controls jump in an audiobook or podcast. Video seeking "
                "is unchanged."), QString());
        // --- Gestures (issue #162). ONE "Gestures" home in the UI, sitting with the jump interval it shares
        // rather than in a screen of its own. Every row is a whole gesture FAMILY, on by default, and every
        // one of them is inert unless this device reports a touch form factor — so a desktop or TV user sees
        // rows that describe something their input cannot do, which the hint says outright rather than
        // hiding the section and leaving a phone-and-TV household unable to find it from the couch. The
        // classic twins are in the QWidget builder below (GS_TWINS). ---
        sep(tr("Gestures"));
        info(QStringLiteral("gest.hint"),
             tr("Swipe, tap and pinch over a playing video. These apply on touch screens only — a mouse, a "
                "keyboard and a remote behave exactly as they always have."), QString());
        toggle(QStringLiteral("gest.volume"), tr("Swipe up and down on the right for volume"),
               Settings::gestureVolume());
        toggle(QStringLiteral("gest.brightness"), tr("Swipe up and down on the left for brightness"),
               Settings::gestureBrightness());
        toggle(QStringLiteral("gest.seek"), tr("Swipe across to scrub"), Settings::gestureSeek());
        toggle(QStringLiteral("gest.doubletap"), tr("Double-tap the sides to skip"), Settings::gestureDoubleTap());
        toggle(QStringLiteral("gest.longpress"), tr("Hold for double speed"), Settings::gestureLongPress());
        toggle(QStringLiteral("gest.pinch"), tr("Pinch to change how the video fits"), Settings::gesturePinch());
        choice(QStringLiteral("gest.edge"), tr("Ignore touches near the screen edge"), gestEdgeOpts, curGestEdgeDisp);
        info(QStringLiteral("gest.edgehint"),
             tr("A double-tap skips by the same interval as the row above. The edge band is left to the system "
                "so its own back and notification swipes still work."), QString());
        // Offer to skip an episode's opening and end credits when one is known; "Skip automatically" seeks
        // past them without asking, instead of showing a button.
        toggle(QStringLiteral("pb.skipseg"), tr("Skip intros and credits"), Settings::skipSegments());
        toggle(QStringLiteral("pb.skipsegauto"), tr("Skip them automatically (no button)"), Settings::skipSegmentsAuto());
        info(QStringLiteral("pb.skipseghint"),
             // Named for whichever device is driving: hintText() hands back the key itself on a mouse and the
             // mapped controller button on a pad, so the mode test is written once in InputMode rather than at
             // every string. This panel is rebuilt on every open, so it never needs a changed() subscription.
             tr("While a video is playing: %1 skips the offered segment, %2 marks where one starts and ends.")
                 .arg(InputMode::instance().hintText(QStringLiteral("S")),
                      InputMode::instance().hintText(QStringLiteral("I"))),
             QString());
        // Hardware video decoding (issue #67). Auto prefers safe copy-back decode and falls back to software;
        // the twin below lives in the QWidget builder. Applies to the next video opened.
        choice(QStringLiteral("pb.hwdec"), tr("Hardware video decoding"), hwdecOpts, curHwdecDisp);
        info(QStringLiteral("pb.hwdechint"),
             tr("Auto uses safe hardware decode with a software fallback. Applies to the next video you open."),
             QString());
        // Seek previews (issue #85). Placed here because generation is decode-bound and this is the setting
        // above it that governs decoding. Twin below in the QWidget builder.
        choice(QStringLiteral("pb.seekpreview"), tr("Seek preview thumbnails"), previewCacheOpts, curPreviewDisp);
        info(QStringLiteral("pb.seekpreviewhint"),
             tr("Shows a picture of where you are dragging to on the seek bar. Thumbnails are made in the "
                "background, between playbacks, for videos stored on this device only — a stream is never "
                "previewed. Older previews are deleted first when the limit is reached."), QString());
        // Idle library sweep (issue #302). SEPARATE from the size above, and it is not a second way to say
        // no: with the size at Off this is inert, so the two can never disagree in the direction that
        // matters. It answers a different question — may the app go LOOKING for work — and it is off by
        // default because it is the one part of this feature the user did not implicitly ask for by
        // opening a file. Twin below in the QWidget builder.
        toggle(QStringLiteral("pb.seekpreviewidle"), tr("Make them ahead of time when idle"),
               Settings::previewIdleScan());
        info(QStringLiteral("pb.seekpreviewidlehint"),
             tr("Goes through the videos in your library while nothing else is happening, so a film has its "
                "previews the first time you watch it. One at a time, and it stops the moment you start "
                "playing something, a library scan begins or you touch anything. Never while running on "
                "battery, and never when this device cannot tell whether it is plugged in."), QString());
        // Refresh-rate matching, Tier 1 (issue #70). video-sync=display-resync locks video to the display clock
        // (mpv resamples audio) to smooth 24fps-on-60Hz judder; default on for desktop/TV, off on iOS's software
        // render path (RefreshSync::videoSyncFor). Applies to the next video. Twin below in the QWidget builder.
        toggle(QStringLiteral("pb.refreshsync"), tr("Reduce judder (sync video to display)"),
               Settings::videoRefreshSync());
        info(QStringLiteral("pb.refreshsynchint"),
             tr("Resamples audio to lock video to your display's refresh, smoothing the judder of 24fps film on a "
                "60Hz screen. Applies to the next video you open."), QString());
        // HDR output (issue #68). Tone-map maps HDR down to SDR so it stops washing out on an SDR panel;
        // Passthrough signals HDR10 to a display that supports it (Windows/Android) and tone-maps as the fallback
        // elsewhere (HdrOutput::optionsFor; iOS is forced to tone-map). Twin below in the QWidget builder.
        choice(QStringLiteral("pb.hdr"), tr("HDR video"), hdrOpts, curHdrDisp);
        info(QStringLiteral("pb.hdrhint"),
             tr("Tone-map keeps HDR looking right on an SDR screen; Passthrough sends HDR10 to a display that "
                "supports it (Windows/Android), tone-mapping where it doesn't. Applies to the next video you open."),
             QString());
        // Videos play in the built-in player by default, or hand off to an installed/custom external player.
        // Hidden ENTIRELY for a restricted (kids) profile — no external escape hatch offered, PIN or not.
        if (!ProfileStore::current().restricted)
        {
            choice(QStringLiteral("player.external"), tr("Play videos with"), playerOpts, curPlayerDisp);
            action(QStringLiteral("player.custompath"), Settings::externalPlayerPath().isEmpty()
                       ? tr("Choose custom player program…")
                       : tr("Custom player: %1").arg(QFileInfo(Settings::externalPlayerPath()).fileName()));
        }
        toggle(QStringLiteral("pb.bezel"), tr("Show bezel / border art around games"), Settings::bezelEnabled());
        action(QStringLiteral("pb.bezelopen"), tr("Open bezels folder"));
        // Decoration packs (#187) sit HERE, next to the bezel toggle, as well as on Appearance beside the
        // theme gallery. This is where #106's selection lives, so it is where someone who has just turned
        // bezels on and found the folder empty is standing — the gallery being reachable only from a panel
        // about themes is the shape of "a setting reachable from one surface is unreachable".
        action(QStringLiteral("pb.decorations"),
               decorationPackCount() > 0
                   ? tr("Decoration packs — get more… (%n installed)", "", decorationPackCount())
                   : tr("Decoration packs — get more…"));
        // --- Audio output (issue #69). Device / passthrough / exclusive mode, mapped to mpv audio-device /
        // audio-spdif / audio-exclusive. DEVICE-LOCAL (audio/* is in CloudSync's carve-out — a device id is
        // meaningless on another machine). Each row writes an audio/* key and re-applies live via
        // applyAudioOutputLive(); every row has a classic twin in the QWidget builder below. ---
        sep(tr("Audio"));
        choice(QStringLiteral("audio.device"), tr("Output device"), audioDevOpts, curAudioDevDisp);
        toggle(QStringLiteral("audio.passthrough"), tr("Passthrough (bitstream to receiver)"), Settings::audioPassthrough());
        toggle(QStringLiteral("audio.exclusive"), tr("Exclusive mode (bit-perfect)"), Settings::audioExclusive());
        info(QStringLiteral("audio.hint"),
             tr("Passthrough sends Dolby/DTS untouched to an AV receiver instead of decoding to stereo — while "
                "it is on, volume boost and pitch-corrected speed don't apply. Exclusive mode takes sole control "
                "of the device for bit-perfect output. Both apply to the next audio you play."), QString());
        // --- Subtitles ---
        sep(tr("Subtitles"));
        toggle(QStringLiteral("subs.on"), tr("Show subtitles by default"), Settings::subtitlesOnByDefault());
        // Subtitle appearance (issue #71). Applies to mpv's un-styled (SRT/text) renderer; styled ASS/SSA subs
        // keep their own typography unless "Override styled" is on. Every row writes a subs/* Settings key and
        // re-applies live via applySubtitleStyleLive(); each has a classic twin in the QWidget builder below.
        choice(QStringLiteral("subs.font"),        tr("Font"),                subFontOpts,        curSubFontDisp);
        choice(QStringLiteral("subs.size"),        tr("Size"),                subSizeOpts,        curSubSizeDisp);
        choice(QStringLiteral("subs.color"),       tr("Text colour"),         subTextColorOpts,   curSubColorDisp);
        choice(QStringLiteral("subs.bordersize"),  tr("Outline thickness"),   subBorderOpts,      curSubBorderDisp);
        choice(QStringLiteral("subs.bordercolor"), tr("Outline colour"),      subBorderColorOpts, curSubBorderColorDisp);
        toggle(QStringLiteral("subs.box"),         tr("Show a background box behind subtitles"), Settings::subtitleBox());
        choice(QStringLiteral("subs.boxopacity"),  tr("Background box opacity"), subOpacityOpts,  curSubOpacityDisp);
        choice(QStringLiteral("subs.pos"),         tr("Vertical position"),   subPosOpts,         curSubPosDisp);
        toggle(QStringLiteral("subs.bold"),        tr("Bold subtitles"),      Settings::subtitleBold());
        toggle(QStringLiteral("subs.override"),    tr("Override styled (ASS/SSA) subtitles"), Settings::subtitleOverrideStyled());
        info(QStringLiteral("subs.stylehint"),
             tr("Font, colour, outline, box and position apply to plain-text (SRT) subtitles. Styled subtitles "
                "keep their own look unless you override them."), QString());
        // --- Auto-download from OpenSubtitles (the password is masked — dots in the row; the OSK is unchanged) ---
        sep(tr("Auto-download from OpenSubtitles"));
        textf(QStringLiteral("os.api"), tr("API key"), Settings::openSubApiKey());
        builtinHint(SubtitleFetcher::usingBuiltinKey());
        textf(QStringLiteral("os.user"), tr("Username"), Settings::openSubUsername());
        textf(QStringLiteral("os.pass"), tr("Password"), Settings::openSubPassword(), /*masked=*/true);
        // --- Reading (issue #135). Font / size / line spacing / margins / justify / reading theme for the ebook
        // reader (EbookView), applied to its QTextDocument + page chrome via ReaderTypography::resolve. Each row
        // writes a reader/* Settings key (size shares the ebook/fontSize key the in-reader A+/A− stepper drives)
        // and re-applies live via applyReaderTypographyLive(); each has a classic twin in the QWidget builder. ---
        sep(tr("Reading"));
        choice(QStringLiteral("reader.font"),    tr("Font"),          readerFontOpts,    curReaderFontDisp);
        choice(QStringLiteral("reader.size"),    tr("Size"),          readerSizeOpts,    curReaderSizeDisp);
        choice(QStringLiteral("reader.spacing"), tr("Line spacing"),  readerSpacingOpts, curReaderSpacingDisp);
        choice(QStringLiteral("reader.margin"),  tr("Margins"),       readerMarginOpts,  curReaderMarginDisp);
        toggle(QStringLiteral("reader.justify"), tr("Justify text"),  Settings::readerJustify());
        choice(QStringLiteral("reader.theme"),   tr("Reading theme"), readerThemeOpts,   curReaderThemeDisp);
        info(QStringLiteral("reader.hint"),
             tr("Font, size, spacing, margins and theme for ebooks. Changes reflow the page but keep your place."),
             QString());
        // --- In-book lookup (issue #137). Define and Wikipedia need no configuration at all, so the only
        // control here is WHERE to translate; leaving it empty removes the Translate verb from the reader
        // rather than leaving it there to fail. The word list is an action row for #161's reason — it is a
        // LIST, and a list wants its own surface, not a panel of rows. The hint is a PRIVACY statement, and it
        // is deliberately blunt: a lookup is a network call carrying the words you selected. Classic twins are
        // in the QWidget builder below (GS_TWINS). ---
        textf(QStringLiteral("reader.translate"), tr("Translation service (LibreTranslate address)"),
              Settings::readerTranslateEndpoint());
        action(QStringLiteral("reader.vocab"),
               vocabularyWordCount() > 0
                   ? tr("Words I looked up (%n)", "", vocabularyWordCount())
                   : tr("Words I looked up"));
        info(QStringLiteral("reader.lookuphint"),
             tr("Select a word while reading and the menu offers Define, Wikipedia and Translate. Each one is "
                "a NETWORK REQUEST that sends the words you selected — to Wiktionary, to Wikipedia, or to the "
                "translation service you name above. Nothing is ever looked up just because you selected it: "
                "it takes pressing one of those three. Words you look up are kept in the list above, on this "
                "profile, and sync with your other devices. Leave the translation address empty and the "
                "Translate verb is not offered at all."),
             QString());
        // --- Touch reading (issue #147). The tap-zone preset, swipe paging and keep-awake are inert unless
        // this device reports a touch form factor (ReaderGestureConfig applies the same FormFactor gate the
        // video player's gestures do), which is also what keeps a phone's preset out of the TV profile; the
        // hint says so outright rather than hiding the rows, so a phone-and-television household can still
        // find them from either. Dual-page landscape is NOT gated: a wide window is a wide window whether
        // you are holding it or not. Classic twins are in the QWidget builder below (GS_TWINS). ---
        choice(QStringLiteral("reader.zones"), tr("Tap zones"), readerZoneOpts, curReaderZoneDisp);
        toggle(QStringLiteral("reader.swipe"), tr("Swipe sideways to turn the page"),
               Settings::readerSwipePaging());
        toggle(QStringLiteral("reader.keepawake"), tr("Keep the screen on while reading"),
               Settings::readerKeepAwake());
        toggle(QStringLiteral("reader.dualpage"), tr("Two pages side by side on a wide screen"),
               Settings::readerDualPage());
        info(QStringLiteral("reader.touchhint"),
             tr("Tap zones and swiping apply on touch screens only — a mouse, a keyboard and a remote behave "
                "exactly as they always have, and a tap across the top of the page always opens the menu. "
                "Two pages side by side applies to books whenever the window is wider than it is tall."),
             QString());
        // Read aloud (issue #145). An INFO row, not a control: the things you actually set — start, speed,
        // voice — belong in the reader, where you are when you want them. What belongs here is the one thing
        // the reader cannot tell you in a button, and the thing users are otherwise left to discover by being
        // disappointed: the voices are the operating system's, and how good they are is the operating
        // system's business, not this app's. Its classic twin is readAloudNote below.
        info(QStringLiteral("readaloud.hint"),
             tr("Read aloud (in the reader's controls) speaks with your system's own voices — add more in your "
                "operating system's speech settings. Quality varies a lot by platform; nothing is downloaded "
                "and no voice is bundled."),
             QString());
        // --- Trakt.tv ---
        sep(tr("Trakt.tv"));
        textf(QStringLiteral("trakt.id"), tr("Client ID"), Settings::traktClientId());
        builtinHint(TraktClient::usingBuiltin());
        textf(QStringLiteral("trakt.secret"), tr("Client secret"), Settings::traktClientSecret(), /*masked=*/true);
        builtinHint(TraktClient::usingBuiltin() && !TraktClient::appCredentials().secret.isEmpty());
        action(QStringLiteral("trakt.connect"), TraktClient::connected() ? tr("Disconnect from Trakt")
                                                                          : tr("Connect to Trakt"));
        // The watched-history import (#23). An ACTION, not a toggle: it is something the user asks for
        // once after linking, and it reports what it did. Its twin lives in the QWidget builder below —
        // a user-facing setting has to exist in BOTH or it is unreachable in one of the two modes.
        action(QStringLiteral("trakt.backfill"), tr("Import watched history from Trakt"));
        // The escape hatch the incremental rule needs, and its twin lives in the QWidget builder below.
        // Separate from the row above ON PURPOSE: this one can re-mark something the user unmarked, so
        // it asks first and must never be reachable by pressing the ordinary import twice.
        action(QStringLiteral("trakt.reimport"), tr("Re-import everything from Trakt"));
        // What the ordinary import will and will not consider, in one line — the ONLY place the
        // watermark is visible at all. Without it "finished: 0 newly marked" is indistinguishable from
        // "nothing to do", which is exactly how a mis-scoped cursor hides.
        info(QStringLiteral("trakt.data"), tr("Trakt data"), traktStatusLine());
        info(QStringLiteral("trakt.status"), tr("Status"), TraktClient::connected() ? tr("Connected")
                                                                                     : tr("Not connected"));
        // --- AniList (issue #156): anime + manga progress. Trakt above keeps film and general TV; this
        // keeps anime and manga, and the two never write to each other. Every row has a twin in the
        // QWidget builder below - a setting in one builder is unreachable in the other mode. ---
        // THE NEWS THAT WAITED (issue #328), taken before the first status line below is built: an update a
        // service refused permanently was dropped while nobody was looking, and this is where it is said.
        trackerPanelOpened();
        sep(tr("AniList (anime and manga)"));
        info(QStringLiteral("anilist.help"),
             tr("Sync chapters read and episodes watched to your AniList list. Create a free API client at "
                "anilist.co (Settings > Developer > Create New Client), set its redirect URL to "
                "the loopback address 127.0.0.1 , paste the Client ID + Secret below, then Connect."), QString());
        // The TYPED values (#81): clientId() would read a built-in client back out into the row.
        textf(QStringLiteral("anilist.id"), tr("Client ID"), AniListTracker::typedClientId());
        builtinHint(AniListTracker::usingBuiltin());
        // MASKED. It is the user's own OAuth secret and the row must not read it back out on a TV in a
        // living room. It is also carved out of the sync bundle entirely (CloudSync::isDeviceLocalKey).
        textf(QStringLiteral("anilist.secret"), tr("Client secret"), AniListTracker::typedClientSecret(),
              /*masked=*/true);
        builtinHint(AniListTracker::usingBuiltin() && !AniListTracker::clientSecret().isEmpty());
        action(QStringLiteral("anilist.connect"), AniListTracker::isConnected()
                   ? tr("Disconnect from AniList") : tr("Connect to AniList"));
        // The queue depth is the only thing that distinguishes "connected and delivering" from "connected
        // and silently accumulating"; without it a broken push looks exactly like nothing to push.
        info(QStringLiteral("anilist.data"), tr("AniList"), anilistStatusLine());
        info(QStringLiteral("anilist.status"), tr("Status"), AniListTracker::isConnected()
                   ? tr("Connected") : tr("Not connected"));
        // --- MyAnimeList (issue #156, increment 2): the SAME rows for the second tracker. Every row has a
        // twin in the QWidget builder below - a setting in one builder is unreachable in the other mode.
        // Connect BOTH and every finished chapter goes to both lists, each with its own link and its own
        // queue; neither one failing stops the other. ---
        sep(tr("MyAnimeList (anime and manga)"));
        info(QStringLiteral("mal.help"),
             tr("Sync chapters read and episodes watched to your MyAnimeList list. Create a free API client "
                "at myanimelist.net (Account Settings > API > Create ID), set its redirect URL to the "
                "loopback address http://127.0.0.1 , paste the Client ID below, then Connect. Leave the "
                "secret empty if your client has none."), QString());
        textf(QStringLiteral("mal.id"), tr("Client ID"), MyAnimeListTracker::typedClientId());
        builtinHint(MyAnimeListTracker::usingBuiltin());
        // MASKED, for the reason AniList's is: it is the user's own OAuth secret and the row must not read
        // it back out on a TV in a living room. Carved out of the sync bundle entirely. The built-in client is a
        // public one with no secret, so this row never says "Built in".
        textf(QStringLiteral("mal.secret"), tr("Client secret (optional)"), MyAnimeListTracker::typedClientSecret(),
              /*masked=*/true);
        builtinHint(MyAnimeListTracker::usingBuiltin() && !MyAnimeListTracker::clientSecret().isEmpty());
        action(QStringLiteral("mal.connect"), MyAnimeListTracker::isConnected()
                   ? tr("Disconnect from MyAnimeList") : tr("Connect to MyAnimeList"));
        info(QStringLiteral("mal.data"), tr("MyAnimeList"), malStatusLine());
        info(QStringLiteral("mal.status"), tr("Status"), MyAnimeListTracker::isConnected()
                   ? tr("Connected") : tr("Not connected"));
        // --- Kitsu (issue #156, increment 3): the SAME rows again for the third tracker, and the
        // shape is the same because the seam is. The ONE difference a user sees is that there is
        // nothing to register: Kitsu signs in with the account's own email and password, so these two
        // rows are credentials rather than a client id, and NEITHER is ever written to disk — they are
        // held in memory for the length of one sign-in and cleared by it (see KitsuTracker.h). The
        // password row is therefore built EMPTY every time: there is no stored value to read back.
        // Every row has a twin in the QWidget builder below. ---
        sep(tr("Kitsu (anime and manga)"));
        info(QStringLiteral("kitsu.help"),
             tr("Sync chapters read and episodes watched to your Kitsu list. There is nothing to "
                "register: type the email and password of your Kitsu account below and press Sign in. "
                "Your password is used once to sign in and is never stored on this device."), QString());
        textf(QStringLiteral("kitsu.email"), tr("Kitsu email"), KitsuTracker::email());
        // MASKED, and built from an EMPTY string rather than from a getter — there deliberately is no
        // getter for it. A masked field that reads its own value back is one screenshot away from
        // being read out loud on a television.
        textf(QStringLiteral("kitsu.password"), tr("Kitsu password"), QString(), /*masked=*/true);
        action(QStringLiteral("kitsu.connect"), KitsuTracker::isConnected()
                   ? tr("Sign out of Kitsu") : tr("Sign in to Kitsu"));
        info(QStringLiteral("kitsu.data"), tr("Kitsu"), kitsuStatusLine());
        info(QStringLiteral("kitsu.status"), tr("Status"), KitsuTracker::isConnected()
                   ? tr("Connected") : tr("Not connected"));
        // --- Music scrobbling (issue #192) ---
        // The twin of every row here lives in the QWidget builder below; a setting in one builder is simply
        // unreachable in the other mode. OFF by default and gated on a token: this sends what somebody listens
        // to, by name, to a third party, so it is opted into twice over.
        sep(tr("Music scrobbling"));
        toggle(QStringLiteral("scrobble.on"), tr("Scrobble music I listen to"), Settings::scrobbleEnabled());
        // MASKED. It is the user's own secret and the row must not read it back out on a TV in a living room.
        textf(QStringLiteral("scrobble.lbtoken"), tr("ListenBrainz user token"),
              Settings::listenBrainzToken(), /*masked=*/true);
        // Empty means the public ListenBrainz service. A URL here transparently covers Maloja and the other
        // servers that implement the same endpoint, which is why it is a setting and not a second provider.
        textf(QStringLiteral("scrobble.lburl"), tr("Custom API URL"), Settings::listenBrainzApiUrl());
        // LAST.FM (#192 increment 2): a connect/disconnect ACTION rather than a credential field, because
        // there is no credential for the user to type — the application key is baked in and the account is
        // authorised in a browser. Its twin lives in the QWidget builder below. Shown even when this build
        // has no key: the row is what SAYS so, and hiding it would leave "why is Last.fm missing" unanswered.
        action(QStringLiteral("scrobble.lastfm"), LastFmClient::connectActionLabel());
        info(QStringLiteral("scrobble.lastfmstatus"), tr("Last.fm"), LastFmClient::statusText());
        toggle(QStringLiteral("scrobble.spoken"), tr("Also scrobble audiobooks and podcasts"),
               Settings::scrobbleSpokenAudio());
        // THE DOUBLE-COUNT COORDINATION (#193), and it is ONE setting on purpose — the issue's requirement
        // is "a single clear setting rather than two that silently conflict". Its twin is in the QWidget
        // builder below. The label asks the question and the hint under it says what happens either way, so
        // nobody has to reason it out: a setting whose two positions are not both spelled out is a setting
        // people leave alone.
        toggle(QStringLiteral("scrobble.serverforwards"),
               tr("My music server scrobbles for me"), Settings::scrobbleServerForwards());
        info(QStringLiteral("scrobble.serverhint"),
             tr("Turn this on if Navidrome (or Airsonic, or Gonic) is signed in to Last.fm or ListenBrainz "
                "itself. Off, a track played from a music server is reported to the server AND to the "
                "services above, which is right when the server forwards nothing. On, it is reported to the "
                "server only, and the server passes it on — otherwise every one of those plays is counted "
                "twice. Music on this device is unaffected either way."), QString());
        // THE CONFIDENCE INDICATOR, and the reason it exists is in the issue: scrobbling that silently stops
        // working is the classic complaint about every client that has implemented it. One line, from one
        // builder, shown by both surfaces — a number that grows, or the reason it does not.
        info(QStringLiteral("scrobble.hint"),
             tr("A track is scrobbled once you have played half of it, or four minutes, whichever comes "
                "first. Leave the custom API URL empty for ListenBrainz itself, or point it at a compatible "
                "server such as Maloja. Last.fm is a second destination rather than an alternative: connect "
                "it as well and every listen goes to both, each with its own queue."), QString());
        info(QStringLiteral("scrobble.status"), tr("Scrobbling"), scrobbleStatusLine());
        // --- Discord Rich Presence ---
        // The twin of every row here lives in the QWidget builder below; a setting in one builder is simply
        // unreachable in the other mode. OFF by default: this announces what somebody is watching, by name,
        // to everyone who can see their Discord profile, so it is asked for rather than assumed.
        sep(tr("Discord"));
        toggle(QStringLiteral("discord.on"), tr("Show what I'm doing on Discord"), Settings::discordEnabled());
        toggle(QStringLiteral("discord.movies"),   tr("Movies and TV"),        Settings::discordMovies());
        toggle(QStringLiteral("discord.games"),    tr("Games"),                Settings::discordGames());
        toggle(QStringLiteral("discord.music"),    tr("Music and audiobooks"), Settings::discordMusic());
        toggle(QStringLiteral("discord.reading"),  tr("Books and comics"),     Settings::discordReading());
        toggle(QStringLiteral("discord.livetv"),   tr("Live TV"),              Settings::discordLiveTv());
        toggle(QStringLiteral("discord.browsing"), tr("Just browsing"),        Settings::discordBrowsing());
        info(QStringLiteral("discord.hint"),
             tr("Your Discord profile shows what you're watching, playing or reading, with its artwork. "
                "Each category can be silenced on its own, and this machine's choice is its own — turning "
                "it on here doesn't turn it on anywhere else."), QString());
        info(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
        // --- Profiles (issue #30) ---
        // The ONE escape hatch from always-ask. Phrased as the opt-out it is, so the default reads as the
        // behaviour rather than as a feature someone has to find. Must exist in the classic builder too —
        // see the twin below; a setting in one builder is unreachable in the other mode.
        sep(tr("Profiles"));
        toggle(QStringLiteral("profiles.skipsingle"), tr("Skip the profile picker when there's only one profile"),
               Settings::skipProfilePickerWhenSingle());
        info(QStringLiteral("profiles.skipnote"), tr("Note"),
             tr("Ignored while that profile has a passcode."));
        // --- Parental Controls (the PIN flows keep Osk::getText nesting exactly) ---
        sep(tr("Parental Controls"));
        info(QStringLiteral("parental.status"), tr("PIN"), Settings::hasParentalPin() ? tr("A PIN is set")
                                                                                       : tr("No PIN set"));
        action(QStringLiteral("parental.setpin"), Settings::hasParentalPin() ? tr("Change PIN") : tr("Set PIN"));
        action(QStringLiteral("parental.clearpin"), tr("Remove PIN"));
        info(QStringLiteral("parental.profileshdr"), tr("Restricted (kids) profiles"), QString());
        for (const Profile& pr : ProfileStore::list())
            toggle(QStringLiteral("profile:") + pr.id,
                   (pr.icon.isEmpty() ? QString() : pr.icon + QStringLiteral("  ")) + pr.name, pr.restricted);
        // --- Background Music ---
        sep(tr("Background Music"));
        toggle(QStringLiteral("bgm.on"), tr("Play background music"), Settings::bgmEnabled());
        choice(QStringLiteral("bgm.vol"), tr("Volume"), volOpts, curVolDisp);
        action(QStringLiteral("bgm.open"), tr("Open music folder"));
        // --- Video previews (issue #55) --- twin lives in the QWidget builder below; a setting in one builder
        // is unreachable in the other mode (GS_TWINS gate). ON by default (previews are the intended browse
        // experience) but the snap volume defaults to 0 (muted), so it never ducks the music until the user asks.
        sep(tr("Video previews"));
        toggle(QStringLiteral("video.previews"), tr("Play video previews on hover"), Settings::videoPreviewsEnabled());
        choice(QStringLiteral("video.snapvol"), tr("Preview volume"), volOpts, curSnapVolDisp);
        // --- Steam (achievements + owned library) ---
        // One Steam Web API key serves both PC-game achievements and the owned-not-installed library on the Steam
        // console; the SteamID (64-bit) enables the latter. Key is MASKED. (Owned-games UI lives here on General
        // rather than an addon-config surface: it's a native feature, not a manifest-driven addon setting.)
        sep(tr("Steam (achievements + owned library)"));
        textf(QStringLiteral("steam.key"), tr("Steam Web API key"),
              Settings::steamWebApiKey(), /*masked=*/true);
        textf(QStringLiteral("steam.steamid"), tr("SteamID (64-bit) — shows owned, not-installed games"),
              Settings::steamId());
        // --- Epic without the Epic launcher (issue #118) --- twin lives in the QWidget builder below.
        // legendary is a third-party CLI we SHELL OUT to; it is detected, never installed by us, so the
        // status row's job is to be honest about which of the three states this machine is in.
        sep(tr("Epic Games (legendary)"));
        info(QStringLiteral("epic.legendary.status"), tr("legendary"), legendaryStatusLine());
        action(QStringLiteral("epic.legendary.auth"), tr("Sign in to Epic (legendary)"));
        action(QStringLiteral("epic.legendary.get"), tr("Get legendary"));
        // --- Streaming (Debrid) ---
        sep(tr("Streaming (Debrid)"));
        textf(QStringLiteral("debrid.torbox"), tr("TorBox API key"),
              store().value(QStringLiteral("debrid/torbox/apikey")).toString());
        // --- Community ---
        sep(tr("Community"));
        action(QStringLiteral("community.discord"), tr("Join the Discord"));
        action(QStringLiteral("community.patreon"), tr("Support on Patreon"));

        // Small helpers to patch a status Info / an Action label in place (updateRow replaces the whole row).
        auto setInfo = [this](const QString& id, const QString& caption, const QString& value) {
            PanelRow r; r.kind = PanelRow::Info; r.id = id; r.label = caption; r.value = value;
            themedPanelHost_->updateRow(id, r); };
        auto setAction = [this](const QString& id, const QString& label) {
            PanelRow r; r.kind = PanelRow::Action; r.id = id; r.label = label;
            themedPanelHost_->updateRow(id, r); };

        // The Trakt status line is re-read from here after an import lands or the link changes, so the
        // row cannot go on claiming a watermark that has since moved. Safe to leave installed after the
        // panel is replaced: ThemedPanelHost::updateRow is a no-op for a row id no live panel holds,
        // and the host itself outlives every presentation.
        traktStatusUpdate_ = [this, setInfo] {
            setInfo(QStringLiteral("trakt.data"), tr("Trakt data"), traktStatusLine()); };

        // ...and the same for the AniList line (#156), which moves on its own: a queued chapter delivered
        // by the retry timer while this panel is up must move the number the user is looking at.
        anilistStatusUpdate_ = [this, setInfo] {
            setInfo(QStringLiteral("anilist.data"), tr("AniList"), anilistStatusLine()); };
        // ...and MyAnimeList's, which moves on its own for the same reason and on its own queue.
        malStatusUpdate_ = [this, setInfo] {
            setInfo(QStringLiteral("mal.data"), tr("MyAnimeList"), malStatusLine()); };
        // ...and Kitsu's, third queue, same reason.
        kitsuStatusUpdate_ = [this, setInfo] {
            setInfo(QStringLiteral("kitsu.data"), tr("Kitsu"), kitsuStatusLine()); };

        // ...and the same for the scrobble line (#192), which moves on its own: a listen delivered by the
        // background pump while this panel is up must move the number the user is looking at.
        scrobbleStatusUpdate_ = [this, setInfo] {
            setInfo(QStringLiteral("scrobble.status"), tr("Scrobbling"), scrobbleStatusLine()); };

        // ...and the Jellyfin server line (#160). Load-bearing rather than cosmetic: connecting a server is
        // TWO network round trips, so the row's own handler returns long before the server exists and a line
        // refreshed there would still say "1 server" after the user has just been told a second one is
        // connected. Driven from the store's change hook instead, which is the moment the answer changes.
        jellyfinStatusUpdate_ = [this, setInfo] {
            setInfo(QStringLiteral("jellyfin.serverstatus"), tr("Jellyfin"), jellyfinServerStatusLine()); };

        themedPanelHost_->present(tr("General"), rows,
            [this, dlCapPairs, langOptPairs, playerOptPairs, hwdecPairs, hdrPairs, defSpeedPairs, jumpPairs, gestEdgePairs, attractTimeoutPairs, resumeModePairs,
             previewCachePairs,        // Seek previews (#85): same, for the preview-cache size row
             followIntervalPairs,      // Following (#155): the handler maps the picked display back through them
             rgPairs, rgPreampPairs,   // ReplayGain (#141): the handler maps the picked display back through them
             xfPairs,                  // Crossfade (#141): same, for the seconds row
             wtPairs,                  // Watch together (#86): same, for the stall-policy row
             musicSrcPairs,            // Preferred music source (#194): same, for the "Play music from" row
             shaderPresetPairs,
             runaheadPairs,            // Runahead (#100): the handler maps the picked display back to N
             dosMidiPairs,             // MS-DOS MIDI device (#191): the handler maps the picked display back
#ifdef EB_HAVE_RETROPARK
             rpDrivenBackendPairs,
#endif
             subColorPairs, subPosOptPairs, audioDevPairs, readerThemePairs, readerZonePairs, setInfo, setAction](const QString& id, const QString& val) {
                const bool on = (val == QStringLiteral("1"));   // Toggle rows deliver "1"/"0"
                if (id == QStringLiteral("disp.fullscreen")) {
                    Settings::setStartFullscreen(on);
                    if (on) showFullScreen(); else if (isFullScreen()) leaveFullScreen();
                }
                else if (id == QStringLiteral("attract.enabled")) {
                    Settings::setAttractEnabled(on);
                    applyAttractConfig();
                }
                else if (id == QStringLiteral("attract.timeout")) {
                    for (const auto& a : attractTimeoutPairs) if (a.first == val) { Settings::setAttractTimeoutMinutes(a.second); break; }
                    applyAttractConfig();
                }
                // #110: the two download-hygiene rows. Same setters the QWidget twins call — one write path.
                else if (id == QStringLiteral("downloads.cap")) {
                    for (const auto& c : dlCapPairs) if (c.first == val) { JellyfinDownload::setCapGb(c.second); break; }
                    checkJellyfinDownloadCap();   // a tighter limit may make this true immediately
                }
                else if (id == QStringLiteral("downloads.removewatched")) JellyfinDownload::setRemoveAfterWatched(on);
                // #160: the same setter the QWidget twin calls — one write path. The store's change hook
                // re-renders the home, so the sections re-shape without a restart.
                else if (id == QStringLiteral("jellyfin.continuemerge")) JellyfinServerStore::setContinueMerged(on);
                else if (id == QStringLiteral("emu.autoinc")) Settings::setStateAutoIncrement(on);
                else if (id == QStringLiteral("emu.resume")) {
                    for (const auto& r : resumeModePairs) if (r.first == val) { Settings::setResumeMode(r.second); break; }
                }
                else if (id == QStringLiteral("emu.shaderpreset")) {
                    for (const auto& p : shaderPresetPairs) if (p.first == val) { Settings::setShaderPreset(p.second); break; }
                }
                else if (id == QStringLiteral("emu.runahead")) {
                    for (const auto& r : runaheadPairs) if (r.first == val) { Settings::setRunaheadFrames(r.second); break; }
                }
#ifdef EB_HAVE_RETROPARK
                else if (id == QStringLiteral("emu.rpdriven")) {
                    for (const auto& p : rpDrivenBackendPairs) if (p.first == val) { Settings::setRetroParkDrivenBackend(p.second); break; }
                }
#endif
                else if (id == QStringLiteral("emu.dosmidi")) {
                    for (const auto& p : dosMidiPairs) if (p.first == val) { Settings::setDosMidiDevice(p.second); break; }
                }
                else if (id == QStringLiteral("emu.hardcore")) {
                    // Hardcore (#94): enabling needs consent (it disables the emulator's comforts and resets the
                    // achievement session). The row already flipped visually; only persist + apply on confirm,
                    // else put it back. Disabling just drops to softcore, no consent needed.
                    if (on) {
                        const int r = NavConfirm::ask(tr("Enable hardcore mode?"),
                            tr("Hardcore disables save states, rewind, fast-forward and cheats. Enabling resets "
                               "your current achievement session. Continue?"),
                            { tr("Enable"), tr("Cancel") }, 0, 1, this);
                        if (r == 0) { Settings::setHardcoreAchievements(true); if (ach_) ach_->setHardcore(true); }
                        else {
                            PanelRow rr; rr.kind = PanelRow::Toggle; rr.id = id;
                            rr.label = tr("Hardcore RetroAchievements (no save states, rewind or cheats)");
                            rr.checked = false;
                            themedPanelHost_->updateRow(id, rr);
                        }
                    } else {
                        Settings::setHardcoreAchievements(false); if (ach_) ach_->setHardcore(false);
                    }
                }
                else if (id == QStringLiteral("community.discord")) {
                    // Outward navigation to the browser — same idiom as Appearance's theme-gallery row.
                    QDesktopServices::openUrl(QUrl(QString::fromLatin1(kDiscordInvite)));
                }
                else if (id == QStringLiteral("community.patreon")) {
                    QDesktopServices::openUrl(QUrl(QString::fromLatin1(kPatreonUrl)));
                }
                // Store backends (#118). Both rows are defined in src/ui/MainWindowStoreBackend.cpp; the
                // sign-in hands its own status updater in, so the row a person is looking at is the row that
                // changes when the sign-in settles.
                else if (id == QStringLiteral("epic.legendary.auth")) {
                    promptLegendaryAuth([this, setInfo](const QString& line) {
                        setInfo(QStringLiteral("epic.legendary.status"), tr("legendary"), line); });
                }
                else if (id == QStringLiteral("epic.legendary.get")) {
                    openLegendaryReleases();
                }
                else if (id == QStringLiteral("lib.showhidden")) {
                    store().setValue(QStringLiteral("library/showHidden"), on);
                    store().sync();                 // flush so HomeView's QSettings sees it on the refresh below
                    home_->reloadForFilterChange(); // hidden rows appear/disappear on the live surface at once
                }
                // Following (issue #155). Same Settings key, same setter and the same live-scheduler push as
                // the classic twins below — one write path, no drift (GS_TWINS).
                else if (id == QStringLiteral("following.interval")) {
                    for (const auto& f : followIntervalPairs) if (f.first == val) {
                        Settings::setFollowIntervalHours(f.second);
                        if (followSched_) followSched_->setIntervalHours(f.second);
                        break;
                    }
                }
                else if (id == QStringLiteral("following.metered")) {
                    Settings::setFollowOnMetered(on);
                    if (followSched_) followSched_->setAllowMetered(on);
                }
                else if (id == QStringLiteral("following.notify")) {
                    setFollowNotifyFromUi(on);
                }
                else if (id == QStringLiteral("following.check")) {
                    // The row below follows the scheduler's status (setupFollowStatus) - start, ignore, finish.
                    followUserCheckNow(false);
                }
                else if (id == QStringLiteral("update.autocheck")) Settings::setCheckUpdatesOnStartup(on);
                else if (id == QStringLiteral("remote.enabled")) {
                    Settings::setRemoteControlEnabled(on);
                    updateRemoteServer();            // start/stop the server right away
                    // Reflect the reachable URL (or the off state) on the row below without rebuilding the panel.
                    setInfo(QStringLiteral("remote.url"), tr("Open on your phone"),
                            on ? RemoteServer::lanUrl(static_cast<quint16>(Settings::remoteControlPort()))
                               : tr("Turn on to get a URL"));
                }
                else if (id == QStringLiteral("deeplink.enabled")) setDeepLinksFromUi(on);   // #80
                else if (id == QStringLiteral("remote.filedrop")) {
                    setFileDropFromUi(on);             // #115: starts the listener if needed, and says so
                    setInfo(QStringLiteral("remote.filedrop.url"), tr("Open in a browser"), fileDropStatusText());
                }
                else if (id == QStringLiteral("update.check")) {
                    if (!updater_) return;
                    setInfo(QStringLiteral("update.status"), tr("Status"), tr("Checking…"));
                    connect(updater_, &AppUpdater::updateAvailable, this,
                        [this, setInfo, setAction](const QString& ver, const QString&) {
                            setInfo(QStringLiteral("update.status"), tr("Status"), tr("Version %1 is available.").arg(ver));
                            setAction(QStringLiteral("update.install"), tr("Install %1 and restart").arg(ver));
                        }, Qt::SingleShotConnection);
                    connect(updater_, &AppUpdater::upToDate, this, [this, setInfo] {
                        setInfo(QStringLiteral("update.status"), tr("Status"), tr("You're already on the latest version."));
                    }, Qt::SingleShotConnection);
                    connect(updater_, &AppUpdater::checkFailed, this, [this, setInfo](const QString& why) {
                        setInfo(QStringLiteral("update.status"), tr("Status"), tr("Couldn't check for updates: %1").arg(why));
                    }, Qt::SingleShotConnection);
                    updater_->checkForUpdate();
                }
                else if (id == QStringLiteral("update.install")) {
                    if (updater_ && updater_->updatePending()) {
                        setInfo(QStringLiteral("update.status"), tr("Status"),
                                tr("Downloading and installing… the app will restart."));
                        updater_->downloadAndApply();
                    } else {
                        setInfo(QStringLiteral("update.status"), tr("Status"), tr("No update ready — check first."));
                    }
                }
                else if (id == QStringLiteral("playon.rename")) {
                    const QString name = Osk::getText(tr("What should other devices call this one?"),
                                                      Settings::deviceName(), QLineEdit::Normal, this);
                    if (name.isNull()) return;
                    Settings::setDeviceName(name);
                    updatePlayOnAdvert();            // re-advertise under the new name at once
                    setInfo(QStringLiteral("playon.name"), tr("This device is called"), Settings::deviceName());
                }
                else if (id == QStringLiteral("playon.pick")) {
                    showPlayOnMenu();
                }
                // Watch together (issue #86). The policy is applied to a LIVE room as well as stored: a host
                // that switches to "keep going" while the room is waiting for a stalled guest expects it to
                // start moving, and the next stall report -- which is what would otherwise carry the change --
                // never comes if everyone has already recovered.
                else if (id == QStringLiteral("wt.open")) {
                    showWatchTogetherMenu();
                }
                else if (id == QStringLiteral("wt.buffering")) {
                    for (const auto& p : wtPairs) if (p.first == val) { Settings::setWatchTogetherPolicy(p.second); break; }
                    applyWatchTogetherPolicy();
                }
                else if (id == QStringLiteral("playon.sendlib")) {
                    showSendLibraryMenu();
                }
                else if (id == QStringLiteral("livetv.add")) {
                    if (!home_ || !home_->promptForLiveTvSource()) return;
                    setInfo(QStringLiteral("livetv.count"), tr("Sources"),
                            tr("%n saved", "", int(IptvSourceStore::list().size())));
                    statusBar()->showMessage(tr("Live TV source saved — it's under Video on the home screen."), 6000);
                }
                else if (id == QStringLiteral("roms.change")) {
                    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose the ROMs folder"),
                                                                          Settings::romsFolder());
                    if (dir.isEmpty()) return;
                    Settings::setRomsFolder(dir);
                    setInfo(QStringLiteral("roms.path"), dir, QString());
                    RomLibrary::ensureStructure();
                    const int added = RomLibrary::syncToDownloads();
                    statusBar()->showMessage(added > 0
                        ? tr("ROMs folder set to %1 — added %n game(s) to Downloaded.", "", added).arg(dir)
                        : tr("ROMs folder set to %1").arg(dir), 6000);
                }
                else if (id == QStringLiteral("home.rows")) {
                    // Deferred a turn: the editor spins NavMenu/NavConfirm nested loops, and this runs inside
                    // the themed panel's own activation. The deferPastQmlEmission discipline (crash #28) —
                    // the same shape music.addserver above uses, and `setInfo` is captured explicitly for the
                    // same reason (the dispatch lambda has no default capture).
                    QMetaObject::invokeMethod(this, [this, setInfo] {
                        openHomeRowsEditor();
                        setInfo(QStringLiteral("home.rows.state"), tr("Rows"),
                                HomeRowStore::isCustomised() ? tr("Customised") : tr("Default"));
                    }, Qt::QueuedConnection);
                }
                else if (id == QStringLiteral("roms.open")) {
                    RomLibrary::ensureStructure();
                    QDesktopServices::openUrl(QUrl::fromLocalFile(RomLibrary::root()));
                }
                else if (id == QStringLiteral("library.change")) {
                    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your local video library folder"),
                                                                          Settings::libraryFolder());
                    if (dir.isEmpty()) return;
                    Settings::setLibraryFolder(dir);
                    setInfo(QStringLiteral("library.path"), dir, QString());
                    rescanLocalLibrary();
                    statusBar()->showMessage(tr("Local Library folder set to %1 — rescanning…").arg(dir), 6000);
                }
                else if (id == QStringLiteral("library.rescan")) {
                    rescanLocalLibrary();
                    statusBar()->showMessage(tr("Rescanning your Local Library…"), 4000);
                }
                else if (id == QStringLiteral("chan.bumperchange")) {
                    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your channel bumper folder"),
                                                                          Settings::interstitialFolder());
                    if (dir.isEmpty()) return;
                    Settings::setInterstitialFolder(dir);
                    setInfo(QStringLiteral("chan.bumperpath"), dir, QString());
                    statusBar()->showMessage(tr("Channel bumpers will come from %1").arg(dir), 6000);
                }
                else if (id == QStringLiteral("chan.bumperclear")) {
                    Settings::setInterstitialFolder(QString());
                    setInfo(QStringLiteral("chan.bumperpath"), tr("No bumper folder set"), QString());
                    statusBar()->showMessage(tr("Channels will play no bumpers."), 6000);
                }
                else if (id == QStringLiteral("photos.change")) {
                    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your photo library folder"),
                                                                          Settings::photosFolder());
                    if (dir.isEmpty()) return;
                    Settings::setPhotosFolder(dir);
                    setInfo(QStringLiteral("photos.path"), dir, QString());
                    statusBar()->showMessage(tr("Photos folder set to %1").arg(dir), 6000);
                }
                else if (id == QStringLiteral("music.change")) {
                    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your music folder"),
                                                                          Settings::musicFolder());
                    if (dir.isEmpty()) return;
                    Settings::setMusicFolder(dir);
                    setInfo(QStringLiteral("music.path"), dir, QString());
                    rescanMusicLibrary();
                    statusBar()->showMessage(tr("Music folder set to %1 — scanning…").arg(dir), 6000);
                }
                else if (id == QStringLiteral("music.rescan")) {
                    rescanMusicLibrary();
                    statusBar()->showMessage(tr("Scanning your music…"), 4000);
                }
                else if (id == QStringLiteral("audiobooks.change")) {
                    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your audiobook folder"),
                                                                          Settings::audiobookFolder());
                    if (dir.isEmpty()) return;
                    Settings::setAudiobookFolder(dir);
                    setInfo(QStringLiteral("audiobooks.path"), dir, QString());
                    rescanAudiobookLibrary();
                    statusBar()->showMessage(tr("Audiobooks folder set to %1 — scanning…").arg(dir), 6000);
                }
                else if (id == QStringLiteral("audiobooks.rescan")) {
                    rescanAudiobookLibrary();
                    statusBar()->showMessage(tr("Scanning your audiobooks…"), 4000);
                }
                else if (id == QStringLiteral("audiobooks.addserver")) {
                    // Deferred a turn: the prompt spins Osk/NavConfirm nested loops, and this runs inside
                    // the themed panel's own activation. The deferPastQmlEmission discipline (crash #28).
                    // setInfo is the enclosing builder's own lambda, so it is captured explicitly (the
                    // dispatch lambda has no default capture on purpose - see the builder).
                    absServerStatusUpdate_ = [this, setInfo] {
                        setInfo(QStringLiteral("audiobooks.serverstatus"), tr("Audiobook servers"),
                                audiobookServerStatusLine());
                    };
                    QMetaObject::invokeMethod(this, [this] {
                        if (home_) home_->addAudiobookServerInteractive();
                    }, Qt::QueuedConnection);
                }
                else if (id == QStringLiteral("books.change")) {
                    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your books folder"),
                                                                          Settings::readingFolder());
                    if (dir.isEmpty()) return;
                    Settings::setReadingFolder(dir);
                    setInfo(QStringLiteral("books.path"), dir, QString());
                    rescanBookLibrary();
                    statusBar()->showMessage(tr("Books folder set to %1 — scanning…").arg(dir), 6000);
                }
                else if (id == QStringLiteral("books.rescan")) {
                    rescanBookLibrary();
                    statusBar()->showMessage(tr("Scanning your books…"), 4000);
                }
                else if (id == QStringLiteral("jellyfin.servers")) {
                    // Deferred a turn, for the same reason music.addserver is: the manager spins Osk /
                    // NavMenu / NavConfirm nested loops, and this runs inside the themed panel's own
                    // activation. The deferPastQmlEmission discipline (crash #28 / #211).
                    QMetaObject::invokeMethod(this, [this, setInfo] {
                        if (!home_) return;
                        home_->manageJellyfinServersInteractive();
                        setInfo(QStringLiteral("jellyfin.serverstatus"), tr("Jellyfin"),
                                jellyfinServerStatusLine());
                    }, Qt::QueuedConnection);
                }
                else if (id == QStringLiteral("requests.service")) {
                    // #109. Deferred a turn for the same reason the row above it is: the manager spins
                    // Osk / NavMenu / NavConfirm nested loops inside the themed panel's own activation.
                    // The status line is refreshed on the way back out AND from the store's change hook,
                    // because the verify step is asynchronous and the click alone would leave the line a
                    // service behind.
                    requestsStatusUpdate_ = [this, setInfo] {
                        setInfo(QStringLiteral("requests.status"), tr("Requests"),
                                JellyseerrStore::statusLine());
                    };
                    QMetaObject::invokeMethod(this, [this, setInfo] {
                        manageRequestServiceInteractive();
                        setInfo(QStringLiteral("requests.status"), tr("Requests"),
                                JellyseerrStore::statusLine());
                    }, Qt::QueuedConnection);
                }
                else if (id == QStringLiteral("music.addserver")) {
                    // Deferred a turn: the prompt spins Osk/NavConfirm nested loops, and this runs inside
                    // the themed panel's own activation. The deferPastQmlEmission discipline (crash #28).
                    // setInfo is the enclosing builder's own lambda, so it is captured explicitly (the
                    // dispatch lambda has no default capture on purpose - see the builder).
                    QMetaObject::invokeMethod(this, [this, setInfo] {
                        if (!home_) return;
                        home_->addMusicServerInteractive();
                        setInfo(QStringLiteral("music.serverstatus"), tr("Music servers"),
                                musicServerStatusLine());
                    }, Qt::QueuedConnection);
                }
                else if (id == QStringLiteral("music.streamquality")) {
                    // Mapped back through the same list, so only a listed rate is ever written. Nothing already
                    // playing is re-minted; the next queue streams at the new cap.
                    for (const auto& pr : subsonicStreamQualityPairs())
                        if (pr.first == val) { Settings::setSubsonicStreamMaxBitRate(pr.second); break; }
                }
                else if (id == QStringLiteral("music.prefsource")) {
                    // Map the picked DISPLAY back through the same list, so only a listed value is written.
                    for (const auto& pr : musicSrcPairs)
                        if (pr.first == val) { Settings::setMusicPreferredSource(pr.second); break; }
                    // The preference decides which copy a merged row is keyed and rendered from, so the music
                    // levels have to be rebuilt rather than left showing the previous pick.
                    if (home_) home_->refreshMusicLevels();
                }
                else if (id == QStringLiteral("music.clearmatches")) {
                    const int n = musicMatchOverrideCount();
                    if (n == 0) { statusBar()->showMessage(tr("You haven't corrected any music matches."), 4000); }
                    else {
                        clearMusicMatchOverrides();
                        setAction(QStringLiteral("music.clearmatches"),
                                  tr("Reset my music match corrections (%n)", nullptr, musicMatchOverrideCount()));
                        if (home_) home_->refreshMusicLevels();
                        statusBar()->showMessage(tr("Your music match corrections were reset."), 4000);
                    }
                }
                else if (id == QStringLiteral("music.separators")) {
                    // Same setter and same follow-up as the classic twin. The rescan is not optional: the
                    // separators only matter at tag-read time, and the scan skips unchanged files, so the
                    // library re-tags itself here (MusicLibrary.h's "seps" stamp) or not at all.
                    Settings::setMusicTagSeparators(val);
                    rescanMusicLibrary();
                    statusBar()->showMessage(tr("Re-reading your music tags…"), 4000);
                }
                else if (id == QStringLiteral("library.resolveonline")) {
                    Settings::setResolveOnline(on);
                    if (on && resolver_) resolver_->enqueue(LocalLibrary::index().all());
                }
                else if (id == QStringLiteral("library.rematch")) {
                    // With resolveOnline off, clearing the cache then enqueue() (which no-ops) would
                    // wipe every resolved id on the next rebuild and never re-resolve — badges vanish.
                    // Require the toggle on before clearing.
                    if (!Settings::resolveOnline()) {
                        statusBar()->showMessage(tr("Turn on \"Match local files to online catalogs\" first."), 4000);
                    } else {
                        if (resolver_) resolver_->clearCacheAndRequeue(LocalLibrary::index().all());
                        statusBar()->showMessage(tr("Re-matching your Local Library online…"), 4000);
                    }
                }
                else if (id == QStringLiteral("library.clearmetaedits")) {
                    const int n = MetaOverrides::count();
                    if (n == 0) { statusBar()->showMessage(tr("You haven't edited any item's info."), 4000); }
                    else {
                        // Destructive and library-wide, so it confirms with the count — the confirmDeleteProfile
                        // card shape (Cancel focused, Back = Cancel).
                        const int c = NavConfirm::ask(tr("Reset metadata edits"),
                            tr("Discard your info edits on %n item(s) and go back to what the scrapers found? "
                               "This can't be undone.", nullptr, n),
                            { tr("Cancel"), tr("Reset all") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
                        if (c == 1) {
                            MetaOverrides::clearAll();
                            if (home_) home_->refreshDetailMetaCard();
                            setAction(QStringLiteral("library.clearmetaedits"),
                                      tr("Reset my metadata edits (%n item(s))", nullptr, MetaOverrides::count()));
                            statusBar()->showMessage(tr("Your metadata edits were reset."), 4000);
                        }
                    }
                }
                else if (id == QStringLiteral("roms.keepscrape")) Settings::setKeepScrapedData(on);
                else if (id == QStringLiteral("roms.softpatch")) Settings::setAutoApplyRomPatches(on);
                else if (id == QStringLiteral("roms.verify")) Settings::setVerifyRoms(on);
                else if (id == QStringLiteral("roms.collapseregions")) Settings::setCollapseRegionalDuplicates(on);
                else if (id == QStringLiteral("roms.keepdownloads")) Settings::setKeepDownloadsInRoms(on);
                else if (id == QStringLiteral("ps3.autoupdate")) Settings::setPs3AutoUpdate(on);
                else if (id == QStringLiteral("content.autoinstall")) Settings::setInstallGameContent(on);
                else if (id == QStringLiteral("books.enrich")) {
                    Settings::setBooksEnrichOnline(on);
                    // Switching it ON is the moment to look, because the library is already scanned and the
                    // sweep after a scan has long since run. Switching it OFF cancels nothing in flight and
                    // needs to: the sweep's own gate is re-read per book, so what is queued stops asking.
                    if (on) sweepBookMetadata();
                }
                else if (id == QStringLiteral("pb.autonext")) Settings::setAutoplayNextEpisode(on);
                else if (id == QStringLiteral("pb.gapless")) Settings::setGaplessAudio(on);
                // ReplayGain (issue #141). Both rows re-apply live so a mode/preamp change is audible on the
                // track already playing rather than only on the next one.
                else if (id == QStringLiteral("pb.replaygain")) {
                    for (const auto& p : rgPairs) if (p.first == val) { Settings::setReplayGainMode(p.second); break; }
                    applyReplayGainLive();
                }
                else if (id == QStringLiteral("pb.rgpreamp")) {
                    for (const auto& p : rgPreampPairs) if (p.first == val) { Settings::setReplayGainPreamp(p.second); break; }
                    applyReplayGainLive();
                }
                // Crossfade (issue #141). No live re-apply, unlike the ReplayGain rows: the arming happens
                // when a queue STARTS (it has to, because it changes how the next track reaches mpv), so
                // turning it on mid-album cannot retro-fit an overlap onto a boundary gapless has already
                // been handed. The hint row says so rather than leaving the user to wonder.
                else if (id == QStringLiteral("pb.crossfade")) {
                    for (const auto& p : xfPairs) if (p.first == val) { Settings::setCrossfadeSeconds(p.second); break; }
                }
                // Online lyrics (issue #142). No live re-apply and none is owed: the lookup is keyed on a
                // TRACK CHANGE, so turning it on mid-song takes effect at the next track, and turning it off
                // cannot un-fetch what is already cached and on screen.
                else if (id == QStringLiteral("pb.onlinelyrics")) Settings::setOnlineLyrics(on);
                else if (id == QStringLiteral("pb.defaultspeed")) {
                    for (const auto& p : defSpeedPairs) if (p.first == val) { Settings::setDefaultPlaybackSpeed(p.second); break; }
                }
                else if (id == QStringLiteral("pb.jump")) {
                    for (const auto& p : jumpPairs) if (p.first == val) { Settings::setAudioJumpSeconds(p.second); break; }
                }
                // Touch gestures (issue #162). No live re-apply and none is owed: the recogniser rebuilds its
                // whole Config from these keys on the NEXT press (applyGestureConfig), so a row flipped here
                // is in force by the time a finger next touches the video.
                else if (id == QStringLiteral("gest.volume"))     Settings::setGestureVolume(on);
                else if (id == QStringLiteral("gest.brightness")) Settings::setGestureBrightness(on);
                else if (id == QStringLiteral("gest.seek"))       Settings::setGestureSeek(on);
                else if (id == QStringLiteral("gest.doubletap"))  Settings::setGestureDoubleTap(on);
                else if (id == QStringLiteral("gest.longpress"))  Settings::setGestureLongPress(on);
                else if (id == QStringLiteral("gest.pinch"))      Settings::setGesturePinch(on);
                // ---- THIS CHAIN IS SPLIT IN TWO ON PURPOSE (MSVC C1061) -----------------------------
                // Every branch above and below compares the SAME `id` against a distinct string literal and
                // there is no trailing `else`, so two independent chains behave exactly as one long one.
                // It is split because MSVC counts each `else if` as a nested block and refuses at ~128:
                // this handler had reached 125 branches, so the next feature to add a settings row simply
                // would not compile. Do not "tidy" this back into one chain — add new rows to the lower
                // half, and split again around 120. The real fix is issue #186, breaking up MainWindow.cpp;
                // this is the stop-gap that keeps the file compiling until then.
                if (id == QStringLiteral("gest.edge")) {
                    for (const auto& p : gestEdgePairs) if (p.first == val) { Settings::setGestureEdgeInset(p.second); break; }
                }
                else if (id == QStringLiteral("pb.skipseg")) Settings::setSkipSegments(on);
                else if (id == QStringLiteral("pb.skipsegauto")) Settings::setSkipSegmentsAuto(on);
                else if (id == QStringLiteral("pb.hwdec")) {
                    for (const auto& p : hwdecPairs) if (p.first == val) { Settings::setHwDecode(p.second); break; }
                }
                else if (id == QStringLiteral("pb.seekpreview")) {
                    for (const auto& p : previewCachePairs) if (p.first == val) { Settings::setPreviewCacheMb(p.second); break; }
                }
                else if (id == QStringLiteral("pb.seekpreviewidle")) Settings::setPreviewIdleScan(on);
                else if (id == QStringLiteral("player.external")) {
                    QString key = val;                              // map the picked display back to the stored key
                    for (const auto& p : playerOptPairs) if (p.first == val) { key = p.second; break; }
                    Settings::setExternalPlayer(key);
                    // Choosing "Custom…" with no path yet: prompt for the program path right away (the on-screen
                    // keyboard, same nesting as the parental PIN) so the setting isn't left pointing nowhere.
                    if (key == QStringLiteral("custom") && Settings::externalPlayerPath().isEmpty()) {
                        const QString picked = Osk::getText(tr("Path to the player program:"), QString(),
                                                            QLineEdit::Normal, this, themedPanelHost_->navGraph());
                        if (!picked.isNull() && !picked.trimmed().isEmpty()) {
                            Settings::setExternalPlayerPath(picked.trimmed());
                            setAction(QStringLiteral("player.custompath"),
                                      tr("Custom player: %1").arg(QFileInfo(picked.trimmed()).fileName()));
                        }
                    }
                }
                else if (id == QStringLiteral("player.custompath")) {
                    const QString exe = QFileDialog::getOpenFileName(this, tr("Choose a media player program"),
                        QString(),
#ifdef Q_OS_WIN
                        tr("Programs (*.exe);;All files (*.*)"));
#else
                        tr("All files (*.*)"));
#endif
                    if (exe.isEmpty()) return;
                    Settings::setExternalPlayerPath(exe);
                    Settings::setExternalPlayer(QStringLiteral("custom")); // picking an exe implies Custom mode
                    setAction(QStringLiteral("player.custompath"),
                              tr("Custom player: %1").arg(QFileInfo(exe).fileName()));
                }
                else if (id == QStringLiteral("pb.refreshsync")) {
                    Settings::setVideoRefreshSync(on); applyRefreshSyncLive();
                }
                else if (id == QStringLiteral("pb.hdr")) {
                    for (const auto& p : hdrPairs) if (p.first == val) { Settings::setHdrOutput(p.second); break; }
                    applyHdrOutputLive();
                }
                else if (id == QStringLiteral("pb.bezel")) Settings::setBezelEnabled(on);
                else if (id == QStringLiteral("pb.bezelopen")) {
                    const QString d = AppPaths::dataDir() + QStringLiteral("/bezels");
                    QDir().mkpath(d);
                    QDesktopServices::openUrl(QUrl::fromLocalFile(d));
                }
                else if (id == QStringLiteral("pb.decorations")) presentDecorationRegistry();
                // Audio output (issue #69). The device Choice maps the picked display back to its stored id;
                // the two toggles write their bool. All three re-apply live so the change is heard on the next
                // audio init (the device switch is immediate).
                else if (id == QStringLiteral("audio.device")) {
                    QString devId = val;                                 // an unlisted stored id shows as itself
                    for (const auto& p : audioDevPairs) if (p.first == val) { devId = p.second; break; }
                    Settings::setAudioDevice(devId);
                    applyAudioOutputLive();
                }
                else if (id == QStringLiteral("audio.passthrough")) {
                    Settings::setAudioPassthrough(on); applyAudioOutputLive();
                }
                else if (id == QStringLiteral("audio.exclusive")) {
                    Settings::setAudioExclusive(on); applyAudioOutputLive();
                }
                else if (id == QStringLiteral("subs.on")) Settings::setSubtitlesOnByDefault(on);
                else if (id == QStringLiteral("content.lang")) {
                    QString code = val;
                    for (const auto& p : langOptPairs) if (p.first == val) { code = p.second; break; }
                    Settings::setPreferredLanguage(code);
                }
                // Subtitle appearance (issue #71). Each maps the picked display back to its stored value and
                // re-applies the whole style live, so a change is visible on the current sub at once.
                else if (id == QStringLiteral("subs.font")) {
                    Settings::setSubtitleFont(val == tr("Default") ? QString() : val);
                    applySubtitleStyleLive();
                }
                else if (id == QStringLiteral("subs.size")) {
                    Settings::setSubtitleSizePercent(val.left(val.size() - 1).toInt());   // strip trailing "%"
                    applySubtitleStyleLive();
                }
                else if (id == QStringLiteral("subs.color")) {
                    QString hex = val;                                                    // an unlisted "#…" is itself
                    for (const auto& c : subColorPairs) if (c.first == val) { hex = c.second; break; }
                    Settings::setSubtitleColor(hex);
                    applySubtitleStyleLive();
                }
                else if (id == QStringLiteral("subs.bordersize")) {
                    Settings::setSubtitleBorderSize(val.toInt());
                    applySubtitleStyleLive();
                }
                else if (id == QStringLiteral("subs.bordercolor")) {
                    QString hex = val;
                    for (const auto& c : subColorPairs) if (c.first == val) { hex = c.second; break; }
                    Settings::setSubtitleBorderColor(hex);
                    applySubtitleStyleLive();
                }
                else if (id == QStringLiteral("subs.box")) {
                    Settings::setSubtitleBox(on); applySubtitleStyleLive();
                }
                else if (id == QStringLiteral("subs.boxopacity")) {
                    Settings::setSubtitleBoxOpacity(val.left(val.size() - 1).toInt());    // strip trailing "%"
                    applySubtitleStyleLive();
                }
                else if (id == QStringLiteral("subs.pos")) {
                    int pos = Settings::subtitlePosition();
                    for (const auto& p : subPosOptPairs) if (p.first == val) { pos = p.second; break; }
                    Settings::setSubtitlePosition(pos);
                    applySubtitleStyleLive();
                }
                else if (id == QStringLiteral("subs.bold")) {
                    Settings::setSubtitleBold(on); applySubtitleStyleLive();
                }
                else if (id == QStringLiteral("subs.override")) {
                    Settings::setSubtitleOverrideStyled(on); applySubtitleStyleLive();
                }
                // Reader typography (issue #135). Each maps the picked display back to its stored value and
                // re-applies the whole typography live, so a change reflows the open book at once, in place.
                else if (id == QStringLiteral("reader.font")) {
                    Settings::setReaderFont(val == tr("Default") ? QString() : val);
                    applyReaderTypographyLive();
                }
                else if (id == QStringLiteral("reader.size")) {
                    Settings::setReaderFontSize(val.left(val.indexOf(QLatin1Char(' '))).toInt());  // strip " pt"
                    applyReaderTypographyLive();
                }
                else if (id == QStringLiteral("reader.spacing")) {
                    Settings::setReaderLineSpacing(val.left(val.size() - 1).toInt());   // strip trailing "%"
                    applyReaderTypographyLive();
                }
                else if (id == QStringLiteral("reader.margin")) {
                    Settings::setReaderMargin(val.left(val.size() - 1).toInt());        // strip trailing "%"
                    applyReaderTypographyLive();
                }
                else if (id == QStringLiteral("reader.justify")) {
                    Settings::setReaderJustify(on); applyReaderTypographyLive();
                }
                else if (id == QStringLiteral("reader.theme")) {
                    ReaderTypography::Theme th = Settings::readerTheme();
                    for (const auto& t : readerThemePairs) if (t.first == val) { th = t.second; break; }
                    Settings::setReaderTheme(th);
                    applyReaderTypographyLive();
                }
                // Touch reading (issue #147). The two gesture rows need no live re-apply and are owed none:
                // the reader rebuilds its whole Config from these keys on the NEXT tap, exactly as the video
                // player does, so a row flipped here is in force by the time a finger next touches a page.
                // The other two DO re-apply, through #135's existing hook — dual-page repaginates the open
                // book on the same words, and keep-awake takes or drops the lock without waiting for a turn.
                else if (id == QStringLiteral("reader.zones")) {
                    for (const auto& z : readerZonePairs) if (z.first == val) { Settings::setReaderTapZones(z.second); break; }
                }
                else if (id == QStringLiteral("reader.swipe")) Settings::setReaderSwipePaging(on);
                else if (id == QStringLiteral("reader.keepawake")) {
                    Settings::setReaderKeepAwake(on); applyReaderTypographyLive();
                }
                else if (id == QStringLiteral("reader.dualpage")) {
                    Settings::setReaderDualPage(on); applyReaderTypographyLive();
                }
                // In-book lookup (issue #137). Same setter as the classic twin, and no live re-apply is owed:
                // the reader reads the endpoint at the moment a verb is pressed, so a row changed here is in
                // force by the next lookup without anything having to be told about it.
                else if (id == QStringLiteral("reader.translate")) Settings::setReaderTranslateEndpoint(val);
                else if (id == QStringLiteral("reader.vocab")) { openVocabularyList(); return; }
                else if (id == QStringLiteral("os.api"))  Settings::setOpenSubApiKey(val);
                else if (id == QStringLiteral("os.user")) Settings::setOpenSubUsername(val);
                else if (id == QStringLiteral("os.pass")) Settings::setOpenSubPassword(val);
                else if (id == QStringLiteral("trakt.id"))     Settings::setTraktClientId(val);
                else if (id == QStringLiteral("trakt.secret")) Settings::setTraktClientSecret(val);
                else if (id == QStringLiteral("trakt.backfill")) { runTraktBackfill(); return; }
                else if (id == QStringLiteral("trakt.reimport")) { reimportTraktHistory(); return; }
                else if (id == QStringLiteral("trakt.connect")) {
                    if (TraktClient::connected()) { trakt_->disconnectAccount(); return; }
                    if (!TraktClient::configured()) {
                        setInfo(QStringLiteral("trakt.status"), tr("Status"),
                                tr("Enter your Client ID and Secret first.")); return;
                    }
                    setInfo(QStringLiteral("trakt.status"), tr("Status"), tr("Requesting a code from Trakt…"));
                    trakt_->connectAccount();
                }
                // --- AniList (#156). The two credential rows write through AniListTracker rather than
                // Settings, so the #81 BuiltinSecrets follow-up has ONE place to change what "the client
                // id" means. ---
                else if (id == QStringLiteral("anilist.id"))     AniListTracker::setClientId(val);
                else if (id == QStringLiteral("anilist.secret")) AniListTracker::setClientSecret(val);
                else if (id == QStringLiteral("anilist.connect")) {
                    if (AniListTracker::isConnected()) { anilist_->disconnectAccount(); return; }
                    if (!AniListTracker::isConfigured()) {
                        setInfo(QStringLiteral("anilist.status"), tr("Status"),
                                tr("Enter your Client ID and Secret first.")); return;
                    }
                    setInfo(QStringLiteral("anilist.status"), tr("Status"),
                            tr("Opening AniList in your browser…"));
                    anilist_->connectAccount();
                }
                // --- MyAnimeList (#156 increment 2). The twin of the three arms above; the client id is
                // the only required half, because MAL issues public clients with no secret at all. ---
                else if (id == QStringLiteral("mal.id"))     MyAnimeListTracker::setClientId(val);
                else if (id == QStringLiteral("mal.secret")) MyAnimeListTracker::setClientSecret(val);
                else if (id == QStringLiteral("mal.connect")) {
                    if (MyAnimeListTracker::isConnected()) { mal_->disconnectAccount(); return; }
                    if (!MyAnimeListTracker::isConfigured()) {
                        setInfo(QStringLiteral("mal.status"), tr("Status"),
                                tr("Enter your Client ID first.")); return;
                    }
                    setInfo(QStringLiteral("mal.status"), tr("Status"),
                            tr("Opening MyAnimeList in your browser…"));
                    mal_->connectAccount();
                }
                // --- Kitsu (#156 increment 3). The twin of the three arms above, with the one real
                // difference: the two rows are the ACCOUNT's email and password, they go to statics
                // that live in memory only, and no browser opens — Kitsu's grant is a single POST. ---
                else if (id == QStringLiteral("kitsu.email"))    KitsuTracker::setEmail(val);
                else if (id == QStringLiteral("kitsu.password")) KitsuTracker::setPassword(val);
                else if (id == QStringLiteral("kitsu.connect")) {
                    if (KitsuTracker::isConnected()) { kitsu_->disconnectAccount(); return; }
                    if (!KitsuTracker::hasSignInCredentials()) {
                        setInfo(QStringLiteral("kitsu.status"), tr("Status"),
                                tr("Enter your Kitsu email and password first.")); return;
                    }
                    setInfo(QStringLiteral("kitsu.status"), tr("Status"), tr("Signing in to Kitsu…"));
                    kitsu_->connectAccount();
                }
                // --- Music scrobbling (#192). Every arm re-reads the status line afterwards: the answer to
                // "is this on and working" changes with each of them, and a line that still says "Scrobbling
                // is off" after the toggle was flipped is the same silence the line exists to break.
                else if (id == QStringLiteral("scrobble.on")) {
                    Settings::setScrobbleEnabled(on);
                    if (scrobbler_) scrobbler_->retryNow();   // switching it on delivers anything already queued
                    setInfo(QStringLiteral("scrobble.status"), tr("Scrobbling"), scrobbleStatusLine());
                }
                else if (id == QStringLiteral("scrobble.spoken")) {
                    Settings::setScrobbleSpokenAudio(on);
                    setInfo(QStringLiteral("scrobble.status"), tr("Scrobbling"), scrobbleStatusLine());
                }
                else if (id == QStringLiteral("scrobble.serverforwards")) {
                    Settings::setScrobbleServerForwards(on);
                    setInfo(QStringLiteral("scrobble.status"), tr("Scrobbling"), scrobbleStatusLine());
                }
                else if (id == QStringLiteral("scrobble.lbtoken")) {
                    // The value goes STRAIGHT to the store. It is not logged, not echoed into the status line,
                    // and not put in any message — see the credential note at the top of ListenBrainzClient.h.
                    Settings::setListenBrainzToken(val);
                    if (scrobbler_) scrobbler_->retryNow();   // a fixed token releases a queue held by an auth refusal
                    setInfo(QStringLiteral("scrobble.status"), tr("Scrobbling"), scrobbleStatusLine());
                }
                else if (id == QStringLiteral("scrobble.lburl")) {
                    Settings::setListenBrainzApiUrl(val);
                    if (scrobbler_) scrobbler_->retryNow();
                    setInfo(QStringLiteral("scrobble.status"), tr("Scrobbling"), scrobbleStatusLine());
                }
                else if (id == QStringLiteral("scrobble.lastfm")) {
                    // NOTHING IS TYPED HERE. The application key is baked in and the account is authorised in
                    // a browser, so this row is an action with three answers: no key in this build, link, or
                    // unlink. The URL arrives on the authUrl signal wired below.
                    if (!LastFmClient::availableInThisBuild() || !lastfm_) {
                        setInfo(QStringLiteral("scrobble.lastfmstatus"), tr("Last.fm"),
                                LastFmClient::statusText());
                        return;
                    }
                    if (LastFmClient::connected()) { lastfm_->disconnectAccount(); return; }
                    setInfo(QStringLiteral("scrobble.lastfmstatus"), tr("Last.fm"),
                            tr("Asking Last.fm for an authorisation link\u2026"));
                    lastfm_->connectAccount();
                    return;
                }
                // --- Discord presence. Every arm re-reads the status line, and every arm tells the
                // controller at once: a category silenced mid-film must clear the card now, not at the next
                // track boundary. The seven arms are spelled out rather than folded into a table because the
                // setter differs per row and a table would need a map that could drift from the rows above.
                else if (id == QStringLiteral("discord.on")) {
                    Settings::setDiscordEnabled(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("discord.movies")) {
                    Settings::setDiscordMovies(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("discord.games")) {
                    Settings::setDiscordGames(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("discord.music")) {
                    Settings::setDiscordMusic(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("discord.reading")) {
                    Settings::setDiscordReading(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("discord.livetv")) {
                    Settings::setDiscordLiveTv(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("discord.browsing")) {
                    Settings::setDiscordBrowsing(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("parental.setpin")) {
                    if (Settings::hasParentalPin()) {
                        const QString cur = Osk::getText(tr("Enter the current PIN:"), QString(),
                                                         QLineEdit::Password, this, themedPanelHost_->navGraph());
                        if (cur.isNull()) return;
                        if (!Settings::checkParentalPin(cur)) {
                            setInfo(QStringLiteral("parental.status"), tr("PIN"), tr("Incorrect PIN.")); return; }
                    }
                    const QString a = Osk::getText(tr("New PIN:"), QString(), QLineEdit::Password, this,
                                                   themedPanelHost_->navGraph());
                    if (a.isNull() || a.isEmpty()) return;
                    const QString b = Osk::getText(tr("Confirm PIN:"), QString(), QLineEdit::Password, this,
                                                   themedPanelHost_->navGraph());
                    if (b.isNull()) return;
                    if (a != b) { setInfo(QStringLiteral("parental.status"), tr("PIN"), tr("PINs didn't match.")); return; }
                    Settings::setParentalPin(a);
                    setInfo(QStringLiteral("parental.status"), tr("PIN"), tr("A PIN is set"));
                    setAction(QStringLiteral("parental.setpin"), tr("Change PIN"));
                }
                else if (id == QStringLiteral("parental.clearpin")) {
                    if (!Settings::hasParentalPin()) {
                        setInfo(QStringLiteral("parental.status"), tr("PIN"), tr("No PIN set")); return; }
                    const QString cur = Osk::getText(tr("Enter the current PIN:"), QString(),
                                                     QLineEdit::Password, this, themedPanelHost_->navGraph());
                    if (cur.isNull()) return;
                    if (!Settings::checkParentalPin(cur)) {
                        setInfo(QStringLiteral("parental.status"), tr("PIN"), tr("Incorrect PIN.")); return; }
                    Settings::setParentalPin(QString());
                    setInfo(QStringLiteral("parental.status"), tr("PIN"), tr("No PIN set"));
                    setAction(QStringLiteral("parental.setpin"), tr("Set PIN"));
                }
                else if (id == QStringLiteral("profiles.skipsingle"))
                    Settings::setSkipProfilePickerWhenSingle(on);
                else if (id.startsWith(QStringLiteral("profile:"))) {
                    // GATED (fix round 2, finding 2): the restricted flag is what withholds the timed reset
                    // from a kids profile, so flipping it OFF un-does the user's decision and hands the reset
                    // back. See allowRestrictedChange for the rule and the no-PIN answer.
                    const QString pid = id.mid(8);
                    if (!allowRestrictedChange(pid, on)) {
                        // Refused: the row already shows the new state, so put it back to what the store says.
                        PanelRow r; r.kind = PanelRow::Toggle; r.id = id;
                        for (const Profile& pr : ProfileStore::list())
                            if (pr.id == pid) {
                                r.label = (pr.icon.isEmpty() ? QString() : pr.icon + QStringLiteral("  ")) + pr.name;
                                r.checked = pr.restricted;
                            }
                        themedPanelHost_->updateRow(id, r);
                        return;
                    }
                    ProfileStore::setRestricted(pid, on);
                }
                else if (id == QStringLiteral("bgm.on")) {
                    Settings::setBgmEnabled(on); if (bgm_) bgm_->setEnabled(on); updateBackgroundMusic();
                }
                else if (id == QStringLiteral("bgm.vol")) {
                    const int p = val.left(val.size() - 1).toInt();   // strip the trailing "%"
                    Settings::setBgmVolume(p); if (bgm_) bgm_->setVolume(p);
                }
                else if (id == QStringLiteral("bgm.open")) {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(BackgroundMusic::musicDir()));
                    if (bgm_) { bgm_->reload(); updateBackgroundMusic(); }
                }
                else if (id == QStringLiteral("video.previews")) {
                    Settings::setVideoPreviewsEnabled(on); VideoPreviewBridge::instance().setEnabled(on);
                }
                else if (id == QStringLiteral("video.snapvol")) {
                    const int p = val.left(val.size() - 1).toInt();   // strip the trailing "%"
                    Settings::setVideoSnapVolume(p); VideoPreviewBridge::instance().setVolume(p);
                }
                else if (id == QStringLiteral("steam.key")) {
                    Settings::setSteamWebApiKey(val); // never echoed back to the log
                    statusBar()->showMessage(tr("Saved Steam Web API key."), 4000);
                }
                else if (id == QStringLiteral("steam.steamid")) {
                    Settings::setSteamId(val);
                    statusBar()->showMessage(tr("Saved SteamID."), 4000);
                }
                else if (id == QStringLiteral("debrid.torbox")) {
                    store().setValue(QStringLiteral("debrid/torbox/apikey"), val.trimmed()); store().sync();
                }
            },
            [this] { openSettingsHub(); });   // defensive root onBack: General is nested, so a pop re-renders the hub

        // Live Trakt status (persist while the panel is up; dropped at the top of the next present via genSettingsConns_).
        genSettingsConns_ << connect(trakt_, &TraktClient::deviceCode, this,
            [setInfo](const QString& code, const QString& url) {
                setInfo(QStringLiteral("trakt.status"), MainWindow::tr("Status"),
                        MainWindow::tr("Go to %1 and enter code: %2").arg(url, code)); });
        genSettingsConns_ << connect(trakt_, &TraktClient::connectError, this,
            [setInfo](const QString& m) { setInfo(QStringLiteral("trakt.status"), MainWindow::tr("Status"), m); });
        genSettingsConns_ << connect(trakt_, &TraktClient::connectedChanged, this,
            [setInfo, setAction](bool conn) {
                setInfo(QStringLiteral("trakt.status"), MainWindow::tr("Status"),
                        conn ? MainWindow::tr("Connected") : MainWindow::tr("Not connected"));
                setAction(QStringLiteral("trakt.connect"),
                          conn ? MainWindow::tr("Disconnect from Trakt") : MainWindow::tr("Connect to Trakt")); });

        // Live Last.fm status (#192 increment 2), on the same terms as the Trakt block above. The URL is
        // SHOWN as well as opened: on a TV there is no browser to open it in, and the Trakt device-code row
        // beside this one has always been read-and-type for exactly that reason.
        if (lastfm_)
        {
            genSettingsConns_ << connect(lastfm_, &LastFmClient::authUrl, this,
                [setInfo](const QString& url) {
                    setInfo(QStringLiteral("scrobble.lastfmstatus"), MainWindow::tr("Last.fm"),
                            MainWindow::tr("Open %1 and approve, then this will finish on its own.").arg(url));
                    openAuthPage(url); });
            genSettingsConns_ << connect(lastfm_, &LastFmClient::connectError, this,
                [setInfo](const QString& m) {
                    setInfo(QStringLiteral("scrobble.lastfmstatus"), MainWindow::tr("Last.fm"), m); });
            genSettingsConns_ << connect(lastfm_, &LastFmClient::connectedChanged, this,
                [this, setInfo, setAction](bool linked) {
                    setInfo(QStringLiteral("scrobble.lastfmstatus"), MainWindow::tr("Last.fm"),
                            LastFmClient::statusText());
                    setAction(QStringLiteral("scrobble.lastfm"),
                              LastFmClient::connectActionLabel(LastFmClient::availableInThisBuild(), linked));
                    setInfo(QStringLiteral("scrobble.status"), MainWindow::tr("Scrobbling"),
                            scrobbleStatusLine()); });
        }
        // Live AniList status (#156), same lifetime and same genSettingsConns_ teardown. authUrlReady is
        // shown as TEXT rather than assumed to have opened: on a TV the browser may have opened somewhere
        // the user cannot see, and the URL carries no secret (the client id is public and there is no
        // token in it), so it is safe to put on screen.
        genSettingsConns_ << connect(anilist_, &AniListTracker::authUrlReady, this,
            [setInfo](const QString& url) {
                setInfo(QStringLiteral("anilist.status"), MainWindow::tr("Status"),
                        MainWindow::tr("Sign in at: %1").arg(url)); });
        genSettingsConns_ << connect(anilist_, &AniListTracker::connectError, this,
            [setInfo](const QString& m) { setInfo(QStringLiteral("anilist.status"), MainWindow::tr("Status"), m); });
        genSettingsConns_ << connect(anilist_, &AniListTracker::connectedChanged, this,
            [setInfo, setAction](bool conn) {
                setInfo(QStringLiteral("anilist.status"), MainWindow::tr("Status"),
                        conn ? MainWindow::tr("Connected") : MainWindow::tr("Not connected"));
                setAction(QStringLiteral("anilist.connect"),
                          conn ? MainWindow::tr("Disconnect from AniList")
                               : MainWindow::tr("Connect to AniList")); });
        // The same three for MyAnimeList. authUrlReady is shown as TEXT for AniList's reason, and MAL's URL
        // carries no secret either: the client id is public by design and the PKCE challenge in it is
        // single-use and worthless without the loopback listener that minted it.
        genSettingsConns_ << connect(mal_, &MyAnimeListTracker::authUrlReady, this,
            [setInfo](const QString& url) {
                setInfo(QStringLiteral("mal.status"), MainWindow::tr("Status"),
                        MainWindow::tr("Sign in at: %1").arg(url)); });
        genSettingsConns_ << connect(mal_, &MyAnimeListTracker::connectError, this,
            [setInfo](const QString& m) { setInfo(QStringLiteral("mal.status"), MainWindow::tr("Status"), m); });
        genSettingsConns_ << connect(mal_, &MyAnimeListTracker::connectedChanged, this,
            [setInfo, setAction](bool conn) {
                setInfo(QStringLiteral("mal.status"), MainWindow::tr("Status"),
                        conn ? MainWindow::tr("Connected") : MainWindow::tr("Not connected"));
                setAction(QStringLiteral("mal.connect"),
                          conn ? MainWindow::tr("Disconnect from MyAnimeList")
                               : MainWindow::tr("Connect to MyAnimeList")); });
        // ...and Kitsu's two. There is no authUrlReady to connect: nothing opens a browser, so there
        // is never a URL to put on screen.
        genSettingsConns_ << connect(kitsu_, &KitsuTracker::connectError, this,
            [setInfo](const QString& m) { setInfo(QStringLiteral("kitsu.status"), MainWindow::tr("Status"), m); });
        genSettingsConns_ << connect(kitsu_, &KitsuTracker::connectedChanged, this,
            [setInfo, setAction](bool conn) {
                setInfo(QStringLiteral("kitsu.status"), MainWindow::tr("Status"),
                        conn ? MainWindow::tr("Connected") : MainWindow::tr("Not connected"));
                setAction(QStringLiteral("kitsu.connect"),
                          conn ? MainWindow::tr("Sign out of Kitsu")
                               : MainWindow::tr("Sign in to Kitsu")); });
        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        updateBackgroundMusic();
        return;
    }
#endif

    showPanel(tr("General"), [this](QVBoxLayout* v) {
        // --- Display: open the app full screen on launch. ---
        auto* dispHeading = new QLabel(tr("Display"));
        dispHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(dispHeading);
        auto* fs = new QCheckBox(tr("Open in full screen on startup"));
        fs->setStyleSheet(QStringLiteral("font-size:15px;"));
        fs->setChecked(Settings::startFullscreen());
        v->addWidget(fs);
        connect(fs, &QCheckBox::toggled, this, [this](bool c) {
            Settings::setStartFullscreen(c);
            if (c) showFullScreen(); else if (isFullScreen()) leaveFullScreen(); // reflect the choice right away
        });
        v->addSpacing(10);

        // --- Language --- (general preferred content language; governs subtitle + audio track + Accept-Language;
        // classic twin of the themed builder's content.lang row — always enabled, not tied to the subtitle toggle).
        auto* langHeading = new QLabel(tr("Language"));
        langHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(langHeading);
        auto* langRow = new QHBoxLayout();
        langRow->addWidget(new QLabel(tr("Preferred content language:")));
        auto* lang = new QComboBox();
        lang->setMinimumHeight(34);
        const QList<QPair<QString, QString>> langs = {
            { tr("Any / no preference"), QString() }, { QStringLiteral("English"), QStringLiteral("en") },
            { QStringLiteral("Spanish"), QStringLiteral("es") }, { QStringLiteral("French"), QStringLiteral("fr") },
            { QStringLiteral("German"), QStringLiteral("de") }, { QStringLiteral("Italian"), QStringLiteral("it") },
            { QStringLiteral("Portuguese"), QStringLiteral("pt") }, { QStringLiteral("Dutch"), QStringLiteral("nl") },
            { QStringLiteral("Russian"), QStringLiteral("ru") }, { QStringLiteral("Japanese"), QStringLiteral("ja") },
            { QStringLiteral("Korean"), QStringLiteral("ko") }, { QStringLiteral("Chinese"), QStringLiteral("zh") },
            { QStringLiteral("Arabic"), QStringLiteral("ar") },
        };
        const QString cur = Settings::preferredLanguage();
        bool found = false;
        for (const auto& l : langs) { lang->addItem(l.first, l.second); if (l.second == cur) found = true; }
        if (!found && !cur.isEmpty()) lang->addItem(tr("%1 (custom)").arg(cur), cur); // keep a previously-set code
        lang->setCurrentIndex(qMax(0, lang->findData(cur)));
        // Non-editable: clicking anywhere opens the list (an editable combo only opens via the tiny arrow),
        // and it's fully arrow/remote navigable.
        connect(lang, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [lang](int idx) { Settings::setPreferredLanguage(lang->itemData(idx).toString()); }); // save on change
        langRow->addWidget(lang, 1);
        v->addLayout(langRow);
        v->addSpacing(10);

        // --- Attract mode (idle screensaver, issue #54). The classic twin of the themed builder's
        // attract.enabled / attract.timeout rows — a user who has not enabled the themed home reaches it here,
        // same Settings keys, same live re-apply. ---
        auto* attractHeading = new QLabel(tr("Attract mode"));
        attractHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(attractHeading);
        auto* attractOn = new QCheckBox(tr("Play a screensaver slideshow when idle"));
        attractOn->setStyleSheet(QStringLiteral("font-size:15px;"));
        attractOn->setChecked(Settings::attractEnabled());
        v->addWidget(attractOn);
        connect(attractOn, &QCheckBox::toggled, this, [this](bool c) {
            Settings::setAttractEnabled(c); applyAttractConfig(); });
        auto* attractRow = new QHBoxLayout();
        attractRow->addWidget(new QLabel(tr("Start after")));
        auto* attractTimeout = new QComboBox();
        const QList<QPair<QString, int>> attractMins = {
            { tr("1 minute"), 1 }, { tr("2 minutes"), 2 }, { tr("3 minutes"), 3 }, { tr("5 minutes"), 5 },
            { tr("10 minutes"), 10 }, { tr("15 minutes"), 15 }, { tr("20 minutes"), 20 },
            { tr("30 minutes"), 30 }, { tr("45 minutes"), 45 }, { tr("60 minutes"), 60 },
        };
        for (const auto& a : attractMins) attractTimeout->addItem(a.first, a.second);
        {
            int idx = attractTimeout->findData(Settings::attractTimeoutMinutes());
            if (idx < 0) { attractTimeout->addItem(tr("%n minute(s)", nullptr, Settings::attractTimeoutMinutes()),
                                                   Settings::attractTimeoutMinutes());
                           idx = attractTimeout->count() - 1; }
            attractTimeout->setCurrentIndex(idx);
        }
        attractRow->addWidget(attractTimeout);
        attractRow->addStretch(1);
        v->addLayout(attractRow);
        connect(attractTimeout, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, attractTimeout](int) { Settings::setAttractTimeoutMinutes(attractTimeout->currentData().toInt());
                                          applyAttractConfig(); });
        auto* attractNote = new QLabel(tr("After this long with no input on a menu, drift through your library's "
                                          "artwork. Any button wakes it."));
        attractNote->setWordWrap(true);
        attractNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(attractNote);
        v->addSpacing(10);

        // --- Library: the classic twin of the themed builder's "lib.showhidden" row (issue #133). Same store,
        // same key, same live refresh. Global rather than per-profile ON PURPOSE: it reveals what ANY profile
        // has hidden, so it is the one place a mistakenly-hidden item can be found again — which is exactly why
        // it may not exist in only one of the two builders. ---
        // --- Home screen: the classic twin of the themed builder's "home.rows" row (issue #161). Same
        // editor, same store — openHomeRowsEditor() is the one implementation, so the two surfaces cannot
        // drift (GS_TWINS). ---
        auto* homeRowsHeading = new QLabel(tr("Home screen"));
        homeRowsHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(homeRowsHeading);
        auto* homeRowsBtn = new QPushButton(tr("Choose home rows…"));
        connect(homeRowsBtn, &QPushButton::clicked, this, [this] { openHomeRowsEditor(); });
        v->addWidget(homeRowsBtn);
        auto* homeRowsNote = new QLabel(tr("Pick which rows your home shows, put them in the order you want, "
                                           "and cap how many items each one holds. Syncs with the rest of "
                                           "this profile."));
        homeRowsNote->setWordWrap(true);
        homeRowsNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(homeRowsNote);
        v->addSpacing(10);

        auto* libHeading = new QLabel(tr("Library"));
        libHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(libHeading);
        auto* showHidden = new QCheckBox(tr("Show hidden items"));
        showHidden->setStyleSheet(QStringLiteral("font-size:15px;"));
        showHidden->setChecked(store().value(QStringLiteral("library/showHidden"), false).toBool());
        connect(showHidden, &QCheckBox::toggled, this, [this](bool c) {
            store().setValue(QStringLiteral("library/showHidden"), c);
            store().sync();                        // flush so HomeView's own QSettings sees it on the refresh below
            if (home_) home_->reloadForFilterChange(); // hidden rows appear/disappear on the live surface at once
        });
        v->addWidget(showHidden);
        auto* showHiddenNote = new QLabel(tr("Items you hid from their detail view reappear in the library, "
                                             "for every profile, so you can un-hide them."));
        showHiddenNote->setWordWrap(true);
        showHiddenNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(showHiddenNote);
        v->addSpacing(10);

        // --- Following (issue #155): the classic twins of the themed builder's following.* rows. Same
        // Settings keys, same setters, same live-scheduler push — one write path, no drift (GS_TWINS). ---
        auto* fHeading = new QLabel(tr("Following"));
        fHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(fHeading);
        auto* fNote = new QLabel(tr("Series you follow are checked in the background and anything new appears "
                                    "on the New shelf. The check is skipped while something is playing, and one "
                                    "source is never asked twice at once."));
        fNote->setWordWrap(true);
        fNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(fNote);
        auto* fRow = new QHBoxLayout();
        fRow->addWidget(new QLabel(tr("Check followed series:")));
        auto* followInterval = new QComboBox();
        // The SAME list the themed row offers, from the same pure source (follow::intervalChoicesHours) and
        // the same label function, so the two surfaces cannot drift apart in wording or in order.
        for (int h : follow::intervalChoicesHours())
            followInterval->addItem(followIntervalLabel(h), h);
        followInterval->setCurrentIndex(qMax(0, followInterval->findData(Settings::followIntervalHours())));
        fRow->addWidget(followInterval); fRow->addStretch(1);
        v->addLayout(fRow);
        connect(followInterval, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, followInterval](int i) {
            const int h = followInterval->itemData(i).toInt();
            Settings::setFollowIntervalHours(h);
            if (followSched_) followSched_->setIntervalHours(h);
        });
        auto* fMetered = new QCheckBox(tr("Check on metered connections"));
        fMetered->setStyleSheet(QStringLiteral("font-size:15px;"));
        fMetered->setChecked(Settings::followOnMetered());
        v->addWidget(fMetered);
        connect(fMetered, &QCheckBox::toggled, this, [this](bool c) {
            Settings::setFollowOnMetered(c);
            if (followSched_) followSched_->setAllowMetered(c);
        });
        // Grouped notifications (#155 increment 2), off by default; twin of the themed "following.notify".
        auto* fNotify = new QCheckBox(tr("Notify me about new episodes"));
        fNotify->setStyleSheet(QStringLiteral("font-size:15px;"));
        fNotify->setChecked(Settings::followNotify());
        v->addWidget(fNotify);
        connect(fNotify, &QCheckBox::toggled, this, [this](bool c) { setFollowNotifyFromUi(c); });
        auto* fCheck = new QPushButton(tr("Check for new items now"));
        auto* fCheckRow = new QHBoxLayout();
        fCheckRow->addWidget(fCheck); fCheckRow->addStretch(1);
        v->addLayout(fCheckRow);
        // The classic twin of the themed "following.hint" line (#420): the same scheduler status through the
        // same sentence function. (The status bar this button used to write to is hidden on every layout.)
        auto* fStatus = new QLabel(followStatusLine());
        fStatus->setWordWrap(true);
        fStatus->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        // Before any check the sentence is the section's own hint, which this form already prints above.
        fStatus->setVisible(!followSched_ || followSched_->status().phase != follow::CheckPhase::Idle);
        v->addWidget(fStatus);
        followStatusLabel_ = fStatus;
        connect(fCheck, &QPushButton::clicked, this, [this] { followUserCheckNow(false); });
        v->addSpacing(10);

        // --- Updates: check GitHub Releases and install a newer build in place. ---
        auto* uHeading = new QLabel(tr("Updates"));
        uHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(uHeading);
        auto* uVer = new QLabel(tr("You're on version %1.").arg(AppUpdater::currentVersion()));
        uVer->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(uVer);
        auto* uAuto = new QCheckBox(tr("Check for updates on startup"));
        uAuto->setStyleSheet(QStringLiteral("font-size:15px;"));
        uAuto->setChecked(Settings::checkUpdatesOnStartup());
        v->addWidget(uAuto);
        connect(uAuto, &QCheckBox::toggled, this, [](bool c) { Settings::setCheckUpdatesOnStartup(c); });
        auto* uRow = new QHBoxLayout();
        auto* uCheck = new QPushButton(tr("Check now"));
        auto* uInstall = new QPushButton(tr("Install update"));
        uInstall->setVisible(updater_ && updater_->updatePending());
        if (updater_ && updater_->updatePending())
            uInstall->setText(tr("Install %1 and restart").arg(updater_->latestVersion()));
        uRow->addWidget(uCheck); uRow->addWidget(uInstall); uRow->addStretch(1);
        v->addLayout(uRow);
        auto* uStatus = new QLabel();
        uStatus->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(uStatus);
        // Wire the panel's controls to the shared updater. uStatus is the connection context, so the handlers
        // auto-disconnect when the panel closes; SingleShotConnection drops each after it fires once.
        connect(uCheck, &QPushButton::clicked, this, [this, uStatus, uInstall] {
            uStatus->setText(tr("Checking…"));
            connect(updater_, &AppUpdater::updateAvailable, uStatus, [uStatus, uInstall](const QString& ver, const QString&) {
                uStatus->setText(tr("Version %1 is available.").arg(ver));
                uInstall->setText(tr("Install %1 and restart").arg(ver));
                uInstall->setVisible(true);
            }, Qt::SingleShotConnection);
            connect(updater_, &AppUpdater::upToDate, uStatus, [uStatus] {
                uStatus->setText(tr("You're already on the latest version."));
            }, Qt::SingleShotConnection);
            connect(updater_, &AppUpdater::checkFailed, uStatus, [uStatus](const QString& why) {
                uStatus->setText(tr("Couldn't check for updates: %1").arg(why));
            }, Qt::SingleShotConnection);
            updater_->checkForUpdate();
        });
        connect(uInstall, &QPushButton::clicked, this, [this, uStatus] {
            uStatus->setText(tr("Downloading and installing… the app will restart."));
            updater_->downloadAndApply();
        });
        v->addSpacing(10);

        // --- Remote control (issue #76): the classic twin of the themed builder's remote.enabled / remote.url
        // rows — a user who has not enabled the themed home reaches it here, same Settings key, same live
        // start/stop. Off by default; the URL label shows the LAN address to open on a phone while it is on. ---
        auto* remHeading = new QLabel(tr("Remote control"));
        remHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(remHeading);
        auto* remOn = new QCheckBox(tr("Control from a phone on your network"));
        remOn->setStyleSheet(QStringLiteral("font-size:15px;"));
        remOn->setChecked(Settings::remoteControlEnabled());
        v->addWidget(remOn);
        auto* remUrl = new QLabel();
        remUrl->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        remUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto remUrlText = [this] {
            return Settings::remoteControlEnabled()
                ? tr("Open on your phone: %1").arg(RemoteServer::lanUrl(static_cast<quint16>(Settings::remoteControlPort())))
                : tr("A tiny local web control (play/pause, seek, D-pad). Off by default; LAN only.");
        };
        remUrl->setText(remUrlText());
        v->addWidget(remUrl);
        connect(remOn, &QCheckBox::toggled, this, [this, remUrl, remUrlText](bool c) {
            Settings::setRemoteControlEnabled(c);
            updateRemoteServer();                 // start/stop the server right away
            remUrl->setText(remUrlText());        // reflect the reachable URL (or the off hint)
        });
        // #115: the classic twin of the themed remote.filedrop row (GS_TWINS) -- same Setting, same setter.
        auto* dropOn = new QCheckBox(tr("Receive files over the network (file drop)"));
        dropOn->setStyleSheet(QStringLiteral("font-size:15px;"));
        dropOn->setChecked(Settings::fileDropEnabled());
        v->addWidget(dropOn);
        auto* dropUrl = new QLabel();
        dropUrl->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        dropUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto dropUrlText = [this] {
            return Settings::fileDropEnabled()
                ? tr("Open in a browser: %1").arg(fileDropStatusText())
                : tr("Upload ROMs and media from a browser on your network into fixed folders. Off by default; pairing required.");
        };
        dropUrl->setText(dropUrlText());
        v->addWidget(dropUrl);
        connect(dropOn, &QCheckBox::toggled, this, [this, dropUrl, dropUrlText](bool c) {
            setFileDropFromUi(c);
            dropUrl->setText(dropUrlText());
        });
        // #80: the classic twin of the themed deeplink.enabled row (GS_TWINS) -- same Setting, same setter.
        // The label is built first: it contains "//", and the GS_TWINS gate reads this file with // comments
        // stripped, so the construction names the variable instead.
        const QString linksLabel = tr("Open everythingbox:// links");
        auto* linksOn = new QCheckBox(linksLabel);
        linksOn->setStyleSheet(QStringLiteral("font-size:15px;"));
        linksOn->setChecked(Settings::deepLinksEnabled());
        v->addWidget(linksOn);
        auto* linksNote = new QLabel(tr("Lets an add-on's website hand its install link straight to EverythingBox. "
                                        "Every link still asks before installing anything. Off by default."));
        linksNote->setWordWrap(true);
        linksNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(linksNote);
        connect(linksOn, &QCheckBox::toggled, this, [this](bool c) { setDeepLinksFromUi(c); });
        v->addSpacing(10);

        // --- Play on device (issue #143): the classic twin of the themed builder's playon.* rows — same
        // Settings key, same setter, same picker, one write path and no drift (GS_TWINS). ---
        auto* poHeading = new QLabel(tr("Play on device"));
        poHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(poHeading);
        auto* poNote = new QLabel(tr("Other EverythingBoxes on your network appear beside Chromecast and DLNA "
            "in the cast picker. A hand-off sends what to play and where you are in it — never the video "
            "itself — so the other device fetches its own stream. Needs remote control on at BOTH ends."));
        poNote->setWordWrap(true); poNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(poNote);
        auto* poName = new QLabel(tr("This device is called: %1").arg(Settings::deviceName()));
        poName->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(poName);
        auto* poRename = panelRow(tr("Rename This Device…"));
        connect(poRename, &QPushButton::clicked, this, [this, poName] {
            const QString name = Osk::getText(tr("What should other devices call this one?"),
                                              Settings::deviceName(), QLineEdit::Normal, this);
            if (name.isNull()) return;
            Settings::setDeviceName(name);
            updatePlayOnAdvert();
            poName->setText(tr("This device is called: %1").arg(Settings::deviceName()));
        });
        v->addWidget(poRename);
        auto* poPick = panelRow(tr("Play on Another Device…"));
        connect(poPick, &QPushButton::clicked, this, [this] { showPlayOnMenu(); });
        v->addWidget(poPick);
        // Watch together (issue #86): the classic twins of the themed wt.open / wt.buffering rows. Same menu,
        // same Settings key and the same live re-apply -- one code path each, no drift (GS_TWINS).
        auto* wtOpen = panelRow(tr("Watch Together…"));
        connect(wtOpen, &QPushButton::clicked, this, [this] { showWatchTogetherMenu(); });
        v->addWidget(wtOpen);
        auto* wtRow = new QHBoxLayout();
        auto* wtLbl = new QLabel(tr("When someone's stream stalls"));
        wtLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        auto* wtStall = new QComboBox();
        wtStall->addItem(tr("Pause for everyone until they catch up"), QStringLiteral("wait"));
        wtStall->addItem(tr("Keep going and show who's behind"), QStringLiteral("keepgoing"));
        wtStall->setCurrentIndex(qMax(0, wtStall->findData(Settings::watchTogetherPolicy())));
        connect(wtStall, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, wtStall](int) {
                    Settings::setWatchTogetherPolicy(wtStall->currentData().toString());
                    applyWatchTogetherPolicy();
                });
        wtRow->addWidget(wtLbl); wtRow->addWidget(wtStall); wtRow->addStretch(1);
        v->addLayout(wtRow);
        auto* wtNote = new QLabel(tr("Host a room, give someone the code, and the two of you watch the same "
            "thing at the same point. The room shares WHAT to play, never the video and never your account: "
            "each of you fetches your own stream. Over the internet it uses the same relay as online netplay."));
        wtNote->setWordWrap(true);
        wtNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(wtNote);
        // #127: the classic twin of the themed builder's playon.sendlib row — same menu, one code path
        // (GS_TWINS).
        auto* poSendNote = new QLabel(tr("Warms another box's artwork cache from this one so it doesn't "
            "re-scrape thousands of images. Only what's missing or newer is sent, so running it twice sends "
            "nothing the second time. It moves artwork and metadata — never your marks, favourites or resume "
            "points, and never the games or videos themselves."));
        poSendNote->setWordWrap(true);
        poSendNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(poSendNote);
        auto* poSend = panelRow(tr("Send Library To Device…"));
        connect(poSend, &QPushButton::clicked, this, [this] { showSendLibraryMenu(); });
        v->addWidget(poSend);
        v->addSpacing(10);

        // --- Live TV: the classic twin of the themed builder's Live TV section. ---
        auto* tvHeading = new QLabel(tr("Live TV"));
        tvHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(tvHeading);
        auto* tvNote = new QLabel(tr("An M3U playlist — a URL or a local file. Once one is saved, Live TV "
            "appears under Video on the home screen, where you can browse its channels and remove it. The "
            "shelf stays hidden while no source is saved, so this is the way to add the first one."));
        tvNote->setWordWrap(true); tvNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(tvNote);
        auto* tvAdd = panelRow(tr("Add a Live TV Source…"));
        connect(tvAdd, &QPushButton::clicked, this, [this] {
            if (!home_ || !home_->promptForLiveTvSource()) return;
            statusBar()->showMessage(tr("Live TV source saved — it's under Video on the home screen."), 6000);
        });
        v->addWidget(tvAdd);

        // --- Channels (issue #179 increment 2): the classic twin of the themed builder's chan.bumper* rows -
        // same Setting, same setter, same picker, one write path and no drift (GS_TWINS). ---
        auto* cbHeading = new QLabel(tr("Channels"));
        cbHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(cbHeading);
        auto* cbNote = new QLabel(tr("Short idents and bumpers your channels play BETWEEN programmes. They "
            "fill the gap a channel's break grid leaves and never delay a programme: if there is no room for "
            "one, none airs. A channel whose programmes run back to back has no gaps and so plays none. Each "
            "file has to have been played once before its length is known. Leave this empty for no bumpers; a "
            "channel may point at its own folder instead, in its editor under Video > Channels."));
        cbNote->setWordWrap(true); cbNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(cbNote);
        auto* cbRow = new QHBoxLayout();
        auto* cbPath = new QLineEdit(Settings::interstitialFolder());
        cbPath->setMinimumHeight(34);
        cbPath->setReadOnly(true);  // chosen via the picker, so it's always a real folder
        cbPath->setPlaceholderText(tr("No bumper folder set"));
        cbRow->addWidget(cbPath, 1);
        auto* cbBrowse = new QPushButton(tr("Change…"));
        cbRow->addWidget(cbBrowse);
        auto* cbClear = new QPushButton(tr("Clear"));
        cbRow->addWidget(cbClear);
        v->addLayout(cbRow);
        connect(cbBrowse, &QPushButton::clicked, this, [this, cbPath] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your channel bumper folder"),
                                                                  Settings::interstitialFolder());
            if (dir.isEmpty()) return;
            Settings::setInterstitialFolder(dir);
            cbPath->setText(dir);
            statusBar()->showMessage(tr("Channel bumpers will come from %1").arg(dir), 6000);
        });
        connect(cbClear, &QPushButton::clicked, this, [this, cbPath] {
            Settings::setInterstitialFolder(QString());
            cbPath->clear();
            statusBar()->showMessage(tr("Channels will play no bumpers."), 6000);
        });
        v->addSpacing(10);

        // --- Game ROMs: a local ROM library laid out RetroBat / ES-DE style (<root>/<system>/roms). ---
        auto* rHeading = new QLabel(tr("Game ROMs"));
        rHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(rHeading);
        auto* rNote = new QLabel(tr("ROMs live in per-system folders under one root (RetroBat / EmulationStation "
            "Desktop Edition layout). Point this anywhere on your system; games then appear under “Local ROMs” "
            "in the Library."));
        rNote->setWordWrap(true); rNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(rNote);
        auto* rRow = new QHBoxLayout();
        auto* rPath = new QLineEdit(Settings::romsFolder());
        rPath->setMinimumHeight(34);
        rPath->setReadOnly(true); // chosen via the picker, so it's always a real folder
        rRow->addWidget(rPath, 1);
        auto* rBrowse = new QPushButton(tr("Change…"));
        rRow->addWidget(rBrowse);
        v->addLayout(rRow);
        connect(rBrowse, &QPushButton::clicked, this, [this, rPath] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose the ROMs folder"),
                                                                  Settings::romsFolder());
            if (dir.isEmpty()) return;
            Settings::setRomsFolder(dir);
            rPath->setText(dir);
            RomLibrary::ensureStructure();        // create the per-system sub-folders in the new location
            const int added = RomLibrary::syncToDownloads(); // pull any ROMs already there into Downloaded
            statusBar()->showMessage(added > 0 ? tr("ROMs folder set to %1 — added %n game(s) to Downloaded.", "", added).arg(dir)
                                               : tr("ROMs folder set to %1").arg(dir), 6000);
        });
        auto* rOpen = panelRow(tr("Open ROMs Folder"));
        connect(rOpen, &QPushButton::clicked, this, [this] {
            RomLibrary::ensureStructure(); // make sure the tree exists before we open it
            QDesktopServices::openUrl(QUrl::fromLocalFile(RomLibrary::root()));
        });
        v->addWidget(rOpen);
        auto* keepScrape = new QCheckBox(tr("Keep scraped data in the ROMs folder (EmulationStation gamelist.xml)"));
        keepScrape->setStyleSheet(QStringLiteral("font-size:15px;"));
        keepScrape->setChecked(Settings::keepScrapedData());
        keepScrape->setToolTip(tr("When a game is scraped online, also save its info + art into the system's "
                                  "gamelist.xml + media folders, so it's reused from the folder next time and by "
                                  "other EmulationStation/RetroBat frontends. Existing gamelist data is always read."));
        connect(keepScrape, &QCheckBox::toggled, this, [](bool c) { Settings::setKeepScrapedData(c); });
        v->addWidget(keepScrape);
        auto* softPatch = new QCheckBox(tr("Auto-apply ROM patches (translations, romhacks)"));
        softPatch->setStyleSheet(QStringLiteral("font-size:15px;"));
        softPatch->setChecked(Settings::autoApplyRomPatches());
        softPatch->setToolTip(tr("When a ROM has a matching .ips / .bps / .ups patch file beside it, apply it "
                                 "as the game launches — the patched game runs while the original ROM file is "
                                 "never modified. Turn this off to boot the original without moving the patch away."));
        connect(softPatch, &QCheckBox::toggled, this, [](bool c) { Settings::setAutoApplyRomPatches(c); });
        v->addWidget(softPatch);
        auto* verifyDats = new QCheckBox(tr("Verify ROMs against DAT files (No-Intro / Redump)"));
        verifyDats->setStyleSheet(QStringLiteral("font-size:15px;"));
        verifyDats->setChecked(Settings::verifyRoms());
        verifyDats->setToolTip(tr("Drop No-Intro / Redump DAT files into the \"dats\" folder in your data "
                                  "directory, and each game's detail view shows whether its dump is Verified, "
                                  "Bad (a known game whose bytes don't match — often the cause of mysterious "
                                  "emulation glitches), or Unknown (no DAT covers it — neutral). Nothing is "
                                  "ever modified; this only reads."));
        connect(verifyDats, &QCheckBox::toggled, this, [](bool c) { Settings::setVerifyRoms(c); });
        v->addWidget(verifyDats);

        // The classic twin of roms.collapseregions (issue #50). Same key, same setter as the themed row — one
        // write path, no drift. Off by default; the hidden variants stay reachable from a game's "Other versions".
        auto* collapseRegions = new QCheckBox(tr("Collapse regional duplicates"));
        collapseRegions->setStyleSheet(QStringLiteral("font-size:15px;"));
        collapseRegions->setChecked(Settings::collapseRegionalDuplicates());
        collapseRegions->setToolTip(tr("When a folder holds region/revision variants of the same game "
                                       "(e.g. \"Game (USA)\", \"Game (Europe)\", \"Game (Japan)\"), show only "
                                       "the best one — preferred region first, then highest revision. The other "
                                       "versions are never deleted; open a game's detail view to reach them. "
                                       "Leave off for a curated one-file-per-game collection."));
        connect(collapseRegions, &QCheckBox::toggled, this, [](bool c) { Settings::setCollapseRegionalDuplicates(c); });
        v->addWidget(collapseRegions);

        // Classic twin of roms.keepdownloads. Same key + setter as the themed row, one write path. Default on:
        // a downloaded online game is saved into the ROMs folder so the next play reuses the local ROM instead
        // of downloading it again.
        auto* keepDownloads = new QCheckBox(tr("Keep downloaded games in the ROMs folder"));
        keepDownloads->setStyleSheet(QStringLiteral("font-size:15px;"));
        keepDownloads->setChecked(Settings::keepDownloadsInRoms());
        keepDownloads->setToolTip(tr("When you play an online game, save the downloaded ROM into your ROMs folder "
                                     "and add it to your library, so the next time you play it launches from the "
                                     "local file instead of downloading again. Turn off to keep online games "
                                     "download-only (a fresh transient copy each play)."));
        connect(keepDownloads, &QCheckBox::toggled, this, [](bool c) { Settings::setKeepDownloadsInRoms(c); });
        v->addWidget(keepDownloads);

        // Classic twin of ps3.autoupdate. Same key + setter as the themed row, one write path. Default on: a
        // PS3 game's official Sony updates are installed into RPCS3's portable dev_hdd0 before it boots.
        auto* ps3AutoUpdate = new QCheckBox(tr("Auto-install PS3 game updates"));
        ps3AutoUpdate->setStyleSheet(QStringLiteral("font-size:15px;"));
        ps3AutoUpdate->setChecked(Settings::ps3AutoUpdate());
        ps3AutoUpdate->setToolTip(tr("Before launching a PS3 game in RPCS3, download and install its latest "
                                     "official Sony update so the game runs patched. Turn off to always boot the "
                                     "unpatched disc version. A failed update never stops the game from launching."));
        connect(ps3AutoUpdate, &QCheckBox::toggled, this, [](bool c) { Settings::setPs3AutoUpdate(c); });
        v->addWidget(ps3AutoUpdate);

        // Classic twin of content.autoinstall (issue #189). Same key + setter as the themed row, one write path.
        auto* contentInstall = new QCheckBox(tr("Install game updates and DLC before launch"));
        contentInstall->setStyleSheet(QStringLiteral("font-size:15px;"));
        contentInstall->setChecked(Settings::installGameContent());
        contentInstall->setToolTip(tr("Put a game's update and DLC packages in \"updates\" and \"dlc\" folders "
                                      "beside the game file, and they are installed into that emulator's own "
                                      "content store before it launches. Content you installed yourself is never "
                                      "replaced, and a failed install never stops the game from starting. "
                                      "Per-game control lives in that game's launch options."));
        connect(contentInstall, &QCheckBox::toggled, this, [](bool c) { Settings::setInstallGameContent(c); });
        v->addWidget(contentInstall);

        // Save states (#93): auto-increment quick-save + save-on-exit resume mode. Classic twins of the themed
        // emu.autoinc / emu.resume rows.
        auto* autoInc = new QCheckBox(tr("Quick-save to the next free slot (keeps a history)"));
        autoInc->setStyleSheet(QStringLiteral("font-size:15px;"));
        autoInc->setChecked(Settings::stateAutoIncrement());
        autoInc->setToolTip(tr("With this on, the quick-save key writes to the next empty slot instead of "
                               "overwriting the current one, so your quick-saves build up a history."));
        connect(autoInc, &QCheckBox::toggled, this, [](bool c) { Settings::setStateAutoIncrement(c); });
        v->addWidget(autoInc);

        auto* resumeRow = new QHBoxLayout();
        auto* resumeLbl = new QLabel(tr("Resume where you left off"));
        auto* resumeMode = new QComboBox();
        resumeMode->addItem(tr("Ask me"),        int(Settings::ResumePrompt));
        resumeMode->addItem(tr("Automatically"), int(Settings::ResumeSilent));
        resumeMode->addItem(tr("Off"),           int(Settings::ResumeOff));
        resumeMode->setCurrentIndex(qMax(0, resumeMode->findData(Settings::resumeMode())));
        resumeMode->setToolTip(tr("Closing a game saves your spot to a reserved slot — distinct from your "
                                  "numbered save slots, so a manual save is never overwritten. On relaunch, ask "
                                  "before resuming, resume automatically, or never resume."));
        connect(resumeMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [resumeMode](int) { Settings::setResumeMode(resumeMode->currentData().toInt()); });
        resumeRow->addWidget(resumeLbl); resumeRow->addWidget(resumeMode); resumeRow->addStretch(1);
        v->addLayout(resumeRow);

        // Hardcore RetroAchievements (#94): classic twin of the themed emu.hardcore row. Enabling asks for
        // consent (NavConfirm) and resets the achievement session; declining puts the box back without a write.
        auto* hardcore = new QCheckBox(tr("Hardcore RetroAchievements (no save states, rewind or cheats)"));
        hardcore->setStyleSheet(QStringLiteral("font-size:15px;"));
        hardcore->setChecked(Settings::hardcoreAchievements());
        hardcore->setToolTip(tr("Hardcore mode earns full RetroAchievements prestige (softcore unlocks are "
                                "second-class on the site, and leaderboards only count in hardcore), but disables "
                                "save states, rewind, fast-forward and cheats while you play. Enabling resets your "
                                "current achievement session. Softcore stays the default and fully supported."));
        connect(hardcore, &QCheckBox::toggled, this, [this, hardcore](bool c) {
            if (c) {
                const int r = NavConfirm::ask(tr("Enable hardcore mode?"),
                    tr("Hardcore disables save states, rewind, fast-forward and cheats. Enabling resets your "
                       "current achievement session. Continue?"),
                    { tr("Enable"), tr("Cancel") }, 0, 1, this);
                if (r != 0) { QSignalBlocker b(hardcore); hardcore->setChecked(false); return; }
                Settings::setHardcoreAchievements(true); if (ach_) ach_->setHardcore(true);
            } else {
                Settings::setHardcoreAchievements(false); if (ach_) ach_->setHardcore(false);
            }
        });
        v->addWidget(hardcore);

        // Runahead default (#100): classic twin of the themed emu.runahead row. The same four options, N carried
        // in the item data. This is only the DEFAULT — the per-game value (the one that matters, because the
        // right N is that game's own internal input lag) is set from the emulator's pause menu.
        auto* runaheadRow = new QHBoxLayout();
        auto* runaheadLbl = new QLabel(tr("Runahead (default)"));
        auto* runahead = new QComboBox();
        runahead->addItem(tr("Off"),      0);
        runahead->addItem(tr("1 frame"),  1);
        runahead->addItem(tr("2 frames"), 2);
        runahead->addItem(tr("3 frames"), 3);
        runahead->setCurrentIndex(qMax(0, runahead->findData(Settings::runaheadFrames())));
        runahead->setToolTip(tr("Many classic games read the controller a frame or three before they draw the "
                                "response. Runahead hides that delay by emulating those frames in advance, so a "
                                "button press shows up immediately. It costs several times the emulation work per "
                                "frame, so it is refused — with a reason — on a game or a device that can't afford "
                                "it, and it is off during netplay and split screen. Set it per game from the pause "
                                "menu; the right number is that game's own lag, not a device setting."));
        connect(runahead, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [runahead](int) { Settings::setRunaheadFrames(runahead->currentData().toInt()); });
        runaheadRow->addWidget(runaheadLbl); runaheadRow->addWidget(runahead); runaheadRow->addStretch(1);
        v->addLayout(runaheadRow);

        // Global-default shader preset (#99): classic twin of the themed emu.shaderpreset row. A QComboBox over
        // the same curated ShaderPreset registry, id carried in the item data; the current value seeds from the
        // legacy filter via Settings::shaderPreset(). An out-of-registry stored id (custom / hand-edited) is
        // appended so it stays shown. Loads no shader — the render slice is later.
        auto* shaderRow = new QHBoxLayout();
        auto* shaderLbl = new QLabel(tr("Shader preset (experimental)"));
        auto* shaderPreset = new QComboBox();
        for (const ShaderPreset::Entry& e : ShaderPreset::registry()) shaderPreset->addItem(e.displayName, e.id);
        {
            const QString curId = Settings::shaderPreset();
            int idx = shaderPreset->findData(curId);
            if (idx < 0) {   // custom / hand-edited id not in the registry: keep it shown and selected
                const QString disp = ShaderPreset::isCustomId(curId) ? ShaderPreset::customPath(curId) : curId;
                shaderPreset->addItem(disp, curId);
                idx = shaderPreset->count() - 1;
            }
            shaderPreset->setCurrentIndex(qMax(0, idx));
        }
        shaderPreset->setToolTip(tr("Slang shaders reproduce the look of a CRT, an LCD grid or a crisp upscale. "
                                    "This sets the default for every game; heavy presets can slow weak GPUs. "
                                    "Custom .slangp files and per-game overrides arrive in a later update."));
        connect(shaderPreset, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [shaderPreset](int) { Settings::setShaderPreset(shaderPreset->currentData().toString()); });
        shaderRow->addWidget(shaderLbl); shaderRow->addWidget(shaderPreset); shaderRow->addStretch(1);
        v->addLayout(shaderRow);

#ifdef EB_HAVE_RETROPARK
        // RetroPark driven backend (OpenGL host runtime task): classic twin of the themed emu.rpdriven row. A
        // QComboBox over the same two options, stored id ("d3d11"|"opengl") carried in the item data. Selects the
        // host graphics API RetroPark's DRIVEN cores (the in-process NES shim / reference core) run on; D3D11 is
        // the proven default, OpenGL an opt-in. Presenting cores (Dolphin / GameCube) always use Vulkan.
        auto* rpDrivenRow = new QHBoxLayout();
        auto* rpDrivenLbl = new QLabel(tr("RetroPark driven backend"));
        auto* rpDriven = new QComboBox();
        rpDriven->addItem(tr("Direct3D 11 (default)"), QStringLiteral("d3d11"));
        rpDriven->addItem(tr("OpenGL"),                QStringLiteral("opengl"));
        rpDriven->setCurrentIndex(qMax(0, rpDriven->findData(Settings::retroParkDrivenBackend())));
        rpDriven->setToolTip(tr("Which graphics API RetroPark uses to run its in-process (NES / reference) cores. "
                                "Direct3D 11 is the proven default; OpenGL is an experimental opt-in. GameCube "
                                "(Dolphin) is unaffected — it always uses Vulkan. Takes effect the next time you "
                                "launch a game."));
        connect(rpDriven, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [rpDriven](int) { Settings::setRetroParkDrivenBackend(rpDriven->currentData().toString()); });
        rpDrivenRow->addWidget(rpDrivenLbl); rpDrivenRow->addWidget(rpDriven); rpDrivenRow->addStretch(1);
        v->addLayout(rpDrivenRow);
#endif

        // MS-DOS MIDI device (issue #191): classic twin of the themed emu.dosmidi row. The options are read
        // from the msdos launch recipe's `midi` block, exactly as the themed builder reads them, so the two
        // surfaces can never offer different devices; the stored id ("" | "gm" | "mt32") rides in the item
        // data. Nothing is ever downloaded — the assets are the user's to supply, and a launch with one
        // missing plays on the core's default audio and names the file it wanted.
        auto* dosMidiRow = new QHBoxLayout();
        auto* dosMidiLbl = new QLabel(tr("MS-DOS MIDI device"));
        auto* dosMidi = new QComboBox();
        dosMidi->addItem(tr("Default (the core decides)"), QString());
        {
            const LaunchRecipe& dosRecipeC = LaunchRecipes::forSystem(QStringLiteral("msdos"));
            const RecipeCore* dosCoreC = dosRecipeC.isNull()
                ? nullptr : LaunchRecipes::coreFor(dosRecipeC, QStringLiteral("dosbox_pure"));
            if (dosCoreC)
                for (const DosConf::MidiDevice& d : dosCoreC->midi.devices)
                    dosMidi->addItem(d.label.isEmpty() ? d.id : d.label, d.id);
        }
        dosMidi->setCurrentIndex(qMax(0, dosMidi->findData(Settings::dosMidiDevice())));
        dosMidi->setToolTip(tr("Many DOS games sound far better through a Roland MT-32 or General MIDI than "
                               "through Adlib. The ROMs and the soundfont are yours to supply - EverythingBox "
                               "never downloads them. Put MT32_CONTROL.ROM and MT32_PCM.ROM, or a soundfont "
                               "named DOSBOX.SF2, in the system folder; if a file is missing the game still "
                               "plays, on its default audio, and says which file it wanted."));
        connect(dosMidi, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [dosMidi](int) { Settings::setDosMidiDevice(dosMidi->currentData().toString()); });
        dosMidiRow->addWidget(dosMidiLbl); dosMidiRow->addWidget(dosMidi); dosMidiRow->addStretch(1);
        v->addLayout(dosMidiRow);

        v->addSpacing(10);

        // --- Local Library (movies + TV): a folder of local video files surfaced under the video category. ---
        auto* llHeading = new QLabel(tr("Local Library"));
        llHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(llHeading);
        auto* llNote = new QLabel(tr("Point this at a folder of your own movies + TV episodes. They then appear "
            "under “Local Library” in the video category. Use “Rescan” after adding or removing files."));
        llNote->setWordWrap(true); llNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(llNote);
        auto* llRow = new QHBoxLayout();
        auto* llPath = new QLineEdit(Settings::libraryFolder());
        llPath->setMinimumHeight(34);
        llPath->setReadOnly(true); // chosen via the picker, so it's always a real folder
        llRow->addWidget(llPath, 1);
        auto* llBrowse = new QPushButton(tr("Change…"));
        llRow->addWidget(llBrowse);
        auto* llRescan = new QPushButton(tr("Rescan"));
        llRow->addWidget(llRescan);
        v->addLayout(llRow);
        connect(llBrowse, &QPushButton::clicked, this, [this, llPath] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your local video library folder"),
                                                                  Settings::libraryFolder());
            if (dir.isEmpty()) return;
            Settings::setLibraryFolder(dir);
            llPath->setText(dir);
            rescanLocalLibrary();
            statusBar()->showMessage(tr("Local Library folder set to %1 — rescanning…").arg(dir), 6000);
        });
        connect(llRescan, &QPushButton::clicked, this, [this] {
            rescanLocalLibrary();
            statusBar()->showMessage(tr("Rescanning your local library…"), 4000);
        });

        // The classic twins of library.resolveonline + library.rematch (issue #133). Same Settings key, same
        // setter, same resolver calls as the themed rows — one write path, no drift. Without these two the
        // classic surface can point at a library folder and rescan it, but can neither turn online matching on
        // nor ask for it again: posters and synopses would be a thing that either happened or didn't.
        auto* llResolve = new QCheckBox(tr("Match local files to online catalogs"));
        llResolve->setStyleSheet(QStringLiteral("font-size:15px;"));
        llResolve->setChecked(Settings::resolveOnline());
        connect(llResolve, &QCheckBox::toggled, this, [this](bool c) {
            Settings::setResolveOnline(c);
            if (c && resolver_) resolver_->enqueue(LocalLibrary::index().all());
        });
        v->addWidget(llResolve);
        auto* llResolveNote = new QLabel(tr("Looks your files up online to fill in posters, synopses and "
                                            "episode titles. Turn it off to show only what the filenames say."));
        llResolveNote->setWordWrap(true);
        llResolveNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(llResolveNote);

        auto* llRematch = new QPushButton(tr("Re-match Local Library online"));
        llRematch->setMinimumHeight(34);
        auto* llRematchRow = new QHBoxLayout(); llRematchRow->addWidget(llRematch); llRematchRow->addStretch(1);
        v->addLayout(llRematchRow);
        auto* llRematchNote = new QLabel(tr("Throws away every match already made and looks the whole library "
                                            "up again — use it when files came back matched to the wrong title."));
        llRematchNote->setWordWrap(true);
        llRematchNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(llRematchNote);
        connect(llRematch, &QPushButton::clicked, this, [this] {
            // Same guard as the themed row, for the same reason: with resolveOnline off, clearing the cache
            // then enqueue() (which no-ops) wipes every resolved id on the next rebuild and never re-resolves.
            if (!Settings::resolveOnline()) {
                statusBar()->showMessage(tr("Turn on \"Match local files to online catalogs\" first."), 4000);
                return;
            }
            if (resolver_) resolver_->clearCacheAndRequeue(LocalLibrary::index().all());
            statusBar()->showMessage(tr("Re-matching your Local Library online…"), 4000);
        });

        // The themed twin of library.clearmetaedits (issue #24). Same store, same clearAll, same confirm — the
        // editor lives on each item's detail card, this is only the library-wide undo. The button carries the
        // count so it is never a blind "reset everything", and relabels itself after the reset.
        auto* metaEdits = new QPushButton(tr("Reset my metadata edits (%n item(s))", nullptr,
                                             MetaOverrides::count()));
        metaEdits->setMinimumHeight(34);
        v->addWidget(metaEdits);
        auto* metaEditsNote = new QLabel(tr("Discards the title/synopsis/poster corrections you made on "
                                            "individual items, restoring what the scrapers found."));
        metaEditsNote->setWordWrap(true);
        metaEditsNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(metaEditsNote);
        connect(metaEdits, &QPushButton::clicked, this, [this, metaEdits] {
            const int n = MetaOverrides::count();
            if (n == 0) { statusBar()->showMessage(tr("You haven't edited any item's info."), 4000); return; }
            const int c = NavConfirm::ask(tr("Reset metadata edits"),
                tr("Discard your info edits on %n item(s) and go back to what the scrapers found? "
                   "This can't be undone.", nullptr, n),
                { tr("Cancel"), tr("Reset all") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
            if (c != 1) return;
            MetaOverrides::clearAll();
            if (home_) home_->refreshDetailMetaCard();
            metaEdits->setText(tr("Reset my metadata edits (%n item(s))", nullptr, MetaOverrides::count()));
            statusBar()->showMessage(tr("Your metadata edits were reset."), 4000);
        });
        v->addSpacing(10);

        // --- Jellyfin (#160): the classic twin of the themed jellyfin.servers row. Same manager, same
        // store, same status sentence, so the two surfaces cannot tell the user different things. ---
        auto* jfHeading = new QLabel(tr("Jellyfin"));
        jfHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(jfHeading);
        auto* jfNote = new QLabel(tr("Connect a Jellyfin server you already run — or one a friend shares "
            "with you. Several can be connected at once and their libraries appear together, each row "
            "labelled with the server it came from. A server can be switched off to hide its rows without "
            "forgetting the sign-in. Sign-ins are kept on this device only and are never included in "
            "anything this app syncs."));
        jfNote->setWordWrap(true); jfNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(jfNote);
        auto* jfSrvStatus = new QLabel(jellyfinServerStatusLine());
        jfSrvStatus->setWordWrap(true);
        jfSrvStatus->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        auto* jfSrv = new QPushButton(tr("Jellyfin servers…"));
        v->addWidget(jfSrv);
        v->addWidget(jfSrvStatus);
        // While THIS panel is up it owns the refresh hook — the scrobbleStatusUpdate_ idiom, QPointer-guarded
        // so a store change after the panel is gone touches nothing. The connect flow is asynchronous, so the
        // click handler alone would leave the line a server behind.
        {
            QPointer<QLabel> guard(jfSrvStatus);
            jellyfinStatusUpdate_ = [this, guard] {
                if (guard) guard->setText(jellyfinServerStatusLine()); };
        }
        connect(jfSrv, &QPushButton::clicked, this, [this, jfSrvStatus] {
            if (!home_) return;
            home_->manageJellyfinServersInteractive();
            jfSrvStatus->setText(jellyfinServerStatusLine());
        });
        // The classic twin of the themed jellyfin.continuemerge row (#160 increment 2). Same store, same
        // setter — one write path, no drift (GS_TWINS).
        auto* jfMerge = new QCheckBox(tr("Merge Continue Watching across servers"));
        jfMerge->setStyleSheet(QStringLiteral("font-size:15px;"));
        jfMerge->setChecked(JellyfinServerStore::continueMerged());
        jfMerge->setToolTip(tr("On, the servers' half-watched items share one Continue Watching section. "
                               "Off, each server gets its own section, labelled with its name. This is a "
                               "setting for THIS device and is never included in anything this app syncs."));
        connect(jfMerge, &QCheckBox::toggled, this,
                [](bool c) { JellyfinServerStore::setContinueMerged(c); });
        v->addWidget(jfMerge);
        v->addSpacing(10);

        // --- Requests (#109): the classic twin of the themed requests.service row. Same manager, same
        // store, same status sentence, so the two surfaces cannot tell the user different things. ---
        auto* rqHeading = new QLabel(tr("Requests"));
        rqHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(rqHeading);
        auto* rqNote = new QLabel(tr("Point this at the request service your household already runs and a "
            "“Request” button appears on films and series you do not have. Asking is always an explicit "
            "press — nothing is ever requested for you. The API key is kept on this device only and is "
            "never included in anything this app syncs."));
        rqNote->setWordWrap(true); rqNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(rqNote);
        auto* rqStatus = new QLabel(JellyseerrStore::statusLine());
        rqStatus->setWordWrap(true);
        rqStatus->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        auto* rqSrv = new QPushButton(tr("Request service…"));
        v->addWidget(rqSrv);
        v->addWidget(rqStatus);
        // While THIS panel is up it owns the refresh hook — the jellyfinStatusUpdate_ idiom, QPointer-guarded
        // so a store change after the panel is gone touches nothing. The verify step is asynchronous, so the
        // click handler alone would leave the line a service behind.
        {
            QPointer<QLabel> guard(rqStatus);
            requestsStatusUpdate_ = [guard] {
                if (guard) guard->setText(JellyseerrStore::statusLine()); };
        }
        connect(rqSrv, &QPushButton::clicked, this, [this, rqStatus] {
            manageRequestServiceInteractive();
            rqStatus->setText(JellyseerrStore::statusLine());
        });
        v->addSpacing(10);

        // --- Downloads (#110): the classic twins of downloads.cap and downloads.removewatched. Same store
        // and same setters as the themed rows — one write path, no drift (GS_TWINS). ---
        auto* dlHeading = new QLabel(tr("Downloads"));
        dlHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(dlHeading);
        auto* dlNote = new QLabel(tr("Items you download for offline viewing are kept on this device. These "
            "two settings are about THIS device's disk, so they are never included in anything this app "
            "syncs."));
        dlNote->setWordWrap(true); dlNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(dlNote);
        auto* dlCap = new QComboBox();
        // The same six options and the same order as the themed row's dlCapPairs; the value stored is the
        // number of gigabytes, and 0 is "No limit".
        const QList<QPair<QString, int>> dlCapPairsClassic = {
            { tr("No limit"),  0   }, { tr("10 GB"), 10  }, { tr("25 GB"),  25  },
            { tr("50 GB"),     50  }, { tr("100 GB"), 100 }, { tr("250 GB"), 250 } };
        for (const auto& c : dlCapPairsClassic) dlCap->addItem(c.first, c.second);
        dlCap->setCurrentIndex(qMax(0, dlCap->findData(JellyfinDownload::capGb())));
        dlCap->setToolTip(tr("When your downloads go over this limit you are shown the least recently "
                             "watched ones and can remove them. Nothing is ever deleted for you."));
        {
            auto* dlCapRow = new QHBoxLayout();
            dlCapRow->addWidget(new QLabel(tr("Storage limit for downloads")));
            dlCapRow->addWidget(dlCap);
            dlCapRow->addStretch();
            v->addLayout(dlCapRow);
        }
        connect(dlCap, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, dlCap](int) {
            JellyfinDownload::setCapGb(dlCap->currentData().toInt());
            checkJellyfinDownloadCap();   // a tighter limit may make this true immediately
        });
        auto* dlRemoveWatched = new QCheckBox(tr("Offer to remove downloads once watched"));
        dlRemoveWatched->setStyleSheet(QStringLiteral("font-size:15px;"));
        dlRemoveWatched->setChecked(JellyfinDownload::removeAfterWatched());
        dlRemoveWatched->setToolTip(tr("After you finish watching a downloaded item, offer to free the space "
                                       "it is using. Off by default, and it always asks first."));
        connect(dlRemoveWatched, &QCheckBox::toggled, this,
                [](bool c) { JellyfinDownload::setRemoveAfterWatched(c); });
        v->addWidget(dlRemoveWatched);
        v->addSpacing(10);

        // --- Photos (#102): the classic twin of the themed photos.path/photos.change rows. Same Settings key
        // and setter — one write path, no drift (GS_TWINS). ---
        auto* phHeading = new QLabel(tr("Photos"));
        phHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(phHeading);
        auto* phNote = new QLabel(tr("Point this at a folder of your own images. Open any image to view it "
            "full-screen and page through its folder (JPEG, PNG, WebP, GIF and BMP; HEIC only where your "
            "platform supports it)."));
        phNote->setWordWrap(true); phNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(phNote);
        auto* phRow = new QHBoxLayout();
        auto* phPath = new QLineEdit(Settings::photosFolder());
        phPath->setMinimumHeight(34);
        phPath->setReadOnly(true); // chosen via the picker, so it's always a real folder
        phRow->addWidget(phPath, 1);
        auto* phBrowse = new QPushButton(tr("Change…"));
        phRow->addWidget(phBrowse);
        v->addLayout(phRow);
        connect(phBrowse, &QPushButton::clicked, this, [this, phPath] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your photo library folder"),
                                                                  Settings::photosFolder());
            if (dir.isEmpty()) return;
            Settings::setPhotosFolder(dir);
            phPath->setText(dir);
            statusBar()->showMessage(tr("Photos folder set to %1").arg(dir), 6000);
        });
        v->addSpacing(10);

        // --- Music (#74): the classic twin of the themed music.path/music.change/music.rescan rows. Same
        // Settings key, same setter, same rescan call — one write path, no drift (GS_TWINS). ---
        auto* muHeading = new QLabel(tr("Music"));
        muHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(muHeading);
        auto* muNote = new QLabel(tr("Point this at a folder of your own music. Its tags are read once and "
            "kept, so it browses by artist and album instead of by file. Use “Rescan” after adding or "
            "removing tracks. This is not the background-music folder above — that one holds the "
            "interface's own looping tracks."));
        muNote->setWordWrap(true); muNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(muNote);
        auto* muRow = new QHBoxLayout();
        auto* muPath = new QLineEdit(Settings::musicFolder());
        muPath->setMinimumHeight(34);
        muPath->setReadOnly(true); // chosen via the picker, so it's always a real folder
        muRow->addWidget(muPath, 1);
        auto* muBrowse = new QPushButton(tr("Change…"));
        muRow->addWidget(muBrowse);
        auto* muRescan = new QPushButton(tr("Rescan"));
        muRow->addWidget(muRescan);
        v->addLayout(muRow);
        connect(muBrowse, &QPushButton::clicked, this, [this, muPath] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your music folder"),
                                                                  Settings::musicFolder());
            if (dir.isEmpty()) return;
            Settings::setMusicFolder(dir);
            muPath->setText(dir);
            rescanMusicLibrary();
            statusBar()->showMessage(tr("Music folder set to %1 — scanning…").arg(dir), 6000);
        });
        connect(muRescan, &QPushButton::clicked, this, [this] {
            rescanMusicLibrary();
            statusBar()->showMessage(tr("Scanning your music…"), 4000);
        });

        // Music SERVERS (#193) — the classic twin of the themed music.addserver row. Same prompt, same
        // store, same status sentence, so the two surfaces cannot tell the user different things.
        auto* muSrvNote = new QLabel(tr("Play a Subsonic music server you already run — Navidrome, Airsonic, "
            "Gonic, Ampache and others speak the same API. Your servers appear inside Music, browsable by "
            "artist and album exactly like your own files. The password is kept on this device only and is "
            "never included in anything this app syncs."));
        muSrvNote->setWordWrap(true); muSrvNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(muSrvNote);
        auto* muSrvStatus = new QLabel(musicServerStatusLine());
        muSrvStatus->setWordWrap(true); muSrvStatus->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        auto* muSrvAdd = new QPushButton(tr("Add a music server…"));
        v->addWidget(muSrvAdd);
        v->addWidget(muSrvStatus);
        connect(muSrvAdd, &QPushButton::clicked, this, [this, muSrvStatus] {
            if (!home_) return;
            home_->addMusicServerInteractive();
            muSrvStatus->setText(musicServerStatusLine());
        });

        // ONE LIBRARY ACROSS SOURCES (#194) — the classic twins of the themed music.prefsource /
        // music.clearmatches rows. Same shared option list, same Settings key/setter, same reset: one write
        // path, no drift (GS_TWINS).
        auto* muPrefNote = new QLabel(tr("When the same album is on this device and on a music server, this is "
            "the copy that plays. The others stay one press away on the album itself. Matching is deliberately "
            "cautious: two copies it is not sure about are left as two rows rather than merged into one, "
            "because a wrong match hides music."));
        muPrefNote->setWordWrap(true); muPrefNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(muPrefNote);
        auto* muPrefRow = new QHBoxLayout();
        auto* muPrefLbl = new QLabel(tr("Play music from"));
        muPrefLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        auto* muPref = new QComboBox();
        for (const auto& pr : musicSourcePrefPairs()) muPref->addItem(pr.first, pr.second);
        muPref->setCurrentIndex(qMax(0, muPref->findData(Settings::musicPreferredSource())));
        connect(muPref, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, muPref](int) {
            Settings::setMusicPreferredSource(muPref->currentData().toString());
            if (home_) home_->refreshMusicLevels();
        });
        muPrefRow->addWidget(muPrefLbl); muPrefRow->addWidget(muPref); muPrefRow->addStretch(1);
        v->addLayout(muPrefRow);

        // SERVER STREAMING QUALITY (#193) — the classic twin of the themed music.streamquality row. Same option
        // list, same setter (GS_TWINS).
        auto* ssqNote = new QLabel(subsonicStreamQualityHint());
        ssqNote->setWordWrap(true); ssqNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(ssqNote);
        auto* ssqRow = new QHBoxLayout();
        auto* ssqLbl = new QLabel(tr("Server streaming quality"));
        ssqLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        auto* ssQuality = new QComboBox();
        for (const auto& pr : subsonicStreamQualityPairs()) ssQuality->addItem(pr.first, pr.second);
        ssQuality->setCurrentIndex(qMax(0, ssQuality->findData(Settings::subsonicStreamMaxBitRate())));
        connect(ssQuality, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [ssQuality](int) {
            Settings::setSubsonicStreamMaxBitRate(ssQuality->currentData().toInt());
        });
        ssqRow->addWidget(ssqLbl); ssqRow->addWidget(ssQuality); ssqRow->addStretch(1);
        v->addLayout(ssqRow);

        auto* muClear = new QPushButton(tr("Reset my music match corrections (%1)").arg(musicMatchOverrideCount()));
        v->addWidget(muClear);
        connect(muClear, &QPushButton::clicked, this, [this, muClear] {
            if (musicMatchOverrideCount() == 0) {
                statusBar()->showMessage(tr("You haven't corrected any music matches."), 4000);
                return;
            }
            clearMusicMatchOverrides();
            muClear->setText(tr("Reset my music match corrections (%1)").arg(musicMatchOverrideCount()));
            if (home_) home_->refreshMusicLevels();
            statusBar()->showMessage(tr("Your music match corrections were reset."), 4000);
        });

        // Multi-value tag separators (#196) — the classic twin of the themed music.separators TextField row.
        // Same Settings key/setter and the same rescan, one write path, no drift (GS_TWINS).
        auto* muSepNote = new QLabel(tr("Characters that separate several artists or genres inside one tag, "
            "spaced apart — “; /” is two of them. Files that store their values properly (FLAC, and mp3s "
            "tagged as ID3v2.4) are already split correctly and are never affected by this. Album artist is "
            "never split, so albums stay whole. Only “;” by default: “/” would turn AC/DC into two bands. "
            "Leave it empty to split nothing."));
        muSepNote->setWordWrap(true); muSepNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(muSepNote);
        auto* muSepRow = new QHBoxLayout();
        auto* muSepLbl = new QLabel(tr("Artist / genre separators"));
        muSepLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        muSepRow->addWidget(muSepLbl);
        auto* muSeps = new QLineEdit(Settings::musicTagSeparators());
        muSeps->setMinimumHeight(34);
        muSepRow->addWidget(muSeps, 1);
        v->addLayout(muSepRow);
        // editingFinished, not textChanged: a rescan per keystroke would re-tag the whole library while the
        // user is still typing the second separator.
        connect(muSeps, &QLineEdit::editingFinished, this, [this, muSeps] {
            if (muSeps->text().trimmed() == Settings::musicTagSeparators()) return;   // focus out, no edit
            Settings::setMusicTagSeparators(muSeps->text());
            rescanMusicLibrary();
            statusBar()->showMessage(tr("Re-reading your music tags…"), 4000);
        });
        v->addSpacing(10);

        // --- Audiobooks (#139): the classic twin of the themed audiobooks.path/.change/.rescan rows. Same
        // Settings key, same setter, same rescan call — one write path, no drift (GS_TWINS). ---
        auto* abHeading = new QLabel(tr("Audiobooks"));
        abHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(abHeading);
        auto* abNote = new QLabel(tr("Point this at a folder of your own audiobooks and they browse by "
            "author, narrator and series. A folder of numbered files is treated as one book that plays "
            "straight through and remembers where you were. This is kept apart from your Music folder on "
            "purpose: nothing is guessed from the file, so an mp3 in here is a book and the same mp3 in "
            "your music folder is music."));
        abNote->setWordWrap(true); abNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(abNote);
        auto* abRow = new QHBoxLayout();
        auto* abPath = new QLineEdit(Settings::audiobookFolder());
        abPath->setMinimumHeight(34);
        abPath->setReadOnly(true); // chosen via the picker, so it's always a real folder
        abRow->addWidget(abPath, 1);
        auto* abBrowse = new QPushButton(tr("Change…"));
        abRow->addWidget(abBrowse);
        auto* abRescan = new QPushButton(tr("Rescan"));
        abRow->addWidget(abRescan);
        v->addLayout(abRow);
        connect(abBrowse, &QPushButton::clicked, this, [this, abPath] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your audiobook folder"),
                                                                  Settings::audiobookFolder());
            if (dir.isEmpty()) return;
            Settings::setAudiobookFolder(dir);
            abPath->setText(dir);
            rescanAudiobookLibrary();
            statusBar()->showMessage(tr("Audiobooks folder set to %1 — scanning…").arg(dir), 6000);
        });
        connect(abRescan, &QPushButton::clicked, this, [this] {
            rescanAudiobookLibrary();
            statusBar()->showMessage(tr("Scanning your audiobooks…"), 4000);
        });

        // Audiobook SERVERS (#197) — the classic twin of the themed audiobooks.addserver row. Same prompt,
        // same store, same status sentence: this opens HomeView's own add flow rather than a second copy of
        // it, so there is one place that decides what adding a server asks and what it saves.
        auto* abSrvNote = new QLabel(tr("Play an Audiobookshelf server you already run. Your books, series "
                                        "and podcasts appear under Audiobooks, with the chapters and the "
                                        "listening position the server itself keeps — so where you are in a "
                                        "book is the same here as in every other app you use with it."));
        abSrvNote->setWordWrap(true); abSrvNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(abSrvNote);
        auto* abSrvAdd = new QPushButton(tr("Add an audiobook server…"));
        abSrvAdd->setMinimumHeight(34);
        v->addWidget(abSrvAdd);
        auto* abSrvStatus = new QLabel(audiobookServerStatusLine());
        abSrvStatus->setWordWrap(true);
        abSrvStatus->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(abSrvStatus);
        connect(abSrvAdd, &QPushButton::clicked, this, [this, abSrvStatus] {
            if (!home_) return;
            // While THIS panel is up it owns the refresh hook — the scrobbleStatusUpdate_ idiom. QPointer,
            // because the sign-in is a round trip and the panel can be gone before it lands.
            const QPointer<QLabel> guard(abSrvStatus);
            absServerStatusUpdate_ = [this, guard] { if (guard) guard->setText(audiobookServerStatusLine()); };
            home_->addAudiobookServerInteractive();
        });
        v->addSpacing(10);

        // --- Books (#134): the classic twin of the themed books.path/.change/.rescan rows. Same Settings
        // key, same setter, same rescan call - one write path, no drift (GS_TWINS). ---
        auto* bkHeading = new QLabel(tr("Books"));
        bkHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(bkHeading);
        auto* bkNote = new QLabel(tr("Point this at a folder of your own books and comics and they browse by "
            "author and series. EPUB books bring their own title, author and cover; comics are grouped by "
            "what their files are called. Anything with no information at all still shows up, under its "
            "file name."));
        bkNote->setWordWrap(true); bkNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(bkNote);
        auto* bkRow = new QHBoxLayout();
        auto* bkPath = new QLineEdit(Settings::readingFolder());
        bkPath->setMinimumHeight(34);
        bkPath->setReadOnly(true); // chosen via the picker, so it's always a real folder
        bkRow->addWidget(bkPath, 1);
        auto* bkBrowse = new QPushButton(tr("Change…"));
        bkRow->addWidget(bkBrowse);
        auto* bkRescan = new QPushButton(tr("Rescan"));
        bkRow->addWidget(bkRescan);
        v->addLayout(bkRow);
        connect(bkBrowse, &QPushButton::clicked, this, [this, bkPath] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your books folder"),
                                                                  Settings::readingFolder());
            if (dir.isEmpty()) return;
            Settings::setReadingFolder(dir);
            bkPath->setText(dir);
            rescanBookLibrary();
            statusBar()->showMessage(tr("Books folder set to %1 — scanning…").arg(dir), 6000);
        });
        connect(bkRescan, &QPushButton::clicked, this, [this] {
            rescanBookLibrary();
            statusBar()->showMessage(tr("Scanning your books…"), 4000);
        });
        // Online blank-filling (#134 increment 2) - same Settings key/setter and the same sweep call as the
        // themed twin above (GS_TWINS), one write path, no drift.
        auto* bkEnrich = new QCheckBox(tr("Fill in missing book covers and authors online"));
        bkEnrich->setStyleSheet(QStringLiteral("font-size:15px;"));
        bkEnrich->setChecked(Settings::booksEnrichOnline());
        connect(bkEnrich, &QCheckBox::toggled, this, [this](bool c) {
            Settings::setBooksEnrichOnline(c);
            if (c) sweepBookMetadata();
        });
        v->addWidget(bkEnrich);
        v->addSpacing(10);

        auto* pbHeading = new QLabel(tr("Playback"));
        pbHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(pbHeading);
        auto* autoNext = new QCheckBox(tr("Auto-play the next episode"));
        autoNext->setStyleSheet(QStringLiteral("font-size:15px;"));
        autoNext->setChecked(Settings::autoplayNextEpisode());
        connect(autoNext, &QCheckBox::toggled, this, [](bool c) { Settings::setAutoplayNextEpisode(c); });
        v->addWidget(autoNext);

        // Gapless playback (#141) — same Settings key/setter as the themed twin above, one write path, no drift.
        auto* gapless = new QCheckBox(tr("Gapless playback"));
        gapless->setStyleSheet(QStringLiteral("font-size:15px;"));
        gapless->setChecked(Settings::gaplessAudio());
        connect(gapless, &QCheckBox::toggled, this, [](bool c) { Settings::setGaplessAudio(c); });
        v->addWidget(gapless);

        // ReplayGain (issue #141): the classic twins of the themed pb.replaygain / pb.rgpreamp rows. Same
        // Settings keys/setters (playback/replayGain, playback/replayGainPreamp) and the same live re-apply
        // (applyReplayGainLive) — one write path, no drift. Music only; the audiobook/podcast carve-out and the
        // untagged case live in ReplayGain::effectiveMode, not in either builder.
        auto* rgRow = new QHBoxLayout();
        auto* rgLbl = new QLabel(tr("Volume levelling (ReplayGain)"));
        rgLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        auto* replayGain = new QComboBox();
        replayGain->addItem(tr("Off (play as mastered)"),       ReplayGain::idForMode(ReplayGain::Mode::Off));
        replayGain->addItem(tr("Per track (best for shuffle)"), ReplayGain::idForMode(ReplayGain::Mode::Track));
        replayGain->addItem(tr("Per album (default)"),          ReplayGain::idForMode(ReplayGain::Mode::Album));
        replayGain->setCurrentIndex(qMax(0, replayGain->findData(ReplayGain::idForMode(Settings::replayGainMode()))));
        connect(replayGain, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, replayGain](int) {
                    Settings::setReplayGainMode(ReplayGain::modeFromId(replayGain->currentData().toString()));
                    applyReplayGainLive();
                });
        rgRow->addWidget(rgLbl); rgRow->addWidget(replayGain); rgRow->addStretch(1);
        v->addLayout(rgRow);

        auto* rgPreRow = new QHBoxLayout();
        auto* rgPreLbl = new QLabel(tr("Levelling preamp"));
        rgPreLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        auto* rgPreamp = new QComboBox();
        for (int db = int(ReplayGain::minPreampDb()); db <= int(ReplayGain::maxPreampDb()); ++db)
            rgPreamp->addItem(db == 0 ? tr("0 dB (none)")
                                      : QStringLiteral("%1%2 dB").arg(db > 0 ? QStringLiteral("+") : QString()).arg(db),
                              double(db));
        rgPreamp->setCurrentIndex(qMax(0, rgPreamp->findData(Settings::replayGainPreamp())));
        connect(rgPreamp, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, rgPreamp](int) {
                    Settings::setReplayGainPreamp(rgPreamp->currentData().toDouble());
                    applyReplayGainLive();
                });
        rgPreRow->addWidget(rgPreLbl); rgPreRow->addWidget(rgPreamp); rgPreRow->addStretch(1);
        v->addLayout(rgPreRow);
        auto* rgNote = new QLabel(tr("Plays tracks at a matched volume using the ReplayGain tags they already "
                                     "carry — nothing is analysed and no file is ever modified. Per album keeps "
                                     "the loudness differences within a record; per track is better for shuffle. "
                                     "Untagged files, audiobooks and podcasts are left alone."));
        rgNote->setWordWrap(true); rgNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(rgNote);

        // Crossfade (issue #141): the classic twin of the themed pb.crossfade row. Same Settings key/setter
        // (playback/crossfadeSeconds) - one write path, no drift. Off plus the 1-12 s band Crossfade.h defines;
        // WHERE it then applies (music only, never across an album boundary, never longer than the music)
        // lives in Crossfade::secondsFor, not in either builder.
        auto* xfRow = new QHBoxLayout();
        auto* xfLbl = new QLabel(tr("Crossfade between tracks"));
        xfLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        auto* crossfade = new QComboBox();
        crossfade->addItem(tr("Off"), Crossfade::offSeconds());
        for (int sec = Crossfade::minSeconds(); sec <= Crossfade::maxSeconds(); ++sec)
            crossfade->addItem(sec == 1 ? tr("1 second") : tr("%1 seconds").arg(sec), sec);
        crossfade->setCurrentIndex(qMax(0, crossfade->findData(Settings::crossfadeSeconds())));
        connect(crossfade, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [crossfade](int) { Settings::setCrossfadeSeconds(crossfade->currentData().toInt()); });
        xfRow->addWidget(xfLbl); xfRow->addWidget(crossfade); xfRow->addStretch(1);
        v->addLayout(xfRow);
        auto* xfNote = new QLabel(tr("Overlaps the end of one track with the start of the next. Music queues "
                                     "only - audiobooks, podcasts and video are never crossfaded, and neither "
                                     "are two tracks from the same album. Takes effect on the next queue you "
                                     "start."));
        xfNote->setWordWrap(true); xfNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(xfNote);

        // Online lyric lookup (issue #142): the classic twin of the themed pb.onlinelyrics row. Same Settings
        // key/setter (playback/onlineLyrics) — one write path, no drift. WHEN it is consulted (only for a
        // track with no sidecar and no embedded lyrics, only on play) lives in LyricSources::needsOnline and
        // loadTrackLyrics, not in either builder.
        auto* onlineLyrics = new QCheckBox(tr("Look up lyrics online"));
        onlineLyrics->setStyleSheet(QStringLiteral("font-size:15px;"));
        onlineLyrics->setChecked(Settings::onlineLyrics());
        connect(onlineLyrics, &QCheckBox::toggled, this, [](bool c) { Settings::setOnlineLyrics(c); });
        v->addWidget(onlineLyrics);
        auto* lyricsNote = new QLabel(tr("Fetches synced lyrics from LRCLIB, a free community database that "
                                         "needs no account and no key, for tracks that do not already have "
                                         "them. A lyrics file saved next to the song, or lyrics inside the "
                                         "file's own tags, are always used first and are never sent anywhere. "
                                         "Each lookup happens once, while the track plays, and is saved so it "
                                         "works offline after."));
        lyricsNote->setWordWrap(true); lyricsNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(lyricsNote);

        // Same Settings keys/setters as the themed panel — one write path, no drift.
        auto* skipSeg = new QCheckBox(tr("Skip intros and credits"));
        skipSeg->setStyleSheet(QStringLiteral("font-size:15px;"));
        skipSeg->setChecked(Settings::skipSegments());
        connect(skipSeg, &QCheckBox::toggled, this, [](bool c) { Settings::setSkipSegments(c); });
        v->addWidget(skipSeg);

        auto* skipAuto = new QCheckBox(tr("Skip them automatically (no button)"));
        skipAuto->setStyleSheet(QStringLiteral("font-size:15px;"));
        skipAuto->setChecked(Settings::skipSegmentsAuto());
        connect(skipAuto, &QCheckBox::toggled, this, [](bool c) { Settings::setSkipSegmentsAuto(c); });
        v->addWidget(skipAuto);

        // The twin of the themed pb.skipseghint row, word for word and mode-aware the same way: a string that
        // names the driving device in only one of the two settings surfaces is wrong in the other.
        auto* skipHint = new QLabel(
            tr("While a video is playing: %1 skips the offered segment, %2 marks where one starts and ends.")
                .arg(InputMode::instance().hintText(QStringLiteral("S")),
                     InputMode::instance().hintText(QStringLiteral("I"))));
        skipHint->setStyleSheet(QStringLiteral("font-size:13px;color:#999;"));
        skipHint->setWordWrap(true);
        v->addWidget(skipHint);

        // Hardware video decoding (issue #67). Same Settings key/setter as the themed panel above — one write
        // path, no drift. Off/Auto/On map to the mpv hwdec option at player creation (HwDecode::mpvOption).
        auto* hwRow = new QHBoxLayout();
        auto* hwLbl = new QLabel(tr("Hardware video decoding"));
        hwLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        auto* hwdec = new QComboBox();
        hwdec->addItem(tr("Off (software only)"), QStringLiteral("off"));
        hwdec->addItem(tr("Auto (recommended)"),  QStringLiteral("auto"));
        hwdec->addItem(tr("On (full hardware)"),   QStringLiteral("on"));
        hwdec->setCurrentIndex(qMax(0, hwdec->findData(Settings::hwDecode())));
        connect(hwdec, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [hwdec](int) { Settings::setHwDecode(hwdec->currentData().toString()); });
        hwRow->addWidget(hwLbl); hwRow->addWidget(hwdec); hwRow->addStretch(1);
        v->addLayout(hwRow);
        auto* hwNote = new QLabel(tr("Auto prefers safe copy-back hardware decode and falls back to software. "
                                     "Applies to the next video."));
        hwNote->setWordWrap(true); hwNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(hwNote);

        // Seek previews (issue #85): the classic twin of the themed pb.seekpreview row. Same Settings key
        // and setter (previews/cacheMb) - one write path, no drift. Zero megabytes IS "off": nothing is
        // generated at all, which is why there is no separate checkbox to disagree with the size.
        auto* previewRow = new QHBoxLayout();
        auto* previewLbl = new QLabel(tr("Seek preview thumbnails"));
        previewLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        auto* previewCache = new QComboBox();
        previewCache->addItem(tr("Off (never make previews)"), 0);
        previewCache->addItem(tr("Up to 256 MB"), 256);
        previewCache->addItem(tr("Up to 512 MB"), 512);
        previewCache->addItem(tr("Up to 1 GB"), 1024);
        previewCache->addItem(tr("Up to 2 GB"), 2048);
        previewCache->addItem(tr("Up to 5 GB"), 5120);
        previewCache->setCurrentIndex(qMax(0, previewCache->findData(Settings::previewCacheMb())));
        connect(previewCache, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [previewCache](int) { Settings::setPreviewCacheMb(previewCache->currentData().toInt()); });
        previewRow->addWidget(previewLbl); previewRow->addWidget(previewCache); previewRow->addStretch(1);
        v->addLayout(previewRow);
        auto* previewNote = new QLabel(tr("Shows a picture of where you are dragging to on the seek bar. "
                                          "Thumbnails are made in the background, between playbacks, for videos "
                                          "stored on this device only — a stream is never previewed. Older "
                                          "previews are deleted first when the limit is reached."));
        previewNote->setWordWrap(true); previewNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(previewNote);

        // Idle library sweep (issue #302): the classic twin of the themed pb.seekpreviewidle row. Same
        // Settings key and setter (previews/idleScan) - one write path, no drift. It is a second control
        // beside the size rather than another spelling of it: the size says how much this may cost, this
        // says whether the app may go looking for work at all, and with the size at Off it is inert.
        auto* previewIdle = new QCheckBox(tr("Make them ahead of time when idle"));
        previewIdle->setChecked(Settings::previewIdleScan());
        connect(previewIdle, &QCheckBox::toggled, this, [](bool on) { Settings::setPreviewIdleScan(on); });
        v->addWidget(previewIdle);
        auto* previewIdleNote = new QLabel(tr("Goes through the videos in your library while nothing else is "
                                              "happening, so a film has its previews the first time you watch "
                                              "it. One at a time, and it stops the moment you start playing "
                                              "something, a library scan begins or you touch anything. Never "
                                              "while running on battery, and never when this device cannot "
                                              "tell whether it is plugged in."));
        previewIdleNote->setWordWrap(true);
        previewIdleNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(previewIdleNote);

        // Default audiobook/podcast speed (issue #140): the classic twin of the themed pb.defaultspeed row. Same
        // Settings key/setter (playback/defaultSpeed) — one write path, no drift. A book with a remembered
        // per-item speed overrides this, and music always plays at 1x unless explicitly changed.
        auto* defSpeedRow = new QHBoxLayout();
        auto* defSpeedLbl = new QLabel(tr("Default audiobook speed"));
        defSpeedLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        auto* defSpeed = new QComboBox();
        for (const double r : { 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0 })
            defSpeed->addItem(QString::number(r, 'g', 3) + QStringLiteral("×"), r);
        defSpeed->setCurrentIndex(qMax(0, defSpeed->findData(Settings::defaultPlaybackSpeed())));
        connect(defSpeed, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [defSpeed](int) { Settings::setDefaultPlaybackSpeed(defSpeed->currentData().toDouble()); });
        defSpeedRow->addWidget(defSpeedLbl); defSpeedRow->addWidget(defSpeed); defSpeedRow->addStretch(1);
        v->addLayout(defSpeedRow);
        auto* defSpeedNote = new QLabel(tr("Applied to audiobooks and podcasts with no remembered speed. Each book "
                                           "remembers the speed you last chose; music always plays at 1× unless changed."));
        defSpeedNote->setWordWrap(true); defSpeedNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(defSpeedNote);

        // Audio jump interval (issue #140): the classic twin of the themed pb.jump row. Same Settings key/setter
        // (playback/jumpSeconds) — one write path, no drift. Applies to the audio transport only; video seeking
        // is unchanged.
        auto* jumpRow = new QHBoxLayout();
        auto* jumpLbl = new QLabel(tr("Audio jump interval"));
        jumpLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        auto* jumpBox = new QComboBox();
        for (const int s : { 10, 15, 30, 45, 60 })
            jumpBox->addItem(tr("%1 seconds").arg(s), s);
        jumpBox->setCurrentIndex(qMax(0, jumpBox->findData(Settings::audioJumpSeconds())));
        connect(jumpBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [jumpBox](int) { Settings::setAudioJumpSeconds(jumpBox->currentData().toInt()); });
        jumpRow->addWidget(jumpLbl); jumpRow->addWidget(jumpBox); jumpRow->addStretch(1);
        v->addLayout(jumpRow);
        auto* jumpNote = new QLabel(tr("How far the skip-back and skip-forward controls jump in an audiobook or "
                                       "podcast. Video seeking is unchanged."));
        jumpNote->setWordWrap(true); jumpNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(jumpNote);

        // --- Gestures (issue #162): the classic twins of the themed gest.* rows. Same Settings keys, same
        // setters — one write path, no drift (GS_TWINS). No live re-apply is needed on either side: the
        // recogniser rebuilds its Config from these keys on the next press. ---
        auto* gestHeading = new QLabel(tr("Gestures"));
        gestHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(gestHeading);
        auto* gestNote = new QLabel(tr("Swipe, tap and pinch over a playing video. These apply on touch screens "
                                       "only — a mouse, a keyboard and a remote behave exactly as they always have."));
        gestNote->setWordWrap(true); gestNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(gestNote);
        // Spelled out one construction at a time rather than through a factory: the parity gate matches the
        // CLASSIC CONSTRUCTION as a fixed string, so a shared factory would give six rows one indistinguishable
        // twin and the gate would stop asserting which of them exists.
        auto* gestVol = new QCheckBox(tr("Swipe up and down on the right for volume"));
        gestVol->setStyleSheet(QStringLiteral("font-size:15px;"));
        gestVol->setChecked(Settings::gestureVolume());
        connect(gestVol, &QCheckBox::toggled, this, [](bool c) { Settings::setGestureVolume(c); });
        v->addWidget(gestVol);
        auto* gestBright = new QCheckBox(tr("Swipe up and down on the left for brightness"));
        gestBright->setStyleSheet(QStringLiteral("font-size:15px;"));
        gestBright->setChecked(Settings::gestureBrightness());
        connect(gestBright, &QCheckBox::toggled, this, [](bool c) { Settings::setGestureBrightness(c); });
        v->addWidget(gestBright);
        auto* gestSeek = new QCheckBox(tr("Swipe across to scrub"));
        gestSeek->setStyleSheet(QStringLiteral("font-size:15px;"));
        gestSeek->setChecked(Settings::gestureSeek());
        connect(gestSeek, &QCheckBox::toggled, this, [](bool c) { Settings::setGestureSeek(c); });
        v->addWidget(gestSeek);
        auto* gestDouble = new QCheckBox(tr("Double-tap the sides to skip"));
        gestDouble->setStyleSheet(QStringLiteral("font-size:15px;"));
        gestDouble->setChecked(Settings::gestureDoubleTap());
        connect(gestDouble, &QCheckBox::toggled, this, [](bool c) { Settings::setGestureDoubleTap(c); });
        v->addWidget(gestDouble);
        auto* gestHold = new QCheckBox(tr("Hold for double speed"));
        gestHold->setStyleSheet(QStringLiteral("font-size:15px;"));
        gestHold->setChecked(Settings::gestureLongPress());
        connect(gestHold, &QCheckBox::toggled, this, [](bool c) { Settings::setGestureLongPress(c); });
        v->addWidget(gestHold);
        auto* gestPinch = new QCheckBox(tr("Pinch to change how the video fits"));
        gestPinch->setStyleSheet(QStringLiteral("font-size:15px;"));
        gestPinch->setChecked(Settings::gesturePinch());
        connect(gestPinch, &QCheckBox::toggled, this, [](bool c) { Settings::setGesturePinch(c); });
        v->addWidget(gestPinch);
        auto* gestEdgeRow = new QHBoxLayout();
        auto* gestEdgeLbl = new QLabel(tr("Ignore touches near the screen edge"));
        gestEdgeLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        auto* gestEdge = new QComboBox();
        gestEdge->addItem(tr("Off"), 0);
        gestEdge->addItem(tr("16 px"), 16);
        gestEdge->addItem(tr("24 px (default)"), 24);
        gestEdge->addItem(tr("32 px"), 32);
        gestEdge->addItem(tr("48 px"), 48);
        gestEdge->addItem(tr("64 px"), 64);
        gestEdge->setCurrentIndex(qMax(0, gestEdge->findData(Settings::gestureEdgeInset())));
        connect(gestEdge, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [gestEdge](int) { Settings::setGestureEdgeInset(gestEdge->currentData().toInt()); });
        gestEdgeRow->addWidget(gestEdgeLbl); gestEdgeRow->addWidget(gestEdge); gestEdgeRow->addStretch(1);
        v->addLayout(gestEdgeRow);
        auto* gestEdgeNote = new QLabel(tr("A double-tap skips by the same interval as the row above. The edge "
                                           "band is left to the system so its own back and notification swipes "
                                           "still work."));
        gestEdgeNote->setWordWrap(true); gestEdgeNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(gestEdgeNote);

        // Refresh-rate matching, Tier 1 (issue #70): the classic twin of the themed pb.refreshsync row. Same
        // Settings key/setter and the same live re-apply (applyRefreshSyncLive) — one write path, no drift.
        auto* refreshSync = new QCheckBox(tr("Reduce judder (sync video to display)"));
        refreshSync->setStyleSheet(QStringLiteral("font-size:15px;"));
        refreshSync->setChecked(Settings::videoRefreshSync());
        connect(refreshSync, &QCheckBox::toggled, this, [this](bool c) { Settings::setVideoRefreshSync(c);
                                                                         applyRefreshSyncLive(); });
        v->addWidget(refreshSync);
        auto* refreshNote = new QLabel(tr("Resamples audio to lock video to your display's refresh, smoothing the "
                                          "judder of 24fps film on a 60Hz screen. Applies to the next video."));
        refreshNote->setWordWrap(true); refreshNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(refreshNote);

        // HDR output (issue #68): the classic twin of the themed pb.hdr row. Same Settings key/setter ("video/hdr")
        // and the same live re-apply (applyHdrOutputLive) — one write path, no drift. Tone-map / Passthrough map to
        // the mpv output options at player creation and on change (HdrOutput::optionsFor).
        auto* hdrRow = new QHBoxLayout();
        auto* hdrLbl = new QLabel(tr("HDR video"));
        hdrLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        auto* hdr = new QComboBox();
        hdr->addItem(tr("Tone-map to SDR (default)"),            QStringLiteral("tonemap"));
        hdr->addItem(tr("Passthrough when display supports it"), QStringLiteral("passthrough"));
        hdr->setCurrentIndex(qMax(0, hdr->findData(HdrOutput::idForMode(Settings::hdrOutput()))));
        connect(hdr, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, hdr](int) { Settings::setHdrOutput(hdr->currentData().toString()); applyHdrOutputLive(); });
        hdrRow->addWidget(hdrLbl); hdrRow->addWidget(hdr); hdrRow->addStretch(1);
        v->addLayout(hdrRow);
        auto* hdrNote = new QLabel(tr("Tone-map keeps HDR looking right on an SDR screen; Passthrough signals HDR10 "
                                      "to a display that supports it (Windows/Android). Applies to the next video."));
        hdrNote->setWordWrap(true); hdrNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(hdrNote);

        // Play videos with: the built-in player, a detected desktop player (VLC/MPC), or a custom program.
        // Same Settings keys/setters as the themed panel — one write path, no drift. Hidden entirely for a
        // restricted (kids) profile (no external escape hatch), matching the themed panel.
        if (!ProfileStore::current().restricted)
        {
            auto* plRow = new QHBoxLayout();
            auto* plLbl = new QLabel(tr("Play videos with"));
            plLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
            auto* player = new QComboBox();
            player->addItem(tr("Built-in player"), QStringLiteral("builtin"));
#ifdef Q_OS_ANDROID
            player->addItem(tr("Ask another app…"), QStringLiteral("android"));
#else
            for (const ExternalPlayer::Detected& d : ExternalPlayer::detect())
            {
                if      (d.kind == ExternalPlayer::Kind::Vlc) player->addItem(d.display, QStringLiteral("vlc"));
                else if (d.kind == ExternalPlayer::Kind::Mpc) player->addItem(d.display, QStringLiteral("mpc"));
            }
            player->addItem(tr("Custom…"), QStringLiteral("custom"));
#endif
            const QString curKey = Settings::externalPlayer();
            int sel = player->findData(curKey);
            if (sel < 0) { // a configured-but-undetected player: keep it visible
                const QString disp = curKey == QStringLiteral("vlc") ? tr("VLC media player")
                                   : curKey == QStringLiteral("mpc") ? tr("MPC-HC") : QString();
                if (!disp.isEmpty()) { player->addItem(disp, curKey); sel = player->count() - 1; } else sel = 0;
            }
            player->setCurrentIndex(qMax(0, sel));
            auto* plCustom = new QPushButton(tr("Choose custom program…"));
            plRow->addWidget(plLbl); plRow->addWidget(player); plRow->addWidget(plCustom); plRow->addStretch(1);
            v->addLayout(plRow);
            auto* plPath = new QLabel(Settings::externalPlayerPath().isEmpty()
                ? QString() : tr("Custom player: %1").arg(Settings::externalPlayerPath()));
            plPath->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
            v->addWidget(plPath);
            connect(player, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [player](int) { Settings::setExternalPlayer(player->currentData().toString()); });
            connect(plCustom, &QPushButton::clicked, this, [this, player, plPath] {
                const QString exe = QFileDialog::getOpenFileName(this, tr("Choose a media player program"),
                    QString(),
#ifdef Q_OS_WIN
                    tr("Programs (*.exe);;All files (*.*)"));
#else
                    tr("All files (*.*)"));
#endif
                if (exe.isEmpty()) return;
                Settings::setExternalPlayerPath(exe);
                Settings::setExternalPlayer(QStringLiteral("custom"));
                const int ci = player->findData(QStringLiteral("custom"));
                if (ci >= 0) player->setCurrentIndex(ci);
                plPath->setText(tr("Custom player: %1").arg(exe));
            });
        }

        auto* bezel = new QCheckBox(tr("Show bezel / border art around games"));
        bezel->setStyleSheet(QStringLiteral("font-size:15px;"));
        bezel->setChecked(Settings::bezelEnabled());
        connect(bezel, &QCheckBox::toggled, this, [](bool c) { Settings::setBezelEnabled(c); });
        v->addWidget(bezel);
        auto* bezelNote = new QLabel(tr("Drop PNGs into the bezels folder. Per system: bezels/<system>/default.png "
                                        "(or <rom-name>.png for one game); or a global <core>.png / default.png. "
                                        "A matching .cfg or .info file with custom_viewport_* scales the game into "
                                        "the bezel's screen cutout; without one it's drawn as a flat overlay. "
                                        "Decoration packs install alongside as bezels/<system>/<pack>/… and lose "
                                        "to a file you put there yourself."));
        bezelNote->setWordWrap(true); bezelNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(bezelNote);
        // The gallery, beside the folder — the classic twin of the themed "pb.decorations" row. Someone who
        // has just ticked the box above and found an empty folder is standing exactly here.
        auto* bezelPacks = new QPushButton(tr("Browse decoration packs…"));
        connect(bezelPacks, &QPushButton::clicked, this, [this] {
            auto* dlg = new RegistryBrowser(RegistryBrowser::Decorations, nullptr, this);
            showDialogPanel(tr("Browse Decorations"), dlg, [this](int) { openGeneralSettings(); },
                            [this, dlg] {
                if (dlg->isInstalling()) { dlg->closeWhenIdle(); return; }
                openGeneralSettings();
            });
        });
        auto* bezelOpen = new QPushButton(tr("Open bezels folder"));
        connect(bezelOpen, &QPushButton::clicked, this, [] {
            const QString d = AppPaths::dataDir() + QStringLiteral("/bezels");
            QDir().mkpath(d);
            QDesktopServices::openUrl(QUrl::fromLocalFile(d));
        });
        auto* bezelRow = new QHBoxLayout();
        bezelRow->addWidget(bezelPacks);
        bezelRow->addWidget(bezelOpen);
        bezelRow->addStretch(1);
        v->addLayout(bezelRow);
        v->addSpacing(10);

        // --- Audio output (issue #69): the classic twins of the themed audio.device / audio.passthrough /
        // audio.exclusive rows. Same audio/* keys, same setters, same live re-apply (applyAudioOutputLive) as the
        // themed rows — one write path, no drift. DEVICE-LOCAL (audio/* is in CloudSync's carve-out). A user who
        // has not enabled the themed home reaches the whole group here. ---
        auto* audioHeading = new QLabel(tr("Audio"));
        audioHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(audioHeading);

        // Output device: "Auto (system default)" (stored id "") plus each device mpv enumerates. A stored id the
        // current machine no longer sees is kept visible. Same list the themed picker builds.
        auto* audioDevRow = new QHBoxLayout();
        audioDevRow->addWidget(new QLabel(tr("Output device")));
        auto* audioDev = new QComboBox();
        audioDev->setMinimumHeight(30);
        audioDev->addItem(tr("Auto (system default)"), QString());
        if (player_)
            for (const MpvWidget::AudioDevice& d : player_->availableAudioDevices())
                audioDev->addItem(d.description.isEmpty() ? d.name : d.description, d.name);
        {
            int i = audioDev->findData(Settings::audioDevice());
            if (i < 0 && !Settings::audioDevice().isEmpty()) {
                audioDev->addItem(Settings::audioDevice(), Settings::audioDevice()); i = audioDev->count() - 1;
            }
            audioDev->setCurrentIndex(qMax(0, i));
        }
        connect(audioDev, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, audioDev](int) { Settings::setAudioDevice(audioDev->currentData().toString());
                                        applyAudioOutputLive(); });
        audioDevRow->addWidget(audioDev, 1);
        v->addLayout(audioDevRow);

        // Passthrough (bitstream Dolby/DTS to the receiver, no PCM decode).
        auto* audioPass = new QCheckBox(tr("Passthrough (bitstream to receiver)"));
        audioPass->setStyleSheet(QStringLiteral("font-size:15px;"));
        audioPass->setChecked(Settings::audioPassthrough());
        connect(audioPass, &QCheckBox::toggled, this, [this](bool c) { Settings::setAudioPassthrough(c);
                                                                       applyAudioOutputLive(); });
        v->addWidget(audioPass);

        // Exclusive mode (sole control of the device — bit-perfect, bypasses the OS mixer).
        auto* audioExcl = new QCheckBox(tr("Exclusive mode (bit-perfect)"));
        audioExcl->setStyleSheet(QStringLiteral("font-size:15px;"));
        audioExcl->setChecked(Settings::audioExclusive());
        connect(audioExcl, &QCheckBox::toggled, this, [this](bool c) { Settings::setAudioExclusive(c);
                                                                       applyAudioOutputLive(); });
        v->addWidget(audioExcl);

        auto* audioNote = new QLabel(tr("Passthrough sends Dolby/DTS untouched to an AV receiver instead of "
                                        "decoding to stereo — while it is on, volume boost and pitch-corrected "
                                        "speed don't apply. Exclusive mode takes sole control of the device for "
                                        "bit-perfect output. Both apply to the next audio you play."));
        audioNote->setWordWrap(true); audioNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(audioNote);
        v->addSpacing(10);

        auto* heading = new QLabel(tr("Subtitles"));
        heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(heading);

        auto* on = new QCheckBox(tr("Show subtitles by default"));
        on->setStyleSheet(QStringLiteral("font-size:15px;"));
        on->setChecked(Settings::subtitlesOnByDefault());
        v->addWidget(on);
        connect(on, &QCheckBox::toggled, this, [](bool c) { Settings::setSubtitlesOnByDefault(c); }); // save on change

        auto* note = new QLabel(tr("Applies to the next video. Subtitles still toggle in-player with the CC button."));
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(note);

        // --- Subtitle appearance (issue #71): the classic twins of the themed subs.font / subs.size /
        // subs.color / subs.bordersize / subs.bordercolor / subs.box / subs.boxopacity / subs.pos / subs.bold /
        // subs.override rows. Same subs/* keys, same setters, same live re-apply (applySubtitleStyleLive) as the
        // themed rows — one write path, no drift. Applies to plain-text (SRT) subtitles; styled ASS/SSA keep
        // their own look unless "Override styled" is on. A user who has not enabled the themed home reaches the
        // whole style here. ---
        v->addSpacing(6);
        auto* subStyleHeading = new QLabel(tr("Subtitle style"));
        subStyleHeading->setStyleSheet(QStringLiteral("font-size:15px;font-weight:bold;"));
        v->addWidget(subStyleHeading);

        // Font (system families; "Default" => mpv's own family).
        auto* subFontRow = new QHBoxLayout();
        subFontRow->addWidget(new QLabel(tr("Font")));
        auto* subFont = new QComboBox();
        subFont->setMinimumHeight(30);
        subFont->addItem(tr("Default"), QString());
        for (const QString& fam : QFontDatabase::families()) subFont->addItem(fam, fam);
        {
            int i = subFont->findData(Settings::subtitleFont());
            if (i < 0 && !Settings::subtitleFont().isEmpty()) {
                subFont->addItem(Settings::subtitleFont(), Settings::subtitleFont()); i = subFont->count() - 1;
            }
            subFont->setCurrentIndex(qMax(0, i));
        }
        connect(subFont, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, subFont](int) { Settings::setSubtitleFont(subFont->currentData().toString());
                                       applySubtitleStyleLive(); });
        subFontRow->addWidget(subFont, 1);
        v->addLayout(subFontRow);

        // Size (percent -> sub-scale; the SAME size notion the in-player stepper drives, not a second one).
        auto* subSizeRow = new QHBoxLayout();
        subSizeRow->addWidget(new QLabel(tr("Size")));
        auto* subSize = new QComboBox();
        subSize->setMinimumHeight(30);
        for (int p : { 50, 75, 90, 100, 110, 125, 150, 175, 200, 250, 300 }) subSize->addItem(QStringLiteral("%1%").arg(p), p);
        {
            int i = subSize->findData(Settings::subtitleSizePercent());
            if (i < 0) { subSize->addItem(QStringLiteral("%1%").arg(Settings::subtitleSizePercent()),
                                          Settings::subtitleSizePercent()); i = subSize->count() - 1; }
            subSize->setCurrentIndex(i);
        }
        connect(subSize, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, subSize](int) { Settings::setSubtitleSizePercent(subSize->currentData().toInt());
                                       applySubtitleStyleLive(); });
        subSizeRow->addWidget(subSize, 1);
        v->addLayout(subSizeRow);

        // A palette shared by the two colour combos (display -> #RRGGBB in the item data).
        const QList<QPair<QString, QString>> subColorPairs = {
            { tr("White"), QStringLiteral("#FFFFFF") },   { tr("Yellow"), QStringLiteral("#FFFF00") },
            { tr("Black"), QStringLiteral("#000000") },   { tr("Red"),    QStringLiteral("#FF0000") },
            { tr("Green"), QStringLiteral("#00FF00") },   { tr("Cyan"),   QStringLiteral("#00FFFF") },
            { tr("Magenta"), QStringLiteral("#FF00FF") }, { tr("Grey"),   QStringLiteral("#808080") },
        };
        auto fillColorCombo = [&subColorPairs](QComboBox* box, const QString& cur) {
            for (const auto& c : subColorPairs) box->addItem(c.first, c.second);
            int i = box->findData(cur);
            if (i < 0 && !cur.isEmpty()) { box->addItem(cur, cur); i = box->count() - 1; }  // keep an unlisted hex
            box->setCurrentIndex(qMax(0, i));
        };

        // Text colour.
        auto* subColorRow = new QHBoxLayout();
        subColorRow->addWidget(new QLabel(tr("Text colour")));
        auto* subColor = new QComboBox();
        subColor->setMinimumHeight(30);
        fillColorCombo(subColor, Settings::subtitleColor());
        connect(subColor, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, subColor](int) { Settings::setSubtitleColor(subColor->currentData().toString());
                                        applySubtitleStyleLive(); });
        subColorRow->addWidget(subColor, 1);
        v->addLayout(subColorRow);

        // Outline thickness.
        auto* subBorderSizeRow = new QHBoxLayout();
        subBorderSizeRow->addWidget(new QLabel(tr("Outline thickness")));
        auto* subBorderSize = new QComboBox();
        subBorderSize->setMinimumHeight(30);
        for (int p : { 0, 1, 2, 3, 4, 5, 6 }) subBorderSize->addItem(QString::number(p), p);
        {
            int i = subBorderSize->findData(Settings::subtitleBorderSize());
            if (i < 0) { subBorderSize->addItem(QString::number(Settings::subtitleBorderSize()),
                                                Settings::subtitleBorderSize()); i = subBorderSize->count() - 1; }
            subBorderSize->setCurrentIndex(i);
        }
        connect(subBorderSize, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, subBorderSize](int) { Settings::setSubtitleBorderSize(subBorderSize->currentData().toInt());
                                             applySubtitleStyleLive(); });
        subBorderSizeRow->addWidget(subBorderSize, 1);
        v->addLayout(subBorderSizeRow);

        // Outline colour.
        auto* subBorderColorRow = new QHBoxLayout();
        subBorderColorRow->addWidget(new QLabel(tr("Outline colour")));
        auto* subBorderColor = new QComboBox();
        subBorderColor->setMinimumHeight(30);
        fillColorCombo(subBorderColor, Settings::subtitleBorderColor());
        connect(subBorderColor, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, subBorderColor](int) { Settings::setSubtitleBorderColor(subBorderColor->currentData().toString());
                                              applySubtitleStyleLive(); });
        subBorderColorRow->addWidget(subBorderColor, 1);
        v->addLayout(subBorderColorRow);

        // Background box on/off.
        auto* subBox = new QCheckBox(tr("Show a background box behind subtitles"));
        subBox->setStyleSheet(QStringLiteral("font-size:15px;"));
        subBox->setChecked(Settings::subtitleBox());
        connect(subBox, &QCheckBox::toggled, this, [this](bool c) { Settings::setSubtitleBox(c);
                                                                    applySubtitleStyleLive(); });
        v->addWidget(subBox);

        // Background box opacity.
        auto* subBoxOpacityRow = new QHBoxLayout();
        subBoxOpacityRow->addWidget(new QLabel(tr("Background box opacity")));
        auto* subBoxOpacity = new QComboBox();
        subBoxOpacity->setMinimumHeight(30);
        for (int p : { 25, 50, 75, 90, 100 }) subBoxOpacity->addItem(QStringLiteral("%1%").arg(p), p);
        {
            int i = subBoxOpacity->findData(Settings::subtitleBoxOpacity());
            if (i < 0) { subBoxOpacity->addItem(QStringLiteral("%1%").arg(Settings::subtitleBoxOpacity()),
                                                Settings::subtitleBoxOpacity()); i = subBoxOpacity->count() - 1; }
            subBoxOpacity->setCurrentIndex(i);
        }
        connect(subBoxOpacity, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, subBoxOpacity](int) { Settings::setSubtitleBoxOpacity(subBoxOpacity->currentData().toInt());
                                             applySubtitleStyleLive(); });
        subBoxOpacityRow->addWidget(subBoxOpacity, 1);
        v->addLayout(subBoxOpacityRow);

        // Vertical position (0 = top … 100 = bottom).
        auto* subPosRow = new QHBoxLayout();
        subPosRow->addWidget(new QLabel(tr("Vertical position")));
        auto* subPos = new QComboBox();
        subPos->setMinimumHeight(30);
        const QList<QPair<QString, int>> subPosPairs = {
            { tr("Top"), 0 }, { tr("Upper"), 25 }, { tr("Middle"), 50 }, { tr("Lower"), 75 }, { tr("Bottom"), 100 },
        };
        for (const auto& p : subPosPairs) subPos->addItem(p.first, p.second);
        {
            int i = subPos->findData(Settings::subtitlePosition());
            if (i < 0) { subPos->addItem(QString::number(Settings::subtitlePosition()),
                                         Settings::subtitlePosition()); i = subPos->count() - 1; }
            subPos->setCurrentIndex(i);
        }
        connect(subPos, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, subPos](int) { Settings::setSubtitlePosition(subPos->currentData().toInt());
                                      applySubtitleStyleLive(); });
        subPosRow->addWidget(subPos, 1);
        v->addLayout(subPosRow);

        // Bold.
        auto* subBold = new QCheckBox(tr("Bold subtitles"));
        subBold->setStyleSheet(QStringLiteral("font-size:15px;"));
        subBold->setChecked(Settings::subtitleBold());
        connect(subBold, &QCheckBox::toggled, this, [this](bool c) { Settings::setSubtitleBold(c);
                                                                     applySubtitleStyleLive(); });
        v->addWidget(subBold);

        // Override styled subtitles (ASS/SSA). DEFAULT OFF — leaving it off preserves each styled sub's own
        // typography; on maps to mpv sub-ass-override=force so the plain style above applies to ASS too.
        auto* subOverride = new QCheckBox(tr("Override styled (ASS/SSA) subtitles"));
        subOverride->setStyleSheet(QStringLiteral("font-size:15px;"));
        subOverride->setChecked(Settings::subtitleOverrideStyled());
        connect(subOverride, &QCheckBox::toggled, this, [this](bool c) { Settings::setSubtitleOverrideStyled(c);
                                                                         applySubtitleStyleLive(); });
        v->addWidget(subOverride);
        auto* subStyleNote = new QLabel(tr("Font, colour, outline, box and position apply to plain-text (SRT) "
                                           "subtitles. Styled subtitles keep their own look unless you override them."));
        subStyleNote->setWordWrap(true);
        subStyleNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(subStyleNote);

        // --- Reading (issue #135): the classic twins of the themed reader.font / reader.size / reader.spacing /
        // reader.margin / reader.justify / reader.theme rows. Same reader/* (and ebook/fontSize) keys, same
        // setters, same live re-apply (applyReaderTypographyLive) as the themed rows — one write path, no drift.
        // A user who has not enabled the themed home reaches the whole reader typography here. ---
        v->addSpacing(6);
        auto* readingHeading = new QLabel(tr("Reading"));
        readingHeading->setStyleSheet(QStringLiteral("font-size:15px;font-weight:bold;"));
        v->addWidget(readingHeading);

        // Font (system families; "Default" => the reader's own default family).
        auto* readerFontRow = new QHBoxLayout();
        readerFontRow->addWidget(new QLabel(tr("Font")));
        auto* readerFont = new QComboBox();
        readerFont->setMinimumHeight(30);
        readerFont->addItem(tr("Default"), QString());
        for (const QString& fam : QFontDatabase::families()) readerFont->addItem(fam, fam);
        {
            int i = readerFont->findData(Settings::readerFont());
            if (i < 0 && !Settings::readerFont().isEmpty()) {
                readerFont->addItem(Settings::readerFont(), Settings::readerFont()); i = readerFont->count() - 1;
            }
            readerFont->setCurrentIndex(qMax(0, i));
        }
        connect(readerFont, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, readerFont](int) { Settings::setReaderFont(readerFont->currentData().toString());
                                          applyReaderTypographyLive(); });
        readerFontRow->addWidget(readerFont, 1);
        v->addLayout(readerFontRow);

        // Size (points; the SAME size the in-reader A+/A− stepper drives — one notion of reading size).
        auto* readerSizeRow = new QHBoxLayout();
        readerSizeRow->addWidget(new QLabel(tr("Size")));
        auto* readerSize = new QComboBox();
        readerSize->setMinimumHeight(30);
        for (int p : { 8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40 })
            readerSize->addItem(QStringLiteral("%1 pt").arg(p), p);
        {
            int i = readerSize->findData(Settings::readerFontSize());
            if (i < 0) { readerSize->addItem(QStringLiteral("%1 pt").arg(Settings::readerFontSize()),
                                             Settings::readerFontSize()); i = readerSize->count() - 1; }
            readerSize->setCurrentIndex(i);
        }
        connect(readerSize, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, readerSize](int) { Settings::setReaderFontSize(readerSize->currentData().toInt());
                                          applyReaderTypographyLive(); });
        readerSizeRow->addWidget(readerSize, 1);
        v->addLayout(readerSizeRow);

        // Line spacing (percent of natural leading).
        auto* readerSpacingRow = new QHBoxLayout();
        readerSpacingRow->addWidget(new QLabel(tr("Line spacing")));
        auto* readerSpacing = new QComboBox();
        readerSpacing->setMinimumHeight(30);
        for (int p : { 100, 115, 130, 150, 175, 200, 250 }) readerSpacing->addItem(QStringLiteral("%1%").arg(p), p);
        {
            int i = readerSpacing->findData(Settings::readerLineSpacing());
            if (i < 0) { readerSpacing->addItem(QStringLiteral("%1%").arg(Settings::readerLineSpacing()),
                                                Settings::readerLineSpacing()); i = readerSpacing->count() - 1; }
            readerSpacing->setCurrentIndex(i);
        }
        connect(readerSpacing, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, readerSpacing](int) { Settings::setReaderLineSpacing(readerSpacing->currentData().toInt());
                                             applyReaderTypographyLive(); });
        readerSpacingRow->addWidget(readerSpacing, 1);
        v->addLayout(readerSpacingRow);

        // Margins (percent of page width).
        auto* readerMarginRow = new QHBoxLayout();
        readerMarginRow->addWidget(new QLabel(tr("Margins")));
        auto* readerMargin = new QComboBox();
        readerMargin->setMinimumHeight(30);
        for (int p : { 0, 3, 6, 9, 12, 15, 20, 25 }) readerMargin->addItem(QStringLiteral("%1%").arg(p), p);
        {
            int i = readerMargin->findData(Settings::readerMargin());
            if (i < 0) { readerMargin->addItem(QStringLiteral("%1%").arg(Settings::readerMargin()),
                                               Settings::readerMargin()); i = readerMargin->count() - 1; }
            readerMargin->setCurrentIndex(i);
        }
        connect(readerMargin, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, readerMargin](int) { Settings::setReaderMargin(readerMargin->currentData().toInt());
                                            applyReaderTypographyLive(); });
        readerMarginRow->addWidget(readerMargin, 1);
        v->addLayout(readerMarginRow);

        // Reading theme (Light / Sepia / Dark / True black), stored as the enum's int.
        auto* readerThemeRow = new QHBoxLayout();
        readerThemeRow->addWidget(new QLabel(tr("Reading theme")));
        auto* readerTheme = new QComboBox();
        readerTheme->setMinimumHeight(30);
        const QList<QPair<QString, ReaderTypography::Theme>> readerThemePairs = {
            { tr("Light"),      ReaderTypography::Theme::Light },
            { tr("Sepia"),      ReaderTypography::Theme::Sepia },
            { tr("Dark"),       ReaderTypography::Theme::Dark },
            { tr("True black"), ReaderTypography::Theme::TrueBlack },
        };
        for (const auto& t : readerThemePairs) readerTheme->addItem(t.first, ReaderTypography::themeToInt(t.second));
        readerTheme->setCurrentIndex(qMax(0, readerTheme->findData(ReaderTypography::themeToInt(Settings::readerTheme()))));
        connect(readerTheme, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, readerTheme](int) {
                    Settings::setReaderTheme(ReaderTypography::themeFromInt(readerTheme->currentData().toInt()));
                    applyReaderTypographyLive(); });
        readerThemeRow->addWidget(readerTheme, 1);
        v->addLayout(readerThemeRow);

        // Justify.
        auto* readerJustify = new QCheckBox(tr("Justify text"));
        readerJustify->setStyleSheet(QStringLiteral("font-size:15px;"));
        readerJustify->setChecked(Settings::readerJustify());
        connect(readerJustify, &QCheckBox::toggled, this, [this](bool c) { Settings::setReaderJustify(c);
                                                                           applyReaderTypographyLive(); });
        v->addWidget(readerJustify);
        // Touch reading (issue #147): the classic twins of the themed reader.zones / reader.swipe /
        // reader.keepawake / reader.dualpage rows. Same keys, same setters, same live re-apply — one write
        // path, no drift. A user who has not enabled the themed home reaches all four here.
        auto* readerZonesRow = new QHBoxLayout();
        readerZonesRow->addWidget(new QLabel(tr("Tap zones")));
        auto* readerZones = new QComboBox();
        readerZones->setMinimumHeight(30);
        readerZones->addItem(tr("Left goes back, right goes forward"), 0);
        readerZones->addItem(tr("Left goes forward, right goes back"), 1);
        readerZones->addItem(tr("Every tap opens the menu (swipe to turn pages)"), 2);
        readerZones->setCurrentIndex(qMax(0, readerZones->findData(Settings::readerTapZones())));
        connect(readerZones, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [readerZones](int) { Settings::setReaderTapZones(readerZones->currentData().toInt()); });
        readerZonesRow->addWidget(readerZones, 1);
        v->addLayout(readerZonesRow);

        auto* readerSwipe = new QCheckBox(tr("Swipe sideways to turn the page"));
        readerSwipe->setStyleSheet(QStringLiteral("font-size:15px;"));
        readerSwipe->setChecked(Settings::readerSwipePaging());
        connect(readerSwipe, &QCheckBox::toggled, this, [](bool c) { Settings::setReaderSwipePaging(c); });
        v->addWidget(readerSwipe);

        auto* readerAwake = new QCheckBox(tr("Keep the screen on while reading"));
        readerAwake->setStyleSheet(QStringLiteral("font-size:15px;"));
        readerAwake->setChecked(Settings::readerKeepAwake());
        connect(readerAwake, &QCheckBox::toggled, this, [this](bool c) { Settings::setReaderKeepAwake(c);
                                                                        applyReaderTypographyLive(); });
        v->addWidget(readerAwake);

        auto* readerDual = new QCheckBox(tr("Two pages side by side on a wide screen"));
        readerDual->setStyleSheet(QStringLiteral("font-size:15px;"));
        readerDual->setChecked(Settings::readerDualPage());
        connect(readerDual, &QCheckBox::toggled, this, [this](bool c) { Settings::setReaderDualPage(c);
                                                                       applyReaderTypographyLive(); });
        v->addWidget(readerDual);

        auto* readerTouchNote = new QLabel(tr("Tap zones and swiping apply on touch screens only — a mouse, a "
                                              "keyboard and a remote behave exactly as they always have, and a "
                                              "tap across the top of the page always opens the menu. Two pages "
                                              "side by side applies to books whenever the window is wider than "
                                              "it is tall."));
        readerTouchNote->setWordWrap(true);
        readerTouchNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(readerTouchNote);

        // In-book lookup (issue #137): the classic twins of the themed reader.translate TextField and
        // reader.vocab Action rows. Same Settings key, same setter, same list surface — one write path, no
        // drift (GS_TWINS). The privacy paragraph is the twin of the themed reader.lookuphint Info row, and
        // it says the same thing in the same words: a lookup is a network call, and only a verb press makes
        // one happen.
        auto* rlNote = new QLabel(tr("Select a word while reading and the menu offers Define, Wikipedia and "
            "Translate. Each one is a NETWORK REQUEST that sends the words you selected — to Wiktionary, to "
            "Wikipedia, or to the translation service you name below. Nothing is ever looked up just because "
            "you selected it: it takes pressing one of those three. Words you look up are kept in the list "
            "below, on this profile, and sync with your other devices. Leave the translation address empty "
            "and the Translate verb is not offered at all."));
        rlNote->setWordWrap(true);
        rlNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(rlNote);
        auto* rlRow = new QHBoxLayout();
        auto* rlLbl = new QLabel(tr("Translation service (LibreTranslate address)"));
        rlLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        rlRow->addWidget(rlLbl);
        auto* rlTranslate = new QLineEdit(Settings::readerTranslateEndpoint());
        rlTranslate->setMinimumHeight(34);
        rlRow->addWidget(rlTranslate, 1);
        v->addLayout(rlRow);
        // editingFinished, not textChanged: a half-typed URL is not an endpoint, and there is nothing to
        // re-apply per keystroke — the reader reads this key at the moment a verb is pressed.
        connect(rlTranslate, &QLineEdit::editingFinished, this, [rlTranslate] {
            if (rlTranslate->text().trimmed() == Settings::readerTranslateEndpoint()) return;  // focus out, no edit
            Settings::setReaderTranslateEndpoint(rlTranslate->text());
        });
        auto* rlVocab = new QPushButton(tr("Words I looked up…"));
        connect(rlVocab, &QPushButton::clicked, this, [this] { openVocabularyList(); });
        v->addWidget(rlVocab);
        v->addSpacing(10);

        auto* readerNote = new QLabel(tr("Font, size, spacing, margins and theme for ebooks. Changes reflow the "
                                         "page but keep your place."));
        readerNote->setWordWrap(true);
        readerNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(readerNote);
        // Read aloud (issue #145) — the classic twin of the themed readaloud.hint info row. Same words, same
        // point: the voices come from the system, and their quality is the system's to answer for.
        auto* readAloudNote = new QLabel(tr("Read aloud (in the reader's controls) speaks with your system's own "
                                            "voices — add more in your operating system's speech settings. "
                                            "Quality varies a lot by platform; nothing is downloaded and no "
                                            "voice is bundled."));
        readAloudNote->setWordWrap(true);
        readAloudNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(readAloudNote);

        // --- Auto-download subtitles (OpenSubtitles.com). When a movie/episode has no subtitle in the chosen
        // language, fetch one automatically. Needs a free API key + the user's account (login is required to
        // download). All three blank => the feature stays off. ---
        v->addSpacing(10);
        auto* osHeading = new QLabel(tr("Auto-download from OpenSubtitles"));
        osHeading->setStyleSheet(QStringLiteral("font-size:15px;font-weight:bold;"));
        v->addWidget(osHeading);
        auto* osNote = new QLabel(tr("When “show subtitles by default” is on and a video has none in your "
                                     "language, fetch one automatically. Searching needs only an API key: "
                                     "get a free one at opensubtitles.com (Consumers → New consumer), or use "
                                     "the one built into this release where there is one. Downloading needs "
                                     "your OpenSubtitles account; the first download asks for it."));
        osNote->setWordWrap(true);
        osNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(osNote);

        // builtin (#81): the value in use is the one built into this release, so the EMPTY field says so through
        // its placeholder. The field still holds the user's own value and stays editable; typing wins.
        auto addCredRow = [this, v](const QString& label, const QString& value, bool secret,
                                    std::function<void(const QString&)> save, bool builtin = false) {
            auto* row = new QHBoxLayout();
            auto* l = new QLabel(label); l->setMinimumWidth(90);
            row->addWidget(l);
            auto* edit = new QLineEdit(value);
            edit->setMinimumHeight(30);
            if (secret) edit->setEchoMode(QLineEdit::Password);
            if (builtin) edit->setPlaceholderText(tr("Built in (you can use your own)"));
            connect(edit, &QLineEdit::textChanged, this, [save](const QString& t) { save(t); }); // save as typed
            row->addWidget(edit, 1);
            v->addLayout(row);
        };
        addCredRow(tr("API key:"), Settings::openSubApiKey(), false,
                   [](const QString& t) { Settings::setOpenSubApiKey(t); }, SubtitleFetcher::usingBuiltinKey());
        addCredRow(tr("Username:"), Settings::openSubUsername(), false,
                   [](const QString& t) { Settings::setOpenSubUsername(t); });
        addCredRow(tr("Password:"), Settings::openSubPassword(), true,
                   [](const QString& t) { Settings::setOpenSubPassword(t); });

        // --- Trakt.tv scrobbling: mark movies/episodes watched on your Trakt profile as you play them. ---
        v->addSpacing(12);
        auto* tkHeading = new QLabel(tr("Trakt.tv"));
        tkHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(tkHeading);
        auto* tkNote = new QLabel(tr("Sync what you watch to your Trakt profile (movies + episodes are marked "
                                     "watched automatically). Create a free API app at trakt.tv/oauth/applications "
                                     "(redirect URI: urn:ietf:wg:oauth:2.0:oob), paste its Client ID + Secret, then "
                                     "Connect."));
        tkNote->setWordWrap(true);
        tkNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(tkNote);
        addCredRow(tr("Client ID:"), Settings::traktClientId(), false,
                   [](const QString& t) { Settings::setTraktClientId(t); }, TraktClient::usingBuiltin());
        addCredRow(tr("Client secret:"), Settings::traktClientSecret(), true,
                   [](const QString& t) { Settings::setTraktClientSecret(t); },
                   TraktClient::usingBuiltin() && !TraktClient::appCredentials().secret.isEmpty());

        auto* tkStatus = new QLabel(TraktClient::connected() ? tr("✓ Connected to Trakt.") : tr("Not connected."));
        tkStatus->setWordWrap(true);
        tkStatus->setStyleSheet(QStringLiteral("font-size:13px;color:#bbb;"));
        auto* tkBtn = new QPushButton(TraktClient::connected() ? tr("Disconnect") : tr("Connect to Trakt"));
        tkBtn->setMinimumHeight(32);
        auto* tkRow = new QHBoxLayout(); tkRow->addWidget(tkBtn); tkRow->addStretch(1);
        v->addLayout(tkRow);
        v->addWidget(tkStatus);
        // Wire this panel's Trakt signals; disconnected when the panel is torn down (the labels are its children).
        connect(trakt_, &TraktClient::deviceCode, tkStatus, [tkStatus](const QString& code, const QString& url) {
            tkStatus->setText(tr("Go to %1 and enter code:  %2").arg(url, code)); });
        connect(trakt_, &TraktClient::connectError, tkStatus, [tkStatus](const QString& m) { tkStatus->setText(m); });
        connect(trakt_, &TraktClient::connectedChanged, tkBtn, [tkBtn, tkStatus](bool on) {
            tkBtn->setText(on ? tr("Disconnect") : tr("Connect to Trakt"));
            tkStatus->setText(on ? tr("✓ Connected to Trakt.") : tr("Not connected.")); });
        // The watched-history import (#23): the twin of the themed builder's "trakt.backfill" row.
        auto* tkBackfill = new QPushButton(tr("Import watched history from Trakt"));
        tkBackfill->setMinimumHeight(32);
        auto* tkBfRow = new QHBoxLayout(); tkBfRow->addWidget(tkBackfill); tkBfRow->addStretch(1);
        v->addLayout(tkBfRow);
        // The note says what running it AGAIN does, and — since this branch — what running it again
        // does NOT do. "Only picks up what is new" was true of the mechanism and misleading about the
        // outcome: a watch Trakt gains later carrying an OLDER date is not new by that test and is
        // never imported. The re-import button below is the answer, and the note points at it rather
        // than leaving the user to conclude the import is broken.
        auto* tkBfNote = new QLabel(tr("Marks everything Trakt says you have watched. It never clears a "
                                       "mark and never overrides one you set yourself. Running it again "
                                       "considers only watches NEWER than your last import — if something "
                                       "you added to Trakt later (a backdated check-in, a Letterboxd or "
                                       "Netflix import) never appears, use Re-import everything."));
        tkBfNote->setWordWrap(true);
        tkBfNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(tkBfNote);
        connect(tkBackfill, &QPushButton::clicked, this, [this] { runTraktBackfill(); });

        // The twin of the themed builder's "trakt.reimport" row. A user-facing action has to exist in
        // BOTH builders or it is unreachable in one of the two modes — and this one is the only escape
        // from the watermark, so being unreachable would leave that mode with no way out at all.
        auto* tkReimport = new QPushButton(tr("Re-import everything from Trakt"));
        tkReimport->setMinimumHeight(32);
        auto* tkReRow = new QHBoxLayout(); tkReRow->addWidget(tkReimport); tkReRow->addStretch(1);
        v->addLayout(tkReRow);
        connect(tkReimport, &QPushButton::clicked, this, [this] { reimportTraktHistory(); });

        // ...and the twin of "trakt.data": the same line, from the same pure builder, so the two
        // builders cannot tell the user different things about the same state. It is the only place
        // the import's watermark is visible.
        auto* tkData = new QLabel(traktStatusLine());
        tkData->setWordWrap(true);
        tkData->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(tkData);
        // While THIS panel is up it owns the refresh hook; the themed builder installs its own when it
        // presents. Guarded by a QPointer so a run landing after the panel is destroyed writes nowhere.
        {
            QPointer<QLabel> guard(tkData);
            traktStatusUpdate_ = [guard] { if (guard) guard->setText(MainWindow::traktStatusLine()); };
        }

        connect(tkBtn, &QPushButton::clicked, this, [this, tkStatus] {
            if (TraktClient::connected()) { trakt_->disconnectAccount(); return; }
            if (!TraktClient::configured()) { tkStatus->setText(tr("Enter your Client ID and Secret first.")); return; }
            tkStatus->setText(tr("Requesting a code from Trakt…"));
            trakt_->connectAccount();
        });

        // --- AniList (issue #156): the twins of the themed builder's anilist.* rows. A user-facing setting
        // has to exist in BOTH surfaces or it is simply unreachable in one mode. ---
        v->addSpacing(12);
        // The twin of the themed builder's call (issue #328): the dropped-update notices are taken on open
        // here too, or they would be shown on one layout and silently expire on the other.
        trackerPanelOpened();
        auto* alHeading = new QLabel(tr("AniList (anime and manga)"));
        alHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(alHeading);
        auto* alNote = new QLabel(tr("Sync chapters read and episodes watched to your AniList list. Create a "
                                     "free API client at anilist.co (Settings, Developer, Create New Client), "
                                     "set its redirect URL to the loopback address 127.0.0.1 , paste the "
                                     "Client ID and "
                                     "Secret below, then Connect. Trakt above keeps film and general TV; this "
                                     "keeps anime and manga. Your AniList credentials stay on this device and "
                                     "are never included in cloud sync."));
        alNote->setWordWrap(true);
        alNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(alNote);
        // The labels are qualified with "AniList" rather than reusing Trakt's "Client ID:" so the two pairs
        // are distinguishable on a form that now carries both - and so the parity gate's twin patterns name
        // exactly one control each.
        // The TYPED values (#81) — clientId() would read a built-in client back out into the field.
        addCredRow(tr("AniList Client ID:"), AniListTracker::typedClientId(), false,
                   [](const QString& t) { AniListTracker::setClientId(t); }, AniListTracker::usingBuiltin());
        addCredRow(tr("AniList Client secret:"), AniListTracker::typedClientSecret(), true,
                   [](const QString& t) { AniListTracker::setClientSecret(t); },
                   AniListTracker::usingBuiltin() && !AniListTracker::clientSecret().isEmpty());

        auto* alStatus = new QLabel(AniListTracker::isConnected() ? tr("\u2713 Connected to AniList.")
                                                                  : tr("Not connected."));
        alStatus->setWordWrap(true);
        alStatus->setStyleSheet(QStringLiteral("font-size:13px;color:#bbb;"));
        auto* alBtn = new QPushButton(AniListTracker::isConnected() ? tr("Disconnect")
                                                                    : tr("Connect to AniList"));
        alBtn->setMinimumHeight(32);
        auto* alRow = new QHBoxLayout(); alRow->addWidget(alBtn); alRow->addStretch(1);
        v->addLayout(alRow);
        v->addWidget(alStatus);
        // ...and the twin of "anilist.data": the same line from the same builder, so the two surfaces
        // cannot tell the user different things about the same queue.
        auto* alData = new QLabel(anilistStatusLine());
        alData->setWordWrap(true);
        alData->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(alData);
        {
            // While THIS panel is up it owns the refresh hook; the themed builder installs its own when it
            // presents. Guarded by a QPointer so a delivery landing after the panel is destroyed writes
            // nowhere - the traktStatusUpdate_ idiom above, for the same reason.
            QPointer<QLabel> guard(alData);
            anilistStatusUpdate_ = [guard] { if (guard) guard->setText(MainWindow::anilistStatusLine()); };
        }
        // The panel's own connections; they die with the labels, which are its children.
        connect(anilist_, &AniListTracker::authUrlReady, alStatus, [alStatus](const QString& url) {
            alStatus->setText(tr("Sign in at: %1").arg(url)); });
        connect(anilist_, &AniListTracker::connectError, alStatus,
                [alStatus](const QString& m) { alStatus->setText(m); });
        connect(anilist_, &AniListTracker::connectedChanged, alBtn, [alBtn, alStatus](bool on) {
            alBtn->setText(on ? tr("Disconnect") : tr("Connect to AniList"));
            alStatus->setText(on ? tr("\u2713 Connected to AniList.") : tr("Not connected.")); });
        connect(alBtn, &QPushButton::clicked, this, [this, alStatus] {
            if (AniListTracker::isConnected()) { anilist_->disconnectAccount(); return; }
            if (!AniListTracker::isConfigured())
            { alStatus->setText(tr("Enter your Client ID and Secret first.")); return; }
            alStatus->setText(tr("Opening AniList in your browser\u2026"));
            anilist_->connectAccount();
        });

        // --- MyAnimeList (issue #156, increment 2): the twins of the themed builder's mal.* rows. A
        // user-facing setting has to exist in BOTH surfaces or it is simply unreachable in one mode. ---
        v->addSpacing(12);
        auto* mlHeading = new QLabel(tr("MyAnimeList (anime and manga)"));
        mlHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(mlHeading);
        auto* mlNote = new QLabel(tr("Sync chapters read and episodes watched to your MyAnimeList list. "
                                     "Create a free API client at myanimelist.net (Account Settings, API, "
                                     "Create ID), set its redirect URL to the loopback address "
                                     "http://127.0.0.1 , paste the Client ID below, then Connect. Leave the "
                                     "secret empty if your client has none. AniList above and this one are "
                                     "independent: connect both and every finished chapter goes to both "
                                     "lists, each with its own link and its own queue, and neither one "
                                     "failing stops the other. Your MyAnimeList credentials stay on this "
                                     "device and are never included in cloud sync."));
        mlNote->setWordWrap(true);
        mlNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(mlNote);
        // Qualified with "MyAnimeList" for the reason the AniList pair is qualified: the form now carries
        // three "Client ID:" rows and the parity gate's twin patterns must name exactly one control each.
        addCredRow(tr("MyAnimeList Client ID:"), MyAnimeListTracker::typedClientId(), false,
                   [](const QString& t) { MyAnimeListTracker::setClientId(t); }, MyAnimeListTracker::usingBuiltin());
        addCredRow(tr("MyAnimeList Client secret:"), MyAnimeListTracker::typedClientSecret(), true,
                   [](const QString& t) { MyAnimeListTracker::setClientSecret(t); },
                   MyAnimeListTracker::usingBuiltin() && !MyAnimeListTracker::clientSecret().isEmpty());

        auto* mlStatus = new QLabel(MyAnimeListTracker::isConnected()
                                        ? tr("\u2713 Connected to MyAnimeList.") : tr("Not connected."));
        mlStatus->setWordWrap(true);
        mlStatus->setStyleSheet(QStringLiteral("font-size:13px;color:#bbb;"));
        auto* mlBtn = new QPushButton(MyAnimeListTracker::isConnected() ? tr("Disconnect")
                                                                        : tr("Connect to MyAnimeList"));
        mlBtn->setMinimumHeight(32);
        auto* mlRow = new QHBoxLayout(); mlRow->addWidget(mlBtn); mlRow->addStretch(1);
        v->addLayout(mlRow);
        v->addWidget(mlStatus);
        // ...and the twin of "mal.data": the same line from the same builder, so the two surfaces cannot
        // tell the user different things about the same queue.
        auto* mlData = new QLabel(malStatusLine());
        mlData->setWordWrap(true);
        mlData->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(mlData);
        {
            // While THIS panel is up it owns the refresh hook; the themed builder installs its own when it
            // presents. QPointer-guarded so a delivery landing after the panel is destroyed writes nowhere.
            QPointer<QLabel> mlGuard(mlData);
            malStatusUpdate_ = [mlGuard] { if (mlGuard) mlGuard->setText(MainWindow::malStatusLine()); };
        }
        // The panel's own connections; they die with the labels, which are its children.
        connect(mal_, &MyAnimeListTracker::authUrlReady, mlStatus, [mlStatus](const QString& url) {
            mlStatus->setText(tr("Sign in at: %1").arg(url)); });
        connect(mal_, &MyAnimeListTracker::connectError, mlStatus,
                [mlStatus](const QString& m) { mlStatus->setText(m); });
        connect(mal_, &MyAnimeListTracker::connectedChanged, mlBtn, [mlBtn, mlStatus](bool on) {
            mlBtn->setText(on ? tr("Disconnect") : tr("Connect to MyAnimeList"));
            mlStatus->setText(on ? tr("\u2713 Connected to MyAnimeList.") : tr("Not connected.")); });
        connect(mlBtn, &QPushButton::clicked, this, [this, mlStatus] {
            if (MyAnimeListTracker::isConnected()) { mal_->disconnectAccount(); return; }
            if (!MyAnimeListTracker::isConfigured())
            { mlStatus->setText(tr("Enter your Client ID first.")); return; }
            mlStatus->setText(tr("Opening MyAnimeList in your browser\u2026"));
            mal_->connectAccount();
        });

        // --- Kitsu (issue #156, increment 3): the twins of the themed builder's kitsu.* rows. A
        // user-facing setting has to exist in BOTH surfaces or it is simply unreachable in one mode. ---
        v->addSpacing(12);
        auto* ktHeading = new QLabel(tr("Kitsu (anime and manga)"));
        ktHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(ktHeading);
        auto* ktNote = new QLabel(tr("Sync chapters read and episodes watched to your Kitsu list. There "
                                     "is nothing to register here: type the email and password of your "
                                     "Kitsu account and press Sign in. The password is used once to sign "
                                     "in and is never stored on this device; only the token it returns is "
                                     "kept, and that stays on this machine and is never included in cloud "
                                     "sync. AniList and MyAnimeList above and this one are independent: "
                                     "connect all three and every finished chapter goes to all three "
                                     "lists, each with its own link and its own queue, and none of them "
                                     "failing stops the others."));
        ktNote->setWordWrap(true);
        ktNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(ktNote);
        // Qualified with "Kitsu" for the reason the other two pairs are: the form now carries three
        // credential blocks and the parity gate's twin patterns must name exactly one control each.
        // The password row starts EMPTY and stays write-only - there is no getter for it.
        addCredRow(tr("Kitsu email:"), KitsuTracker::email(), false,
                   [](const QString& t) { KitsuTracker::setEmail(t); });
        addCredRow(tr("Kitsu password:"), QString(), true,
                   [](const QString& t) { KitsuTracker::setPassword(t); });

        auto* ktStatus = new QLabel(KitsuTracker::isConnected()
                                        ? tr("\u2713 Signed in to Kitsu.") : tr("Not signed in."));
        ktStatus->setWordWrap(true);
        ktStatus->setStyleSheet(QStringLiteral("font-size:13px;color:#bbb;"));
        auto* ktBtn = new QPushButton(KitsuTracker::isConnected() ? tr("Sign out")
                                                                  : tr("Sign in to Kitsu"));
        ktBtn->setMinimumHeight(32);
        auto* ktRow = new QHBoxLayout(); ktRow->addWidget(ktBtn); ktRow->addStretch(1);
        v->addLayout(ktRow);
        v->addWidget(ktStatus);
        // ...and the twin of "kitsu.data": the same line from the same builder, so the two surfaces
        // cannot tell the user different things about the same queue.
        auto* ktData = new QLabel(kitsuStatusLine());
        ktData->setWordWrap(true);
        ktData->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(ktData);
        {
            // QPointer-guarded so a delivery landing after the panel is destroyed writes nowhere.
            QPointer<QLabel> ktGuard(ktData);
            kitsuStatusUpdate_ = [ktGuard] { if (ktGuard) ktGuard->setText(MainWindow::kitsuStatusLine()); };
        }
        // The panel's own connections; they die with the labels, which are its children. There is no
        // authUrlReady here - no browser opens, so there is never a URL to show.
        connect(kitsu_, &KitsuTracker::connectError, ktStatus,
                [ktStatus](const QString& m) { ktStatus->setText(m); });
        connect(kitsu_, &KitsuTracker::connectedChanged, ktBtn, [ktBtn, ktStatus](bool on) {
            ktBtn->setText(on ? tr("Sign out") : tr("Sign in to Kitsu"));
            ktStatus->setText(on ? tr("\u2713 Signed in to Kitsu.") : tr("Not signed in.")); });
        connect(ktBtn, &QPushButton::clicked, this, [this, ktStatus] {
            if (KitsuTracker::isConnected()) { kitsu_->disconnectAccount(); return; }
            if (!KitsuTracker::hasSignInCredentials())
            { ktStatus->setText(tr("Enter your Kitsu email and password first.")); return; }
            ktStatus->setText(tr("Signing in to Kitsu\u2026"));
            kitsu_->connectAccount();
        });

        // --- Music scrobbling (issue #192): the twins of the themed builder's rows. A user-facing setting has
        // to exist in BOTH surfaces or it is simply unreachable in one mode. ---
        v->addSpacing(12);
        auto* sbHeading = new QLabel(tr("Music scrobbling"));
        sbHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(sbHeading);
        auto* sbNote = new QLabel(tr("Send the music you play here to your ListenBrainz listening history. Create a free "
                                     "account, copy the "
                                     "user token from your ListenBrainz profile settings, and paste it below. "
                                     "A track is scrobbled once you have played half of it, or four minutes, "
                                     "whichever comes first. Leave the custom API URL empty for ListenBrainz "
                                     "itself, or point it at a compatible server such as Maloja. Last.fm is a "
                                     "second destination rather than an alternative: connect it as well and "
                                     "every listen goes to both, each with its own queue."));
        sbNote->setWordWrap(true);
        sbNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(sbNote);
        auto* sbOn = new QCheckBox(tr("Scrobble music I listen to"));
        sbOn->setStyleSheet(QStringLiteral("font-size:15px;"));
        sbOn->setChecked(Settings::scrobbleEnabled());
        v->addWidget(sbOn);
        // MASKED, exactly as the themed row is. Through the same addCredRow the OpenSubtitles and Trakt
        // credentials use — one construction, so the echo mode cannot be got right in one place and wrong here.
        addCredRow(tr("ListenBrainz token:"), Settings::listenBrainzToken(), true,
                   [this](const QString& t) { Settings::setListenBrainzToken(t);
                                              if (scrobbler_) scrobbler_->retryNow(); });
        addCredRow(tr("Custom API URL:"), Settings::listenBrainzApiUrl(), false,
                   [this](const QString& t) { Settings::setListenBrainzApiUrl(t);
                                              if (scrobbler_) scrobbler_->retryNow(); });
        // LAST.FM (#192 increment 2): the twin of the themed builder's "scrobble.lastfm" action and its
        // status info row. A BUTTON and a label rather than a credential field, because nothing about this
        // link is typed. Both are built before the connect handler because every arm writes to them.
        auto* lfStatus = new QLabel(LastFmClient::statusText());
        lfStatus->setWordWrap(true);
        lfStatus->setStyleSheet(QStringLiteral("font-size:13px;color:#bbb;"));
        auto* lfBtn = new QPushButton(LastFmClient::connectActionLabel());
        lfBtn->setMinimumHeight(32);
        lfBtn->setEnabled(LastFmClient::availableInThisBuild());
        auto* lfRow = new QHBoxLayout(); lfRow->addWidget(lfBtn); lfRow->addStretch(1);
        v->addLayout(lfRow);
        v->addWidget(lfStatus);
        connect(lfBtn, &QPushButton::clicked, this, [this, lfStatus] {
            if (!lastfm_) { lfStatus->setText(LastFmClient::statusText()); return; }
            if (LastFmClient::connected()) { lastfm_->disconnectAccount(); return; }
            lfStatus->setText(tr("Asking Last.fm for an authorisation link\u2026"));
            lastfm_->connectAccount(); });
        if (lastfm_)
        {
            // Wired to THIS panel's widgets, so the connections die with it - the tkStatus idiom above.
            connect(lastfm_, &LastFmClient::authUrl, lfStatus, [lfStatus](const QString& url) {
                lfStatus->setText(tr("Open %1 and approve, then this will finish on its own.").arg(url));
                MainWindow::openAuthPage(url); });
            connect(lastfm_, &LastFmClient::connectError, lfStatus,
                    [lfStatus](const QString& m) { lfStatus->setText(m); });
            connect(lastfm_, &LastFmClient::connectedChanged, lfBtn, [lfBtn, lfStatus](bool linked) {
                lfBtn->setText(LastFmClient::connectActionLabel(LastFmClient::availableInThisBuild(), linked));
                lfStatus->setText(LastFmClient::statusText()); });
        }
        auto* sbSpoken = new QCheckBox(tr("Also scrobble audiobooks and podcasts"));
        sbSpoken->setStyleSheet(QStringLiteral("font-size:15px;"));
        sbSpoken->setChecked(Settings::scrobbleSpokenAudio());
        v->addWidget(sbSpoken);
        // The twin of the themed "scrobble.serverforwards" row (#193): a user-facing setting has to exist in
        // BOTH surfaces or it is unreachable in one mode. Same label, and the same explanation of what each
        // position does, so the two surfaces cannot describe one switch differently.
        auto* sbFwd = new QCheckBox(tr("My music server scrobbles for me"));
        sbFwd->setStyleSheet(QStringLiteral("font-size:15px;"));
        sbFwd->setChecked(Settings::scrobbleServerForwards());
        v->addWidget(sbFwd);
        auto* sbFwdHint = new QLabel(
            tr("Turn this on if Navidrome (or Airsonic, or Gonic) is signed in to Last.fm or ListenBrainz "
               "itself. Off, a track played from a music server is reported to the server AND to the "
               "services above, which is right when the server forwards nothing. On, it is reported to the "
               "server only, and the server passes it on — otherwise every one of those plays is counted "
               "twice. Music on this device is unaffected either way."));
        sbFwdHint->setWordWrap(true);
        sbFwdHint->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(sbFwdHint);
        // ...and the twin of "scrobble.status": the same line, from the same builder, so neither surface can
        // claim something the other contradicts. It is the only place the feature says whether it is working.
        auto* sbStatus = new QLabel(scrobbleStatusLine());
        sbStatus->setWordWrap(true);
        sbStatus->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(sbStatus);
        {
            // While THIS panel is up it owns the refresh hook; the themed builder installs its own when it
            // presents. Guarded by a QPointer so a delivery landing after the panel is destroyed writes nowhere
            // — the traktStatusUpdate_ idiom, for the same reason.
            QPointer<QLabel> guard(sbStatus);
            scrobbleStatusUpdate_ = [this, guard] { if (guard) guard->setText(scrobbleStatusLine()); };
        }
        connect(sbOn, &QCheckBox::toggled, this, [this, sbStatus](bool c) {
            Settings::setScrobbleEnabled(c);
            if (scrobbler_) scrobbler_->retryNow();   // switching it on delivers anything already queued
            sbStatus->setText(scrobbleStatusLine()); });
        connect(sbSpoken, &QCheckBox::toggled, this, [this, sbStatus](bool c) {
            Settings::setScrobbleSpokenAudio(c);
            sbStatus->setText(scrobbleStatusLine()); });
        connect(sbFwd, &QCheckBox::toggled, this, [this, sbStatus](bool c) {
            Settings::setScrobbleServerForwards(c);
            sbStatus->setText(scrobbleStatusLine()); });

        // --- Discord Rich Presence: the twin of every themed row above. A user-facing setting has to exist
        // in BOTH surfaces or it is unreachable in one mode - the ROMs folder row is the precedent. ---
        v->addSpacing(12);
        auto* dcHeading = new QLabel(tr("Discord"));
        dcHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(dcHeading);
        auto* dcNote = new QLabel(tr("Your Discord profile shows what you're watching, playing or reading, "
                                     "with its artwork. Each category can be silenced on its own, and this "
                                     "machine's choice is its own — turning it on here doesn't turn it "
                                     "on anywhere else."));
        dcNote->setWordWrap(true);
        dcNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(dcNote);

        // Built before the rows because every row's handler writes to it - the same shape as sbStatus above.
        auto* dcStatus = new QLabel(discordStatusLine());
        dcStatus->setWordWrap(true);
        dcStatus->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));

        // ONE builder for all seven rows: same key, same setter and same status refresh as the themed twin,
        // so the two surfaces cannot drift. The setter is a plain function pointer because every one of
        // these settings is a bare bool - nothing here needs a capture.
        auto dcRow = [this, v, dcStatus](const QString& label, bool checked, void (*setter)(bool)) {
            auto* cb = new QCheckBox(label);
            cb->setStyleSheet(QStringLiteral("font-size:15px;"));
            cb->setChecked(checked);
            v->addWidget(cb);
            connect(cb, &QCheckBox::toggled, this, [this, dcStatus, setter](bool c) {
                setter(c);
                if (presence_) presence_->settingsChanged();
                dcStatus->setText(discordStatusLine()); });
        };
        dcRow(tr("Show what I'm doing on Discord"), Settings::discordEnabled(),  &Settings::setDiscordEnabled);
        dcRow(tr("Movies and TV"),                  Settings::discordMovies(),   &Settings::setDiscordMovies);
        dcRow(tr("Games"),                          Settings::discordGames(),    &Settings::setDiscordGames);
        dcRow(tr("Music and audiobooks"),           Settings::discordMusic(),    &Settings::setDiscordMusic);
        dcRow(tr("Books and comics"),               Settings::discordReading(),  &Settings::setDiscordReading);
        dcRow(tr("Live TV"),                        Settings::discordLiveTv(),   &Settings::setDiscordLiveTv);
        dcRow(tr("Just browsing"),                  Settings::discordBrowsing(), &Settings::setDiscordBrowsing);

        v->addWidget(dcStatus);
        {
            // While THIS panel is up it owns the refresh hook - the scrobbleStatusUpdate_ idiom, QPointer
            // guarded so a Discord connection landing after the panel is destroyed writes nowhere.
            QPointer<QLabel> guard(dcStatus);
            presenceStatusUpdate_ = [this, guard] { if (guard) guard->setText(discordStatusLine()); };
        }

        // --- Profiles (issue #30): the twin of the themed builder's row. A user-facing setting has to exist
        // in BOTH surfaces or it is simply unreachable in one mode. ---
        v->addSpacing(12);
        auto* prHeading = new QLabel(tr("Profiles"));
        prHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(prHeading);
        auto* prNote = new QLabel(tr("EverythingBox asks who's using it every time it opens. Turn this on to "
                                     "go straight in when there's only one profile — it's ignored while that "
                                     "profile has a passcode."));
        prNote->setWordWrap(true); prNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(prNote);
        auto* prSkip = new QCheckBox(tr("Skip the profile picker when there's only one profile"));
        prSkip->setStyleSheet(QStringLiteral("font-size:15px;"));
        prSkip->setChecked(Settings::skipProfilePickerWhenSingle());
        connect(prSkip, &QCheckBox::toggled, this, [](bool c) { Settings::setSkipProfilePickerWhenSingle(c); });
        v->addWidget(prSkip);

        // --- Parental controls: a PIN that gates leaving a restricted (kids) profile. ---
        v->addSpacing(12);
        auto* pcHeading = new QLabel(tr("Parental Controls"));
        pcHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(pcHeading);
        auto* pcNote = new QLabel(tr("Set a PIN, then mark the kids' profiles below as restricted. While a "
                                     "restricted profile is active, switching profiles or opening Settings "
                                     "requires the PIN."));
        pcNote->setWordWrap(true); pcNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(pcNote);

        auto* pcStatus = new QLabel(Settings::hasParentalPin() ? tr("A PIN is set.") : tr("No PIN set."));
        pcStatus->setStyleSheet(QStringLiteral("font-size:13px;color:#bbb;"));
        auto* setPin = new QPushButton(Settings::hasParentalPin() ? tr("Change PIN") : tr("Set PIN"));
        auto* clrPin = new QPushButton(tr("Remove PIN"));
        setPin->setMinimumHeight(32); clrPin->setMinimumHeight(32);
        clrPin->setEnabled(Settings::hasParentalPin());
        auto* pcRow = new QHBoxLayout(); pcRow->addWidget(setPin); pcRow->addWidget(clrPin); pcRow->addStretch(1);
        v->addLayout(pcRow);
        v->addWidget(pcStatus);
        // PIN entry via the in-window on-screen keyboard (password echo): typeable from the couch, no popup.
        connect(setPin, &QPushButton::clicked, this, [this, setPin, clrPin, pcStatus] {
            if (Settings::hasParentalPin()) {
                const QString cur = Osk::getText(tr("Enter the current PIN:"), QString(), QLineEdit::Password, this);
                if (cur.isNull()) return;
                if (!Settings::checkParentalPin(cur)) { pcStatus->setText(tr("Incorrect PIN.")); return; }
            }
            const QString a = Osk::getText(tr("New PIN:"), QString(), QLineEdit::Password, this);
            if (a.isNull() || a.isEmpty()) return;
            const QString b = Osk::getText(tr("Confirm PIN:"), QString(), QLineEdit::Password, this);
            if (b.isNull()) return;
            if (a != b) { pcStatus->setText(tr("PINs didn't match.")); return; }
            Settings::setParentalPin(a);
            pcStatus->setText(tr("A PIN is set.")); setPin->setText(tr("Change PIN")); clrPin->setEnabled(true);
        });
        connect(clrPin, &QPushButton::clicked, this, [this, setPin, clrPin, pcStatus] {
            const QString cur = Osk::getText(tr("Enter the current PIN:"), QString(), QLineEdit::Password, this);
            if (cur.isNull()) return;
            if (!Settings::checkParentalPin(cur)) { pcStatus->setText(tr("Incorrect PIN.")); return; }
            Settings::setParentalPin(QString());
            pcStatus->setText(tr("No PIN set.")); setPin->setText(tr("Set PIN")); clrPin->setEnabled(false);
        });

        v->addSpacing(6);
        auto* pcProfiles = new QLabel(tr("Restricted (kids) profiles:"));
        pcProfiles->setStyleSheet(QStringLiteral("font-size:13px;color:#bbb;"));
        v->addWidget(pcProfiles);
        for (const Profile& pr : ProfileStore::list()) {
            auto* cb = new QCheckBox((pr.icon.isEmpty() ? QString() : pr.icon + QStringLiteral("  ")) + pr.name);
            cb->setStyleSheet(QStringLiteral("font-size:15px;"));
            cb->setChecked(pr.restricted);
            const QString id = pr.id;
            // GATED, the classic twin of the themed builder's row (fix round 2, finding 2) — the same one
            // allowRestrictedChange, not a second copy of the rule. On a refusal the checkbox is put back;
            // the signal is blocked around that write so the revert does not re-enter this handler.
            connect(cb, &QCheckBox::toggled, this, [this, cb, id](bool c) {
                if (!allowRestrictedChange(id, c)) {
                    QSignalBlocker block(cb);
                    cb->setChecked(!c);
                    return;
                }
                ProfileStore::setRestricted(id, c);
            });
            v->addWidget(cb);
        }

        // --- Background music: play tracks dropped in <data>/music while browsing the menus. ---
        v->addSpacing(10);
        if (bgm_) bgm_->reload(); // rescan so files added since last time are picked up
        auto* bHeading = new QLabel(tr("Background Music"));
        bHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(bHeading);
        auto* bNote = new QLabel(tr("Drop audio files into the music folder to play them quietly while you browse "
                                    "(they pause during games and video)."));
        bNote->setWordWrap(true); bNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(bNote);

        auto* bOn = new QCheckBox(tr("Play background music"));
        bOn->setStyleSheet(QStringLiteral("font-size:15px;"));
        bOn->setChecked(Settings::bgmEnabled());
        v->addWidget(bOn);
        connect(bOn, &QCheckBox::toggled, this, [this](bool c) {
            Settings::setBgmEnabled(c); if (bgm_) bgm_->setEnabled(c); updateBackgroundMusic(); });

        auto* volRow = new QHBoxLayout();
        volRow->addWidget(new QLabel(tr("Volume")));
        auto* bVol = new QSlider(Qt::Horizontal); bVol->setRange(0, 100); bVol->setValue(Settings::bgmVolume());
        bVol->setEnabled(bOn->isChecked());
        connect(bOn, &QCheckBox::toggled, bVol, &QSlider::setEnabled);
        connect(bVol, &QSlider::valueChanged, this,
                [this](int val) { Settings::setBgmVolume(val); if (bgm_) bgm_->setVolume(val); });
        volRow->addWidget(bVol, 1);
        v->addLayout(volRow);

        auto* openMusic = panelRow(tr("Open Music Folder"));
        connect(openMusic, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(BackgroundMusic::musicDir()));
            if (bgm_) { bgm_->reload(); updateBackgroundMusic(); } // pick up any files already there
        });
        v->addWidget(openMusic);

        // --- Video previews (issue #55): the themed twin lives in the PanelRow builder above. Play a game's
        // scraped/gamelist video snap on hover; ON by default but muted (snap volume 0) so it never fights
        // the background music until the user raises it. ---
        v->addSpacing(10);
        auto* vpHeading = new QLabel(tr("Video previews"));
        vpHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(vpHeading);
        auto* vpNote = new QLabel(tr("Play a game's video snap when you hover it in the browser (falls back to a "
                                     "still when there's no video). Muted by default — raise the volume to hear it."));
        vpNote->setWordWrap(true); vpNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(vpNote);

        auto* vpOn = new QCheckBox(tr("Play video previews on hover"));
        vpOn->setStyleSheet(QStringLiteral("font-size:15px;"));
        vpOn->setChecked(Settings::videoPreviewsEnabled());
        v->addWidget(vpOn);
        connect(vpOn, &QCheckBox::toggled, this, [this](bool c) {
            Settings::setVideoPreviewsEnabled(c); VideoPreviewBridge::instance().setEnabled(c); });

        auto* vpVolRow = new QHBoxLayout();
        vpVolRow->addWidget(new QLabel(tr("Preview volume")));
        auto* vpVol = new QSlider(Qt::Horizontal); vpVol->setRange(0, 100); vpVol->setValue(Settings::videoSnapVolume());
        vpVol->setEnabled(vpOn->isChecked());
        connect(vpOn, &QCheckBox::toggled, vpVol, &QSlider::setEnabled);
        connect(vpVol, &QSlider::valueChanged, this,
                [this](int val) { Settings::setVideoSnapVolume(val); VideoPreviewBridge::instance().setVolume(val); });
        vpVolRow->addWidget(vpVol, 1);
        v->addLayout(vpVolRow);

        // --- Steam (achievements + owned library): a Steam Web API key shows an installed PC game's Steam
        // achievements; the key + a 64-bit SteamID also surface owned-but-not-installed games on the Steam console. ---
        v->addSpacing(10);
        auto* sHeading = new QLabel(tr("Steam (achievements + owned library)"));
        sHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(sHeading);
        auto* sNote = new QLabel(tr("Paste a Steam Web API key (steamcommunity.com/dev/apikey) to show an installed "
            "PC game's Steam achievements in the Triple theme, with the ones you've unlocked highlighted. Add your "
            "64-bit SteamID to also list owned-but-not-installed games on the Steam console (activating one installs "
            "it via Steam). Leave the SteamID blank to keep the console installed-only."));
        sNote->setWordWrap(true); sNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(sNote);
        auto* sKey = new QLineEdit(Settings::steamWebApiKey());
        sKey->setMinimumHeight(34); sKey->setEchoMode(QLineEdit::Password);
        sKey->setPlaceholderText(tr("Steam Web API key"));
        v->addWidget(sKey);
        auto* sId = new QLineEdit(Settings::steamId());
        sId->setMinimumHeight(34);
        sId->setPlaceholderText(tr("64-bit SteamID (optional — for owned games)"));
        v->addWidget(sId);
        auto* sSave = panelRow(tr("Save Steam Key + SteamID"));
        connect(sSave, &QPushButton::clicked, this, [this, sKey, sId] {
            Settings::setSteamWebApiKey(sKey->text());
            Settings::setSteamId(sId->text());
            statusBar()->showMessage(tr("Saved Steam Web API key + SteamID."), 4000);
        });
        v->addWidget(sSave);

        // --- Epic Games (legendary), issue #118: the classic twin of the themed builder's
        // epic.legendary.auth / epic.legendary.get rows. A setting that exists in one builder is unreachable
        // in the other mode, and this one matters most exactly where the classic layout is common — a Linux
        // desktop, which has no Epic Games Launcher for the importer to read. ---
        v->addSpacing(10);
        auto* lgHeading = new QLabel(tr("Epic Games (legendary)"));
        lgHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(lgHeading);
        auto* lgNote = new QLabel(tr("legendary is a small command-line program that talks to Epic directly, so "
            "your owned Epic games can be listed with no Epic Games Launcher installed. EverythingBox looks for "
            "it on your PATH and in the app's tools folder; it never downloads or installs it for you. Signing "
            "in happens in legendary's own account — EverythingBox keeps no Epic password or token."));
        lgNote->setWordWrap(true); lgNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(lgNote);
        auto* lgStatus = new QLabel(legendaryStatusLine());
        lgStatus->setWordWrap(true); lgStatus->setStyleSheet(QStringLiteral("color:#bbb;font-size:12px;"));
        v->addWidget(lgStatus);
        auto* lgAuth = panelRow(tr("Sign in to Epic (legendary)"));
        connect(lgAuth, &QPushButton::clicked, this, [this, lgStatus] {
            QPointer<QLabel> keep(lgStatus);   // the panel can be replaced while the sign-in is in flight
            promptLegendaryAuth([keep](const QString& line) { if (keep) keep->setText(line); });
        });
        v->addWidget(lgAuth);
        auto* lgGet = panelRow(tr("Get legendary"));
        connect(lgGet, &QPushButton::clicked, this, [this] { openLegendaryReleases(); });
        v->addWidget(lgGet);

        // --- Streaming (Debrid): a TorBox API key turns Stremio torrent results into playable streams. ---
        v->addSpacing(10);
        auto* dHeading = new QLabel(tr("Streaming (Debrid)"));
        dHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(dHeading);
        v->addWidget(new QLabel(tr("TorBox API key")));
        auto* tbKey = new QLineEdit(store().value(QStringLiteral("debrid/torbox/apikey")).toString());
        tbKey->setMinimumHeight(34);
        tbKey->setPlaceholderText(tr("Paste your TorBox API key"));
        v->addWidget(tbKey);
        connect(tbKey, &QLineEdit::editingFinished, this, [tbKey] {
            store().setValue(QStringLiteral("debrid/torbox/apikey"), tbKey->text().trimmed());
            store().sync();
        });
        auto* dNote = new QLabel(tr("Lets Stremio torrent addons (Debridio, Torrentio…) play: cached torrents "
            "are resolved to a stream through your TorBox account. Find the key at torbox.app → Settings → API. "
            "Stored on this device."));
        dNote->setWordWrap(true);
        dNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(dNote);

        // --- Community: the classic twin of the themed builder's community.discord row. ---
        v->addSpacing(10);
        auto* cHeading = new QLabel(tr("Community"));
        cHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(cHeading);
        auto* cNote = new QLabel(tr("Questions, setup help, and release news. Ask in #support and tag your "
            "platform — it gets you a faster, more specific answer than the issue tracker will."));
        cNote->setWordWrap(true);
        cNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(cNote);
        auto* cJoin = panelRow(tr("Join the Discord"));
        connect(cJoin, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(QUrl(QString::fromLatin1(kDiscordInvite)));
        });
        v->addWidget(cJoin);
        auto* cSupport = panelRow(tr("Support on Patreon"));
        connect(cSupport, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(QUrl(QString::fromLatin1(kPatreonUrl)));
        });
        v->addWidget(cSupport);
    }, [this] { openSettingsHub(); });
}
