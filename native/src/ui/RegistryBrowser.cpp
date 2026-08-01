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

// Bound a reply's body AS IT ARRIVES. QNetworkAccessManager buffers a whole response by default — no
// setReadBufferSize, no MaximumDownloadBufferSizeAttribute, no readyRead handling anywhere here — so a cap
// tested after readAll() is a cap on nothing: a registry serving ONE 500 MB file has 500 MB resident before
// anything gets to refuse it, which on the 32-bit armv7 box is exactly the OOM ThemeRegistry's caps were
// written to prevent. Two mechanisms, because neither does the whole job:
//
//   setReadBufferSize  caps what the reply will hold un-read; QNAM stops reading from the socket once it is
//                      full. Nothing here reads before finished(), so that buffer IS the resident body, and
//                      this is the thing that actually bounds memory.
//   downloadProgress   ends the transfer rather than leaving it stalled against a full buffer until the 20 s
//                      wall, and is what lets the caller tell a REFUSAL from a network error. bytesTotal is
//                      checked too, so a response that declares itself over budget is dropped before its body
//                      arrives; a Content-Length that lies is caught by bytesReceived regardless.
//
// It does not prevent the chunk that crosses the line, only the response after it — see the residual-gap note
// on ThemeRegistry::remainingDownloadBudget, which is the header this is the other half of.
//
// `context` owns the connection's lifetime: the blocking caller passes its stack QEventLoop so the lambda
// cannot outlive the `overBudget` flag it writes into, while an async caller passes the reply itself.
static void boundIncoming(QNetworkReply* reply, qint64 maxBytes, QObject* context, bool* overBudget)
{
    reply->setReadBufferSize(maxBytes + 1);   // +1 so the first byte PAST the budget is still seen and refused
    QObject::connect(reply, &QNetworkReply::downloadProgress, context,
                     [reply, maxBytes, overBudget](qint64 received, qint64 total) {
        if (received <= maxBytes && total <= maxBytes) return;   // total is -1 when unknown, which passes
        if (overBudget) *overBudget = true;
        reply->abort();
    });
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
    // PLAIN TEXT, explicitly. A QLabel defaults to Qt::AutoText, which renders anything that looks like
    // markup AS markup — and every sentence this label carries is built from registry-supplied strings: a
    // theme's name, a download error naming a listed path, and now the top-level keys of an index that did
    // not have the one we wanted. Those keys are attacker-chosen (a public registry takes pull requests), so
    // the one surface that quotes a registry's own vocabulary must not be the one that interprets it.
    status_->setTextFormat(Qt::PlainText);
    v->addWidget(status_);

    auto* bottom = new QHBoxLayout();
    repoLink_ = new QLabel(this);
    repoLink_->setTextFormat(Qt::RichText);
    repoLink_->setOpenExternalLinks(true);
    bottom->addWidget(repoLink_);
    bottom->addStretch(1);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    // Plain accept(): the mid-install guard is on done(), which every close goes through — this button,
    // Escape, a host's reject(). Guarding the button here instead would leave all the others open.
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::accept);
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
    // NOT WHILE AN INSTALL IS RUNNING. The first line of this function synchronously DELETES every card,
    // and an install holds its own card's QPushButton across up to 64 nested event loops (downloadTo) —
    // renderThemeEntry's lambda relabels `btn` the instant installThemeEntry returns. Three doors reach
    // here from inside those loops with the buttons still live: Reload, committing an added registry, and
    // removing a registry row. That is the same use-after-free the done() funnel was added for; the funnel
    // guards EXITS, and these three are not exits.
    //
    // Said out loud rather than returned silently: the user pressed Reload and, from where they are sitting,
    // nothing happened. The install's own per-file status line overwrites this within a file or two, which
    // is fine — what matters is that the press is acknowledged at all, and the line it is overwritten by
    // names the install that is the reason.
    //
    // OWED, not dropped, and the message says so because it is now true. Only ONE of the three doors is
    // Reload. The other two have already changed the registry list by the time they arrive here: commitAdd
    // has persisted the new registry and drawn its row, and the remove-row handler has deleted one and
    // dropped its row — so telling either of them "can't reload" describes neither what happened nor what
    // the user is looking at, and "it will finish first" promised a refresh that nothing was going to run.
    // finishInstall() now runs the refused refresh, which is what makes the sentence below a fact.
    if (installing_)
    {
        refreshPending_ = true;
        status_->setText(installStatus(tr("One install at a time — the list refreshes when it finishes.")));
        return;
    }
    while (QLayoutItem* it = listLayout_->takeAt(0)) { delete it->widget(); delete it; }
    const QStringList all = allRegistries();
    pending_ = all.size();
    total_ = 0;
    shapeProblems_ = 0;    // cleared with the cards that carried them, or a fixed registry stays "broken"
    status_->setText(tr("Loading…"));
    for (const QString& url : all) fetchOne(url);
}

