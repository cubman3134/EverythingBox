#pragma once
#include <QString>
#include <QElapsedTimer>

// Lightweight span tracing for the phase-2 perf track. Enabled by MMV_PERF=1 (or
// forceEnableForTest in probes); when disabled every call is one cached-bool branch.
// Lines land in <dataDir>/perf_trace.log as:  ISO-ts | span.name | duration_ms | detail
// Semantics: begin() on an open span RESTARTS it; end() without a begin is a no-op —
// so instrumentation sites never have to prove a matching pair fired.
namespace PerfTrace
{
    bool enabled();
    void write(const QString& span, qint64 ms, const QString& detail = QString());
    void begin(const QString& span);
    void end(const QString& span, const QString& detail = QString());
    QString logPath();
    void forceEnableForTest(const QString& logFile); // probes: enable + redirect output

    class Scope
    {
    public:
        explicit Scope(const QString& name);
        void setDetail(const QString& d);
        ~Scope();
    private:
        QString name_, detail_;
        QElapsedTimer t_;
        bool on_ = false;
    };
}

#define PERF_CAT2(a, b) a##b
#define PERF_CAT(a, b) PERF_CAT2(a, b)
#define PERF_SPAN(name) PerfTrace::Scope PERF_CAT(perfScope_, __LINE__)(QStringLiteral(name))
