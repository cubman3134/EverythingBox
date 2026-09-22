#include "BuildbotInstall.h"
#include "CustomCoreInstall.h"
#include "../libretro/CoreInspect.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <cstring>

#include "miniz.h"

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <cerrno>
  #include <cstdio>
#endif

namespace {

QString tr(const char* s) { return QCoreApplication::translate("BuildbotInstall", s); }

// The largest single file the extractor will write. A core is tens of MiB; an entry claiming more than this
// uncompressed is not a core, and inflating it would only fill the disk.
constexpr quint64 kMaxCoreBytes = 512ULL * 1024 * 1024;

// The basename of a zip member, splitting on BOTH separators: a zip made on Windows may carry '\', and on POSIX
// QFileInfo would read that as part of the name.
QString memberBasename(const QString& member)
{
    const int cut = qMax(member.lastIndexOf(QLatin1Char('/')), member.lastIndexOf(QLatin1Char('\\')));
    return cut < 0 ? member : member.mid(cut + 1);
}

bool sameFile(const QString& a, const QString& b)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

// One rename that replaces the destination, so the destination is never absent and never half-written: at every
// instant it is either the whole old file or the whole new one. Same volume by construction (the staging
// directory is inside the destination's).
bool replaceFile(const QString& from, const QString& to, QString* why)
{
#ifdef _WIN32
    const std::wstring src = QDir::toNativeSeparators(from).toStdWString();
    const std::wstring dst = QDir::toNativeSeparators(to).toStdWString();
    if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    if (why)
    {
        const DWORD e = GetLastError();
        *why = (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION)
                   ? tr("the installed copy is in use — close the game running it and try again")
                   : tr("Windows error %1").arg(static_cast<qulonglong>(e));
    }
    return false;
#else
    if (std::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0)
        return true;
    if (why) *why = QString::fromLocal8Bit(std::strerror(errno));
    return false;
#endif
}

} // namespace

// ---- policy ----------------------------------------------------------------------------------------------

BuildbotInstall::FetchPolicy BuildbotInstall::productionPolicy()
{
    FetchPolicy p;
    p.base  = BuildbotIndex::productionBase();
    p.allow = [](const QUrl& u) { return BuildbotIndex::isAllowedUrl(u); };
    return p;
}

BuildbotInstall::FetchPolicy BuildbotInstall::policyFor(bool uitest, const QString& overrideBase)
{
    if (!uitest || overrideBase.trimmed().isEmpty()) return productionPolicy();

    QUrl base(overrideBase.trimmed());
    const QString scheme = base.scheme();
    const QString h = base.host();
    const bool loopback = h == QLatin1String("localhost") || QHostAddress(h).isLoopback();
    if (!base.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")) || !loopback
        || !base.userInfo().isEmpty())
        return productionPolicy();            // an override that is not a loopback stub is no override at all
    if (!base.path().endsWith(QLatin1Char('/'))) base.setPath(base.path() + QLatin1Char('/'));

    FetchPolicy p;
    p.base = base;
    const int port = base.port(-1);
    p.allow = [scheme, h, port](const QUrl& u) {
        return u.isValid() && u.scheme() == scheme && u.host() == h && u.port(-1) == port && u.userInfo().isEmpty();
    };
    return p;
}

BuildbotInstall::FetchPolicy BuildbotInstall::effectivePolicy()
{
    return policyFor(qEnvironmentVariableIsSet("EB_UITEST"),
                     qEnvironmentVariable("EB_UITEST_BUILDBOT_BASE"));
}

// ---- extraction ------------------------------------------------------------------------------------------

bool BuildbotInstall::extractCoreFromZip(const QByteArray& zipData, const QString& destDir, const QString& ext,
                                         const QString& expectedFile, QString* outPath)
{
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zipData.constData(), static_cast<size_t>(zipData.size()), 0))
        return false;

    bool ok = false;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            continue;
        const QString name = QString::fromUtf8(st.m_filename);
        if (expectedFile.isEmpty())
        {
            // THE CATALOGUE PATH, unchanged from CoreManager: the first library, written under its basename.
            if (name.endsWith(ext, Qt::CaseInsensitive))
            {
                const QString out = destDir + QStringLiteral("/") + QFileInfo(name).fileName();
                if (mz_zip_reader_extract_to_file(&zip, i, out.toUtf8().constData(), 0))
                {
                    if (outPath) *outPath = out;
                    ok = true;
                }
                break;
            }
            continue;
        }

        // THE BROWSER PATH: only the one file the index entry names. The archive's directory structure is
        // ignored (basename only), the written name is OURS rather than the archive's, a directory entry is
        // never a match, and an entry claiming an absurd size is refused before inflating a byte of it.
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        if (!sameFile(memberBasename(name), expectedFile)) continue;
        if (st.m_uncomp_size > kMaxCoreBytes) break;
        const QString out = destDir + QStringLiteral("/") + expectedFile;
        QFile::remove(out);                                 // a stale staged copy from an interrupted attempt
        if (mz_zip_reader_extract_to_file(&zip, i, out.toUtf8().constData(), 0))   // miniz opens UTF-8 paths
        {
            if (outPath) *outPath = out;
            ok = true;
        }
        else
            QFile::remove(out);
        break;
    }
    mz_zip_reader_end(&zip);
    return ok;
}

