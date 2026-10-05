#include "app/taskpane.h"

#include "app/appfuncs.h"
#include "app/dialogs.h"
#include "app/icons.h"
#include "app/mainwindow.h"
#include "app/settings.h"
#include "canvas/canvas.h"
#include "io/importers.h"
#include "text/textprops.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace jp {

TaskPane::TaskPane(MainWindow *win) : QWidget(win), m_win(win)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    auto *head = new QHBoxLayout();
    m_title = new QLabel(this);
    QFont f = m_title->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 1.15);
    m_title->setFont(f);
    auto *close = new QToolButton(this);
    close->setIcon(icon("x"));
    close->setAutoRaise(true);
    connect(close, &QToolButton::clicked, this, &TaskPane::closed);
    head->addWidget(m_title, 1);
    head->addWidget(close);
    v->addLayout(head);
    m_stack = new QStackedWidget(this);
    v->addWidget(m_stack, 1);
    setMinimumWidth(240);
}

void TaskPane::open(const QString &nameIn)
{
    QString name = nameIn == "replace" ? QStringLiteral("find") : nameIn;
    if (!m_panes.contains(name)) {
        QWidget *w = create(name);
        if (!w) return;
        m_panes.insert(name, w);
        m_stack->addWidget(w);
    }
    m_current = name;
    m_stack->setCurrentWidget(m_panes[name]);
    static const QHash<QString, QString> titles{{"designchecker", "Design Checker"}, {"mailmerge", "Mail Merge"}, {"find", "Find and Replace"},
                                                {"graphics", "Graphics Manager"}, {"research", "Research"}, {"online", "Online Pictures"},
                                                {"catalog", "Catalog Merge"}};
    m_title->setText(titles.value(name, name));
    if (nameIn == "replace") if (auto *e = m_panes[name]->findChild<QLineEdit *>("replace")) e->setFocus();
    refresh();
}

void TaskPane::refresh()
{
    if (!isVisible() || m_current.isEmpty()) return;
    QWidget *w = m_panes.value(m_current);
    if (w && w->metaObject()->indexOfMethod("refresh()") >= 0) QMetaObject::invokeMethod(w, "refresh", Qt::DirectConnection);
}

