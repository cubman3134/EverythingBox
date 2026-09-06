#include "VocabularyStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "LookupRequest.h"
#include "ProfileStore.h"
#include "Tombstones.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>

// Shares the portable everythingbox.ini with the other per-item stores (same AppPaths::dataDir() posture as
// BookmarkStore/HighlightStore). Coherence with any other QSettings on the same file comes from every writer
// calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Per-profile, so each reader keeps their own vocabulary. The profile leaf mirrors HighlightStore's.
static QString profileId()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

static std::function<void()> g_changeHook;
void VocabularyStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
static void fireChanged() { if (g_changeHook) g_changeHook(); }

QString VocabularyStore::itemsKey()
{
    return QStringLiteral("vocabulary/") + profileId() + QStringLiteral("/items");
}

QString VocabularyStore::tombstoneStore()
{
    return QStringLiteral("vocabulary/") + profileId();
}

QString VocabularyStore::idFor(const QString& word, const QString& lang)
{
    const QString norm = LookupRequest::normalizeTerm(word);
    if (norm.isEmpty()) return QString();
    // md5(case-folded word | language) — LookupRequest::vocabKey is that seed, defined once so the identity
    // rule and the normalisation the lookup itself used cannot drift apart.
    const QByteArray seed = LookupRequest::vocabKey(word, lang).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Md5).toHex().left(16));
}

static VocabularyStore::Word wordFromObject(const QJsonObject& o)
{
    VocabularyStore::Word w;
    w.id         = o.value(QStringLiteral("id")).toString();
    w.word       = o.value(QStringLiteral("word")).toString();
    w.lang       = o.value(QStringLiteral("lang")).toString();
    w.source     = o.value(QStringLiteral("source")).toString();
    w.definition = o.value(QStringLiteral("definition")).toString();
    w.bookKey    = o.value(QStringLiteral("bookKey")).toString();
    w.bookTitle  = o.value(QStringLiteral("bookTitle")).toString();
    w.context    = o.value(QStringLiteral("context")).toString();
    w.spine      = o.value(QStringLiteral("spine")).toInt(-1);
    w.offset     = o.value(QStringLiteral("offset")).toInt(-1);
    w.ts         = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
    return w;
}

static QJsonObject wordToObject(const VocabularyStore::Word& w)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), w.id);
    o.insert(QStringLiteral("word"), w.word);
    o.insert(QStringLiteral("lang"), w.lang);
    o.insert(QStringLiteral("source"), w.source);
    o.insert(QStringLiteral("definition"), w.definition);
    o.insert(QStringLiteral("bookKey"), w.bookKey);
    o.insert(QStringLiteral("bookTitle"), w.bookTitle);
    o.insert(QStringLiteral("context"), w.context);
    o.insert(QStringLiteral("spine"), w.spine);
    o.insert(QStringLiteral("offset"), w.offset);
    o.insert(QStringLiteral("ts"), static_cast<double>(w.ts));
    return o;
}

static QVector<VocabularyStore::Word> readAll()
{
    QVector<VocabularyStore::Word> out;
    const QByteArray raw = store().value(VocabularyStore::itemsKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(raw).array())
        if (v.isObject())
        {
            const VocabularyStore::Word w = wordFromObject(v.toObject());
            if (!w.id.isEmpty()) out.push_back(w);
        }
    return out;
}

static void writeAll(const QVector<VocabularyStore::Word>& items)
{
    QJsonArray arr;
    for (const VocabularyStore::Word& w : items) arr.append(wordToObject(w));
    store().setValue(VocabularyStore::itemsKey(),
                     QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

VocabularyStore::Word VocabularyStore::add(const Word& in)
{
    Word w = in;
    w.lang = LookupRequest::normalizeLang(w.lang);
    w.word = LookupRequest::normalizeTerm(w.word);
    if (w.word.isEmpty()) return Word();

    w.id = idFor(w.word, w.lang);
    w.ts = QDateTime::currentSecsSinceEpoch();

    // UPDATE, never duplicate: the same word in the same language is one row, carrying the most recent
    // definition and the most recent place it was met.
    QVector<Word> items = readAll();
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].id == w.id) items.remove(i);
    items.push_back(w);
    writeAll(items);

    // A word removed and then looked up again comes back: clear the stale tombstone so it does not
    // self-suppress on the next merge (the same "deletion undone" path bookmarks and highlights take).
    Tombstones::remove(tombstoneStore(), w.id);

    fireChanged();
    return w;
}

void VocabularyStore::remove(const QString& id)
{
    if (id.isEmpty()) return;
    QVector<Word> items = readAll();
    bool removed = false;
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].id == id) { items.remove(i); removed = true; }
    if (!removed) return;
    writeAll(items);
    Tombstones::record(tombstoneStore(), id);
    fireChanged();
}

QVector<VocabularyStore::Word> VocabularyStore::all()
{
    QVector<Word> out = readAll();
    // Most recent first. The tie-break is the word itself, so two rows written in the same second order the
    // same way on every device — an order that depended on the array's happenstance would make two peers
    // disagree about a list they hold identical bytes for.
    std::sort(out.begin(), out.end(), [](const Word& a, const Word& b) {
        if (a.ts != b.ts) return a.ts > b.ts;
        return a.word.localeAwareCompare(b.word) < 0;
    });
    return out;
}

int VocabularyStore::count() { return int(readAll().size()); }

VocabularyStore::Word VocabularyStore::byId(const QString& id)
{
    if (id.isEmpty()) return Word();
    for (const Word& w : readAll())
        if (w.id == id) return w;
    return Word();
}
