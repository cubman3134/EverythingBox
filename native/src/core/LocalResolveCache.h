#pragma once
#include <QString>
#include <QStringList>
#include <QHash>

class LocalResolveCache
{
public:
    struct Entry { qint64 size = 0; qint64 mtime = 0; QStringList ids; bool matched = false; qint64 ts = 0; };

    explicit LocalResolveCache(QString filePath) : file_(std::move(filePath)) {}
    void load();
    void save() const;
    bool has(const QString& path) const { return byPath_.contains(path); }
    Entry entry(const QString& path) const { return byPath_.value(path); }
    bool isFresh(const QString& path, qint64 size, qint64 mtime, qint64 nowSecs, qint64 retryDays = 14) const;
    void putMatched(const QString& path, qint64 size, qint64 mtime, const QStringList& ids, qint64 nowSecs);
    void putNoMatch(const QString& path, qint64 size, qint64 mtime, qint64 nowSecs);
    QHash<QString, QStringList> matchedIdsByPath() const;

private:
    QString file_;
    QHash<QString, Entry> byPath_;
};
