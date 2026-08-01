#include "RegistryBrowser.h"
#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/ThemeRegistry.h"
#include "../addons/AddonManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QFrame>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QSettings>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QPair>
#include <QUrl>
#include <QVector>

// The directory ThemeEngine::availableThemes() scans, from the one function that defines it. Asking
// ThemeRegistry rather than ThemeEngine is what makes this unconditional: ThemeRegistry is QtCore-only and
// always compiled, while ThemeEngine.cpp is built only when EB_HAVE_QML is set — and this dialog is in the
// unconditional app sources, so a Qt install without qtdeclarative would fail to LINK against the engine.
static QString themesRoot() { return ThemeRegistry::themesRoot(AppPaths::dataDir()); }

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

static QString extrasKey(RegistryBrowser::Kind kind)
{
    return kind == RegistryBrowser::Themes ? QStringLiteral("registry/themesExtras")
                                           : QStringLiteral("registry/addonsExtras");
}

QString RegistryBrowser::defaultUrl() const
{
    return kind_ == Themes
        ? QStringLiteral("https://raw.githubusercontent.com/cubman3134/everythingbox-themes/main/index.json")
        : QStringLiteral("https://raw.githubusercontent.com/cubman3134/everythingbox-addons/main/index.json");
}

QStringList RegistryBrowser::extraRegistries() const { return store().value(extrasKey(kind_)).toStringList(); }
void RegistryBrowser::saveExtras(const QStringList& list) { store().setValue(extrasKey(kind_), list); store().sync(); }

QStringList RegistryBrowser::allRegistries() const
{
    QStringList l;
    l << defaultUrl();
    for (const QString& u : extraRegistries())
        if (!u.trimmed().isEmpty() && !l.contains(u)) l << u.trimmed();
    return l;
}

QString RegistryBrowser::baseUrl(const QString& indexUrl)
{
    const int slash = indexUrl.lastIndexOf(QLatin1Char('/'));
    return slash > 0 ? indexUrl.left(slash) : indexUrl;
}

// Turn a raw index URL (raw.githubusercontent.com/<user>/<repo>/<branch>/index.json) into "<user>/<repo>".
static QString repoOf(const QString& rawUrl)
{
    const QUrl u(rawUrl);
    if (u.host().contains(QStringLiteral("raw.githubusercontent.com")))
    {
        const QStringList p = u.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (p.size() >= 2) return p[0] + QStringLiteral("/") + p[1];
    }
    return u.host();
}

