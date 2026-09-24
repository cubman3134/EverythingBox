// Casting a LOCAL file (issue #72), the MainWindow half — a separate translation unit so the feature costs
// MainWindow.cpp three short insertions (the precedent is MainWindowPlayOn.cpp, #143).
//
// The picker (showCastMenu, the classic player page's cast button, reached from its "Player settings" menu) is
// the same widget on BOTH layouts: video plays on the classic player page in the themed layout too. So the one
// change there — CastServe::offerFor instead of "castUrl_ starts with http" — covers both.
//
// What is castable. A remote stream, as before, is castUrl_. A LOCAL file is what mpv actually loaded
// (MpvWidget::playedUrl) during a VIDEO session, because castUrl_ is only assigned on the stream paths and
// would otherwise describe whatever was streamed before. Audio sessions are left out on purpose: a gapless queue
// advances without a replace-load, so the loaded url can name a track that is no longer the one playing.
//
// Lifecycle. CastManager starts the file server on castLocalFile and stops it with the cast (stopCasting, a
// Chromecast session error, a rejected SetAVTransportURI, any other cast) and on quit (aboutToQuit). The one
// ending only this window can see is the item changing: a different file starting to play HERE. That ends the
// local-file cast, so a file stops being served the moment the user has moved on from it.
#include "MainWindow.h"

#include <QFileInfo>
#include <QTimer>
#include <QtDebug>

#include "../core/CastFileServer.h"
#include "../core/CastManager.h"
#include "../media/PlaybackSession.h"
#include "../video/MpvWidget.h"

QString MainWindow::castLocalPath() const
{
    if (!player_ || !session_ || !session_->mediaIsVideo()) return QString();
    return CastServe::localFileFor(player_->playedUrl());
}

void MainWindow::castTo(const CastDevice& dev)
{
    if (!castMgr_) return;
    const QString local = castLocalPath();
    const CastServe::Offer offer = CastServe::offerFor(castUrl_, castHeaderGated_, local);
    if (offer == CastServe::Offer::LocalFile)
    {
        // A catalog title when this file came from a library item that carried one; else the file's own name.
        const QString title = (!castTitle_.isEmpty() && CastServe::localFileFor(castUrl_) == local)
                                  ? castTitle_ : QFileInfo(local).completeBaseName();
        // Local playback stops only once the file is being served: a device no LAN interface reaches leaves
        // the film playing here, with the reason on screen (castError).
        if (castMgr_->castLocalFile(dev, local, title)) player_->stop();
        return;
    }
    if (offer != CastServe::Offer::RemoteUrl) return;
    player_->stop();                         // hand playback to the device; free the local decoder
    castMgr_->cast(dev, castUrl_, castTitle_, castMime_);
}

void MainWindow::wireLocalFileCasting()
{
    connect(player_, &MpvWidget::fileLoaded, this, [this] {
        // Deferred past mpv's delivery: stopCasting emits castStopped, and nothing here should run inside it.
        QTimer::singleShot(0, this, [this] {
            if (!castMgr_) return;
            const QString local = castLocalPath();
            // Prime discovery for a local video, as the stream paths do, so the picker is populated when opened.
            if (!local.isEmpty() && !castMgr_->isServingFile()) castMgr_->startDiscovery();
            if (!castMgr_->isServingFile()) return;
            if (!local.isEmpty() && local == castMgr_->servedFilePath()) return;   // the same file: carry on
            qInfo().noquote() << QStringLiteral("cast: another item started playing here; ending the cast of \"%1\"")
                                     .arg(QFileInfo(castMgr_->servedFilePath()).fileName());
            castMgr_->stopCasting();
        });
    });
}