// ---------------- Design Checker ----------------
class DesignChecker : public QWidget {
    Q_OBJECT
public:
    DesignChecker(MainWindow *win) : m_win(win)
    {
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        m_all = new QCheckBox(QStringLiteral("Run general design checks"), this);
        m_all->setChecked(true);
        m_print = new QCheckBox(QStringLiteral("Run final publishing checks"), this);
        m_print->setChecked(true);
        v->addWidget(m_all);
        v->addWidget(m_print);
        v->addWidget(new QLabel(QStringLiteral("Select an item to fix:"), this));
        m_list = new QListWidget(this);
        m_list->setWordWrap(true);
        v->addWidget(m_list, 1);
        auto *fix = new QPushButton(QStringLiteral("Go to Item"), this);
        v->addWidget(fix);
        connect(m_all, &QCheckBox::toggled, this, &DesignChecker::refresh);
        connect(m_print, &QCheckBox::toggled, this, &DesignChecker::refresh);
        connect(m_list, &QListWidget::itemDoubleClicked, this, &DesignChecker::go);
        connect(fix, &QPushButton::clicked, this, [this] { if (m_list->currentItem()) go(m_list->currentItem()); });
    }
    Q_INVOKABLE void refresh()
    {
        Editor *ed = m_win->editor();
        Document *d = ed->doc();
        m_list->clear();
        const QRectF pageRect(QPointF(0, 0), d->pageSize());
        auto add = [&](const QString &msg, const QString &id, int page, const QString &iconName) {
            auto *it = new QListWidgetItem(icon(iconName), page >= 0 ? QStringLiteral("%1 (Page %2)").arg(msg).arg(page + 1) : msg);
            it->setData(Qt::UserRole, id);
            it->setData(Qt::UserRole + 1, page);
            m_list->addItem(it);
        };
        for (int p = 0; p < d->pages.size(); ++p) {
            const auto &pg = d->pages[p];
            if (m_all->isChecked() && pg->items.empty()) add(QStringLiteral("Page has no content"), QString(), p, "file");
            walkItems(pg->items, [&](const ItemPtr &it) {
                if (it->type() == ItemType::Group) return;
                const QRectF b = it->bounds();
                if (m_all->isChecked()) {
                    if (!pageRect.contains(b) && b.intersects(pageRect)) add(QStringLiteral("Object is partially off the page"), it->id, p, "triangle-alert");
                    if (it->type() == ItemType::Text) {
                        auto *t = static_cast<TextItem *>(it.get());
                        const auto chain = d->chainOf(t->id);
                        QTextDocument *sd = d->storyDoc(t->storyId);
                        if (chain.size() == 1 && sd && sd->toPlainText().trimmed().isEmpty()) add(QStringLiteral("Text box is empty"), t->id, p, "square-dashed");
                        if (!chain.isEmpty() && chain.last() == t) {
                            const auto fl = ed->cache().textFrame(*d, *t, p + 1, RenderOptions());
                            if (fl.layout && fl.layout->overflow()) add(QStringLiteral("Story with text in overflow area"), t->id, p, "circle-alert");
                        }
                        if (t->autofit == TextItem::BestFit || t->autofit == TextItem::ShrinkOnOverflow) {
                            const auto fl = ed->cache().textFrame(*d, *t, p + 1, RenderOptions());
                            if (fl.fitScale < 0.5) add(QStringLiteral("Text is shrunk a great deal to fit"), t->id, p, "a-arrow-down");
                        }
                    }
                    if (it->type() == ItemType::Picture) {
                        auto *pic = static_cast<PictureItem *>(it.get());
                        if (pic->imageId.isEmpty()) add(QStringLiteral("Picture placeholder is empty"), pic->id, p, "image");
                        else {
                            const QSize px = d->imageSize(pic->imageId);
                            const ImageData data = d->images.value(pic->imageId);
                            if (px.isValid() && data.format != "svg" && data.format != "wmf" && data.format != "emf") {
                                const double ppiX = px.width() / (pic->imgRect.width() / 72.0), ppiY = px.height() / (pic->imgRect.height() / 72.0);
                                if (std::abs(ppiX - ppiY) / std::max(ppiX, ppiY) > 0.04) add(QStringLiteral("Picture is not scaled proportionally"), pic->id, p, "scaling");
                                if (m_print->isChecked() && std::min(ppiX, ppiY) < 150)
                                    add(QStringLiteral("Picture has low resolution (%1 ppi)").arg(int(std::min(ppiX, ppiY))), pic->id, p, "image-off");
                            }
                            if (data.linked) add(QStringLiteral("Picture is linked, not embedded"), pic->id, p, "link");
                        }
                    }
                }
                if (m_print->isChecked()) {
                    if (it->fill.transparency > 0 || it->fx.any()) add(QStringLiteral("Object has transparency or effects (may print differently)"), it->id, p, "blend");
                    if (it->type() == ItemType::Text) {
                        QTextDocument *sd = d->storyDoc(static_cast<TextItem *>(it.get())->storyId);
                        if (sd) for (QTextBlock b = sd->begin(); b.isValid(); b = b.next())
                            for (auto f = b.begin(); !f.atEnd(); ++f) {
                                const QStringList fams = f.fragment().charFormat().fontFamilies().toStringList();
                                if (!fams.isEmpty() && !QFontDatabase::hasFamily(fams.first())) {
                                    add(QStringLiteral("Font \"%1\" is not installed").arg(fams.first()), it->id, p, "type");
                                    return;
                                }
                            }
                    }
                }
            });
        }
        if (m_list->count() == 0) m_list->addItem(new QListWidgetItem(icon("circle-check"), QStringLiteral("No problems found.")));
    }

private:
    void go(QListWidgetItem *it)
    {
        Editor *ed = m_win->editor();
        const int page = it->data(Qt::UserRole + 1).toInt();
        const QString id = it->data(Qt::UserRole).toString();
        if (page >= 0) ed->setCurrentPage(page);
        if (!id.isEmpty()) {
            ed->select(id);
            if (Item *x = ed->doc()->item(id)) m_win->canvas()->ensureVisible(x->bounds());
        }
    }
    MainWindow *m_win;
    QCheckBox *m_all, *m_print;
    QListWidget *m_list;
};

