// crash28repro - standalone minimal Qt Quick reproducer for EverythingBox issue #28.
//
// Target fault (from a real WER dump):
//     Qt6Quick!QQuickRepeater::clear + 0x95, EXCEPTION_ACCESS_VIOLATION reading 0x5,
//     Rax=1 Rbx=4  =>  QPointer::data() reading ExternalRefCountData::strongref at [d+4]
//                      on a garbage QPointer read out of d->deletables.
//
// Qt 6.8.3 source, verbatim:
//     void QQuickRepeater::clear() {
//         if (d->model) {
//             for (int i = d->deletables.size() - 1; i >= 0; --i) {
//                 if (QQuickItem *item = d->deletables.at(i)) {   // at() unchecked in release
//                     if (complete) emit itemRemoved(i, item);    // runs arbitrary QML
//                     d->model->release(item);                    // runs arbitrary C++/QML
//                 }
//             }
//             ...
//         }
//         d->deletables.clear();
//     }
//     void QQuickRepeater::regenerate() { clear(); ...; d->deletables.resize(d->itemCount); d->requestItems(); }
//     void QQuickRepeaterPrivate::requestItems() { model->object(i, QQmlIncubator::AsynchronousIfNested); }
//     void QQuickRepeater::initItem(int index, QObject *o) {
//         if (index >= d->deletables.size()) d->deletables.resize(d->model->count() + 1);  // REALLOC
//         ...
//     }
//     void QQuickRepeater::itemChange(c, v) { if (c == ItemParentHasChanged) regenerate(); }
//
// This binary hammers that shape and, crucially, watches d->deletables' heap buffer
// directly (address + size) from inside the walk, so a *near miss* (buffer reallocated
// mid-walk but the stale read happened to be benign) is distinguishable from nothing.

#include <QtGui/QGuiApplication>
#include <QtQuick/QQuickView>
#include <QtQuick/QQuickItem>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlIncubationController>
#include <QtQml/qqml.h>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtCore/QTimer>
#include <QtCore/QElapsedTimer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QBuffer>
#include <QtCore/QDebug>
#include <QtCore/QPointer>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QEvent>
#include <QtGui/QImage>

#include <QtQuick/private/qquickitem_p.h>
#include <QtQuick/private/qquickrepeater_p.h>
#include <QtQmlModels/private/qqmlobjectmodel_p.h>   // QQmlInstanceModel (complete type)

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
// ---------------------------------------------------------------------------
// First-chance AV reporter. Prints exactly the fields the production WER dump
// gave us -- fault module + offset, read/write, bad address, and the registers
// that make the QPointer arithmetic checkable (bad addr == Rax + 4) -- then
// lets the exception continue so WER still writes its dump.
// ---------------------------------------------------------------------------
static LONG CALLBACK crash28Vectored(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    const quintptr addr = quintptr(er->ExceptionAddress);
    HMODULE h = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(addr), &h);
    wchar_t name[MAX_PATH] = L"?";
    if (h)
        ::GetModuleFileNameW(h, name, MAX_PATH);
    const wchar_t *base = wcsrchr(name, L'\\');
    std::printf("\n########## ACCESS VIOLATION ##########\n");
    std::printf("  module    : %ls\n", base ? base + 1 : name);
    std::printf("  at        : 0x%llx  +0x%llx\n", (unsigned long long)addr,
                (unsigned long long)(h ? addr - quintptr(h) : 0));
    std::printf("  operation : %s\n", er->ExceptionInformation[0] == 0 ? "READ"
                                    : er->ExceptionInformation[0] == 1 ? "WRITE" : "EXEC");
    std::printf("  bad addr  : 0x%llx\n", (unsigned long long)er->ExceptionInformation[1]);
