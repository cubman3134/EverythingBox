#include "PerfTrace.h"
#include "AppPaths.h"
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace
{
    bool g_forced = false;
    QString g_forcedPath;
    QMutex g_mutex;
    QHash<QString, QElapsedTimer> g_open; // begin()ed spans awaiting end()

    bool computeEnabled() { return g_forced || qEnvironmentVariableIntValue("MMV_PERF") == 1; }
}

namespace PerfTrace
{
    bool enabled()
    {
        static bool cached = computeEnabled();   // cached once; forceEnableForTest resets below
        return g_forced || cached;
    }

    QString logPath()
    {
        return g_forced ? g_forcedPath : AppPaths::dataDir() + QStringLiteral("/perf_trace.log");
    }

    void forceEnableForTest(const QString& logFile) { g_forced = true; g_forcedPath = logFile; }

    void write(const QString& span, qint64 ms, const QString& detail)
    {
        if (!enabled()) return;
        QMutexLocker lock(&g_mutex);
        QFile f(logPath());
        if (!f.open(QIODevice::Append | QIODevice::Text)) return;
        QString line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                     + QStringLiteral(" | ") + span + QStringLiteral(" | ") + QString::number(ms);
        if (!detail.isEmpty()) line += QStringLiteral(" | ") + detail;
        f.write((line + QStringLiteral("\n")).toUtf8());
    }

    void begin(const QString& span)
    {
        if (!enabled()) return;
        QMutexLocker lock(&g_mutex);
        g_open[span].start();                    // insert-or-overwrite restarts the clock
    }

    void end(const QString& span, const QString& detail)
    {
        if (!enabled()) return;
        qint64 ms = -1;
        {
            QMutexLocker lock(&g_mutex);
            const auto it = g_open.find(span);
            if (it == g_open.end()) return;      // orphan end: silent no-op
            ms = it->elapsed();
            g_open.erase(it);
        }
        write(span, ms, detail);
    }

    Scope::Scope(const QString& name) : name_(name), on_(enabled()) { if (on_) t_.start(); }
    void Scope::setDetail(const QString& d) { if (on_) detail_ = d; }
    Scope::~Scope() { if (on_) write(name_, t_.elapsed(), detail_); }
}
