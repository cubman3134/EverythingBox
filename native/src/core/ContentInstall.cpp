#include "ContentInstall.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QStandardPaths>

using ContentRecipe::Verdict;

namespace {

QString normPath(const QString& p) { return QDir::cleanPath(QDir::fromNativeSeparators(p)); }
bool samePath(const QString& a, const QString& b) { return normPath(a).compare(normPath(b), Qt::CaseInsensitive) == 0; }

qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }

// Every file under `dir`, as paths RELATIVE to it, sorted — the stable listing both treeSha1 and the tree
// snapshot are built from. Deterministic across platforms (QDirIterator's order is not).
QStringList relFilesSorted(const QString& dir)
{
    QStringList out;
    QDir base(dir);
    if (!base.exists()) return out;
    QStringList stack{ QString() };
    while (!stack.isEmpty())
    {
        const QString rel = stack.takeLast();
        const QString abs = rel.isEmpty() ? dir : dir + QLatin1Char('/') + rel;
        const QFileInfoList es = QDir(abs).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& fi : es)
        {
            const QString childRel = rel.isEmpty() ? fi.fileName() : rel + QLatin1Char('/') + fi.fileName();
            if (fi.isDir()) stack << childRel;
            else            out << childRel;
        }
    }
    out.sort();
    return out;
}

bool ensureParentDir(const QString& filePath)
{
    const QString parent = QFileInfo(filePath).absolutePath();
    return QDir().mkpath(parent);
}

} // namespace