#if defined(_M_X64)
    const CONTEXT *c = ep->ContextRecord;
    std::printf("  Rax=0x%llx Rbx=0x%llx Rcx=0x%llx Rdx=0x%llx Rsi=0x%llx Rdi=0x%llx\n",
                (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
                (unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
                (unsigned long long)c->Rsi, (unsigned long long)c->Rdi);
    const unsigned long long bad = (unsigned long long)er->ExceptionInformation[1];
    std::printf("  QPointer check: Rax+4=0x%llx %s bad addr\n",
                (unsigned long long)c->Rax + 4,
                ((unsigned long long)c->Rax + 4 == bad) ? "==" : "!=");
#endif
    std::printf("######################################\n");
    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// ---------------------------------------------------------------------------
// Layout mirror of QQuickRepeaterPrivate (qquickrepeater_p_p.h, Qt 6.8.3).
// Members are private there; the mirror reproduces base + declaration order so
// we can read d->deletables directly. Validated at runtime (see deepSelfCheck).
// ---------------------------------------------------------------------------
struct RepeaterPrivMirror : public QQuickItemPrivate
{
    QPointer<QQmlInstanceModel> model;
    QVariant dataSource;
    QPointer<QObject> dataSourceAsObject;
    bool ownModel : 1;
    bool dataSourceIsObject : 1;
    bool delegateValidated : 1;
    int itemCount;
    QVector<QPointer<QQuickItem> > deletables;
};

static bool g_deepOk = false;

static RepeaterPrivMirror *mirrorOf(QQuickItem *rep)
{
    return static_cast<RepeaterPrivMirror *>(QQuickItemPrivate::get(rep));
}

struct BufSnap { const void *buf = nullptr; int size = -1; int itemCount = -1; int lastIndex = -1; };

static BufSnap snapOf(QObject *o)
{
    BufSnap s;
    QQuickItem *it = qobject_cast<QQuickItem *>(o);
    if (!it || !g_deepOk)
        return s;
    RepeaterPrivMirror *m = mirrorOf(it);
    s.buf = static_cast<const void *>(m->deletables.constData());
    s.size = m->deletables.size();
    s.itemCount = m->itemCount;
    return s;
}

// ---------------------------------------------------------------------------
// Loopback HTTP server with configurable latency + jitter. Serves synthetic
// bytes we generate ourselves. Binds 127.0.0.1 only.
// ---------------------------------------------------------------------------
class SlowServer : public QTcpServer
{
    Q_OBJECT
public:
    int latencyMs = 40;
    int jitterMs = 40;
    QByteArray png;
    QByteArray elementQml;
    qint64 served = 0;

protected:
    void incomingConnection(qintptr sd) override
    {
        QTcpSocket *s = new QTcpSocket(this);
        s->setSocketDescriptor(sd);
        connect(s, &QTcpSocket::readyRead, this, [this, s] {
            QByteArray req = s->peek(4096);
            int eoh = req.indexOf("\r\n\r\n");
            if (eoh < 0)
                return;
            s->readAll();
            int sp1 = req.indexOf(' ');
            int sp2 = req.indexOf(' ', sp1 + 1);
            QByteArray path = req.mid(sp1 + 1, sp2 - sp1 - 1);
            int d = latencyMs + (jitterMs > 0 ? int(QRandomGenerator::global()->bounded(jitterMs)) : 0);
            QPointer<QTcpSocket> sp = s;
            QTimer::singleShot(d, s, [this, sp, path] {
                if (!sp)
                    return;
                QByteArray body, ctype;
                if (path.contains(".qml")) { body = elementQml; ctype = "text/plain"; }
                else { body = png; ctype = "image/png"; }
                QByteArray hdr = "HTTP/1.1 200 OK\r\nContent-Type: " + ctype
                        + "\r\nCache-Control: no-store\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
                sp->write(hdr);
                sp->write(body);
                sp->flush();
                sp->disconnectFromHost();
                ++served;
            });
        });
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
    }
};

// ---------------------------------------------------------------------------
// A model that can emit remove / insert / MOVE change sets on demand. The move
// branch of QQuickRepeater::modelUpdated() is the one code path that *reassigns*
// d->deletables wholesale (d->deletables = mid + items + mid), freeing the block
// the outer clear() is still indexing.
// ---------------------------------------------------------------------------
class ChurnModel : public QAbstractListModel
{
    Q_OBJECT
public:
    int n = 30;
    int rowCount(const QModelIndex &p = QModelIndex()) const override { return p.isValid() ? 0 : n; }
    QVariant data(const QModelIndex &i, int role) const override
    { Q_UNUSED(role); return i.row(); }
    QHash<int, QByteArray> roleNames() const override
    { return {{Qt::UserRole + 1, QByteArrayLiteral("value")}}; }

    void doMove()
    {
        if (n < 4) return;
        if (!beginMoveRows(QModelIndex(), 0, 0, QModelIndex(), n)) return;
        endMoveRows();
    }
    void doRemove() { if (n < 2) return; beginRemoveRows(QModelIndex(), 0, 0); --n; endRemoveRows(); }
    void doRemoveMany(int k)
    {
        k = qMin(k, n - 2);
        if (k <= 0) return;
        beginRemoveRows(QModelIndex(), 0, k - 1);
        n -= k;
        endRemoveRows();
    }
    void reset(int newN) { beginResetModel(); n = newN; endResetModel(); }
    void doInsert() { beginInsertRows(QModelIndex(), 0, 0); ++n; endInsertRows(); }
};

// ---------------------------------------------------------------------------
// Manually driven incubation controller (so incubation can be pumped from an
// arbitrary point, including from inside a clear() walk).
// ---------------------------------------------------------------------------
class PumpCtl : public QObject, public QQmlIncubationController
{
    Q_OBJECT
public:
    int budgetMs = 3;
    void start(int ms) { m_t = startTimer(ms); }
protected:
    void timerEvent(QTimerEvent *) override { incubateFor(budgetMs); }
private:
    int m_t = 0;
};

// ---------------------------------------------------------------------------
// Probe: QML-facing driver + instrumentation.
// ---------------------------------------------------------------------------
class Probe : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList elements READ elements NOTIFY elementsChanged)
    Q_PROPERTY(int innerCount READ innerCount NOTIFY innerCountChanged)
    Q_PROPERTY(int inner2Count MEMBER m_inner2Count CONSTANT)
    Q_PROPERTY(bool asyncLoader MEMBER m_asyncLoader CONSTANT)
    Q_PROPERTY(bool asyncImages MEMBER m_asyncImages CONSTANT)
    Q_PROPERTY(bool useImages MEMBER m_useImages CONSTANT)
    Q_PROPERTY(QString elementUrl MEMBER m_elementUrl CONSTANT)
    Q_PROPERTY(bool useChurn MEMBER m_useChurn CONSTANT)
    Q_PROPERTY(bool useChurnOuter MEMBER m_useChurnOuter CONSTANT)
    Q_PROPERTY(QObject *churn READ churnObj CONSTANT)

public:
    // ---- knobs ----
    QString mutMode = "none";  // none|model|pump|events|killrep|modelrow|release
    int mutAtIndex = -1;       // -1 = every itemRemoved, else only that index
    QString mutWho = "inner";  // which repeater's itemRemoved fires the mutation
    int m_inner2Count = 0;
    bool m_asyncLoader = false;
    bool m_asyncImages = false;
    bool m_useImages = false;
    QString m_elementUrl;
    PumpCtl *ctl = nullptr;
    int pumpMs = 5;
    int mutShrink = 150;
    bool m_useChurnOuter = false;
    int mutModelCount = 0;   // >0: mut=model grows the model to this size (forces realloc)
    bool m_useChurn = false;
    ChurnModel *churn = nullptr;
    QObject *churnObj() const { return churn; }
    bool verbose = false;

    // ---- counters ----
    qint64 seq = 0;
    qint64 nRem = 0, nAdd = 0, nWalks = 0, nLoaded = 0, nDestroy = 0;
    qint64 nAddInsideWalk = 0;      // incubation completion landed inside a clear() walk
    qint64 nOtherInsideWalk = 0;    // any other instrumented event inside a walk
    qint64 nBufRealloc = 0;         // deletables heap buffer moved mid-walk  <-- the bug
    qint64 nMutFreedBuf = 0;        // the mutation moved (= freed) the walked buffer
    qint64 nMutResized = 0;         // the mutation resized it in place
    qint64 nOobRem = 0;            // itemRemoved(i) with i >= deletables.size()  <-- the fault
    QHash<QObject *, BufSnap> perRep;
    qint64 nSizeChange = 0;         // deletables size changed mid-walk
    qint64 nRegenNested = 0;        // regenerate() re-entered while one is on the stack
    qint64 nCycles = 0;
    int regenDepth = 0;
    QStringList anomalies;

    // ---- self-destruct experiment (issue #28 "the Repeater outlives its own clear()") ----
    // Hypothesis: on the teardown path the Repeater is itself a child of the dying
    // parent, so its own destruction can interleave with its in-flight clear().
    // Destroying the Repeater frees QQuickRepeaterPrivate and, with it, the
    // deletables heap block the walk is still indexing.
    QString mutVia = "rem";          // rem | release | detach  -- where the destruct is fired FROM
    int poisonBlocks = 0;            // >0: reclaim the freed block and stamp quintptr(1)
    bool sdLive = false;             // allow firing on live-path clears too
    qint64 nSdFired = 0, nSdRepDied = 0;
    qint64 nTripTotal = 0, nTripInWalk = 0;      // delegate dtor ran / ran inside a walk
    qint64 nDetachTotal = 0, nDetachInWalk = 0;  // delegate itemChange(parent->null) inside a walk
    bool sdArmed = false;            // a self-destruct already fired in this walk: stop probing
    QVector<void *> m_poison;

    // walk tracking (a clear() walk = strictly descending itemRemoved indices, same repeater)
    QObject *walkRep = nullptr;
    int walkLastIdx = -1;
    BufSnap walkSnap;
    qint64 addsSinceLastRem = 0;
    qint64 othersSinceLastRem = 0;
    // Path-independent (works for the live modelUpdated->clear() walk too, which is
    // not bracketed by regenEnter): anything counted between two strictly descending
    // itemRemoved emissions of the same Repeater ran INSIDE clear()'s loop.
    qint64 tripsSinceLastRem = 0, detachSinceLastRem = 0, qmlRemSinceLastRem = 0;
    qint64 nTripInAnyWalk = 0, nDetachInAnyWalk = 0, nQmlRemInAnyWalk = 0;

    // ---- view driving ----
    int outerCount = 6;
    int m_innerCount = 20;
    int gen = 0;

    QVariantList elements() const
    {
        QVariantList l;
        // alternate the payload so the model is genuinely different each switch
        int n = outerCount - (gen % 2);
        for (int i = 0; i < n; ++i) {
            QVariantMap m;
            m["k"] = gen * 1000 + i;
            l << m;
        }
        return l;
    }
    int innerCount() const { return m_innerCount; }

    QStringList critical;   // never crowded out by high-volume informational notes
    void note(const QString &s)
    {
        if (anomalies.size() < 400)
            anomalies << s;
    }
    void notec(const QString &s)
    {
        if (critical.size() < 200)
            critical << s;
    }

    Q_INVOKABLE QString imgUrl(int i)
    {
        return m_imgBase + QString::number(i) + ".png?n=" + QString::number(++m_nonce);
    }
    QString m_imgBase;
    qint64 m_nonce = 0;

    Q_INVOKABLE void ev(const QString &kind, QObject *rep, int index)
    {
        ++seq;
        if (kind == QLatin1String("REM")) {
            ++nRem;
            // Once a self-destruct has fired in this walk the Repeater is freed;
            // reading its private again would be our own UB, not the bug's.
            BufSnap now = sdArmed ? BufSnap() : snapOf(rep);
            // Nesting-immune detectors, evaluated on every itemRemoved emission.
            if (g_deepOk && now.size >= 0) {
                // (1) clear()'s at(i) read past the end of the live buffer.
                if (index >= now.size) {
                    ++nOobRem;
                    notec(QString("OOB-AT seq=%1 rep=%2 idx=%3 >= size=%4 buf=%5")
                                 .arg(seq).arg(rep->objectName()).arg(index).arg(now.size)
                                 .arg(quintptr(now.buf), 0, 16));
                }
                // (2) the buffer this repeater is walking moved or resized since its
                //     previous itemRemoved emission (nesting-immune: keyed per repeater).
                auto it = perRep.find(rep);
                // Only compare within one clear() walk: clear() iterates strictly
                // downwards, so a non-descending index means a *different* walk and any
                // buffer difference is ordinary churn, not mid-walk mutation.
                if (it != perRep.end() && index < it->lastIndex) {
                    if (it->buf != now.buf) {
                        ++nBufRealloc;
                        notec(QString("BUF-MOVED seq=%1 rep=%2 idx=%3 %4->%5 size %6->%7")
                                     .arg(seq).arg(rep->objectName()).arg(index)
                                     .arg(quintptr(it->buf), 0, 16).arg(quintptr(now.buf), 0, 16)
                                     .arg(it->size).arg(now.size));
                    } else if (it->size != now.size) {
                        ++nSizeChange;
                        note(QString("SIZE-CHANGED seq=%1 rep=%2 idx=%3 size %4->%5")
                                     .arg(seq).arg(rep->objectName()).arg(index)
                                     .arg(it->size).arg(now.size));
                    }
                }
                now.lastIndex = index;
                perRep[rep] = now;
            }
            if (rep == walkRep && index < walkLastIdx) {
                // continuing the same descending walk: everything since the last
                // REM happened *inside* QQuickRepeater::clear()'s loop.
                if (addsSinceLastRem > 0) {
                    nAddInsideWalk += addsSinceLastRem;
                    note(QString("ADD-INSIDE-WALK seq=%1 rep=%2 idx=%3 adds=%4")
                                 .arg(seq).arg(rep->objectName()).arg(index).arg(addsSinceLastRem));
                }
                if (othersSinceLastRem > 0)
                    nOtherInsideWalk += othersSinceLastRem;
                nTripInAnyWalk   += tripsSinceLastRem;
                nDetachInAnyWalk += detachSinceLastRem;
                nQmlRemInAnyWalk += qmlRemSinceLastRem;
                if (g_deepOk && walkSnap.size >= 0) {
                    if (now.buf != walkSnap.buf) {
                        ++nBufRealloc;
                        notec(QString("BUF-REALLOC-MID-WALK seq=%1 rep=%2 idx=%3 %4->%5 size %6->%7")
                                     .arg(seq).arg(rep->objectName()).arg(index)
                                     .arg(quintptr(walkSnap.buf), 0, 16).arg(quintptr(now.buf), 0, 16)
                                     .arg(walkSnap.size).arg(now.size));
                    }
                    if (now.size != walkSnap.size) {
                        ++nSizeChange;
                        note(QString("SIZE-CHANGE-MID-WALK seq=%1 rep=%2 idx=%3 %4->%5")
                                     .arg(seq).arg(rep->objectName()).arg(index)
                                     .arg(walkSnap.size).arg(now.size));
                    }
                }
            } else {
                ++nWalks;  // first REM of a new walk
                mutFiredThisWalk = false;
                sdArmed = false;
            }
            walkRep = rep;
            walkLastIdx = index;
            walkSnap = now;
            addsSinceLastRem = 0;
            othersSinceLastRem = 0;
            tripsSinceLastRem = detachSinceLastRem = qmlRemSinceLastRem = 0;
            if (verbose)
                std::printf("[%lld] REM %s idx=%d buf=%p size=%d ic=%d\n", (long long)seq,
                            qPrintable(rep ? rep->objectName() : QStringLiteral("?")), index,
                            now.buf, now.size, now.itemCount);
            maybeMutate(rep, index, QStringLiteral("REM"));
            return;
        }
        if (kind == QLatin1String("ADD")) {
            ++nAdd;
            ++addsSinceLastRem;
            if (verbose)
                std::printf("[%lld] ADD %s idx=%d\n", (long long)seq,
                            qPrintable(rep ? rep->objectName() : QStringLiteral("?")), index);
            return;
        }
        if (kind == QLatin1String("LOADED")) { ++nLoaded; ++othersSinceLastRem; return; }
        if (kind == QLatin1String("DESTROY")) { ++nDestroy; ++othersSinceLastRem; return; }
        ++othersSinceLastRem;
    }

    // Called from QQuickRepeater::itemChange override, bracketing regenerate()->clear().
    qint64 nRegen = 0, nRegenModelNull = 0, nRegenWithItems = 0, nRegenIncomplete = 0;
    QMap<QString, qint64> regenByWho;

    // vehicle 5: QQmlInstanceModel::destroyingItem. This is emitted from inside
    // QQmlDelegateModelPrivate::release(), i.e. from the LAST statement of
    // clear()'s loop body -- so a free here lands the next d->deletables.at(i)
    // on freed memory, which is where the production dump faulted.
    qint64 nDestroyingTotal = 0, nDestroyingInWalk = 0;
    QSet<QObject *> m_hooked;
    void hookModel(QQuickItem *rep)
    {
        if (!g_deepOk || !rep)
            return;
        QQmlInstanceModel *m = mirrorOf(rep)->model;
        if (!m || m_hooked.contains(m))
            return;
        m_hooked.insert(m);
        if (verbose) {
            std::printf("HOOK-MODEL rep=%s model=%s\n", qPrintable(rep->objectName()),
                        m->metaObject()->className());
            std::fflush(stdout);
        }
        QObject::connect(m, &QQmlInstanceModel::destroyingItem, m, [this](QObject *) {
            ++nDestroyingTotal;
            if (regenDepth > 0)
                ++nDestroyingInWalk;
            if (mutVia == QLatin1String("destroying") && sdGate())
                selfDestruct(walkRep, "destroying");
        }, Qt::DirectConnection);
    }

    void regenEnter(QObject *rep)
    {
        ++seq;
        ++nRegen;
        regenByWho[rep->objectName()]++;
        QQuickItem *it = qobject_cast<QQuickItem *>(rep);
        hookModel(it);
        if (g_deepOk && it) {
            RepeaterPrivMirror *m = mirrorOf(it);
            const bool modelNull = (m->model == nullptr);
            if (modelNull)
                ++nRegenModelNull;
            if (!m->componentComplete)
                ++nRegenIncomplete;
            if (!modelNull && m->deletables.size() > 0)
                ++nRegenWithItems;
            if (verbose)
                std::printf("[%lld] >REGEN %s modelNull=%d complete=%d size=%lld ic=%d buf=%p\n",
                            (long long)seq, qPrintable(rep->objectName()), int(modelNull),
                            int(m->componentComplete), (long long)m->deletables.size(),
                            m->itemCount, (const void *)m->deletables.constData());
        }
        if (regenDepth > 0) {
            ++nRegenNested;
            note(QString("NESTED-REGENERATE seq=%1 rep=%2 depth=%3")
                         .arg(seq).arg(rep->objectName()).arg(regenDepth));
        }
        ++regenDepth;
    }
    void regenExit(QObject *) { --regenDepth; }

    bool mutBusy = false;
    bool mutFiredThisWalk = false;
    qint64 nMutations = 0;

    // ------------------------------------------------------------------
    // Self-destruct: destroy the Repeater while its own clear() is walking.
    // ------------------------------------------------------------------
    bool isSdMode() const { return mutMode.startsWith(QLatin1String("sd-")); }

    // Common gate for the non-itemRemoved vehicles (release / detach), which do
    // not come through maybeMutate() and so need the same filtering.
    bool sdGate() const
    {
        if (!isSdMode() || mutBusy || mutFiredThisWalk || sdArmed)
            return false;
        // must be inside itemChange->regenerate()->clear() unless --sd-live, which
        // also allows the modelUpdated(reset)->regenerate()->clear() (live) path.
        if (!sdLive && regenDepth <= 0)
            return false;
        if (!walkRep)
            return false;
        if (!mutWho.isEmpty() && walkRep->objectName() != mutWho)
            return false;
        if (mutAtIndex >= 0 && walkLastIdx != mutAtIndex)
            return false;
        // loop 1 needs iterations left; loop 2 (detach) runs after walkLastIdx hit 0.
        return sdLive ? (walkLastIdx >= 0) : (walkLastIdx > 0);
    }

    // Reclaim the block the destruct just freed and stamp it with quintptr(1) --
    // the exact QPointer::d value from the production dump (bad addr = 1+4 = 5).
    void poisonFreed(int nbytes)
    {
        if (poisonBlocks <= 0 || nbytes <= 0)
            return;
        // QList's allocation is sizeof(QArrayData) + capacity*sizeof(T); spread the
        // reclaim sizes a little so one of them lands on the freed block.
        for (int k = 0; k < poisonBlocks; ++k) {
            const size_t sz = size_t(nbytes) + size_t(8 * (k % 8));
            void *p = ::malloc(sz);
            if (!p)
                break;
            quintptr *q = static_cast<quintptr *>(p);
            for (size_t j = 0; j < sz / sizeof(quintptr); ++j)
                q[j] = 1;
            m_poison.append(p);
        }
    }

    void selfDestruct(QObject *rep, const char *vehicle)
    {
        if (!rep)
            return;
        int nbytes = 0;
        if (g_deepOk) {
            if (QQuickItem *it = qobject_cast<QQuickItem *>(rep)) {
                RepeaterPrivMirror *m = mirrorOf(it);
                nbytes = int(m->deletables.size() * sizeof(QPointer<QQuickItem>));
            }
        }
        ++nSdFired;
        mutBusy = true;
        mutFiredThisWalk = true;
        sdArmed = true;
        ++nMutations;
        QPointer<QObject> watch(rep);
        notec(QString("SD-FIRE via=%1 mode=%2 rep=%3 seq=%4 walkIdx=%5 bufBytes=%6")
                     .arg(QLatin1String(vehicle)).arg(mutMode).arg(rep->objectName())
                     .arg(seq).arg(walkLastIdx).arg(nbytes));
        if (verbose) {
            std::printf("[%lld] SD-FIRE via=%s mode=%s rep=%s walkIdx=%d bufBytes=%d\n",
                        (long long)seq, vehicle, qPrintable(mutMode),
                        qPrintable(rep->objectName()), walkLastIdx, nbytes);
            std::fflush(stdout);
        }

        if (mutMode == QLatin1String("sd-delete")) {
            // The raw shape: the Repeater dies under its own clear().
            delete rep;
        } else if (mutMode == QLatin1String("sd-later")) {
            // The production shape: a deferred delete, flushed by a nested
            // event dispatch that happens to run inside the walk.
            rep->deleteLater();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        } else if (mutMode == QLatin1String("sd-events")) {
            rep->deleteLater();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        } else if (mutMode == QLatin1String("sd-qparent")) {
            // Delete the QObject that owns the Repeater: it dies as a child.
            if (QObject *pp = rep->parent())
                delete pp;
            else
                delete rep;
        } else if (mutMode == QLatin1String("sd-loader")) {
            QObject *o = rep->parent();
            for (int hop = 0; o && hop < 12; ++hop, o = o->parent()) {
                if (o->inherits("QQuickLoader")) {
                    o->setProperty("active", false);
                    break;
                }
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }

        if (!watch)
            ++nSdRepDied;
        walkRep = nullptr;   // never touch it again from the probe
        poisonFreed(nbytes);
        mutBusy = false;
    }

    // vehicle 4: a plain QML `onItemRemoved:` handler on the Repeater. Whether it
    // still runs on the TEARDOWN path (QQmlContext half torn down) decides whether
    // app QML can reach inside the walk at all.
    qint64 nQmlRemTotal = 0, nQmlRemInWalk = 0;
    Q_INVOKABLE void qmlRem(int index)
    {
        ++nQmlRemTotal;
        ++qmlRemSinceLastRem;
        if (regenDepth > 0)
            ++nQmlRemInWalk;
        if (mutVia == QLatin1String("qml") && isSdMode() && !mutBusy && !mutFiredThisWalk
                && !sdArmed && regenDepth > 0 && index > 0 && walkRep
                && (mutWho.isEmpty() || walkRep->objectName() == mutWho))
            selfDestruct(walkRep, "qml");
    }

    // vehicle 2: the delegate's destructor, i.e. inside d->model->release(item).
    void tripwireDtor()
    {
        ++nTripTotal;
        ++tripsSinceLastRem;
        if (regenDepth > 0)
            ++nTripInWalk;
        if (verbose)
            std::printf("      DTOR regenDepth=%d walkRep=%s walkIdx=%d\n", regenDepth,
                        qPrintable(walkRep ? walkRep->objectName() : QStringLiteral("-")), walkLastIdx);
        if (mutVia == QLatin1String("release") && sdGate())
            selfDestruct(walkRep, "release");
    }

    // vehicle 3: the delegate being detached, i.e. clear()'s SECOND loop
    // (for (item : deletables) item->setParentItem(nullptr)).
    void detachHook()
    {
        ++nDetachTotal;
        ++detachSinceLastRem;
        if (regenDepth > 0)
            ++nDetachInWalk;
        if (verbose)
            std::printf("      DETACH regenDepth=%d walkRep=%s walkIdx=%d\n", regenDepth,
                        qPrintable(walkRep ? walkRep->objectName() : QStringLiteral("-")), walkLastIdx);
        if (mutVia == QLatin1String("detach") && sdGate())
            selfDestruct(walkRep, "detach");
    }

    void maybeMutate(QObject *rep, int index, const QString &)
    {
        if (mutMode == QLatin1String("none") || !rep)
            return;
        if (isSdMode()) {
            if (mutVia != QLatin1String("rem"))
                return;                     // another vehicle owns the firing
            if (mutBusy || mutFiredThisWalk || sdArmed)
                return;
            if (!mutWho.isEmpty() && rep->objectName() != mutWho)
                return;
            if (mutAtIndex >= 0 && index != mutAtIndex)
                return;
            if (regenDepth <= 0 || index <= 0)
                return;
            selfDestruct(rep, "rem");
            return;
        }
        if (mutBusy || mutFiredThisWalk)   // once per walk, never re-entrant
            return;
        if (!mutWho.isEmpty() && rep->objectName() != mutWho)
            return;
        if (mutAtIndex >= 0 && index != mutAtIndex)
            return;
        mutBusy = true;
        mutFiredThisWalk = true;
        ++nMutations;
        struct Guard { bool *b; ~Guard() { *b = false; } } g{&mutBusy};
        // Direct before/after measurement of what the mutation did to the very
        // buffer clear() is walking -- independent of any walk heuristic.
        const BufSnap before = snapOf(rep);
        struct Post {
            Probe *p; QObject *r; BufSnap b;
            ~Post() {
                if (!g_deepOk || b.size < 0) return;
                const BufSnap a = snapOf(r);
                if (a.buf != b.buf) {
                    ++p->nMutFreedBuf;
                    p->notec(QString("MUT-MOVED-BUF rep=%1 %2->%3 size %4->%5")
                                    .arg(r->objectName())
                                    .arg(quintptr(b.buf), 0, 16).arg(quintptr(a.buf), 0, 16)
                                    .arg(b.size).arg(a.size));
                } else if (a.size != b.size) {
                    ++p->nMutResized;
                    p->note(QString("MUT-RESIZED rep=%1 size %2->%3")
                                    .arg(r->objectName()).arg(b.size).arg(a.size));
                }
            }
        } post{this, rep, before};

        if (mutMode == QLatin1String("model")) {
            // Re-entrant regenerate(): clear() + resize() on the buffer being walked.
            // NB: QList::clear() keeps the allocation, so shrinking is benign. Only a
            // resize past the current *capacity* frees the buffer the outer clear() is
            // still indexing -- that is the condition that turns OOB into garbage.
            const int n = (mutModelCount > 0) ? (mutModelCount + int(m_bump++ % 2))
                                              : (3 + int(m_bump++ % 7));
            rep->setProperty("model", QVariant(n));
        } else if (mutMode == QLatin1String("pump")) {
            if (ctl)
                ctl->incubateFor(pumpMs);
        } else if (mutMode == QLatin1String("events")) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        } else if (mutMode == QLatin1String("unload")) {
            // Realistic shape: a handler running during teardown flips an ancestor
            // Loader off. If QQuickLoader deletes its item synchronously the Repeater
            // dies while its own clear() is still on the stack.
            QObject *o = rep->parent();
            for (int hop = 0; o && hop < 12; ++hop, o = o->parent()) {
                if (o->inherits("QQuickLoader")) {
                    o->setProperty("active", false);
                    break;
                }
            }
        } else if (mutMode == QLatin1String("deletelater")) {
            rep->deleteLater();
        } else if (mutMode == QLatin1String("killrep")) {
            delete rep;  // destroy the Repeater whose clear() is on the stack
        } else if (mutMode == QLatin1String("combo")) {
            // (1) re-entrant regenerate -> deletables.clear() (size 0, capacity kept)
            const int cnt = (mutModelCount > 0) ? mutModelCount : 3;
            rep->setProperty("model", QVariant(cnt + int(m_bump++ % 2)));
            // (2) let a pending async incubation land: initItem() now sees
            //     index >= size(0) and calls resize(model->count()+1) -- past the
            //     retained capacity this REALLOCATES and frees the walked block.
            if (ctl)
                ctl->incubateFor(pumpMs);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        } else if (mutMode == QLatin1String("shrinkmove")) {
            // The one mutation that can leave a stale index outside the LIVE allocation:
            //   (1) remove many rows  -> size drops, QList keeps the big capacity (benign)
            //   (2) a MOVE change set -> modelUpdated does
            //         d->deletables = mid(0,i) + items + mid(i)
            //       which allocates a NEW, exactly-sized (now much smaller) block and
            //       FREES the big one clear() is still indexing.
            if (churn) {
                churn->doRemoveMany(mutShrink);
                churn->doMove();
            }
        } else if (mutMode == QLatin1String("rowmove")) {
            if (churn) churn->doMove();
        } else if (mutMode == QLatin1String("rowremove")) {
            if (churn) churn->doRemove();
        } else if (mutMode == QLatin1String("rowinsert")) {
            if (churn) churn->doInsert();
        } else if (mutMode == QLatin1String("churnpump")) {
            if (churn) {
                switch (int(m_bump++ % 3)) {
                case 0: churn->doMove(); break;
                case 1: churn->doRemove(); break;
                default: churn->doInsert(); break;
                }
            }
            if (ctl) ctl->incubateFor(pumpMs);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        } else if (mutMode == QLatin1String("rowchurn")) {
            if (churn) {
                switch (int(m_bump++ % 3)) {
                case 0: churn->doMove(); break;
                case 1: churn->doRemove(); break;
                default: churn->doInsert(); break;
                }
            }
        } else if (mutMode == QLatin1String("release")) {
            // force the *parent* item away, re-triggering itemChange(ItemParentHasChanged)
            if (QQuickItem *it = qobject_cast<QQuickItem *>(rep))
                it->setParentItem(nullptr);
        }
    }
    quint64 m_bump = 0;

    bool liveWalks = false;
    bool m_minFired = false, m_minBusy = false;
    Q_INVOKABLE void minimalMutate()
    {
        if (m_minFired || m_minBusy || !churn)
            return;
        m_minFired = true;
        m_minBusy = true;
        churn->doRemoveMany(mutShrink);   // size drops; QList keeps the big capacity
        churn->doMove();                  // modelUpdated reassigns deletables to a
                                          // smaller block and frees the one being walked
        m_minBusy = false;
        ++nMutations;
    }

    Q_INVOKABLE void tickView()
    {
        m_minFired = false;
        ++gen;
        ++nCycles;
        if (m_useChurnOuter && churn) {
            // full model reset -> regenerate() -> clear() with parentItem() still ALIVE,
            // so regenerate can get past its !parentItem() guard and modelUpdated works.
            churn->reset(outerChurnN);
        }
        if (liveWalks) {
            // Reset every inner Repeater's model while its parent is still alive, so
            // regenerate() gets past its !parentItem() guard and can actually resize().
            m_innerCount = baseInner + (gen % 2 ? liveDelta : 0);
            emit innerCountChanged();
        }
        emit elementsChanged();
    }
    int outerChurnN = 200;
    int baseInner = 30;
    int liveDelta = 0;
    void setInner(int n) { m_innerCount = n; emit innerCountChanged(); }

signals:
    void elementsChanged();
    void innerCountChanged();
};

