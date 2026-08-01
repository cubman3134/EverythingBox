// PC-game identity and source selection. The PC library used to be one folder per launcher (Steam,
// GOG, Epic, Battle.net) plus a folder of downloaded games, so the same game appeared several times
// with unrelated ids. This unit is what lets ONE entry carry all of them as sources — the same shape
// the video stream picker already uses for one movie with many streams.
//
// Pure and QtCore-only (the override store is a small ini map), so probe_pcgames links lean.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace pcgame
{
    // The matching key. Lowercases, collapses whitespace, strips punctuation, trademark symbols and
    // edition noise (Game of the Year / Definitive / Remastered / Director's Cut / a trailing year).
    //
    // It deliberately does NOT strip SEQUEL NUMERALS, Arabic or Roman. "Hades" and "Hades II" are
    // different games: merging them removes one from the user's library, while failing to merge two
    // editions only shows a game twice. That asymmetry is why the numeral rule is a hard requirement
    // and not a nicety.
    //
    // The edition strip is a SUFFIX rule: those phrases are stripped only off the END of the title,
    // because mid-title they are part of the product name ("Command & Conquer Remastered Collection").
    //
    // KNOWN COLLISION, carried on purpose: the trailing-year strip merges a REMAKE with its original.
    // "Prey (2006)" and "Prey (2017)" both normalise to "prey", as do "Resident Evil 2" and "Resident
    // Evil 2 (2019)". The igdb id is what disambiguates them (see sameGame / mergeKey); a TITLE-ONLY
    // library will show the remake and the original as ONE entry. See the note in the .cpp for why the
    // year strip is nonetheless required.
    QString normalizeTitle(const QString& raw);

    // normalizeTitle's body with the trailing-year strip made optional. Exposed only because separationTag
    // below is defined as "everything normalizeTitle does EXCEPT the year", and re-implementing that here
    // would be a second copy of the eight-step rule — the exact drift this unit's whole design fights.
    // stripYear = true is normalizeTitle; callers who want that should call normalizeTitle.
    QString normalizeCore(const QString& raw, bool stripYear);

    // THE KEY A COPY KEEPS ONCE ITS GROUP IS SEPARATED — normalizeTitle with the year left in.
    //
    // Choosing it this way, rather than "the raw title", is the whole difference between a usable escape
    // hatch and a worse mess. The trailing-year strip is the ONE step that fuses genuinely different games
    // ("Prey (2006)" onto "Prey (2017)"), so restoring it is precisely the distinction the user is pointing
    // at. Every other step removes edition noise — and keying a separated group on the raw title would then
    // split "Prey" from "Prey: Game of the Year Edition" too, turning a two-way over-merge into a
    // three-entry library, which is a fresh instance of the harm being cured.
    //
    // The honest consequence: two copies that differ ONLY in something normalizeTitle removes for good
    // reason (punctuation, a trademark symbol) share a tag, so separating cannot tell them apart. They are
    // still both launchable — the picker disambiguates same-launcher rows by their per-launcher title and,
    // failing that, by launch id — and the surface that offers "separate" checks how many distinct tags a
    // group actually has, so it never offers an action that would do nothing.
    QString separationTag(const QString& title);

    // The grouping key used by the catalog builder: the igdb id when there is one, else the
    // normalised title. Two entries group together iff their mergeKey matches.
    //
    // A title can normalise to EMPTY ("!!!", "GOTY", "Enhanced Edition"). Such an entry gets a private
    // fallback key derived from its raw title, so it groups with nothing but itself instead of every
    // other empty-normalising entry landing in one bucket. Callers do not have to special-case it.
    QString mergeKey(const QString& title, const QString& igdbId);

    // THE PC-GAME ITEM ID. The single place that turns a game's title into the id everything else keys
    // on — the catalog tile, and therefore the user's favourite, hidden/completion/tags, play time,
    // consumption seconds and resume position, all of which are stored under a hash of THIS string.
    //
    // It exists because two callers used to build it independently (pcGamesCatalog and
    // pcgame::remapTable) from the same ingredients but not the same arguments, and the failure mode of
    // them disagreeing is invisible: the remap moves every record onto an id no lookup ever performs, so
    // the user's stars and hours simply stop existing with nothing logged. Two functions that must agree
    // by convention is how that happened, so there is now one function and both call it. Do not inline
    // "pcgame:" + mergeKey(...) anywhere; call this.
    //
    // DELIBERATELY TITLE-ONLY, i.e. NOT igdb-aware. mergeKey will key on an igdb id when handed one, and
    // doing that here would be a real improvement in one respect — it is what separates "Prey (2006)"
    // from "Prey (2017)", which the year strip otherwise fuses. It is rejected anyway, on two grounds:
    //   * NOTHING SUPPLIES ONE at the point the id is minted. The catalog builder is given four launcher
    //     scans and a downloads list; no metadata resolver runs there, and none can, because the id has
    //     to exist before the tile it would fetch metadata for.
    //   * AN ID MUST NOT DEPEND ON A NETWORK RESULT. Metadata resolution is partial and varies run to
    //     run — a game resolved today and unresolved tomorrow would change identity between two refreshes
    //     and strand its own records, which is precisely the harm this whole unit exists to prevent. An
    //     id has to be a pure, total function of what the local scan yields.
    // Returns an EMPTY string for a title with nothing to group on; callers treat that as "skip", never
    // as a key (see rule 1 in PcGameRemap.h).
    QString itemId(const QString& title);

    // THE TITLE A DOWNLOADED COPY IS KNOWN BY — the ONE fallback for a Downloads record with no title
    // of its own (`DownloadedItem::title` is optional; the file name is all such a record has).
    //
    // It exists for the same reason itemId does, and it is the same failure. The catalog GROUPS a
    // downloaded copy under itemId(this title), while the remap's candidate destination is
    // itemId(the title populatePcGames hands remapTable). Those were two separate expressions — the
    // catalog fell back to the file's base name, the remap fed the raw (empty) title — so a download
    // with an EMPTY title got a tile keyed on the base name while its remap destination was empty, i.e.
    // absent from the table (rule 1). Its marks and play time then accrued under the launch id forever,
    // on no tile, invisibly. One expression, called by both, is the only shape that cannot come apart;
    // probe_browse pins the equality.
    //
    // Whitespace-only counts as empty: the catalog trims before grouping, so a title of " " would
    // otherwise group on nothing while still reading as "has a title" here.
    QString downloadedTitle(const QString& title, const QString& path);

    // Are these the same game? A user override wins; then, when BOTH sides carry an igdb id, that
    // decides (equal ids -> same, different ids -> NOT same, even if the titles agree); otherwise the
    // normalised titles decide. A missing id on one side is not a mismatch — it just means fall back.
    bool sameGame(const QString& titleA, const QString& igdbA,
                  const QString& titleB, const QString& igdbB);

    // The user's manual "these are/aren't the same" verdicts, keyed on the pair of normalised titles
    // and symmetric in the pair. This is the escape hatch that makes a fuzzy heuristic shippable: a
    // wrong merge is otherwise uncurable.
    //
    // THESE FOUR TAKE RAW TITLES and normalise once, internally. Do not hand them a key you already
    // normalised: normalizeTitle is NOT idempotent (the edition-phrase strip runs before punctuation
    // becomes space, so "Batman Arkham City (GOTY)" -> "batman arkham city goty" -> "batman arkham
    // city"), so a second pass builds a key nothing ever wrote and the lookup or removal silently misses.
    // Callers that already hold a STORED key — the Undo surface, which walks overrides() — use
    // clearOverrideKeys below instead. The full account is in the .cpp above verdictForKeys.
    bool overrideSaysSame(const QString& titleA, const QString& titleB);
    void setOverride(const QString& titleA, const QString& titleB, bool same);
    // Forget a verdict entirely — NOT the same as storing "not the same". Storing a negative is the user
    // SEPARATING a wrongly merged key and has to keep beating the heuristic forever; clearing is the user
    // undoing their own correction and handing the decision back to the heuristic. Two different states, so
    // two different calls (a UI that only had setOverride could never restore the default).
    void clearOverride(const QString& titleA, const QString& titleB);
    // The same removal named by the keys the verdict is STORED under, i.e. the `a`/`b` of a MergeVerdict.
    // Normalises nothing. This is what an Undo that walks overrides() must call.
    void clearOverrideKeys(const QString& keyA, const QString& keyB);

    // One stored verdict, with both sides already normalised (that is the form they are keyed in).
    struct MergeVerdict { QString a; QString b; bool same = false; };
    // Every verdict the user has recorded. Needed because a "same" verdict is a graph edge — the id builder
    // has to find everything a title was fused WITH, and a pair lookup can only answer about a pair it was
    // already told about.
    QVector<MergeVerdict> overrides();

    // Has the user SEPARATED this game, i.e. said the copies that fuse under its key are not one game?
    // Stored as the self-pair (norm, norm), which is exactly what setOverride writes when the two titles
    // normalise to the same thing — the case the merge is wrong in ("Prey (2006)" and "Prey (2017)" both
    // normalise to "prey", so there is no second key to name).
    //
    // A RAW TITLE, normalised once here — same rule as setOverride, and for the same reason.
    bool overrideSaysSeparate(const QString& title);

    // EVERY normalised key in this one's FUSED component, itself included — the "same" verdicts walked as
    // undirected edges. Order-independent and cycle-safe, so the component is identical whichever member you
    // ask from. Empty for an empty key.
    //
    // It is public because three surfaces need the same walk and a second copy of it is exactly the drift
    // this unit exists to prevent: effectiveItemId takes its minimum as the surviving id, the merge confirm
    // has to NAME that minimum, and Undo has to clear the whole component rather than only the edges that
    // happen to mention the key the user was looking at.
    QStringList fusedKeys(const QString& normKey);

    // The key a fuse of these two normalised keys would settle on: the minimum of the UNION of their two
    // components, because the new edge joins them into one.
    //
    // The merge confirm's entire purpose is to say whose banked history survives, and the pairwise smaller
    // of the two titles is the wrong answer whenever either side was already fused with something smaller —
    // the dialog then names an entry whose records strand under its old id immediately after promising they
    // were kept. Both arguments are normalizeTitle output.
    QString fusedCanonicalKey(const QString& normA, const QString& normB);

    // THE ID THE FOLDER AND THE REMAP ACTUALLY KEY ON — itemId with the user's overrides applied.
    //
    // itemId above stays the pure, override-free base and is still the only place the title arithmetic
    // lives; this is the one place the escape hatch is spent. It exists as a SEPARATE function rather than
    // as a change to itemId so the base rule ("an id is a total function of the title") is still stated and
    // still tested, and so the override is visible at the two call sites that spend it.
    //
    // BOTH the catalog builder and pcgame::remapTable call THIS, for the same reason they both call itemId:
    // the catalog groups on it and the remap moves records onto it, and the failure mode of the two
    // disagreeing is silent — every record lands on an id no lookup performs. One function, two callers.
    //
    // Three outcomes:
    //   * no verdict touches this title      -> exactly itemId(title)
    //   * the key was SEPARATED              -> itemId(title) + "#" + separationTag(title), so copies that
    //                                           differ by the year the merge key threw away get one entry
    //                                           each while two editions of one copy still fuse. Both callers
    //                                           are handed the same raw launcher name, so they cannot derive
    //                                           different ids from it.
    //   * the key was FUSED with other keys  -> the smallest normalised key in the fused set, so the answer
    //                                           does not depend on which side the user was looking at, nor on
    //                                           the order the verdicts were recorded.
    //
    // SEPARATE BEATS FUSE when a key somehow carries both: a key cannot simultaneously be too coarse and too
    // fine, and picking one deterministically is better than a rule that depends on lookup order.
    //
    // HONEST LIMIT, and it is inherent rather than an oversight: records are stored under a hash of the id,
    // so changing the id STRANDS what was already banked. Separating a wrongly merged entry does not
    // re-divide the play time that was already summed onto it — nothing records which copy earned which
    // hour — and the surface that offers this has to say so plainly rather than implying an undo.
    QString effectiveItemId(const QString& title);

#ifdef EB_PCGAMEID_TEST_SEAM
    // Test-only ini redirect, declared and compiled ONLY for probe_pcgames (same rule as
    // ProfilePasscode / SettingsTxn / ThemeChoice): without the macro this does not exist, so a
    // production call is a compile error rather than a silent process-wide redirect.
    //
    // It is not a nicety. The override store PERSISTS, so a probe run that writes into the app's real
    // ini leaves state behind that the NEXT run reads — and a mutation run that normalises titles
    // wrongly writes a key no correct build would ever produce, which then fails the reverted build
    // and reads as "the revert didn't work". The probe must own a fresh file.
    void setIniPathForTesting(const QString& path);
#endif

    // Order candidates for the "this is the same game as…" picker: the plausible ones first.
    //
    // It exists because the picker's honest list is EVERY other entry in the library, and the two spellings
    // a user is trying to join are similar by definition — an alphabetical wall of four hundred games buries
    // the one row the whole action is about. Ranked on how many LEADING normalised words the two titles
    // share, so "Final Fantasy VII Remake" surfaces beside "Final Fantasy VII", then alphabetically, which
    // makes the order total and therefore stable between two identical libraries.
    //
    // It is an ORDERING and never a filter: a user who knows the two titles look nothing alike must still be
    // able to reach the row, so nothing is dropped. Returns the same titles, reordered.
    QStringList rankMergeCandidates(const QString& title, const QStringList& others);

    // One way to launch a game.
    struct PcGameSource
    {
        enum Kind { LauncherInstalled, LauncherOwned, Downloaded, AddonAvailable };
        Kind    kind = LauncherInstalled;
        QString launcher;     // "steam" | "epic" | "gog" | "battlenet"; empty for an addon source
        QString launchId;     // appid / appName / gog id / battle.net code
        QString exePath;      // when the launch is a direct exe
        QString launchUrl;    // when the launch is a protocol URL
        QString addonItemId;  // Downloaded / AddonAvailable
        QString label;        // the picker row
        // The launcher's OWN name for this copy, VERBATIM as the launcher reported it — NOT the merged
        // tile's display title, which is the best-ranked name across every launcher and is therefore
        // frequently some other store's spelling. It is the identity half of the pre-merge id for a source
        // whose launcher has no id of its own (a code-less Battle.net title), so it has to be the same
        // string the remap's candidate id is built from. See legacyLaunchId.
        QString sourceName;
        bool    ready = false; // launches NOW, with no download
    };

    // The PRE-MERGE, per-launcher id a launch through this source banks its Recent, play time and marks
    // under — i.e. the id the remap migrates FROM ("steam:<appid>", "epic:<appName>", "gog:<id>",
    // "bnet:<code>", and for a code-less Battle.net title "bnet:<the launcher's own name>").
    //
    // It exists because that last case had two independent constructions. The launch site minted
    // "bnet:" + the MERGED DISPLAY TITLE, while the remap's candidate is built from Battle.net's own name;
    // for a code-less game whose title loses the display-title contest to another launcher those differ,
    // and the play time then accrues under an id the remap never visits — permanently stranded, silently.
    // One function, called by both sides, is the only way that cannot come apart; probe_browse pins the
    // equality against the table remapTable builds.
    //
    // Empty for a source with no launcher (a downloaded / addon copy: it is already keyed by addonItemId)
    // or with nothing to key on at all. Callers treat empty as "no pre-merge id", never as a key.
    QString legacyLaunchId(const PcGameSource& s);

    // Which source should Play use? Returns an index, or -1 meaning "ask the user".
    //
    // Exactly one ready source -> that one. Several ready, or none ready -> ask. It must NEVER return
    // a not-ready source: Play is a single keypress, and silently starting a multi-gigabyte download
    // from it is precisely what this function exists to prevent.
    int pickAutoSource(const QVector<PcGameSource>& all);
}