void RegistryBrowser::fetchOne(const QString& indexUrl)
{
    QNetworkRequest req((QUrl(indexUrl)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(req);
    // An index.json is read into memory whole by the readAll() below, exactly like a blob, and a registry is
    // as free to serve half a gigabyte of it. Bounded on arrival for the same reason; an index refused this
    // way simply contributes no entries, which the "no entries found" line below already covers.
    boundIncoming(reply, ThemeRegistry::kMaxListingBytes, reply, nullptr);
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
                const ThemeRegistry::Index index = ThemeRegistry::parseIndex(body);
                // A document that parsed and held no container this reader knows is NOT an empty registry,
                // and used to be indistinguishable from one (#174). Show the reason on a card of its own,
                // attributed to the registry that served it — a key-name drift is otherwise found only by
                // someone who thinks to attach a debugger to a television.
                if (!index.ok()) { addProblemCard(indexUrl, index.shapeError); ++shapeProblems_; }
                for (const ThemeRegistry::Entry& e : index.entries) { renderThemeEntry(e, indexUrl); ++total_; }
            }
            else
            {
                const QJsonObject root = QJsonDocument::fromJson(body).object();
                for (const QJsonValue& e : root.value(QStringLiteral("addons")).toArray())
                    if (e.isObject()) { renderEntry(e.toObject(), indexUrl); ++total_; }
            }
        }
        if (--pending_ <= 0)
        {
            // Three outcomes, not two. "No entries found. Check the registry URLs." was being printed for a
            // registry that answered with a document we could not read, which reads as "there is nothing to
            // install" and sends the reader to check a URL that is perfectly correct. It is kept for the
            // case it was always right about — nothing came back, or a registry legitimately offers nothing
            // — and a registry we could not understand now says so, and points at the cards that say why.
            // Spelled with an explicit singular rather than tr()'s "%n registr(y/ies)": this string's whole
            // job is to be read off a television, and an untranslated Qt plural marker renders literally.
            const QString unreadable = shapeProblems_ == 1
                ? tr("1 registry could not be read — see above.")
                : tr("%1 registries could not be read — see above.").arg(shapeProblems_);
            QString msg;
            if (shapeProblems_ > 0 && total_ == 0)
                msg = tr("No entries available. %1").arg(unreadable);
            else if (shapeProblems_ > 0)
                msg = tr("%1 available. %2").arg(total_).arg(unreadable);
            else if (total_ == 0)
                msg = tr("No entries found. Check the registry URLs.");
            else
                msg = tr("%1 available across %2 registr%3.")
                          .arg(total_).arg(allRegistries().size())
                          .arg(allRegistries().size() == 1 ? tr("y") : tr("ies"));
            status_->setText(msg);
        }
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

// The card a registry gets INSTEAD of entries when what it served was not a document this app understands.
// It carries no Install button, because there is nothing here to install and offering a dead button would
// be a third way of saying the wrong thing.
void RegistryBrowser::addProblemCard(const QString& indexUrl, const QString& reason)
{
    auto* card = new QFrame();
    card->setFrameShape(QFrame::StyledPanel);
    auto* v = new QVBoxLayout(card);

    auto* title = new QLabel(tr("%1 — could not be read").arg(repoOf(indexUrl)));
    // Plain text for the same reason status_ is: `reason` quotes the index's own top-level key names, which
    // come off the network. This is the label that renders them, so this is where it has to be said.
    title->setTextFormat(Qt::PlainText);
    // A red that reads on BOTH a light and a dark palette. The theme cards' own #444/#999 greys are fine for
    // a description nobody has to read; this card is nothing BUT the thing to read, so it does not inherit
    // the habit of dimming it.
    title->setStyleSheet(QStringLiteral("font-weight:bold; color:#e05252;"));
    v->addWidget(title);

    auto* body = new QLabel(reason);
    body->setTextFormat(Qt::PlainText);
    body->setWordWrap(true);
    // Deliberately NO colour: the reason is the diagnosis, so it takes the palette's ordinary text colour
    // and stays legible whichever way round the palette is. Hard-coding a grey here made it the least
    // readable line on a panel whose whole purpose is that line.
    v->addWidget(body);

    // The URL, spelled out. A registry is identified everywhere else in this dialog by repoOf(), which is
    // deliberately short — but the reader of THIS card is about to go and look at the document, and the
    // whole point of the card is that they should not have to reconstruct which URL produced it.
    auto* src = new QLabel(indexUrl);
    src->setTextFormat(Qt::PlainText);
    src->setWordWrap(true);
    src->setStyleSheet(QStringLiteral("color:#999; font-size:11px;"));
    v->addWidget(src);

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
        // addCard's click handler already greyed this card to "Installing…". If the press was REFUSED
        // (another install is running) nothing was attempted, so put the card back rather than falling
        // through to the relabel below — which would read isInstalled, find it false, and offer "Retry"
        // for a failure that never happened.
        if (!installEntry(entry, indexUrl)) { btn->setText(tr("Install")); btn->setEnabled(true); return; }
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
        // Refused (another install is already running): nothing was attempted, so restore the card
        // addCard already greyed to "Installing…" instead of relabelling it "Retry" — see renderEntry.
        if (!installThemeEntry(entry, indexUrl)) { btn->setText(tr("Install")); btn->setEnabled(true); return; }
        const bool ok = isThemeInstalled(entry);
        btn->setText(ok ? tr("Installed ✓") : tr("Retry"));
        btn->setEnabled(!ok);
    });
}