static Probe *g_probe = nullptr;

// ---------------------------------------------------------------------------
// QQuickRepeater subclass: identical code path (clear/regenerate/initItem are
// non-virtual base members), but itemChange is virtual so we can bracket the
// exact regenerate()->clear() that the crash dump implicates.
// ---------------------------------------------------------------------------
class ProbeRepeater : public QQuickRepeater
{
    Q_OBJECT
    QML_ELEMENT
public:
    explicit ProbeRepeater(QQuickItem *p = nullptr) : QQuickRepeater(p)
    {
        // Direct C++ connections: a QML onItemRemoved handler is silently gone by the
        // time the teardown-path clear() runs (its QQmlContext is already invalidated),
        // so QML-side instrumentation of this exact path is blind.
        connect(this, &QQuickRepeater::itemRemoved, this,
                [this](int i, QQuickItem *) { if (g_probe) g_probe->ev(QStringLiteral("REM"), this, i); },
                Qt::DirectConnection);
        connect(this, &QQuickRepeater::itemAdded, this,
                [this](int i, QQuickItem *) { if (g_probe) g_probe->ev(QStringLiteral("ADD"), this, i); },
                Qt::DirectConnection);
    }
protected:
    void itemChange(ItemChange c, const ItemChangeData &v) override
    {
        const bool parentChange = (c == ItemParentHasChanged);
        if (parentChange && g_probe)
            g_probe->regenEnter(this);
        QQuickRepeater::itemChange(c, v);   // -> regenerate() -> clear()
        if (parentChange && g_probe)
            g_probe->regenExit(this);
    }
};