RegistryBrowser::RegistryBrowser(Kind kind, AddonManager* addons, QWidget* parent)
    : QDialog(parent), kind_(kind), addons_(addons)
{
    setWindowTitle(kind_ == Themes ? tr("Browse Themes") : tr("Browse Add-ons"));
    resize(580, 560);
    nam_ = new QNetworkAccessManager(this);

    auto* v = new QVBoxLayout(this);

    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel(tr("Registries"), this));
    top->addStretch(1);
    auto* add = new QPushButton(tr("Add registry…"), this);
    auto* reload = new QPushButton(tr("Reload"), this);
    top->addWidget(add);
    top->addWidget(reload);
    v->addLayout(top);

    // Inline "add registry" row, revealed by the Add button (instead of a popup input dialog).
    auto* addRow = new QWidget(this);
    addRow->setVisible(false);
    auto* addH = new QHBoxLayout(addRow);
    addH->setContentsMargins(0, 0, 0, 0);
    auto* urlEdit = new QLineEdit(addRow);
    urlEdit->setPlaceholderText(tr("Registry index URL (raw GitHub)"));
    auto* addOk = new QPushButton(tr("Add"), addRow);
    auto* addCancel = new QPushButton(tr("Cancel"), addRow);
    addH->addWidget(urlEdit, 1);
    addH->addWidget(addOk);
    addH->addWidget(addCancel);
    v->addWidget(addRow);

    registriesLayout_ = new QVBoxLayout();
    v->addLayout(registriesLayout_);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto* host = new QWidget(scroll);
    listLayout_ = new QVBoxLayout(host);
    listLayout_->setAlignment(Qt::AlignTop);
    scroll->setWidget(host);
    // Hosted inline (MainWindow::showDialogPanel), this dialog is laid out at its sizeHint — and the card
    // area's sizeHint comes from a host widget that is still EMPTY at that moment, because the registry
    // fetch is asynchronous. Without a floor the list renders about one card tall under 500 px of dead
    // panel, and the outer layout never re-runs when the cards land. resize() above cannot supply it: a
    // layout consults sizeHint/minimumSizeHint and never a prior resize. Height only, so a narrow window
    // is not forced to scroll sideways; a short one still degrades gracefully because the panel scrolls.
    scroll->setMinimumHeight(360);
    v->addWidget(scroll, 1);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    v->addWidget(status_);

    auto* bottom = new QHBoxLayout();
    repoLink_ = new QLabel(this);
    repoLink_->setTextFormat(Qt::RichText);
    repoLink_->setOpenExternalLinks(true);
    bottom->addWidget(repoLink_);
    bottom->addStretch(1);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    // Not accept() directly: Close is live while an install's nested loops are running, and the host's
    // finished handler navigates away — which deletes this dialog. Queued or not, that lands in whichever
    // loop is spinning, including a nested one. closeWhenIdle waits for the stack to unwind.
    connect(box, &QDialogButtonBox::rejected, this, &RegistryBrowser::closeWhenIdle);
    bottom->addWidget(box);
    v->addLayout(bottom);

    connect(reload, &QPushButton::clicked, this, [this] { fetchAll(); });
    connect(add, &QPushButton::clicked, this, [addRow, urlEdit] {
        addRow->setVisible(true); urlEdit->clear(); urlEdit->setFocus();
    });
    auto commitAdd = [this, addRow, urlEdit] {
        const QString url = urlEdit->text().trimmed();
        if (url.isEmpty()) { addRow->setVisible(false); return; }
        QStringList extras = extraRegistries();
        if (!extras.contains(url)) { extras << url; saveExtras(extras); }
        addRow->setVisible(false);
        renderRegistryRows();
        fetchAll();
    };
    connect(addOk, &QPushButton::clicked, this, commitAdd);
    connect(urlEdit, &QLineEdit::returnPressed, this, commitAdd);
    connect(addCancel, &QPushButton::clicked, this, [addRow] { addRow->setVisible(false); });

    renderRegistryRows();
    updateRepoLink();
    fetchAll();
}

void RegistryBrowser::renderRegistryRows()
{
    while (QLayoutItem* it = registriesLayout_->takeAt(0)) { delete it->widget(); delete it; }
    const QStringList all = allRegistries();
    for (int i = 0; i < all.size(); ++i)
    {
        auto* row = new QHBoxLayout();
        const bool isDefault = (i == 0);
        auto* lbl = new QLabel(QStringLiteral("%1%2").arg(isDefault ? tr("(default) ") : QString(),
                                                          repoOf(all[i])), this);
        lbl->setToolTip(all[i]);
        lbl->setStyleSheet(QStringLiteral("color:#555;"));
        row->addWidget(lbl, 1);
        if (!isDefault)
        {
            auto* rm = new QPushButton(tr("✕"), this);
            rm->setFixedWidth(28);
            rm->setToolTip(tr("Remove this registry"));
            const QString url = all[i];
            connect(rm, &QPushButton::clicked, this, [this, url] {
                QStringList extras = extraRegistries();
                extras.removeAll(url);
                saveExtras(extras);
                renderRegistryRows();
                fetchAll();
            });
            row->addWidget(rm);
        }
        registriesLayout_->addLayout(row);
    }
}

QString RegistryBrowser::localDirFor(const QString& id) const
{
    // Themes install as FOLDERS under the themes2 root ThemeEngine::availableThemes() scans. The old
    // <dataDir>/themes was the legacy flat colour-theme directory and nothing reads it.
    if (kind_ == Themes) return themesRoot() + QStringLiteral("/") + id;
    return AppPaths::dataDir() + QStringLiteral("/addons/") + id;
}

// A registry entry with a "url" is a remote (HTTP) addon: installing it just subscribes to the URL.
static bool isRemoteEntry(const QJsonObject& e) { return !e.value(QStringLiteral("url")).toString().isEmpty(); }
static QString normalizeRemoteUrl(QString u)
{
    u = u.trimmed();
    if (u.endsWith(QStringLiteral("/manifest.json"))) u.chop(int(qstrlen("/manifest.json")));
    while (u.endsWith(QLatin1Char('/'))) u.chop(1);
    return u;
}

