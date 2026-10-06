// Ribbon layout (Home, Insert, Page Design, Mailings, Review, View and the
// contextual tabs), the status bar, and keeping controls in sync with the
// selection.

#include "app/appfuncs.h"
#include "app/mainwindow.h"
#include "app/taskpane.h"

#include "app/dialogs.h"
#include "app/icons.h"
#include "app/pagespane.h"
#include "app/ribbon.h"
#include "app/settings.h"
#include "app/widgets.h"
#include "canvas/canvas.h"
#include "core/fonts.h"
#include "render/shapes.h"
#include "render/textart.h"
#include "templates/templates.h"
#include "text/textprops.h"
#include "core/presets.h"

#include <QAction>
#include <QActionGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QSlider>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextList>
#include <QToolButton>
#include <QWidgetAction>
#include <QApplication>

namespace jp {

static TableItem *selTableForUi(Editor *ed)
{
    if (ed->isEditingText()) return dynamic_cast<TableItem *>(ed->doc()->item(ed->textTarget().itemId));
    return dynamic_cast<TableItem *>(ed->single());
}

static Stroke g_borderStroke = Stroke::line(ColorRef::scheme(Main), 0.75);
Stroke currentBorderStroke() { return g_borderStroke; }

static QMenu *menuOf(QWidget *parent, const QList<QAction *> &actions)
{
    auto *m = new QMenu(parent);
    for (QAction *a : actions) {
        if (a) m->addAction(a); else m->addSeparator();
    }
    return m;
}

// A plain action that only opens a menu (for ribbon buttons).
static QAction *menuAction(QObject *parent, const QString &text, const QString &iconName)
{
    auto *a = new QAction(icon(iconName), text, parent);
    return a;
}

void MainWindow::buildRibbon()
{
    Editor *ed = m_ed;
    Ribbon *r = m_ribbon;
    for (const char *id : {"file.save", "edit.undo", "edit.redo", "file.print"}) r->addQuickAccess(act(id));

    auto fontRow = [this]() {
        auto *font = new FontCombo();
        auto *size = new SizeCombo();
        connect(font, &FontCombo::familyChosen, this, [this](const QString &f) { m_ed->setFontFamily(f); m_canvas->setFocus(); });
        connect(size, &SizeCombo::sizeChosen, this, [this](double s) { m_ed->setFontSize(s); m_canvas->setFocus(); });
        m_fontCombos << font;
        m_sizeCombos << size;
        return QList<QWidget *>{font, size, ribbonButton(act("fmt.grow"), false, nullptr), ribbonButton(act("fmt.shrink"), false, nullptr)};
    };
    auto iconOnly = [](QAction *a) {
        QToolButton *b = ribbonButton(a, false, nullptr);
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
        return b;
    };
    auto withMenu = [](QToolButton *b, QMenu *m, bool split) {
        b->setMenu(m);
        b->setPopupMode(split ? QToolButton::MenuButtonPopup : QToolButton::InstantPopup);
        return b;
    };
    auto colorBtn = [this](const QString &iconName, const QString &tip, bool allowNone, const QString &noneLabel, std::function<void(const ColorRef &)> apply,
                           const ColorRef &initial) {
        auto *b = new ColorButton(iconName, tip, allowNone, noneLabel);
        b->setCurrent(initial);
        connect(b, &ColorButton::colorPicked, this, [apply, this](const ColorRef &c) { apply(c); m_canvas->setFocus(); });
        m_colorButtons << b;
        return b;
    };
    auto textColorBtn = [this, colorBtn]() {
        auto *b = colorBtn("baseline", QStringLiteral("Font Color"), true, QStringLiteral("Automatic"), [this](const ColorRef &c) { m_ed->setTextColor(c); },
                           ColorRef::rgb(QColor(0xC0, 0, 0)));
        auto *tints = new QAction(QStringLiteral("Tints…"), b);
        connect(tints, &QAction::triggered, this, [this] { tintsDialog(this, m_ed, [this](const ColorRef &c) { m_ed->setTextColor(c); }); });
        b->setExtraActions({tints});
        return b;
    };
    auto fillBtn = [this, colorBtn]() {
        auto *b = colorBtn("paint-bucket", QStringLiteral("Shape Fill"), true, QStringLiteral("No Fill"),
                           [this](const ColorRef &c) { m_ed->forEachSelected(QStringLiteral("Fill"), [c](Item *it) { it->fill = c.isNone() ? Fill() : Fill::solid(c, it->fill.transparency); }); },
                           ColorRef::scheme(Accent1));
        b->setExtraActions({act("fill.picture"), act("fill.effects")});
        return b;
    };
    auto lineBtn = [this, colorBtn]() {
        auto *b = colorBtn("pen-line", QStringLiteral("Shape Outline"), true, QStringLiteral("No Outline"), [this](const ColorRef &c) {
            m_ed->forEachSelected(QStringLiteral("Outline"), [c](Item *it) {
                if (it->stroke.width <= 0) it->stroke.width = 0.75;
                it->stroke.color = c;
            });
        }, ColorRef::scheme(Main));
        b->setExtraActions({act("line.more")});
        return b;
    };
    QList<QAction *> weights, dashes, arrows;
    for (double w : {0.25, 0.5, 0.75, 1.0, 1.5, 2.25, 3.0, 4.5, 6.0}) weights << act(QStringLiteral("weight.%1").arg(w));
    for (int d = 0; d < 8; ++d) dashes << act(QStringLiteral("dash.%1").arg(d));
    for (int a = 0; a < 6; ++a) arrows << act(QStringLiteral("arrowEnd.%1").arg(a));
    for (int a = 0; a < 6; ++a) arrows << act(QStringLiteral("arrowStart.%1").arg(a));

    auto arrangeGroup = [&](RibbonTab *tab) {
        RibbonGroup *g = tab->addGroup(QStringLiteral("Arrange"));
        auto *wrapA = menuAction(this, QStringLiteral("Wrap Text"), "wrap-text");
        g->addLarge(wrapA, menuOf(this, {act("wrap.none"), act("wrap.square"), act("wrap.tight"), act("wrap.through"), act("wrap.topBottom"), nullptr, act("wrap.edit"), act("wrap.more")}));
        g->addSmall(act("arr.forward"), menuOf(this, {act("arr.forward"), act("arr.front")}), true);
        g->addSmall(act("arr.backward"), menuOf(this, {act("arr.backward"), act("arr.back")}), true);
        g->addSmall(act("arr.group"));
        auto *alignA = menuAction(this, QStringLiteral("Align"), "align-horizontal-justify-center");
        g->addSmall(alignA, menuOf(this, {act("arr.alignLeft"), act("arr.alignCenter"), act("arr.alignRight"), nullptr, act("arr.alignTop"), act("arr.alignMiddle"),
                                          act("arr.alignBottom"), nullptr, act("arr.distH"), act("arr.distV"), nullptr, act("arr.relMargins")}));
        g->addSmall(act("arr.ungroup"));
        auto *rotA = menuAction(this, QStringLiteral("Rotate"), "rotate-cw");
        g->addSmall(rotA, menuOf(this, {act("arr.rotR"), act("arr.rotL"), act("arr.flipV"), act("arr.flipH"), nullptr, act("arr.freeRotate")}));
        return g;
    };
    auto sizeGroup = [&](RibbonTab *tab) {
        RibbonGroup *g = tab->addGroup(QStringLiteral("Size"));
        auto *h = new MeasureSpin();
        auto *w = new MeasureSpin();
        h->setToolTip(QStringLiteral("Shape Height"));
        w->setToolTip(QStringLiteral("Shape Width"));
        auto *hl = new QLabel(QStringLiteral("Height")), *wl = new QLabel(QStringLiteral("Width"));
        g->addRow({hl, h});
        g->addRow({wl, w});
        m_heightSpins << h;
        m_widthSpins << w;
        connect(h, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (v <= 0) return;
            m_ed->forEachSelected(QStringLiteral("Size"), [v](Item *it) {
                if (it->type() == ItemType::Line || it->locked) return;
                const QRectF b = it->bounds();
                QRectF to = b;
                to.setHeight(v);
                if (it->rotation == 0 && it->type() != ItemType::Group) { const double k = v / it->rect.height(); it->scaleInto(it->rect, QRectF(it->rect.topLeft(), QSizeF(it->rect.width(), it->rect.height() * k))); }
                else it->scaleInto(b, to);
            });
        });
        connect(w, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (v <= 0) return;
            m_ed->forEachSelected(QStringLiteral("Size"), [v](Item *it) {
                if (it->type() == ItemType::Line || it->locked) return;
                const QRectF b = it->bounds();
                QRectF to = b;
                to.setWidth(v);
                if (it->rotation == 0 && it->type() != ItemType::Group) { const double k = v / it->rect.width(); it->scaleInto(it->rect, QRectF(it->rect.topLeft(), QSizeF(it->rect.width() * k, it->rect.height()))); }
                else it->scaleInto(b, to);
            });
        });
        g->setLauncher([this] { formatObjectDialog(this, m_ed, 1); }, QStringLiteral("Size and Position"));
        return g;
    };
    auto shapeItems = []() {
        QVector<GalleryItem> v;
        v << GalleryItem{"tool:line", "Line", icon("minus"), "Lines"} << GalleryItem{"tool:arrow", "Arrow", icon("move-right"), "Lines"}
          << GalleryItem{"tool:double", "Double Arrow", icon("move-horizontal"), "Lines"};
        for (const auto &s : shapeLibrary()) v << GalleryItem{s.id, s.name, shapeIcon(s.id), s.category};
        return v;
    };
    auto shapesButton = [&](bool large) {
        auto *b = new GalleryButton(icon("shapes"), QStringLiteral("Shapes"), QSize(24, 24), 12, large);
        b->setToolTip(QStringLiteral("Shapes"));
        b->setItemsProvider(shapeItems);
        connect(b, &GalleryButton::activated, this, [this](const QString &id) {
            if (id == "tool:line") m_ed->setTool(Tool::Line);
            else if (id == "tool:arrow") m_ed->setTool(Tool::Arrow);
            else if (id == "tool:double") m_ed->setTool(Tool::DoubleArrow);
            else m_ed->setTool(Tool::Shape, id);
        });
        return b;
    };

    // ================= Home =================
    {
        RibbonTab *t = r->addTab(QStringLiteral("Home"));
        RibbonGroup *g = t->addGroup(QStringLiteral("Clipboard"));
        g->addLarge(act("edit.paste"), menuOf(this, {act("edit.paste"), act("edit.pasteText"), act("edit.pasteSpecial")}), true);
        g->addSmall(act("edit.cut"));
        g->addSmall(act("edit.copy"));
        g->addSmall(act("edit.formatPainter"));

        g = t->addGroup(QStringLiteral("Font"));
        g->addRow(fontRow());
        QToolButton *ub = iconOnly(act("fmt.underline"));
        withMenu(ub, menuOf(this, {act("fmt.underline"), act("fmt.underlineDouble"), act("fmt.underlineDotted"), act("fmt.underlineDash"), act("fmt.underlineWave")}), true);
        auto *caseA = menuAction(this, QStringLiteral("Change Case"), "case-sensitive");
        QToolButton *cb = iconOnly(caseA);
        withMenu(cb, menuOf(this, {act("case.0"), act("case.1"), act("case.2"), act("case.3"), act("case.4"), nullptr, act("fmt.smallCaps"), act("fmt.allCaps")}), false);
        auto *spA = menuAction(this, QStringLiteral("Character Spacing"), "move-horizontal");
        QToolButton *spb = iconOnly(spA);
        withMenu(spb, menuOf(this, {act("spacing.Very Tight"), act("spacing.Tight"), act("spacing.Normal"), act("spacing.Loose"), act("spacing.Very Loose"), nullptr, act("fmt.spacingDialog")}), false);
        g->addRow({iconOnly(act("fmt.bold")), iconOnly(act("fmt.italic")), ub, iconOnly(act("fmt.strike")), iconOnly(act("fmt.sub")), iconOnly(act("fmt.sup")), cb, spb,
                   iconOnly(act("fmt.clear")), textColorBtn()});
        g->setLauncher([this] { fontDialog(this, m_ed); }, QStringLiteral("Font"));

        g = t->addGroup(QStringLiteral("Paragraph"));
        QList<QAction *> bullets;
        for (const char *id : {"bullet.disc", "bullet.circle", "bullet.square", "bullet.diamond", "bullet.arrow", "bullet.check", "bullet.star", "bullet.dash"}) bullets << act(id);
        bullets << nullptr << act("para.listNone") << act("para.bulletsDialog");
        QList<QAction *> numbers;
        for (int i = 0; i < 7; ++i) numbers << act(QStringLiteral("number.%1").arg(i));
        numbers << nullptr << act("para.listNone") << act("para.bulletsDialog");
        QToolButton *bb = withMenu(iconOnly(act("para.bullets")), menuOf(this, bullets), true);
        QToolButton *nb = withMenu(iconOnly(act("para.numbers")), menuOf(this, numbers), true);
        g->addRow({bb, nb, iconOnly(act("para.indentDec")), iconOnly(act("para.indentInc")), iconOnly(act("para.special"))});
        QList<QAction *> ls;
        for (double s : {1.0, 1.15, 1.5, 2.0, 2.5, 3.0}) ls << act(QStringLiteral("ls.%1").arg(s));
        ls << nullptr << act("para.dialog");
        auto *lsA = menuAction(this, QStringLiteral("Line Spacing"), "list-chevrons-up-down");
        auto *psA = menuAction(this, QStringLiteral("Paragraph Spacing"), "arrow-up-down");
        g->addRow({iconOnly(act("para.left")), iconOnly(act("para.center")), iconOnly(act("para.right")), iconOnly(act("para.justify")), iconOnly(act("para.distribute")),
                   iconOnly(act("para.ltr")), iconOnly(act("para.rtl")), withMenu(iconOnly(lsA), menuOf(this, ls), false),
                   withMenu(iconOnly(psA), menuOf(this, {act("para.spaceBefore0"), act("para.spaceBefore6"), act("para.spaceBefore12"), nullptr, act("para.spaceAfter0"),
                                                         act("para.spaceAfter6"), act("para.spaceAfter12"), nullptr, act("para.dialog")}), false)});
        g->setLauncher([this] { paragraphDialog(this, m_ed, 0); }, QStringLiteral("Paragraph"));

        g = t->addGroup(QStringLiteral("Styles"));
        m_styleGallery = new Gallery(QSize(76, 46), 3);
        m_styleGallery->setItemsProvider([this] {
            QVector<GalleryItem> v;
            for (const auto &s : m_ed->doc()->styles) {
                const QString name = s.name;
                Editor *e = m_ed;
                v << GalleryItem{name, name, drawnIcon([e, name](QPainter *p, const QRectF &rc) {
                    const TextStyle *st = e->doc()->style(name);
                    if (!st) return;
                    LayoutEnv env;
                    env.colors = e->doc()->colors;
                    env.fonts = e->doc()->fonts;
                    QTextCharFormat f = resolveCharFormat(st->chr, env);
                    QFont font = f.font();
                    // Sample in the style's own font, sized to read in the tile.
                    const double docPt = f.fontPointSize() / fontPointFactor();
                    font.setPixelSize(int(std::clamp(docPt * 1.15, 12.0, rc.height() * 0.5)));
                    p->setFont(font);
                    p->setPen(f.foreground().color());
                    p->drawText(rc.adjusted(4, 0, -2, -rc.height() * 0.32), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("AaBbCc"));
                    QFont lf;
                    lf.setPixelSize(std::max(9, int(rc.height() * 0.22)));
                    p->setFont(lf);
                    QColor muted = uiText();
                    muted.setAlphaF(0.65f);
                    p->setPen(muted);
                    p->drawText(rc.adjusted(4, rc.height() * 0.66, -2, 0), Qt::AlignLeft | Qt::AlignVCenter,
                                QFontMetrics(lf).elidedText(name, Qt::ElideRight, int(rc.width()) - 6));
                }), QString()};
            }
            return v;
        });
        m_styleGallery->setFooterActions({act("style.new"), act("style.modify"), act("style.byExample"), act("style.import")});
        connect(m_styleGallery, &Gallery::activated, this, [this](const QString &id) { m_ed->applyStyle(id); m_canvas->setFocus(); });
        g->addWidget(m_styleGallery);

        g = t->addGroup(QStringLiteral("Objects"));
        g->addLarge(act("ins.textbox"));
        auto *tableBtn = ribbonButton(menuAction(this, QStringLiteral("Table"), "table"), true, nullptr);
        {
            auto *m = new QMenu(tableBtn);
            auto *grid = new TableGrid();
            auto *wa = new QWidgetAction(m);
            wa->setDefaultWidget(grid);
            m->addAction(wa);
            m->addAction(act("ins.tableDialog"));
            m->addAction(act("ins.drawTable"));
            connect(grid, &TableGrid::picked, this, [this, m](int rows, int cols) {
                m->close();
                const QSizeF ps = m_ed->doc()->pageSize();
                const double w = std::min(ps.width() * 0.7, 72.0 * cols), h = 22.0 * rows;
                auto tb = m_ed->newTable(QRectF((ps.width() - w) / 2, (ps.height() - h) / 2, w, h), rows, cols);
                m_ed->addItem(tb);
            });
            tableBtn->setMenu(m);
        }
        tableBtn->setPopupMode(QToolButton::InstantPopup);
        tableBtn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        tableBtn->setIconSize(QSize(30, 30));
        tableBtn->setAutoRaise(true);
        tableBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        g->addWidget(tableBtn);
        g->addLarge(act("ins.picture"));
        g->addWidget(shapesButton(true));

        arrangeGroup(t);

        g = t->addGroup(QStringLiteral("Editing"));
        g->addSmall(act("edit.find"));
        g->addSmall(act("edit.replace"));
        auto *selA = menuAction(this, QStringLiteral("Select"), "mouse-pointer-2");
        g->addSmall(selA, menuOf(this, {act("edit.selectText"), act("edit.selectAll"), act("edit.selectObjects"), nullptr, act("sel.text"), act("sel.pictures"),
                                        act("sel.shapes"), act("sel.tables"), act("sel.textart")}));
        t->finish();
    }

    // ================= Insert =================
    {
        RibbonTab *t = r->addTab(QStringLiteral("Insert"));
        RibbonGroup *g = t->addGroup(QStringLiteral("Pages"));
        g->addLarge(act("page.insert"), menuOf(this, {act("page.insert"), act("page.insertDup"), act("page.insertDialog")}), true);
        auto *catA = menuAction(this, QStringLiteral("Catalog Pages"), "book-copy");
        connect(catA, &QAction::triggered, this, [this] { showTaskPane("catalog"); });
        g->addLarge(catA);

        g = t->addGroup(QStringLiteral("Tables"));
        {
            auto *tableBtn = ribbonButton(menuAction(this, QStringLiteral("Table"), "table"), true, nullptr);
            auto *m = new QMenu(tableBtn);
            auto *grid = new TableGrid();
            auto *wa = new QWidgetAction(m);
            wa->setDefaultWidget(grid);
            m->addAction(wa);
            m->addAction(act("ins.tableDialog"));
            connect(grid, &TableGrid::picked, this, [this, m](int rows, int cols) {
                m->close();
                const QSizeF ps = m_ed->doc()->pageSize();
                const double w = std::min(ps.width() * 0.7, 72.0 * cols), h = 22.0 * rows;
                m_ed->addItem(m_ed->newTable(QRectF((ps.width() - w) / 2, (ps.height() - h) / 2, w, h), rows, cols));
            });
            tableBtn->setMenu(m);
            tableBtn->setPopupMode(QToolButton::InstantPopup);
            tableBtn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            tableBtn->setIconSize(QSize(30, 30));
            tableBtn->setAutoRaise(true);
            tableBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
            g->addWidget(tableBtn);
        }

        g = t->addGroup(QStringLiteral("Illustrations"));
        g->addLarge(act("ins.picture"));
        g->addLarge(act("ins.onlinePicture"));
        g->addWidget(shapesButton(true));
        g->addLarge(act("ins.placeholder"));

        g = t->addGroup(QStringLiteral("Building Blocks"));
        auto blockButton = [this](const QString &category, const QString &label, const QString &iconName) {
            auto *b = new GalleryButton(icon(iconName), label, QSize(96, 72), 5, true);
            b->setItemsProvider([this, category] {
                QVector<GalleryItem> v;
                for (const auto &bb : buildingBlocks()) {
                    if (bb.category != category) continue;
                    Editor *e = m_ed;
                    const QString id = bb.id;
                    v << GalleryItem{id, bb.name, drawnIcon([e, id](QPainter *p, const QRectF &rc) {
                        // Live preview: build the block in a scratch document with the current schemes.
                        Document tmp;
                        tmp.colors = e->doc()->colors;
                        tmp.fonts = e->doc()->fonts;
                        tmp.biz = e->doc()->biz;
                        tmp.setup.size = QSizeF(612, 792);
                        tmp.pages.clear();
                        tmp.addPage();
                        const BuildingBlock *blk = findBlock(id);
                        if (!blk) return;
                        ItemList items = blk->build(tmp, QRectF(36, 36, 540, 720));
                        const QRectF b = unionBounds(items);
                        if (b.isEmpty()) return;
                        LayoutCache cache;
                        PaintContext ctx;
                        ctx.doc = &tmp;
                        ctx.cache = &cache;
                        ctx.opt.output = true;
                        const double s = std::min(rc.width() / b.width(), rc.height() / b.height()) * 0.92;
                        p->save();
                        p->translate(rc.center());
                        p->scale(s, s);
                        p->translate(-b.center());
                        Renderer::paintItems(p, ctx, items);
                        p->restore();
                    }), bb.category};
                }
                return v;
            });
            connect(b, &GalleryButton::activated, this, [this](const QString &id) {
                const BuildingBlock *blk = findBlock(id);
                if (!blk) return;
                m_ed->beginChange(QStringLiteral("Insert Building Block"));
                const QRectF content = QRectF(QPointF(0, 0), m_ed->doc()->pageSize()).marginsRemoved(m_ed->doc()->setup.margins);
                ItemList items = blk->build(*m_ed->doc(), content);
                QStringList ids;
                for (auto &it : items) { m_ed->surfaceItems().push_back(it); ids << it->id; }
                m_ed->endChange();
                m_ed->select(ids);
            });
            return b;
        };
        g->addWidget(blockButton(QStringLiteral("Page Parts"), QStringLiteral("Page Parts"), "layout-template"));
        {
            auto *cal = blockButton(QStringLiteral("Calendars"), QStringLiteral("Calendars"), "calendar-days");
            auto *more = new QAction(QStringLiteral("More Calendars…"), cal);
            connect(more, &QAction::triggered, this, [this] { calendarDialog(this, m_ed); });
            cal->setFooterActions({more});
            g->addWidget(cal);
        }
        g->addWidget(blockButton(QStringLiteral("Borders & Accents"), QStringLiteral("Borders & Accents"), "frame"));
        g->addWidget(blockButton(QStringLiteral("Advertisements"), QStringLiteral("Advertisements"), "badge-percent"));

        g = t->addGroup(QStringLiteral("Text"));
        g->addLarge(act("ins.textbox"));
        QList<QAction *> biz;
        for (const QString &k : BusinessInfo::keys()) biz << act("biz." + k);
        biz << act("biz.logo") << nullptr << act("ins.bizInfo");
        auto *bizA = menuAction(this, QStringLiteral("Business Information"), "contact");
        g->addLarge(bizA, menuOf(this, biz));
        {
            auto *wa = new GalleryButton(icon("type"), QStringLiteral("Text Art"), QSize(64, 40), 6, true);
            wa->setItemsProvider([this] {
                QVector<GalleryItem> v;
                Editor *e = m_ed;
                for (const auto &s : textArtStyles()) {
                    const QString sid = s.id;
                    v << GalleryItem{sid, s.name, drawnIcon([e, sid](QPainter *p, const QRectF &rc) {
                        TextArtItem w;
                        for (const auto &st : textArtStyles()) if (st.id == sid) applyTextArtStyle(w, st);
                        w.text = QStringLiteral("Aa");
                        w.rect = QRectF(QPointF(0, 0), rc.size() * 0.8);
                        const QPainterPath path = textArtPath(w, w.rect.size()).translated(rc.topLeft() + QPointF(rc.width() * 0.1, rc.height() * 0.1));
                        p->fillPath(path, w.fill.brush(path.boundingRect(), e->doc()->colors));
                        if (!w.stroke.isNone()) p->strokePath(path, QPen(w.stroke.color.resolve(e->doc()->colors), std::max(0.5, w.stroke.width / 2)));
                    }), QString()};
                }
                return v;
            });
            connect(wa, &GalleryButton::activated, this, [this](const QString &sid) {
                auto w = std::make_shared<TextArtItem>();
                for (const auto &st : textArtStyles()) if (st.id == sid) applyTextArtStyle(*w, st);
                const QSizeF ps = m_ed->doc()->pageSize();
                w->rect = QRectF(ps.width() / 2 - 144, ps.height() / 3, 288, 72);
                m_ed->addItem(w);
                editTextArt(w->id);
            });
            g->addWidget(wa);
        }
        g->addSmall(act("ins.file"));
        {
            // Symbol: the recently used symbols to click, then More Symbols.
            auto *sm = new QMenu(this);
            connect(sm, &QMenu::aboutToShow, this, [this, sm] {
                sm->clear();
                const QStringList recents = recentSymbols();
                if (!recents.isEmpty()) {
                    auto *panel = new QWidget(sm);
                    auto *gl = new QGridLayout(panel);
                    gl->setContentsMargins(6, 6, 6, 6);
                    gl->setSpacing(2);
                    for (int i = 0; i < recents.size() && i < 20; ++i) {
                        const QString ch = recents[i].section(QLatin1Char('\t'), 0, 0), fam = recents[i].section(QLatin1Char('\t'), 1);
                        auto *b = new QToolButton(panel);
                        b->setText(ch);
                        QFont f = b->font();
                        if (!fam.isEmpty()) f.setFamily(fam);
                        f.setPointSize(13);
                        b->setFont(f);
                        b->setFixedSize(30, 30);
                        b->setAutoRaise(true);
                        connect(b, &QToolButton::clicked, this, [this, sm, ch, fam] {
                            sm->close();
                            insertSymbol(this, m_ed, ch, fam);
                        });
                        gl->addWidget(b, i / 5, i % 5);
                    }
                    auto *wa = new QWidgetAction(sm);
                    wa->setDefaultWidget(panel);
                    sm->addAction(wa);
                    sm->addSeparator();
                }
                QAction *more = sm->addAction(icon("omega"), QStringLiteral("More Symbols…"));
                connect(more, &QAction::triggered, this, [this] { symbolDialog(this, m_ed); });
            });
            g->addSmall(act("ins.symbol"), sm, true);
        }
        g->addSmall(act("ins.datetime"));
        g->addSmall(act("ins.object"));

        g = t->addGroup(QStringLiteral("Links"));
        g->addLarge(act("ins.link"));
        g->addLarge(act("ins.bookmark"));

        g = t->addGroup(QStringLiteral("Header & Footer"));
        g->addLarge(act("ins.header"));
        g->addLarge(act("ins.footer"));
        g->addLarge(act("ins.pageNumber"), menuOf(this, {act("ins.pageNumber"), act("ins.pageCount"), act("ins.pageNumberFormat")}), true);
        t->finish();
    }

    // ================= Page Design =================
    {
        RibbonTab *t = r->addTab(QStringLiteral("Page Design"));
        RibbonGroup *g = t->addGroup(QStringLiteral("Template"));
        g->addLarge(act("pd.changeTemplate"));

        g = t->addGroup(QStringLiteral("Page Setup"));
        QList<QAction *> margins;
        for (const char *m : {"None", "Narrow", "Moderate", "Wide", "Extra Wide"}) margins << act(QStringLiteral("margins.%1").arg(QString::fromLatin1(m)));
        margins << nullptr << act("pd.customMargins");
        g->addLarge(menuAction(this, QStringLiteral("Margins"), "square-dashed-bottom"), menuOf(this, margins));
        g->addLarge(menuAction(this, QStringLiteral("Orientation"), "rotate-3d"), menuOf(this, {act("pd.portrait"), act("pd.landscape")}));
        {
            // Built each time it opens: the preset sizes, then the user's own
            // (Custom), then creating and editing custom sizes and Page Setup.
            auto *sizeMenu = new QMenu(this);
            connect(sizeMenu, &QMenu::aboutToShow, this, [this, sizeMenu] {
                sizeMenu->clear();
                auto applySize = [this](const QSizeF &s, const QString &name) {
                    m_ed->change(QStringLiteral("Page Size"), [&] {
                        // Keep the current orientation.
                        QSizeF ns = s;
                        const QSizeF cur = m_ed->doc()->setup.size;
                        if ((cur.width() > cur.height()) != (ns.width() > ns.height()) && ns.width() != ns.height()) ns = ns.transposed();
                        m_ed->doc()->setup.size = ns;
                        m_ed->doc()->setup.sheet = ns;
                        m_ed->doc()->setup.sizeName = name;
                    });
                };
                QString group;
                for (const auto &bs : blankSizes()) {
                    if (bs.group != group) { group = bs.group; sizeMenu->addSection(group); }
                    QAction *a = sizeMenu->addAction(QStringLiteral("%1 (%2 × %3)").arg(bs.name, Settings::get().format(bs.size.width()), Settings::get().format(bs.size.height())));
                    a->setCheckable(true);
                    a->setChecked(m_ed->doc()->setup.sizeName == bs.name);
                    const QSizeF s = bs.size;
                    const QString name = bs.name;
                    connect(a, &QAction::triggered, this, [applySize, s, name] { applySize(s, name); });
                }
                const auto custom = customPageSizes();
                if (!custom.isEmpty()) {
                    sizeMenu->addSection(QStringLiteral("Custom"));
                    for (const auto &c : custom) {
                        QAction *a = sizeMenu->addAction(QStringLiteral("%1 (%2 × %3)").arg(c.first, Settings::get().format(c.second.size.width()), Settings::get().format(c.second.size.height())));
                        a->setCheckable(true);
                        a->setChecked(m_ed->doc()->setup.sizeName == c.first);
                        const PageSetup setup = c.second;
                        const QString name = c.first;
                        connect(a, &QAction::triggered, this, [this, setup, name] { applyPageSetup(m_ed, setup, name, QStringLiteral("Page Size")); });
                    }
                }
                sizeMenu->addSeparator();
                sizeMenu->addAction(act("pd.newPageSize"));
                if (!custom.isEmpty()) sizeMenu->addAction(act("pd.customSizes"));
                sizeMenu->addAction(act("pd.pageSetup"));
            });
            g->addLarge(menuAction(this, QStringLiteral("Size"), "file-scan"), sizeMenu);
        }
        {
            auto *gm = new QMenu(this);
            const QVector<QPair<QString, QPair<int, int>>> presets = {{"No Grid", {1, 1}}, {"2 Columns", {2, 1}}, {"3 Columns", {3, 1}}, {"4 Columns", {4, 1}},
                                                                     {"2 × 2 Grid", {2, 2}}, {"3 × 3 Grid", {3, 3}}, {"4 × 4 Grid", {4, 4}}};
            gm->addSection(QStringLiteral("Built-In Guides"));
            for (const auto &pr : presets) {
                QAction *a = gm->addAction(pr.first);
                const int c = pr.second.first, rr = pr.second.second;
                connect(a, &QAction::triggered, this, [this, c, rr] {
                    m_ed->change(QStringLiteral("Guides"), [&] {
                        for (auto &m : m_ed->doc()->masters) { m->grid.cols = c; m->grid.rows = rr; }
                    });
                });
            }
            gm->addSeparator();
            gm->addAction(act("pd.guidesDialog"));
            gm->addAction(act("pd.addH"));
            gm->addAction(act("pd.addV"));
            gm->addAction(act("pd.clearGuides"));
            g->addLarge(menuAction(this, QStringLiteral("Guides"), "layout-grid"), gm);
        }
        g->setLauncher([this] { pageSetupDialog(this, m_ed); }, QStringLiteral("Page Setup"));

        g = t->addGroup(QStringLiteral("Layout"));
        g->addSmall(act("pd.alignGuides"));
        g->addSmall(act("pd.alignObjects"));

        g = t->addGroup(QStringLiteral("Pages"));
        g->addLarge(act("page.delete"));
        g->addLarge(act("page.move"));
        g->addLarge(act("page.rename"));
        auto *mpMenu = new QMenu(this);
        connect(mpMenu, &QMenu::aboutToShow, this, [this, mpMenu] {
            mpMenu->clear();
            for (const auto &m : m_ed->doc()->masters) {
                QAction *a = mpMenu->addAction(QStringLiteral("(%1) %2").arg(m->abbr, m->name));
                a->setCheckable(true);
                a->setChecked(m_ed->doc()->pages[m_ed->currentPage()]->masterId == m->id);
                const QString id = m->id;
                connect(a, &QAction::triggered, this, [this, id] { m_ed->applyMaster(m_ed->currentPage(), id); });
            }
            mpMenu->addAction(act("mp.none"));
            mpMenu->addSeparator();
            mpMenu->addAction(act("view.master"));
        });
        g->addLarge(menuAction(this, QStringLiteral("Master Pages"), "layout-panel-top"), mpMenu);

        g = t->addGroup(QStringLiteral("Schemes"));
        m_schemeGallery = new Gallery(QSize(64, 40), 4);
        auto schemeIcon = [](const ColorScheme &s) {
            return drawnIcon([s](QPainter *p, const QRectF &rc) {
                const double w = rc.width() / 4;
                p->fillRect(QRectF(rc.left(), rc.top(), rc.width(), rc.height() * 0.55), s.c[Main]);
                for (int i = 0; i < 4; ++i) p->fillRect(QRectF(rc.left() + i * w, rc.top() + rc.height() * 0.55, w, rc.height() * 0.45), s.c[Accent1 + i]);
            });
        };
        QVector<GalleryItem> schemes;
        for (const auto &s : builtinColorSchemes()) schemes << GalleryItem{s.name, s.name, schemeIcon(s), QString()};
        m_schemeGallery->setItems(schemes);
        m_schemeGallery->setFooterActions({act("pd.newColorScheme")});
        connect(m_schemeGallery, &Gallery::activated, this, [this](const QString &name) {
            const ColorScheme *s = findColorScheme(name);
            if (s) m_ed->change(QStringLiteral("Color Scheme"), [&] { m_ed->doc()->colors = *s; });
        });
        g->addWidget(m_schemeGallery);
        {
            auto *fm = new QMenu(this);
            for (const auto &fs : builtinFontSchemes()) {
                QAction *a = fm->addAction(QStringLiteral("%1 — %2 / %3").arg(fs.name, fs.heading, fs.body));
                const FontScheme scheme = fs;
                connect(a, &QAction::triggered, this, [this, scheme] { m_ed->change(QStringLiteral("Font Scheme"), [&] { m_ed->doc()->fonts = scheme; }); });
            }
            fm->addSeparator();
            fm->addAction(act("pd.newFontScheme"));
            fm->addAction(act("pd.updateFonts"));
            g->addLarge(menuAction(this, QStringLiteral("Fonts"), "type"), fm);
        }

        g = t->addGroup(QStringLiteral("Page Background"));
        {
            auto *bg = new GalleryButton(icon("paint-roller"), QStringLiteral("Background"), QSize(48, 48), 6, true);
            bg->setItemsProvider([this] {
                QVector<GalleryItem> v;
                Editor *e = m_ed;
                auto add = [&](const QString &id, const QString &tip, const QString &group) {
                    v << GalleryItem{id, tip, drawnIcon([e, id](QPainter *p, const QRectF &rc) {
                        const Fill f = backgroundPreset(id);
                        p->fillRect(rc, Qt::white);
                        p->fillRect(rc, f.brush(rc, e->doc()->colors));
                        p->setPen(QColor(0, 0, 0, 60));
                        p->drawRect(rc.adjusted(0, 0, -1, -1));
                    }), group};
                };
                add("none", "No Background", "No Background");
                for (int s = 1; s <= 5; ++s) for (int t : {0, 40, 80}) add(QStringLiteral("solid.%1.%2").arg(s).arg(t), QStringLiteral("%1 %2%").arg(slotName(s)).arg(100 - t), "Solid Background");
                for (int s = 1; s <= 5; ++s) for (int k = 0; k < 3; ++k) add(QStringLiteral("grad.%1.%2").arg(s).arg(k), QStringLiteral("%1 Gradient").arg(slotName(s)), "Gradient Background");
                return v;
            });
            bg->setFooterActions({act("pd.bgMore"), act("pd.bgAllPages")});
            connect(bg, &GalleryButton::activated, this, [this](const QString &id) {
                const Fill f = backgroundPreset(id);
                m_ed->change(QStringLiteral("Background"), [&] { m_ed->surface()->background = f; });
            });
            g->addWidget(bg);
        }
        g->addLarge(act("pd.bgImage"));
        t->finish();
    }

    // ================= Mailings =================
    {
        RibbonTab *t = r->addTab(QStringLiteral("Mailings"));
        RibbonGroup *g = t->addGroup(QStringLiteral("Start"));
        g->addLarge(menuAction(this, QStringLiteral("Mail Merge"), "mail"), menuOf(this, {act("mm.wizard"), act("mm.mergeEmail")}));
        g->addLarge(menuAction(this, QStringLiteral("Select Recipients"), "users"), menuOf(this, {act("mm.typeNew"), act("mm.existing")}));
        g->addLarge(act("mm.editList"));
        g = t->addGroup(QStringLiteral("Write & Insert Fields"));
        g->addSmall(act("mm.addressBlock"));
        g->addSmall(act("mm.greeting"));
        {
            auto *fieldMenu = new QMenu(this);
            connect(fieldMenu, &QMenu::aboutToShow, this, [this, fieldMenu] {
                fieldMenu->clear();
                const QStringList fields = m_ed->doc()->merge.fields;
                if (fields.isEmpty()) fieldMenu->addAction(QStringLiteral("(Select recipients first)"))->setEnabled(false);
                for (const QString &f : fields) {
                    QAction *a = fieldMenu->addAction(f);
                    connect(a, &QAction::triggered, this, [this, f] {
                        if (m_ed->isEditingText()) { m_ed->insertField("merge:" + f); return; }
                        const QSizeF ps = m_ed->doc()->pageSize();
                        auto tb = std::static_pointer_cast<TextItem>(m_ed->newTextBox(QRectF(ps.width() / 2 - 108, ps.height() / 2 - 14, 216, 28)));
                        QTextCursor c(m_ed->doc()->storyDoc(tb->storyId));
                        QTextCharFormat cf;
                        cf.setProperty(tp::Field, "merge:" + f);
                        c.insertText(QString(QChar::ObjectReplacementCharacter), cf);
                        m_ed->addItem(tb);
                    });
                }
            });
            g->addSmall(menuAction(this, QStringLiteral("Insert Merge Field"), "braces"), fieldMenu);
        }
        g->addSmall(act("mm.pictureField"));
        g = t->addGroup(QStringLiteral("Preview Results"));
        g->addLarge(act("mm.preview"));
        g->addRow({iconOnly(act("mm.first")), iconOnly(act("mm.prev")), iconOnly(act("mm.next")), iconOnly(act("mm.last"))});
        g->addSmall(act("mm.findRecipient"));
        g = t->addGroup(QStringLiteral("Finish"));
        g->addLarge(menuAction(this, QStringLiteral("Finish & Merge"), "send"),
                    menuOf(this, {act("mm.mergePrint"), act("mm.mergeNew"), act("mm.mergePdf"), act("mm.mergeEmail"), nullptr, act("mm.exportList")}));
        t->finish();
    }

    // ================= Review =================
    {
        RibbonTab *t = r->addTab(QStringLiteral("Review"));
        RibbonGroup *g = t->addGroup(QStringLiteral("Proofing"));
        g->addLarge(act("rev.spelling"), menuOf(this, {act("rev.spelling"), act("rev.checkAsType")}), true);
        g->addLarge(act("rev.research"));
        g->addLarge(act("rev.thesaurus"));
        g = t->addGroup(QStringLiteral("Language"));
        g->addLarge(act("rev.translate"));
        g->addLarge(act("rev.language"));
        g = t->addGroup(QStringLiteral("Check"));
        g->addLarge(act("rev.designChecker"));
        g->addLarge(act("rev.wordCount"));
        g->addLarge(act("rev.hyphenation"));
        t->finish();
    }

    // ================= View =================
    {
        RibbonTab *t = r->addTab(QStringLiteral("View"));
        auto *views = new QActionGroup(this);
        views->addAction(act("view.normal"));
        views->addAction(act("view.master"));
        RibbonGroup *g = t->addGroup(QStringLiteral("Views"));
        g->addLarge(act("view.normal"));
        g->addLarge(act("view.master"));
        auto *layouts = new QActionGroup(this);
        layouts->addAction(act("view.single"));
        layouts->addAction(act("view.spread"));
        g = t->addGroup(QStringLiteral("Layout"));
        g->addLarge(act("view.single"));
        g->addLarge(act("view.spread"));
        g = t->addGroup(QStringLiteral("Show"));
        for (const char *id : {"view.boundaries", "view.guides", "view.fields", "view.rulers", "view.pageNav", "view.scratch", "view.baselines", "view.graphics", "view.special"}) {
            if (QString(id) == "view.special") g->addSmall(act("para.special"));
            else g->addSmall(act(id));
        }
        g = t->addGroup(QStringLiteral("Zoom"));
        {
            auto *zoomBox = new QComboBox();
            zoomBox->setEditable(true);
            for (const char *z : {"400%", "300%", "200%", "150%", "100%", "75%", "66%", "50%", "33%", "25%", "10%"}) zoomBox->addItem(QString::fromLatin1(z));
            zoomBox->setFixedWidth(80);
            zoomBox->setCurrentText(QStringLiteral("100%"));
            connect(zoomBox, &QComboBox::textActivated, this, [this](const QString &s) {
                const double z = QString(s).remove('%').toDouble() / 100.0;
                if (z > 0) { m_canvas->zoomToFit(Canvas::Fit::None); m_canvas->setZoom(z); }
            });
            connect(m_canvas, &Canvas::zoomChanged, zoomBox, [zoomBox](double z) { if (!zoomBox->hasFocus()) zoomBox->setCurrentText(QStringLiteral("%1%").arg(std::lround(z * 100))); });
            g->addRow({new QLabel(QStringLiteral("Zoom")), zoomBox});
            g->addRow({ribbonButton(act("zoom.100"), false, nullptr), ribbonButton(act("zoom.page"), false, nullptr)});
            g->addRow({ribbonButton(act("zoom.width"), false, nullptr), ribbonButton(act("zoom.selection"), false, nullptr)});
        }
        g = t->addGroup(QStringLiteral("Window"));
        g->addLarge(act("win.new"));
        g->addSmall(act("win.arrange"));
        g->addSmall(act("win.cascade"));
        auto *switchMenu = new QMenu(this);
        connect(switchMenu, &QMenu::aboutToShow, this, [switchMenu] {
            switchMenu->clear();
            for (QWidget *w : QApplication::topLevelWidgets()) {
                auto *mw = qobject_cast<MainWindow *>(w);
                if (!mw || !mw->isVisible()) continue;
                QAction *a = switchMenu->addAction(mw->windowTitle());
                connect(a, &QAction::triggered, mw, [mw] { mw->raise(); mw->activateWindow(); });
            }
        });
        g->addSmall(menuAction(this, QStringLiteral("Switch Windows"), "app-window"), switchMenu);
        g = t->addGroup(QStringLiteral("Tools"));
        g->addLarge(act("view.measurement"));
        t->finish();
    }

    // ================= Contextual: Master Page =================
    {
        const QColor c(0x7A, 0x5C, 0xA8);
        RibbonTab *t = r->addTab(QStringLiteral("Master Page"), QStringLiteral("Master Page"), c);
        RibbonGroup *g = t->addGroup(QStringLiteral("Master Page"));
        g->addLarge(act("mp.add"));
        g->addLarge(act("mp.dup"));
        g->addLarge(act("mp.rename"));
        g->addLarge(act("mp.delete"));
        g = t->addGroup(QStringLiteral("Layout"));
        g->addLarge(act("mp.twoPage"));
        g->addLarge(menuAction(this, QStringLiteral("Apply To"), "copy-check"), menuOf(this, {act("mp.applyAll"), act("mp.applyCurrent")}));
        g = t->addGroup(QStringLiteral("Header & Footer"));
        g->addSmall(act("ins.header"));
        g->addSmall(act("ins.footer"));
        g->addSmall(act("ins.pageNumber"));
        g = t->addGroup(QStringLiteral("Close"));
        g->addLarge(act("mp.close"));
        t->finish();
    }

    // ================= Contextual: Text Box =================
    {
        const QColor c(0x2C, 0x6E, 0x91);
        RibbonTab *t = r->addTab(QStringLiteral("Text Box"), QStringLiteral("Text Box Tools"), c);
        RibbonGroup *g = t->addGroup(QStringLiteral("Text"));
        g->addLarge(act("tb.textFit"), menuOf(this, {act("fit.best"), act("fit.shrink"), act("fit.grow"), act("fit.none")}));
        g->addLarge(act("tb.direction"));
        g->addLarge(menuAction(this, QStringLiteral("Hyphenation"), "hyphenation"), menuOf(this, {act("rev.hyphenation")}));
        g = t->addGroup(QStringLiteral("Alignment"));
        g->addRow({iconOnly(act("valign.0")), iconOnly(act("para.left")), iconOnly(act("para.center")), iconOnly(act("para.right"))});
        g->addRow({iconOnly(act("valign.1")), iconOnly(act("para.justify")), iconOnly(act("para.distribute"))});
        g->addRow({iconOnly(act("valign.2"))});
        g->addLarge(menuAction(this, QStringLiteral("Columns"), "columns-2"), menuOf(this, {act("cols.1"), act("cols.2"), act("cols.3"), act("cols.4"), nullptr, act("cols.more")}));
        g->addLarge(menuAction(this, QStringLiteral("Margins"), "square-dashed"),
                    menuOf(this, {act("tbmargin.None"), act("tbmargin.Narrow"), act("tbmargin.Moderate"), act("tbmargin.Wide"), nullptr, act("cols.more")}));
        g = t->addGroup(QStringLiteral("Linking"));
        g->addLarge(act("tb.link"));
        g->addSmall(act("tb.break"));
        g->addSmall(act("tb.prev"));
        g->addSmall(act("tb.next"));
        g = t->addGroup(QStringLiteral("Font"));
        g->addRow(fontRow());
        g->addRow({iconOnly(act("fmt.bold")), iconOnly(act("fmt.italic")), iconOnly(act("fmt.underline")), iconOnly(act("fmt.strike")), iconOnly(act("fmt.sub")),
                   iconOnly(act("fmt.sup")), textColorBtn()});
        g = t->addGroup(QStringLiteral("Effects"));
        g->addSmall(act("tb.shadow"));
        g->addSmall(act("tb.outline"));
        g->addSmall(act("tb.emboss"));
        g->addSmall(act("tb.engrave"));
        {
            auto *fill = new ColorButton("paint-bucket", QStringLiteral("Text Fill"), false, QString());
            connect(fill, &ColorButton::colorPicked, this, [this](const ColorRef &c) { m_ed->setTextColor(c); });
            auto *outline = new ColorButton("pen-line", QStringLiteral("Text Outline"), true, QStringLiteral("No Outline"));
            connect(outline, &ColorButton::colorPicked, this, [this](const ColorRef &c) {
                if (c.isNone()) m_ed->clearCharProperty(tp::OutlineRef, QStringLiteral("Text Outline"));
                else m_ed->setCharProperty(tp::OutlineRef, c.toString(), QStringLiteral("Text Outline"));
            });
            auto *glow = new ColorButton("sparkles", QStringLiteral("Text Glow"), true, QStringLiteral("No Glow"));
            connect(glow, &ColorButton::colorPicked, this, [this](const ColorRef &c) {
                if (c.isNone()) m_ed->clearCharProperty(tp::GlowRef, QStringLiteral("Text Glow"));
                else m_ed->setCharProperty(tp::GlowRef, c.toString(), QStringLiteral("Text Glow"));
            });
            m_colorButtons << fill << outline << glow;
            g->addRow({fill, outline, glow});
        }
        g = t->addGroup(QStringLiteral("Typography"));
        QList<QAction *> dc;
        for (int l : {0, 2, 3, 4, 5}) dc << act(QStringLiteral("dropcap.%1").arg(l));
        dc << nullptr << act("dropcap.custom");
        g->addLarge(menuAction(this, QStringLiteral("Drop Cap"), "a-large-small"), menuOf(this, dc));
        QList<QAction *> ns;
        for (int i = 0; i < 3; ++i) ns << act(QStringLiteral("numstyle.%1").arg(i));
        ns << nullptr;
        for (int i = 0; i < 3; ++i) ns << act(QStringLiteral("numspacing.%1").arg(i));
        g->addSmall(menuAction(this, QStringLiteral("Number Style"), "hash"), menuOf(this, ns));
        g->addSmall(menuAction(this, QStringLiteral("Ligatures"), "link"), menuOf(this, {act("lig.0"), act("lig.1"), act("lig.2")}));
        QList<QAction *> sets;
        for (int i = 0; i <= 20; ++i) sets << act(QStringLiteral("ss.%1").arg(i));
        g->addSmall(menuAction(this, QStringLiteral("Stylistic Sets"), "spline"), menuOf(this, sets));
        g->addSmall(act("tb.swash"));
        g->addSmall(act("tb.alternates"));
        g->addSmall(act("tb.trueSmallCaps"));
        arrangeGroup(t);
        sizeGroup(t);
        t->finish();
    }

    // ================= Contextual: Shape Format =================
    {
        const QColor c(0xB5, 0x6A, 0x1E);
        RibbonTab *t = r->addTab(QStringLiteral("Shape Format"), QStringLiteral("Drawing Tools"), c);
        RibbonGroup *g = t->addGroup(QStringLiteral("Insert Shapes"));
        g->addWidget(shapesButton(true));
        {
            auto *change = new GalleryButton(icon("shapes"), QStringLiteral("Change Shape"), QSize(24, 24), 12, false);
            change->setItemsProvider([] {
                QVector<GalleryItem> v;
                for (const auto &s : shapeLibrary()) v << GalleryItem{s.id, s.name, shapeIcon(s.id), s.category};
                return v;
            });
            connect(change, &GalleryButton::activated, this, [this](const QString &id) {
                m_ed->forEachSelected(QStringLiteral("Change Shape"), [id](Item *it) {
                    if (auto *s = dynamic_cast<ShapeItem *>(it)) { s->shape = id; s->adj.clear(); s->customPath = QPainterPath(); }
                    if (auto *p = dynamic_cast<PictureItem *>(it)) p->maskShape = id;
                });
            });
            g->addRow({change});
        }
        g->addSmall(act("shape.editPoints"));
        g->addSmall(act("ins.textbox"));
        g = t->addGroup(QStringLiteral("Shape Styles"));
        {
            auto *gal = new Gallery(QSize(40, 28), 6);
            Editor *e = m_ed;
            gal->setItemsProvider([e] {
                QVector<GalleryItem> v;
                for (int row = 0; row < 6; ++row)
                    for (int slot = 0; slot < 6; ++slot) {
                        const QString id = QStringLiteral("%1.%2").arg(row).arg(slot);
                        v << GalleryItem{id, QStringLiteral("%1 style %2").arg(slotName(slot)).arg(row + 1), drawnIcon([e, row, slot](QPainter *p, const QRectF &rc) {
                            Fill f; Stroke s; Effects fx;
                            shapeStylePreset(row, slot, &f, &s, &fx);
                            const QRectF b = rc.adjusted(4, 4, -4, -4);
                            QPainterPath path;
                            path.addRoundedRect(b, 3, 3);
                            p->fillPath(path, f.brush(b, e->doc()->colors));
                            if (!s.isNone()) p->strokePath(path, s.pen(e->doc()->colors));
                        }), QString()};
                    }
                return v;
            });
            connect(gal, &Gallery::activated, this, [this](const QString &id) {
                const int row = id.section('.', 0, 0).toInt(), slot = id.section('.', 1).toInt();
                m_ed->forEachSelected(QStringLiteral("Shape Style"), [row, slot](Item *it) { shapeStylePreset(row, slot, &it->fill, &it->stroke, &it->fx); });
            });
            g->addWidget(gal);
            gal->setObjectName("shapeStyles");
        }
        g->addRow({fillBtn()});
        {
            auto *lb = lineBtn();
            QMenu *lm = new QMenu(lb);
            lm->addSection(QStringLiteral("Weight"));
            lm->addActions(weights);
            lm->addSection(QStringLiteral("Dashes"));
            lm->addActions(dashes);
            lm->addSection(QStringLiteral("Arrows"));
            lm->addActions(arrows);
            auto *more = new QToolButton();
            more->setIcon(icon("chevron-down"));
            more->setAutoRaise(true);
            more->setPopupMode(QToolButton::InstantPopup);
            more->setMenu(lm);
            more->setToolTip(QStringLiteral("Weight, dashes and arrows"));
            g->addRow({lb, more});
        }
        {
            auto *fx = new QMenu(this);
            QMenu *sh = fx->addMenu(icon("square"), QStringLiteral("Shadow"));
            for (int k = 0; k < 5; ++k) sh->addAction(act(QStringLiteral("shadow.%1").arg(k)));
            sh->addSeparator();
            sh->addAction(act("shadow.options"));
            QMenu *rf = fx->addMenu(QStringLiteral("Reflection"));
            for (int k = 0; k < 4; ++k) rf->addAction(act(QStringLiteral("refl.%1").arg(k)));
            QMenu *gl = fx->addMenu(QStringLiteral("Glow"));
            for (int k : {0, 5, 8, 11, 18}) gl->addAction(act(QStringLiteral("glow.%1").arg(k)));
            QMenu *se = fx->addMenu(QStringLiteral("Soft Edges"));
            for (int k : {0, 2, 5, 10, 25}) se->addAction(act(QStringLiteral("soft.%1").arg(k)));
            QMenu *bv = fx->addMenu(QStringLiteral("Bevel"));
            for (int k = 0; k < 7; ++k) bv->addAction(act(QStringLiteral("bevel.%1").arg(k)));
            QMenu *r3 = fx->addMenu(QStringLiteral("3-D Rotation"));
            for (const char *n : {"No Rotation", "Perspective Left", "Perspective Right", "Perspective Above", "Perspective Below", "Off Axis"})
                r3->addAction(act(QStringLiteral("rot3d.%1").arg(QString::fromLatin1(n))));
            g->addSmall(menuAction(this, QStringLiteral("Shape Effects"), "sparkles"), fx);
        }
        g->setLauncher([this] { formatObjectDialog(this, m_ed, 0); }, QStringLiteral("Format Shape"));
        g = t->addGroup(QStringLiteral("Lines"));
        g->addSmall(act("line.draw"));
        g->addSmall(act("line.arrow"));
        g->addSmall(act("line.double"));
        arrangeGroup(t);
        sizeGroup(t);
        t->finish();
    }

    // ================= Contextual: Picture Format =================
    {
        const QColor c(0x2F, 0x7D, 0x4F);
        RibbonTab *t = r->addTab(QStringLiteral("Picture Format"), QStringLiteral("Picture Tools"), c);
        RibbonGroup *g = t->addGroup(QStringLiteral("Insert"));
        g->addLarge(act("pic.change"), menuOf(this, {act("pic.change"), act("pic.remove")}), true);
        g->addLarge(act("pic.swap"));
        g->addLarge(act("pic.arrangeThumbs"));
        g = t->addGroup(QStringLiteral("Adjust"));
        QMenu *corr = new QMenu(this);
        corr->addSection(QStringLiteral("Brightness / Contrast"));
        for (int b : {-40, -20, 0, 20, 40}) {
            QMenu *sub = corr->addMenu(QStringLiteral("Brightness %1%").arg(b > 0 ? "+" + QString::number(b) : QString::number(b)));
            for (int cc : {-40, -20, 0, 20, 40}) sub->addAction(act(QStringLiteral("corr.%1.%2").arg(b).arg(cc)));
        }
        corr->addSeparator();
        corr->addAction(act("obj.format"));
        g->addLarge(menuAction(this, QStringLiteral("Corrections"), "sun-medium"), corr);
        QList<QAction *> rec;
        for (int k = 0; k < 5; ++k) rec << act(QStringLiteral("recolor.%1").arg(k));
        rec << nullptr;
        for (int s = 1; s <= 5; ++s) rec << act(QStringLiteral("recolor.slot%1").arg(s));
        rec << nullptr << act("pic.transparent");
        g->addLarge(menuAction(this, QStringLiteral("Recolor"), "palette"), menuOf(this, rec));
        g->addSmall(act("pic.compress"));
        g->addSmall(act("pic.reset"));
        g->addSmall(act("obj.transparency"));
        g = t->addGroup(QStringLiteral("Picture Styles"));
        {
            auto *gal = new Gallery(QSize(40, 40), 5);
            Editor *e = m_ed;
            gal->setItemsProvider([e] {
                QVector<GalleryItem> v;
                for (int k = 0; k < 20; ++k) {
                    v << GalleryItem{QString::number(k), QStringLiteral("Picture Style %1").arg(k + 1), drawnIcon([e, k](QPainter *p, const QRectF &rc) {
                        PictureItem pic;
                        pictureStylePreset(k, &pic);
                        const QRectF b = rc.adjusted(5, 5, -5, -5);
                        const QPainterPath path = shapePath(pic.maskShape, b.size()).translated(b.topLeft());
                        QLinearGradient lg(b.topLeft(), b.bottomRight());
                        lg.setColorAt(0, QColor(120, 170, 220));
                        lg.setColorAt(1, QColor(60, 120, 80));
                        if (pic.fx.shadow.on) p->fillPath(path.translated(2, 2), QColor(0, 0, 0, 70));
                        p->fillPath(path, lg);
                        if (!pic.stroke.isNone()) {
                            QPen pen = pic.stroke.pen(e->doc()->colors);
                            pen.setWidthF(std::clamp(pic.stroke.width / 2, 0.75, 4.0));
                            p->strokePath(path, pen);
                        }
                    }), QString()};
                }
                return v;
            });
            connect(gal, &Gallery::activated, this, [this](const QString &id) {
                m_ed->forEachSelected(QStringLiteral("Picture Style"), [id](Item *it) {
                    if (auto *p = dynamic_cast<PictureItem *>(it)) pictureStylePreset(id.toInt(), p);
                });
            });
            gal->setObjectName("pictureStyles");
            g->addWidget(gal);
        }
        {
            auto *border = new ColorButton("square", QStringLiteral("Picture Border"), true, QStringLiteral("No Outline"));
            connect(border, &ColorButton::colorPicked, this, [this](const ColorRef &c) {
                m_ed->forEachSelected(QStringLiteral("Picture Border"), [c](Item *it) { if (it->stroke.width <= 0) it->stroke.width = 1; it->stroke.color = c; });
            });
            m_colorButtons << border;
            QMenu *bm = new QMenu(border);
            bm->addActions(weights);
            bm->addSeparator();
            bm->addActions(dashes);
            auto *more = new QToolButton();
            more->setIcon(icon("chevron-down"));
            more->setAutoRaise(true);
            more->setPopupMode(QToolButton::InstantPopup);
            more->setMenu(bm);
            g->addRow({border, more});
        }
        {
            auto *fx = new QMenu(this);
            QMenu *sh = fx->addMenu(QStringLiteral("Shadow"));
            for (int k = 0; k < 5; ++k) sh->addAction(act(QStringLiteral("shadow.%1").arg(k)));
            QMenu *rf = fx->addMenu(QStringLiteral("Reflection"));
            for (int k = 0; k < 4; ++k) rf->addAction(act(QStringLiteral("refl.%1").arg(k)));
            QMenu *gl = fx->addMenu(QStringLiteral("Glow"));
            for (int k : {0, 5, 8, 11, 18}) gl->addAction(act(QStringLiteral("glow.%1").arg(k)));
            QMenu *se = fx->addMenu(QStringLiteral("Soft Edges"));
            for (int k : {0, 2, 5, 10, 25}) se->addAction(act(QStringLiteral("soft.%1").arg(k)));
            QMenu *bv = fx->addMenu(QStringLiteral("Bevel"));
            for (int k = 0; k < 7; ++k) bv->addAction(act(QStringLiteral("bevel.%1").arg(k)));
            QMenu *r3 = fx->addMenu(QStringLiteral("3-D Rotation"));
            for (const char *n : {"No Rotation", "Perspective Left", "Perspective Right", "Perspective Above", "Perspective Below", "Off Axis"})
                r3->addAction(act(QStringLiteral("rot3d.%1").arg(QString::fromLatin1(n))));
            g->addSmall(menuAction(this, QStringLiteral("Picture Effects"), "sparkles"), fx);
        }
        g->addSmall(act("pic.caption"));
        {
            auto *ps = new GalleryButton(icon("shapes"), QStringLiteral("Picture Shape"), QSize(24, 24), 12, false);
            ps->setItemsProvider([] {
                QVector<GalleryItem> v;
                for (const auto &s : shapeLibrary()) if (!s.open) v << GalleryItem{s.id, s.name, shapeIcon(s.id), s.category};
                return v;
            });
            connect(ps, &GalleryButton::activated, this, [this](const QString &id) {
                m_ed->forEachSelected(QStringLiteral("Picture Shape"), [id](Item *it) { if (auto *p = dynamic_cast<PictureItem *>(it)) p->maskShape = id; });
            });
            g->addRow({ps});
        }
        arrangeGroup(t);
        g = t->addGroup(QStringLiteral("Crop"));
        g->addLarge(act("pic.crop"));
        g->addSmall(act("pic.fit"));
        g->addSmall(act("pic.fill"));
        {
            // Crop to Shape: trim the picture to any closed shape (the same
            // setting as Picture Shape).
            auto *cs = new GalleryButton(icon("crop"), QStringLiteral("Crop to Shape"), QSize(24, 24), 12, false);
            cs->setItemsProvider([] {
                QVector<GalleryItem> v;
                for (const auto &s : shapeLibrary()) if (!s.open) v << GalleryItem{s.id, s.name, shapeIcon(s.id), s.category};
                return v;
            });
            connect(cs, &GalleryButton::activated, this, [this](const QString &id) {
                m_ed->forEachSelected(QStringLiteral("Crop to Shape"), [id](Item *it) { if (auto *p = dynamic_cast<PictureItem *>(it)) p->maskShape = id; });
            });
            g->addRow({cs});
        }
        g->addSmall(act("pic.clearCrop"));
        sizeGroup(t);
        t->finish();
    }

    // ================= Contextual: Table Design / Layout =================
    {
        const QColor c(0x8E, 0x2A, 0x5E);
        RibbonTab *t = r->addTab(QStringLiteral("Table Design"), QStringLiteral("Table Tools"), c);
        RibbonGroup *g = t->addGroup(QStringLiteral("Table Formats"));
        {
            auto *gal = new Gallery(QSize(48, 36), 5);
            Editor *e = m_ed;
            gal->setItemsProvider([e] {
                QVector<GalleryItem> v;
                QStringList names{"None"};
                for (int i = 1; i <= 20; ++i) names << QStringLiteral("Table Style %1").arg(i);
                names << "Basic 1" << "Basic 2" << "Basic 3" << "Checkbook Register" << "List 1" << "List 2" << "List 3" << "Numbers 1" << "Numbers 2";
                for (const QString &n : names) {
                    v << GalleryItem{n, n, drawnIcon([e, n](QPainter *p, const QRectF &rc) {
                        Document tmp;
                        tmp.colors = e->doc()->colors;
                        tmp.pages.clear();
                        tmp.addPage();
                        Editor ed2;
                        Q_UNUSED(ed2);
                        TableItem tb;
                        tb.rows = 4; tb.cols = 3;
                        tb.colW = {rc.width() / 3, rc.width() / 3, rc.width() / 3};
                        tb.rowH = {rc.height() / 4, rc.height() / 4, rc.height() / 4, rc.height() / 4};
                        tb.cells.resize(12);
                        applyTableFormatCells(&tb, n, nullptr);
                        for (int rr = 0; rr < 4; ++rr)
                            for (int cc = 0; cc < 3; ++cc) {
                                const QRectF cr = tb.cellRect(rr, cc).translated(rc.topLeft());
                                const TableCell &cell = tb.cell(rr, cc);
                                if (!cell.fill.isNone()) p->fillRect(cr, cell.fill.brush(cr, tmp.colors));
                                QPen pen(QColor(0, 0, 0, 0));
                                if (!cell.border.top.isNone()) { p->setPen(cell.border.top.pen(tmp.colors)); p->drawLine(cr.topLeft(), cr.topRight()); }
                                if (!cell.border.bottom.isNone()) { p->setPen(cell.border.bottom.pen(tmp.colors)); p->drawLine(cr.bottomLeft(), cr.bottomRight()); }
                                if (!cell.border.left.isNone()) { p->setPen(cell.border.left.pen(tmp.colors)); p->drawLine(cr.topLeft(), cr.bottomLeft()); }
                                if (!cell.border.right.isNone()) { p->setPen(cell.border.right.pen(tmp.colors)); p->drawLine(cr.topRight(), cr.bottomRight()); }
                            }
                    }), QString()};
                }
                return v;
            });
            connect(gal, &Gallery::activated, this, [this](const QString &n) {
                TableItem *tb = selTableForUi(m_ed);
                if (tb) m_ed->change(QStringLiteral("Table Format"), [&] { m_ed->applyTableFormat(tb, n); });
            });
            gal->setObjectName("tableFormats");
            g->addWidget(gal);
        }
        g = t->addGroup(QStringLiteral("Fill"));
        {
            auto *fill = new ColorButton("paint-bucket", QStringLiteral("Cell Fill"), true, QStringLiteral("No Fill"));
            connect(fill, &ColorButton::colorPicked, this, [this](const ColorRef &c) {
                TableItem *tb = selTableForUi(m_ed);
                if (!tb) return;
                const bool inCell = m_ed->isEditingText();
                const auto tt = m_ed->textTarget();
                m_ed->change(QStringLiteral("Fill"), [&] {
                    for (int rr = 0; rr < tb->rows; ++rr)
                        for (int cc = 0; cc < tb->cols; ++cc)
                            if (!inCell || (rr == tt.row && cc == tt.col)) tb->cell(rr, cc).fill = c.isNone() ? Fill() : Fill::solid(c);
                });
            });
            m_colorButtons << fill;
            g->addRow({fill});
        }
        g = t->addGroup(QStringLiteral("Borders"));
        {
            auto *bc = new ColorButton("pen-line", QStringLiteral("Line Color"), false, QString());
            connect(bc, &ColorButton::colorPicked, this, [](const ColorRef &c) { g_borderStroke.color = c; });
            m_colorButtons << bc;
            auto *weightMenu = new QMenu(this);
            for (double w : {0.25, 0.5, 0.75, 1.0, 1.5, 2.25, 3.0, 4.5}) {
                QAction *a = weightMenu->addAction(QStringLiteral("%1 pt").arg(w));
                connect(a, &QAction::triggered, this, [w] { g_borderStroke.width = w; });
            }
            auto *wb = new QToolButton();
            wb->setText(QStringLiteral("Line Weight"));
            wb->setIcon(icon("minus"));
            wb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            wb->setPopupMode(QToolButton::InstantPopup);
            wb->setMenu(weightMenu);
            wb->setAutoRaise(true);
            g->addRow({bc});
            g->addRow({wb});
            QList<QAction *> borders;
            for (const char *id : {"border.all", "border.outside", "border.inside", "border.none", "border.top", "border.bottom", "border.left", "border.right"}) borders << act(id);
            g->addLarge(menuAction(this, QStringLiteral("Borders"), "border-all"), menuOf(this, borders));
        }
        g = t->addGroup(QStringLiteral("Alignment"));
        g->addLarge(menuAction(this, QStringLiteral("Diagonals"), "slash"), menuOf(this, {act("tbl.diagDown"), act("tbl.diagUp"), act("tbl.diagNone")}));
        t->finish();

        RibbonTab *l = r->addTab(QStringLiteral("Table Layout"), QStringLiteral("Table Tools"), c);
        g = l->addGroup(QStringLiteral("Table"));
        g->addLarge(menuAction(this, QStringLiteral("Select"), "mouse-pointer-2"), menuOf(this, {act("tbl.selectCell"), act("tbl.selectTable")}));
        g->addLarge(act("view.gridlines"));
        g = l->addGroup(QStringLiteral("Rows & Columns"));
        g->addLarge(menuAction(this, QStringLiteral("Delete"), "trash-2"), menuOf(this, {act("tbl.delRow"), act("tbl.delCol"), act("tbl.delTable")}));
        g->addLarge(act("tbl.insAbove"));
        g->addLarge(act("tbl.insBelow"));
        g->addLarge(act("tbl.insLeft"));
        g->addLarge(act("tbl.insRight"));
        g = l->addGroup(QStringLiteral("Merge"));
        g->addLarge(act("tbl.merge"));
        g->addLarge(act("tbl.split"));
        g = l->addGroup(QStringLiteral("Alignment"));
        g->addRow({iconOnly(act("valign.0")), iconOnly(act("para.left")), iconOnly(act("para.center")), iconOnly(act("para.right"))});
        g->addRow({iconOnly(act("valign.1")), iconOnly(act("para.justify"))});
        g->addRow({iconOnly(act("valign.2"))});
        g->addLarge(menuAction(this, QStringLiteral("Cell Margins"), "square-dashed"), menuOf(this, {act("tbmargin.None"), act("tbmargin.Narrow"), act("tbmargin.Moderate"), act("tbmargin.Wide")}));
        g = l->addGroup(QStringLiteral("Size"));
        g->addSmall(act("tbl.grow"));
        g->addSmall(act("tbl.distributeRows"));
        g->addSmall(act("tbl.distributeCols"));
        arrangeGroup(l);
        l->finish();
    }

    // ================= Contextual: TextArt Format =================
    {
        const QColor c(0x1D, 0x6F, 0x9E);
        RibbonTab *t = r->addTab(QStringLiteral("Text Art Format"), QStringLiteral("Text Art Tools"), c);
        RibbonGroup *g = t->addGroup(QStringLiteral("Text Art Text"));
        g->addLarge(act("wa.edit"));
        g->addSmall(menuAction(this, QStringLiteral("Spacing"), "move-horizontal"), menuOf(this, {act("waspace.vt"), act("waspace.t"), act("waspace.n"), act("waspace.l"), act("waspace.vl")}));
        g->addSmall(act("wa.even"));
        g->addSmall(act("wa.vertical"));
        QList<QAction *> al;
        for (int i = 0; i < 6; ++i) al << act(QStringLiteral("waalign.%1").arg(i));
        g->addSmall(menuAction(this, QStringLiteral("Align Text"), "align-center"), menuOf(this, al));
        g = t->addGroup(QStringLiteral("Text Art Styles"));
        {
            auto *gal = new Gallery(QSize(48, 30), 5);
            Editor *e = m_ed;
            gal->setItemsProvider([e] {
                QVector<GalleryItem> v;
                for (const auto &s : textArtStyles()) {
                    const QString sid = s.id;
                    v << GalleryItem{sid, s.name, drawnIcon([e, sid](QPainter *p, const QRectF &rc) {
                        TextArtItem w;
                        for (const auto &st : textArtStyles()) if (st.id == sid) applyTextArtStyle(w, st);
                        w.text = QStringLiteral("Aa");
                        w.rect = QRectF(QPointF(0, 0), rc.size() * 0.8);
                        const QPainterPath path = textArtPath(w, w.rect.size()).translated(rc.topLeft() + QPointF(rc.width() * 0.1, rc.height() * 0.1));
                        p->fillPath(path, w.fill.brush(path.boundingRect(), e->doc()->colors));
                        if (!w.stroke.isNone()) p->strokePath(path, QPen(w.stroke.color.resolve(e->doc()->colors), 0.75));
                    }), QString()};
                }
                return v;
            });
            connect(gal, &Gallery::activated, this, [this](const QString &sid) {
                m_ed->forEachSelected(QStringLiteral("Text Art Style"), [sid](Item *it) {
                    if (auto *w = dynamic_cast<TextArtItem *>(it))
                        for (const auto &st : textArtStyles()) if (st.id == sid) { const QString text = w->text; applyTextArtStyle(*w, st); w->text = text; }
                });
            });
            gal->setObjectName("textartStyles");
            g->addWidget(gal);
        }
        g->addRow({fillBtn()});
        g->addRow({lineBtn()});
        {
            auto *shape = new GalleryButton(icon("spline"), QStringLiteral("Change Shape"), QSize(56, 36), 6, false);
            shape->setItemsProvider([this] {
                QVector<GalleryItem> v;
                Editor *e = m_ed;
                for (const auto &tr : textArtTransforms()) {
                    const QString id = tr.id;
                    v << GalleryItem{id, tr.name, drawnIcon([e, id](QPainter *p, const QRectF &rc) {
                        TextArtItem w;
                        w.text = QStringLiteral("abcde");
                        w.transform_ = id;
                        w.rect = QRectF(QPointF(0, 0), rc.size() * 0.86);
                        const QPainterPath path = textArtPath(w, w.rect.size()).translated(rc.topLeft() + QPointF(rc.width() * 0.07, rc.height() * 0.07));
                        p->fillPath(path, uiText());
                        Q_UNUSED(e);
                    }), QString()};
                }
                return v;
            });
            connect(shape, &GalleryButton::activated, this, [this](const QString &id) {
                m_ed->forEachSelected(QStringLiteral("Text Art Shape"), [id](Item *it) { if (auto *w = dynamic_cast<TextArtItem *>(it)) w->transform_ = id; });
            });
            g->addRow({shape});
        }
        g = t->addGroup(QStringLiteral("Effects"));
        {
            auto *sh = new QMenu(this);
            for (int k = 0; k < 5; ++k) sh->addAction(act(QStringLiteral("shadow.%1").arg(k)));
            sh->addSeparator();
            sh->addAction(act("shadow.options"));
            g->addLarge(menuAction(this, QStringLiteral("Shadow Effects"), "copy"), sh);
            auto *r3 = new QMenu(this);
            for (const char *n : {"No Rotation", "Perspective Left", "Perspective Right", "Perspective Above", "Perspective Below", "Off Axis"})
                r3->addAction(act(QStringLiteral("rot3d.%1").arg(QString::fromLatin1(n))));
            r3->addSeparator();
            for (int k = 0; k < 7; ++k) r3->addAction(act(QStringLiteral("bevel.%1").arg(k)));
            g->addLarge(menuAction(this, QStringLiteral("3-D Effects"), "box"), r3);
        }
        arrangeGroup(t);
        sizeGroup(t);
        t->finish();
    }

    r->showTab(r->tab(QStringLiteral("Home")));
    connect(r, &Ribbon::tabChanged, this, [this] { refreshUi(); });
    Q_UNUSED(ed);
}

