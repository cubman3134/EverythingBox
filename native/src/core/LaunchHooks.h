// Per-game pre-launch / post-exit command hooks (issue #64) — the PURE half: turn a user-authored command
// line into an argv list and substitute the {rom} placeholder. QtCore-only, no QProcess and no disk, so it is
// the mutation-tested heart the launch pipeline (GameLauncher) and probe_launchhooks both call.
//
// ARGV, NOT SHELL. A hook is a plain command line run via QProcess::start(program, argsList) — never through a
// shell, never a single re-parsed string. parseCommandLine tokenizes with the ONE grouping rule a shell-free
// argv needs: double quotes group a run that contains spaces into a single token (the quote characters
// themselves are dropped). There is deliberately NO backslash escaping, no single-quote grouping, no variable
// or glob expansion — a command line, not a scripting language (issue #64's "deliberate limits"). Whitespace
// between tokens is collapsed; an empty line yields an empty list.
//
// {rom} SUBSTITUTION HAPPENS AFTER TOKENIZING, and that ordering is the whole safety argument: the argv is
// already split, so replacing the literal {rom} inside a token can never re-split a ROM path that contains
// spaces — "C:/My Games/x.iso" stays exactly one argument. Both spellings are supported: a whole-token {rom}
// becomes the rom path as that same single token, and a substring {rom} (e.g. --disc={rom}) is replaced in
// place, still one token. The token COUNT is invariant across substitution — that invariant is what the probe
// pins, because it is exactly the property a naive "join, replace, re-split" implementation would break.
#pragma once
#include <QString>
#include <QStringList>

namespace LaunchHooks
{
    // Tokenize a command line into an argv list. Double quotes group spaces into one token and are dropped from
    // the result; runs of whitespace between tokens collapse; an empty (or whitespace-only) line -> empty list.
    // An unterminated quote extends to the end of the line. No shell metacharacters are interpreted.
    inline QStringList parseCommandLine(const QString& line)
    {
        QStringList out;
        QString cur;
        bool inToken = false;   // have we started a token? (so "" yields one empty token, not nothing)
        bool inQuotes = false;
        for (int i = 0; i < line.size(); ++i)
        {
            const QChar c = line.at(i);
            if (inQuotes)
            {
                if (c == QLatin1Char('"')) inQuotes = false;   // closing quote: dropped, token continues
                else                       cur.append(c);       // inside quotes whitespace is literal
            }
            else if (c == QLatin1Char('"'))
            {
                inQuotes = true;    // opening quote: dropped
                inToken  = true;    // an empty "" is still a real (empty) token
            }
            else if (c == QLatin1Char(' ')  || c == QLatin1Char('\t')
                  || c == QLatin1Char('\r') || c == QLatin1Char('\n'))
            {
                if (inToken) { out.append(cur); cur.clear(); inToken = false; }  // token boundary
                // else: collapse consecutive separators
            }
            else
            {
                cur.append(c);
                inToken = true;
            }
        }
        if (inToken) out.append(cur);   // flush the trailing token (also handles an unterminated quote)
        return out;
    }

    // Replace the literal {rom} inside each already-tokenized argument with romPath, IN PLACE — the token count
    // is unchanged, so a rom path with spaces stays one argument (never re-tokenized). Supports both a
    // whole-token {rom} and a substring {rom} within a larger token (--disc={rom}); every occurrence in a token
    // is replaced.
    inline QStringList substituteRom(QStringList argv, const QString& romPath)
    {
        for (QString& tok : argv)
            if (tok.contains(QStringLiteral("{rom}")))
                tok.replace(QStringLiteral("{rom}"), romPath);
        return argv;
    }
}
