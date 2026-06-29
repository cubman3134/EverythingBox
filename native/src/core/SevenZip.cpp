#include "SevenZip.h"

#include <QFileInfo>
#include <mutex>
#include <vector>

// The LZMA SDK headers are C; include them as such. The include dir (third_party/lzma) is on the
// target's include path (see CMakeLists).
extern "C" {
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
}

namespace {

constexpr size_t kInputBufSize = (size_t)1 << 18; // 256 KiB look-ahead buffer (matches the SDK sample)

// SzAlloc/SzFree are the SDK's malloc/free wrappers; one allocator serves both main and temp use.
const ISzAlloc kAlloc = { SzAlloc, SzFree };

// CrcGenerateTable() must run once before any CRC use; guard it so repeated extractions are safe.
void ensureCrcTable()
{
    static std::once_flag once;
    std::call_once(once, [] { CrcGenerateTable(); });
}

// The archive name may carry a path with either separator; we only want the final component.
QString baseName(const QString& name)
{
    const int slash = qMax(name.lastIndexOf(QLatin1Char('/')), name.lastIndexOf(QLatin1Char('\\')));
    return slash >= 0 ? name.mid(slash + 1) : name;
}

bool matchesAnyExt(const QString& name, const QStringList& exts)
{
    for (const QString& e : exts)
        if (name.endsWith(e, Qt::CaseInsensitive))
            return true;
    return false;
}

} // namespace

bool SevenZip::extractBest(const QString& sevenZipPath, const QStringList& wantedExts,
                           QString& memberName, QByteArray& data, QString* error)
{
    ensureCrcTable();

    ISzAlloc allocImp = kAlloc;
    ISzAlloc allocTempImp = kAlloc;

    // Open the file first (InFile_Open initialises the handle itself), then build the stream vtables —
    // the exact order the SDK's own 7z sample uses.
    CFileInStream archiveStream;
#ifdef _WIN32
    if (InFile_OpenW(&archiveStream.file, reinterpret_cast<const WCHAR*>(sevenZipPath.utf16())) != 0)
#else
    if (InFile_Open(&archiveStream.file, sevenZipPath.toUtf8().constData()) != 0)
#endif
    {
        if (error) *error = QStringLiteral("could not open archive");
        return false;
    }

    FileInStream_CreateVTable(&archiveStream);
    archiveStream.wres = 0;

    CLookToRead2 lookStream;
    LookToRead2_CreateVTable(&lookStream, False);
    lookStream.buf = static_cast<Byte*>(ISzAlloc_Alloc(&allocImp, kInputBufSize));
    if (!lookStream.buf)
    {
        File_Close(&archiveStream.file);
        if (error) *error = QStringLiteral("out of memory");
        return false;
    }
    lookStream.bufSize = kInputBufSize;
    lookStream.realStream = &archiveStream.vt;
    LookToRead2_INIT(&lookStream)

    bool ok = false;
    CSzArEx db;
    SzArEx_Init(&db);

    if (SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp) == SZ_OK)
    {
        // Choose the member to extract: prefer one whose name matches a wanted extension; otherwise the
        // largest regular file (a No-Intro .7z holds a single ROM, so "largest" trivially picks it).
        int bestIdx = -1;
        UInt64 bestSize = 0;
        bool bestExt = false;
        QString bestName;
        std::vector<UInt16> nameBuf;

        for (UInt32 i = 0; i < db.NumFiles; ++i)
        {
            if (SzArEx_IsDir(&db, i))
                continue;

            const size_t len = SzArEx_GetFileNameUtf16(&db, i, nullptr); // includes the trailing NUL
            nameBuf.resize(len);
            SzArEx_GetFileNameUtf16(&db, i, nameBuf.data());
            const QString name = QString::fromUtf16(
                reinterpret_cast<const char16_t*>(nameBuf.data()), int(len > 0 ? len - 1 : 0));
            const UInt64 size = SzArEx_GetFileSize(&db, i);
            const bool extMatch = !wantedExts.isEmpty() && matchesAnyExt(name, wantedExts);

            // An extension match always beats a non-match; within the same match class, larger wins.
            const bool better = (extMatch && !bestExt) || (extMatch == bestExt && size > bestSize);
            if (bestIdx < 0 || better)
            {
                bestIdx = int(i);
                bestSize = size;
                bestExt = extMatch;
                bestName = name;
            }
        }

        if (bestIdx >= 0)
        {
            UInt32 blockIndex = 0xFFFFFFFF; // "no cached block yet" — the SDK fills/uses it across calls
            Byte* outBuffer = nullptr;
            size_t outBufferSize = 0, offset = 0, outSizeProcessed = 0;

            if (SzArEx_Extract(&db, &lookStream.vt, static_cast<UInt32>(bestIdx), &blockIndex,
                               &outBuffer, &outBufferSize, &offset, &outSizeProcessed,
                               &allocImp, &allocTempImp) == SZ_OK)
            {
                data = QByteArray(reinterpret_cast<const char*>(outBuffer + offset),
                                  static_cast<qsizetype>(outSizeProcessed));
                memberName = baseName(bestName);
                ok = true;
            }
            else if (error)
                *error = QStringLiteral("failed to decompress the ROM inside the archive");

            if (outBuffer)
                ISzAlloc_Free(&allocImp, outBuffer);
        }
        else if (error)
            *error = QStringLiteral("the archive contains no file to open");
    }
    else if (error)
        *error = QStringLiteral("not a valid .7z archive");

    SzArEx_Free(&db, &allocImp);
    ISzAlloc_Free(&allocImp, lookStream.buf);
    File_Close(&archiveStream.file);
    return ok;
}