namespace ContentInstall {

// ---- pure: what the record says ---------------------------------------------------------------------------

bool alreadyInstalled(const TitleRecord& rec, const QString& slot, const QString& name,
                      qint64 size, qint64 mtime, const QString& sha1)
{
    for (const Item& it : rec.items)
    {
        if (it.slot != slot) continue;
        // The FAST gate: same package name, same size, same mtime. A match is conclusive — it is the exact
        // file we installed, untouched since. (HashVerify's own path+mtime+size stamp idiom.)
        if (it.name == name && it.size == size && it.mtime == mtime && size > 0) return true;
        // The AUTHORITY: the same bytes under a different stamp (the file was copied, touched, or renamed).
        if (!sha1.isEmpty() && it.sha1.compare(sha1, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

bool weInstalled(const TitleRecord& rec, const QString& destFile)
{
    for (const Item& it : rec.items)
    {
        if (it.dest.isEmpty()) continue;
        if (samePath(it.dest, destFile)) return true;
        // A folder package owns exactly the files it laid down — never the whole destination directory. A
        // user's own file that happens to live in the same folder stays theirs, so the NEXT package leaves it
        // alone instead of overwriting it (probe_contentinstall pins that second-package case).
        for (const QString& rel : it.files)
            if (samePath(it.dest + QLatin1Char('/') + rel, destFile)) return true;
    }
    return false;
}

QStringList ourPaths(const TitleRecord& rec, const QString& slot)
{
    QStringList out;
    for (const Item& it : rec.items)
        if (it.slot == slot && !it.dest.isEmpty()) out << it.dest;
    return out;
}

// ---- pure: JSON <-> record --------------------------------------------------------------------------------

QJsonObject toJson(const Record& r)
{
    QJsonObject titles;
    QStringList ids = r.titles.keys();
    ids.sort();                                   // deterministic file bytes -> a no-op save is a no-op
    for (const QString& id : ids)
    {
        const TitleRecord& t = r.titles.value(id);
        QJsonArray items;
        for (const Item& it : t.items)
        {
            QJsonObject o;
            o.insert(QStringLiteral("slot"), it.slot);
            o.insert(QStringLiteral("name"), it.name);
            if (!it.sha1.isEmpty()) o.insert(QStringLiteral("sha1"), it.sha1);
            if (!it.dest.isEmpty()) o.insert(QStringLiteral("dest"), it.dest);
            if (!it.files.isEmpty())
            {
                QJsonArray fa;
                for (const QString& rel : it.files) fa.push_back(rel);
                o.insert(QStringLiteral("files"), fa);
            }
            o.insert(QStringLiteral("size"),  it.size);
            o.insert(QStringLiteral("mtime"), it.mtime);
            o.insert(QStringLiteral("at"),    it.at);
            items.push_back(o);
        }
        QJsonObject to;
        if (!items.isEmpty())        to.insert(QStringLiteral("installed"), items);
        if (!t.snapshots.isEmpty())  to.insert(QStringLiteral("snapshots"), t.snapshots);
        titles.insert(id, to);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("titles"), titles);
    return root;
}

Record fromJson(const QJsonObject& o)
{
    Record r;
    const QJsonObject titles = o.value(QStringLiteral("titles")).toObject();
    for (auto it = titles.constBegin(); it != titles.constEnd(); ++it)
    {
        TitleRecord t;
        const QJsonObject to = it.value().toObject();
        for (const QJsonValue& v : to.value(QStringLiteral("installed")).toArray())
        {
            const QJsonObject io = v.toObject();
            Item item;
            item.slot  = io.value(QStringLiteral("slot")).toString();
            item.name  = io.value(QStringLiteral("name")).toString();
            item.sha1  = io.value(QStringLiteral("sha1")).toString();
            item.dest  = io.value(QStringLiteral("dest")).toString();
            item.size  = static_cast<qint64>(io.value(QStringLiteral("size")).toDouble());
            item.mtime = static_cast<qint64>(io.value(QStringLiteral("mtime")).toDouble());
            item.at    = static_cast<qint64>(io.value(QStringLiteral("at")).toDouble());
            for (const QJsonValue& fv : io.value(QStringLiteral("files")).toArray())
                if (fv.isString()) item.files << fv.toString();
            if (!item.name.isEmpty()) t.items.push_back(item);
        }
        t.snapshots = to.value(QStringLiteral("snapshots")).toObject();
        r.titles.insert(it.key().toUpper(), t);
    }
    return r;
}

// ---- the record's file ------------------------------------------------------------------------------------

QString recordDir() { return AppPaths::dataDir() + QStringLiteral("/contentinstall"); }

QString recordPath(const QString& emulatorId)
{
    QString safe = emulatorId;
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    return recordDir() + QLatin1Char('/') + safe + QStringLiteral(".json");
}

Record loadRecord(const QString& emulatorId)
{
    QFile f(recordPath(emulatorId));
    if (!f.open(QIODevice::ReadOnly)) return Record{};
    const QByteArray bytes = f.readAll();
    f.close();
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return Record{};
    return fromJson(doc.object());
}

bool saveRecord(const QString& emulatorId, const Record& r)
{
    if (!QDir().mkpath(recordDir())) return false;
    const QByteArray bytes = QJsonDocument(toJson(r)).toJson(QJsonDocument::Indented);
    QSaveFile f(recordPath(emulatorId));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (f.write(bytes) != bytes.size()) { f.cancelWriting(); return false; }
    return f.commit();
}

// ---- sidecar discovery ------------------------------------------------------------------------------------

QString sidecarDir(const QString& gamePath, const QString& slot)
{
    if (gamePath.isEmpty() || slot.isEmpty()) return QString();
    const QFileInfo fi(gamePath);
    const QString folder = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    return folder + QLatin1Char('/') + slot;
}

QStringList discover(const QString& gamePath, const QString& slot)
{
    const QString dir = sidecarDir(gamePath, slot);
    if (dir.isEmpty()) return {};
    QDir d(dir);
    if (!d.exists()) return {};
    QStringList out;
    for (const QFileInfo& fi : d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        out << fi.absoluteFilePath();
    return out;
}

QString resolveTitleId(const QString& gamePath)
{
    if (gamePath.isEmpty()) return QString();
    const QFileInfo fi(gamePath);
    const QString folder = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    // 1. The owner's explicit statement. Wins over any guess, because a wrong id writes into another game.
    QFile tid(folder + QStringLiteral("/titleid.txt"));
    if (tid.open(QIODevice::ReadOnly))
    {
        const QString s = QString::fromUtf8(tid.readAll()).trimmed().toUpper();
        tid.close();
        if (!s.isEmpty()) return s;
    }
    // 2. The game file's own name, 3. its folder's name.
    const QString fromFile = ContentRecipe::titleIdFromName(fi.fileName());
    if (!fromFile.isEmpty()) return fromFile;
    return ContentRecipe::titleIdFromName(QFileInfo(folder).fileName());
}

// ---- pure: the plan ---------------------------------------------------------------------------------------

QVector<Planned> planSlot(const Recipe& recipe, const QString& slot, const QVector<Candidate>& candidates,
                          const TitleRecord& rec, const QString& lever, const QString& titleId)
{
    QVector<Planned> out;
    // A SLOT-level decision yields exactly ONE Planned, never one per candidate: "unknown kind = ignored with
    // ONE logged line" (#189) is a property of this shape, not of the caller remembering to de-duplicate.
    auto slotWide = [&](Decision d, const QString& note) {
        Planned p; p.slot = slot; p.decision = d; p.note = note;
        p.item.name = QStringLiteral("%1 file(s)").arg(candidates.size());
        out.push_back(p);
    };

    // The recipe first: an emulator that declares nothing, a kind we do not know, a kind somebody else owns.
    if (recipe.isEmpty())        { slotWide(Decision::NoRecipe,      QString()); return out; }
    if (!recipe.isValid())       { slotWide(Decision::UnknownKind,   QStringLiteral("recipe kind \"%1\"").arg(recipe.kind)); return out; }
    if (recipe.isDelegated())    { slotWide(Decision::Delegated,     recipe.note); return out; }
    if (recipe.isDescribedOnly()){ slotWide(Decision::DescribedOnly, recipe.note); return out; }

    // The per-game lever. For updates it is a version pin ("" = any, "none" = nothing, else a substring
    // match); for DLC it is on/off. One lever per slot, and OFF is honoured before anything is looked at.
    const bool isDlc = (slot == ContentRecipe::slotDlc());
    if (isDlc && !ContentRecipe::dlcEnabled(lever))   { slotWide(Decision::Disabled, QStringLiteral("DLC turned off for this game")); return out; }
    if (!isDlc && ContentRecipe::pinIsNone(lever))    { slotWide(Decision::Disabled, QStringLiteral("updates pinned to none for this game")); return out; }

    // The title id, for the recipes whose path needs it. Reported, never guessed — a wrong id writes into
    // another game's content store, which is the one failure this feature must never have.
    const bool needsTitleId = (recipe.path + recipe.dest).contains(QStringLiteral("{titleId"));
    if (needsTitleId && titleId.isEmpty()) { slotWide(Decision::NoTitleId, QString()); return out; }

    for (const Candidate& c : candidates)
    {
        Planned p; p.item = c; p.slot = slot;
        if (!isDlc && !ContentRecipe::pinAccepts(lever, c.name))
        {
            p.decision = Decision::PinnedOut;
            p.note = QStringLiteral("does not match the pinned version \"%1\"").arg(lever.trimmed());
        }
        else if (alreadyInstalled(rec, slot, c.name, c.size, c.mtime, c.sha1))
        {
            p.decision = Decision::AlreadyInstalled;
        }
        else
        {
            p.decision = Decision::Install;
        }
        out.push_back(p);
    }
    return out;
}

// ---- pure: merging an entry into a jsonRegistry index -------------------------------------------------------

bool jsonNamesPath(const QJsonValue& v, const QString& needle)
{
    if (needle.isEmpty()) return false;
    if (v.isString()) return samePath(v.toString(), needle);
    if (v.isArray())
    {
        for (const QJsonValue& e : v.toArray()) if (jsonNamesPath(e, needle)) return true;
        return false;
    }
    if (v.isObject())
    {
        const QJsonObject o = v.toObject();
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) if (jsonNamesPath(it.value(), needle)) return true;
        return false;
    }
    return false;
}

namespace {
// The path this entry is ABOUT: the first string, anywhere in it, that names an existing-or-not file path.
// Used as the entry's identity when deciding whether an index already carries it.
QString entryIdentity(const QJsonValue& v)
{
    if (v.isString()) { const QString s = v.toString(); return s.contains(QLatin1Char('/')) || s.contains(QLatin1Char('\\')) ? s : QString(); }
    if (v.isArray())
        for (const QJsonValue& e : v.toArray()) { const QString s = entryIdentity(e); if (!s.isEmpty()) return s; }
    if (v.isObject())
    {
        const QJsonObject o = v.toObject();
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) { const QString s = entryIdentity(it.value()); if (!s.isEmpty()) return s; }
    }
    return QString();
}
} // namespace

MergeResult mergeRegistry(const QJsonDocument& existing, const QString& container,
                          const QJsonObject& entry, const QStringList& oursPaths)
{
    MergeResult r;
    const bool wantArray = (container == QLatin1String("array"));
    const QString identity = entryIdentity(entry);

    if (wantArray)
    {
        // Preserve every element the file had, in order. Append ours only if nothing already names the file.
        QJsonArray arr = existing.isArray() ? existing.array() : QJsonArray{};
        bool present = false;
        for (const QJsonValue& e : arr) if (jsonNamesPath(e, identity)) { present = true; break; }
        if (!present) { arr.push_back(entry); r.changed = true; }
        r.doc = QJsonDocument(arr);
        return r;
    }

    QJsonObject obj = existing.isObject() ? existing.object() : QJsonObject{};
    for (auto it = entry.constBegin(); it != entry.constEnd(); ++it)
    {
        const QString key = it.key();
        const QJsonValue want = it.value();
        if (want.isArray())
        {
            // Union-append: the user's own entries keep their positions and their values, ours goes on the end
            // only if the array does not already carry it.
            QJsonArray cur = obj.value(key).isArray() ? obj.value(key).toArray() : QJsonArray{};
            for (const QJsonValue& w : want.toArray())
            {
                const QString wid = entryIdentity(w);
                bool present = false;
                for (const QJsonValue& e : cur)
                    if (e == w || (!wid.isEmpty() && jsonNamesPath(e, wid))) { present = true; break; }
                if (!present) { cur.push_back(w); r.changed = true; }
            }
            obj.insert(key, cur);
        }
        else if (want.isString())
        {
            const bool present = obj.contains(key);
            const QString cur = present ? obj.value(key).toString() : QString();
            const Verdict v = ContentRecipe::verdictForScalar(present, cur, want.toString(), oursPaths);
            if (v == Verdict::Write) { if (cur != want.toString()) { obj.insert(key, want); r.changed = true; } }
            else if (v == Verdict::LeaveAlone) r.leftAlone << key;
            // SkipIdentical: nothing to do.
        }
        else
        {
            // A non-string, non-array leaf (a bool/number an emulator's schema requires): set it only when the
            // key is absent, so a value the user changed is never overwritten.
            if (!obj.contains(key)) { obj.insert(key, want); r.changed = true; }
        }
    }
    r.doc = QJsonDocument(obj);
    return r;
}

// ---- glue: hashing ----------------------------------------------------------------------------------------

QString fileSha1(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Sha1);
    if (!h.addData(&f)) { f.close(); return QString(); }
    f.close();
    return QString::fromLatin1(h.result().toHex());
}

QString treeSha1(const QString& dir)
{
    const QStringList rels = relFilesSorted(dir);
    if (rels.isEmpty()) return QString();
    QCryptographicHash h(QCryptographicHash::Sha1);
    for (const QString& rel : rels)
    {
        const QString abs = dir + QLatin1Char('/') + rel;
        h.addData(rel.toUtf8());
        h.addData(QByteArray::number(QFileInfo(abs).size()));
        h.addData(fileSha1(abs).toLatin1());
    }
    return QString::fromLatin1(h.result().toHex());
}

// ---- glue: the snapshot -----------------------------------------------------------------------------------

QJsonObject snapshotOfFile(const QString& path)
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QStringLiteral("file"));
    o.insert(QStringLiteral("path"), path);
    o.insert(QStringLiteral("at"), nowSecs());
    QFile f(path);
    if (f.open(QIODevice::ReadOnly))
    {
        const QByteArray bytes = f.readAll();
        f.close();
        o.insert(QStringLiteral("present"), true);
        o.insert(QStringLiteral("bytes"), QString::fromLatin1(bytes.toBase64()));
    }
    else o.insert(QStringLiteral("present"), false);
    return o;
}

QJsonObject snapshotOfTree(const QString& dir)
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QStringLiteral("tree"));
    o.insert(QStringLiteral("path"), dir);
    o.insert(QStringLiteral("at"), nowSecs());
    const bool present = QDir(dir).exists();
    o.insert(QStringLiteral("present"), present);
    if (present)
    {
        QJsonArray files;
        for (const QString& rel : relFilesSorted(dir))
        {
            QJsonObject fo;
            fo.insert(QStringLiteral("p"), rel);
            fo.insert(QStringLiteral("n"), QFileInfo(dir + QLatin1Char('/') + rel).size());
            files.push_back(fo);
        }
        o.insert(QStringLiteral("files"), files);
    }
    return o;
}

