#include "BackgroundMusic.h"
#include "AppPaths.h"
#include "Settings.h"

#include <QMediaPlayer>
#include <QAudioOutput>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QRandomGenerator>

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
    out_ = new QAudioOutput(this);
    player_ = new QMediaPlayer(this);
    player_->setAudioOutput(out_);
    enabled_ = Settings::bgmEnabled();
    out_->setVolume(qBound(0, Settings::bgmVolume(), 100) / 100.0);
    // Advance to the next track when one finishes (loop the shuffled list forever).
    connect(player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus s) {
        if (s == QMediaPlayer::EndOfMedia) playIndex(idx_ + 1);
    });
    reload();
}

void BackgroundMusic::reload()
{
    const QDir d(musicDir());
    static const QStringList exts = { QStringLiteral("*.mp3"), QStringLiteral("*.ogg"), QStringLiteral("*.flac"),
        QStringLiteral("*.m4a"), QStringLiteral("*.wav"), QStringLiteral("*.opus"), QStringLiteral("*.aac") };
    tracks_ = d.entryList(exts, QDir::Files, QDir::Name);
    for (QString& t : tracks_) t = d.absoluteFilePath(t);
    for (int i = tracks_.size() - 1; i > 0; --i) // Fisher-Yates shuffle
        tracks_.swapItemsAt(i, QRandomGenerator::global()->bounded(i + 1));
    idx_ = tracks_.isEmpty() ? -1 : 0;
    if (tracks_.isEmpty() && !title_.isEmpty()) { title_.clear(); emit nowPlayingChanged(title_); }
    applyState(); // start playing if we're on a menu and just found tracks (won't interrupt a current one)
}

void BackgroundMusic::playIndex(int i)
{
    if (tracks_.isEmpty()) return;
    idx_ = ((i % tracks_.size()) + tracks_.size()) % tracks_.size(); // wrap
    player_->setSource(QUrl::fromLocalFile(tracks_[idx_]));
    player_->play();
    title_ = QFileInfo(tracks_[idx_]).completeBaseName();
    emit nowPlayingChanged(title_);
}

void BackgroundMusic::applyState()
{
    if (enabled_ && active_ && !tracks_.isEmpty())
    {
        if (player_->playbackState() == QMediaPlayer::PlayingState) return;
        if (player_->source().isEmpty()) playIndex(idx_ < 0 ? 0 : idx_); // first start
        else player_->play();                                            // resume where it paused
    }
    else
    {
        player_->pause();
    }
}

void BackgroundMusic::setActive(bool on)  { if (active_ == on) return; active_ = on; applyState(); }
void BackgroundMusic::setEnabled(bool on)
{
    enabled_ = on;
    if (!on && !title_.isEmpty()) { title_.clear(); emit nowPlayingChanged(title_); } // hide the readout when off
    applyState();
}
void BackgroundMusic::setVolume(int pct)  { out_->setVolume(qBound(0, pct, 100) / 100.0); }
