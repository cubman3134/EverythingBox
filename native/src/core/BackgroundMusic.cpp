#include "BackgroundMusic.h"
#include "AppPaths.h"
#include "Settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QMetaObject>

#include <mpv/client.h>

QString BackgroundMusic::musicDir()
{
    const QString d = AppPaths::dataDir() + QStringLiteral("/music");
    QDir().mkpath(d);
    // Leave a note the first time so it's clear what the folder is for.
    const QString readme = d + QStringLiteral("/README.txt");
    if (!QFile::exists(readme))
    {
        QFile f(readme);
        if (f.open(QIODevice::WriteOnly))
            f.write("Drop music files here (mp3, ogg, flac, m4a, wav, opus, aac).\r\n"
                    "They play quietly in the background while you browse the menus, and pause during games and video.\r\n"
                    "Toggle it and set the volume under Settings > General > Background Music.\r\n");
    }
    return d;
}

BackgroundMusic::BackgroundMusic(QObject* parent) : QObject(parent)
{
    mpv_ = mpv_create();
    if (mpv_)
    {
        // Audio only: vid=no ignores any embedded cover-art "video" stream (the thing that makes some MP3s
        // play silently through other backends); vo=null + audio-display=no keep it strictly audio; idle=yes
        // keeps the instance alive between tracks so we can load the next one.
        mpv_set_option_string(mpv_, "vid", "no");
        mpv_set_option_string(mpv_, "vo", "null");
        mpv_set_option_string(mpv_, "audio-display", "no");
        mpv_set_option_string(mpv_, "idle", "yes");
        mpv_set_option_string(mpv_, "force-window", "no");
        mpv_set_option_string(mpv_, "terminal", "no");
        mpv_set_option_string(mpv_, "config", "no");
        // mpv parses numbers with the C locale; main() already calls setlocale(LC_NUMERIC, "C").
        if (mpv_initialize(mpv_) < 0) { mpv_terminate_destroy(mpv_); mpv_ = nullptr; }
    }
    if (mpv_)
    {
        double v = qBound(0, Settings::bgmVolume(), 100);
        mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &v);
        mpv_set_wakeup_callback(mpv_, &BackgroundMusic::onWakeup, this);
    }
    enabled_ = Settings::bgmEnabled();
    reload();
}

BackgroundMusic::~BackgroundMusic()
{
    if (mpv_) { mpv_terminate_destroy(mpv_); mpv_ = nullptr; }
}

void BackgroundMusic::reload()
{
    const QDir d(musicDir());
    static const QStringList exts = { QStringLiteral("*.mp3"), QStringLiteral("*.ogg"), QStringLiteral("*.flac"),
        QStringLiteral("*.m4a"), QStringLiteral("*.wav"), QStringLiteral("*.opus"), QStringLiteral("*.aac") };
    userTracks_ = d.entryList(exts, QDir::Files, QDir::Name);
    for (QString& t : userTracks_) t = d.absoluteFilePath(t);
    for (int i = userTracks_.size() - 1; i > 0; --i) // Fisher-Yates shuffle
        userTracks_.swapItemsAt(i, QRandomGenerator::global()->bounded(i + 1));
    rebuildPool();
}

// The playback pool is the user's music-folder tracks, or — when that folder is empty — the theme's shipped
// default track, so a fresh install still has menu music.
void BackgroundMusic::rebuildPool()
{
    tracks_ = !userTracks_.isEmpty() ? userTracks_
            : (!themeDefault_.isEmpty() ? QStringList{ themeDefault_ } : QStringList{});
    idx_ = tracks_.isEmpty() ? -1 : 0;
    if (tracks_.isEmpty() && !title_.isEmpty()) { title_.clear(); emit nowPlayingChanged(title_); }
    applyState(); // start playing if we're on a menu and just found tracks (won't interrupt a current one)
}

void BackgroundMusic::setThemeDefault(const QString& absPath)
{
    if (themeDefault_ == absPath) return;
    themeDefault_ = absPath;
    if (userTracks_.isEmpty()) rebuildPool(); // only matters when the user has no music of their own
}

