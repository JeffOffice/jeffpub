#include "app/backstage.h"

#include "app/appfuncs.h"
#include "app/dialogs.h"
#include "app/icons.h"
#include "app/mainwindow.h"
#include "app/settings.h"
#include "app/updater.h"
#include "io/importers.h"
#include "io/jpubfile.h"
#include "render/renderer.h"
#include "templates/templates.h"

#include <QAction>
#include <QRegularExpression>
#include <QLocale>
#include <QUrl>
#include <QDesktopServices>
#include <QTextOption>
#include <QStyledItemDelegate>
#include <QFrame>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QJsonDocument>
#include <QTimer>
#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QPrinterInfo>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyleHints>
#include <QToolButton>
#include <QVBoxLayout>
#include <QMenu>

namespace jp {

static bool dark() { return uiDark(); }

static QLabel *heading(const QString &t, QWidget *parent, double scale = 1.75)
{
    auto *l = new QLabel(t, parent);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * scale);
    f.setWeight(QFont::DemiBold);
    l->setFont(f);
    l->setContentsMargins(0, 0, 0, 6);
    return l;
}

// A page drawn as paper: soft layered shadow and a hairline edge.
static QPixmap paperPixmap(const QImage &page, const QSize &box, qreal dpr)
{
    QPixmap pm(box * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QSizeF avail = QSizeF(box) - QSizeF(16, 18);
    QSizeF sz = QSizeF(page.size()) / page.devicePixelRatio();
    sz.scale(avail, Qt::KeepAspectRatio);
    const QRectF r(QPointF((box.width() - sz.width()) / 2.0, (box.height() - sz.height()) / 2.0 - 2), sz);
    for (int i = 6; i >= 1; --i) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 7));
        p.drawRoundedRect(r.adjusted(-i * 0.6, -i * 0.3 + 1, i * 0.6, i * 0.9 + 1), 2 + i * 0.5, 2 + i * 0.5);
    }
    p.drawImage(r, page);
    p.setPen(QPen(QColor(0, 0, 0, 38), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
    return pm;
}

// A rounded white (or dark) panel used for grouped content.
static QFrame *cardFrame(QWidget *parent)
{
    auto *f = new QFrame(parent);
    f->setObjectName(QStringLiteral("jpCard"));
    f->setStyleSheet(dark() ? QStringLiteral("#jpCard{background:#22262d; border:1px solid #2f343d; border-radius:12px;}")
                            : QStringLiteral("#jpCard{background:#ffffff; border:1px solid #e3e5ea; border-radius:12px;}"));
    return f;
}

static QLabel *mutedLabel(const QString &t, QWidget *parent, double alpha = 0.62)
{
    auto *l = new QLabel(t, parent);
    QPalette pl = l->palette();
    QColor c = uiText();
    c.setAlphaF(float(alpha));
    pl.setColor(QPalette::WindowText, c);
    l->setPalette(pl);
    return l;
}

namespace {
// Template cards: rounded hover and selection, picture above a two-line name.
class CardDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(opt.rect).adjusted(5, 5, -5, -5);
        const bool sel = opt.state & QStyle::State_Selected, hover = opt.state & QStyle::State_MouseOver;
        QColor accent = uiAccent();
        if (sel) {
            QColor bg = accent;
            bg.setAlphaF(dark() ? 0.22f : 0.08f);
            p->setPen(QPen(accent, 2));
            p->setBrush(bg);
            p->drawRoundedRect(r.adjusted(1, 1, -1, -1), 10, 10);
        } else if (hover) {
            p->setPen(Qt::NoPen);
            p->setBrush(dark() ? QColor(255, 255, 255, 16) : QColor(0, 0, 0, 10));
            p->drawRoundedRect(r, 10, 10);
        }
        const QIcon ic = idx.data(Qt::DecorationRole).value<QIcon>();
        const QSize is = opt.decorationSize;
        const QRect ir(int(r.center().x() - is.width() / 2.0), int(r.top() + 8), is.width(), is.height());
        ic.paint(p, ir, Qt::AlignCenter, QIcon::Normal);
        QFont f = opt.font;
        if (sel) f.setWeight(QFont::DemiBold);
        p->setFont(f);
        p->setPen(sel && !dark() ? accent : uiText());
        const QRectF tr(r.left() + 8, ir.bottom() + 8, r.width() - 16, r.bottom() - ir.bottom() - 10);
        const QString text = idx.data(Qt::DisplayRole).toString();
        QTextOption to(Qt::AlignHCenter | Qt::AlignTop);
        to.setWrapMode(QTextOption::WordWrap);
        p->drawText(tr, QFontMetrics(f).elidedText(text, Qt::ElideRight, int(tr.width() * 2 - 10)), to);
        p->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &) const override
    {
        return QSize(opt.decorationSize.width() + 40, opt.decorationSize.height() + 62);
    }
};
} // namespace

namespace {

// One sidebar entry: icon and label, a rounded highlight when current, a
// softer one on hover, and a short accent bar at the left edge.
class NavItem : public QAbstractButton {
public:
    NavItem(const QString &iconName, const QString &text, QWidget *parent) : QAbstractButton(parent), m_icon(iconName)
    {
        setText(text);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setMinimumHeight(38);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAttribute(Qt::WA_Hover);
    }
    QSize sizeHint() const override { return QSize(200, 38); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(10, 2, -10, -2);
        const QColor accent = uiAccent();
        const bool on = isChecked(), hover = underMouse() && !on;
        if (on) {
            QColor bg = accent;
            bg.setAlphaF(dark() ? 0.28f : 0.12f);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(r, 8, 8);
            p.setBrush(accent);
            p.drawRoundedRect(QRectF(r.left() + 2, r.center().y() - 9, 3.5, 18), 1.75, 1.75);
        } else if (hover) {
            p.setPen(Qt::NoPen);
            p.setBrush(dark() ? QColor(255, 255, 255, 18) : QColor(0, 0, 0, 12));
            p.drawRoundedRect(r, 8, 8);
        }
        if (hasFocus() && !on) {
            p.setPen(QPen(accent, 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(0.75, 0.75, -0.75, -0.75), 8, 8);
        }
        const QColor fg = on ? (dark() ? accent.lighter(170) : accent) : uiText();
        // Tint the line icon to the label color.
        const int is = 18;
        QPixmap pm = jp::icon(m_icon).pixmap(QSize(is, is), devicePixelRatioF());
        QPainter tp(&pm);
        tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tp.fillRect(pm.rect(), fg);
        tp.end();
        p.drawPixmap(QPointF(r.left() + 14, r.center().y() - is / 2.0), pm);
        QFont f = font();
        f.setWeight(on ? QFont::DemiBold : QFont::Normal);
        p.setFont(f);
        p.setPen(fg);
        p.drawText(r.adjusted(14 + is + 12, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, text());
    }

private:
    QString m_icon;
};


// A command shown as a card: tinted icon tile, bold title, muted description.
class ActionCard : public QAbstractButton {
public:
    ActionCard(const QString &iconName, const QString &title, const QString &desc, QWidget *parent)
        : QAbstractButton(parent), m_icon(iconName), m_desc(desc)
    {
        setText(title);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMaximumWidth(820);
    }
    QSize sizeHint() const override
    {
        const QFontMetrics fm(font());
        const int textW = std::max(200, width() - 92);
        const int dh = fm.boundingRect(QRect(0, 0, textW, 1000), Qt::TextWordWrap, m_desc).height();
        return QSize(520, std::max(64, 22 + fm.height() + 4 + dh));
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override
    {
        const QFontMetrics fm(font());
        const int dh = fm.boundingRect(QRect(0, 0, std::max(200, w - 92), 1000), Qt::TextWordWrap, m_desc).height();
        return std::max(64, 22 + fm.height() + 4 + dh);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool hover = underMouse(), down = isDown();
        const QColor accent = uiAccent();
        QColor bg = dark() ? QColor(0x22, 0x26, 0x2d) : QColor(Qt::white);
        if (down) bg = dark() ? QColor(0x2c, 0x31, 0x3a) : QColor(0xf3, 0xf4, 0xf6);
        QColor border = dark() ? QColor(0x2f, 0x34, 0x3d) : QColor(0xe3, 0xe5, 0xea);
        if (hover || hasFocus()) border = accent;
        p.setPen(QPen(border, hover ? 1.4 : 1));
        p.setBrush(bg);
        p.drawRoundedRect(r, 10, 10);
        const QRectF tile(r.left() + 14, r.top() + 12, 40, 40);
        QColor tbg = accent;
        tbg.setAlphaF(dark() ? 0.25f : 0.10f);
        p.setPen(Qt::NoPen);
        p.setBrush(tbg);
        p.drawRoundedRect(tile, 9, 9);
        QPixmap pm = jp::icon(m_icon).pixmap(QSize(22, 22), devicePixelRatioF());
        {
            QPainter tp(&pm);
            tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tp.fillRect(pm.rect(), dark() ? accent.lighter(150) : accent);
        }
        p.drawPixmap(QPointF(tile.center().x() - 11, tile.center().y() - 11), pm);
        QFont tf = font();
        tf.setWeight(QFont::DemiBold);
        p.setFont(tf);
        p.setPen(uiText());
        const QFontMetrics fm(font());
        const QRectF tr(tile.right() + 14, r.top() + 11, r.right() - tile.right() - 28, fm.height() + 2);
        p.drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, text());
        p.setFont(font());
        QColor dc = uiText();
        dc.setAlphaF(0.68f);
        p.setPen(dc);
        p.drawText(QRectF(tr.left(), tr.bottom() + 2, tr.width(), r.bottom() - tr.bottom() - 8), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, m_desc);
        // Chevron on the right.
        p.setPen(QPen(hover ? accent : dc, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF c(r.right() - 18, r.center().y());
        p.drawPolyline(QPolygonF({c + QPointF(-3, -5), c + QPointF(2, 0), c + QPointF(-3, 5)}));
    }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave) update();
        return QAbstractButton::event(e);
    }

private:
    QString m_icon, m_desc;
};

QLabel *sectionLabel(const QString &t, QWidget *parent)
{
    auto *l = new QLabel(t.toUpper(), parent);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.78);
    f.setWeight(QFont::DemiBold);
    f.setLetterSpacing(QFont::PercentageSpacing, 108);
    l->setFont(f);
    QPalette pl = l->palette();
    QColor c = uiText();
    c.setAlphaF(0.55f);
    pl.setColor(QPalette::WindowText, c);
    l->setPalette(pl);
    l->setContentsMargins(24, 14, 12, 4);
    return l;
}

} // namespace

Backstage::Backstage(MainWindow *win) : QWidget(win), m_win(win)
{
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, dark() ? QColor(0x1E, 0x22, 0x29) : QColor(0xFB, 0xFB, 0xFC));
    setPalette(pal);
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    // Sidebar: a quiet panel with the wordmark, a back button and grouped commands.
    auto *side = new QFrame(this);
    side->setObjectName(QStringLiteral("jpSide"));
    side->setStyleSheet(dark() ? QStringLiteral("#jpSide{background:#171a1f; border-right:1px solid #2b3038;}")
                               : QStringLiteral("#jpSide{background:#f1f2f5; border-right:1px solid #dfe1e6;}"));
    side->setFixedWidth(236);
    auto *sv = new QVBoxLayout(side);
    sv->setContentsMargins(0, 18, 0, 14);
    sv->setSpacing(0);

