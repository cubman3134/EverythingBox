// WHAT THE USER IS TOLD WHEN A SCROBBLE DESTINATION IS REMOVED (issue #337) — the sentences, and the one
// rule that binds all of them.
//
// ==================================================================================================
// THE RULE
// ==================================================================================================
// NOTHING IS DISCARDED WITHOUT THE USER BEING TOLD, IN THE SAME INTERACTION, BEFORE IT HAPPENS. That is the
// whole of #337. The bug it replaces is an invisible one — a music server removed in Settings left its
// unsent listens on disk under a provider id built from the server's uuid, and since re-adding the same
// server mints a NEW uuid, that queue could never be matched to a destination again. Data the user
// generated, kept for ever, delivered never, mentioned nowhere.
//
// The fix cannot be "delete it quietly", because a silent delete is strictly worse than the orphan: the
// orphan at least still exists. So every path that ends in a deletion goes through a sentence built here,
// and every sentence that describes a deletion NAMES THE NUMBER. probe_scrobble asserts exactly that over
// the whole matrix of outcomes, which is why these are pure functions in a header of their own rather than
// tr() calls buried in a UI file where nothing could ever check them.
//
// ==================================================================================================
// WHY THE WORDING IS THE FEATURE
// ==================================================================================================
// There are three moments and they say different things:
//
//   offerMessage    BEFORE the removal, while the sign-in still works. This is the only moment the pending
//                   listens can still be delivered — afterwards there is no credential and no address — so
//                   the offer exists at all only here, and the message says both halves: they can be sent
//                   now, and what is not sent is deleted with the server.
//   outcomeMessage  AFTER the attempt. Three arms, because a flush has three honest endings, and the middle
//                   one (some landed, some did not) is the one a careless implementation reports as either
//                   "sent" or "failed" and is neither.
//   sweepMessage    for the orphans that already exist on somebody's disk — a queue whose server went away
//                   before any of this was written. Nothing can send those: there is no sign-in left. All
//                   that is owed is an honest account of what was found, before it goes.
//
// No sentence here ever carries a credential. `why` comes from ScrobbleResult::message, whose contract
// (ScrobbleProvider.h) forbids echoing the request that failed — which for Subsonic would be the token.
#pragma once
#include <QString>

namespace ScrobbleRemoval
{
    // The confirmation shown when a music server with unsent listens is removed. `pending` > 0 always: with
    // nothing waiting there is nothing to offer and the plain removal confirmation is used instead.
    QString offerMessage(const QString& serverName, int pending);

    // The two buttons that are not Cancel, labelled with the count so the answer is legible without the
    // message — a person who read only the buttons still knows what "discard" costs.
    QString sendLabel(int pending);
    QString discardLabel(int pending);

    // What happened, once the attempt has finished. `discarded` is what is being deleted with the server,
    // and the returned sentence NAMES IT whenever it is above zero — the invariant probe_scrobble pins.
    QString outcomeMessage(const QString& serverName, int sent, int discarded, const QString& why);

    // The sweep over queues left behind by servers that are already gone. `queues` is how many destinations
    // they belong to, `plays` how many listens in total.
    QString sweepMessage(int queues, int plays);
    QString sweepKeepLabel();
    QString sweepDiscardLabel(int plays);
}
