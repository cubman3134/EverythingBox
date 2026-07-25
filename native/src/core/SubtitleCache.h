// Remembers which .srt we already downloaded for a given (identifier, language), so replaying a film does not
// re-hit OpenSubtitles — their free tier has a hard daily download quota. The identifier is whichever thing
// matched: the OSDb hash, the IMDB stream id, or the title. Device-local JSON; never synced (it points at
// local cache paths).
#pragma once
#include <QHash>
#include <QString>

class SubtitleCache
{
public:
    explicit SubtitleCache(QString filePath) : file_(std::move(filePath)) {}
    void load();
    void save() const;
    // The recorded path, or empty when absent OR when the file has since been deleted (self-healing miss).
    QString lookup(const QString& key) const;
    void put(const QString& key, const QString& srtPath);   // overwrite: a picker choice replaces an auto-pick
    void clear();
    static QString keyFor(const QString& identifier, const QString& lang)
    { return identifier + QLatin1Char('|') + lang; }

private:
    QString file_;
    QHash<QString, QString> byKey_;
};