// ---- the fetcher -----------------------------------------------------------------------------------------

namespace {

class Fetcher : public QObject
{
public:
    Fetcher(const BuildbotInstall::FetchPolicy& policy, const QUrl& url, qint64 maxBytes, QObject* context,
            std::function<void(int)> onProgress, std::function<void(const QByteArray&, const QString&)> onDone)
        : QObject(context), policy_(policy), maxBytes_(maxBytes),
          onProgress_(std::move(onProgress)), onDone_(std::move(onDone))
    {
        // THE HOST RULE, before a single byte leaves the machine.
        if (!policy_.allow || !policy_.allow(url))
        {
            const QString why = tr("EverythingBox only downloads cores from https://%1, not from %2.")
                                    .arg(BuildbotIndex::host(), url.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery));
            QMetaObject::invokeMethod(this, [this, why] { finish(QByteArray(), why); }, Qt::QueuedConnection);
            return;
        }

        nam_ = new QNetworkAccessManager(this);
        nam_->setTransferTimeout(60000);       // a stalled connection fails instead of hanging the page
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("EverythingBoxNative"));
        // Every redirect is put to the same predicate: a buildbot answer that bounced us to another host (or
        // down to http) would otherwise be followed silently by Qt's default policy.
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
        QNetworkReply* reply = nam_->get(req);
        reply_ = reply;

        connect(reply, &QNetworkReply::redirected, this, [this, reply](const QUrl& to) {
            if (policy_.allow(to)) { emit reply->redirectAllowed(); return; }
            refusedRedirect_ = to.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery);
            reply->abort();
        });
        connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 r, qint64 t) {
            if (r > maxBytes_ || t > maxBytes_) { tooLarge_ = true; reply->abort(); return; }
            if (t <= 0 || !onProgress_) return;
            const int pct = static_cast<int>(r * 100 / t);
            if (pct != lastPct_) { lastPct_ = pct; onProgress_(pct); }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            QString error;
            QByteArray bytes;
            const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (!refusedRedirect_.isEmpty())
                error = tr("The download was redirected to %1, which EverythingBox doesn't download cores from.")
                            .arg(refusedRedirect_);
            else if (tooLarge_)
                error = tr("The download was larger than %1 MiB, so it was stopped.").arg(maxBytes_ / (1024 * 1024));
            else if (reply->error() != QNetworkReply::NoError)
                error = reply->errorString();
            else if (http != 0 && (http < 200 || http > 299))
                error = tr("The server answered HTTP %1.").arg(http);
            else
            {
                bytes = reply->readAll();
                if (bytes.size() > maxBytes_) { bytes.clear(); error = tr("The download was too large."); }
            }
            reply->deleteLater();
            finish(bytes, error);
        });
    }

private:
    void finish(const QByteArray& bytes, const QString& error)
    {
        if (done_) return;
        done_ = true;
        if (onDone_) onDone_(bytes, error);
        deleteLater();
    }

    BuildbotInstall::FetchPolicy policy_;
    qint64 maxBytes_;
    std::function<void(int)> onProgress_;
    std::function<void(const QByteArray&, const QString&)> onDone_;
    QNetworkAccessManager* nam_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QString refusedRedirect_;
    bool tooLarge_ = false;
    bool done_ = false;
    int lastPct_ = -1;
};

} // namespace

void BuildbotInstall::fetch(const FetchPolicy& policy, const QUrl& url, qint64 maxBytes, QObject* context,
                            const std::function<void(int)>& onProgress,
                            const std::function<void(const QByteArray&, const QString&)>& onDone)
{
    new Fetcher(policy, url, maxBytes, context, onProgress, onDone);   // deletes itself when it settles
}