// ---------------------------------------------------------------------------
// Vehicle 2: a C++-backed QML element living inside the delegate. Its DESTRUCTOR
// runs when the delegate object is destroyed -- i.e. from inside
// QQuickRepeater::clear()'s `d->model->release(item)`, if that release destroys
// the delegate synchronously. This is exactly the shape EverythingBox has
// (`Component.onDestruction: nav.removeZone(...)`), only in C++ so it survives
// QQmlContext invalidation on the teardown path.
// ---------------------------------------------------------------------------
class Tripwire : public QObject
{
    Q_OBJECT
    QML_ELEMENT
public:
    explicit Tripwire(QObject *p = nullptr) : QObject(p) {}
    ~Tripwire() override
    {
        if (g_probe)
            g_probe->tripwireDtor();
    }
};

// ---------------------------------------------------------------------------
// Vehicle 3: the delegate root itself. clear()'s SECOND loop calls
// item->setParentItem(nullptr) on every delegate while still iterating
// d->deletables; this override fires from inside that loop.
// ---------------------------------------------------------------------------
class DelegateItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
public:
    explicit DelegateItem(QQuickItem *p = nullptr) : QQuickItem(p) {}
protected:
    void itemChange(ItemChange c, const ItemChangeData &v) override
    {
        QQuickItem::itemChange(c, v);
        if (c == ItemParentHasChanged && v.item == nullptr && g_probe)
            g_probe->detachHook();
    }
};