void MainWindow::buildStatusBar()
{
    QStatusBar *sb = statusBar();
    m_pageLabel = new QLabel();
    m_pageLabel->setMinimumWidth(110);
    auto *prev = new QToolButton();
    prev->setDefaultAction(act("page.prev"));
    prev->setAutoRaise(true);
    auto *next = new QToolButton();
    next->setDefaultAction(act("page.next"));
    next->setAutoRaise(true);
    m_posLabel = new QLabel();
    m_posLabel->setMinimumWidth(120);
    m_sizeLabel = new QLabel();
    m_sizeLabel->setMinimumWidth(120);
    sb->addWidget(prev);
    sb->addWidget(m_pageLabel);
    sb->addWidget(next);
    sb->addWidget(new QLabel(QStringLiteral("  ")));
    auto *posIcon = new QLabel();
    posIcon->setPixmap(icon("move").pixmap(14, 14));
    sb->addWidget(posIcon);
    sb->addWidget(m_posLabel);
    auto *sizeIcon = new QLabel();
    sizeIcon->setPixmap(icon("scaling").pixmap(14, 14));
    sb->addWidget(sizeIcon);
    sb->addWidget(m_sizeLabel);
    auto mkBtn = [&](const char *id) {
        auto *b = new QToolButton();
        b->setDefaultAction(act(id));
        b->setAutoRaise(true);
        b->setIconSize(QSize(16, 16));
        sb->addPermanentWidget(b);
    };
    mkBtn("view.single");
    mkBtn("view.spread");
    auto *zo = new QToolButton();
    zo->setDefaultAction(act("zoom.out"));
    zo->setAutoRaise(true);
    sb->addPermanentWidget(zo);
    m_zoomSlider = new QSlider(Qt::Horizontal);
    m_zoomSlider->setRange(-48, 57);   // 1.05^n: 10% .. 1600%
    m_zoomSlider->setFixedWidth(140);
    connect(m_zoomSlider, &QSlider::valueChanged, this, [this](int v) {
        m_canvas->zoomToFit(Canvas::Fit::None);
        m_canvas->setZoom(std::pow(1.05, v));
    });
    sb->addPermanentWidget(m_zoomSlider);
    auto *zi = new QToolButton();
    zi->setDefaultAction(act("zoom.in"));
    zi->setAutoRaise(true);
    sb->addPermanentWidget(zi);
    m_zoomLabel = new QLabel(QStringLiteral("100%"));
    m_zoomLabel->setMinimumWidth(44);
    sb->addPermanentWidget(m_zoomLabel);
    mkBtn("zoom.page");
}