    auto *brand = new QWidget(side);
    auto *bh = new QHBoxLayout(brand);
    bh->setContentsMargins(22, 0, 12, 0);
    bh->setSpacing(10);
    auto *badge = new QLabel(brand);
    {
        const qreal dpr = devicePixelRatioF();
        QPixmap pm(QSize(30, 30) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(uiAccent());
        p.drawRoundedRect(QRectF(0, 0, 30, 30), 8, 8);
        QFont bf = font();
        bf.setBold(true);
        bf.setPixelSize(13);
        p.setFont(bf);
        p.setPen(Qt::white);
        p.drawText(QRectF(0, 0, 30, 30), Qt::AlignCenter, QStringLiteral("79"));
        p.end();
        badge->setPixmap(pm);
    }
    auto *word = new QLabel(QStringLiteral("JeffPub"), brand);
    QFont wf = word->font();
    wf.setPointSizeF(wf.pointSizeF() * 1.35);
    wf.setWeight(QFont::DemiBold);
    word->setFont(wf);
    bh->addWidget(badge);
    bh->addWidget(word);
    bh->addStretch(1);
    sv->addWidget(brand);
    sv->addSpacing(14);

    auto *back = new NavItem(QStringLiteral("arrow-left"), QStringLiteral("Back to publication"), side);
    back->setCheckable(false);
    back->setToolTip(QStringLiteral("Return to your publication (Esc)"));
    connect(back, &QAbstractButton::clicked, this, &Backstage::closeRequested);
    sv->addWidget(back);

    auto *group = new QButtonGroup(this);
    group->setExclusive(true);
    struct Entry { const char *key, *icon, *label; };
    const QList<QPair<QString, QList<Entry>>> sections = {
        {QStringLiteral("Start"), {{"new", "file-plus", "New"}, {"open", "folder-open", "Open"}}},
        {QStringLiteral("This publication"),
         {{"save", "save", "Save"}, {"saveas", "copy", "Save a Copy"}, {"print", "printer", "Print"}, {"export", "file-output", "Export"},
          {"share", "share-2", "Share"}, {"info", "file-text", "Properties"}, {"close", "circle-x", "Close"}}},
        {QStringLiteral("JeffPub"), {{"options", "settings", "Settings"}, {"about", "info", "About"}}},
    };
    for (const auto &sec : sections) {
        sv->addWidget(sectionLabel(sec.first, side));
        for (const Entry &e : sec.second) {
            const QString key = QString::fromLatin1(e.key);
            auto *item = new NavItem(QString::fromLatin1(e.icon), QString::fromLatin1(e.label), side);
            const bool command = key == "save" || key == "saveas" || key == "close" || key == "options";
            if (command) item->setCheckable(false);
            else group->addButton(item);
            m_navItems.insert(key, item);
            sv->addWidget(item);
            connect(item, &QAbstractButton::clicked, this, [this, key] {
                if (key == "save") { if (m_win->save()) Q_EMIT closeRequested(); return; }
                if (key == "saveas") { if (m_win->saveAs()) Q_EMIT closeRequested(); return; }
                if (key == "close") { m_win->act("file.close")->trigger(); Q_EMIT closeRequested(); return; }
                if (key == "options") { optionsDialog(this, m_win->editor()); return; }
                showPage(key);
            });
        }
    }
    sv->addStretch(1);
    auto *ver = new QLabel(QStringLiteral("Version %1").arg(QStringLiteral(JP_VERSION)), side);
    ver->setContentsMargins(24, 0, 12, 0);
    QPalette vp = ver->palette();
    QColor vc = uiText();
    vc.setAlphaF(0.5f);
    vp.setColor(QPalette::WindowText, vc);
    ver->setPalette(vp);
    sv->addWidget(ver);
    h->addWidget(side);

    m_stack = new QStackedWidget(this);
    h->addWidget(m_stack, 1);
    for (const QString &key : {QStringLiteral("info"), QStringLiteral("new"), QStringLiteral("open"), QStringLiteral("print"),
                               QStringLiteral("share"), QStringLiteral("export"), QStringLiteral("about")})
        m_index[key] = m_stack->addWidget(new QWidget());
}

void Backstage::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) { Q_EMIT closeRequested(); return; }
    QWidget::keyPressEvent(e);
}

void Backstage::rebuild(const QString &name)
{
    QWidget *w = nullptr;
    if (name == "info") w = buildInfo();
    else if (name == "new") w = buildNew();
    else if (name == "open") w = buildOpen();
    else if (name == "print") w = buildPrint();
    else if (name == "share") w = buildShare();
    else if (name == "export") w = buildExport();
    else if (name == "about") w = buildAbout();
    if (!w) return;
    auto *scroll = new QScrollArea();
    scroll->setWidget(w);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    const int idx = m_index.value(name, -1);
    if (idx < 0) return;
    QWidget *old = m_stack->widget(idx);
    m_stack->insertWidget(idx, scroll);
    m_stack->removeWidget(old);
    old->deleteLater();
}

void Backstage::showPage(const QString &name)
{
    rebuild(name);
    m_stack->setCurrentIndex(m_index.value(name, 0));
    if (QAbstractButton *b = m_navItems.value(name)) b->setChecked(true);
}

