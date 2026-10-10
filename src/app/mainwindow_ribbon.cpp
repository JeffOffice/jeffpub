// The ribbon's controls (its layout is resources/ribbon.json, built by
// ribbonbuilder.cpp), the status bar, and keeping controls in sync with the
// selection.

#include "app/appfuncs.h"
#include "app/mainwindow.h"
#include "app/taskpane.h"

#include "app/dialogs.h"
#include "app/icons.h"
#include "app/pagespane.h"
#include "app/ribbon.h"
#include "app/ribbonbuilder.h"
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
#include <QComboBox>
#include <QFile>

namespace jp {

static TableItem *selTableForUi(Editor *ed)
{
    if (ed->isEditingText()) return dynamic_cast<TableItem *>(ed->doc()->item(ed->textTarget().itemId));
    return dynamic_cast<TableItem *>(ed->single());
}

static Stroke g_borderStroke = Stroke::line(ColorRef::scheme(Main), 0.75);

// A style's sample in the Styles gallery: its text as it prints, on paper
// where the interface is dark or in high contrast (black text on a dark tile
// couldn't be read), and the style's name under it.
static void drawStylePreview(QPainter *p, const QRectF &rc, Editor *e, const QString &name, bool large)
{
    const TextStyle *st = e->doc()->style(name);
    if (!st) return;
    LayoutEnv env;
    env.colors = e->doc()->colors;
    env.fonts = e->doc()->fonts;
    const QTextCharFormat f = resolveCharFormat(st->chr, env);
    const QRectF sample = rc.adjusted(large ? 4 : 2, 0, -2, -rc.height() * (large ? 0.32 : 0.3));
    if (uiDark() || uiHighContrast()) {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(Qt::white);
        p->drawRoundedRect(sample.adjusted(-2, 2, 0, -1), 3, 3);
        p->restore();
    }
    QFont font = f.font();
    if (large) {
        // Sized to read in the tile.
        const double docPt = f.fontPointSize() / fontPointFactor();
        font.setPixelSize(int(std::clamp(docPt * 1.15, 12.0, rc.height() * 0.5)));
    } else {
        font.setPixelSize(int(std::clamp(f.fontPointSize() * 0.9, 9.0, rc.height() * 0.45)));
    }
    p->setFont(font);
    p->setPen(f.foreground().color());
    p->drawText(sample, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("AaBbCc"));
    QFont lf;
    lf.setPixelSize(std::max(9, int(rc.height() * 0.22)));
    p->setFont(lf);
    QColor muted = uiText();
    if (large) muted.setAlphaF(0.65f);
    p->setPen(muted);
    p->drawText(rc.adjusted(large ? 4 : 2, rc.height() * (large ? 0.66 : 0.7), -2, 0), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(lf).elidedText(name, Qt::ElideRight, int(rc.width()) - 6));
}
Stroke currentBorderStroke() { return g_borderStroke; }

void MainWindow::buildRibbon()
{
    RibbonParts parts;
    // Not act(): an unknown id is the loader's to report, once, with where it was used.
    parts.action = [this](const QString &id) { return m_actions.value(id); };
    ribbonControlParts(parts);
    ribbonGalleryParts(parts);
    ribbonMenuParts(parts);

    parts.launchers[QStringLiteral("font")] = [this] { fontDialog(this, m_ed); };
    parts.launchers[QStringLiteral("paragraph")] = [this] { paragraphDialog(this, m_ed, 0); };
    parts.launchers[QStringLiteral("pageSetup")] = [this] { pageSetupDialog(this, m_ed); };
    parts.launchers[QStringLiteral("formatShape")] = [this] { formatObjectDialog(this, m_ed, 0); };
    parts.launchers[QStringLiteral("sizePosition")] = [this] { formatObjectDialog(this, m_ed, 1); };
    parts.triggers[QStringLiteral("ribbon.catalogPages")] = [this] { showTaskPane("catalog"); };
    parts.colors[QStringLiteral("masterPage")] = QColor(0x7A, 0x5C, 0xA8);
    parts.colors[QStringLiteral("textBox")] = QColor(0x2C, 0x6E, 0x91);
    parts.colors[QStringLiteral("shapeFormat")] = QColor(0xB5, 0x6A, 0x1E);
    parts.colors[QStringLiteral("pictureFormat")] = QColor(0x2F, 0x7D, 0x4F);
    parts.colors[QStringLiteral("table")] = QColor(0x8E, 0x2A, 0x5E);   // Table Design and Layout share it
    parts.colors[QStringLiteral("textArt")] = QColor(0x1D, 0x6F, 0x9E);

    // Only one view and one layout can be on at a time.
    auto *views = new QActionGroup(this);
    views->addAction(act("view.normal"));
    views->addAction(act("view.master"));
    auto *layouts = new QActionGroup(this);
    layouts->addAction(act("view.single"));
    layouts->addAction(act("view.spread"));

    // The layout ships inside the program, so a failure here is a mistake in
    // ribbon.json: the loader names every unknown command, widget or menu.
    QFile file(QStringLiteral(":/ribbon.json"));
    if (!file.open(QIODevice::ReadOnly)) m_ribbonError = QStringLiteral("ribbon.json is missing from the program");
    else if (!jp::buildRibbon(m_ribbon, file.readAll(), parts, this, &m_ribbonError) && m_ribbonError.isEmpty())
        m_ribbonError = QStringLiteral("ribbon.json didn't build");
    if (!m_ribbonError.isEmpty()) qCritical("JeffPub: the ribbon didn't build:\n%s", qPrintable(m_ribbonError));

    m_ribbon->showTab(m_ribbon->tab(QStringLiteral("Home")));
    connect(m_ribbon, &Ribbon::tabChanged, this, [this] { refreshUi(); });
}

// Text, color and measurement controls. Each factory makes a new control and
// enrolls it where refreshUi() keeps it in step with the selection.
void MainWindow::ribbonControlParts(RibbonParts &parts)
{
    parts.widgets[QStringLiteral("fontCombo")] = [this]() -> QWidget * {
        auto *font = new FontCombo();
        connect(font, &FontCombo::familyChosen, this, [this](const QString &f) { m_ed->setFontFamily(f); m_canvas->setFocus(); });
        m_fontCombos << font;
        return font;
    };
    parts.widgets[QStringLiteral("sizeCombo")] = [this]() -> QWidget * {
        auto *size = new SizeCombo();
        connect(size, &SizeCombo::sizeChosen, this, [this](double s) { m_ed->setFontSize(s); m_canvas->setFocus(); });
        m_sizeCombos << size;
        return size;
    };

    auto colorBtn = [this](const QString &iconName, const QString &tip, bool allowNone, const QString &noneLabel, std::function<void(const ColorRef &)> apply,
                           const ColorRef &initial) {
        auto *b = new ColorButton(iconName, tip, allowNone, noneLabel);
        b->setCurrent(initial);
        connect(b, &ColorButton::colorPicked, this, [apply, this](const ColorRef &c) { apply(c); m_canvas->setFocus(); });
        m_colorButtons << b;
        return b;
    };
    parts.widgets[QStringLiteral("textColor")] = [this, colorBtn]() -> QWidget * {
        auto *b = colorBtn("baseline", tr("Font Color"), true, tr("Automatic"), [this](const ColorRef &c) { m_ed->setTextColor(c); },
                           ColorRef::rgb(QColor(0xC0, 0, 0)));
        auto *tints = new QAction(tr("Tints…"), b);
        connect(tints, &QAction::triggered, this, [this] { tintsDialog(this, m_ed, [this](const ColorRef &c) { m_ed->setTextColor(c); }); });
        b->setExtraActions({tints});
        return b;
    };
    parts.widgets[QStringLiteral("shapeFill")] = [this, colorBtn]() -> QWidget * {
        auto *b = colorBtn("paint-bucket", tr("Shape Fill"), true, tr("No Fill"),
                           [this](const ColorRef &c) { m_ed->forEachSelected(tr("Fill"), [c](Item *it) { it->fill = c.isNone() ? Fill() : Fill::solid(c, it->fill.transparency); }); },
                           ColorRef::scheme(Accent1));
        b->setExtraActions({act("fill.picture"), act("fill.effects")});
        return b;
    };
    parts.widgets[QStringLiteral("shapeOutline")] = [this, colorBtn]() -> QWidget * {
        auto *b = colorBtn("pen-line", tr("Shape Outline"), true, tr("No Outline"), [this](const ColorRef &c) {
            m_ed->forEachSelected(tr("Outline"), [c](Item *it) {
                if (it->stroke.width <= 0) it->stroke.width = 0.75;
                it->stroke.color = c;
            });
        }, ColorRef::scheme(Main));
        b->setExtraActions({act("line.more")});
        return b;
    };

    // Text Box Tools: fill, outline and glow for the text itself.
    parts.widgets[QStringLiteral("textFill")] = [this]() -> QWidget * {
        auto *fill = new ColorButton("paint-bucket", tr("Text Fill"), false, QString());
        connect(fill, &ColorButton::colorPicked, this, [this](const ColorRef &c) { m_ed->setTextColor(c); });
        auto *gradient = new QAction(icon("blend"), tr("Gradient…"), this);
        connect(gradient, &QAction::triggered, this, [this] { textGradientDialog(this, m_ed); });
        fill->setExtraActions({gradient});
        m_colorButtons << fill;
        return fill;
    };
    parts.widgets[QStringLiteral("textOutline")] = [this]() -> QWidget * {
        auto *outline = new ColorButton("pen-line", tr("Text Outline"), true, tr("No Outline"));
        connect(outline, &ColorButton::colorPicked, this, [this](const ColorRef &c) {
            if (c.isNone()) m_ed->clearCharProperty(tp::OutlineRef, tr("Text Outline"));
            else m_ed->setCharProperty(tp::OutlineRef, c.toString(), tr("Text Outline"));
        });
        m_colorButtons << outline;
        return outline;
    };
    parts.widgets[QStringLiteral("textGlow")] = [this]() -> QWidget * {
        auto *glow = new ColorButton("sparkles", tr("Text Glow"), true, tr("No Glow"));
        connect(glow, &ColorButton::colorPicked, this, [this](const ColorRef &c) {
            if (c.isNone()) m_ed->clearCharProperty(tp::GlowRef, tr("Text Glow"));
            else m_ed->setCharProperty(tp::GlowRef, c.toString(), tr("Text Glow"));
        });
        m_colorButtons << glow;
        return glow;
    };

    parts.widgets[QStringLiteral("pictureBorder")] = [this]() -> QWidget * {
        auto *border = new ColorButton("square", tr("Picture Border"), true, tr("No Outline"));
        connect(border, &ColorButton::colorPicked, this, [this](const ColorRef &c) {
            m_ed->forEachSelected(tr("Picture Border"), [c](Item *it) { if (it->stroke.width <= 0) it->stroke.width = 1; it->stroke.color = c; });
        });
        m_colorButtons << border;
        return border;
    };
    parts.widgets[QStringLiteral("cellFill")] = [this]() -> QWidget * {
        auto *fill = new ColorButton("paint-bucket", tr("Cell Fill"), true, tr("No Fill"));
        connect(fill, &ColorButton::colorPicked, this, [this](const ColorRef &c) {
            TableItem *tb = selTableForUi(m_ed);
            if (!tb) return;
            const bool inCell = m_ed->isEditingText();
            const auto tt = m_ed->textTarget();
            m_ed->change(tr("Fill"), [&] {
                for (int rr = 0; rr < tb->rows; ++rr)
                    for (int cc = 0; cc < tb->cols; ++cc)
                        if (!inCell || (rr == tt.row && cc == tt.col)) tb->cell(rr, cc).fill = c.isNone() ? Fill() : Fill::solid(c);
            });
        });
        m_colorButtons << fill;
        return fill;
    };
    parts.widgets[QStringLiteral("borderColor")] = [this]() -> QWidget * {
        auto *bc = new ColorButton("pen-line", tr("Line Color"), false, QString());
        connect(bc, &ColorButton::colorPicked, this, [](const ColorRef &c) { g_borderStroke.color = c; });
        m_colorButtons << bc;
        return bc;
    };
    parts.widgets[QStringLiteral("borderWeight")] = [this]() -> QWidget * {
        auto *weightMenu = new QMenu(this);
        for (double w : {0.25, 0.5, 0.75, 1.0, 1.5, 2.25, 3.0, 4.5}) {
            QAction *a = weightMenu->addAction(tr("%1 pt").arg(w));
            connect(a, &QAction::triggered, this, [w] { g_borderStroke.width = w; });
        }
        auto *wb = new QToolButton();
        wb->setText(tr("Line Weight"));
        wb->setToolTip(tr("The weight of the border lines that Borders draws."));
        wb->setIcon(icon("minus"));
        wb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        wb->setPopupMode(QToolButton::InstantPopup);
        wb->setMenu(weightMenu);
        wb->setAutoRaise(true);
        return wb;
    };

    // Size boxes: the same pair in every tab that has a Size group.
    parts.widgets[QStringLiteral("heightSpin")] = [this]() -> QWidget * {
        auto *h = new MeasureSpin();
        h->setToolTip(tr("Shape Height"));
        m_heightSpins << h;
        connect(h, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (v <= 0) return;
            m_ed->forEachSelected(tr("Size"), [v](Item *it) {
                if (it->type() == ItemType::Line || it->locked) return;
                const QRectF b = it->bounds();
                QRectF to = b;
                to.setHeight(v);
                if (it->rotation == 0 && it->type() != ItemType::Group) { const double k = v / it->rect.height(); it->scaleInto(it->rect, QRectF(it->rect.topLeft(), QSizeF(it->rect.width(), it->rect.height() * k))); }
                else it->scaleInto(b, to);
            });
        });
        return h;
    };
    parts.widgets[QStringLiteral("widthSpin")] = [this]() -> QWidget * {
        auto *w = new MeasureSpin();
        w->setToolTip(tr("Shape Width"));
        m_widthSpins << w;
        connect(w, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (v <= 0) return;
            m_ed->forEachSelected(tr("Size"), [v](Item *it) {
                if (it->type() == ItemType::Line || it->locked) return;
                const QRectF b = it->bounds();
                QRectF to = b;
                to.setWidth(v);
                if (it->rotation == 0 && it->type() != ItemType::Group) { const double k = v / it->rect.width(); it->scaleInto(it->rect, QRectF(it->rect.topLeft(), QSizeF(it->rect.width() * k, it->rect.height()))); }
                else it->scaleInto(b, to);
            });
        });
        return w;
    };

    parts.widgets[QStringLiteral("zoomBox")] = [this]() -> QWidget * {
        auto *zoomBox = new QComboBox();
        zoomBox->setAccessibleName(tr("Zoom"));
        zoomBox->setToolTip(tr("Zoom"));
        zoomBox->setEditable(true);
        for (const char *z : {"400%", "300%", "200%", "150%", "100%", "75%", "66%", "50%", "33%", "25%", "10%"}) zoomBox->addItem(QString::fromLatin1(z));
        zoomBox->setFixedWidth(80);
        zoomBox->setCurrentText(QStringLiteral("100%"));
        connect(zoomBox, &QComboBox::textActivated, this, [this](const QString &s) {
            const double z = QString(s).remove('%').toDouble() / 100.0;
            if (z > 0) { m_canvas->zoomToFit(Canvas::Fit::None); m_canvas->setZoom(z); }
        });
        connect(m_canvas, &Canvas::zoomChanged, zoomBox, [zoomBox](double z) { if (!zoomBox->hasFocus()) zoomBox->setCurrentText(QStringLiteral("%1%").arg(std::lround(z * 100))); });
        return zoomBox;
    };

    // Insert > Table: a hover grid, then the dialog (and, on Home, drawing).
    auto tableButton = [this](bool withDraw) -> QWidget * {
        auto *tableBtn = ribbonButton(new QAction(icon("table"), tr("Table"), this), true, nullptr);
        auto *m = new QMenu(tableBtn);
        auto *grid = new TableGrid();
        auto *wa = new QWidgetAction(m);
        wa->setDefaultWidget(grid);
        m->addAction(wa);
        m->addAction(act("ins.tableDialog"));
        if (withDraw) m->addAction(act("ins.drawTable"));
        connect(grid, &TableGrid::picked, this, [this, m](int rows, int cols) {
            m->close();
            const QSizeF ps = m_ed->doc()->pageSize();
            const double w = std::min(ps.width() * 0.7, 72.0 * cols), h = 22.0 * rows;
            auto tb = m_ed->newTable(QRectF((ps.width() - w) / 2, (ps.height() - h) / 2, w, h), rows, cols);
            m_ed->addItem(tb);
        });
        tableBtn->setMenu(m);
        tableBtn->setPopupMode(QToolButton::InstantPopup);
        tableBtn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        tableBtn->setIconSize(QSize(30, 30));
        tableBtn->setAutoRaise(true);
        tableBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        return tableBtn;
    };
    parts.widgets[QStringLiteral("tableButton")] = [tableButton] { return tableButton(true); };
    parts.widgets[QStringLiteral("tableButtonPlain")] = [tableButton] { return tableButton(false); };
}

