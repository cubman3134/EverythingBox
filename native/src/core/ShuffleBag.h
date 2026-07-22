// A "shuffle bag" random sequencer — the pure logic behind Channel mode's random autoplay. Draw indices
// 0..n-1 with NO repeat until the whole bag is exhausted, then reshuffle and keep drawing; on a reshuffle the
// first draw of the new bag never immediately repeats the last item of the old one (when n > 1), so a channel
// never airs the same clip twice in a row across the bag boundary. A single-item bag always yields 0. Session
// only (no persistence): a fresh channel builds a fresh bag.
//
// Header-only + QtCore-only (QVector + QRandomGenerator), so the headless probe (probe_playlists) can link it
// straight and pin the invariants without pulling in any UI. MainWindow owns one of these while a channel runs.
#pragma once
#include <QVector>
#include <QRandomGenerator>
#include <algorithm>

class ShuffleBag
{
public:
    ShuffleBag() = default;
    explicit ShuffleBag(int n) { reset(n); }

    // (Re)initialise for n items and shuffle a fresh bag. n <= 0 makes an empty (invalid) bag.
    void reset(int n)
    {
        n_ = n < 0 ? 0 : n;
        lastDrawn_ = -1;
        bag_.clear();
        if (n_ <= 0) { cursor_ = 0; return; }
        bag_.resize(n_);
        for (int i = 0; i < n_; ++i) bag_[i] = i;
        shuffle();              // first bag has no "previous" to avoid (lastDrawn_ == -1)
        cursor_ = 0;
    }

    bool valid() const { return n_ > 0; }
    int size() const { return n_; }

    // The next index to play. Reshuffles transparently when the current bag is exhausted, avoiding an immediate
    // repeat of the last drawn item (when n_ > 1). Returns -1 for an empty bag.
    int next()
    {
        if (n_ <= 0) return -1;
        if (n_ == 1) { lastDrawn_ = 0; return 0; }
        if (cursor_ >= bag_.size())         // bag exhausted -> reshuffle for the next pass
        {
            shuffle();
            if (bag_[0] == lastDrawn_)      // don't immediately repeat the last-played across the boundary
                std::swap(bag_[0], bag_[1 + int(rng()->bounded(quint32(n_ - 1)))]);
            cursor_ = 0;
        }
        const int idx = bag_[cursor_++];
        lastDrawn_ = idx;
        return idx;
    }

private:
    static QRandomGenerator* rng() { return QRandomGenerator::global(); }

    // Fisher-Yates over the whole bag.
    void shuffle()
    {
        for (int i = n_ - 1; i > 0; --i)
        {
            const int j = int(rng()->bounded(quint32(i + 1)));
            std::swap(bag_[i], bag_[j]);
        }
    }

    int n_ = 0;
    QVector<int> bag_;
    int cursor_ = 0;
    int lastDrawn_ = -1;
};