// ---------------- Find and Replace ----------------
class FindPane : public QWidget {
    Q_OBJECT
public:
    explicit FindPane(MainWindow *win) : m_win(win)
    {
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->addWidget(new QLabel(QStringLiteral("Find what:"), this));
        m_find = new QLineEdit(this);
        v->addWidget(m_find);
        v->addWidget(new QLabel(QStringLiteral("Replace with:"), this));
        m_replace = new QLineEdit(this);
        m_replace->setObjectName("replace");
        v->addWidget(m_replace);
        m_case = new QCheckBox(QStringLiteral("Match case"), this);
        m_whole = new QCheckBox(QStringLiteral("Find whole words only"), this);
        m_up = new QCheckBox(QStringLiteral("Search up"), this);
        v->addWidget(m_case);
        v->addWidget(m_whole);
        v->addWidget(m_up);
        auto *row = new QHBoxLayout();
        auto *next = new QPushButton(QStringLiteral("Find Next"), this);
        auto *rep = new QPushButton(QStringLiteral("Replace"), this);
        auto *all = new QPushButton(QStringLiteral("Replace All"), this);
        row->addWidget(next);
        row->addWidget(rep);
        row->addWidget(all);
        v->addLayout(row);
        m_status = new QLabel(this);
        m_status->setWordWrap(true);
        v->addWidget(m_status);
        v->addStretch(1);
        connect(next, &QPushButton::clicked, this, [this] { findNext(); });
        connect(m_find, &QLineEdit::returnPressed, this, [this] { findNext(); });
        connect(rep, &QPushButton::clicked, this, [this] { replaceOne(); });
        connect(all, &QPushButton::clicked, this, [this] { replaceAll(); });
    }
    Q_INVOKABLE void refresh() {}

private:
    struct Hit { QString itemId, storyId; int row = -1, col = -1; int page = -1; QString master; };
    QVector<Hit> stories()
    {
        // Every story once, in page order, using the first frame of each chain.
        QVector<Hit> out;
        QSet<QString> seen;
        Document *d = m_win->editor()->doc();
        auto visit = [&](const ItemList &items, int page, const QString &master) {
            walkItems(items, [&](const ItemPtr &it) {
                if (it->type() == ItemType::Text) {
                    auto *t = static_cast<TextItem *>(it.get());
                    if (!seen.contains(t->storyId)) { seen.insert(t->storyId); out << Hit{d->chainOf(t->id).value(0, t)->id, t->storyId, -1, -1, page, master}; }
                } else if (it->type() == ItemType::Shape) {
                    auto *s = static_cast<ShapeItem *>(it.get());
                    if (!s->storyId.isEmpty() && !seen.contains(s->storyId)) { seen.insert(s->storyId); out << Hit{s->id, s->storyId, -1, -1, page, master}; }
                } else if (it->type() == ItemType::Table) {
                    auto *tb = static_cast<TableItem *>(it.get());
                    for (int r = 0; r < tb->rows; ++r)
                        for (int c = 0; c < tb->cols; ++c) out << Hit{tb->id, tb->cell(r, c).storyId, r, c, page, master};
                }
            });
        };
        for (int p = 0; p < d->pages.size(); ++p) visit(d->pages[p]->items, p, QString());
        for (const auto &m : d->masters) visit(m->items, -1, m->id);
        return out;
    }
    QTextDocument::FindFlags flags() const
    {
        QTextDocument::FindFlags f;
        if (m_case->isChecked()) f |= QTextDocument::FindCaseSensitively;
        if (m_whole->isChecked()) f |= QTextDocument::FindWholeWords;
        if (m_up->isChecked()) f |= QTextDocument::FindBackward;
        return f;
    }
    bool findNext()
    {
        Editor *ed = m_win->editor();
        const QString q = m_find->text();
        if (q.isEmpty()) return false;
        const QVector<Hit> all = stories();
        if (all.isEmpty()) return false;
        int start = 0;
        int pos = 0;
        if (ed->isEditingText()) {
            for (int i = 0; i < all.size(); ++i)
                if (all[i].storyId == ed->textTarget().storyId) { start = i; break; }
            pos = m_up->isChecked() ? ed->cursor().selectionStart() : ed->cursor().selectionEnd();
        } else if (m_up->isChecked()) {
            start = all.size() - 1;
            pos = -1;
        }
        for (int k = 0; k <= all.size(); ++k) {
            const int i = m_up->isChecked() ? (start - k + all.size() * 2) % all.size() : (start + k) % all.size();
            const Hit &h = all[i];
            QTextDocument *doc = ed->doc()->storyDoc(h.storyId);
            if (!doc) continue;
            int from = (k == 0) ? pos : (m_up->isChecked() ? doc->characterCount() : 0);
            if (from < 0) from = doc->characterCount();
            const QTextCursor found = doc->find(q, from, flags());
            if (found.isNull()) continue;
            if (!h.master.isEmpty()) ed->setMasterView(h.master);
            else if (h.page >= 0) ed->setCurrentPage(h.page);
            ed->beginTextEdit(h.itemId, found.selectionStart(), h.row, h.col);
            QTextCursor c = ed->cursor();
            c.setPosition(found.selectionStart());
            c.setPosition(found.selectionEnd(), QTextCursor::KeepAnchor);
            ed->setCursor(c);
            m_status->clear();
            return true;
        }
        m_status->setText(QStringLiteral("JeffPub 79 finished searching the publication. \"%1\" was not found.").arg(q));
        return false;
    }
    void replaceOne()
    {
        Editor *ed = m_win->editor();
        if (ed->isEditingText() && ed->cursor().hasSelection() &&
            ed->cursor().selectedText().compare(m_find->text(), m_case->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive) == 0) {
            ed->change(QStringLiteral("Replace"), [&] { ed->cursor().insertText(m_replace->text()); });
            ed->textEdited();
        }
        findNext();
    }
    void replaceAll()
    {
        Editor *ed = m_win->editor();
        const QString q = m_find->text();
        if (q.isEmpty()) return;
        ed->endTextEdit();
        int n = 0;
        ed->change(QStringLiteral("Replace All"), [&] {
            for (const Hit &h : stories()) {
                QTextDocument *doc = ed->doc()->storyDoc(h.storyId);
                if (!doc) continue;
                QTextCursor c = doc->find(q, 0, flags() & ~QTextDocument::FindBackward);
                while (!c.isNull()) {
                    c.insertText(m_replace->text());
                    ++n;
                    c = doc->find(q, c, flags() & ~QTextDocument::FindBackward);
                }
            }
        });
        m_status->setText(QStringLiteral("JeffPub 79 made %1 replacement(s).").arg(n));
    }
    MainWindow *m_win;
    QLineEdit *m_find, *m_replace;
    QCheckBox *m_case, *m_whole, *m_up;
    QLabel *m_status;
};

