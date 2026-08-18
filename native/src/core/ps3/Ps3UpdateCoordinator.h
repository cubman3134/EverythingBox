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

    Ps3UpdateCoordinator(TitleIdReader readId, FeedFetcher fetch,
                         Ps3UpdateState* state, Ps3UpdateInstaller* installer, Progress progress);
    bool maybeUpdate(const QString& romPath);

private:
    TitleIdReader       readId_;
    FeedFetcher         fetch_;
    Ps3UpdateState*     state_;
    Ps3UpdateInstaller* installer_;
    Progress            progress_;
};