bool RegistryBrowser::fetchToBuffer(const QString& url, qint64 maxBytes, QByteArray* out, QString* error,
                                    bool* overBudget)
{
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(req);

    QEventLoop loop;
    QTimer to; to.setSingleShot(true);
    connect(&to, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    bool over = false;
    boundIncoming(reply, maxBytes, &loop, &over);
    to.start(20000);
    loop.exec();

    // Checked BEFORE the error branch, because the abort this refusal performs surfaces as
    // OperationCanceledError with the errorString "Operation canceled" — which reads as a transient hiccup
    // worth retrying, and this is not one: it is the app declining a response, and the same registry will
    // send the same oversized body every time.
    if (over)
    {
        if (overBudget) *overBudget = true;
        if (error) *error = tr("the server sent more than the %1 MB this app will accept for one download, "
                               "so the transfer was stopped")
                                .arg(double(maxBytes) / (1024.0 * 1024.0));
        reply->abort(); reply->deleteLater();
        return false;
    }
    if (!reply->isFinished() || reply->error() != QNetworkReply::NoError)
    {
        if (error) *error = reply->isFinished() ? reply->errorString() : QStringLiteral("timed out");
        reply->abort(); reply->deleteLater();
        return false;
    }
    if (out) *out = reply->readAll();
    reply->deleteLater();
    return true;
}

bool RegistryBrowser::downloadTo(const QString& url, const QString& destPath, QString* error)
{
    QByteArray data;
    // The add-on path has no per-entry budget of its own, so one file's worth is the bound: the same
    // kMaxFileBytes a theme file gets, applied for the same reason (this reads the whole body into memory
    // before writing a byte of it). An add-on is a manifest and some scripts — orders of magnitude under it.
    if (!fetchToBuffer(url, ThemeRegistry::kMaxFileBytes, &data, error)) return false;

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

// EVERY way out of this dialog ends here — accept(), reject(), the Escape key (QDialog::keyPressEvent
// matches QKeySequence::Cancel), a close event, and a host calling reject() explicitly (LibraryView::
// navBack does). That is why the guard is here and not on the Close button: the button is one door of
// five, and the crash came in through Escape.
//
// Refusing here stops finished() being emitted at all, so the host's queued handler — which navigates
// away and, on the panel host, deletes us synchronously — cannot be delivered by the nested event loop
// we are currently sitting inside.
void RegistryBrowser::done(int r)
{
    if (installing_) { deferExit(); return; }
    QDialog::done(r);
}

void RegistryBrowser::closeWhenIdle()
{
    if (installing_) { deferExit(); return; }   // finishInstall() will do it
    accept();
}

// A deferred exit with nothing on screen to show for it reads as a dead button: an install can be several
// files behind a 20 s wall each, so the press could go unacknowledged for a minute, and an unresponsive
// Back on a TV reads as a freeze. Say the press landed and what it is waiting for.
void RegistryBrowser::deferExit()
{
    if (closeWhenIdle_) return;        // already owed — don't stack the note on a second press
    closeWhenIdle_ = true;
    status_->setText(installStatus(status_->text()));
}

// The install loop repaints the status line for every file, so the note above has to ride along with it or
// it would flash once and be overwritten by the next "Downloading…".
QString RegistryBrowser::installStatus(const QString& text) const
{
    if (!closeWhenIdle_) return text;
    return text + QLatin1Char('\n') + tr("Leaving when this install finishes…");
}

// Bracket for the two install paths: while one runs, this dialog's own frames are on the stack under a
// nested event loop, so anything that would delete it has to wait. Called on EVERY exit from an install,
// including the early error returns, or a failed install would leave the dialog permanently "busy".
void RegistryBrowser::finishInstall()
{
    installing_ = false;
    if (closeWhenIdle_)
    {
        closeWhenIdle_ = false;
        // Safe now: the nested loops have unwound, and finished() reaches the host queued, so nothing of ours
        // is on the stack when it deletes us. A refresh owed at the same time is moot — we are leaving.
        refreshPending_ = false;
        accept();
        return;
    }
    if (!refreshPending_) return;
    refreshPending_ = false;
    // A refresh refused mid-install (an added or removed registry, or Reload), run now that it is safe to.
    //
    // SINGLE-SHOT, not a direct call, and that is the whole care in this function. ~InstallScope runs this as
    // installThemeEntry RETURNS, and the caller it returns into — renderThemeEntry's lambda — goes straight on
    // to `btn->setText(...)` on the card fetchAll would have just deleted. Calling fetchAll() from here is
    // therefore the exact use-after-free the guard at the top of fetchAll was added to prevent, reintroduced
    // through the fix for it. Posting it to the event loop puts it after the click handler has fully returned,
    // with no dialog frame left on the stack.
    //
    // It also cannot land inside a nested loop and do harm: `this` as the context object drops it if the
    // dialog is destroyed first, and if a SECOND install has started by the time it fires, fetchAll's own
    // installing_ guard refuses it and owes it again.
    QTimer::singleShot(0, this, [this] { fetchAll(); });
}

bool RegistryBrowser::installEntry(const QJsonObject& entry, const QString& indexUrl)
{
    // A second card's Install, clicked from inside the first one's nested loop. Say so: silently returning
    // left the caller to re-read isInstalled, find it false and relabel an untouched card "Retry" — a
    // failure that never happened.
    if (installing_) { status_->setText(tr("One install at a time — this one has to finish first.")); return false; }
    const InstallScope scope(this);

    // A theme is a folder, not a file list, and the flattening loop below would drop its sounds/ and fonts/
    // subdirectories onto one level. Themes have their own path from fetchOne onwards and cannot reach this
    // function; the guard is here so that a future caller which forgets that is refused rather than served.
    //
    // FALSE, not true: nothing was attempted, so the card must go back to "Install" exactly as it was. The
    // bool exists precisely to tell a refusal from a failure, and answering true here offers "Retry" on an
    // untouched card for a failure that never happened — the confusion the return value was invented to
    // prevent, restated by the one branch that most needs it.
    if (kind_ == Themes)
    { status_->setText(tr("A theme can't be installed this way. Reopen this window and try again.")); return false; }

    const QString base = baseUrl(indexUrl);
    QStringList files;
    QString destDir;

    // FALSE for both of these, for the same reason the kind_ guard above returns false: nothing was
    // attempted. An entry with no id and an entry with no files are refusals by the registry's own listing,
    // not failures of an install — returning true had the caller re-read isInstalled, find it false and
    // relabel an untouched card "Retry" for something that never ran and that pressing again cannot change.
    // This is the confusion the return value exists to prevent, five lines from where it was just fixed.
    const QString id = entry.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) { status_->setText(tr("Entry has no id.")); return false; }
    destDir = localDirFor(id);
    for (const QJsonValue& fv : entry.value(QStringLiteral("files")).toArray()) files << fv.toString();

    if (files.isEmpty()) { status_->setText(tr("Nothing to download for this entry.")); return false; }

    for (const QString& rel : files)
    {
        if (rel.isEmpty()) continue;
        const QString url = base + QStringLiteral("/") + rel;
        const QString dest = destDir + QStringLiteral("/") + QFileInfo(rel).fileName();
        QString err;
        if (!downloadTo(url, dest, &err))
        {
            status_->setText(tr("Download failed: %1\n%2").arg(QFileInfo(rel).fileName(), err));
            return true;
        }
    }

    installed_ = true;
    status_->setText(tr("Installed “%1”.").arg(entry.value(QStringLiteral("name")).toString()));
    if (kind_ == Addons && addons_) addons_->reload();
    return true;
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

    // Reuse fetchToBuffer rather than opening a second blocking-fetch event loop: it already carries the
    // user agent, the redirect policy, the 20 s cap and the arrival-time byte budget, and a second copy of
    // that would drift from it. The listing is wanted in memory, so it never becomes a file — the fixed /tmp
    // path it used to round-trip through was shared with the themed surface and followed a pre-planted
    // symlink on desktop Linux.
    //
    // The budget matters here as much as on a blob: a hostile registry can serve a 500 MB tree JSON as
    // cheaply as a 500 MB font, and this response is read into memory whole.
    QByteArray body;
    QString err;
    if (!fetchToBuffer(api, ThemeRegistry::kMaxListingBytes, &body, &err))
    {
        // Rate limiting lands here too (GitHub allows 60 unauthenticated calls an hour per IP).
        if (error) *error = tr("Couldn't read the registry's file list: %1").arg(err);
        return QByteArray();
    }
    if (body.isEmpty())
    { if (error) *error = tr("The registry's file list came back empty."); return QByteArray(); }

    treeCache_.insert(indexUrl, body);
    return body;
}

// Install one themes2 entry: list the folder from the repo tree, download every file, then hand the whole
// set to ThemeRegistry::installFiles, which writes it atomically. Nothing touches themes2/<Name> until
// every byte is in hand — a half-installed theme would still be offered by the picker.
bool RegistryBrowser::installThemeEntry(const ThemeRegistry::Entry& e, const QString& indexUrl)
{
    // A second card's Install, clicked from inside the first one's nested loop. Say so: silently returning
    // left the caller to re-read isThemeInstalled, find it false and relabel an untouched card "Retry" — a
    // failure that never happened.
    if (installing_) { status_->setText(tr("One install at a time — this one has to finish first.")); return false; }
    const InstallScope scope(this);

    const QString folder = e.folder();
    if (folder.isEmpty()) { status_->setText(tr("This entry doesn't name a usable theme folder.")); return true; }

    status_->setText(installStatus(tr("Reading the registry's file list…")));
    QString err;
    const QByteArray tree = treeFor(indexUrl, &err);
    if (tree.isEmpty()) { status_->setText(err); return true; }

    const ThemeRegistry::Listing listing = ThemeRegistry::filesUnder(tree, e.dir);
    if (!listing.ok()) { status_->setText(listing.error); return true; }

    const QString base = baseUrl(indexUrl);
    QVector<QPair<QString, QByteArray>> blobs;
    // The bytes actually held so far. filesUnder has already checked the size the TREE CLAIMED, but the
    // blobs below are separate requests against branch HEAD: a registry that commits small files, gets
    // listed, then force-pushes large ones serves whatever it likes into an unbounded readAll(). The caps
    // are ThemeRegistry's so the themed surface enforces the same two numbers.
    qint64 got = 0;
    for (int i = 0; i < listing.files.size(); ++i)
    {
        const QString& rel = listing.files.at(i);
        // Named per file. This loop is up to kMaxFiles sequential fetches, each behind its own 20 s wall, on
        // the UI thread — a single unchanging "Installing…" across all of them is indistinguishable from a
        // hang. The label does repaint: downloadTo enters a nested event loop on the very next line.
        // The multi-arg form substitutes in ONE pass, so a file whose name contains "%2" is not treated as a
        // placeholder for the count that follows it.
        status_->setText(installStatus(tr("Downloading %1 (%2 of %3)…")
                             .arg(rel, QString::number(i + 1), QString::number(listing.files.size()))));

        // `rel` travels to installFiles as the string filesUnder validated. assetUrl percent-encodes for the
        // URL side only; nothing here re-parses, re-joins or decodes it, or a file legitimately named
        // "%2e%2e" would become ".." on the way to disk.
        const QString url = ThemeRegistry::assetUrl(base, e.dir, rel);
        QByteArray blob;
        QString derr;
        // The budget the transfer itself is held to, carrying the running total: the per-file cap and what is
        // left of the entry's total, whichever is smaller. Passed IN rather than applied afterwards, because
        // a check that runs once readAll() has returned is a check on bytes that are already resident.
        bool over = false;
        if (!fetchToBuffer(url, ThemeRegistry::remainingDownloadBudget(got), &blob, &derr, &over))
        {
            // A refusal is not a failure and must not read as one: nothing about this registry's response is
            // going to be different next time, so "Download failed" (which invites a retry) would be wrong.
            status_->setText(over ? tr("Refused %1: %2").arg(rel, derr)
                                  : tr("Download failed: %1\n%2").arg(rel, derr));
            return true;
        }
        // Belt to the arrival-time brace: the abort above is asynchronous, so a reply that finishes in the
        // same turn it crosses its budget can still hand back a body over it. This refuses that body rather
        // than installing it, and is the only bound left if a future caller forgets the budget argument.
        if (!ThemeRegistry::acceptDownloadedBytes(blob.size(), got, rel, &derr))
        { status_->setText(derr); return true; }
        got += blob.size();
        blobs << qMakePair(rel, blob);
    }

    // No "Writing the theme folder…" line here: installFiles is synchronous and no event loop spins between
    // setting such a label and replacing it, so it would never reach the screen. A status that cannot paint
    // is a comment written to the wrong place.
    if (!ThemeRegistry::installFiles(themesRoot(), folder, blobs, &err))
    { status_->setText(err); return true; }

    installed_ = true;
    // The folder stands in for a nameless entry: an index that omits "name" would otherwise say Installed “”.
    status_->setText(tr("Installed “%1”. Pick it from the theme list.")
                         .arg(e.name.isEmpty() ? folder : e.name));
    return true;
}

void RegistryBrowser::updateRepoLink()
{
    const QString page = QStringLiteral("https://github.com/") + repoOf(defaultUrl());
    const QString label = kind_ == Themes ? tr("↗ Browse / contribute themes on GitHub")
                                          : tr("↗ Browse / contribute add-ons on GitHub");
    repoLink_->setText(QStringLiteral("<a href=\"%1\">%2</a>").arg(page.toHtmlEscaped(), label));
}