// ---------------------------------------------------------------------------
// The reduced repro: a plain Repeater (no subclass), a plain QML onItemRemoved
// handler, no Loader, no nesting, no asynchronous incubation, no network.
static const char *kMinimalQml = R"QML(
import QtQuick
Item {
    width: 200; height: 200
    Repeater {
        id: rep
        model: probe.churn
        delegate: Item { width: 1; height: 1 }
        onItemRemoved: function(index, item) { probe.minimalMutate() }
    }
}
)QML";

static const char *kMainQml = R"QML(
import QtQuick
import Crash28
Item {
    id: root
    width: 640; height: 480
    ProbeRepeater {
        id: outerRep
        objectName: "outer"
        model: probe.useChurnOuter ? probe.churn : probe.elements
        delegate: Item {
            id: cell
            width: 100; height: 100
            Component.onDestruction: probe.ev("DESTROY", null, 0)
            Loader {
                anchors.fill: parent
                asynchronous: probe.asyncLoader
                source: probe.elementUrl
                onLoaded: probe.ev("LOADED", null, 0)
            }
        }
    }
}
)QML";

static const char *kElementQml = R"QML(
import QtQuick
import Crash28
Item {
    id: el
    width: 100; height: 100
    ProbeRepeater {
        id: innerRep
        objectName: "inner"
        onItemRemoved: function(index, item) { probe.qmlRem(index) }
        model: probe.useChurn ? probe.churn : probe.innerCount
        delegate: DelegateItem {
            id: leaf
            width: 8; height: 8
            Tripwire { }
            Image {
                anchors.fill: parent
                visible: false
                cache: false
                asynchronous: probe.asyncImages
                source: probe.useImages ? probe.imgUrl(index) : ""
            }
            ProbeRepeater {
                id: inner2Rep
                objectName: "inner2"
                model: probe.inner2Count
                delegate: Item { width: 2; height: 2 }
            }
        }
    }
}
)QML";

// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    qputenv("QT_LOGGING_RULES", "qt.qml.*=false");
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("crash28repro");

    QCommandLineParser p;
    p.addHelpOption();
    auto opt = [&](const char *n, const char *d, const char *def) {
        QCommandLineOption o(QString::fromLatin1(n), QString::fromLatin1(d),
                             QString::fromLatin1(n), QString::fromLatin1(def));
        p.addOption(o);
        return o;
    };
    auto oCycles   = opt("cycles",   "view switches before exiting",        "2000");
    auto oOuter    = opt("outer",    "outer repeater element count",        "6");
    auto oInner    = opt("inner",    "inner repeater delegate count",       "40");
    auto oInner2   = opt("inner2",   "depth-2 repeater count",              "0");
    auto oSwitch   = opt("switch-ms","ms between view switches",            "8");
    auto oJitter   = opt("jitter-ms","random extra ms per switch",          "0");
    auto oMut      = opt("mut",      "none|model|pump|events|killrep|release", "none");
    auto oMutIdx   = opt("mut-at",   "fire mutation only at this index (-1=all)", "-1");
    auto oMutWho   = opt("mut-who",  "objectName of repeater to mutate on",  "inner");
    auto oHttpMs   = opt("http-ms",  "loopback server latency ms",           "40");
    auto oHttpJit  = opt("http-jit", "loopback server jitter ms",            "40");
    auto oCtl      = opt("ctl",      "window|timer incubation controller",   "window");
    auto oCtlMs    = opt("ctl-ms",   "timer controller period ms",           "16");
    auto oPumpMs   = opt("pump-ms",  "incubateFor budget for mut=pump",      "5");
    auto oShrink   = opt("mut-shrink","shrinkmove: rows to remove before the move", "150");
    auto oOuterN   = opt("outer-n",   "churn-outer: model size restored each tick",  "200");
    auto oLiveDelta= opt("live-delta","--live-walks: inner count delta per tick", "200");
    auto oMutModel = opt("mut-model","mut=model target count (>0 forces realloc)", "0");
    auto oMutVia   = opt("mut-via",  "sd-*: rem|release|detach -- what fires the destruct", "rem");
    auto oPoison   = opt("poison",   "sd-*: blocks of the freed size to reclaim + stamp with 1", "0");
    QCommandLineOption oAsyncLoader("async-loader", "Loader { asynchronous: true }");
    QCommandLineOption oAsyncImages("async-images", "async Image loading");
    QCommandLineOption oImages("images", "load Images from the loopback server");
    QCommandLineOption oNetQml("net-qml", "load Element.qml over http (async component load)");
    QCommandLineOption oMinimal("minimal", "load the reduced plain-Repeater repro");
    QCommandLineOption oChurnOuter("churn-outer", "outer Repeater uses the churn model; each tick resets it");
    QCommandLineOption oLive("live-walks", "also reset the inner model each tick (walks with a live parentItem)");
    QCommandLineOption oChurn("churn-model", "inner Repeater uses a QAbstractListModel that can emit move/remove/insert");
    QCommandLineOption oSdLive("sd-live", "sd-*: also fire on live-path clears (drops the regenerate bracket gate)");
    QCommandLineOption oVerbose("verbose", "per-event trace");
    QCommandLineOption oOffscreen("offscreen", "use the offscreen platform plugin");
    p.addOptions({oAsyncLoader, oAsyncImages, oImages, oNetQml, oChurn, oChurnOuter, oLive, oMinimal, oSdLive, oVerbose, oOffscreen});
    p.process(app);

    Probe probe;
    ChurnModel churnModel;
    probe.churn = &churnModel;
    g_probe = &probe;
    probe.outerCount   = p.value(oOuter).toInt();
    probe.m_innerCount = p.value(oInner).toInt();
    churnModel.n       = probe.m_innerCount;
    probe.baseInner    = probe.m_innerCount;
    probe.liveWalks    = p.isSet(oLive);
    probe.mutShrink    = p.value(oShrink).toInt();
    probe.m_useChurnOuter = p.isSet(oChurnOuter);
    probe.outerChurnN  = p.value(oOuterN).toInt();
    if (probe.m_useChurnOuter || p.isSet(oMinimal)) {
        probe.m_useChurnOuter = true;         // tickView() resets the churn model
        churnModel.n = probe.outerChurnN;
    }
    probe.liveDelta    = p.value(oLiveDelta).toInt();
    probe.m_inner2Count= p.value(oInner2).toInt();
    probe.mutMode      = p.value(oMut);
    probe.mutAtIndex   = p.value(oMutIdx).toInt();
    probe.mutWho       = p.value(oMutWho);
    probe.pumpMs       = p.value(oPumpMs).toInt();
    probe.mutModelCount= p.value(oMutModel).toInt();
    probe.mutVia       = p.value(oMutVia);
    probe.poisonBlocks = p.value(oPoison).toInt();
    probe.sdLive       = p.isSet(oSdLive);
    probe.m_asyncLoader= p.isSet(oAsyncLoader);
    probe.m_asyncImages= p.isSet(oAsyncImages);
    probe.m_useImages  = p.isSet(oImages);
    probe.m_useChurn   = p.isSet(oChurn);
    probe.verbose      = p.isSet(oVerbose);
    const int cycles   = p.value(oCycles).toInt();
    const int switchMs = p.value(oSwitch).toInt();
    const int jitterMs = p.value(oJitter).toInt();

    // --- synthetic payload + loopback server -------------------------------
    QByteArray png;
    {
        QImage img(8, 8, QImage::Format_ARGB32);
        img.fill(0xff3366aa);
        QBuffer b(&png);
        b.open(QIODevice::WriteOnly);
        img.save(&b, "PNG");
    }
    SlowServer srv;
    srv.latencyMs = p.value(oHttpMs).toInt();
    srv.jitterMs  = p.value(oHttpJit).toInt();
    srv.png = png;
    srv.elementQml = QByteArray(kElementQml);
    if (!srv.listen(QHostAddress::LocalHost, 0)) {
        std::fprintf(stderr, "listen failed\n");
        return 3;
    }
    const QString base = QStringLiteral("http://127.0.0.1:%1/").arg(srv.serverPort());
    probe.m_imgBase = base + "img/";

    // --- QML on disk (scratch dir, not the repo) ---------------------------
    QString scratch = qEnvironmentVariable("CRASH28_SCRATCH");
    if (scratch.isEmpty())
        scratch = QDir::tempPath() + "/crash28repro";
    QDir().mkpath(scratch);
    auto put = [&](const QString &name, const char *body) {
        QFile f(scratch + "/" + name);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(body);
        f.close();
        return QUrl::fromLocalFile(f.fileName()).toString();
    };
    const QString elementLocal = put("Element.qml", kElementQml);
    const QString minimalLocal = put("Minimal.qml", kMinimalQml);
    const QString mainLocal = p.isSet(oMinimal) ? minimalLocal : put("Main.qml", kMainQml);
    probe.m_elementUrl = p.isSet(oNetQml) ? (base + "e/Element.qml") : elementLocal;

    // --- engine ------------------------------------------------------------