// ---------------- Info ----------------
QWidget *Backstage::buildInfo()
{
    Editor *ed = m_win->editor();
    Document *d = ed->doc();
    auto *w = new QWidget();
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(40, 30, 40, 30);
    v->addWidget(heading(QStringLiteral("Properties"), w));

    // Where the publication lives.
    auto *where = new QHBoxLayout();
    where->setSpacing(10);
    const QString path = ed->filePath();
    auto *pathIcon = new QLabel(w);
    pathIcon->setPixmap(icon(path.isEmpty() ? "file" : "folder").pixmap(QSize(16, 16), devicePixelRatioF()));
    where->addWidget(pathIcon);
    auto *pathLabel = mutedLabel(path.isEmpty() ? QStringLiteral("This publication hasn't been saved yet.") : QDir::toNativeSeparators(path), w, 0.7);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLabel->setWordWrap(true);
    where->addWidget(pathLabel, 1);
    if (!path.isEmpty()) {
        auto *showBtn = new QPushButton(QStringLiteral("Show in Folder"), w);
        showBtn->setCursor(Qt::PointingHandCursor);
        connect(showBtn, &QPushButton::clicked, this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())); });
        where->addWidget(showBtn);
    }
    v->addLayout(where);
    v->addSpacing(14);

    auto *body = new QHBoxLayout();
    body->setSpacing(20);
    auto *left = new QVBoxLayout();
    left->setSpacing(10);
    auto card = [&](const QString &ic, const QString &title, const QString &text, const std::function<void()> &fn) {
        auto *b = new ActionCard(ic, title, text, w);
        connect(b, &QAbstractButton::clicked, this, [fn] { fn(); });
        left->addWidget(b);
    };
    card("contact", QStringLiteral("Business Information"), QStringLiteral("Edit the contact details that templates and Business Information fields use."),
         [this] { businessInfoDialog(this, m_win->editor()); });
    card("printer", QStringLiteral("Commercial Print Settings"), QStringLiteral("Choose the color model (RGB, process CMYK, spot colors) and font embedding for a print shop."),
         [this] { documentPropertiesDialog(this, m_win->editor(), 1); });
    card("shield-check", QStringLiteral("Run Design Checker"), QStringLiteral("Find problems such as text that doesn't fit, empty frames and low-resolution pictures."),
         [this] { m_win->showTaskPane("designchecker"); Q_EMIT closeRequested(); });
    left->addStretch(1);
    body->addLayout(left, 1);

    // Details card: first-page preview and the publication's facts.
    int pics = 0, words = 0;
    d->forEachItem([&](Item *it, int, const QString &) {
        if (it->type() == ItemType::Picture) ++pics;
    });
    for (auto it = d->stories.cbegin(); it != d->stories.cend(); ++it)
        words += (*it)->doc->toPlainText().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).size();
    auto *details = cardFrame(w);
    details->setFixedWidth(340);
    auto *dv = new QVBoxLayout(details);
    dv->setContentsMargins(18, 18, 18, 18);
    dv->setSpacing(10);
    auto *thumb = new QLabel(details);
    thumb->setFixedSize(304, 210);
    thumb->setAlignment(Qt::AlignCenter);
    {
        const qreal dpr = devicePixelRatioF();
        QImage img = m_win->pageThumbnail(std::clamp(ed->currentPage(), 0, int(d->pages.size()) - 1), int(200 * dpr));
        img.setDevicePixelRatio(dpr);
        thumb->setPixmap(paperPixmap(img.convertToFormat(QImage::Format_ARGB32_Premultiplied), thumb->size(), dpr));
    }
    dv->addWidget(thumb, 0, Qt::AlignHCenter);
    auto *dh = new QLabel(QStringLiteral("Details"), details);
    QFont df = dh->font();
    df.setWeight(QFont::DemiBold);
    df.setPointSizeF(df.pointSizeF() * 1.1);
    dh->setFont(df);
    dv->addWidget(dh);
    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(7);
    const QList<QPair<QString, QString>> facts = {
        {QStringLiteral("Pages"), QString::number(d->pages.size())},
        {QStringLiteral("Page size"), QStringLiteral("%1 × %2").arg(Settings::get().format(d->pageSize().width()), Settings::get().format(d->pageSize().height()))},
        {QStringLiteral("Pictures"), QString::number(pics)},
        {QStringLiteral("Words"), QLocale().toString(words)},
        {QStringLiteral("Author"), d->props.author.isEmpty() ? Settings::get().userName() : d->props.author},
        {QStringLiteral("Created"), QLocale().toString(d->props.created, QStringLiteral("MMM d, yyyy h:mm AP"))},
        {QStringLiteral("Modified"), QLocale().toString(d->props.modified, QStringLiteral("MMM d, yyyy h:mm AP"))},
    };
    for (int i = 0; i < facts.size(); ++i) {
        grid->addWidget(mutedLabel(facts[i].first, details), i, 0, Qt::AlignLeft | Qt::AlignTop);
        auto *val = new QLabel(facts[i].second, details);
        val->setWordWrap(true);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(val, i, 1);
    }
    grid->setColumnStretch(1, 1);
    dv->addLayout(grid);
    dv->addSpacing(4);
    auto *more = new QPushButton(QStringLiteral("Edit Properties…"), details);
    more->setCursor(Qt::PointingHandCursor);
    more->setMinimumHeight(34);
    connect(more, &QPushButton::clicked, this, [this] { documentPropertiesDialog(this, m_win->editor()); });
    dv->addWidget(more);
    body->addWidget(details, 0, Qt::AlignTop);
    v->addLayout(body, 1);
    return w;
}

