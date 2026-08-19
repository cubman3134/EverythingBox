#pragma once
#include "core/ps3/Ps3UpdateState.h"
#include "core/ps3/Ps3UpdateInstaller.h"
#include <QByteArray>
#include <QString>
#include <functional>
#include <optional>

class Ps3UpdateCoordinator {
public:
    using TitleIdReader = std::function<std::optional<QString>(const QString& romPath)>;
    using FeedFetcher   = std::function<std::optional<QByteArray>(const QString& titleId)>;
    using Progress      = std::function<void(const QString& message)>;
    // Consulted ONLY when the state file says the title is current: false means the on-disk install
    // is visibly poisoned (e.g. a 0-byte file under its USRDIR) and the chain must run anyway. The
    // state file records what an installer CLAIMED; the pre-file-table builds (≤2026-08-19) could
    // claim success over a truncated tree, and that record otherwise gates the heal out forever.
    // Absent = trust the state file (the pre-heal behavior).
    using InstallIntact = std::function<bool(const QString& titleId)>;

    Ps3UpdateCoordinator(TitleIdReader readId, FeedFetcher fetch,
                         Ps3UpdateState* state, Ps3UpdateInstaller* installer, Progress progress,
                         InstallIntact intact = {});
    bool maybeUpdate(const QString& romPath);

private:
    TitleIdReader       readId_;
    FeedFetcher         fetch_;
    Ps3UpdateState*     state_;
    Ps3UpdateInstaller* installer_;
    Progress            progress_;
    InstallIntact       intact_;
};