// A folder already on disk is installed — the same predicate ThemeEngine::availableThemes() uses, so the
// gallery cannot claim something is installed that the picker will not list. This is also what keeps the
// bundled Channels/Night/Triple from being replaced by the registry's older copies of them (issue #131):
// they exist, so they are never offered, and there is deliberately no overwrite path.
bool RegistryBrowser::isThemeInstalled(const ThemeRegistry::Entry& entry) const
{
    const QString folder = entry.folder();   // parseIndex guarantees this, but this is the path predicate
    return !folder.isEmpty() && QFile::exists(localDirFor(folder) + QStringLiteral("/theme.json"));
}

// Add-ons only. A theme entry never reaches here: fetchOne parses the themes array through
// ThemeRegistry::parseIndex and renders it through renderThemeEntry, whose predicate is isThemeInstalled.
bool RegistryBrowser::isInstalled(const QJsonObject& entry) const
{
    if (kind_ == Addons && isRemoteEntry(entry)) // remote addon: "installed" = its URL is already in the source list
        return addons_ && addons_->remoteSourceUrls().contains(normalizeRemoteUrl(entry.value(QStringLiteral("url")).toString()));
    const QString id = entry.value(QStringLiteral("id")).toString();
    return !id.isEmpty() && QFile::exists(localDirFor(id) + QStringLiteral("/manifest.json"));
}

void RegistryBrowser::fetchAll()
{
    while (QLayoutItem* it = listLayout_->takeAt(0)) { delete it->widget(); delete it; }
    const QStringList all = allRegistries();
    pending_ = all.size();
    total_ = 0;
    status_->setText(tr("Loading…"));
    for (const QString& url : all) fetchOne(url);
}

void RegistryBrowser::fetchOne(const QString& indexUrl)
{
    QNetworkRequest req((QUrl(indexUrl)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, indexUrl] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
        {
            const QByteArray body = reply->readAll();
            if (kind_ == Themes)
            {
                // ThemeRegistry::parseIndex is the ONE reader of a themes index. It owns which key is
                // authoritative ("themes2" when PRESENT wins outright over the legacy "themes", because an
                // index that empties themes2 is withdrawing its themes rather than falling back) AND which
                // entries are offerable at all (one without a usable `dir` is dropped). Restating either rule
                // here — as the previous version of this line did for the first — is exactly how this gallery
                // ends up listing, and counting in the "%1 available" total, a theme the themed Appearance
                // surface will not show and this dialog will refuse to install when pressed.
                const QVector<ThemeRegistry::Entry> entries = ThemeRegistry::parseIndex(body);
                for (const ThemeRegistry::Entry& e : entries) { renderThemeEntry(e, indexUrl); ++total_; }
            }
            else
            {
                const QJsonObject root = QJsonDocument::fromJson(body).object();
                for (const QJsonValue& e : root.value(QStringLiteral("addons")).toArray())
                    if (e.isObject()) { renderEntry(e.toObject(), indexUrl); ++total_; }
            }
        }
        if (--pending_ <= 0)
            status_->setText(total_ == 0 ? tr("No entries found. Check the registry URLs.")
                                         : tr("%1 available across %2 registr%3.")
                                               .arg(total_).arg(allRegistries().size())
                                               .arg(allRegistries().size() == 1 ? tr("y") : tr("ies")));
    });
}