// ---------------- Graphics Manager ----------------
class GraphicsPane : public QWidget {
    Q_OBJECT
public:
    explicit GraphicsPane(MainWindow *win) : m_win(win)
    {
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        m_sort = new QComboBox(this);
        m_sort->addItems({"Sort by page", "Sort by name", "Sort by size", "Sort by type"});
        v->addWidget(m_sort);
        m_list = new QListWidget(this);
        m_list->setIconSize(QSize(56, 56));
        v->addWidget(m_list, 1);
        auto *row = new QHBoxLayout();
        auto *go = new QPushButton(QStringLiteral("Go to"), this);
        auto *save = new QPushButton(QStringLiteral("Save As…"), this);
        auto *replace = new QPushButton(QStringLiteral("Replace…"), this);
        row->addWidget(go);
        row->addWidget(save);
        row->addWidget(replace);
        v->addLayout(row);
        m_details = new QLabel(this);
        m_details->setWordWrap(true);
        v->addWidget(m_details);
        connect(m_sort, &QComboBox::currentIndexChanged, this, &GraphicsPane::refresh);
        connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) { showDetails(it); });
        connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) { goTo(it); });
        connect(go, &QPushButton::clicked, this, [this] { goTo(m_list->currentItem()); });
        connect(save, &QPushButton::clicked, this, [this] {
            auto *it = m_list->currentItem();
            if (!it) return;
            const QString iid = it->data(Qt::UserRole + 2).toString();
            const ImageData data = m_win->editor()->doc()->images.value(iid);
            const QString p = askSavePath(this, QStringLiteral("Save Picture"), QStringLiteral("picture.") + data.format);
            if (p.isEmpty()) return;
            QFile f(p);
            if (f.open(QIODevice::WriteOnly)) f.write(data.bytes);
        });
        connect(replace, &QPushButton::clicked, this, [this] {
            auto *it = m_list->currentItem();
            if (it) m_win->insertPictureFromFile(it->data(Qt::UserRole).toString());
        });
    }
    Q_INVOKABLE void refresh()
    {
        Editor *ed = m_win->editor();
        Document *d = ed->doc();
        struct Row { QString id, iid; int page; qint64 size; QString name, fmt; };
        QVector<Row> rows;
        d->forEachItem([&](Item *it, int page, const QString &) {
            auto *p = dynamic_cast<PictureItem *>(it);
            if (!p || p->imageId.isEmpty()) return;
            const ImageData data = d->images.value(p->imageId);
            rows << Row{p->id, p->imageId, page, data.bytes.size(), data.sourcePath.isEmpty() ? p->imageId : QFileInfo(data.sourcePath).fileName(), data.format};
        });
        const int sort = m_sort->currentIndex();
        std::sort(rows.begin(), rows.end(), [sort](const Row &a, const Row &b) {
            if (sort == 1) return a.name < b.name;
            if (sort == 2) return a.size > b.size;
            if (sort == 3) return a.fmt < b.fmt;
            return a.page < b.page;
        });
        m_list->clear();
        for (const Row &r : rows) {
            const QImage thumb = d->image(r.iid).scaled(56, 56, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            auto *it = new QListWidgetItem(QIcon(QPixmap::fromImage(thumb)),
                                           QStringLiteral("%1\nPage %2 · %3 · %4 KB").arg(r.name).arg(r.page >= 0 ? QString::number(r.page + 1) : QStringLiteral("master")).arg(r.fmt.toUpper()).arg(r.size / 1024));
            it->setData(Qt::UserRole, r.id);
            it->setData(Qt::UserRole + 1, r.page);
            it->setData(Qt::UserRole + 2, r.iid);
            m_list->addItem(it);
        }
    }

private:
    void showDetails(QListWidgetItem *it)
    {
        if (!it) { m_details->clear(); return; }
        Document *d = m_win->editor()->doc();
        auto *p = dynamic_cast<PictureItem *>(d->item(it->data(Qt::UserRole).toString()));
        if (!p) return;
        const ImageData data = d->images.value(p->imageId);
        const QSize px = d->imageSize(p->imageId);
        const double ppi = p->imgRect.width() > 0 ? px.width() / (p->imgRect.width() / 72.0) : 0;
        m_details->setText(QStringLiteral("Status: Embedded\nOriginal file: %1\nPixels: %2 × %3\nEffective resolution: %4 ppi")
                               .arg(data.sourcePath.isEmpty() ? QStringLiteral("(not available)") : data.sourcePath)
                               .arg(px.width()).arg(px.height()).arg(int(ppi)));
    }
    void goTo(QListWidgetItem *it)
    {
        if (!it) return;
        Editor *ed = m_win->editor();
        const int page = it->data(Qt::UserRole + 1).toInt();
        if (page >= 0) ed->setCurrentPage(page);
        ed->select(it->data(Qt::UserRole).toString());
    }
    MainWindow *m_win;
    QComboBox *m_sort;
    QListWidget *m_list;
    QLabel *m_details;
};