// ---------------- New ----------------
QWidget *Backstage::buildNew()
{
    auto *w = new QWidget();
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(40, 30, 40, 30);
    v->addWidget(heading(QStringLiteral("New Publication"), w));
    auto *search = new QLineEdit(w);
    search->setPlaceholderText(QStringLiteral("Search templates"));
    search->setClearButtonEnabled(true);
    search->addAction(icon("search"), QLineEdit::LeadingPosition);
    search->setMinimumHeight(34);
    v->addWidget(search);
    v->addSpacing(8);

    // The template categories, always in view down the left side, each
    // with how many templates it has.
    auto *cats = new QListWidget(w);
    {
        const QColor a = uiAccent();
        cats->setStyleSheet(QStringLiteral("QListWidget{background:%1; border:1px solid %2; border-radius:12px; padding:8px; outline:0;}"
                                           "QListWidget::item{padding:5px 10px; border-radius:8px; color:%4;}"
                                           "QListWidget::item:hover{background:%3;}"
                                           "QListWidget::item:selected{background:rgba(%5,%6,%7,48); color:%4; font-weight:bold;}")
                                .arg(dark() ? "#22262d" : "#ffffff", dark() ? "#2f343d" : "#e3e5ea", dark() ? "#2c313a" : "#f3f4f6", uiText().name())
                                .arg(a.red())
                                .arg(a.green())
                                .arg(a.blue()));
        auto add = [cats](const QString &key, const QString &text) {
            auto *it = new QListWidgetItem(text, cats);
            it->setData(Qt::UserRole, key);
        };
        add(QStringLiteral("Featured"), QStringLiteral("Featured"));
        for (const QString &c : templateCategories()) {
            int n = 0;
            for (const auto &t : templates()) n += t.category == c;
            add(c, QStringLiteral("%1  (%2)").arg(c).arg(n));
        }
        add(QStringLiteral("Blank Sizes"), QStringLiteral("Blank Sizes"));
        add(QStringLiteral("My Templates"), QStringLiteral("My Templates"));
        QFont cf = cats->font();
        cf.setPointSizeF(cf.pointSizeF() * 1.05);
        cats->setFont(cf);
        cats->setFixedWidth(240);
        cats->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        cats->setTextElideMode(Qt::ElideRight);
        cats->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        cats->setCurrentRow(0);
    }

    auto *body = new QHBoxLayout();
    body->setSpacing(20);
    {
        auto *catCol = new QVBoxLayout();
        catCol->setSpacing(6);
        auto *catTitle = new QLabel(QStringLiteral("Categories"), w);
        QFont ct = catTitle->font();
        ct.setBold(true);
        catTitle->setFont(ct);
        catCol->addWidget(catTitle);
        catCol->addWidget(cats, 1);
        body->addLayout(catCol);
    }
    auto *left = new QVBoxLayout();
    auto *list = new QListWidget(w);
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(150, 150));
    list->setGridSize(QSize(190, 214));
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setWordWrap(true);
    list->setSpacing(0);
    list->setUniformItemSizes(true);
    list->setMouseTracking(true);
    list->setFrameShape(QFrame::NoFrame);
    list->setStyleSheet(QStringLiteral("QListWidget{background:transparent; border:none; outline:0;}"));
    list->setItemDelegate(new CardDelegate(list));
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setMinimumHeight(520);
    left->addWidget(list, 1);
    body->addLayout(left, 1);

    // Customize panel, drawn as a card.
    auto *panel = new QFrame(w);
    panel->setObjectName(QStringLiteral("jpCard"));
    panel->setStyleSheet(dark() ? QStringLiteral("#jpCard{background:#22262d; border:1px solid #2f343d; border-radius:12px;}")
                                : QStringLiteral("#jpCard{background:#ffffff; border:1px solid #e3e5ea; border-radius:12px;}"));
    panel->setFixedWidth(320);
    auto *pv = new QVBoxLayout(panel);
    pv->setContentsMargins(18, 18, 18, 18);
    pv->setSpacing(8);
    auto *preview = new QLabel(panel);
    preview->setFixedSize(284, 260);
    preview->setAlignment(Qt::AlignCenter);
    auto *title = new QLabel(panel);
    title->setWordWrap(true);
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() * 1.2);
    title->setFont(tf);
    auto *desc = new QLabel(panel);
    desc->setWordWrap(true);
    {
        QPalette dp = desc->palette();
        QColor c = uiText();
        c.setAlphaF(0.7f);
        dp.setColor(QPalette::WindowText, c);
        desc->setPalette(dp);
    }
    auto *form = new QFormLayout();
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    form->setVerticalSpacing(6);
    auto *scheme = new QComboBox(panel);
    scheme->addItem(QStringLiteral("(template default)"));
    for (const auto &s : builtinColorSchemes()) scheme->addItem(s.name);
    auto *fonts = new QComboBox(panel);
    fonts->addItem(QStringLiteral("(template default)"));
    for (const auto &s : builtinFontSchemes()) fonts->addItem(s.name);
    auto *bizBox = new QComboBox(panel);
    for (const auto &b : m_win->editor()->doc()->biz) bizBox->addItem(b.setName);
    auto *editBiz = new QPushButton(QStringLiteral("Edit…"), panel);
    auto *bizRow = new QHBoxLayout();
    bizRow->addWidget(bizBox, 1);
    bizRow->addWidget(editBiz);
    connect(editBiz, &QPushButton::clicked, this, [this] { businessInfoDialog(this, m_win->editor()); });
    form->addRow(QStringLiteral("Color scheme:"), scheme);
    form->addRow(QStringLiteral("Font scheme:"), fonts);
    form->addRow(QStringLiteral("Business information:"), bizRow);
    auto *optLogo = new QCheckBox(QStringLiteral("Include logo"), panel);
    auto *optAddr = new QCheckBox(QStringLiteral("Include mailing address"), panel);
    optAddr->setChecked(true);
    form->addRow(optLogo);
    form->addRow(optAddr);
    auto *create = new QPushButton(QStringLiteral("Create"), panel);
    create->setProperty("primary", true);
    create->setDefault(true);
    create->setMinimumHeight(38);
    create->setCursor(Qt::PointingHandCursor);
    pv->addWidget(preview, 0, Qt::AlignHCenter);
    pv->addSpacing(4);
    pv->addWidget(title);
    pv->addWidget(desc);
    pv->addSpacing(6);
    pv->addLayout(form);
    pv->addSpacing(8);
    pv->addWidget(create);
    pv->addStretch(1);
    body->addWidget(panel, 0, Qt::AlignTop);
    v->addLayout(body, 1);

    auto options = [=]() {
        TemplateOptions o;
        o.colorScheme = scheme->currentIndex() > 0 ? scheme->currentText() : QString();
        o.fontScheme = fonts->currentIndex() > 0 ? fonts->currentText() : QString();
        const auto &biz = m_win->editor()->doc()->biz;
        o.business = biz.value(bizBox->currentIndex(), BusinessInfo());
        o.options["logo"] = optLogo->isChecked();
        o.options["address"] = optAddr->isChecked();
        // The business logo's picture comes along for templates that place it.
        const Document *cur = m_win->editor()->doc();
        const auto img = cur->images.constFind(o.business.logoImageId);
        if (!o.business.logoImageId.isEmpty() && img != cur->images.cend()) {
            o.logoBytes = img->bytes;
            o.logoFormat = img->format;
        }
        return o;
    };
    const qreal dpr = devicePixelRatioF();
    auto thumbFor = [dpr](Document &doc) {
        LayoutCache cache;
        PaintContext ctx;
        ctx.doc = &doc;
        ctx.cache = &cache;
        ctx.opt.output = true;
        const QSizeF ps = doc.pageSize();
        QImage img = Renderer::renderToImage(ctx, 0, 140.0 * dpr / std::max(ps.width(), ps.height()));
        img.setDevicePixelRatio(dpr);
        return paperPixmap(img, QSize(150, 150), dpr);
    };
    // A category's list appears at once; thumbnails not made yet are drawn
    // one at a time after it (building the twelve months of a calendar takes
    // a moment), and are kept for the session in m_thumbs.
    // One thumbnail per tick of a zero-interval timer, so clicks and typing
    // are answered between them; a newer list (another category, a search, a
    // different scheme) replaces the work left.
    auto pending = std::make_shared<QVector<QPair<QListWidgetItem *, const TemplateInfo *>>>();
    auto pendingOptions = std::make_shared<TemplateOptions>();
    auto *thumbTimer = new QTimer(list);
    thumbTimer->setInterval(0);
    const QIcon placeholder = [&] {
        auto blank = Document::blank(QSizeF(612, 792));
        return QIcon(thumbFor(*blank));
    }();
    auto optionsKey = [](const TemplateOptions &o) {
        return o.colorScheme + QLatin1Char('|') + o.fontScheme + QLatin1Char('|') +
               QString::fromUtf8(QJsonDocument(o.business.toJson()).toJson(QJsonDocument::Compact)) + QLatin1Char('|') +
               QString::fromUtf8(QJsonDocument(o.options).toJson(QJsonDocument::Compact)) + QLatin1Char('|') +
               QString::fromLatin1(QCryptographicHash::hash(o.logoBytes, QCryptographicHash::Md5).toHex());
    };
    connect(thumbTimer, &QTimer::timeout, list, [=]() {
        if (pending->isEmpty()) {
            thumbTimer->stop();
            return;
        }
        const auto [it, t] = pending->takeFirst();
        auto doc = t->build(*pendingOptions);
        const QIcon made(thumbFor(*doc));
        m_thumbs.insert(t->id, made);
        it->setIcon(made);
    });
    auto populate = [=]() {
        thumbTimer->stop();
        pending->clear();   // before the items go
        list->clear();
        const QString cat = cats->currentItem() ? cats->currentItem()->data(Qt::UserRole).toString() : QStringLiteral("Featured");
        const QString q = search->text().trimmed();
        if (cat == "Blank Sizes" && q.isEmpty()) {
            for (const auto &bs : blankSizes()) {
                auto doc = Document::blank(bs.size, bs.name);
                auto *it = new QListWidgetItem(QIcon(thumbFor(*doc)), QStringLiteral("%1\n%2 × %3").arg(bs.name, Settings::get().format(bs.size.width()), Settings::get().format(bs.size.height())));
                it->setData(Qt::UserRole, "blank:" + bs.name);
                list->addItem(it);
            }
            // The user's own sizes, then making a new one.
            const auto own = customPageSizes();
            for (int i = 0; i < own.size(); ++i) {
                auto doc = Document::blank(own[i].second.size, own[i].first);
                doc->setup = own[i].second;
                doc->setup.sizeName = own[i].first;
                auto *it = new QListWidgetItem(QIcon(thumbFor(*doc)), QStringLiteral("%1\n%2 × %3").arg(own[i].first, Settings::get().format(own[i].second.size.width()),
                                                                                                   Settings::get().format(own[i].second.size.height())));
                it->setData(Qt::UserRole, QStringLiteral("customsize:%1").arg(i));
                list->addItem(it);
            }
            auto *custom = new QListWidgetItem(icon("ruler"), QStringLiteral("Create New Page Size…"));
            custom->setData(Qt::UserRole, "custom");
            list->addItem(custom);
            return;
        }
        if (cat == "My Templates") {
            const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Templates";
            for (const QFileInfo &fi : QDir(dir).entryInfoList({"*.jpub"}, QDir::Files)) {
                const QImage th = publicationThumbnail(fi.absoluteFilePath());
                auto *it = new QListWidgetItem(th.isNull() ? icon("file") : QIcon(QPixmap::fromImage(th.scaled(150, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation))), fi.completeBaseName());
                it->setData(Qt::UserRole, "file:" + fi.absoluteFilePath());
                list->addItem(it);
            }
            return;
        }
        if (cat == "Featured" && q.isEmpty()) {
            auto *it = new QListWidgetItem(placeholder, QStringLiteral("Blank 8.5 × 11\""));
            it->setData(Qt::UserRole, "blank:Letter");
            list->addItem(it);
            auto blankL = Document::blank(QSizeF(792, 612));
            it = new QListWidgetItem(QIcon(thumbFor(*blankL)), QStringLiteral("Blank 11 × 8.5\""));
            it->setData(Qt::UserRole, "blank:Letter Landscape");
            list->addItem(it);
        }
        const TemplateOptions o = options();
        if (const QString k = optionsKey(o); k != m_thumbOptions) {
            m_thumbs.clear();
            m_thumbOptions = k;
        }
        for (const auto &t : templates()) {
            if (!q.isEmpty() && !t.name.contains(q, Qt::CaseInsensitive) && !t.category.contains(q, Qt::CaseInsensitive)) continue;
            if (q.isEmpty() && cat != "Featured" && t.category != cat) continue;
            if (q.isEmpty() && cat == "Featured" && list->count() >= 14) continue;
            const auto known = m_thumbs.constFind(t.id);
            auto *it = new QListWidgetItem(known != m_thumbs.cend() ? *known : placeholder, t.name);
            it->setData(Qt::UserRole, "tpl:" + t.id);
            it->setToolTip(t.description);
            list->addItem(it);
            if (known == m_thumbs.cend()) *pending << qMakePair(it, &t);
        }
        if (!pending->isEmpty()) {
            *pendingOptions = o;
            thumbTimer->start();
        }
    };
    connect(cats, &QListWidget::currentRowChanged, this, [populate] { populate(); });
    connect(search, &QLineEdit::textChanged, this, [populate] { populate(); });
    connect(scheme, &QComboBox::currentIndexChanged, this, [populate] { populate(); });
    connect(fonts, &QComboBox::currentIndexChanged, this, [populate] { populate(); });
    auto build = [=](const QString &key) -> std::unique_ptr<Document> {
        if (key.startsWith("tpl:")) {
            const TemplateInfo *t = findTemplate(key.mid(4));
            return t ? t->build(options()) : nullptr;
        }
        if (key.startsWith("blank:")) {
            const QString name = key.mid(6);
            if (name == "Letter Landscape") return Document::blank(QSizeF(792, 612));
            for (const auto &bs : blankSizes()) if (bs.name == name) {
                auto d = Document::blank(bs.size, bs.name);
                TemplateOptions o = options();
                if (const ColorScheme *cs = findColorScheme(o.colorScheme)) d->colors = *cs;
                if (const FontScheme *fs = findFontScheme(o.fontScheme)) d->fonts = *fs;
                d->biz = {o.business};
                return d;
            }
            return Document::blank(QSizeF(612, 792));
        }
        if (key.startsWith("customsize:")) {
            const auto own = customPageSizes();
            const int i = key.mid(11).toInt();
            if (i < 0 || i >= own.size()) return nullptr;
            auto d = Document::blank(own[i].second.size, own[i].first);
            d->setup = own[i].second;
            d->setup.sizeName = own[i].first;
            TemplateOptions o = options();
            if (const ColorScheme *cs = findColorScheme(o.colorScheme)) d->colors = *cs;
            if (const FontScheme *fs = findFontScheme(o.fontScheme)) d->fonts = *fs;
            d->biz = {o.business};
            return d;
        }
        if (key.startsWith("file:")) {
            QString err;
            auto d = loadPublication(key.mid(5), &err);
            return d;
        }
        return nullptr;
    };
    connect(list, &QListWidget::currentItemChanged, this, [=](QListWidgetItem *it) {
        if (!it) return;
        const QString key = it->data(Qt::UserRole).toString();
        title->setText(it->text().section('\n', 0, 0));
        const TemplateInfo *t = key.startsWith("tpl:") ? findTemplate(key.mid(4)) : nullptr;
        desc->setText(t ? t->description : QString());
        // Only the options this template has.
        optLogo->setVisible(t && t->optionKeys.contains(QStringLiteral("logo")));
        optAddr->setVisible(t && t->optionKeys.contains(QStringLiteral("address")));
        if (key == "custom") { preview->clear(); return; }
        auto doc = build(key);
        if (!doc) return;
        LayoutCache cache;
        PaintContext ctx;
        ctx.doc = doc.get();
        ctx.cache = &cache;
        ctx.opt.output = true;
        const QSizeF ps = doc->pageSize();
        QImage img = Renderer::renderToImage(ctx, 0, 250.0 * dpr / std::max(ps.width(), ps.height()));
        img.setDevicePixelRatio(dpr);
        preview->setPixmap(paperPixmap(img, preview->size(), dpr));
    });
    // Changing an option redraws the preview.
    for (QCheckBox *cb : {optLogo, optAddr})
        connect(cb, &QCheckBox::toggled, this, [=] {
            if (QListWidgetItem *it = list->currentItem()) Q_EMIT list->currentItemChanged(it, it);
        });
    auto doCreate = [=] {
        QListWidgetItem *it = list->currentItem();
        if (!it) return;
        const QString key = it->data(Qt::UserRole).toString();
        if (key == "custom") {
            // Save a new size, then select it in the list.
            if (createPageSizeDialog(this, m_win->editor(), -1, false)) {
                populate();
                const QString want = QStringLiteral("customsize:%1").arg(customPageSizes().size() - 1);
                for (int r = 0; r < list->count(); ++r)
                    if (list->item(r)->data(Qt::UserRole).toString() == want) list->setCurrentRow(r);
            }
            return;
        }
        if (!m_win->maybeSave()) return;
        auto doc = build(key);
        if (doc) {
            if (key.startsWith("tpl:")) doc->templateId = key.mid(4);
            m_win->newPublication(std::move(doc));
        }
    };
    connect(create, &QPushButton::clicked, this, doCreate);
    connect(list, &QListWidget::itemDoubleClicked, this, [doCreate] { doCreate(); });
    populate();
    if (list->count()) list->setCurrentRow(0);
    return w;
}

