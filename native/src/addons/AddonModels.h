// Data models for the addon system, ported from the Unity AddonModels. An addon is a folder with a
// manifest.json + an entry script (main.js). A "media-source" addon's JS returns catalogs of MediaItems.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

struct AddonManifest
{
    QString id;            // unique, reverse-DNS recommended
    QString name;          // display name
    QString version;
    QString type;          // e.g. "media-source"
    QString entry;         // script file name (default "main.js")
    QString author;
    QString description;
    QStringList permissions; // declared capabilities, e.g. ["network"]
    QString minAppVersion;

    static AddonManifest fromJson(const QByteArray& json, bool* ok = nullptr);
    QString entryOrDefault() const { return entry.isEmpty() ? QStringLiteral("main.js") : entry; }
};

struct MediaItem
{
    QString id;
    QString title;
    QString subtitle;
    QString type;          // "ebook", "video", "audio", "pdf", "link", ...
    QString thumbnailUrl;
    QString url;           // resolvable location: file path, http(s) url, ...
    QString mime;
};

struct MediaCatalog
{
    QString title;
    QVector<MediaItem> items;

    static MediaCatalog fromJson(const QByteArray& json);
};
