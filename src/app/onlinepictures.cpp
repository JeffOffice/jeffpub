#include "app/onlinepictures.h"

#include "app/editor.h"
#include "app/mainwindow.h"

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QFontMetrics>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <algorithm>

namespace jp {
namespace online {

namespace {
QString plain(const QString &html) { return QTextDocumentFragment::fromHtml(html).toPlainText().simplified(); }
} // namespace

std::function<void(const QUrl &, QObject *, const Done &)> fetch = [](const QUrl &url, QObject *context, const Done &done) {
    static auto *net = new QNetworkAccessManager(qApp);
    QNetworkRequest req(url);
    // Wikimedia asks programs to say who they are.
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("JeffPub/" JP_VERSION " (https://github.com/JeffOffice/jeffpub)"));
    req.setTransferTimeout(30000);
    QNetworkReply *r = net->get(req);
    QObject::connect(r, &QNetworkReply::finished, context, [r, done] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) done(QByteArray(), r->errorString());
        else done(r->readAll(), QString());
    });
    QObject::connect(context, &QObject::destroyed, r, &QNetworkReply::abort);
};

QUrl searchUrl(Source s, const QString &query)
{
    QUrlQuery q;
    if (s == Openverse) {
        QUrl u(QStringLiteral("https://api.openverse.org/v1/images/"));
        q.addQueryItem(QStringLiteral("q"), query);
        q.addQueryItem(QStringLiteral("page_size"), QStringLiteral("24"));
        u.setQuery(q);
        return u;
    }
    QUrl u(QStringLiteral("https://commons.wikimedia.org/w/api.php"));
    q.addQueryItem(QStringLiteral("action"), QStringLiteral("query"));
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    q.addQueryItem(QStringLiteral("generator"), QStringLiteral("search"));
    q.addQueryItem(QStringLiteral("gsrsearch"), query + QStringLiteral(" filetype:bitmap"));
    q.addQueryItem(QStringLiteral("gsrnamespace"), QStringLiteral("6"));
    q.addQueryItem(QStringLiteral("gsrlimit"), QStringLiteral("24"));
    q.addQueryItem(QStringLiteral("prop"), QStringLiteral("imageinfo"));
    q.addQueryItem(QStringLiteral("iiprop"), QStringLiteral("url|extmetadata"));
    q.addQueryItem(QStringLiteral("iiurlwidth"), QStringLiteral("240"));
    q.addQueryItem(QStringLiteral("iiextmetadatafilter"), QStringLiteral("LicenseShortName|LicenseUrl|Artist|ObjectName|AttributionRequired"));
    u.setQuery(q);
    return u;
}

QList<Picture> parseOpenverse(const QByteArray &json)
{
    QList<Picture> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).object().value(QStringLiteral("results")).toArray()) {
        const QJsonObject o = v.toObject();
        Picture p;
        p.title = o.value(QStringLiteral("title")).toString().simplified();
        p.creator = o.value(QStringLiteral("creator")).toString().simplified();
        const QString code = o.value(QStringLiteral("license")).toString().toLower(), version = o.value(QStringLiteral("license_version")).toString();
        p.license = code == QLatin1String("pdm") ? QStringLiteral("Public Domain Mark") : code == QLatin1String("cc0") ? QStringLiteral("CC0 ") + version
                                                                                                              : (QStringLiteral("CC ") + code.toUpper() + QLatin1Char(' ') + version).trimmed();
        p.licenseUrl = o.value(QStringLiteral("license_url")).toString();
        p.needsCredit = code != QLatin1String("cc0") && code != QLatin1String("pdm");
        p.page = QUrl(o.value(QStringLiteral("foreign_landing_url")).toString());
        p.full = QUrl(o.value(QStringLiteral("url")).toString());
        p.thumb = QUrl(o.value(QStringLiteral("thumbnail")).toString());
        if (p.full.isValid() && !p.full.isEmpty()) out << p;
    }
    return out;
}

QList<Picture> parseCommons(const QByteArray &json)
{
    QList<std::pair<int, Picture>> found;
    const QJsonObject pages = QJsonDocument::fromJson(json).object().value(QStringLiteral("query")).toObject().value(QStringLiteral("pages")).toObject();
    for (const QJsonValue &v : pages) {
        const QJsonObject o = v.toObject();
        const QJsonObject info = o.value(QStringLiteral("imageinfo")).toArray().first().toObject();
        const QJsonObject meta = info.value(QStringLiteral("extmetadata")).toObject();
        auto field = [&](const char *k) { return meta.value(QLatin1String(k)).toObject().value(QStringLiteral("value")).toString(); };
        Picture p;
        QString name = o.value(QStringLiteral("title")).toString();
        if (name.startsWith(QLatin1String("File:"))) name = name.mid(5);
        p.title = plain(field("ObjectName"));
        if (p.title.isEmpty()) p.title = name.section(QLatin1Char('.'), 0, -2);
        p.creator = plain(field("Artist"));
        p.license = plain(field("LicenseShortName"));
        p.licenseUrl = field("LicenseUrl");
        p.needsCredit = field("AttributionRequired") != QLatin1String("false");
        p.page = QUrl(info.value(QStringLiteral("descriptionurl")).toString());
        p.full = QUrl(info.value(QStringLiteral("url")).toString());
        p.thumb = QUrl(info.value(QStringLiteral("thumburl")).toString());
        if (p.full.isValid() && !p.full.isEmpty()) found << std::make_pair(o.value(QStringLiteral("index")).toInt(), p);
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first < b.first; });   // the search's order
    QList<Picture> out;
    for (const auto &f : found) out << f.second;
    return out;
}