void MainWindow::updateContextTabs()
{
    const QString kind = m_ed->selectionKind();
    Item *it = m_ed->isEditingText() ? m_ed->doc()->item(m_ed->textTarget().itemId) : m_ed->single();
    const bool text = (it && (it->type() == ItemType::Text || (it->type() == ItemType::Shape && m_ed->isEditingText())));
    const bool shape = kind == "shape" || kind == "line" || kind == "multi" || kind == "group" || kind == "text" || kind == "textart";
    const bool picture = kind == "picture";
    const bool table = kind == "table" || (m_ed->isEditingText() && it && it->type() == ItemType::Table);
    const bool textart = kind == "textart";
    const QString before = m_ribbon->current() ? QString() : QString();
    m_ribbon->setContextVisible(QStringLiteral("Text Box Tools"), text);
    m_ribbon->setContextVisible(QStringLiteral("Drawing Tools"), shape && !text);
    m_ribbon->setContextVisible(QStringLiteral("Picture Tools"), picture);
    m_ribbon->setContextVisible(QStringLiteral("Table Tools"), table);
    m_ribbon->setContextVisible(QStringLiteral("Text Art Tools"), textart);
    m_ribbon->setContextVisible(QStringLiteral("Master Page"), !m_ed->masterView().isEmpty());
    Q_UNUSED(before);
}