// ---------------- Open ----------------
QWidget *Backstage::buildOpen()
{
    auto *w = new QWidget();
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(40, 30, 40, 30);
    v->addWidget(heading(QStringLiteral("Open"), w));
    auto *cards = new QHBoxLayout();
    cards->setSpacing(12);
    auto *browse = new ActionCard(QStringLiteral("folder-open"), QStringLiteral("Browse"), QStringLiteral("Open a JeffPub publication or a .pub file."), w);
    connect(browse, &QAbstractButton::clicked, this, [this] { m_win->act("file.open")->trigger(); });
    auto *recover = new ActionCard(QStringLiteral("life-buoy"), QStringLiteral("Recover Unsaved Work"), QStringLiteral("Open a copy JeffPub saved automatically before a crash or a close without saving."), w);
    connect(recover, &QAbstractButton::clicked, this, [this] {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/AutoRecover";
        const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Recover"), dir, QStringLiteral("JeffPub Publications (*.jpub)"));
        if (!p.isEmpty() && m_win->maybeSave()) m_win->openFile(p);
    });
    cards->addWidget(browse, 1);
    cards->addWidget(recover, 1);
    v->addLayout(cards);
    v->addSpacing(14);
    auto section = [&](const QString &t) {
        QLabel *l = sectionLabel(t, w);
        l->setContentsMargins(2, 12, 0, 2);
        v->addWidget(l);
    };
    const QStringList pinned = Settings::get().pinnedFiles();
    auto addList = [&](const QStringList &files, bool isPinned) {
        auto *list = new QListWidget(w);
        list->setIconSize(QSize(44, 44));
        list->setContextMenuPolicy(Qt::CustomContextMenu);
        list->setMouseTracking(true);
        list->setMaximumWidth(820);
        list->setStyleSheet(QStringLiteral("QListWidget{background:%1; border:1px solid %2; border-radius:10px; padding:6px; outline:0;}"
                                           "QListWidget::item{padding:6px 8px; border-radius:7px; color:%4;}"
                                           "QListWidget::item:hover{background:%3;}"
                                           "QListWidget::item:selected{background:%3; color:%4;}")
                                .arg(dark() ? "#22262d" : "#ffffff", dark() ? "#2f343d" : "#e3e5ea", dark() ? "#2c313a" : "#f3f4f6", uiText().name()));
        for (const QString &f : files) {
            if (!isPinned && pinned.contains(f)) continue;
            const QFileInfo fi(f);
            QIcon ic = icon(f.endsWith(".pub", Qt::CaseInsensitive) ? "file-type" : "file");
            if (f.endsWith(".jpub", Qt::CaseInsensitive)) {
                const QImage th = publicationThumbnail(f);
                if (!th.isNull()) ic = QIcon(QPixmap::fromImage(th.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            }
            auto *it = new QListWidgetItem(ic, QStringLiteral("%1\n%2").arg(fi.fileName(), fi.absolutePath()));
            it->setData(Qt::UserRole, f);
            if (!fi.exists()) it->setForeground(QColor(150, 150, 150));
            list->addItem(it);
        }
        if (list->count() == 0) {
            delete list;
            auto *empty = new QLabel(QStringLiteral("Publications you open or save will appear here."), w);
            QPalette ep = empty->palette();
            QColor c = uiText();
            c.setAlphaF(0.6f);
            ep.setColor(QPalette::WindowText, c);
            empty->setPalette(ep);
            empty->setContentsMargins(2, 4, 0, 0);
            v->addWidget(empty);
            return;
        }
        list->setMinimumHeight(std::min(440, 58 * list->count() + 16));
        connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
            if (m_win->maybeSave()) m_win->openFile(it->data(Qt::UserRole).toString());
        });
        connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
            if (m_win->maybeSave()) m_win->openFile(it->data(Qt::UserRole).toString());
        });
        connect(list, &QListWidget::customContextMenuRequested, this, [this, list, isPinned](const QPoint &pos) {
            QListWidgetItem *it = list->itemAt(pos);
            if (!it) return;
            const QString f = it->data(Qt::UserRole).toString();
            QMenu m;
            m.addAction(isPinned ? QStringLiteral("Unpin from list") : QStringLiteral("Pin to list"), [this, f, isPinned] {
                Settings::get().setPinned(f, !isPinned);
                showPage("open");
            });
            m.addAction(QStringLiteral("Remove from list"), [this, f] { Settings::get().removeRecentFile(f); showPage("open"); });
            m.exec(list->viewport()->mapToGlobal(pos));
        });
        v->addWidget(list);
    };
    if (!pinned.isEmpty()) {
        section(QStringLiteral("Pinned"));
        addList(pinned, true);
    }
    section(QStringLiteral("Recent Publications"));
    addList(Settings::get().recentFiles(), false);
    v->addStretch(1);
    return w;
}