// The chrome a row is, for either kind. The two kinds no longer share a data shape — an add-on entry is a
// raw QJsonObject, a theme is a parsed ThemeRegistry::Entry — so what they share is spelled as parameters
// rather than as branches on kind_ inside one function reading one JSON object.
void RegistryBrowser::addCard(const QString& name, const QString& author, const QString& description,
                              const QStringList& formFactors, const QString& indexUrl, bool installed,
                              const std::function<void(QPushButton*)>& onInstall)
{
    auto* card = new QFrame();
    card->setFrameShape(QFrame::StyledPanel);
    auto* h = new QHBoxLayout(card);

    auto* texts = new QVBoxLayout();
    auto* title = new QLabel(QStringLiteral("<b>%1</b>%2").arg(name.toHtmlEscaped(),
        author.isEmpty() ? QString()
                         : QStringLiteral("  <span style='color:#888;'>by %1</span>").arg(author.toHtmlEscaped())));
    title->setTextFormat(Qt::RichText);
    texts->addWidget(title);
    auto* desc = new QLabel(description);
    desc->setWordWrap(true);
    desc->setStyleSheet(QStringLiteral("color:#444;"));
    texts->addWidget(desc);

    // Advisory only — a theme declaring a form factor is still installable anywhere, which is how the
    // engine treats it. Shown so the card says what the theme was built for.
    if (!formFactors.isEmpty())
    {
        auto* forLbl = new QLabel(tr("built for %1").arg(formFactors.join(QStringLiteral(", "))));
        forLbl->setStyleSheet(QStringLiteral("color:#999; font-size:11px;"));
        texts->addWidget(forLbl);
    }

    auto* src = new QLabel(tr("from %1").arg(repoOf(indexUrl)));
    src->setStyleSheet(QStringLiteral("color:#999; font-size:11px;"));
    texts->addWidget(src);
    h->addLayout(texts, 1);

    auto* btn = new QPushButton(card);
    btn->setText(installed ? tr("Installed ✓") : tr("Install"));
    btn->setEnabled(!installed);
    connect(btn, &QPushButton::clicked, this, [btn, onInstall] {
        btn->setEnabled(false);
        btn->setText(tr("Installing…"));
        onInstall(btn);
    });
    h->addWidget(btn, 0, Qt::AlignTop);

    listLayout_->addWidget(card);
}

// An ADD-ON row. Reached only from fetchOne's non-Themes branch, so the remote-subscription path below
// cannot be entered by a theme entry that happens to carry a "url" key.
void RegistryBrowser::renderEntry(const QJsonObject& entry, const QString& indexUrl)
{
    QStringList ff;
    for (const QJsonValue& f : entry.value(QStringLiteral("formFactors")).toArray())
        if (!f.toString().isEmpty()) ff << f.toString();

    addCard(entry.value(QStringLiteral("name")).toString(),
            entry.value(QStringLiteral("author")).toString(),
            entry.value(QStringLiteral("description")).toString(),
            ff, indexUrl, isInstalled(entry),
            [this, entry, indexUrl](QPushButton* btn) {
        if (kind_ == Addons && isRemoteEntry(entry) && addons_)
        {
            // Remote addon: subscribe to the URL (async manifest fetch); update the button on the result.
            auto* conn = new QMetaObject::Connection;
            *conn = connect(addons_, &AddonManager::remoteSourceResult, this,
                            [this, btn, conn](bool ok, const QString& msg) {
                status_->setText(msg);
                btn->setText(ok ? tr("Added ✓") : tr("Retry"));
                btn->setEnabled(!ok);
                if (ok) installed_ = true;
                QObject::disconnect(*conn);
                delete conn;
            });
            addons_->addRemoteSource(entry.value(QStringLiteral("url")).toString());
            return;
        }
        installEntry(entry, indexUrl);
        const bool ok = isInstalled(entry);
        btn->setText(ok ? tr("Installed ✓") : tr("Retry"));
        btn->setEnabled(!ok);
    });
}

// A THEME row, from the entry parseIndex kept. The display fields come off the parsed Entry rather than
// being re-read from JSON, so the row and the install act on the same values.
void RegistryBrowser::renderThemeEntry(const ThemeRegistry::Entry& entry, const QString& indexUrl)
{
    addCard(entry.name, entry.author, entry.description, entry.formFactors, indexUrl,
            isThemeInstalled(entry),
            [this, entry, indexUrl](QPushButton* btn) {
        installThemeEntry(entry, indexUrl);
        const bool ok = isThemeInstalled(entry);
        btn->setText(ok ? tr("Installed ✓") : tr("Retry"));
        btn->setEnabled(!ok);
    });
}