QString credit(const Picture &p)
{
    const QString title = p.title.isEmpty() ? QCoreApplication::translate("Online", "Picture") : QStringLiteral("“%1”").arg(p.title);
    QString s = p.creator.isEmpty() ? title : QCoreApplication::translate("Online", "%1 by %2").arg(title, p.creator);
    if (!p.license.isEmpty()) s += QStringLiteral(", ") + p.license;
    return s;
}

} // namespace online

OnlinePicturesPane::OnlinePicturesPane(MainWindow *win) : m_win(win)
{
    setObjectName(QStringLiteral("jpOnlinePictures"));
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    auto *intro = new QLabel(tr("Free, openly licensed pictures from two open libraries. Your search words go to the library you choose."), this);
    intro->setWordWrap(true);
    v->addWidget(intro);
    m_source = new QComboBox(this);
    m_source->addItems({QStringLiteral("Openverse"), QStringLiteral("Wikimedia Commons")});
    m_source->setAccessibleName(tr("Library"));
    m_source->setToolTip(tr("Openverse searches many open collections; Wikimedia Commons is the library of Wikipedia's pictures."));
    v->addWidget(m_source);
    auto *row = new QHBoxLayout();
    m_query = new QLineEdit(this);
    m_query->setPlaceholderText(tr("Search for pictures"));
    m_query->setAccessibleName(tr("Search for pictures"));
    m_query->setClearButtonEnabled(true);
    auto *go = new QPushButton(tr("Search"), this);
    row->addWidget(m_query, 1);
    row->addWidget(go);
    v->addLayout(row);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    v->addWidget(m_status);
    m_results = new QListWidget(this);
    m_results->setViewMode(QListView::IconMode);
    m_results->setIconSize(QSize(110, 110));
    m_results->setGridSize(QSize(124, 140));
    m_results->setResizeMode(QListView::Adjust);
    m_results->setMovement(QListView::Static);
    m_results->setWordWrap(true);
    m_results->setAccessibleName(tr("Pictures found"));
    v->addWidget(m_results, 1);
    m_details = new QLabel(this);
    m_details->setWordWrap(true);
    m_details->setOpenExternalLinks(true);
    m_details->setTextInteractionFlags(Qt::TextBrowserInteraction);
    v->addWidget(m_details);
    m_credit = new QCheckBox(tr("Add a credit under the picture"), this);
    m_credit->setToolTip(tr("Most open licenses ask you to name the author and the license where the picture is used."));
    v->addWidget(m_credit);
    m_insert = new QPushButton(tr("Insert"), this);
    m_insert->setEnabled(false);
    v->addWidget(m_insert);
    connect(go, &QPushButton::clicked, this, [this] { search(m_query->text()); });
    connect(m_query, &QLineEdit::returnPressed, this, [this] { search(m_query->text()); });
    connect(m_results, &QListWidget::currentRowChanged, this, &OnlinePicturesPane::showDetails);
    connect(m_results, &QListWidget::itemDoubleClicked, this, &OnlinePicturesPane::insertChosen);
    connect(m_insert, &QPushButton::clicked, this, &OnlinePicturesPane::insertChosen);
}

