#include "RegistryBrowser.h"
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
#include <QUrl>

static QSettings& store()
{
    static QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/goliath.ini"),
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
        ? QStringLiteral("https://raw.githubusercontent.com/cubman3134/mymediavault-themes/main/index.json")
        : QStringLiteral("https://raw.githubusercontent.com/cubman3134/mymediavault-addons/main/index.json");
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

    registriesLayout_ = new QVBoxLayout();
    v->addLayout(registriesLayout_);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto* host = new QWidget(scroll);
    listLayout_ = new QVBoxLayout(host);
    listLayout_->setAlignment(Qt::AlignTop);
    scroll->setWidget(host);
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
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::accept);
    bottom->addWidget(box);
    v->addLayout(bottom);

    connect(reload, &QPushButton::clicked, this, [this] { fetchAll(); });
    connect(add, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString url = QInputDialog::getText(this, tr("Add registry"),
            tr("Registry index URL (raw GitHub):"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || url.isEmpty()) return;
        QStringList extras = extraRegistries();
        if (!extras.contains(url)) { extras << url; saveExtras(extras); }
        renderRegistryRows();
        fetchAll();
    });

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
    const QString app = QCoreApplication::applicationDirPath();
    if (kind_ == Themes) return app + QStringLiteral("/themes");
    return app + QStringLiteral("/addons/") + id;
}

bool RegistryBrowser::isInstalled(const QJsonObject& entry) const
{
    if (kind_ == Themes)
    {
        const QString file = entry.value(QStringLiteral("file")).toString();
        if (file.isEmpty()) return false;
        return QFile::exists(localDirFor(QString()) + QStringLiteral("/") + QFileInfo(file).fileName());
    }
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
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, indexUrl] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
        {
            const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonArray entries = root.value(kind_ == Themes ? QStringLiteral("themes")
                                                                  : QStringLiteral("addons")).toArray();
            for (const QJsonValue& e : entries)
                if (e.isObject()) { renderEntry(e.toObject(), indexUrl); ++total_; }
        }
        if (--pending_ <= 0)
            status_->setText(total_ == 0 ? tr("No entries found. Check the registry URLs.")
                                         : tr("%1 available across %2 registr%3.")
                                               .arg(total_).arg(allRegistries().size())
                                               .arg(allRegistries().size() == 1 ? tr("y") : tr("ies")));
    });
}

void RegistryBrowser::renderEntry(const QJsonObject& entry, const QString& indexUrl)
{
    auto* card = new QFrame();
    card->setFrameShape(QFrame::StyledPanel);
    auto* h = new QHBoxLayout(card);

    auto* texts = new QVBoxLayout();
    const QString name = entry.value(QStringLiteral("name")).toString();
    const QString author = entry.value(QStringLiteral("author")).toString();
    auto* title = new QLabel(QStringLiteral("<b>%1</b>%2").arg(name.toHtmlEscaped(),
        author.isEmpty() ? QString()
                         : QStringLiteral("  <span style='color:#888;'>by %1</span>").arg(author.toHtmlEscaped())));
    title->setTextFormat(Qt::RichText);
    texts->addWidget(title);
    auto* desc = new QLabel(entry.value(QStringLiteral("description")).toString());
    desc->setWordWrap(true);
    desc->setStyleSheet(QStringLiteral("color:#444;"));
    texts->addWidget(desc);
    auto* src = new QLabel(tr("from %1").arg(repoOf(indexUrl)));
    src->setStyleSheet(QStringLiteral("color:#999; font-size:11px;"));
    texts->addWidget(src);
    h->addLayout(texts, 1);

    auto* btn = new QPushButton(card);
    const bool installed = isInstalled(entry);
    btn->setText(installed ? tr("Installed ✓") : tr("Install"));
    btn->setEnabled(!installed);
    connect(btn, &QPushButton::clicked, this, [this, entry, indexUrl, btn] {
        btn->setEnabled(false);
        btn->setText(tr("Installing…"));
        installEntry(entry, indexUrl);
        const bool ok = isInstalled(entry);
        btn->setText(ok ? tr("Installed ✓") : tr("Retry"));
        btn->setEnabled(!ok);
    });
    h->addWidget(btn, 0, Qt::AlignTop);

    listLayout_->addWidget(card);
}

bool RegistryBrowser::downloadTo(const QString& url, const QString& destPath, QString* error)
{
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
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
    f.write(data);
    return true;
}

void RegistryBrowser::installEntry(const QJsonObject& entry, const QString& indexUrl)
{
    const QString base = baseUrl(indexUrl);
    QStringList files;
    QString destDir;

    if (kind_ == Themes)
    {
        destDir = localDirFor(QString());
        const QString file = entry.value(QStringLiteral("file")).toString();
        if (!file.isEmpty()) files << file;
        for (const QJsonValue& a : entry.value(QStringLiteral("assets")).toArray()) files << a.toString();
    }
    else
    {
        const QString id = entry.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) { status_->setText(tr("Entry has no id.")); return; }
        destDir = localDirFor(id);
        for (const QJsonValue& fv : entry.value(QStringLiteral("files")).toArray()) files << fv.toString();
    }

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

void RegistryBrowser::updateRepoLink()
{
    const QString page = QStringLiteral("https://github.com/") + repoOf(defaultUrl());
    const QString label = kind_ == Themes ? tr("↗ Browse / contribute themes on GitHub")
                                          : tr("↗ Browse / contribute add-ons on GitHub");
    repoLink_->setText(QStringLiteral("<a href=\"%1\">%2</a>").arg(page.toHtmlEscaped(), label));
}