void BackgroundMusic::playIndex(int i)
{
    if (tracks_.isEmpty() || !mpv_) return;
    idx_ = ((i % tracks_.size()) + tracks_.size()) % tracks_.size(); // wrap
    const QByteArray path = QFile::encodeName(tracks_[idx_]); // mpv wants a filesystem-encoded path
    const char* cmd[] = { "loadfile", path.constData(), nullptr };
    mpv_command(mpv_, cmd);
    loaded_ = true;
    setPaused(!(enabled_ && active_)); // load paused if we're not on a menu screen
    title_ = QFileInfo(tracks_[idx_]).completeBaseName();
    emit nowPlayingChanged(title_);
}

void BackgroundMusic::setPaused(bool paused)
{
    if (!mpv_) return;
    int flag = paused ? 1 : 0;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &flag);
}

void BackgroundMusic::applyState()
{
    if (enabled_ && active_ && !tracks_.isEmpty())
    {
        if (idx_ < 0) idx_ = 0;
        if (!loaded_) playIndex(idx_); // first start
        else setPaused(false);         // resume where it paused
    }
    else if (loaded_)
    {
        setPaused(true);
    }
}

void BackgroundMusic::setActive(bool on)  { if (active_ == on) return; active_ = on; applyState(); }
void BackgroundMusic::setEnabled(bool on)
{
    enabled_ = on;
    if (!on && !title_.isEmpty()) { title_.clear(); emit nowPlayingChanged(title_); } // hide the readout when off
    applyState();
}
void BackgroundMusic::setVolume(int pct)
{
    if (!mpv_) return;
    double v = qBound(0, pct, 100);
    mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &v);
}

void BackgroundMusic::setPreview(const QString& src)
{
    if (!mpv_) return;
    if (src.isEmpty())
    {
        if (!previewing_) return;
        previewing_ = false;
        const char* off[] = { "set", "loop-file", "no", nullptr };
        mpv_command(mpv_, off);
        if (enabled_ && active_ && !tracks_.isEmpty()) playIndex(idx_); // resume the shuffle where we were
        else { setPaused(true); if (!title_.isEmpty()) { title_.clear(); emit nowPlayingChanged(title_); } }
        return;
    }
    if (!(enabled_ && active_)) return;                 // don't blare a preview when music is off / on content
    previewing_ = true;
    const char* loop[] = { "set", "loop-file", "inf", nullptr }; // a short theme song loops while hovering
    mpv_command(mpv_, loop);
    const QByteArray p = src.startsWith(QStringLiteral("http")) ? src.toUtf8() : QFile::encodeName(src);
    const char* cmd[] = { "loadfile", p.constData(), nullptr };
    mpv_command(mpv_, cmd);
    loaded_ = true;
    setPaused(false);
}

// Queued from mpv's (other-thread) wakeup callback so the event drain runs on the GUI thread.
void BackgroundMusic::onWakeup(void* ctx)
{
    QMetaObject::invokeMethod(static_cast<BackgroundMusic*>(ctx), "onMpvEvents", Qt::QueuedConnection);
}

void BackgroundMusic::onMpvEvents()
{
    if (!mpv_) return;
    while (true)
    {
        mpv_event* e = mpv_wait_event(mpv_, 0);
        if (e->event_id == MPV_EVENT_NONE) break;
        if (e->event_id == MPV_EVENT_END_FILE)
        {
            auto* ef = static_cast<mpv_event_end_file*>(e->data);
            // A track finished (EOF) or couldn't be decoded (ERROR) -> move to the next one; loop forever.
            // A STOP/REDIRECT reason means WE replaced the file (playIndex), so don't double-advance.
            if (ef && (ef->reason == MPV_END_FILE_REASON_EOF || ef->reason == MPV_END_FILE_REASON_ERROR)
                && !tracks_.isEmpty() && !previewing_) // a ducking preview loops itself; don't advance the shuffle
                playIndex(idx_ + 1);
        }
    }
}