void BuildbotInstall::fetchIndex(const FetchPolicy& policy, QObject* context,
                                 const std::function<void(const BuildbotIndex::Parsed&, const QString&)>& onDone)
{
    fetch(policy, BuildbotIndex::indexUrl(policy.base), BuildbotIndex::kMaxIndexBytes, context, {},
          [onDone](const QByteArray& bytes, const QString& error) {
              if (!error.isEmpty())
              {
                  if (onDone) onDone(BuildbotIndex::Parsed(), tr("Couldn't read the core list: %1").arg(error));
                  return;
              }
              const BuildbotIndex::Parsed parsed = BuildbotIndex::parse(bytes);
              if (onDone)
                  onDone(parsed, parsed.tooLarge ? tr("The core list was larger than %1 MiB, so it wasn't read.")
                                                       .arg(BuildbotIndex::kMaxIndexBytes / (1024 * 1024))
                                                 : QString());
          });
}

// ---- install / update ------------------------------------------------------------------------------------

QString BuildbotInstall::stagingDir()
{
    const QString d = CustomCores::customDir() + QStringLiteral("/.staging");
    QDir().mkpath(d);
    return d;
}

bool BuildbotInstall::installFromZip(const QByteArray& zipData, const BuildbotIndex::Entry& entry, CustomCore* out,
                                     QString* error)
{
    const QString dest = CustomCores::customDir() + QStringLiteral("/") + entry.coreFile;
    const bool replacing = QFileInfo::exists(dest);
    const QString kept = replacing ? tr(" The installed copy was kept.") : QString();

    // Defence in depth: the parser only produces entries that passed the name rule, but this is the function
    // that turns a name into a path, so it asks again rather than trusting its caller.
    if (!BuildbotIndex::isValidCoreName(entry.name) || entry.coreFile != BuildbotIndex::coreFileName(entry.name))
    {
        if (error) *error = tr("‘%1’ isn't a core name EverythingBox will install.").arg(entry.name);
        return false;
    }

    // 3. Only the expected file, into staging.
    QString staged;
    if (!BuildbotInstall::extractCoreFromZip(zipData, stagingDir(), BuildbotIndex::libExt(), entry.coreFile, &staged))
    {
        if (error) *error = tr("The download for %1 didn't contain %2.").arg(entry.name, entry.coreFile) + kept;
        return false;
    }

    // 4. Inspect BEFORE anything is registered or replaced. A file that is not a core goes, with the reason.
    CoreInspection info;
    QString why;
    if (!CoreInspect::inspect(staged, &info, &why))
    {
        QFile::remove(staged);
        if (error) *error = tr("%1 from the buildbot couldn't be loaded as a core, so it was deleted. %2")
                                .arg(entry.coreFile, why) + kept;
        return false;
    }
    const QString id = CustomCoreInstall::idFor(info.libraryName, entry.coreFile);
    if (id.isEmpty())
    {
        QFile::remove(staged);
        if (error) *error = tr("%1 doesn't report a name, so it was deleted.").arg(entry.coreFile) + kept;
        return false;
    }

    // 5. The swap: one replace-existing rename. Until it succeeds the old core is the installed one.
    if (!replaceFile(staged, dest, &why))
    {
        QFile::remove(staged);
        if (error) *error = tr("Couldn't put %1 in place: %2.").arg(entry.coreFile, why) + kept;
        return false;
    }

    // 6. Register, with where it came from.
    CustomCore rec = CustomCoreInstall::recordFrom(info, id, dest);
    rec.source     = CustomCores::sourceBuildbot();
    rec.sourceFile = entry.file;
    rec.sourceDate = entry.date.toString(QStringLiteral("yyyy-MM-dd"));
    // A core that renamed itself upstream (a different library_name, so a different id) is still THIS file: drop
    // the stale record so one file is never two registrations.
    const QList<CustomCore> before = CustomCores::all();
    for (const CustomCore& c : before)
        if (c.id != id && c.source == CustomCores::sourceBuildbot() && c.sourceFile == entry.file)
            CustomCores::remove(c.id);
    if (!CustomCores::add(rec, error))
        return false;
    if (out) *out = rec;
    return true;
}

void BuildbotInstall::install(const FetchPolicy& policy, const BuildbotIndex::Entry& entry, QObject* context,
                              const std::function<void(int)>& onProgress,
                              const std::function<void(bool, const CustomCore&, const QString&)>& onDone)
{
    fetch(policy, BuildbotIndex::zipUrl(policy.base, entry), kMaxZipBytes, context, onProgress,
          [entry, onDone](const QByteArray& bytes, const QString& error) {
              CustomCore rec;
              if (!error.isEmpty())
              {
                  const bool replacing = QFileInfo::exists(CustomCores::customDir() + QStringLiteral("/") + entry.coreFile);
                  if (onDone) onDone(false, rec, tr("Couldn't download %1: %2").arg(entry.name, error)
                                                   + (replacing ? tr(" The installed copy was kept.") : QString()));
                  return;
              }
              QString why;
              const bool ok = installFromZip(bytes, entry, &rec, &why);
              if (onDone) onDone(ok, rec, why);
          });
}
