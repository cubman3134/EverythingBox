// MainWindow's own code, for a probe's NEGATIVE source checks (issue #470).
//
// #186 is moving MainWindow.cpp out into sibling files, one increment at a time. A check that says a pattern
// must NOT appear in MainWindow (a door that no longer hands mpv the frozen queue entry, a status line that no
// longer skips the waiting question) goes blind to every function that moves, unless it reads the moved files
// too. The list of those files lives in ONE place, the MW_OWN_SOURCES array near the top of
// native/tools/run-headless-probes.sh, and every #186 increment appends its new file there and nowhere else.
// So this header reads the list out of the runner rather than keeping a second copy that would drift.
//
// A POSITIVE check (the text must be there) keeps reading the one file its text lives in; only the negative
// ones go through ownCode().
#pragma once

#include <QFile>
#include <QChar>
#include <QString>
#include <QStringList>

namespace mwown {

// The files MW_OWN_SOURCES lists, as paths under `nativeDir` (the directory holding src/ and tools/). EMPTY when
// the runner cannot be read or the array is not in the shape it promises (one "$HERE/../<path>" per line, closed
// by a line holding only ")"): a caller must treat an empty list as a failure, never as "nothing to scan".
inline QStringList sources(const QString& nativeDir)
{
    QFile runner(nativeDir + QStringLiteral("/tools/run-headless-probes.sh"));
    if (!runner.open(QIODevice::ReadOnly)) return {};
    const QStringList lines = QString::fromUtf8(runner.readAll()).remove(QLatin1Char('\r')).split(QLatin1Char('\n'));
    const QString head = QStringLiteral("MW_OWN_SOURCES=(");
    const QString open = QStringLiteral("\"$HERE/../");
    QStringList out;
    bool in = false;
    for (const QString& raw : lines) {
        if (!in) { in = (raw == head); continue; }
        const QString t = raw.trimmed();
        if (t == QStringLiteral(")")) return out;
        if (!t.startsWith(open) || !t.endsWith(QLatin1Char('"')) || t.size() <= open.size() + 1) return {};
        out << nativeDir + QLatin1Char('/') + t.mid(open.size(), t.size() - open.size() - 1);
    }
    return {};   // never opened, or never closed
}

// Every listed file's text, concatenated with CRs dropped. EMPTY when the list is empty or any listed file cannot
// be read, so a negative check over it can never pass on a corpus that silently lost a file.
inline QString ownCode(const QString& nativeDir)
{
    const QStringList files = sources(nativeDir);
    if (files.isEmpty()) return {};
    QString all;
    for (const QString& path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return {};
        all += QString::fromUtf8(f.readAll()).remove(QLatin1Char('\r'));
        all += QLatin1Char('\n');
    }
    return all;
}

} // namespace mwown