// ---- resolution -------------------------------------------------------------------------------------------

QString appDataRoot()
{
#if defined(Q_OS_WIN)
    const QString ad = qEnvironmentVariable("APPDATA");
    if (!ad.isEmpty()) return QDir::fromNativeSeparators(ad);
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#elif defined(Q_OS_MACOS)
    return QDir::homePath() + QStringLiteral("/Library/Application Support");
#else
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    return xdg.isEmpty() ? QDir::homePath() + QStringLiteral("/.config") : QDir::fromNativeSeparators(xdg);
#endif
}

QString resolveDataDir(const Spec& spec, const Vars& vars)
{
    if (spec.dataDirs.isEmpty()) return vars.emuDir;
    QString first;
    for (const QString& t : spec.dataDirs)
    {
        const QString p = ContentRecipe::expand(t, vars);
        if (p.isEmpty() || ContentRecipe::hasUnresolvedPlaceholder(p)) continue;
        if (first.isEmpty()) first = p;
        if (QDir(p).exists()) return p;
    }
    return first;   // nothing there yet: the first candidate, created on demand
}

// ---- the appliers -----------------------------------------------------------------------------------------

namespace {

// Copy one file, honouring the clobber verdict. Returns the outcome and fills `note` when it is worth stating.
Outcome copyOneFile(const QString& src, const QString& dst, const TitleRecord& rec, QString* note)
{
    const bool exists = QFileInfo::exists(dst);
    QString dstSha, srcSha;
    if (exists) { dstSha = fileSha1(dst); srcSha = fileSha1(src); }
    const Verdict v = ContentRecipe::verdictForFile(exists, dstSha, srcSha, weInstalled(rec, dst));
    if (v == Verdict::SkipIdentical) return Outcome::AlreadyInstalled;
    if (v == Verdict::LeaveAlone)
    {
        if (note) *note = QStringLiteral("a different file is already installed there — left alone");
        return Outcome::LeftAlone;
    }
    if (!ensureParentDir(dst)) { if (note) *note = QStringLiteral("could not create the destination folder"); return Outcome::Failed; }
    if (exists && !QFile::remove(dst)) { if (note) *note = QStringLiteral("could not replace the existing file"); return Outcome::Failed; }
    if (!QFile::copy(src, dst)) { if (note) *note = QStringLiteral("copy failed"); return Outcome::Failed; }
    return Outcome::Installed;
}

} // namespace

