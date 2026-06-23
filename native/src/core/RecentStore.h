// A small persistent list of recently opened content (videos, audio, books/PDFs, games). Stored as a
// JSON array in mymediavault.ini so it survives restarts; the Home screen's "Recent" tab lists it and the
// main window re-opens an entry by its kind. Newest first, de-duplicated by path, capped.
#pragma once
#include <QString>
#include <QVector>

struct RecentItem
{
    QString path;   // absolute file path to re-open
    QString title;  // display label
    QString kind;   // "video" | "audio" | "document" | "game"
    QString thumb;  // optional poster image (path or http url); empty -> a type placeholder is drawn
};

namespace RecentStore
{
    QVector<RecentItem> list();          // newest first
    void add(const RecentItem& item);    // move-to-front + de-dup by path + cap
    void clear();
}
