#include "core/ps3/Ps3UpdateCoordinator.h"
#include "core/ps3/Ps3UpdateFeed.h"
#include <utility>

Ps3UpdateCoordinator::Ps3UpdateCoordinator(TitleIdReader readId, FeedFetcher fetch,
                                           Ps3UpdateState* state, Ps3UpdateInstaller* installer, Progress progress,
                                           InstallIntact intact, AttemptSuppressed suppressed)
    : readId_(std::move(readId)), fetch_(std::move(fetch)), state_(state), installer_(installer),
      progress_(std::move(progress)), intact_(std::move(intact)),
      suppressed_(std::move(suppressed)) {}

bool Ps3UpdateCoordinator::maybeUpdate(const QString& romPath)
{
    const auto titleId = readId_ ? readId_(romPath) : std::nullopt;
    if (!titleId || titleId->isEmpty()) return false;

    const auto body = fetch_ ? fetch_(*titleId) : std::nullopt;
    if (!body || body->trimmed().isEmpty()) return false;

    const QVector<Ps3UpdatePackage> pkgs = Ps3UpdateFeed::parseVerXml(*body);
    if (pkgs.isEmpty()) return false;

    const QString latest = pkgs.last().version; // parseVerXml sorts ascending
    if (!state_) return false;
    // State current is only half the verdict: the record says what an installer CLAIMED. When the
    // tree is visibly poisoned (intact_ says so), run the chain anyway — the per-package
    // already-applied and file-table checks downstream then drive a real heal, and pkg entries
    // overwrite in place.
    if (!state_->needsUpdate(*titleId, latest) && (!intact_ || intact_(*titleId))) return false;

    // Asked only once the chain would actually run, and BEFORE the note: a launch this bounds does no
    // work at all, so it must not advertise an update it is not going to attempt.
    if (suppressed_ && suppressed_(*titleId)) return false;

    if (progress_) progress_(QStringLiteral("Updating game… v%1").arg(latest));
    if (!installer_ || !installer_->installAll(*titleId, pkgs)) return false;

    state_->markInstalled(*titleId, latest);
    return true;
}