// ---------------- Print ----------------
QWidget *Backstage::buildPrint()
{
    Editor *ed = m_win->editor();
    Document *d = ed->doc();
    auto *w = new QWidget();
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(40, 30, 40, 30);
    auto *left = new QVBoxLayout();
    left->addWidget(heading(QStringLiteral("Print"), w));
    auto *copies = new QSpinBox(w);
    copies->setRange(1, 999);
    auto *printBtn = new QPushButton(QStringLiteral("Print"), w);
    printBtn->setProperty("primary", true);
    printBtn->setMinimumHeight(40);
    printBtn->setMinimumWidth(120);
    printBtn->setCursor(Qt::PointingHandCursor);
    auto *top = new QHBoxLayout();
    top->addWidget(printBtn);
    top->addWidget(new QLabel(QStringLiteral("Copies:"), w));
    top->addWidget(copies);
    left->addLayout(top);
    auto *form = new QFormLayout();
    auto *printer = new QComboBox(w);
    for (const QPrinterInfo &pi : QPrinterInfo::availablePrinters()) printer->addItem(pi.printerName());
    printer->addItem(QStringLiteral("Save as PDF"));
    if (!QPrinterInfo::defaultPrinter().isNull()) printer->setCurrentText(QPrinterInfo::defaultPrinter().printerName());
    auto *range = new QComboBox(w);
    range->addItems({"Print All Pages", "Print Current Page", "Custom Print"});
    auto *pages = new QLineEdit(w);
    pages->setPlaceholderText(QStringLiteral("e.g. 1-3, 5"));
    auto *layout = new QComboBox(w);
    layout->addItems({"One page per sheet", "Multiple pages per sheet", "Multiple copies per sheet", "Booklet, side-fold", "Booklet, top-fold", "Tiled (posters and banners)"});
    if (d->setup.layout == PageSetup::Booklet) layout->setCurrentIndex(3);
    // Business cards and labels: copies of the page across the sheet.
    else if (d->setup.layout == PageSetup::MultiplePerSheet || d->setup.layout == PageSetup::Labels) layout->setCurrentIndex(2);
    auto *paper = new QComboBox(w);
    paper->addItems({"Letter", "Legal", "Tabloid", "A4", "A3", "Same as publication"});
    auto *sides = new QComboBox(w);
    sides->addItems({"One-sided", "Two-sided, flip on long edge", "Two-sided, flip on short edge"});
    auto *color = new QComboBox(w);
    color->addItems({"Composite RGB", "Composite grayscale", "Separations (CMYK plates)"});
    // Printer's marks: they print outside the page, so they need paper larger
    // than the publication.
    auto *marksBox = new QWidget(w);
    auto *mg = new QGridLayout(marksBox);
    mg->setContentsMargins(0, 0, 0, 0);
    mg->setHorizontalSpacing(16);
    auto *marks = new QCheckBox(QStringLiteral("Crop marks"), w);
    auto *bleedMarks = new QCheckBox(QStringLiteral("Bleed marks"), w);
    auto *registration = new QCheckBox(QStringLiteral("Registration marks"), w);
    auto *density = new QCheckBox(QStringLiteral("Density bars"), w);
    auto *colorBars = new QCheckBox(QStringLiteral("Color bars"), w);
    auto *jobInfo = new QCheckBox(QStringLiteral("Job information"), w);
    auto *allowBleeds = new QCheckBox(QStringLiteral("Allow bleeds"), w);
    allowBleeds->setToolTip(QStringLiteral("Print objects that run off the page up to the bleed edge, so the page can be trimmed without white edges."));
    mg->addWidget(marks, 0, 0);
    mg->addWidget(bleedMarks, 0, 1);
    mg->addWidget(registration, 1, 0);
    mg->addWidget(density, 1, 1);
    mg->addWidget(colorBars, 2, 0);
    mg->addWidget(jobInfo, 2, 1);
    mg->addWidget(allowBleeds, 3, 0);
    auto *plates = new QWidget(w);
    auto *pl = new QHBoxLayout(plates);
    pl->setContentsMargins(0, 0, 0, 0);
    QList<QCheckBox *> plateBoxes;
    for (const QString &n : {QStringLiteral("Cyan"), QStringLiteral("Magenta"), QStringLiteral("Yellow"), QStringLiteral("Black")}) {
        auto *b = new QCheckBox(n, plates);
        b->setChecked(true);
        pl->addWidget(b);
        plateBoxes << b;
    }
    // A plate for each spot color.
    if (d->print.usesSpots())
        for (int i = 0; i < std::min<qsizetype>(10, d->print.spotColors.size()); ++i) {
            auto *b = new QCheckBox(d->print.spotName(i), plates);
            b->setChecked(true);
            pl->addWidget(b);
            plateBoxes << b;
        }
    pl->addStretch(1);
    plates->setVisible(false);
    auto *merged = new QCheckBox(QStringLiteral("Print all mail merge records"), w);
    merged->setEnabled(!d->merge.isEmpty());
    form->addRow(QStringLiteral("Printer:"), printer);
    form->addRow(QStringLiteral("Settings:"), range);
    form->addRow(QStringLiteral("Pages:"), pages);
    form->addRow(QStringLiteral("Layout:"), layout);
    form->addRow(QStringLiteral("Paper:"), paper);
    form->addRow(QStringLiteral("Sides:"), sides);
    form->addRow(QStringLiteral("Color:"), color);
    form->addRow(QStringLiteral("Plates:"), plates);
    form->addRow(QStringLiteral("Marks:"), marksBox);
    form->addRow(merged);
    left->addLayout(form);
    auto *props = new QPushButton(QStringLiteral("Printer Properties…"), w);
    left->addWidget(props, 0, Qt::AlignLeft);
    left->addStretch(1);
    h->addLayout(left, 1);

    // Preview with page navigation.
    auto *right = new QVBoxLayout();
    auto *preview = new QLabel(w);
    preview->setMinimumSize(420, 520);
    preview->setAlignment(Qt::AlignCenter);
    auto *nav = new QHBoxLayout();
    auto *prev = new QToolButton(w);
    prev->setIcon(icon("chevron-left"));
    auto *next = new QToolButton(w);
    next->setIcon(icon("chevron-right"));
    auto *pageLabel = new QLabel(w);
    nav->addStretch(1);
    nav->addWidget(prev);
    nav->addWidget(pageLabel);
    nav->addWidget(next);
    nav->addStretch(1);
    right->addWidget(preview, 1);
    right->addLayout(nav);
    auto *showRulers = new QCheckBox(QStringLiteral("Show rulers"), w);
    auto *showNumbers = new QCheckBox(QStringLiteral("Show page numbers"), w);
    auto *viewRow = new QHBoxLayout();
    viewRow->addStretch(1);
    viewRow->addWidget(showRulers);
    viewRow->addWidget(showNumbers);
    viewRow->addStretch(1);
    right->addLayout(viewRow);
    h->addLayout(right, 2);
    auto *state = new int(0);
    connect(w, &QObject::destroyed, [state] { delete state; });
    auto render = [=]() {
        const bool booklet = layout->currentIndex() == 3 || layout->currentIndex() == 4;
        const bool topFold = layout->currentIndex() == 4;
        QImage img;
        PaintContext ctx;
        ctx.doc = d;
        ctx.cache = &ed->cache();
        ctx.opt.output = true;
        ctx.opt.mergeRecord = ed->mergeRecord();
        const QSizeF ps = d->pageSize();
        double scale = 1;                        // preview pixels per point
        QVector<QPair<QRectF, int>> numbered;    // page areas on the preview and their numbers
        if (booklet) {
            const auto order = bookletOrder(d->pages.size());
            *state = std::clamp(*state, 0, int(order.size()) - 1);
            const auto side = order[*state];
            const double s = 240.0 / std::max(ps.width(), ps.height());
            const QSizeF sheetSz = topFold ? QSizeF(ps.width(), ps.height() * 2) : QSizeF(ps.width() * 2, ps.height());
            img = QImage((sheetSz * s).toSize(), QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::white);
            QPainter p(&img);
            for (int k = 0; k < 2; ++k) {
                if (side[k] < 0) continue;
                const QImage pg = Renderer::renderToImage(ctx, side[k], s);
                const QPointF at = topFold ? QPointF(0, k * ps.height() * s) : QPointF(k * ps.width() * s, 0);
                p.drawImage(at, pg);
                numbered << qMakePair(QRectF(at, ps * s), side[k] + 1);
            }
            scale = s;
            p.setPen(QPen(QColor(150, 150, 150), 1, Qt::DashLine));
            if (topFold) p.drawLine(0, img.height() / 2, img.width(), img.height() / 2);
            else p.drawLine(img.width() / 2, 0, img.width() / 2, img.height());
            pageLabel->setText(QStringLiteral("Sheet %1 %2 of %3").arg(*state / 2 + 1).arg(*state % 2 ? "(back)" : "(front)").arg(order.size() / 2));
        } else {
            *state = std::clamp(*state, 0, int(d->pages.size()) - 1);
            scale = 500.0 / std::max(ps.width(), ps.height());
            img = Renderer::renderToImage(ctx, *state, scale);
            numbered << qMakePair(QRectF(QPointF(0, 0), ps * scale), *state + 1);
            // Two-sided printing: say which side of which sheet this page lands on.
            if (sides->currentIndex() > 0 && layout->currentIndex() == 0)
                pageLabel->setText(QStringLiteral("%1 of %2 (sheet %3, %4)").arg(*state + 1).arg(d->pages.size()).arg(*state / 2 + 1)
                                       .arg(*state % 2 ? QStringLiteral("back") : QStringLiteral("front")));
            else
                pageLabel->setText(QStringLiteral("%1 of %2").arg(*state + 1).arg(d->pages.size()));
        }
        if (color->currentIndex() == 1) img = img.convertToFormat(QImage::Format_Grayscale8);
        if (color->currentIndex() == 2) {
            // Preview the first chosen plate.
            for (int i = 0; i < 4; ++i)
                if (plateBoxes[i]->isChecked()) { img = separationPlate(img, i); pageLabel->setText(pageLabel->text() + QStringLiteral(" · ") + plateName(i)); break; }
        }
        img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (showNumbers->isChecked()) {
            // Each page's number in a badge at its center.
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            QFont f = p.font();
            f.setPixelSize(std::max(12, img.height() / 14));
            f.setBold(true);
            p.setFont(f);
            for (const auto &[r, n] : numbered) {
                const QString t = QString::number(n);
                const double side = QFontMetricsF(f).horizontalAdvance(t) + f.pixelSize();
                const QRectF badge(r.center() - QPointF(side / 2, f.pixelSize() * 0.8), QSizeF(side, f.pixelSize() * 1.6));
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(40, 40, 40, 170));
                p.drawRoundedRect(badge, 6, 6);
                p.setPen(Qt::white);
                p.drawText(badge, Qt::AlignCenter, t);
            }
        }
        if (showRulers->isChecked()) {
            // Rulers along the top and left in the user's units.
            const int band = 18;
            QImage framed(img.size() + QSize(band, band), QImage::Format_ARGB32_Premultiplied);
            framed.fill(QColor(236, 238, 241));
            QPainter p(&framed);
            p.drawImage(band, band, img);
            QFont f = p.font();
            f.setPixelSize(9);
            p.setFont(f);
            p.setPen(QColor(90, 90, 90));
            const double unitPt = Settings::get().fromUnit(1.0);   // points in one unit
            double step = unitPt;                                  // a labeled tick at least 40 px apart
            while (step * scale < 40) step *= 2;
            auto ticks = [&](bool across, double lengthPt) {
                for (double v = 0; v <= lengthPt + 0.01; v += step / 4) {
                    const double px = band + v * scale;
                    const bool major = std::fmod(v + 0.001, step) < 0.01;
                    const int len = major ? 8 : 4;
                    if (across) p.drawLine(QPointF(px, band - len), QPointF(px, band));
                    else p.drawLine(QPointF(band - len, px), QPointF(band, px));
                    if (major && v > 0) {
                        const QString label = QString::number(std::round(Settings::get().toUnit(v) * 100) / 100);
                        if (across) p.drawText(QPointF(px + 2, 9), label);
                        else p.drawText(QPointF(1, px - 2), label);
                    }
                }
            };
            ticks(true, img.width() / scale);
            ticks(false, img.height() / scale);
            p.end();
            img = framed;
        }
        preview->setPixmap(paperPixmap(img, preview->size(), preview->devicePixelRatioF()));
    };
    for (QCheckBox *b : {showRulers, showNumbers}) connect(b, &QCheckBox::toggled, w, [=] { render(); });
    connect(prev, &QToolButton::clicked, w, [=] { --*state; render(); });
    connect(next, &QToolButton::clicked, w, [=] { ++*state; render(); });
    connect(layout, &QComboBox::currentIndexChanged, w, [=] { *state = 0; render(); });
    connect(color, &QComboBox::currentIndexChanged, w, [=] { plates->setVisible(color->currentIndex() == 2); render(); });
    connect(sides, &QComboBox::currentIndexChanged, w, [=] { render(); });
    for (QCheckBox *b : plateBoxes) connect(b, &QCheckBox::toggled, w, [=] { render(); });
    QTimer::singleShot(0, w, render);
    auto setup = [=](QPrinter &p) {
        if (printer->currentText() == QLatin1String("Save as PDF")) {
            const QString f = askSavePath(this, QStringLiteral("Save as PDF"), m_win->editor()->displayName() + ".pdf", QStringLiteral("PDF (*.pdf)"));
            if (f.isEmpty()) return false;
            p.setOutputFormat(QPrinter::PdfFormat);
            p.setOutputFileName(f);
        } else {
            p.setPrinterName(printer->currentText());
        }
        p.setCopyCount(copies->value());
        const QString pp = paper->currentText();
        if (pp == "Same as publication") p.setPageSize(QPageSize(d->pageSize(), QPageSize::Point));
        else p.setPageSize(QPageSize(pp == "Letter" ? QPageSize::Letter : pp == "Legal" ? QPageSize::Legal : pp == "Tabloid" ? QPageSize::Tabloid : pp == "A4" ? QPageSize::A4 : QPageSize::A3));
        const bool wide = d->pageSize().width() > d->pageSize().height();
        p.setPageOrientation(layout->currentIndex() == 3 ? QPageLayout::Landscape : layout->currentIndex() == 4 ? QPageLayout::Portrait
                             : wide ? QPageLayout::Landscape : QPageLayout::Portrait);
        p.setDuplex(sides->currentIndex() == 0 ? QPrinter::DuplexNone : sides->currentIndex() == 1 ? QPrinter::DuplexLongSide : QPrinter::DuplexShortSide);
        p.setColorMode(color->currentIndex() == 1 ? QPrinter::GrayScale : QPrinter::Color);
        p.setFullPage(true);
        if (range->currentIndex() == 1) p.setPrintRange(QPrinter::CurrentPage);
        else if (range->currentIndex() == 2) {
            const QString t = pages->text();
            const int a = t.section('-', 0, 0).trimmed().toInt(), b = t.contains('-') ? t.section('-', 1, 1).trimmed().toInt() : a;
            if (a > 0) { p.setPrintRange(QPrinter::PageRange); p.setFromTo(a, std::max(a, b)); }
        }
        return true;
    };
    connect(props, &QPushButton::clicked, this, [=] {
        QPrinter p(QPrinter::HighResolution);
        if (!setup(p)) return;
        QPrintDialog dlg(&p, this);
        dlg.exec();
    });
    connect(printBtn, &QPushButton::clicked, this, [=] {
        QPrinter p(QPrinter::HighResolution);
        if (!setup(p)) return;
        QJsonObject opts;
        const int li = layout->currentIndex();
        opts["layout"] = li == 0 ? "one" : li == 1 ? "multiple" : li == 2 ? "multiple" : li == 3 ? "booklet" : li == 4 ? "bookletTop" : "tiled";
        opts["copiesPerSheet"] = li == 2;
        opts["cropMarks"] = marks->isChecked();
        opts["bleedMarks"] = bleedMarks->isChecked();
        opts["registration"] = registration->isChecked();
        opts["densityBars"] = density->isChecked();
        opts["colorBars"] = colorBars->isChecked();
        opts["jobInfo"] = jobInfo->isChecked();
        opts["allowBleeds"] = allowBleeds->isChecked();
        opts["grayscale"] = color->currentIndex() == 1;
        opts["separations"] = color->currentIndex() == 2;
        QString want;
        for (int i = 0; i < plateBoxes.size(); ++i)
            if (plateBoxes[i]->isChecked()) want += i < 4 ? QChar("CMYK"[i]) : QChar('0' + (i - 4));
        opts["plates"] = want;
        opts["merged"] = merged->isChecked();
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        printDocument(m_win->editor(), &p, opts);
        QGuiApplication::restoreOverrideCursor();
        Q_EMIT closeRequested();
    });
    return w;
}

