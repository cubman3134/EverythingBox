#pragma once
#include <QObject>
#include <QVector>

class QNetworkAccessManager;

struct M3uEntry { QString title; QString url; };

// .m3u / .m3u8 playlist + stream-link dispatch. Three flavours share the extension: an HLS manifest
// (one adaptive stream libmpv chews directly), an IPTV/media list (becomes a channel queue), and a
// PlayStation multi-disc list (the emulator swaps discs itself). resolve() reads/fetches the source,
// classifies it, and emits exactly one outcome signal; the host decides what "play" means.
class StreamResolver : public QObject
{
    Q_OBJECT
public:
    explicit StreamResolver(QObject* parent = nullptr);

    // Pure classification helpers (probe-tested; see tools/probe_m3u.cpp).
    static bool isM3uRef(const QString& urlOrPath);
    static bool isHlsManifest(const QString& text);
    static QVector<M3uEntry> parseM3u(const QString& text, const QString& src);
    static bool looksLikeDiscPlaylist(const QVector<M3uEntry>& entries);

    void resolve(const QString& src, const QString& title); // local file or http(s) URL

signals:
    void playDirect(const QString& url, const QString& title);   // HLS / unparseable / fetch failed
    void playQueue(const QStringList& urls, const QStringList& titles,
                   const QString& recentSrc, const QString& title); // IPTV/media list
    void openDisc(const QString& src, const QString& title);     // PlayStation multi-disc set
    void status(const QString& message);                          // transient progress ("Loading playlist…")

private:
    void classify(const QString& src, const QString& text, const QString& title); // was handleM3u
    QNetworkAccessManager* nam_ = nullptr; // lazily created for remote playlists
};
