// ThemeZip — the one impure step of a zip-lane theme install (issue #91): downloaded archive bytes become
// the in-memory file set ThemeRegistry::installFiles lands atomically.
//
// Split from ThemeRegistry for the reason every unit in src/core is split that way: ThemeRegistry is
// QtCore-only and network-free, so probe_themereg can pin the index parse, the host rule, the version
// comparison and the atomic landing without linking an archiver. This file is where miniz comes in, and it
// holds exactly one decision of its own — which archive members a theme may be made of.
//
// THAT decision is the dangerous part of the whole feature and it is why this unit exists at all. A zip
// member's name is attacker-controlled: a public registry takes pull requests, and the bytes it names are
// written into the user's data directory by a process running as the user. Four shapes are refused, each
// separately and each with its own sentence, because they fail for four different reasons and a single
// "unsafe path" would hide which one a registry actually tried:
//
//   * a TRAVERSAL member ("../../evil"), which climbs out of the destination;
//   * an ABSOLUTE member ("/etc/cron.d/x", "//host/share/x"), which ignores the destination entirely;
//   * a DRIVE-LETTER member ("C:/Windows/System32/x", "C:x"), which is absolute on Windows and which
//     QDir::cleanPath will happily carry through a join;
//   * a SYMLINK member, which carries its target as content and a link mode in its external attributes.
//     Nothing here follows it — the members become QByteArrays and ThemeRegistry::installFiles writes plain
//     files — so refusing it is defence in depth rather than a fix for a live hole. It is refused anyway:
//     the one thing a theme cannot be is a link out of the themes folder, and the next extractor to be
//     pointed at this file set must not be the one that discovers that nobody checked.
//
// Everything is refused BEFORE anything is written, because nothing is written here at all: the caller gets
// a file set or an error, and only ThemeRegistry::installFiles turns one into files on disk.
#pragma once
#include "ThemeRegistry.h"   // Record — install() stamps one into the file set it lands

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

namespace ThemeZip {

// Read `zipBytes` into the (path, contents) pairs installFiles takes. Paths are relative to the theme
// folder: a single wrapper directory (the shape every "Download ZIP" button produces) is stripped, so an
// archive holding "MyTheme/theme.json" and one holding "theme.json" install identically.
//
// Refuses, with a user-facing reason, when: the bytes are not a readable zip; it holds no theme.json once
// the wrapper is stripped; it holds more than ThemeRegistry::kMaxFiles members; any member is bigger than
// kMaxFileBytes or the set totals more than kMaxTotalBytes; two members differ only in case; or any member
// is one of the four shapes above.
bool unpack(const QByteArray& zipBytes, QVector<QPair<QString, QByteArray>>* files, QString* error);

// The WHOLE zip lane after the download, in one call: unpack, stamp `record` into the file set, land it
// atomically through ThemeRegistry::installFiles. Both Appearance surfaces call this rather than spelling
// the three steps out, for the reason ThemeRegistry exists at all — a lane assembled correctly on one
// surface and not the other is how the two start disagreeing about what an install is. An update is the
// same call: installFiles replaces an existing folder by rename, so landing over the top is atomic too.
//
// Nothing is written when anything refuses. The unpack happens entirely in memory and installFiles stages
// and renames, so an install that is refused halfway leaves the previous copy exactly as it was.
bool install(const QByteArray& zipBytes, const QString& themesRoot, const QString& folder,
             const ThemeRegistry::Record& record, QString* error);

} // namespace ThemeZip