// ---------------- Mail merge wizard ----------------
class MailMergePane : public QWidget {
    Q_OBJECT
public:
    explicit MailMergePane(MainWindow *win) : m_win(win)
    {
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        m_step = new QLabel(this);
        QFont f = m_step->font();
        f.setBold(true);
        m_step->setFont(f);
        v->addWidget(m_step);
        m_body = new QStackedWidget(this);
        v->addWidget(m_body, 1);
        // Step 1: recipients
        {
            auto *w = new QWidget();
            auto *l = new QVBoxLayout(w);
            l->addWidget(new QLabel(QStringLiteral("Create recipient list"), w));
            for (const char *id : {"mm.existing", "mm.typeNew", "mm.editList"}) {
                auto *b = new QPushButton(win->act(id)->icon(), win->act(id)->text(), w);
                QAction *a = win->act(id);
                connect(b, &QPushButton::clicked, a, &QAction::trigger);
                l->addWidget(b);
            }
            m_summary = new QLabel(w);
            m_summary->setWordWrap(true);
            l->addWidget(m_summary);
            l->addStretch(1);
            m_body->addWidget(w);
        }
        // Step 2: fields
        {
            auto *w = new QWidget();
            auto *l = new QVBoxLayout(w);
            l->addWidget(new QLabel(QStringLiteral("Click in a text box, then click a field to insert it:"), w));
            m_fields = new QListWidget(w);
            l->addWidget(m_fields, 1);
            connect(m_fields, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
                Editor *ed = m_win->editor();
                const QString code = it->data(Qt::UserRole).toString();
                if (code == "addr") { mergeFieldDialog(this, ed, 0); return; }
                if (code == "greet") { mergeFieldDialog(this, ed, 1); return; }
                if (ed->isEditingText()) ed->insertField("merge:" + code);
                else QMessageBox::information(this, QStringLiteral("Mail Merge"), QStringLiteral("Click in a text box first."));
            });
            m_body->addWidget(w);
        }
        // Step 3: finish
        {
            auto *w = new QWidget();
            auto *l = new QVBoxLayout(w);
            l->addWidget(new QLabel(QStringLiteral("Preview your publications and finish the merge."), w));
            auto *nav = new QHBoxLayout();
            for (const char *id : {"mm.first", "mm.prev", "mm.next", "mm.last"}) {
                auto *b = new QToolButton(w);
                b->setDefaultAction(win->act(id));
                nav->addWidget(b);
            }
            l->addLayout(nav);
            for (const char *id : {"mm.mergeNew", "mm.mergePdf", "mm.mergePrint", "mm.mergeEmail", "mm.exportList"}) {
                auto *b = new QPushButton(win->act(id)->icon(), win->act(id)->text(), w);
                QAction *a = win->act(id);
                connect(b, &QPushButton::clicked, a, &QAction::trigger);
                l->addWidget(b);
            }
            l->addStretch(1);
            m_body->addWidget(w);
        }
        auto *row = new QHBoxLayout();
        auto *prev = new QPushButton(QStringLiteral("Previous"), this);
        auto *next = new QPushButton(QStringLiteral("Next"), this);
        row->addWidget(prev);
        row->addWidget(next);
        v->addLayout(row);
        connect(prev, &QPushButton::clicked, this, [this] { m_body->setCurrentIndex(std::max(0, m_body->currentIndex() - 1)); refresh(); });
        connect(next, &QPushButton::clicked, this, [this] {
            m_body->setCurrentIndex(std::min(2, m_body->currentIndex() + 1));
            if (m_body->currentIndex() == 2 && m_win->editor()->mergeRecord() < 0) m_win->act("mm.preview")->trigger();
            refresh();
        });
    }
    Q_INVOKABLE void refresh()
    {
        const MergeSource &m = m_win->editor()->doc()->merge;
        m_step->setText(QStringLiteral("Step %1 of 3").arg(m_body->currentIndex() + 1));
        m_summary->setText(m.isEmpty() ? QStringLiteral("No recipient list yet.") : QStringLiteral("%1 recipients, %2 fields.\nSource: %3").arg(m.includedRows().size()).arg(m.fields.size()).arg(m.path.isEmpty() ? QStringLiteral("typed list") : m.path));
        m_fields->clear();
        auto *a = new QListWidgetItem(icon("mail-open"), QStringLiteral("Address block"));
        a->setData(Qt::UserRole, "addr");
        m_fields->addItem(a);
        auto *g = new QListWidgetItem(icon("hand"), QStringLiteral("Greeting line"));
        g->setData(Qt::UserRole, "greet");
        m_fields->addItem(g);
        for (const QString &f : m.fields) {
            auto *it = new QListWidgetItem(icon("braces"), f);
            it->setData(Qt::UserRole, f);
            m_fields->addItem(it);
        }
    }