// Galleries: the inline ones that fill a group and the buttons that open a grid.
void MainWindow::ribbonGalleryParts(RibbonParts &parts)
{
    auto shapeItems = []() {
        QVector<GalleryItem> v;
        const QString lines = tr("Lines");
        v << GalleryItem{"tool:line", tr("Line"), icon("minus"), lines} << GalleryItem{"tool:arrow", tr("Arrow"), icon("move-right"), lines}
          << GalleryItem{"tool:double", tr("Double Arrow"), icon("move-horizontal"), lines};
        for (const char *route : {"elbow", "curved"}) {
            const QString r = QLatin1String(route), name = r == QLatin1String("elbow") ? tr("Elbow") : tr("Curved");
            v << GalleryItem{"tool:" + r, tr("Connector: %1").arg(name), lineToolIcon(r), lines}
              << GalleryItem{"tool:" + r + "Arrow", tr("Connector: %1 Arrow").arg(name), lineToolIcon(r + "Arrow"), lines}
              << GalleryItem{"tool:" + r + "Double", tr("Connector: %1 Double-Arrow").arg(name), lineToolIcon(r + "Double"), lines};
        }
        v << GalleryItem{"tool:curve", tr("Curve"), lineToolIcon("curve"), lines} << GalleryItem{"tool:freeform", tr("Freeform: Shape"), lineToolIcon("freeform"), lines}
          << GalleryItem{"tool:scribble", tr("Freeform: Scribble"), lineToolIcon("scribble"), lines};
        for (const auto &s : shapeLibrary()) v << GalleryItem{s.id, s.name, shapeIcon(s.id), s.category};
        return v;
    };
    parts.widgets[QStringLiteral("shapesButton")] = [this, shapeItems]() -> QWidget * {
        auto *b = new GalleryButton(icon("shapes"), tr("Shapes"), QSize(24, 24), 12, true);
        b->setToolTip(tr("Shapes"));
        b->setItemsProvider(shapeItems);
        connect(b, &GalleryButton::activated, this, [this](const QString &id) {
            if (id == "tool:line") m_ed->setTool(Tool::Line);
            else if (id == "tool:arrow") m_ed->setTool(Tool::Arrow);
            else if (id == "tool:double") m_ed->setTool(Tool::DoubleArrow);
            else if (id.startsWith(QLatin1String("tool:elbow")) || id.startsWith(QLatin1String("tool:curved"))) {
                // Connectors: the line tools with an elbow or curved route.
                const QString route = id.startsWith(QLatin1String("tool:elbow")) ? tr("elbow") : tr("curved");
                m_ed->setTool(id.endsWith(QLatin1String("Double")) ? Tool::DoubleArrow : id.endsWith(QLatin1String("Arrow")) ? Tool::Arrow : Tool::Line, route);
            } else if (id == "tool:curve" || id == "tool:freeform" || id == "tool:scribble") {
                m_ed->setTool(Tool::Freeform, id.mid(5));
            } else m_ed->setTool(Tool::Shape, id);
        });
        return b;
    };

    // Picture Shape and Crop to Shape set the same thing: the closed shape a picture is trimmed to.
    auto maskShapeButton = [this](const char *iconName, const QString &label) {
        auto *b = new GalleryButton(icon(iconName), label, QSize(24, 24), 12, false);
        b->setItemsProvider([] {
            QVector<GalleryItem> v;
            for (const auto &s : shapeLibrary()) if (!s.open) v << GalleryItem{s.id, s.name, shapeIcon(s.id), s.category};
            return v;
        });
        connect(b, &GalleryButton::activated, this, [this, label](const QString &id) {
            m_ed->forEachSelected(label, [id](Item *it) { if (auto *pic = dynamic_cast<PictureItem *>(it)) pic->maskShape = id; });
        });
        return b;
    };
    parts.widgets[QStringLiteral("pictureShapeButton")] = [maskShapeButton]() -> QWidget * { return maskShapeButton("shapes", tr("Picture Shape")); };
    parts.widgets[QStringLiteral("cropToShapeButton")] = [maskShapeButton]() -> QWidget * { return maskShapeButton("crop", tr("Crop to Shape")); };
    parts.widgets[QStringLiteral("changeShapeButton")] = [this]() -> QWidget * {
        auto *change = new GalleryButton(icon("shapes"), tr("Change Shape"), QSize(24, 24), 12, false);
        change->setItemsProvider([] {
            QVector<GalleryItem> v;
            for (const auto &s : shapeLibrary()) v << GalleryItem{s.id, s.name, shapeIcon(s.id), s.category};
            return v;
        });
        connect(change, &GalleryButton::activated, this, [this](const QString &id) {
            m_ed->forEachSelected(tr("Change Shape"), [id](Item *it) {
                if (auto *s = dynamic_cast<ShapeItem *>(it)) { s->shape = id; s->adj.clear(); s->customPath = QPainterPath(); }
                if (auto *pic = dynamic_cast<PictureItem *>(it)) pic->maskShape = id;
            });
        });
        return change;
    };

    // Home > Styles.
    parts.widgets[QStringLiteral("styleGallery")] = [this]() -> QWidget * {
        m_styleGallery = new Gallery(QSize(76, 46), 3);
        m_styleGallery->setItemsProvider([this] {
            QVector<GalleryItem> v;
            for (const auto &s : m_ed->doc()->styles) {
                const QString name = s.name;
                Editor *e = m_ed;
                v << GalleryItem{name, name, drawnIcon([e, name](QPainter *p, const QRectF &rc) { drawStylePreview(p, rc, e, name, true); }), QString()};
            }
            return v;
        });
        m_styleGallery->setFooterActions({act("style.new"), act("style.modify"), act("style.byExample"), act("style.import")});
        connect(m_styleGallery, &Gallery::activated, this, [this](const QString &id) { m_ed->applyStyle(id); m_canvas->setFocus(); });
        return m_styleGallery;
    };

    // Insert > Building Blocks.
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
            m_ed->beginChange(tr("Insert Building Block"));
            const QRectF content = QRectF(QPointF(0, 0), m_ed->doc()->pageSize()).marginsRemoved(m_ed->doc()->setup.margins);
            ItemList items = blk->build(*m_ed->doc(), content);
            QStringList ids;
            for (auto &it : items) { m_ed->surfaceItems().push_back(it); ids << it->id; }
            m_ed->endChange();
            m_ed->select(ids);
        });
        return b;
    };
    parts.widgets[QStringLiteral("blockPageParts")] = [blockButton]() -> QWidget * {
        return blockButton(QStringLiteral("Page Parts"), tr("Page Parts"), QStringLiteral("layout-template"));
    };
    parts.widgets[QStringLiteral("blockCalendars")] = [this, blockButton]() -> QWidget * {
        auto *cal = blockButton(QStringLiteral("Calendars"), tr("Calendars"), QStringLiteral("calendar-days"));
        auto *more = new QAction(tr("More Calendars…"), cal);
        connect(more, &QAction::triggered, this, [this] { calendarDialog(this, m_ed); });
        cal->setFooterActions({more});
        return cal;
    };
    parts.widgets[QStringLiteral("blockBorders")] = [blockButton]() -> QWidget * {
        return blockButton(QStringLiteral("Borders & Accents"), tr("Borders & Accents"), QStringLiteral("frame"));
    };
    parts.widgets[QStringLiteral("blockAdvertisements")] = [blockButton]() -> QWidget * {
        return blockButton(QStringLiteral("Advertisements"), tr("Advertisements"), QStringLiteral("badge-percent"));
    };

    // Insert > Text > Text Art.
    parts.widgets[QStringLiteral("textArtButton")] = [this]() -> QWidget * {
        auto *wa = new GalleryButton(icon("type"), tr("Text Art"), QSize(64, 40), 6, true);
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
        return wa;
    };

    // Page Design > Schemes and Page Background.
    parts.widgets[QStringLiteral("schemeGallery")] = [this]() -> QWidget * {
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
            if (s) m_ed->change(tr("Color Scheme"), [&] { m_ed->doc()->colors = *s; });
        });
        return m_schemeGallery;
    };
    parts.widgets[QStringLiteral("backgroundButton")] = [this]() -> QWidget * {
        auto *bg = new GalleryButton(icon("paint-roller"), tr("Background"), QSize(48, 48), 6, true);
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
            add("none", tr("No Background"), tr("No Background"));
            for (int s = 1; s <= 5; ++s) for (int t : {0, 40, 80}) add(QStringLiteral("solid.%1.%2").arg(s).arg(t), QStringLiteral("%1 %2%").arg(slotName(s)).arg(100 - t), tr("Solid Background"));
            for (int s = 1; s <= 5; ++s) for (int k = 0; k < 3; ++k) add(QStringLiteral("grad.%1.%2").arg(s).arg(k), tr("%1 Gradient").arg(slotName(s)), tr("Gradient Background"));
            return v;
        });
        bg->setFooterActions({act("pd.bgMore"), act("pd.bgAllPages")});
        connect(bg, &GalleryButton::activated, this, [this](const QString &id) {
            const Fill f = backgroundPreset(id);
            m_ed->change(tr("Background"), [&] { m_ed->surface()->background = f; });
        });
        return bg;
    };

    // The contextual tabs' inline galleries. refreshUi() finds them by name.
    parts.widgets[QStringLiteral("shapeStyles")] = [this]() -> QWidget * {
        auto *gal = new Gallery(QSize(40, 28), 6);
        Editor *e = m_ed;
        gal->setItemsProvider([e] {
            QVector<GalleryItem> v;
            for (int row = 0; row < 6; ++row)
                for (int slot = 0; slot < 6; ++slot) {
                    const QString id = QStringLiteral("%1.%2").arg(row).arg(slot);
                    v << GalleryItem{id, tr("%1 style %2").arg(slotName(slot)).arg(row + 1), drawnIcon([e, row, slot](QPainter *p, const QRectF &rc) {
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
            m_ed->forEachSelected(tr("Shape Style"), [row, slot](Item *it) { shapeStylePreset(row, slot, &it->fill, &it->stroke, &it->fx); });
        });
        gal->setObjectName("shapeStyles");
        return gal;
    };
    parts.widgets[QStringLiteral("pictureStyles")] = [this]() -> QWidget * {
        auto *gal = new Gallery(QSize(40, 40), 5);
        Editor *e = m_ed;
        gal->setItemsProvider([e] {
            QVector<GalleryItem> v;
            for (int k = 0; k < 20; ++k) {
                v << GalleryItem{QString::number(k), tr("Picture Style %1").arg(k + 1), drawnIcon([e, k](QPainter *p, const QRectF &rc) {
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
            m_ed->forEachSelected(tr("Picture Style"), [id](Item *it) {
                if (auto *pic = dynamic_cast<PictureItem *>(it)) pictureStylePreset(id.toInt(), pic);
            });
        });
        gal->setObjectName("pictureStyles");
        return gal;
    };
    parts.widgets[QStringLiteral("tableFormats")] = [this]() -> QWidget * {
        auto *gal = new Gallery(QSize(48, 36), 5);
        Editor *e = m_ed;
        gal->setItemsProvider([e] {
            QVector<GalleryItem> v;
            // Each format is known by its English name; the tile shows it translated.
            QVector<QPair<QString, QString>> names{{"None", tr("None")}};
            for (int i = 1; i <= 20; ++i) names << qMakePair(QStringLiteral("Table Style %1").arg(i), tr("Table Style %1").arg(i));
            names << QPair<QString, QString>{"Basic 1", tr("Basic 1")} << QPair<QString, QString>{"Basic 2", tr("Basic 2")} << QPair<QString, QString>{"Basic 3", tr("Basic 3")}
                  << QPair<QString, QString>{"Checkbook Register", tr("Checkbook Register")} << QPair<QString, QString>{"List 1", tr("List 1")}
                  << QPair<QString, QString>{"List 2", tr("List 2")} << QPair<QString, QString>{"List 3", tr("List 3")}
                  << QPair<QString, QString>{"Numbers 1", tr("Numbers 1")} << QPair<QString, QString>{"Numbers 2", tr("Numbers 2")};
            for (const auto &nm : names) {
                const QString n = nm.first;
                v << GalleryItem{n, nm.second, drawnIcon([e, n](QPainter *p, const QRectF &rc) {
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
            if (tb) m_ed->change(tr("Table Format"), [&] { m_ed->applyTableFormat(tb, n); });
        });
        gal->setObjectName("tableFormats");
        return gal;
    };
    parts.widgets[QStringLiteral("textArtStyles")] = [this]() -> QWidget * {
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
            m_ed->forEachSelected(tr("Text Art Style"), [sid](Item *it) {
                if (auto *w = dynamic_cast<TextArtItem *>(it))
                    for (const auto &st : textArtStyles()) if (st.id == sid) { const QString text = w->text; applyTextArtStyle(*w, st); w->text = text; }
            });
        });
        gal->setObjectName("textartStyles");
        return gal;
    };
    parts.widgets[QStringLiteral("textArtShapeButton")] = [this]() -> QWidget * {
        auto *shape = new GalleryButton(icon("spline"), tr("Change Shape"), QSize(56, 36), 6, false);
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
            m_ed->forEachSelected(tr("Text Art Shape"), [id](Item *it) { if (auto *w = dynamic_cast<TextArtItem *>(it)) w->transform_ = id; });
        });
        return shape;
    };
}

// Menus the ribbon can't describe in data: ones that change each time they
// open, or that hold more than commands.
void MainWindow::ribbonMenuParts(RibbonParts &parts)
{
    parts.menus[QStringLiteral("businessInfoMenu")] = [this](QWidget *owner) {
        auto *m = new QMenu(owner);
        for (const QString &k : BusinessInfo::keys()) m->addAction(act("biz." + k));
        m->addAction(act("biz.logo"));
        m->addSeparator();
        m->addAction(act("ins.bizInfo"));
        return m;
    };

    // Symbol: the recently used symbols to click, then More Symbols.
    parts.menus[QStringLiteral("symbolMenu")] = [this](QWidget *owner) {
        auto *sm = new QMenu(owner);
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
            QAction *more = sm->addAction(icon("omega"), tr("More Symbols…"));
            connect(more, &QAction::triggered, this, [this] { symbolDialog(this, m_ed); });
        });
        return sm;
    };

    // Built each time it opens: the preset sizes, then the user's own
    // (Custom), then creating and editing custom sizes and Page Setup.
    parts.menus[QStringLiteral("pageSizeMenu")] = [this](QWidget *owner) {
        auto *sizeMenu = new QMenu(owner);
        connect(sizeMenu, &QMenu::aboutToShow, this, [this, sizeMenu] {
            sizeMenu->clear();
            auto applySize = [this](const QSizeF &s, const QString &name) {
                m_ed->change(tr("Page Size"), [&] {
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
                QAction *a = sizeMenu->addAction(tr("%1 (%2 × %3)").arg(bs.name, Settings::get().format(bs.size.width()), Settings::get().format(bs.size.height())));
                a->setCheckable(true);
                a->setChecked(m_ed->doc()->setup.sizeName == bs.name);
                const QSizeF s = bs.size;
                const QString name = bs.name;
                connect(a, &QAction::triggered, this, [applySize, s, name] { applySize(s, name); });
            }
            const auto custom = customPageSizes();
            if (!custom.isEmpty()) {
                sizeMenu->addSection(tr("Custom"));
                for (const auto &c : custom) {
                    QAction *a = sizeMenu->addAction(tr("%1 (%2 × %3)").arg(c.first, Settings::get().format(c.second.size.width()), Settings::get().format(c.second.size.height())));
                    a->setCheckable(true);
                    a->setChecked(m_ed->doc()->setup.sizeName == c.first);
                    const PageSetup setup = c.second;
                    const QString name = c.first;
                    connect(a, &QAction::triggered, this, [this, setup, name] { applyPageSetup(m_ed, setup, name, tr("Page Size")); });
                }
            }
            sizeMenu->addSeparator();
            sizeMenu->addAction(act("pd.newPageSize"));
            if (!custom.isEmpty()) sizeMenu->addAction(act("pd.customSizes"));
            sizeMenu->addAction(act("pd.pageSetup"));
        });
        return sizeMenu;
    };

    parts.menus[QStringLiteral("guidesMenu")] = [this](QWidget *owner) {
        auto *gm = new QMenu(owner);
        const QVector<QPair<QString, QPair<int, int>>> presets = {{tr("No Grid"), {1, 1}}, {tr("2 Columns"), {2, 1}}, {tr("3 Columns"), {3, 1}}, {tr("4 Columns"), {4, 1}},
                                                                 {tr("2 × 2 Grid"), {2, 2}}, {tr("3 × 3 Grid"), {3, 3}}, {tr("4 × 4 Grid"), {4, 4}}};
        gm->addSection(tr("Built-In Guides"));
        for (const auto &pr : presets) {
            QAction *a = gm->addAction(pr.first);
            const int c = pr.second.first, rr = pr.second.second;
            connect(a, &QAction::triggered, this, [this, c, rr] {
                m_ed->change(tr("Guides"), [&] {
                    for (auto &m : m_ed->doc()->masters) { m->grid.cols = c; m->grid.rows = rr; }
                });
            });
        }
        gm->addSeparator();
        gm->addAction(act("pd.guidesDialog"));
        gm->addAction(act("pd.rulerGuides"));
        gm->addAction(act("pd.addH"));
        gm->addAction(act("pd.addV"));
        gm->addAction(act("pd.clearGuides"));
        return gm;
    };

    parts.menus[QStringLiteral("masterPagesMenu")] = [this](QWidget *owner) {
        auto *mpMenu = new QMenu(owner);
        connect(mpMenu, &QMenu::aboutToShow, this, [this, mpMenu] {
            mpMenu->clear();
            for (const auto &m : m_ed->doc()->masters) {
                QAction *a = mpMenu->addAction(tr("(%1) %2").arg(m->abbr, m->name));
                a->setCheckable(true);
                a->setChecked(m_ed->doc()->pages[m_ed->currentPage()]->masterId == m->id);
                const QString id = m->id;
                connect(a, &QAction::triggered, this, [this, id] { m_ed->applyMaster(m_ed->currentPage(), id); });
            }
            mpMenu->addAction(act("mp.none"));
            mpMenu->addSeparator();
            mpMenu->addAction(act("view.master"));
        });
        return mpMenu;
    };

    parts.menus[QStringLiteral("fontSchemesMenu")] = [this](QWidget *owner) {
        auto *fm = new QMenu(owner);
        for (const auto &fs : builtinFontSchemes()) {
            QAction *a = fm->addAction(tr("%1 — %2 / %3").arg(fs.name, fs.heading, fs.body));
            const FontScheme scheme = fs;
            connect(a, &QAction::triggered, this, [this, scheme] { m_ed->change(tr("Font Scheme"), [&] { m_ed->doc()->fonts = scheme; }); });
        }
        fm->addSeparator();
        fm->addAction(act("pd.newFontScheme"));
        fm->addAction(act("pd.updateFonts"));
        return fm;
    };

    parts.menus[QStringLiteral("mergeFieldMenu")] = [this](QWidget *owner) {
        auto *fieldMenu = new QMenu(owner);
        connect(fieldMenu, &QMenu::aboutToShow, this, [this, fieldMenu] {
            fieldMenu->clear();
            const QStringList fields = m_ed->doc()->merge.fields;
            if (fields.isEmpty()) fieldMenu->addAction(tr("(Select recipients first)"))->setEnabled(false);
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
        return fieldMenu;
    };

    parts.menus[QStringLiteral("switchWindowsMenu")] = [this](QWidget *owner) {
        auto *switchMenu = new QMenu(owner);
        connect(switchMenu, &QMenu::aboutToShow, this, [switchMenu] {
            switchMenu->clear();
            for (QWidget *w : QApplication::topLevelWidgets()) {
                auto *mw = qobject_cast<MainWindow *>(w);
                if (!mw || !mw->isVisible()) continue;
                QAction *a = switchMenu->addAction(mw->windowTitle());
                connect(a, &QAction::triggered, mw, [mw] { mw->raise(); mw->activateWindow(); });
            }
        });
        return switchMenu;
    };
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
    m_zoomSlider->setAccessibleName(tr("Zoom"));
    m_zoomSlider->setAccessibleDescription(tr("Left and Right arrows zoom out and in."));
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
    act("edit.undo")->setToolTip(tr("Undo %1 (Ctrl+Z)").arg(ed->undoStack()->undoText()));
    act("edit.redo")->setToolTip(tr("Redo %1 (Ctrl+Y)").arg(ed->undoStack()->redoText()));
    for (const char *id : {"edit.cut", "edit.copy", "edit.duplicate", "edit.delete"}) act(id)->setEnabled(sel || (editing && ed->cursor().hasSelection()));
    for (const char *id : {"arr.front", "arr.forward", "arr.backward", "arr.back", "arr.rotR", "arr.rotL", "arr.flipH", "arr.flipV", "obj.format", "obj.lock"})
        act(id)->setEnabled(sel);
    act("arr.group")->setEnabled(kind == "multi" || kind == "group");
    act("arr.group")->setText(kind == "group" ? tr("Ungroup") : tr("Group"));
    act("arr.ungroup")->setEnabled(kind == "group" || m_ed->canRegroup());
    act("arr.regroup")->setEnabled(m_ed->canRegroup());
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
        const bool inText = ed->selectionIsInlineObject();
        for (const auto &[id, mode] : {std::pair{"wrap.none", Wrap::None}, {"wrap.square", Wrap::Square}, {"wrap.tight", Wrap::Tight}, {"wrap.through", Wrap::Through},
                                       {"wrap.topBottom", Wrap::TopBottom}})
            act(id)->setChecked(!inText && one->wrap.mode == mode);
        act("wrap.inline")->setChecked(inText);
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
    {
        const auto *p = editing ? nullptr : dynamic_cast<PictureItem *>(one);
        act("pic.toShapes")->setEnabled(p && d->images.value(p->imageId).format == QLatin1String("svg"));
    }
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
            items << GalleryItem{name, name, drawnIcon([e, name](QPainter *p, const QRectF &rc) { drawStylePreview(p, rc, e, name, false); }), QString()};
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
        m_pageLabel->setText(tr("Master Page %1").arg(mp ? mp->abbr : QString()));
    } else {
        m_pageLabel->setText(tr("Page: %1 of %2").arg(ed->currentPage() + 1).arg(d->pages.size()));
    }
    Settings &st = Settings::get();
    m_sizeLabel->setText(sel ? tr("%1 × %2").arg(st.format(b.width()), st.format(b.height())) : QString());
    if (sel) m_posLabel->setText(QStringLiteral("%1, %2").arg(st.format(b.left()), st.format(b.top())));
    m_task->refresh();
}

} // namespace jp
