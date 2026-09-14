// The shell-style argv cut the launch args (#237) and the "cli" content-install recipes (#189) share
// (issue #403). QtCore-only and header-only, with NO QProcess.
//
// WHY THIS EXISTS. Both callers used to call QProcess::splitCommand. That is a static helper that never starts
// a process, but it is declared inside QProcess, and Qt builds for iOS without QT_CONFIG(process) - so the class
// is never declared there, and the iOS app stopped compiling the moment ContentRecipe.h (which the app includes)
// grew the call. This is the same algorithm written out, so the tokeniser no longer depends on a feature a
// platform may not have.
//
// THE RULE, which is Qt's (qprocess.cpp, QProcess::splitCommand, unchanged since 5.15) and must stay Qt's:
//   * whitespace (QChar::isSpace, so a tab, a newline or a non-breaking space too) separates tokens;
//   * a double-quoted run is part of ONE token, and the quote characters are dropped;
//   * three consecutive double quotes are one literal quote character; two are nothing at all;
//   * a backslash is an ordinary character, never an escape (a quoted "C:\Program Files\..." survives);
//   * an unterminated quote simply runs to the end; empty tokens are never produced.
// probe_contentinstall compares this against QProcess::splitCommand itself over a table of hostile inputs on
// every desktop build, so a "tidy-up" that changes any of the above goes red there.
#pragma once
#include <QString>
#include <QStringList>
#include <QStringView>

namespace CommandSplit
{
    inline QStringList split(QStringView command)
    {
        QStringList args;
        QString tmp;
        int quoteCount = 0;
        bool inQuote = false;

        for (qsizetype i = 0; i < command.size(); ++i)
        {
            const QChar c = command.at(i);
            if (c == QLatin1Char('"'))
            {
                // A quote is only COUNTED here; what a run of them means is decided at the next non-quote
                // character (one toggles the quoted state, two cancel out). The third of a run is the literal.
                ++quoteCount;
                if (quoteCount == 3)
                {
                    quoteCount = 0;
                    tmp += c;
                }
                continue;
            }
            if (quoteCount)
            {
                if (quoteCount == 1) inQuote = !inQuote;
                quoteCount = 0;
            }
            if (!inQuote && c.isSpace())
            {
                if (!tmp.isEmpty())
                {
                    args += tmp;
                    tmp.clear();
                }
            }
            else
            {
                tmp += c;
            }
        }
        if (!tmp.isEmpty()) args += tmp;
        return args;
    }
}