private:
    MainWindow *m_win;
    QLabel *m_step, *m_summary;
    QStackedWidget *m_body;
    QListWidget *m_fields;
};

// ---------------- Research (local dictionary) ----------------
class ResearchPane : public QWidget {
    Q_OBJECT
public:
    explicit ResearchPane(MainWindow *win) : m_win(win)
    {
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->addWidget(new QLabel(QStringLiteral("Search for:"), this));
        m_q = new QLineEdit(this);
        v->addWidget(m_q);
        auto *go = new QPushButton(QStringLiteral("Look Up"), this);
        v->addWidget(go);
        m_out = new QListWidget(this);
        m_out->setWordWrap(true);
        v->addWidget(m_out, 1);
        auto *note = new QLabel(QStringLiteral("JeffPub 79 looks up words in its offline thesaurus. It does not send your text to online services."), this);
        note->setWordWrap(true);
        v->addWidget(note);
        connect(go, &QPushButton::clicked, this, &ResearchPane::lookUp);
        connect(m_q, &QLineEdit::returnPressed, this, &ResearchPane::lookUp);
    }
    Q_INVOKABLE void refresh()
    {
        Editor *ed = m_win->editor();
        if (ed->isEditingText() && m_q->text().isEmpty()) {
            QTextCursor c = ed->cursor();
            if (!c.hasSelection()) c.select(QTextCursor::WordUnderCursor);
            m_q->setText(c.selectedText());
        }
    }

private:
    void lookUp()
    {
        m_out->clear();
        const QStringList syn = thesaurusLookup(m_q->text().trimmed());
        if (syn.isEmpty()) m_out->addItem(QStringLiteral("No results for \"%1\".").arg(m_q->text().trimmed()));
        for (const QString &s : syn) m_out->addItem(s);
    }
    MainWindow *m_win;
    QLineEdit *m_q;
    QListWidget *m_out;
};