void OnlinePicturesPane::search(const QString &query)
{
    m_query->setText(query);
    m_results->clear();
    m_found.clear();
    showDetails();
    if (query.trimmed().isEmpty()) return;
    const int mine = ++m_search;
    const auto source = online::Source(m_source->currentIndex());
    m_status->setText(tr("Searching..."));
    QPointer<OnlinePicturesPane> self(this);
    online::fetch(online::searchUrl(source, query.trimmed()), this, [self, mine, source](const QByteArray &bytes, const QString &error) {
        if (!self || mine != self->m_search) return;
        if (!error.isEmpty()) {
            self->m_status->setText(tr("The library couldn't be reached: %1").arg(error));
            return;
        }
        self->m_found = source == online::Openverse ? online::parseOpenverse(bytes) : online::parseCommons(bytes);
        self->m_status->setText(self->m_found.isEmpty() ? tr("No pictures found. Try other words, or the other library.")
                                                        : tr("%n picture(s) found. Choose one to see its license.", nullptr, int(self->m_found.size())));
        for (int i = 0; i < self->m_found.size(); ++i) {
            const online::Picture &p = self->m_found[i];
            auto *it = new QListWidgetItem(QFontMetrics(self->m_results->font()).elidedText(p.title, Qt::ElideRight, 116), self->m_results);
            it->setToolTip(online::credit(p));
            it->setData(Qt::AccessibleTextRole, online::credit(p));
            // Its small copy, when it comes.
            QPointer<QListWidget> list(self->m_results);
            online::fetch(p.thumb, self->m_results, [list, it, mine, self](const QByteArray &img, const QString &) {
                if (!list || !self || mine != self->m_search) return;
                QPixmap pm;
                if (pm.loadFromData(img)) it->setIcon(QIcon(pm));
            });
        }
        if (!self->m_found.isEmpty()) self->m_results->setCurrentRow(0);
    });
}

void OnlinePicturesPane::showDetails()
{
    const int i = m_results->currentRow();
    m_insert->setEnabled(i >= 0 && i < m_found.size());
    if (i < 0 || i >= m_found.size()) {
        m_details->clear();
        return;
    }
    const online::Picture &p = m_found[i];
    QString html = QStringLiteral("<b>%1</b>").arg(p.title.toHtmlEscaped());
    if (!p.creator.isEmpty()) html += QStringLiteral("<br>") + tr("By %1").arg(p.creator.toHtmlEscaped());
    if (!p.license.isEmpty())
        html += QStringLiteral("<br>") + (p.licenseUrl.isEmpty() ? p.license.toHtmlEscaped() : QStringLiteral("<a href=\"%1\">%2</a>").arg(p.licenseUrl.toHtmlEscaped(), p.license.toHtmlEscaped()));
    if (p.page.isValid()) html += QStringLiteral("<br><a href=\"%1\">%2</a>").arg(p.page.toString().toHtmlEscaped(), tr("Its page in the library"));
    m_details->setText(html);
    m_credit->setChecked(p.needsCredit);
}

void OnlinePicturesPane::insertChosen()
{
    const int i = m_results->currentRow();
    if (i < 0 || i >= m_found.size()) return;
    const online::Picture p = m_found[i];
    const bool withCredit = m_credit->isChecked();
    m_status->setText(tr("Downloading %1...").arg(p.title));
    m_insert->setEnabled(false);
    QPointer<OnlinePicturesPane> self(this);
    online::fetch(p.full, this, [self, p, withCredit](const QByteArray &bytes, const QString &error) {
        if (!self) return;
        self->m_insert->setEnabled(true);
        QBuffer buf;
        buf.setData(bytes);
        const QByteArray fmt = QImageReader(&buf).format();
        if (!error.isEmpty() || fmt.isEmpty()) {
            self->m_status->setText(error.isEmpty() ? tr("That picture couldn't be read.") : tr("The picture couldn't be downloaded: %1").arg(error));
            return;
        }
        // A file named after the picture, so the Graphics Manager shows its name.
        QString name = p.title;
        name.replace(QRegularExpression(QStringLiteral("[^\\w -]+")), QStringLiteral(" "));
        name = name.simplified().left(60);
        if (name.isEmpty()) name = QStringLiteral("picture");
        const QString path = QDir(self->m_downloads.path()).filePath(name + QLatin1Char('.') + QString::fromLatin1(fmt == "jpeg" ? QByteArray("jpg") : fmt));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) {
            self->m_status->setText(tr("The picture couldn't be saved for inserting."));
            return;
        }
        f.close();
        MainWindow *win = self->m_win;
        Editor *ed = win->editor();
        win->insertFiles({path}, QPointF(-1, -1));
        auto *pic = dynamic_cast<PictureItem *>(ed->single());
        if (!pic) return;
        // Its title describes it for screen readers; the credit names its author and license.
        ed->change(tr("Picture Credit"), [&] {
            pic->altText = p.title;
            if (!withCredit) return;
            auto t = std::static_pointer_cast<TextItem>(ed->newTextBox(QRectF(pic->rect.left(), pic->rect.bottom() + 4, pic->rect.width(), 22), online::credit(p)));
            QTextCursor c(ed->doc()->storyDoc(t->storyId));
            c.select(QTextCursor::Document);
            if (const TextStyle *s = ed->doc()->style(QStringLiteral("Caption"))) { c.mergeCharFormat(s->chr); c.mergeBlockFormat(s->blk); }
            ed->surfaceItems().push_back(t);
            pic->caption = t->id;
        });
        ed->select(pic->id);
        self->m_status->setText(tr("Inserted. Check that its license fits how you'll use it."));
    });
}

} // namespace jp