// ---------------- Share / Export ----------------
QWidget *Backstage::buildShare()
{
    auto *w = new QWidget();
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(40, 30, 40, 30);
    v->addWidget(heading(QStringLiteral("Share"), w));
    v->setSpacing(10);
    auto row = [&](const QString &ic, const QString &title, const QString &text, const std::function<void()> &fn) {
        auto *b = new ActionCard(ic, title, text, w);
        connect(b, &QAbstractButton::clicked, this, [fn] { fn(); });
        v->addWidget(b);
    };
    row("mail", QStringLiteral("Email Current Page"), QStringLiteral("Creates an email (.eml) with the current page as the message body. Open it in your email program to send."), [this] {
        emailCurrentPage(this, m_win->editor());
    });
    row("paperclip", QStringLiteral("Send as Attachment"), QStringLiteral("Creates an email (.eml) with this publication attached."), [this] {
        emailAsAttachment(this, m_win->editor(), "jpub");
    });
    row("file-text", QStringLiteral("Send as PDF"), QStringLiteral("Creates an email (.eml) with a PDF of this publication attached."), [this] {
        emailAsAttachment(this, m_win->editor(), "pdf");
    });
    v->addStretch(1);
    return w;
}

QWidget *Backstage::buildExport()
{
    auto *w = new QWidget();
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(40, 30, 40, 30);
    v->addWidget(heading(QStringLiteral("Export"), w));
    v->setSpacing(10);
    auto row = [&](const QString &ic, const QString &title, const QString &text, const std::function<void()> &fn) {
        auto *b = new ActionCard(ic, title, text, w);
        connect(b, &QAbstractButton::clicked, this, [fn] { fn(); });
        v->addWidget(b);
    };
    row("file-text", QStringLiteral("Create PDF"), QStringLiteral("Preserves layout and fonts; text stays selectable. Choose the quality, from small files for email to commercial press."), [this] { m_win->exportPdfWithOptions(); });
    row("archive", QStringLiteral("Create PDF/A for Archiving"), QStringLiteral("An ISO 19005 (PDF/A-1b) file for long-term storage and court or records filing: every font embedded, standard sRGB color, no transparency."),
        [this] { m_win->exportPdf(QString(), false, true); });
    row("image-down", QStringLiteral("Save as Picture"), QStringLiteral("PNG, JPEG, GIF, TIFF or BMP at the resolution you choose, one file per page."), [this] { m_win->exportImages(); });
    row("globe", QStringLiteral("Save as Web Page"), QStringLiteral("A single HTML file you can open in any browser."), [this] { m_win->exportHtml(); });
    row("file-output", QStringLiteral("Save as .pub File"), QStringLiteral("Saves a .pub file for people who work with .pub publications."), [this] { m_win->saveAs("pub"); });
    row("package", QStringLiteral("Pack and Go: Save for a Commercial Printer"), QStringLiteral("A high-quality PDF with crop marks plus the publication file in one folder."), [this] {
        packAndGo(this, m_win, true);
    });
    row("hard-drive", QStringLiteral("Pack and Go: Save for Another Computer"), QStringLiteral("The publication, its pictures and the fonts it uses, bundled in one ZIP file."), [this] {
        packAndGo(this, m_win, false);
    });
    row("camera", QStringLiteral("Save for a Photo Printer"), QStringLiteral("Saves each page as a 300 dpi JPEG for photo printing services."), [this] {
        saveForPhotoPrinter(this, m_win);
    });
    row("layout-template", QStringLiteral("Save as Template"), QStringLiteral("Adds this publication to My Templates in File > New."), [this] {
        saveAsTemplate(this, m_win);
    });
    v->addStretch(1);
    return w;
}

