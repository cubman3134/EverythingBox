// The bridge between an OPDS-PSE offer and the ONE page seam (#153).
//
// It is a header of its own, and not part of OpdsPse.h, purely because of the include direction:
// MediaItem carries an OpdsPseLink, so AddonModels.h includes OpdsPse.h — and a page LIST is a
// QVector<AddonPage>, which is declared in AddonModels.h. Putting this one function there instead would
// make the pair circular. Everything else about it belongs with the rest of the PSE rules and is
// documented there.
//
// Pure: no network, no disk, no widgets. probe_pse holds it, which is the point of it being a function at
// all rather than a loop inside MainWindowOpdsPse.cpp — "every page carries the catalog's credentials,
// and the credential is only ever in a HEADER" is exactly the kind of claim that has to be measurable.
#pragma once
#include "OpdsPse.h"
#include "../addons/AddonModels.h"

#include <QVector>

namespace OpdsPse
{

// One AddonPage per page of the volume, in READING order (the seam packs by index, so this vector is the
// book's order; PageSupplyOptions::fetchOrder is the asking order and a different thing entirely).
//
// `maxWidth` is the server-side resize hint, used only where the template accepts one. `auth` is the
// catalog's device-local request headers — the same ones the feed fetch carried, put on the page requests
// unchanged, because a page behind HTTP Basic is behind it exactly as its feed was. They go in the
// HEADERS and never into the url: a url is logged in a dozen places, a header is logged in none.
inline QVector<AddonPage> pageList(const OpdsPseLink& link, int maxWidth,
                                   const StreamHeaders::Headers& auth)
{
    QVector<AddonPage> pages;
    if (!link.isValid()) return pages;
    pages.reserve(link.count);
    for (int i = 0; i < link.count; ++i)
    {
        AddonPage p;
        p.url     = pageUrl(link, i, maxWidth);
        p.headers = auth;
        pages.push_back(p);
    }
    return pages;
}

} // namespace OpdsPse