QWidget *TaskPane::create(const QString &name)
{
    if (name == "designchecker") return new DesignChecker(m_win);
    if (name == "find") return new FindPane(m_win);
    if (name == "graphics") return new GraphicsPane(m_win);
    if (name == "mailmerge" || name == "catalog") return new MailMergePane(m_win);
    if (name == "research") return new ResearchPane(m_win);
    if (name == "online") {
        auto *w = new QWidget();
        auto *v = new QVBoxLayout(w);
        auto *l = new QLabel(QStringLiteral("Find free, openly licensed pictures in these libraries. Check each picture's license, download it, "
                                            "then use Insert > Pictures or drag the file onto the page."), w);
        l->setWordWrap(true);
        v->addWidget(l);
        for (const auto &[name, url] : {std::pair{"Openverse (Creative Commons search)", "https://openverse.org"}, {"Wikimedia Commons", "https://commons.wikimedia.org"},
                                         {"Unsplash", "https://unsplash.com"}, {"Pexels", "https://www.pexels.com"}, {"Openclipart", "https://openclipart.org"}}) {
            auto *b = new QLabel(QStringLiteral("<a href=\"%1\">%2</a>").arg(QString::fromLatin1(url), QString::fromLatin1(name)), w);
            b->setOpenExternalLinks(true);
            v->addWidget(b);
        }
        v->addStretch(1);
        return w;
    }
    return nullptr;
}

} // namespace jp

#include "taskpane.moc"