Result installForLaunch(const QString& emulatorId, const Spec& spec, const QString& gamePath,
                        const QString& emuDir, const QString& updateLever, const QString& dlcLever)
{
    Result res;
    if (emulatorId.isEmpty() || spec.isEmpty() || gamePath.isEmpty()) return res;

    const QString titleId = resolveTitleId(gamePath);
    Record record = loadRecord(emulatorId);
    TitleRecord rec = record.titles.value(titleId);

    Vars base;
    base.titleId = titleId;
    base.emuDir  = QDir::fromNativeSeparators(emuDir);
    base.appData = appDataRoot();
    base.dataDir = resolveDataDir(spec, base);

    bool recordDirty = false;

    // NOT named `slots`: that is a Qt moc keyword macro (Q_SLOTS) and the name silently disappears.
    const QStringList slotNames{ ContentRecipe::slotUpdates(), ContentRecipe::slotDlc() };
    for (const QString& slot : slotNames)
    {
        const Recipe& recipe = spec.forSlot(slot);
        const QStringList found = discover(gamePath, slot);
        if (found.isEmpty()) continue;               // no sidecar for this slot: nothing to say, nothing to do

        // Pass 1 — the stamp gate, no bytes read.
        QVector<Candidate> cands;
        for (const QString& p : found)
        {
            const QFileInfo fi(p);
            Candidate c;
            c.path = fi.absoluteFilePath();
            c.name = fi.fileName();
            c.isDir = fi.isDir();
            if (c.isDir)
            {
                // A FOLDER package (Cemu's code/content/meta) gets the same cheap stamp a file does: total
                // bytes and newest mtime over its tree, both metadata-only. Without it the record could only
                // be gated on treeSha1, i.e. a full read of a multi-gigabyte update on EVERY launch.
                for (const QString& rel : relFilesSorted(c.path))
                {
                    const QFileInfo cf(c.path + QLatin1Char('/') + rel);
                    c.size += cf.size();
                    c.mtime = qMax(c.mtime, cf.lastModified().toSecsSinceEpoch());
                }
            }
            else
            {
                c.size = fi.size();
                c.mtime = fi.lastModified().toSecsSinceEpoch();
            }
            cands.push_back(c);
        }
        const QString lever = (slot == ContentRecipe::slotDlc()) ? dlcLever : updateLever;
        QVector<Planned> plan = planSlot(recipe, slot, cands, rec, lever, titleId);

        // Pass 2 — hash exactly what pass 1 wants to install, and re-decide with the hashes in hand. This is
        // what makes the record's authority the HASH while a library of already-installed packages still costs
        // zero bytes read.
        bool needsSecondPass = false;
        QHash<QString, int> byPath;
        for (int i = 0; i < cands.size(); ++i) byPath.insert(cands[i].path, i);
        for (const Planned& p : plan)
        {
            if (p.decision != Decision::Install) continue;
            const int i = byPath.value(p.item.path, -1);
            if (i < 0) continue;
            cands[i].sha1 = cands[i].isDir ? treeSha1(cands[i].path) : fileSha1(cands[i].path);
            needsSecondPass = true;
        }
        if (needsSecondPass) plan = planSlot(recipe, slot, cands, rec, lever, titleId);

        for (const Planned& p : plan)
        {
            ItemResult ir;
            ir.slot = slot; ir.name = p.item.name; ir.source = p.item.path;
            switch (p.decision)
            {
                case Decision::NoRecipe:
                    ir.outcome = Outcome::Skipped;
                    ir.note = QStringLiteral("%1 has no %2 recipe").arg(emulatorId, slot);
                    res.log << QStringLiteral("content-install: %1 declares no \"%2\" recipe — %3 sidecar file(s) ignored")
                                   .arg(emulatorId, slot).arg(found.size());
                    res.skipped++;
                    res.items.push_back(ir);
                    break;
                case Decision::UnknownKind:
                    ir.outcome = Outcome::Skipped;
                    ir.note = p.note;
                    // "Unknown kind = recipe ignored with ONE logged line, never a crash" (#189).
                    res.log << QStringLiteral("content-install: %1/%2 declares an unrecognised %3 — ignored")
                                   .arg(emulatorId, slot, p.note);
                    res.skipped++;
                    res.items.push_back(ir);
                    break;
                case Decision::Delegated:
                    ir.outcome = Outcome::Skipped;
                    ir.note = p.note.isEmpty() ? QStringLiteral("the emulator installs this itself") : p.note;
                    res.log << QStringLiteral("content-install: %1/%2 is the emulator's own flow — not reimplemented here")
                                   .arg(emulatorId, slot);
                    res.skipped++;
                    res.items.push_back(ir);
                    break;
                case Decision::DescribedOnly:
                    ir.outcome = Outcome::Skipped;
                    ir.note = p.note.isEmpty() ? QStringLiteral("installed by this emulator's own launch path") : p.note;
                    res.skipped++;
                    res.items.push_back(ir);
                    break;
                case Decision::NoTitleId:
                    ir.outcome = Outcome::Skipped;
                    ir.note = QStringLiteral("no title id could be derived for this game");
                    res.log << QStringLiteral("content-install: %1/%2 needs a title id and none could be derived from \"%3\" "
                                              "(add a titleid.txt beside the game) — skipped")
                                   .arg(emulatorId, slot, QFileInfo(gamePath).fileName());
                    res.skipped++;
                    res.items.push_back(ir);
                    break;
                case Decision::Disabled:
                case Decision::PinnedOut:
                    ir.outcome = Outcome::Skipped;
                    ir.note = p.note;
                    res.skipped++;
                    res.items.push_back(ir);
                    break;
                case Decision::AlreadyInstalled:
                    ir.outcome = Outcome::AlreadyInstalled;
                    res.skipped++;
                    res.items.push_back(ir);
                    break;
                case Decision::Install:
                {
                    Vars v = base;
                    v.file = QDir::toNativeSeparators(p.item.path);
                    v.name = p.item.name;

                    if (recipe.kind == QLatin1String("jsonRegistry"))
                    {
                        QString target = ContentRecipe::expand(recipe.path, v);
                        if (!QDir::isAbsolutePath(target)) target = v.dataDir + QLatin1Char('/') + target;
                        if (ContentRecipe::hasUnresolvedPlaceholder(target))
                        {
                            ir.outcome = Outcome::Failed;
                            ir.note = QStringLiteral("the recipe path still holds an unresolved placeholder");
                            res.failed++; res.items.push_back(ir);
                            res.log << QStringLiteral("content-install: %1/%2 path unresolved (%3) — skipped").arg(emulatorId, slot, target);
                            break;
                        }
                        // The snapshot, taken ONCE per (title, slot), BEFORE the first write.
                        if (!rec.snapshots.contains(slot))
                        {
                            rec.snapshots.insert(slot, snapshotOfFile(target));
                            recordDirty = true;
                        }
                        QByteArray existingBytes;
                        QFile ef(target);
                        if (ef.open(QIODevice::ReadOnly)) { existingBytes = ef.readAll(); ef.close(); }
                        QJsonParseError pe{};
                        const QJsonDocument existing = existingBytes.isEmpty()
                            ? QJsonDocument() : QJsonDocument::fromJson(existingBytes, &pe);
                        if (!existingBytes.isEmpty() && pe.error != QJsonParseError::NoError)
                        {
                            // We do not understand this file, so we must not rewrite it.
                            ir.outcome = Outcome::LeftAlone;
                            ir.dest = target;
                            ir.note = QStringLiteral("the emulator's index is not valid JSON — left alone");
                            res.leftAlone++; res.items.push_back(ir);
                            res.log << QStringLiteral("content-install: %1 is not valid JSON — left alone").arg(target);
                            break;
                        }
                        const MergeResult m = mergeRegistry(existing, recipe.container,
                                                            ContentRecipe::expandEntry(recipe.entry, v),
                                                            ourPaths(rec, slot));
                        ir.dest = target;
                        if (!m.leftAlone.isEmpty())
                            res.log << QStringLiteral("content-install: %1 keeps your own \"%2\" — left alone")
                                           .arg(QFileInfo(target).fileName(), m.leftAlone.join(QStringLiteral(", ")));
                        if (!m.changed)
                        {
                            ir.outcome = Outcome::AlreadyInstalled;
                            res.skipped++; res.items.push_back(ir);
                        }
                        else if (!ensureParentDir(target))
                        {
                            ir.outcome = Outcome::Failed;
                            ir.note = QStringLiteral("could not create the emulator's index folder");
                            res.failed++; res.items.push_back(ir);
                        }
                        else
                        {
                            QSaveFile sf(target);
                            const QByteArray outBytes = m.doc.toJson(QJsonDocument::Indented);
                            bool ok = sf.open(QIODevice::WriteOnly | QIODevice::Truncate)
                                   && sf.write(outBytes) == outBytes.size() && sf.commit();
                            ir.outcome = ok ? Outcome::Installed : Outcome::Failed;
                            if (!ok) ir.note = QStringLiteral("could not write the emulator's index");
                            if (ok) res.installed++; else res.failed++;
                            res.items.push_back(ir);
                        }
                        if (ir.outcome == Outcome::Installed)
                        {
                            Item rit;
                            rit.slot = slot; rit.name = p.item.name; rit.sha1 = p.item.sha1;
                            rit.dest = QDir::toNativeSeparators(p.item.path);   // the PACKAGE is what the index points at
                            rit.size = p.item.size; rit.mtime = p.item.mtime; rit.at = nowSecs();
                            rec.items.push_back(rit);
                            recordDirty = true;
                            res.log << QStringLiteral("content-install: registered %1 as %2 for %3")
                                           .arg(p.item.name, slot, titleId);
                        }
                        break;
                    }

                    if (recipe.kind == QLatin1String("copyTree"))
                    {
                        QString destDir = ContentRecipe::expand(recipe.dest, v);
                        if (!QDir::isAbsolutePath(destDir)) destDir = v.dataDir + QLatin1Char('/') + destDir;
                        if (ContentRecipe::hasUnresolvedPlaceholder(destDir))
                        {
                            ir.outcome = Outcome::Failed;
                            ir.note = QStringLiteral("the recipe destination still holds an unresolved placeholder");
                            res.failed++; res.items.push_back(ir);
                            res.log << QStringLiteral("content-install: %1/%2 destination unresolved (%3) — skipped").arg(emulatorId, slot, destDir);
                            break;
                        }
                        if (!rec.snapshots.contains(slot))
                        {
                            rec.snapshots.insert(slot, snapshotOfTree(destDir));
                            recordDirty = true;
                        }
                        ir.dest = destDir;
                        int wrote = 0, left = 0, failed = 0, same = 0;
                        QStringList ownedRels;   // exactly what WE laid down — see Item::files
                        if (p.item.isDir)
                        {
                            for (const QString& rel : relFilesSorted(p.item.path))
                            {
                                QString note;
                                const Outcome o = copyOneFile(p.item.path + QLatin1Char('/') + rel,
                                                              destDir + QLatin1Char('/') + rel, rec, &note);
                                if (o == Outcome::Installed) { wrote++; ownedRels << rel; }
                                else if (o == Outcome::LeftAlone) { left++; res.log << QStringLiteral("content-install: %1/%2 already holds different content — left alone").arg(destDir, rel); }
                                else if (o == Outcome::Failed) failed++;
                                else { same++; ownedRels << rel; }   // byte-identical to ours: it IS our content
                            }
                        }
                        else
                        {
                            QString note;
                            const Outcome o = copyOneFile(p.item.path, destDir + QLatin1Char('/') + p.item.name, rec, &note);
                            if (o == Outcome::Installed) wrote++;
                            else if (o == Outcome::LeftAlone) { left++; res.log << QStringLiteral("content-install: %1 already holds different content — left alone").arg(destDir + QLatin1Char('/') + p.item.name); }
                            else if (o == Outcome::Failed) failed++;
                            else same++;
                        }
                        if (failed > 0)      { ir.outcome = Outcome::Failed;    ir.note = QStringLiteral("%1 file(s) could not be written").arg(failed); res.failed++; }
                        else if (left > 0 && wrote == 0) { ir.outcome = Outcome::LeftAlone; ir.note = QStringLiteral("%1 file(s) already installed by you — left alone").arg(left); res.leftAlone++; }
                        else if (wrote == 0) { ir.outcome = Outcome::AlreadyInstalled; ir.note = QStringLiteral("%1 file(s) already in place").arg(same); res.skipped++; }
                        else                 { ir.outcome = Outcome::Installed; res.installed++; }
                        if (left > 0 && wrote > 0) ir.note = QStringLiteral("%1 file(s) already installed by you were left alone").arg(left);
                        res.items.push_back(ir);
                        if (ir.outcome == Outcome::Installed)
                        {
                            Item rit;
                            rit.slot = slot; rit.name = p.item.name; rit.sha1 = p.item.sha1;
                            rit.dest = p.item.isDir ? destDir : destDir + QLatin1Char('/') + p.item.name;
                            if (p.item.isDir) rit.files = ownedRels;
                            rit.size = p.item.size; rit.mtime = p.item.mtime; rit.at = nowSecs();
                            rec.items.push_back(rit);
                            recordDirty = true;
                            res.log << QStringLiteral("content-install: installed %1 as %2 for %3")
                                           .arg(p.item.name, slot, titleId);
                        }
                        break;
                    }

                    // A valid kind with no applier here (increment 1 wires jsonRegistry + copyTree only).
                    ir.outcome = Outcome::Skipped;
                    ir.note = QStringLiteral("no applier for kind \"%1\" in this build").arg(recipe.kind);
                    res.skipped++;
                    res.items.push_back(ir);
                    break;
                }
            }
        }
    }

    if (recordDirty)
    {
        record.titles.insert(titleId, rec);
        if (!saveRecord(emulatorId, record))
            res.log << QStringLiteral("content-install: could not save the install record — the next launch will re-check");
    }
    return res;
}

} // namespace ContentInstall
