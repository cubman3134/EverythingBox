// RecompFeed::refresh() — the ONE network-touching function of the RetComM feed, in its own translation unit
// so that the parse, the merge and the cache (RecompFeed.cpp) stay QtCore-only and probe_ports can drive all
// of them headlessly without ever making a request.
//
// The fetch is BoundedFetch's: one blocking GET, redirects followed (the published URL is a
// `releases/latest/download/` alias and does nothing but redirect), a hard byte ceiling and a deadline, no
// headers, no cookies and no credentials. That is the same posture as the theme/decoration registry fetches —
// a bounded read of a public document — and the ceiling is applied AS THE BYTES ARRIVE rather than to what
// landed, which is the only version of a ceiling that bounds anything.
#include "RecompFeed.h"

#include "BoundedFetch.h"

namespace RecompFeed {

Feed refresh()
{
    const BoundedFetch::Result r =
        BoundedFetch::get(catalogUrl(), kFetchTimeoutMs, kMaxCatalogBytes);

    if (r.verdict != BoundedFetch::Result::Ok)
    {
        Feed f;
        // The last good copy is NOT touched, and the caller shows the cached rows. What this returns is only
        // the reason THIS attempt contributed nothing.
        //
        // Deliberately not r.error: Qt's string for an aborted transfer is "Operation canceled", which reads
        // as something the user did. What matters to a person is which of the two failures it was.
        f.shapeError = (r.verdict == BoundedFetch::Result::TooBig)
                           ? QStringLiteral("the recomp catalogue download was larger than this app accepts")
                           : QStringLiteral("the recomp catalogue could not be downloaded");
        return f;
    }
    // Stamp the attempt whatever the document turns out to be: a publisher who ships a broken catalogue must
    // not get this app re-downloading it every time somebody opens the section.
    markRefreshed();
    return storeIfParses(r.body);
}

} // namespace RecompFeed