void MainWindow::refreshUi()
{
    Editor *ed = m_ed;
    Document *d = ed->doc();
    updateContextTabs();
    const QString kind = ed->selectionKind();
    const bool sel = !kind.isEmpty();
    const bool editing = ed->isEditingText();
    const bool textish = editing || (ed->single() && ed->single()->hasText()) || kind == "multi";
    const QTextCharFormat cf = textish ? ed->currentCharFormat() : QTextCharFormat();
    const QTextBlockFormat bf = textish ? ed->currentBlockFormat() : QTextBlockFormat();

    act("edit.undo")->setEnabled(ed->undoStack()->canUndo() || editing);
    act("edit.redo")->setEnabled(ed->undoStack()->canRedo());
    act("edit.undo")->setToolTip(QStringLiteral("Undo %1 (Ctrl+Z)").arg(ed->undoStack()->undoText()));
    act("edit.redo")->setToolTip(QStringLiteral("Redo %1 (Ctrl+Y)").arg(ed->undoStack()->redoText()));
    for (const char *id : {"edit.cut", "edit.copy", "edit.duplicate", "edit.delete"}) act(id)->setEnabled(sel || (editing && ed->cursor().hasSelection()));
    for (const char *id : {"arr.front", "arr.forward", "arr.backward", "arr.back", "arr.rotR", "arr.rotL", "arr.flipH", "arr.flipV", "obj.format", "obj.lock"})
        act(id)->setEnabled(sel);
    act("arr.group")->setEnabled(kind == "multi" || kind == "group");
    act("arr.group")->setText(kind == "group" ? QStringLiteral("Ungroup") : QStringLiteral("Group"));
    act("arr.ungroup")->setEnabled(kind == "group");
    for (const char *id : {"fmt.bold", "fmt.italic", "fmt.underline", "fmt.strike", "fmt.sub", "fmt.sup", "fmt.grow", "fmt.shrink", "fmt.clear", "para.bullets",
                           "para.numbers", "para.left", "para.center", "para.right", "para.justify", "para.distribute", "para.indentDec", "para.indentInc",
                           "para.ltr", "para.rtl"})
        act(id)->setEnabled(textish);
    act("fmt.bold")->setChecked(cf.fontWeight() >= QFont::DemiBold);
    act("fmt.italic")->setChecked(cf.fontItalic());
    act("fmt.underline")->setChecked(cf.underlineStyle() != QTextCharFormat::NoUnderline);
    act("fmt.strike")->setChecked(cf.fontStrikeOut());
    act("fmt.sub")->setChecked(cf.verticalAlignment() == QTextCharFormat::AlignSubScript);
    act("fmt.sup")->setChecked(cf.verticalAlignment() == QTextCharFormat::AlignSuperScript);
    const Qt::Alignment al = bf.alignment() & Qt::AlignHorizontal_Mask;
    const bool dist = bf.boolProperty(tp::Distribute);
    act("para.left")->setChecked(textish && (al == Qt::AlignLeft || al == 0 || al == Qt::AlignLeading));
    act("para.center")->setChecked(al == Qt::AlignHCenter);
    act("para.right")->setChecked(al == Qt::AlignRight || al == Qt::AlignTrailing);
    act("para.justify")->setChecked(al == Qt::AlignJustify && !dist);
    act("para.distribute")->setChecked(dist);
    act("para.rtl")->setChecked(textish && bf.layoutDirection() == Qt::RightToLeft);
    act("para.ltr")->setChecked(textish && bf.layoutDirection() != Qt::RightToLeft);
    QTextList *list = editing ? ed->cursor().block().textList() : nullptr;
    act("para.bullets")->setChecked(list && isBulletList(list->format().style()));
    act("para.numbers")->setChecked(list && !isBulletList(list->format().style()));
    act("para.special")->setChecked(ed->view.special);
    act("view.boundaries")->setChecked(ed->view.boundaries);
    act("view.guides")->setChecked(ed->view.guides);
    act("view.fields")->setChecked(ed->view.fields);
    act("view.rulers")->setChecked(ed->view.rulers);
    act("view.pageNav")->setChecked(ed->view.pageNav);
    act("view.scratch")->setChecked(ed->view.scratch);
    act("view.baselines")->setChecked(ed->view.baselines);
    act("view.gridlines")->setChecked(ed->view.gridlines);
    act("view.graphics")->setChecked(currentTaskPane() == "graphics");
    act("pd.alignGuides")->setChecked(ed->view.snapGuides);
    act("pd.alignObjects")->setChecked(ed->view.snapObjects);
    act("rev.checkAsType")->setChecked(ed->view.spelling);
    act("view.master")->setChecked(!ed->masterView().isEmpty());
    act("view.normal")->setChecked(ed->masterView().isEmpty());
    act("view.single")->setChecked(!ed->twoPageSpread());
    act("view.spread")->setChecked(ed->twoPageSpread());
    act("edit.formatPainter")->setCheckable(true);
    act("edit.formatPainter")->setChecked(ed->tool() == Tool::FormatPainter);
    act("mm.preview")->setChecked(ed->mergeRecord() >= 0);
    act("mm.preview")->setEnabled(!d->merge.isEmpty());
    for (const char *id : {"mm.first", "mm.prev", "mm.next", "mm.last", "mm.findRecipient"}) act(id)->setEnabled(ed->mergeRecord() >= 0);
    act("page.delete")->setEnabled(d->pages.size() > 1);
    if (MasterPage *mp = d->master(ed->masterView())) act("mp.twoPage")->setChecked(mp->twoPage);
    // Object-specific checks.
    Item *one = editing ? d->item(ed->textTarget().itemId) : ed->single();
    if (one) {
        for (const auto &[id, mode] : {std::pair{"wrap.none", Wrap::None}, {"wrap.square", Wrap::Square}, {"wrap.tight", Wrap::Tight}, {"wrap.through", Wrap::Through},
                                       {"wrap.topBottom", Wrap::TopBottom}})
            act(id)->setChecked(one->wrap.mode == mode);
        act("obj.lock")->setChecked(one->locked);
    }
    if (auto *t = dynamic_cast<TextItem *>(one)) {
        act("fit.best")->setChecked(t->autofit == TextItem::BestFit);
        act("fit.shrink")->setChecked(t->autofit == TextItem::ShrinkOnOverflow);
        act("fit.grow")->setChecked(t->autofit == TextItem::GrowBox);
        act("fit.none")->setChecked(t->autofit == TextItem::NoAutofit);
        act("tb.direction")->setChecked(t->vertical);
        act("tb.break")->setEnabled(!t->nextId.isEmpty());
        act("tb.next")->setEnabled(!t->nextId.isEmpty());
        act("tb.prev")->setEnabled(d->prevFrame(t->id) != nullptr);
        act("tb.contOn")->setChecked(t->continuedOn);
        act("tb.contFrom")->setChecked(t->continuedFrom);
        for (int k = 0; k < 3; ++k) act(QStringLiteral("valign.%1").arg(k))->setChecked(int(t->valign) == k);
    }
    if (auto *p = dynamic_cast<PictureItem *>(one)) act("pic.crop")->setChecked(ed->cropItem == p->id);
    if (auto *w = dynamic_cast<TextArtItem *>(one)) {
        act("wa.even")->setChecked(w->evenHeight);
        act("wa.vertical")->setChecked(w->vertical);
    }
    if (auto *tb = selTableForUi(ed)) act("tbl.grow")->setChecked(tb->growToFit);
    act("tb.shadow")->setChecked(cf.boolProperty(tp::Shadow));
    act("tb.outline")->setChecked(!cf.stringProperty(tp::OutlineRef).isEmpty());
    act("tb.emboss")->setChecked(cf.boolProperty(tp::Emboss));
    act("tb.swash")->setChecked(cf.boolProperty(tp::Swash));
    act("tb.alternates")->setChecked(cf.boolProperty(tp::Alternates));
    act("tb.trueSmallCaps")->setChecked(cf.boolProperty(tp::TrueSmallCaps));
    act("tb.engrave")->setChecked(cf.boolProperty(tp::Engrave));
    act("fmt.smallCaps")->setChecked(cf.fontCapitalization() == QFont::SmallCaps);
    act("fmt.allCaps")->setChecked(cf.fontCapitalization() == QFont::AllUppercase);

    // Font and size boxes.
    LayoutEnv env;
    env.colors = d->colors;
    env.fonts = d->fonts;
    QString family;
    if (textish) {
        if (cf.hasProperty(QTextFormat::FontFamilies)) family = cf.fontFamilies().toStringList().value(0);
        else family = cf.stringProperty(tp::ThemeFont) == QLatin1String("major") ? d->fonts.heading : d->fonts.body;
    }
    for (FontCombo *f : m_fontCombos) {
        f->setSchemeFonts(d->fonts.heading, d->fonts.body);
        f->setEnabled(textish);
        f->setCurrentFamily(family);
    }
    for (SizeCombo *s : m_sizeCombos) {
        s->setEnabled(textish);
        s->setSize(textish ? (cf.hasProperty(QTextFormat::FontPointSize) ? cf.fontPointSize() : 11) : 0);
    }
    for (ColorButton *c : m_colorButtons) c->setScheme(d->colors);
    const QRectF b = ed->selectionBounds();
    for (MeasureSpin *s : m_widthSpins) { s->setEnabled(sel); s->setPoints(b.width()); }
    for (MeasureSpin *s : m_heightSpins) { s->setEnabled(sel); s->setPoints(b.height()); }
    if (m_styleGallery) {
        // Inline gallery shows the first styles; "More" shows all.
        static QString lastSig;
        QString sig = d->fonts.heading + d->fonts.body + d->colors.name;
        for (const auto &s : d->styles) sig += s.name;
        Q_UNUSED(lastSig);
        QVector<GalleryItem> items;
        Editor *e = ed;
        for (const auto &s : d->styles) {
            const QString name = s.name;
            items << GalleryItem{name, name, drawnIcon([e, name](QPainter *p, const QRectF &rc) {
                const TextStyle *st = e->doc()->style(name);
                if (!st) return;
                LayoutEnv en;
                en.colors = e->doc()->colors;
                en.fonts = e->doc()->fonts;
                QTextCharFormat f = resolveCharFormat(st->chr, en);
                QFont font = f.font();
                font.setPixelSize(int(std::clamp(f.fontPointSize() * 0.9, 9.0, rc.height() * 0.45)));
                p->setFont(font);
                p->setPen(f.foreground().color());
                p->drawText(rc.adjusted(2, 0, -2, -rc.height() * 0.3), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("AaBbCc"));
                QFont lf;
                lf.setPixelSize(int(rc.height() * 0.22));
                p->setFont(lf);
                p->setPen(uiText());
                p->drawText(rc.adjusted(2, rc.height() * 0.7, -2, 0), Qt::AlignLeft | Qt::AlignVCenter, name);
            }), QString()};
        }
        m_styleGallery->setItems(items);
        m_styleGallery->setCurrent(ed->currentStyleName());
    }
    if (m_schemeGallery) m_schemeGallery->setCurrent(d->colors.name);
    // Inline galleries for contextual tabs fill themselves from their providers.
    for (const char *name : {"shapeStyles", "pictureStyles", "tableFormats", "textartStyles"})
        if (auto *g = findChild<Gallery *>(QString::fromLatin1(name)); g && g->isVisible()) g->reload();

    // Status bar.
    if (!ed->masterView().isEmpty()) {
        const MasterPage *mp = d->master(ed->masterView());
        m_pageLabel->setText(QStringLiteral("Master Page %1").arg(mp ? mp->abbr : QString()));
    } else {
        m_pageLabel->setText(QStringLiteral("Page: %1 of %2").arg(ed->currentPage() + 1).arg(d->pages.size()));
    }
    Settings &st = Settings::get();
    m_sizeLabel->setText(sel ? QStringLiteral("%1 × %2").arg(st.format(b.width()), st.format(b.height())) : QString());
    if (sel) m_posLabel->setText(QStringLiteral("%1, %2").arg(st.format(b.left()), st.format(b.top())));
    m_task->refresh();
}

} // namespace jp