QWidget *Backstage::buildAbout()
{
    auto *w = new QWidget();
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(40, 30, 40, 30);
    v->setSpacing(14);
    v->addWidget(heading(QStringLiteral("About"), w));

    // Identity card: badge, name, version and the one-line description.
    auto *id = cardFrame(w);
    id->setMaximumWidth(820);
    auto *ih = new QHBoxLayout(id);
    ih->setContentsMargins(20, 18, 20, 18);
    ih->setSpacing(16);
    auto *badge = new QLabel(id);
    {
        const qreal dpr = devicePixelRatioF();
        QPixmap pm(QSize(56, 56) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(uiAccent());
        p.drawRoundedRect(QRectF(0, 0, 56, 56), 14, 14);
        QFont bf = font();
        bf.setBold(true);
        bf.setPixelSize(24);
        p.setFont(bf);
        p.setPen(Qt::white);
        p.drawText(QRectF(0, 0, 56, 56), Qt::AlignCenter, QStringLiteral("79"));
        p.end();
        badge->setPixmap(pm);
    }
    ih->addWidget(badge, 0, Qt::AlignTop);
    auto *iv = new QVBoxLayout();
    iv->setSpacing(3);
    auto *name = new QLabel(QStringLiteral("JeffPub 79"), id);
    QFont nf = name->font();
    nf.setWeight(QFont::DemiBold);
    nf.setPointSizeF(nf.pointSizeF() * 1.35);
    name->setFont(nf);
    iv->addWidget(name);
    iv->addWidget(mutedLabel(QStringLiteral("Version %1 · Built with Qt %2").arg(QStringLiteral(JP_VERSION), QString::fromLatin1(qVersion())), id));
    auto *tag = new QLabel(QStringLiteral("Free, open-source desktop publishing for Windows and Linux."), id);
    tag->setWordWrap(true);
    iv->addWidget(tag);
    auto *links = new QLabel(QStringLiteral("<a href=\"https://github.com/jeffsteinport/jeffpub79\">Source code</a> &nbsp;·&nbsp; "
                                            "<a href=\"https://github.com/jeffsteinport/jeffpub79/releases\">Downloads</a> &nbsp;·&nbsp; "
                                            "<a href=\"https://github.com/jeffsteinport/jeffpub79/issues\">Report a problem</a>"),
                             id);
    links->setOpenExternalLinks(true);
    iv->addWidget(links);
    // Updates.
    auto *uh = new QHBoxLayout();
    uh->setSpacing(10);
    auto *checkBtn = new QPushButton(QStringLiteral("Check for Updates"), id);
    checkBtn->setCursor(Qt::PointingHandCursor);
    connect(checkBtn, &QPushButton::clicked, this, [this] {
        if (!updater()) setUpdater(new Updater(m_win));
        updater()->check(true);
    });
    auto *autoBox = new QCheckBox(QStringLiteral("Check for updates automatically"), id);
    autoBox->setChecked(Settings::get().value(QStringLiteral("updates/auto"), true).toBool());
    connect(autoBox, &QCheckBox::toggled, this, [](bool on) { Settings::get().setValue(QStringLiteral("updates/auto"), on); });
    uh->addWidget(checkBtn);
    uh->addWidget(autoBox);
    uh->addStretch(1);
    iv->addSpacing(6);
    iv->addLayout(uh);
    ih->addLayout(iv, 1);
    v->addWidget(id);

    // Your name, used as the author of new publications.
    auto *you = cardFrame(w);
    you->setMaximumWidth(820);
    auto *yv = new QVBoxLayout(you);
    yv->setContentsMargins(20, 16, 20, 16);
    yv->setSpacing(6);
    auto *yh = new QLabel(QStringLiteral("Your name"), you);
    QFont yf = yh->font();
    yf.setWeight(QFont::DemiBold);
    yh->setFont(yf);
    yv->addWidget(yh);
    yv->addWidget(mutedLabel(QStringLiteral("Saved as the author of new publications and shown in comments."), you));
    auto *nameEdit = new QLineEdit(Settings::get().userName(), you);
    nameEdit->setMinimumHeight(32);
    nameEdit->setMaximumWidth(360);
    connect(nameEdit, &QLineEdit::editingFinished, this, [nameEdit] { Settings::get().setValue("user/name", nameEdit->text()); });
    yv->addWidget(nameEdit);
    v->addWidget(you);

    // License and legal notices.
    auto *legal = cardFrame(w);
    legal->setMaximumWidth(820);
    auto *lv = new QVBoxLayout(legal);
    lv->setContentsMargins(20, 16, 20, 16);
    lv->setSpacing(8);
    auto *lh = new QLabel(QStringLiteral("License and notices"), legal);
    lh->setFont(yf);
    lv->addWidget(lh);
    auto *lt = mutedLabel(QStringLiteral(
                              "JeffPub 79 is free software under the "
                              "<a href=\"https://github.com/jeffsteinport/jeffpub79/blob/main/LICENSE\">GNU General Public License v3.0</a>. "
                              "It comes with ABSOLUTELY NO WARRANTY; see the license for details. "
                              "<a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">Read the license on gnu.org</a>.<br><br>"
                              "JeffPub 79 is an independent open-source project. It is not affiliated with, endorsed by or supported by any other "
                              "software company. Product names are trademarks of their owners."),
                          legal, 0.75);
    lt->setWordWrap(true);
    lt->setTextFormat(Qt::RichText);
    lt->setOpenExternalLinks(true);
    lv->addWidget(lt);
    // Every bundled component, linked to its project and its license.
    struct Part { const char *name, *url, *use, *license, *licenseUrl; };
    static const Part parts[] = {
        {"Qt 6", "https://www.qt.io/", "Interface, text layout, PDF and printing", "LGPL-3.0", "https://www.gnu.org/licenses/lgpl-3.0.html"},
        {"libmspub (modified)", "https://git.libreoffice.org/libmspub", "Reading .pub files", "MPL-2.0", "https://www.mozilla.org/MPL/2.0/"},
        {"librevenge (modified)", "https://sourceforge.net/p/libwpd/librevenge/", "Document interfaces", "MPL-2.0 or LGPL-2.1+", "https://www.mozilla.org/MPL/2.0/"},
        {"Hunspell", "https://hunspell.github.io/", "Spelling checker", "MPL-1.1, GPL-2.0+ or LGPL-2.1+", "https://www.mozilla.org/MPL/1.1/"},
        {"en_US dictionary (SCOWL)", "http://wordlist.aspell.net/", "Spelling words", "SCOWL license",
         "https://github.com/jeffsteinport/jeffpub79/blob/main/resources/dict/README_en_US.txt"},
        {"hyph_en_US patterns", "https://github.com/jeffsteinport/jeffpub79/blob/main/resources/dict/README_hyph_en_US.txt", "Hyphenation", "BSD-style",
         "https://github.com/jeffsteinport/jeffpub79/blob/main/resources/dict/README_hyph_en_US.txt"},
        {"WordNet", "https://wordnet.princeton.edu/", "Thesaurus", "WordNet license",
         "https://github.com/jeffsteinport/jeffpub79/blob/main/resources/dict/WordNet_license.txt"},
        {"Lucide icons", "https://lucide.dev/", "Interface icons", "ISC", "https://lucide.dev/license"},
        {"Open fonts", "https://github.com/jeffsteinport/jeffpub79/blob/main/resources/fonts/README.md", "Fonts and substitutes",
         "SIL OFL 1.1, GUST, Bitstream Vera", "https://openfontlicense.org/"},
        {"zlib", "https://zlib.net/", "Compression", "zlib license", "https://zlib.net/zlib_license.html"},
#ifdef Q_OS_WIN
        {"MinGW-w64 runtime", "https://www.mingw-w64.org/", "C++ runtime", "GPL-3.0 with GCC Runtime Library Exception; MIT",
         "https://www.gnu.org/licenses/gcc-exception-3.1.html"},
        {"NSIS", "https://nsis.sourceforge.io/", "Installer", "zlib/libpng license", "https://nsis.sourceforge.io/License"},
#else
        {"ICU", "https://icu.unicode.org/", "Unicode text support (in the packages)", "Unicode License", "https://www.unicode.org/license.txt"},
#endif
        {"LibreTranslate", "https://libretranslate.com/", "Translate (opens in your browser)", "AGPL-3.0", "https://www.gnu.org/licenses/agpl-3.0.html"},
    };
    QString rows;
    for (const Part &pt : parts)
        rows += QStringLiteral("<tr><td style=\"padding:3px 14px 3px 0\"><a href=\"%1\">%2</a></td><td style=\"padding:3px 14px 3px 0\">%3</td>"
                               "<td style=\"padding:3px 0\"><a href=\"%4\">%5</a></td></tr>")
                    .arg(QLatin1String(pt.url), QLatin1String(pt.name), QLatin1String(pt.use), QLatin1String(pt.licenseUrl), QLatin1String(pt.license));
    auto *th = new QLabel(QStringLiteral("Third-party software"), legal);
    th->setFont(yf);
    lv->addSpacing(6);
    lv->addWidget(th);
    auto *table = mutedLabel(QStringLiteral("<table cellspacing=\"0\">%1</table>").arg(rows), legal, 0.75);
    table->setTextFormat(Qt::RichText);
    table->setOpenExternalLinks(true);
    lv->addWidget(table);
    v->addWidget(legal);
    v->addStretch(1);
    return w;
}

} // namespace jp