#ifdef _WIN32
    ::AddVectoredExceptionHandler(1, crash28Vectored);
#endif

    qmlRegisterType<ProbeRepeater>("Crash28", 1, 0, "ProbeRepeater");
    qmlRegisterType<Tripwire>("Crash28", 1, 0, "Tripwire");
    qmlRegisterType<DelegateItem>("Crash28", 1, 0, "DelegateItem");

    QQuickView view;
    view.engine()->rootContext()->setContextProperty("probe", &probe);
    view.setResizeMode(QQuickView::SizeRootObjectToView);

    PumpCtl ctl;
    if (p.value(oCtl) == QLatin1String("timer")) {
        view.engine()->setIncubationController(&ctl);
        ctl.start(p.value(oCtlMs).toInt());
        probe.ctl = &ctl;
    } else {
        probe.ctl = nullptr;
    }

    view.setSource(QUrl(mainLocal));
    if (view.status() == QQuickView::Error) {
        for (const auto &e : view.errors())
            std::fprintf(stderr, "QML ERROR: %s\n", qPrintable(e.toString()));
        return 4;
    }
    view.setFlags(view.flags() | Qt::WindowDoesNotAcceptFocus);
    view.resize(640, 480);
    view.setPosition(40, 40);
    view.show();

    // --- deep-instrumentation self check -----------------------------------
    QTimer::singleShot(400, &app, [&] {
        QQuickItem *root = view.rootObject();
        QQuickItem *outer = root ? root->findChild<QQuickItem *>("outer") : nullptr;
        if (!outer && root) outer = root->findChild<QQuickItem *>();
        if (outer) {
            g_deepOk = true;
            RepeaterPrivMirror *m = mirrorOf(outer);
            const int wantCount = outer->property("count").toInt();
            const bool ok = (m->itemCount == wantCount)
                    && (m->deletables.size() == wantCount)
                    && (m->model != nullptr);
            if (!ok) {
                g_deepOk = false;
                std::printf("DEEP-SELFCHECK FAIL: mirror itemCount=%d size=%lld want=%d\n",
                            m->itemCount, (long long)m->deletables.size(), wantCount);
            } else {
                std::printf("DEEP-SELFCHECK OK (deletables buf=%p size=%lld itemCount=%d)\n",
                            (const void *)m->deletables.constData(),
                            (long long)m->deletables.size(), m->itemCount);
            }
        } else {
            std::printf("DEEP-SELFCHECK: outer repeater not found\n");
        }
        std::fflush(stdout);
    });

    // --- driver ------------------------------------------------------------
    QElapsedTimer wall;
    wall.start();
    QTimer drive;
    drive.setSingleShot(true);
    QObject::connect(&drive, &QTimer::timeout, &app, [&] {
        probe.tickView();
        if (probe.nCycles >= cycles) {
            QTimer::singleShot(200, &app, [&] { app.quit(); });
            return;
        }
        int d = switchMs + (jitterMs > 0 ? int(QRandomGenerator::global()->bounded(jitterMs)) : 0);
        drive.start(d);
    });
    QTimer::singleShot(600, &app, [&] { drive.start(switchMs); });

    const int rc = app.exec();

    // --- summary -----------------------------------------------------------
    std::printf("\n=== crash28repro summary ===\n");
    std::printf("mut=%s@%s idx=%d  asyncLoader=%d asyncImages=%d images=%d netQml=%d ctl=%s\n",
                qPrintable(probe.mutMode), qPrintable(probe.mutWho), probe.mutAtIndex,
                int(probe.m_asyncLoader), int(probe.m_asyncImages), int(probe.m_useImages),
                int(p.isSet(oNetQml)), qPrintable(p.value(oCtl)));
    std::printf("outer=%d inner=%d inner2=%d switchMs=%d httpMs=%d/%d deepOk=%d\n",
                probe.outerCount, probe.m_innerCount, probe.m_inner2Count, switchMs,
                srv.latencyMs, srv.jitterMs, int(g_deepOk));
    std::printf("cycles=%lld  elapsed=%.1fs  http_served=%lld\n",
                probe.nCycles, wall.elapsed() / 1000.0, srv.served);
    std::printf("clearWalks=%lld itemRemoved=%lld itemAdded=%lld loaded=%lld destroyed=%lld\n",
                probe.nWalks, probe.nRem, probe.nAdd, probe.nLoaded, probe.nDestroy);
    std::printf("ADD_INSIDE_WALK=%lld  OTHER_INSIDE_WALK=%lld  NESTED_REGEN=%lld\n",
                probe.nAddInsideWalk, probe.nOtherInsideWalk, probe.nRegenNested);
    std::printf("BUF_REALLOC_MID_WALK=%lld  SIZE_CHANGE_MID_WALK=%lld\n",
                probe.nBufRealloc, probe.nSizeChange);
    std::printf("MUTATIONS=%lld  MUT_MOVED_BUF=%lld  MUT_RESIZED=%lld\n",
                probe.nMutations, probe.nMutFreedBuf, probe.nMutResized);
    std::printf("SD via=%s poison=%d  SD_FIRED=%lld  SD_REP_DIED=%lld\n",
                qPrintable(probe.mutVia), probe.poisonBlocks, probe.nSdFired, probe.nSdRepDied);
    std::printf("DELEGATE_DTOR total=%lld inside_walk=%lld   DELEGATE_DETACH total=%lld inside_walk=%lld\n",
                probe.nTripTotal, probe.nTripInWalk, probe.nDetachTotal, probe.nDetachInWalk);
    std::printf("QML_onItemRemoved total=%lld inside_teardown_walk=%lld\n",
                probe.nQmlRemTotal, probe.nQmlRemInWalk);
    std::printf("MODEL_destroyingItem total=%lld inside_teardown_walk=%lld\n",
                probe.nDestroyingTotal, probe.nDestroyingInWalk);
    std::printf("INSIDE_ANY_WALK (live+teardown) delegateDtor=%lld delegateDetach=%lld qmlItemRemoved=%lld\n",
                probe.nTripInAnyWalk, probe.nDetachInAnyWalk, probe.nQmlRemInAnyWalk);
    std::printf("regenerate(ItemParentHasChanged)=%lld  modelNull=%lld withItems=%lld incomplete=%lld\n",
                probe.nRegen, probe.nRegenModelNull, probe.nRegenWithItems, probe.nRegenIncomplete);
    for (auto it = probe.regenByWho.constBegin(); it != probe.regenByWho.constEnd(); ++it)
        std::printf("   regen[%s]=%lld\n", qPrintable(it.key()), it.value());
    std::printf("--- CRITICAL (%lld) ---\n", (long long)probe.critical.size());
    for (const QString &a : probe.critical)
        std::printf("  !! %s\n", qPrintable(a));
    int n = 0;
    for (const QString &a : probe.anomalies) {
        std::printf("  ! %s\n", qPrintable(a));
        if (++n >= 25) { std::printf("  ! ... (%lld more)\n", (long long)(probe.anomalies.size() - n)); break; }
    }
    std::fflush(stdout);
    return rc;
}

#include "main.moc"