bool RegistryBrowser::downloadTo(const QString& url, const QString& destPath, QString* error)
{
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(req);

    QEventLoop loop;
    QTimer to; to.setSingleShot(true);
    connect(&to, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    to.start(20000);
    loop.exec();

    if (!reply->isFinished() || reply->error() != QNetworkReply::NoError)
    {
        if (error) *error = reply->isFinished() ? reply->errorString() : QStringLiteral("timed out");
        reply->abort(); reply->deleteLater();
        return false;
    }
    const QByteArray data = reply->readAll();
    reply->deleteLater();

    QFileInfo fi(destPath);
    QDir().mkpath(fi.absolutePath());
    QFile f(destPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { if (error) *error = tr("can't write %1").arg(destPath); return false; }

    // The write is buffered, so a full disk or a quota can surface only when the file is flushed — and
    // ~QFile flushes and SWALLOWS that error. An unchecked write here is a silently truncated blob that is
    // read straight back and handed to ThemeRegistry::installFiles, which renames it into place and reports
    // success: the atomic install is defeated one layer below where it is enforced. It matters most on the
    // theme path, where isInstalled keys on the folder existing and there is deliberately no overwrite path
    // — the truncated theme would then show "Installed ✓" behind a disabled button, unreinstallable from
    // this dialog. This is the check ThemeRegistry::installFiles already makes on its own writes.
    //
    // close() is void (QFileDevice), so the flush is examined through error() the way installFiles does.
    const bool wrote = f.write(data) == qint64(data.size());
    f.close();
    if (!wrote || f.error() != QFileDevice::NoError)
    {
        if (error) *error = tr("couldn't write all of %1 — the disk may be full").arg(destPath);
        f.remove();   // a partial file left here would be read back as if it were the whole download
        return false;
    }
    return true;
}

void RegistryBrowser::closeWhenIdle()
{
    if (installing_) { closeWhenIdle_ = true; return; }   // finishInstall() will do it
    accept();
}

// Bracket for the two install paths: while one runs, this dialog's own frames are on the stack under a
// nested event loop, so anything that would delete it has to wait. Called on EVERY exit from an install,
// including the early error returns, or a failed install would leave the dialog permanently "busy".
void RegistryBrowser::finishInstall()
{
    installing_ = false;
    if (!closeWhenIdle_) return;
    closeWhenIdle_ = false;
    // Safe now: the nested loops have unwound, and finished() reaches the host queued, so nothing of ours
    // is on the stack when it deletes us.
    accept();
}

void RegistryBrowser::installEntry(const QJsonObject& entry, const QString& indexUrl)
{
    if (installing_) return;   // a second card's Install, clicked from inside the first one's nested loop
    const InstallScope scope(this);

    // A theme is a folder, not a file list, and the flattening loop below would drop its sounds/ and fonts/
    // subdirectories onto one level. Themes have their own path from fetchOne onwards and cannot reach this
    // function; the guard is here so that a future caller which forgets that is refused rather than served.
    if (kind_ == Themes)
    { status_->setText(tr("A theme can't be installed this way. Reopen this window and try again.")); return; }

    const QString base = baseUrl(indexUrl);
    QStringList files;
    QString destDir;

    const QString id = entry.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) { status_->setText(tr("Entry has no id.")); return; }
    destDir = localDirFor(id);
    for (const QJsonValue& fv : entry.value(QStringLiteral("files")).toArray()) files << fv.toString();

    if (files.isEmpty()) { status_->setText(tr("Nothing to download for this entry.")); return; }

    for (const QString& rel : files)
    {
        if (rel.isEmpty()) continue;
        const QString url = base + QStringLiteral("/") + rel;
        const QString dest = destDir + QStringLiteral("/") + QFileInfo(rel).fileName();
        QString err;
        if (!downloadTo(url, dest, &err))
        {
            status_->setText(tr("Download failed: %1\n%2").arg(QFileInfo(rel).fileName(), err));
            return;
        }
    }

    installed_ = true;
    status_->setText(tr("Installed “%1”.").arg(entry.value(QStringLiteral("name")).toString()));
    if (kind_ == Addons && addons_) addons_->reload();
}

// The registry repo's file tree, fetched once per registry per dialog. An entry names a directory, so this
// is how we learn what is in it — from the repository itself, which cannot drift from what it holds.
QByteArray RegistryBrowser::treeFor(const QString& indexUrl, QString* error)
{
    if (treeCache_.contains(indexUrl)) return treeCache_.value(indexUrl);

    const QString api = ThemeRegistry::treeApiUrl(indexUrl);
    if (api.isEmpty())
    {
        // A user-added registry may be anywhere. It still LISTS — the user can read what the theme is and
        // go install it by hand — but we cannot enumerate a folder without the API.
        if (error) *error = tr("This registry isn't hosted on GitHub, so themes can't be installed from "
                               "here. Download the folder from the registry and drop it into %1.")
                                .arg(QDir::toNativeSeparators(themesRoot()));
        return QByteArray();
    }

    // Reuse downloadTo rather than opening a second blocking-fetch event loop: it already carries the user
    // agent, the redirect policy and the 20 s cap, and a second copy of that would drift from it.
    const QString tmp = QDir::tempPath() + QStringLiteral("/eb-theme-tree.tmp");
    QString err;
    if (!downloadTo(api, tmp, &err))
    {
        // Rate limiting lands here too (GitHub allows 60 unauthenticated calls an hour per IP).
        if (error) *error = tr("Couldn't read the registry's file list: %1").arg(err);
        QFile::remove(tmp);
        return QByteArray();
    }
    QByteArray body;
    { QFile f(tmp); if (f.open(QIODevice::ReadOnly)) body = f.readAll(); }
    QFile::remove(tmp);
    if (body.isEmpty())
    { if (error) *error = tr("The registry's file list came back empty."); return QByteArray(); }

    treeCache_.insert(indexUrl, body);
    return body;
}

// Install one themes2 entry: list the folder from the repo tree, download every file, then hand the whole
// set to ThemeRegistry::installFiles, which writes it atomically. Nothing touches themes2/<Name> until
// every byte is in hand — a half-installed theme would still be offered by the picker.
void RegistryBrowser::installThemeEntry(const ThemeRegistry::Entry& e, const QString& indexUrl)
{
    if (installing_) return;   // a second card's Install, clicked from inside the first one's nested loop
    const InstallScope scope(this);

    const QString folder = e.folder();
    if (folder.isEmpty()) { status_->setText(tr("This entry doesn't name a usable theme folder.")); return; }

    status_->setText(tr("Reading the registry's file list…"));
    QString err;
    const QByteArray tree = treeFor(indexUrl, &err);
    if (tree.isEmpty()) { status_->setText(err); return; }

    const ThemeRegistry::Listing listing = ThemeRegistry::filesUnder(tree, e.dir);
    if (!listing.ok()) { status_->setText(listing.error); return; }

    const QString base = baseUrl(indexUrl);
    QVector<QPair<QString, QByteArray>> blobs;
    for (int i = 0; i < listing.files.size(); ++i)
    {
        const QString& rel = listing.files.at(i);
        // Named per file. This loop is up to kMaxFiles sequential fetches, each behind its own 20 s wall, on
        // the UI thread — a single unchanging "Installing…" across all of them is indistinguishable from a
        // hang. The label does repaint: downloadTo enters a nested event loop on the very next line.
        // The multi-arg form substitutes in ONE pass, so a file whose name contains "%2" is not treated as a
        // placeholder for the count that follows it.
        status_->setText(tr("Downloading %1 (%2 of %3)…")
                             .arg(rel, QString::number(i + 1), QString::number(listing.files.size())));

        // `rel` travels to installFiles as the string filesUnder validated. assetUrl percent-encodes for the
        // URL side only; nothing here re-parses, re-joins or decodes it, or a file legitimately named
        // "%2e%2e" would become ".." on the way to disk.
        const QString url = ThemeRegistry::assetUrl(base, e.dir, rel);
        const QString tmp = QDir::tempPath() + QStringLiteral("/eb-theme-dl.tmp");
        QString derr;
        if (!downloadTo(url, tmp, &derr))
        { status_->setText(tr("Download failed: %1\n%2").arg(rel, derr)); QFile::remove(tmp); return; }
        QFile f(tmp);
        if (!f.open(QIODevice::ReadOnly))
        { status_->setText(tr("Download failed: %1").arg(rel)); QFile::remove(tmp); return; }
        blobs << qMakePair(rel, f.readAll());
        f.close();
        QFile::remove(tmp);
    }

    status_->setText(tr("Writing the theme folder…"));
    if (!ThemeRegistry::installFiles(themesRoot(), folder, blobs, &err))
    { status_->setText(err); return; }

    installed_ = true;
    // The folder stands in for a nameless entry: an index that omits "name" would otherwise say Installed “”.
    status_->setText(tr("Installed “%1”. Pick it from the theme list.")
                         .arg(e.name.isEmpty() ? folder : e.name));
}

void RegistryBrowser::updateRepoLink()
{
    const QString page = QStringLiteral("https://github.com/") + repoOf(defaultUrl());
    const QString label = kind_ == Themes ? tr("↗ Browse / contribute themes on GitHub")
                                          : tr("↗ Browse / contribute add-ons on GitHub");
    repoLink_->setText(QStringLiteral("<a href=\"%1\">%2</a>").arg(page.toHtmlEscaped(), label));
}
