#include "core/ps3/Ps3UpdateCoordinator.h"
#include "core/ps3/Ps3UpdateFeed.h"
#include <utility>

Ps3UpdateCoordinator::Ps3UpdateCoordinator(TitleIdReader readId, FeedFetcher fetch,
                                           Ps3UpdateState* state, Ps3UpdateInstaller* installer, Progress progress)
    : readId_(std::move(readId)), fetch_(std::move(fetch)), state_(state), installer_(installer), progress_(std::move(progress)) {}

bool Ps3UpdateCoordinator::maybeUpdate(const QString& romPath)
{
    const auto titleId = readId_ ? readId_(romPath) : std::nullopt;
    if (!titleId || titleId->isEmpty()) return false;

    const auto body = fetch_ ? fetch_(*titleId) : std::nullopt;
    if (!body || body->trimmed().isEmpty()) return false;

    const QVector<Ps3UpdatePackage> pkgs = Ps3UpdateFeed::parseVerXml(*body);
    if (pkgs.isEmpty()) return false;

    const QString latest = pkgs.last().version; // parseVerXml sorts ascending
    if (!state_ || !state_->needsUpdate(*titleId, latest)) return false;

    if (progress_) progress_(QStringLiteral("Updating game… v%1").arg(latest));
    if (!installer_ || !installer_->installAll(*titleId, pkgs)) return false;

    state_->markInstalled(*titleId, latest);
    return true;
}
