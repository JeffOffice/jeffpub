#include "app/dialogs.h"

#include "app/appfuncs.h"
#include "app/editor.h"
#include "app/icons.h"
#include "app/settings.h"
#include "app/widgets.h"
#include "core/presets.h"
#include "io/importers.h"
#include "render/textart.h"
#include "templates/templates.h"
#include "text/textprops.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCalendarWidget>
#include <QCheckBox>

#include <QComboBox>
#include <QJsonDocument>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTableWidget>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>
#include <QVBoxLayout>
#include <QBuffer>
#include <QStyleHints>
#include <QPointer>
#include <QClipboard>
#include <QMimeData>
#include <QTextDocumentFragment>

namespace jp {

namespace {

struct Dlg {
    QDialog d;
    QVBoxLayout *v;
    QDialogButtonBox *bb;
    explicit Dlg(QWidget *p, const QString &title, QDialogButtonBox::StandardButtons buttons = QDialogButtonBox::Ok | QDialogButtonBox::Cancel) : d(p)
    {
        d.setWindowTitle(title);
        v = new QVBoxLayout(&d);
        bb = new QDialogButtonBox(buttons, &d);
        QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    }
    bool exec()
    {
        v->addWidget(bb);
        return d.exec() == QDialog::Accepted;
    }
};

MeasureSpin *measure(double pt, QWidget *p, double min = 0, double max = 72 * 240)
{
    auto *m = new MeasureSpin(p);
    m->setRange(min, max);
    m->setValue(pt);
    m->setFixedWidth(110);
    return m;
}

QDoubleSpinBox *points(double v, QWidget *p, double min = 0, double max = 1000, const QString &suffix = QStringLiteral(" pt"))
{
    auto *s = new DecimalSpin(p);
    s->setRange(min, max);
    s->setValue(v);
    s->setSuffix(suffix);
    return s;
}

ColorButton *colorPick(Editor *ed, const ColorRef &c, bool allowNone, QWidget *p)
{
    auto *b = new ColorButton("square", QString(), allowNone, QStringLiteral("No Color"), p);
    b->setScheme(ed->doc()->colors);
    b->setCurrent(c);
    b->setPopupMode(QToolButton::InstantPopup);
    b->setWell(true);
    QObject::connect(b, &ColorButton::colorPicked, b, [b](const ColorRef &x) { b->setCurrent(x); });
    return b;
}

} // namespace

// ---------------- Page Setup ----------------
// ---------------- Page size, margins and margin guides ----------------
// The page fields Page Setup and Create New Page Size share. Margin guides
// are the non-printing guides on each page; margins and gaps place pages on
// the printed sheet when several share one (multiple pages per sheet, labels).
namespace {

struct PageForm {
    QComboBox *layout = nullptr, *preset = nullptr;
    MeasureSpin *w = nullptr, *h = nullptr, *sheetW = nullptr, *sheetH = nullptr;
    MeasureSpin *side = nullptr, *top = nullptr, *gapH = nullptr, *gapV = nullptr;
    MeasureSpin *gTop = nullptr, *gBottom = nullptr, *gLeft = nullptr, *gRight = nullptr;
    QLabel *fits = nullptr;
    QFormLayout *pageForm = nullptr;

    bool multiple() const
    {
        const int l = layout->currentIndex();
        return l == PageSetup::MultiplePerSheet || l == PageSetup::Labels;
    }
    // How many pages fit across and down the sheet.
    QPair<int, int> grid() const
    {
        const double cw = w->value() + gapH->value(), ch = h->value() + gapV->value();
        const int cols = cw > 0 ? std::max(1, int((sheetW->value() - 2 * side->value() + gapH->value() + 0.001) / cw)) : 1;
        const int rows = ch > 0 ? std::max(1, int((sheetH->value() - 2 * top->value() + gapV->value() + 0.001) / ch)) : 1;
        return qMakePair(cols, rows);
    }
    void update()
    {
        const bool m = multiple();
        const bool changed = pageForm->isRowVisible(side) != m;
        for (QWidget *f : {static_cast<QWidget *>(side), static_cast<QWidget *>(top), static_cast<QWidget *>(gapH), static_cast<QWidget *>(gapV),
                           static_cast<QWidget *>(fits)})
            pageForm->setRowVisible(f, m);
        if (m) {
            const auto g = grid();
            fits->setText(QStringLiteral("%1 across × %2 down = %3 per sheet").arg(g.first).arg(g.second).arg(g.first * g.second));
        }
        // Grow or shrink the dialog with the rows shown, so none are squeezed.
        if (QWidget *win = fits->window(); win && (changed || win->width() < win->sizeHint().width()))
            QTimer::singleShot(0, win, [win] { win->adjustSize(); });
    }
    void read(PageSetup &s) const
    {
        s.layout = PageSetup::Layout(layout->currentIndex());
        s.size = QSizeF(w->value(), h->value());
        s.sheet = QSizeF(sheetW->value(), sheetH->value());
        if (s.layout == PageSetup::OnePerSheet && (s.sheet.width() < s.size.width() || s.sheet.height() < s.size.height())) s.sheet = s.size;
        s.margins = QMarginsF(gLeft->value(), gTop->value(), gRight->value(), gBottom->value());
        s.sideMargin = side->value();
        s.topMargin = top->value();
        s.gapH = gapH->value();
        s.gapV = gapV->value();
        const auto g = grid();
        s.gridCols = multiple() ? g.first : 1;
        s.gridRows = multiple() ? g.second : 1;
    }
};

// Builds the Page and Margin Guides tabs into `tabs` from a page setup.
PageForm buildPageForm(QTabWidget *tabs, const PageSetup &s)
{
    PageForm f;
    auto *page = new QWidget();
    auto *form = new QFormLayout(page);
    f.pageForm = form;
    f.layout = new QComboBox(page);
    f.layout->addItems({"One page per sheet", "Booklet", "Multiple pages per sheet", "Envelope", "Folded card", "Labels"});
    f.layout->setCurrentIndex(int(s.layout));
    f.preset = new QComboBox(page);
    f.preset->addItem(QStringLiteral("Custom"));
    for (const auto &bs : blankSizes()) f.preset->addItem(bs.name);
    const QJsonArray custom = Settings::get().customPageSizes();
    for (const QJsonValue &c : custom) f.preset->addItem(c.toObject()["name"].toString(), QStringLiteral("custom"));
    f.w = measure(s.size.width(), page, 9);
    f.h = measure(s.size.height(), page, 9);
    f.sheetW = measure(s.sheet.width(), page, 9);
    f.sheetH = measure(s.sheet.height(), page, 9);
    f.side = measure(s.sideMargin, page);
    f.top = measure(s.topMargin, page);
    f.gapH = measure(s.gapH, page);
    f.gapV = measure(s.gapV, page);
    f.fits = new QLabel(page);
    f.layout->setObjectName("pageLayout");
    f.w->setObjectName("pageWidth");
    f.h->setObjectName("pageHeight");
    f.sheetW->setObjectName("paperWidth");
    f.sheetH->setObjectName("paperHeight");
    f.side->setObjectName("sideMargin");
    f.top->setObjectName("topMargin");
    f.gapH->setObjectName("gapH");
    f.gapV->setObjectName("gapV");
    f.fits->setObjectName("perSheet");
    auto *portrait = new QRadioButton(QStringLiteral("Portrait"), page), *land = new QRadioButton(QStringLiteral("Landscape"), page);
    (s.size.width() > s.size.height() ? land : portrait)->setChecked(true);
    MeasureSpin *w = f.w, *h = f.h;
    auto swapIf = [=](bool landscape) {
        if ((w->value() > h->value()) != landscape) {
            const double t = w->value();
            w->setValue(h->value());
            h->setValue(t);
        }
    };
    QObject::connect(portrait, &QRadioButton::toggled, page, [=](bool on) { if (on) swapIf(false); });
    QObject::connect(land, &QRadioButton::toggled, page, [=](bool on) { if (on) swapIf(true); });
    auto *orient = new QHBoxLayout();
    orient->addWidget(portrait);
    orient->addWidget(land);
    form->addRow(QStringLiteral("Layout type:"), f.layout);
    form->addRow(QStringLiteral("Page size:"), f.preset);
    form->addRow(QStringLiteral("Width:"), f.w);
    form->addRow(QStringLiteral("Height:"), f.h);
    form->addRow(QStringLiteral("Orientation:"), orient);
    form->addRow(QStringLiteral("Paper width:"), f.sheetW);
    form->addRow(QStringLiteral("Paper height:"), f.sheetH);
    form->addRow(QStringLiteral("Side margin:"), f.side);
    form->addRow(QStringLiteral("Top margin:"), f.top);
    form->addRow(QStringLiteral("Horizontal gap:"), f.gapH);
    form->addRow(QStringLiteral("Vertical gap:"), f.gapV);
    form->addRow(QString(), f.fits);
    tabs->addTab(page, QStringLiteral("Page"));

    auto *mg = new QWidget();
    auto *mf = new QFormLayout(mg);
    auto *note = new QLabel(QStringLiteral("Margin guides are non-printing lines on every page for lining up objects."), mg);
    note->setWordWrap(true);
    mf->addRow(note);
    f.gTop = measure(s.margins.top(), mg);
    f.gBottom = measure(s.margins.bottom(), mg);
    f.gLeft = measure(s.margins.left(), mg);
    f.gRight = measure(s.margins.right(), mg);
    mf->addRow(QStringLiteral("Top:"), f.gTop);
    mf->addRow(QStringLiteral("Bottom:"), f.gBottom);
    mf->addRow(QStringLiteral("Left:"), f.gLeft);
    mf->addRow(QStringLiteral("Right:"), f.gRight);
    tabs->addTab(mg, QStringLiteral("Margin Guides"));

    // Choosing a size fills the fields; a custom size brings its whole setup.
    PageForm *pf = new PageForm(f);
    QObject::connect(page, &QObject::destroyed, [pf] { delete pf; });
    QObject::connect(f.preset, &QComboBox::currentIndexChanged, page, [pf](int i) {
        if (i <= 0) return;
        const int presets = int(blankSizes().size());
        if (i <= presets) {
            const auto &bs = blankSizes()[i - 1];
            pf->w->setValue(bs.size.width());
            pf->h->setValue(bs.size.height());
        } else {
            const PageSetup cs = PageSetup::fromJson(Settings::get().customPageSizes().at(i - 1 - presets).toObject()["setup"].toObject());
            pf->layout->setCurrentIndex(int(cs.layout));
            pf->w->setValue(cs.size.width());
            pf->h->setValue(cs.size.height());
            pf->sheetW->setValue(cs.sheet.width());
            pf->sheetH->setValue(cs.sheet.height());
            pf->side->setValue(cs.sideMargin);
            pf->top->setValue(cs.topMargin);
            pf->gapH->setValue(cs.gapH);
            pf->gapV->setValue(cs.gapV);
            pf->gTop->setValue(cs.margins.top());
            pf->gBottom->setValue(cs.margins.bottom());
            pf->gLeft->setValue(cs.margins.left());
            pf->gRight->setValue(cs.margins.right());
        }
        pf->update();
    });
    for (MeasureSpin *m : {f.w, f.h, f.sheetW, f.sheetH, f.side, f.top, f.gapH, f.gapV})
        QObject::connect(m, &QDoubleSpinBox::valueChanged, page, [pf] { pf->update(); });
    QObject::connect(f.layout, &QComboBox::currentIndexChanged, page, [pf] { pf->update(); });
    pf->update();
    return f;
}

} // namespace

void applyPageSetup(Editor *ed, const PageSetup &ns, const QString &sizeName, const QString &undoName)
{
    Document *doc = ed->doc();
    ed->change(undoName, [&] {
        const QSizeF oldSize = doc->setup.size;
        const PageSetup::Fold fold = doc->setup.fold;
        doc->setup = ns;
        doc->setup.fold = fold;
        doc->setup.sizeName = sizeName;
        // Keep objects in proportion when the page size changes (reflow).
        if (oldSize != doc->setup.size && oldSize.width() > 0) {
            const QRectF from(QPointF(0, 0), oldSize), to(QPointF(0, 0), doc->setup.size);
            for (auto &pg : doc->pages) for (auto &it : pg->items) it->scaleInto(from, to);
            for (auto &mp : doc->masters) for (auto &it : mp->items) it->scaleInto(from, to);
        }
    });
}

void pageSetupDialog(QWidget *p, Editor *ed)
{
    Document *doc = ed->doc();
    Dlg dlg(p, QStringLiteral("Page Setup"));
    auto *tabs = new QTabWidget(&dlg.d);
    const PageForm f = buildPageForm(tabs, doc->setup);
    int pi = f.preset->findText(doc->setup.sizeName);
    f.preset->blockSignals(true);
    f.preset->setCurrentIndex(pi > 0 ? pi : 0);
    f.preset->blockSignals(false);
    dlg.v->addWidget(tabs);
    if (!dlg.exec()) return;
    PageSetup ns = doc->setup;
    f.read(ns);
    applyPageSetup(ed, ns, f.preset->currentIndex() > 0 ? f.preset->currentText() : QStringLiteral("Custom"), QStringLiteral("Page Setup"));
}

QVector<QPair<QString, PageSetup>> customPageSizes()
{
    QVector<QPair<QString, PageSetup>> out;
    for (const QJsonValue &v : Settings::get().customPageSizes())
        out << qMakePair(v.toObject()["name"].toString(), PageSetup::fromJson(v.toObject()["setup"].toObject()));
    return out;
}

bool createPageSizeDialog(QWidget *p, Editor *ed, int editIndex, bool apply)
{
    QJsonArray sizes = Settings::get().customPageSizes();
    const bool editing = editIndex >= 0 && editIndex < sizes.size();
    PageSetup start = editing ? PageSetup::fromJson(sizes.at(editIndex).toObject()["setup"].toObject()) : ed->doc()->setup;
    Dlg dlg(p, editing ? QStringLiteral("Edit Page Size") : QStringLiteral("Create New Page Size"));
    auto *nameRow = new QFormLayout();
    auto *name = new QLineEdit(editing ? sizes.at(editIndex).toObject()["name"].toString() : QStringLiteral("Custom %1").arg(sizes.size() + 1), &dlg.d);
    name->setObjectName("pageSizeName");
    nameRow->addRow(QStringLiteral("Name:"), name);
    dlg.v->addLayout(nameRow);
    auto *tabs = new QTabWidget(&dlg.d);
    const PageForm f = buildPageForm(tabs, start);
    f.pageForm->setRowVisible(f.preset, false);
    dlg.v->addWidget(tabs);
    if (!dlg.exec()) return false;
    PageSetup ns = start;
    f.read(ns);
    const QString n = name->text().trimmed().isEmpty() ? QStringLiteral("Custom") : name->text().trimmed();
    const QJsonObject entry{{"name", n}, {"setup", ns.toJson()}};
    if (editing) sizes.replace(editIndex, entry);
    else sizes.append(entry);
    Settings::get().setCustomPageSizes(sizes);
    if (!editing && apply) applyPageSetup(ed, ns, n, QStringLiteral("Page Size"));
    return true;
}

void customPageSizesDialog(QWidget *p, Editor *ed)
{
    Dlg dlg(p, QStringLiteral("Custom Page Sizes"));
    auto *list = new QListWidget(&dlg.d);
    auto fill = [list] {
        list->clear();
        for (const auto &c : customPageSizes())
            list->addItem(QStringLiteral("%1 (%2 × %3)").arg(c.first, Settings::get().format(c.second.size.width()), Settings::get().format(c.second.size.height())));
        list->setCurrentRow(0);
    };
    fill();
    auto *row = new QHBoxLayout();
    auto *add = new QPushButton(QStringLiteral("New…"), &dlg.d), *edit = new QPushButton(QStringLiteral("Edit…"), &dlg.d),
         *del = new QPushButton(QStringLiteral("Delete"), &dlg.d), *use = new QPushButton(QStringLiteral("Use This Size"), &dlg.d);
    for (QPushButton *b : {add, edit, del, use}) row->addWidget(b);
    QObject::connect(add, &QPushButton::clicked, &dlg.d, [&] { if (createPageSizeDialog(&dlg.d, ed)) fill(); });
    QObject::connect(edit, &QPushButton::clicked, &dlg.d, [&] { if (list->currentRow() >= 0 && createPageSizeDialog(&dlg.d, ed, list->currentRow())) fill(); });
    QObject::connect(del, &QPushButton::clicked, &dlg.d, [&] {
        const int r = list->currentRow();
        QJsonArray sizes = Settings::get().customPageSizes();
        if (r < 0 || r >= sizes.size()) return;
        sizes.removeAt(r);
        Settings::get().setCustomPageSizes(sizes);
        fill();
    });
    QObject::connect(use, &QPushButton::clicked, &dlg.d, [&] {
        const auto all = customPageSizes();
        const int r = list->currentRow();
        if (r < 0 || r >= all.size()) return;
        applyPageSetup(ed, all[r].second, all[r].first, QStringLiteral("Page Size"));
        dlg.d.accept();
    });
    dlg.v->addWidget(list);
    dlg.v->addLayout(row);
    dlg.exec();
}

// ---------------- Grid and Baseline Guides ----------------
void gridGuidesDialog(QWidget *p, Editor *ed, int startTab)
{
    Document *doc = ed->doc();
    MasterPage *mp = ed->masterView().isEmpty() ? doc->masterFor(*doc->pages[ed->currentPage()]) : doc->master(ed->masterView());
    if (!mp) mp = doc->masters.first().get();
    Dlg dlg(p, QStringLiteral("Layout Guides"));
    auto *tabs = new QTabWidget(&dlg.d);
    auto *g = new QWidget();
    auto *gf = new QFormLayout(g);
    auto *cols = new QSpinBox(g), *rows = new QSpinBox(g);
    cols->setRange(1, 63);
    rows->setRange(1, 63);
    cols->setValue(mp->grid.cols);
    rows->setValue(mp->grid.rows);
    auto *cg = measure(mp->grid.colGap, g), *rg = measure(mp->grid.rowGap, g);
    auto *center = new QCheckBox(QStringLiteral("Add center guide between columns and rows"), g);
    center->setChecked(mp->grid.centerGuide);
    gf->addRow(QStringLiteral("Columns:"), cols);
    gf->addRow(QStringLiteral("Column spacing:"), cg);
    gf->addRow(QStringLiteral("Rows:"), rows);
    gf->addRow(QStringLiteral("Row spacing:"), rg);
    gf->addRow(center);
    tabs->addTab(g, QStringLiteral("Grid Guides"));
    auto *b = new QWidget();
    auto *bf = new QFormLayout(b);
    auto *spacing = points(mp->grid.baseline, b, 1, 200);
    auto *offset = points(mp->grid.baselineOffset, b, 0, 200);
    bf->addRow(QStringLiteral("Horizontal baseline spacing:"), spacing);
    bf->addRow(QStringLiteral("Offset:"), offset);
    tabs->addTab(b, QStringLiteral("Baseline Guides"));
    auto *mg = new QWidget();
    auto *mgf = new QFormLayout(mg);
    auto *twoPage = new QCheckBox(QStringLiteral("Two-page master"), mg);
    twoPage->setChecked(mp->twoPage);
    mgf->addRow(twoPage);
    const QMarginsF m = doc->setup.margins;
    auto *mt = measure(m.top(), mg), *mb = measure(m.bottom(), mg), *ml = measure(m.left(), mg), *mr = measure(m.right(), mg);
    mgf->addRow(QStringLiteral("Top:"), mt);
    mgf->addRow(QStringLiteral("Bottom:"), mb);
    mgf->addRow(QStringLiteral("Left:"), ml);
    mgf->addRow(QStringLiteral("Right:"), mr);
    // A two-page master's side guides are inside and outside.
    auto sideNames = [=](bool two) {
        if (auto *l = qobject_cast<QLabel *>(mgf->labelForField(ml))) l->setText(two ? QStringLiteral("Inside:") : QStringLiteral("Left:"));
        if (auto *l = qobject_cast<QLabel *>(mgf->labelForField(mr))) l->setText(two ? QStringLiteral("Outside:") : QStringLiteral("Right:"));
    };
    sideNames(mp->twoPage);
    QObject::connect(twoPage, &QCheckBox::toggled, mg, sideNames);
    tabs->insertTab(0, mg, QStringLiteral("Margin Guides"));
    tabs->setCurrentIndex(std::clamp(startTab, 0, tabs->count() - 1));
    dlg.v->addWidget(tabs);
    if (!dlg.exec()) return;
    ed->change(QStringLiteral("Layout Guides"), [&] {
        mp->grid.cols = cols->value();
        mp->grid.rows = rows->value();
        mp->grid.colGap = cg->value();
        mp->grid.rowGap = rg->value();
        mp->grid.centerGuide = center->isChecked();
        mp->grid.baseline = spacing->value();
        mp->grid.baselineOffset = offset->value();
        mp->twoPage = twoPage->isChecked();
        doc->setup.margins = QMarginsF(ml->value(), mt->value(), mr->value(), mb->value());
    });
}

// ---------------- Font ----------------
void fontDialog(QWidget *p, Editor *ed)
{
    const QTextCharFormat cur = ed->currentCharFormat();
    Dlg dlg(p, QStringLiteral("Font"));
    auto *grid = new QGridLayout();
    auto *font = new FontCombo(&dlg.d);
    font->setSchemeFonts(ed->doc()->fonts.heading, ed->doc()->fonts.body);
    const QString fam = cur.hasProperty(QTextFormat::FontFamilies) ? cur.fontFamilies().toStringList().value(0) : (cur.stringProperty(tp::ThemeFont) == "major" ? QStringLiteral("+Heading") : QStringLiteral("+Body"));
    font->setCurrentFamily(fam);
    QString chosenFamily;
    QObject::connect(font, &FontCombo::familyChosen, &dlg.d, [&](const QString &f) { chosenFamily = f; });
    auto *style = new QComboBox(&dlg.d);
    style->addItems({"Regular", "Italic", "Bold", "Bold Italic"});
    style->setCurrentIndex((cur.fontWeight() >= QFont::DemiBold ? 2 : 0) + (cur.fontItalic() ? 1 : 0));
    auto *size = new DecimalSpin(&dlg.d);
    size->setRange(0.5, 999);
    size->setValue(cur.hasProperty(QTextFormat::FontPointSize) ? cur.fontPointSize() : 11);
    auto *color = colorPick(ed, cur.stringProperty(tp::ColorRefP).isEmpty() ? ColorRef::scheme(Main) : ColorRef::fromString(cur.stringProperty(tp::ColorRefP)), false, &dlg.d);
    auto *underline = new QComboBox(&dlg.d);
    underline->addItems({"(none)", "Single", "Words only", "Double", "Dotted", "Dashed", "Wave"});
    underline->setCurrentIndex(cur.underlineStyle() == QTextCharFormat::NoUnderline ? 0 : cur.underlineStyle() == QTextCharFormat::DotLine ? 4
                                   : cur.underlineStyle() == QTextCharFormat::DashUnderline ? 5 : cur.underlineStyle() == QTextCharFormat::WaveUnderline ? 6 : 1);
    grid->addWidget(new QLabel(QStringLiteral("Font:")), 0, 0);
    grid->addWidget(font, 1, 0);
    grid->addWidget(new QLabel(QStringLiteral("Font style:")), 0, 1);
    grid->addWidget(style, 1, 1);
    grid->addWidget(new QLabel(QStringLiteral("Size:")), 0, 2);
    grid->addWidget(size, 1, 2);
    grid->addWidget(new QLabel(QStringLiteral("Underline:")), 2, 0);
    grid->addWidget(underline, 3, 0);
    grid->addWidget(new QLabel(QStringLiteral("Color:")), 2, 1);
    grid->addWidget(color, 3, 1);
    dlg.v->addLayout(grid);
    auto *effects = new QGroupBox(QStringLiteral("Effects"), &dlg.d);
    auto *eg = new QGridLayout(effects);
    struct Fx { const char *name; QCheckBox *box = nullptr; };
    QCheckBox *sup = new QCheckBox(QStringLiteral("Superscript")), *sub = new QCheckBox(QStringLiteral("Subscript")), *strike = new QCheckBox(QStringLiteral("Strikethrough"));
    QCheckBox *smallcaps = new QCheckBox(QStringLiteral("Small caps")), *allcaps = new QCheckBox(QStringLiteral("All caps")), *shadow = new QCheckBox(QStringLiteral("Shadow"));
    QCheckBox *outline = new QCheckBox(QStringLiteral("Outline")), *emboss = new QCheckBox(QStringLiteral("Emboss")), *engrave = new QCheckBox(QStringLiteral("Engrave"));
    sup->setChecked(cur.verticalAlignment() == QTextCharFormat::AlignSuperScript);
    sub->setChecked(cur.verticalAlignment() == QTextCharFormat::AlignSubScript);
    strike->setChecked(cur.fontStrikeOut());
    smallcaps->setChecked(cur.fontCapitalization() == QFont::SmallCaps);
    allcaps->setChecked(cur.fontCapitalization() == QFont::AllUppercase);
    shadow->setChecked(cur.boolProperty(tp::Shadow));
    outline->setChecked(!cur.stringProperty(tp::OutlineRef).isEmpty());
    emboss->setChecked(cur.boolProperty(tp::Emboss));
    engrave->setChecked(cur.boolProperty(tp::Engrave));
    const QList<QCheckBox *> boxes{sup, sub, strike, smallcaps, allcaps, shadow, outline, emboss, engrave};
    for (int i = 0; i < boxes.size(); ++i) eg->addWidget(boxes[i], i / 3, i % 3);
    dlg.v->addWidget(effects);
    auto *preview = new QLabel(QStringLiteral("JeffPub 79 sample text"), &dlg.d);
    preview->setMinimumHeight(48);
    preview->setAlignment(Qt::AlignCenter);
    preview->setFrameShape(QFrame::StyledPanel);
    auto updatePreview = [&] {
        QFont f(chosenFamily.isEmpty() || chosenFamily.startsWith('+') ? (fam.startsWith('+') ? (fam == "+Heading" ? ed->doc()->fonts.heading : ed->doc()->fonts.body) : fam) : chosenFamily);
        f.setPointSizeF(std::min(size->value(), 28.0));
        f.setBold(style->currentIndex() >= 2);
        f.setItalic(style->currentIndex() % 2);
        f.setUnderline(underline->currentIndex() > 0);
        f.setStrikeOut(strike->isChecked());
        f.setCapitalization(smallcaps->isChecked() ? QFont::SmallCaps : allcaps->isChecked() ? QFont::AllUppercase : QFont::MixedCase);
        preview->setFont(f);
    };
    QObject::connect(font, &FontCombo::familyChosen, &dlg.d, updatePreview);
    QObject::connect(style, &QComboBox::currentIndexChanged, &dlg.d, updatePreview);
    QObject::connect(size, &QDoubleSpinBox::valueChanged, &dlg.d, updatePreview);
    for (QCheckBox *b : boxes) QObject::connect(b, &QCheckBox::toggled, &dlg.d, updatePreview);
    updatePreview();
    dlg.v->addWidget(preview);
    if (!dlg.exec()) return;
    QString f = chosenFamily.isEmpty() ? font->currentText() : chosenFamily;
    ed->beginChange(QStringLiteral("Font"));
    if (!f.isEmpty() && f != fam) ed->setFontFamily(f.contains(" (Headings)") ? "+Heading" : f.contains(" (Body)") ? "+Body" : f);
    QTextCharFormat cf;
    cf.setFontPointSize(size->value());
    cf.setFontWeight(style->currentIndex() >= 2 ? QFont::Bold : QFont::Normal);
    cf.setFontItalic(style->currentIndex() % 2);
    static const QTextCharFormat::UnderlineStyle us[] = {QTextCharFormat::NoUnderline, QTextCharFormat::SingleUnderline, QTextCharFormat::SingleUnderline,
                                                         QTextCharFormat::SingleUnderline, QTextCharFormat::DotLine, QTextCharFormat::DashUnderline, QTextCharFormat::WaveUnderline};
    cf.setUnderlineStyle(us[underline->currentIndex()]);
    cf.setFontStrikeOut(strike->isChecked());
    cf.setVerticalAlignment(sup->isChecked() ? QTextCharFormat::AlignSuperScript : sub->isChecked() ? QTextCharFormat::AlignSubScript : QTextCharFormat::AlignNormal);
    cf.setFontCapitalization(smallcaps->isChecked() ? QFont::SmallCaps : allcaps->isChecked() ? QFont::AllUppercase : QFont::MixedCase);
    cf.setProperty(tp::Shadow, shadow->isChecked());
    cf.setProperty(tp::Emboss, emboss->isChecked());
    cf.setProperty(tp::Engrave, engrave->isChecked());
    if (outline->isChecked()) cf.setProperty(tp::OutlineRef, color->current().toString());
    cf.setProperty(tp::ColorRefP, color->current().toString());
    ed->mergeCharFormat(cf, QStringLiteral("Font"));
    if (!outline->isChecked()) ed->clearCharProperty(tp::OutlineRef, QStringLiteral("Font"));
    ed->endChange();
}

// ---------------- Paragraph ----------------
void paragraphDialog(QWidget *p, Editor *ed, int tab)
{
    const QTextBlockFormat bf = ed->currentBlockFormat();
    Dlg dlg(p, QStringLiteral("Paragraph"));
    auto *tabs = new QTabWidget(&dlg.d);
    auto *t1 = new QWidget();
    auto *f1 = new QFormLayout(t1);
    auto *align = new QComboBox(t1);
    align->addItems({"Left", "Center", "Right", "Justified", "Distributed"});
    const Qt::Alignment al = bf.alignment() & Qt::AlignHorizontal_Mask;
    align->setCurrentIndex(bf.boolProperty(tp::Distribute) ? 4 : al == Qt::AlignHCenter ? 1 : al == Qt::AlignRight ? 2 : al == Qt::AlignJustify ? 3 : 0);
    auto *left = measure(bf.leftMargin(), t1, 0), *first = measure(bf.textIndent(), t1, -2000), *right = measure(bf.rightMargin(), t1, 0);
    first->setMinimum(-2000);
    auto *before = points(bf.topMargin(), t1), *after = points(bf.bottomMargin(), t1);
    auto *lineType = new QComboBox(t1);
    lineType->addItems({"Single", "Multiple (sp)", "Exactly (pt)", "At least (pt)"});
    auto *lineVal = new DecimalSpin(t1);
    lineVal->setRange(0.1, 1000);
    switch (bf.lineHeightType()) {
    case QTextBlockFormat::ProportionalHeight: lineType->setCurrentIndex(1); lineVal->setValue(bf.lineHeight() / 100); break;
    case QTextBlockFormat::FixedHeight: lineType->setCurrentIndex(2); lineVal->setValue(bf.lineHeight()); break;
    case QTextBlockFormat::MinimumHeight: lineType->setCurrentIndex(3); lineVal->setValue(bf.lineHeight()); break;
    default: lineType->setCurrentIndex(0); lineVal->setValue(1.0); break;
    }
    auto *baseline = new QCheckBox(QStringLiteral("Align text to baseline guides"), t1);
    baseline->setChecked(bf.boolProperty(tp::AlignToBaseline));
    auto *direction = new QComboBox(t1);
    direction->addItems({"Left to right", "Right to left"});
    direction->setCurrentIndex(bf.layoutDirection() == Qt::RightToLeft ? 1 : 0);
    f1->addRow(QStringLiteral("Alignment:"), align);
    f1->addRow(QStringLiteral("Direction:"), direction);
    f1->addRow(QStringLiteral("Left indent:"), left);
    f1->addRow(QStringLiteral("First line indent:"), first);
    f1->addRow(QStringLiteral("Right indent:"), right);
    f1->addRow(QStringLiteral("Space before:"), before);
    f1->addRow(QStringLiteral("Space after:"), after);
    auto *lrow = new QHBoxLayout();
    lrow->addWidget(lineType);
    lrow->addWidget(lineVal);
    f1->addRow(QStringLiteral("Line spacing:"), lrow);
    f1->addRow(baseline);
    tabs->addTab(t1, QStringLiteral("Indents and Spacing"));
    auto *t2 = new QWidget();
    auto *f2 = new QVBoxLayout(t2);
    auto *widow = new QCheckBox(QStringLiteral("Widow/Orphan control"), t2);
    auto *withNext = new QCheckBox(QStringLiteral("Keep with next"), t2);
    auto *together = new QCheckBox(QStringLiteral("Keep lines together"), t2);
    auto *nextBox = new QCheckBox(QStringLiteral("Start in next text box"), t2);
    widow->setChecked(bf.boolProperty(tp::WidowControl));
    withNext->setChecked(bf.boolProperty(tp::KeepWithNext));
    together->setChecked(bf.boolProperty(tp::KeepTogether));
    nextBox->setChecked(bf.boolProperty(tp::StartInNextBox));
    f2->addWidget(widow);
    f2->addWidget(withNext);
    f2->addWidget(together);
    f2->addWidget(nextBox);
    f2->addStretch(1);
    tabs->addTab(t2, QStringLiteral("Line and Paragraph Breaks"));
    // Tabs.
    auto *t3 = new QWidget();
    auto *f3 = new QVBoxLayout(t3);
    auto *tabList = new QTableWidget(0, 3, t3);
    tabList->setHorizontalHeaderLabels({"Position", "Alignment", "Leader"});
    tabList->horizontalHeader()->setStretchLastSection(true);
    QList<QTextOption::Tab> tabs0 = bf.tabPositions();
    const QString leaders = bf.stringProperty(tp::TabLeaders);
    auto addTabRow = [&](const QTextOption::Tab &t, QChar leader) {
        const int r = tabList->rowCount();
        tabList->insertRow(r);
        tabList->setItem(r, 0, new QTableWidgetItem(Settings::get().format(t.position)));
        auto *a = new QComboBox();
        a->addItems({"Left", "Center", "Right", "Decimal"});
        a->setCurrentIndex(t.type == QTextOption::LeftTab ? 0 : t.type == QTextOption::CenterTab ? 1 : t.type == QTextOption::RightTab ? 2 : 3);
        tabList->setCellWidget(r, 1, a);
        auto *l = new QComboBox();
        l->addItems({"None", "Dot", "Dash", "Line", "Bullet"});
        l->setCurrentIndex(leader == '.' ? 1 : leader == '-' ? 2 : leader == '_' ? 3 : leader == QChar(0x2022) ? 4 : 0);
        tabList->setCellWidget(r, 2, l);
    };
    for (int i = 0; i < tabs0.size(); ++i) addTabRow(tabs0[i], i < leaders.size() ? leaders.at(i) : QChar());
    auto *trow = new QHBoxLayout();
    auto *addTab = new QPushButton(QStringLiteral("Add"), t3);
    auto *delTab = new QPushButton(QStringLiteral("Delete"), t3);
    auto *clrTab = new QPushButton(QStringLiteral("Clear All"), t3);
    trow->addWidget(addTab);
    trow->addWidget(delTab);
    trow->addWidget(clrTab);
    QObject::connect(addTab, &QPushButton::clicked, t3, [&] { addTabRow(QTextOption::Tab(36 * (tabList->rowCount() + 1), QTextOption::LeftTab), QChar()); });
    QObject::connect(delTab, &QPushButton::clicked, t3, [&] { if (tabList->currentRow() >= 0) tabList->removeRow(tabList->currentRow()); });
    QObject::connect(clrTab, &QPushButton::clicked, t3, [&] { tabList->setRowCount(0); });
    f3->addWidget(tabList);
    f3->addLayout(trow);
    tabs->addTab(t3, QStringLiteral("Tabs"));
    tabs->setCurrentIndex(std::clamp(tab, 0, 2));
    dlg.v->addWidget(tabs);
    if (!dlg.exec()) return;
    QTextBlockFormat f;
    const Qt::Alignment als[] = {Qt::AlignLeft, Qt::AlignHCenter, Qt::AlignRight, Qt::AlignJustify, Qt::AlignJustify};
    f.setAlignment(als[align->currentIndex()]);
    f.setProperty(tp::Distribute, align->currentIndex() == 4);
    f.setLeftMargin(left->value());
    f.setTextIndent(first->value());
    f.setRightMargin(right->value());
    f.setTopMargin(before->value());
    f.setBottomMargin(after->value());
    switch (lineType->currentIndex()) {
    case 0: f.setLineHeight(100, QTextBlockFormat::ProportionalHeight); break;
    case 1: f.setLineHeight(lineVal->value() * 100, QTextBlockFormat::ProportionalHeight); break;
    case 2: f.setLineHeight(lineVal->value(), QTextBlockFormat::FixedHeight); break;
    case 3: f.setLineHeight(lineVal->value(), QTextBlockFormat::MinimumHeight); break;
    }
    f.setProperty(tp::AlignToBaseline, baseline->isChecked());
    f.setLayoutDirection(direction->currentIndex() == 1 ? Qt::RightToLeft : Qt::LeftToRight);
    f.setProperty(tp::WidowControl, widow->isChecked());
    f.setProperty(tp::KeepWithNext, withNext->isChecked());
    f.setProperty(tp::KeepTogether, together->isChecked());
    f.setProperty(tp::StartInNextBox, nextBox->isChecked());
    // Tab stops sorted by position, each keeping its own leader.
    QVector<QPair<QTextOption::Tab, QChar>> rows;
    for (int r = 0; r < tabList->rowCount(); ++r) {
        double pt = 0;
        if (!Settings::get().parse(tabList->item(r, 0)->text(), &pt)) continue;
        const int a = static_cast<QComboBox *>(tabList->cellWidget(r, 1))->currentIndex();
        const int l = static_cast<QComboBox *>(tabList->cellWidget(r, 2))->currentIndex();
        QTextOption::Tab t(pt, a == 0 ? QTextOption::LeftTab : a == 1 ? QTextOption::CenterTab : a == 2 ? QTextOption::RightTab : QTextOption::DelimiterTab, a == 3 ? QChar('.') : QChar());
        rows << qMakePair(t, QString(" .-_•").at(l));
    }
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.first.position < b.first.position; });
    QList<QTextOption::Tab> newTabs;
    QString newLeaders;
    for (const auto &r : rows) {
        newTabs << r.first;
        newLeaders += r.second;
    }
    f.setTabPositions(newTabs);
    f.setProperty(tp::TabLeaders, newLeaders);
    ed->mergeBlockFormat(f, QStringLiteral("Paragraph"));
}

// ---------------- Bullets and Numbering ----------------
void bulletsDialog(QWidget *p, Editor *ed, bool numbering)
{
    Dlg dlg(p, QStringLiteral("Bullets and Numbering"));
    auto *tabs = new QTabWidget(&dlg.d);
    auto *bw = new QWidget();
    auto *bl = new QGridLayout(bw);
    auto *group = new QButtonGroup(bw);
    const QStringList chars{"•", "◦", "▪", "❖", "➢", "✔", "★", "–", "►", "♥", "☺", "→"};
    for (int i = 0; i < chars.size(); ++i) {
        auto *b = new QPushButton(chars[i], bw);
        b->setCheckable(true);
        b->setFixedSize(52, 52);
        QFont f = b->font();
        f.setPointSize(20);
        b->setFont(f);
        group->addButton(b, i);
        bl->addWidget(b, i / 6, i % 6);
    }
    group->button(0)->setChecked(true);
    auto *custom = new QPushButton(QStringLiteral("Character…"), bw);
    QString customChar;
    QObject::connect(custom, &QPushButton::clicked, bw, [&] {
        bool ok = false;
        const QString c = QInputDialog::getText(bw, QStringLiteral("Bullet Character"), QStringLiteral("Character:"), QLineEdit::Normal, QString(), &ok);
        if (ok && !c.isEmpty()) customChar = c.left(2);
    });
    bl->addWidget(custom, 2, 0, 1, 2);
    auto *indent = measure(18, bw);
    bl->addWidget(new QLabel(QStringLiteral("Indent list by:")), 3, 0, 1, 2);
    bl->addWidget(indent, 3, 2, 1, 2);
    tabs->addTab(bw, QStringLiteral("Bullets"));
    auto *nw = new QWidget();
    auto *nf = new QFormLayout(nw);
    auto *format = new QComboBox(nw);
    format->addItems({"1. 2. 3.", "a. b. c.", "A. B. C.", "i. ii. iii.", "I. II. III.", "1) 2) 3)", "(1) (2) (3)"});
    auto *start = new QSpinBox(nw);
    start->setRange(1, 9999);
    nf->addRow(QStringLiteral("Format:"), format);
    nf->addRow(QStringLiteral("Start at:"), start);
    tabs->addTab(nw, QStringLiteral("Numbering"));
    if (numbering) tabs->setCurrentIndex(1);
    dlg.v->addWidget(tabs);
    if (!dlg.exec()) return;
    if (tabs->currentIndex() == 0) ed->setList(1, 0, customChar.isEmpty() ? chars[group->checkedId()] : customChar);
    else ed->setList(2, format->currentIndex() + 1, QString(), start->value());
    Q_UNUSED(indent);
}

// ---------------- Drop Cap ----------------
void dropCapDialog(QWidget *p, Editor *ed)
{
    const QTextBlockFormat bf = ed->currentBlockFormat();
    Dlg dlg(p, QStringLiteral("Drop Cap"));
    auto *form = new QFormLayout();
    auto *pos = new QComboBox(&dlg.d);
    pos->addItems({"Dropped", "Up"});
    pos->setCurrentIndex(bf.boolProperty(tp::DropCapUpper) ? 1 : 0);
    auto *lines = new QSpinBox(&dlg.d);
    lines->setRange(1, 20);
    lines->setValue(bf.intProperty(tp::DropCapLines) ? bf.intProperty(tp::DropCapLines) : 3);
    auto *chars = new QSpinBox(&dlg.d);
    chars->setRange(1, 15);
    chars->setValue(bf.intProperty(tp::DropCapChars) ? bf.intProperty(tp::DropCapChars) : 1);
    auto *font = new FontCombo(&dlg.d);
    font->setCurrentFamily(bf.stringProperty(tp::DropCapFont).isEmpty() ? QStringLiteral("(current font)") : bf.stringProperty(tp::DropCapFont));
    QString chosen = bf.stringProperty(tp::DropCapFont);
    QObject::connect(font, &FontCombo::familyChosen, &dlg.d, [&](const QString &f) { chosen = f.startsWith('+') ? QString() : f; });
    auto *color = colorPick(ed, bf.stringProperty(tp::DropCapColor).isEmpty() ? ColorRef::scheme(Accent1) : ColorRef::fromString(bf.stringProperty(tp::DropCapColor)), true, &dlg.d);
    form->addRow(QStringLiteral("Letter position:"), pos);
    form->addRow(QStringLiteral("Size of letters (lines high):"), lines);
    form->addRow(QStringLiteral("Number of letters:"), chars);
    form->addRow(QStringLiteral("Font:"), font);
    form->addRow(QStringLiteral("Color:"), color);
    dlg.v->addLayout(form);
    auto *remove = new QPushButton(QStringLiteral("Remove"), &dlg.d);
    bool removed = false;
    QObject::connect(remove, &QPushButton::clicked, &dlg.d, [&] { removed = true; dlg.d.accept(); });
    dlg.bb->addButton(remove, QDialogButtonBox::ResetRole);
    if (!dlg.exec()) return;
    if (removed) { ed->setDropCap(0); return; }
    QTextBlockFormat f;
    f.setProperty(tp::DropCapLines, lines->value());
    f.setProperty(tp::DropCapChars, chars->value());
    f.setProperty(tp::DropCapUpper, pos->currentIndex() == 1);
    f.setProperty(tp::DropCapFont, chosen);
    f.setProperty(tp::DropCapColor, color->current().isNone() ? QString() : color->current().toString());
    if (ed->isEditingText()) {
        ed->beginChange(QStringLiteral("Drop Cap"));
        ed->cursor().mergeBlockFormat(f);
        ed->endChange();
    } else {
        ed->mergeBlockFormat(f, QStringLiteral("Drop Cap"));
    }
}

// ---------------- Gradient text fill ----------------
void textGradientDialog(QWidget *p, Editor *ed)
{
    const QTextCharFormat cf = ed->currentCharFormat();
    Fill current = Fill::fromJson(QJsonDocument::fromJson(cf.stringProperty(tp::TextFill).toUtf8()).object());
    if (current.type != Fill::Gradient) current = Fill::gradient(ColorRef::scheme(Accent1), ColorRef::scheme(Accent2), 0);
    Dlg dlg(p, QStringLiteral("Gradient Text Fill"));
    auto *form = new QFormLayout();
    auto *from = colorPick(ed, current.color, false, &dlg.d);
    auto *to = colorPick(ed, current.color2, false, &dlg.d);
    auto *dir = new QComboBox(&dlg.d);
    dir->addItems({QStringLiteral("Left to right"), QStringLiteral("Top to bottom"), QStringLiteral("Diagonal down"), QStringLiteral("Diagonal up")});
    const double angles[] = {0, 90, 45, -45};
    for (int i = 0; i < 4; ++i)
        if (std::abs(current.angle - angles[i]) < 0.5) dir->setCurrentIndex(i);
    form->addRow(QStringLiteral("From:"), from);
    form->addRow(QStringLiteral("To:"), to);
    form->addRow(QStringLiteral("Direction:"), dir);
    dlg.v->addLayout(form);
    if (!dlg.exec()) return;
    const Fill g = Fill::gradient(from->current(), to->current(), angles[dir->currentIndex()]);
    QTextCharFormat f;
    f.setProperty(tp::TextFill, QString::fromUtf8(QJsonDocument(g.toJson()).toJson(QJsonDocument::Compact)));
    ed->mergeCharFormat(f, QStringLiteral("Text Fill"));
}

// ---------------- Character Spacing ----------------
void characterSpacingDialog(QWidget *p, Editor *ed)
{
    const QTextCharFormat cf = ed->currentCharFormat();
    Dlg dlg(p, QStringLiteral("Character Spacing"));
    auto *form = new QFormLayout();
    auto *scale = new QSpinBox(&dlg.d);
    scale->setRange(1, 600);
    scale->setSuffix(QStringLiteral("%"));
    scale->setValue(cf.hasProperty(QTextFormat::FontStretch) ? cf.fontStretch() : 100);
    // Tracking: a preset or a percentage of normal spacing, as Publisher has it.
    static const QPair<const char *, double> presets[] = {{"Very tight", 75}, {"Tight", 87.5}, {"Normal", 100}, {"Loose", 112.5}, {"Very loose", 125}};
    auto *trackKind = new QComboBox(&dlg.d);
    for (const auto &pr : presets) trackKind->addItem(QString::fromLatin1(pr.first), pr.second);
    trackKind->addItem(QStringLiteral("Custom"), -1.0);
    auto *track = new DecimalSpin(&dlg.d);
    track->setRange(0, 600);
    track->setSuffix(QStringLiteral("%"));
    track->setValue(tp::trackingOf(cf));
    auto syncTrackKind = [=] {
        const QSignalBlocker b(trackKind);
        trackKind->setCurrentIndex(trackKind->count() - 1);
        for (int i = 0; i + 1 < trackKind->count(); ++i)
            if (std::abs(trackKind->itemData(i).toDouble() - track->value()) < 0.01) trackKind->setCurrentIndex(i);
    };
    syncTrackKind();
    QObject::connect(trackKind, &QComboBox::currentIndexChanged, &dlg.d, [=](int i) {
        const double v = trackKind->itemData(i).toDouble();
        if (v > 0) { const QSignalBlocker b(track); track->setValue(v); }
    });
    QObject::connect(track, &QDoubleSpinBox::valueChanged, &dlg.d, syncTrackKind);
    // Kerning: space added (or taken) after each selected letter.
    auto *kernKind = new QComboBox(&dlg.d);
    kernKind->addItems({QStringLiteral("Normal"), QStringLiteral("Expand"), QStringLiteral("Condense")});
    const double kernNow = tp::kerningOf(cf);
    auto *kernBy = points(std::abs(kernNow), &dlg.d, 0, 600);
    kernKind->setCurrentIndex(kernNow > 0.0005 ? 1 : kernNow < -0.0005 ? 2 : 0);
    kernBy->setEnabled(kernKind->currentIndex() != 0);
    QObject::connect(kernKind, &QComboBox::currentIndexChanged, &dlg.d, [=](int i) {
        kernBy->setEnabled(i != 0);
        if (i == 0) kernBy->setValue(0);
        else if (kernBy->value() < 0.0005) kernBy->setValue(1);
    });
    auto *kern = new QCheckBox(QStringLiteral("Automatic pair kerning for fonts"), &dlg.d);
    kern->setChecked(!cf.hasProperty(QTextFormat::FontKerning) || cf.fontKerning());
    auto *kernFrom = points(cf.hasProperty(tp::KernAbove) ? cf.property(tp::KernAbove).toDouble() : 14.0, &dlg.d, 0, 1638);
    kernFrom->setEnabled(kern->isChecked());
    QObject::connect(kern, &QCheckBox::toggled, kernFrom, &QWidget::setEnabled);
    auto *word = points(cf.fontWordSpacing(), &dlg.d, -50, 200);
    form->addRow(QStringLiteral("Scaling (shrink or stretch):"), scale);
    auto *trackRow = new QHBoxLayout();
    trackRow->addWidget(trackKind);
    trackRow->addWidget(new QLabel(QStringLiteral("By this amount:"), &dlg.d));
    trackRow->addWidget(track);
    form->addRow(QStringLiteral("Tracking (selected text):"), trackRow);
    auto *kernByRow = new QHBoxLayout();
    kernByRow->addWidget(kernKind);
    kernByRow->addWidget(new QLabel(QStringLiteral("By this amount:"), &dlg.d));
    kernByRow->addWidget(kernBy);
    form->addRow(QStringLiteral("Kerning (selected letters):"), kernByRow);
    form->addRow(QStringLiteral("Word spacing:"), word);
    auto *kernRow = new QHBoxLayout();
    kernRow->addWidget(kern);
    kernRow->addWidget(kernFrom);
    kernRow->addWidget(new QLabel(QStringLiteral("and above"), &dlg.d));
    kernRow->addStretch(1);
    form->addRow(kernRow);
    dlg.v->addLayout(form);
    if (!dlg.exec()) return;
    QTextCharFormat f;
    f.setFontStretch(scale->value());
    f.setProperty(tp::Tracking, track->value());
    f.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    f.setFontLetterSpacing(kernKind->currentIndex() == 2 ? -kernBy->value() : kernKind->currentIndex() == 1 ? kernBy->value() : 0.0);
    f.setFontKerning(kern->isChecked());
    f.setProperty(tp::KernAbove, kernFrom->value());
    f.setFontWordSpacing(word->value());
    ed->mergeCharFormat(f, QStringLiteral("Character Spacing"));
}

// ---------------- Format Object ----------------
void formatObjectDialog(QWidget *p, Editor *ed, int tab)
{
    Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
    if (!it) {
        if (ed->selection().isEmpty()) return;
        it = ed->selectedItems().first();
    }
    Dlg dlg(p, QStringLiteral("Format %1").arg(itemTypeName(it->type())));
    auto *tabs = new QTabWidget(&dlg.d);
    // Colors and Lines.
    auto *cl = new QWidget();
    auto *clf = new QFormLayout(cl);
    Fill fill = it->fill;
    auto *fillColor = colorPick(ed, fill.type == Fill::Solid ? fill.color : ColorRef::none(), true, cl);
    auto *fillFx = new QPushButton(QStringLiteral("Fill Effects…"), cl);
    QObject::connect(fillFx, &QPushButton::clicked, cl, [&] { if (fillEffectsDialog(cl, ed, fill)) fillColor->setCurrent(fill.type == Fill::Solid ? fill.color : ColorRef::none()); });
    QObject::connect(fillColor, &ColorButton::colorPicked, cl, [&](const ColorRef &c) { fill = c.isNone() ? Fill() : Fill::solid(c, fill.transparency); });
    auto *fillRow = new QHBoxLayout();
    fillRow->addWidget(fillColor);
    fillRow->addWidget(fillFx);
    auto *trans = new QSlider(Qt::Horizontal, cl);
    trans->setRange(0, 100);
    trans->setValue(int(fill.transparency * 100));
    auto *lineColor = colorPick(ed, it->stroke.color, true, cl);
    auto *lineWidth = points(it->stroke.width, cl, 0, 200);
    auto *dash = new QComboBox(cl);
    for (int d = 0; d < 8; ++d) dash->addItem(dashName(Stroke::Dash(d)));
    dash->setCurrentIndex(int(it->stroke.dash));
    auto *compound = new QComboBox(cl);
    compound->addItems({"Single", "Double", "Thick-thin", "Thin-thick", "Triple"});
    compound->setCurrentIndex(int(it->stroke.compound));
    auto *begin = new QComboBox(cl), *end = new QComboBox(cl);
    for (QComboBox *c : {begin, end}) c->addItems({"None", "Arrow", "Open arrow", "Stealth arrow", "Diamond", "Oval"});
    begin->setCurrentIndex(int(it->stroke.startArrow));
    end->setCurrentIndex(int(it->stroke.endArrow));
    clf->addRow(QStringLiteral("Fill color:"), fillRow);
    clf->addRow(QStringLiteral("Transparency:"), trans);
    clf->addRow(QStringLiteral("Line color:"), lineColor);
    clf->addRow(QStringLiteral("Line weight:"), lineWidth);
    clf->addRow(QStringLiteral("Dash type:"), dash);
    clf->addRow(QStringLiteral("Compound type:"), compound);
    if (it->type() == ItemType::Line) {
        clf->addRow(QStringLiteral("Begin style:"), begin);
        clf->addRow(QStringLiteral("End style:"), end);
    }
    tabs->addTab(cl, QStringLiteral("Colors and Lines"));
    // Size.
    auto *sz = new QWidget();
    auto *szf = new QFormLayout(sz);
    auto *h = measure(it->rect.height(), sz, 1), *w = measure(it->rect.width(), sz, 1);
    auto *rot = new DecimalSpin(sz);
    rot->setRange(-360, 360);
    rot->setSuffix(QStringLiteral("°"));
    rot->setValue(it->rotation);
    auto *lockAspect = new QCheckBox(QStringLiteral("Lock aspect ratio"), sz);
    lockAspect->setChecked(it->type() == ItemType::Picture);
    const double ratio = it->rect.width() / std::max(1.0, it->rect.height());
    QObject::connect(w, &QDoubleSpinBox::valueChanged, sz, [=](double v) { if (lockAspect->isChecked() && !h->hasFocus()) { const QSignalBlocker b(h); h->setValue(v / ratio); } });
    QObject::connect(h, &QDoubleSpinBox::valueChanged, sz, [=](double v) { if (lockAspect->isChecked() && !w->hasFocus()) { const QSignalBlocker b(w); w->setValue(v * ratio); } });
    szf->addRow(QStringLiteral("Height:"), h);
    szf->addRow(QStringLiteral("Width:"), w);
    szf->addRow(QStringLiteral("Rotation:"), rot);
    szf->addRow(lockAspect);
    tabs->addTab(sz, QStringLiteral("Size"));
    // Layout.
    auto *lay = new QWidget();
    auto *layf = new QFormLayout(lay);
    auto *x = measure(it->rect.x(), lay, -10000), *y = measure(it->rect.y(), lay, -10000);
    x->setMinimum(-10000);
    y->setMinimum(-10000);
    auto *wrap = new QComboBox(lay);
    wrap->addItems({"None", "Square", "Tight", "Through", "Top and bottom"});
    wrap->setCurrentIndex(int(it->wrap.mode));
    auto *side = new QComboBox(lay);
    side->addItems({"Both sides", "Left only", "Right only", "Largest side"});
    side->setCurrentIndex(int(it->wrap.side));
    auto *dt = measure(it->wrap.top, lay), *db = measure(it->wrap.bottom, lay), *dl = measure(it->wrap.left, lay), *dr = measure(it->wrap.right, lay);
    layf->addRow(QStringLiteral("Horizontal position:"), x);
    layf->addRow(QStringLiteral("Vertical position:"), y);
    layf->addRow(QStringLiteral("Wrapping style:"), wrap);
    layf->addRow(QStringLiteral("Wrap text:"), side);
    layf->addRow(QStringLiteral("Distance from text — top:"), dt);
    layf->addRow(QStringLiteral("bottom:"), db);
    layf->addRow(QStringLiteral("left:"), dl);
    layf->addRow(QStringLiteral("right:"), dr);
    tabs->addTab(lay, QStringLiteral("Layout"));
    // Text box.
    QMarginsF ins;
    int columns = 1;
    double colGap = 9;
    VAlign valign = VAlign::Top;
    int autofit = 0;
    bool contOn = false, contFrom = false;
    auto *tb = new QWidget();
    auto *tbf = new QFormLayout(tb);
    auto *tl = measure(0, tb), *tt = measure(0, tb), *tr = measure(0, tb), *tbm = measure(0, tb);
    auto *va = new QComboBox(tb);
    va->addItems({"Top", "Middle", "Bottom"});
    auto *fit = new QComboBox(tb);
    fit->addItems({"Do Not Autofit", "Best Fit", "Shrink Text On Overflow", "Grow Text Box to Fit"});
    auto *cols = new QSpinBox(tb);
    cols->setRange(1, 63);
    auto *gap = measure(9, tb);
    auto *cOn = new QCheckBox(QStringLiteral("Include \"Continued on page…\""), tb);
    auto *cFrom = new QCheckBox(QStringLiteral("Include \"Continued from page…\""), tb);
    const bool isText = it->type() == ItemType::Text, isShape = it->type() == ItemType::Shape;
    if (auto *t = dynamic_cast<TextItem *>(it)) {
        ins = t->insets; columns = t->columns; colGap = t->columnGap; valign = t->valign; autofit = int(t->autofit); contOn = t->continuedOn; contFrom = t->continuedFrom;
    } else if (auto *s = dynamic_cast<ShapeItem *>(it)) {
        ins = s->insets; valign = s->valign;
    }
    tl->setValue(ins.left()); tt->setValue(ins.top()); tr->setValue(ins.right()); tbm->setValue(ins.bottom());
    va->setCurrentIndex(int(valign));
    fit->setCurrentIndex(autofit);
    cols->setValue(columns);
    gap->setValue(colGap);
    cOn->setChecked(contOn);
    cFrom->setChecked(contFrom);
    tbf->addRow(QStringLiteral("Vertical alignment:"), va);
    if (isText) tbf->addRow(QStringLiteral("Text autofitting:"), fit);
    tbf->addRow(QStringLiteral("Left margin:"), tl);
    tbf->addRow(QStringLiteral("Right margin:"), tr);
    tbf->addRow(QStringLiteral("Top margin:"), tt);
    tbf->addRow(QStringLiteral("Bottom margin:"), tbm);
    if (isText) {
        tbf->addRow(QStringLiteral("Columns:"), cols);
        tbf->addRow(QStringLiteral("Column spacing:"), gap);
        tbf->addRow(cOn);
        tbf->addRow(cFrom);
    }
    if (isText || isShape) tabs->addTab(tb, QStringLiteral("Text Box"));
    // Picture.
    auto *pic = dynamic_cast<PictureItem *>(it);
    auto *pw = new QWidget();
    auto *pf = new QFormLayout(pw);
    auto *bright = new QSlider(Qt::Horizontal, pw), *contrast = new QSlider(Qt::Horizontal, pw);
    bright->setRange(-100, 100);
    contrast->setRange(-100, 100);
    auto *recolor = new QComboBox(pw);
    recolor->addItems({"None", "Grayscale", "Sepia", "Washout", "Black and white", "Tint"});
    auto *ptrans = new QSlider(Qt::Horizontal, pw);
    ptrans->setRange(0, 100);
    if (pic) {
        bright->setValue(int(pic->brightness));
        contrast->setValue(int(pic->contrast));
        recolor->setCurrentIndex(int(pic->recolor));
        ptrans->setValue(int(pic->transparency * 100));
        pf->addRow(QStringLiteral("Brightness:"), bright);
        pf->addRow(QStringLiteral("Contrast:"), contrast);
        pf->addRow(QStringLiteral("Color:"), recolor);
        pf->addRow(QStringLiteral("Transparency:"), ptrans);
        tabs->addTab(pw, QStringLiteral("Picture"));
    }
    // Web / Alt text.
    auto *wb = new QWidget();
    auto *wbf = new QFormLayout(wb);
    auto *alt = new QPlainTextEdit(it->altText, wb);
    auto *link = new QLineEdit(it->hyperlink, wb);
    wbf->addRow(QStringLiteral("Alternative text:"), alt);
    wbf->addRow(QStringLiteral("Hyperlink:"), link);
    tabs->addTab(wb, QStringLiteral("Alt Text"));
    // Map caller's tab index (0 colors, 1 size, 2 layout, 3 text box, 4 picture, 5 alt text).
    QWidget *want[] = {cl, sz, lay, tb, pw, wb};
    const int idx = tabs->indexOf(want[std::clamp(tab, 0, 5)]);
    if (idx >= 0) tabs->setCurrentIndex(idx);
    dlg.v->addWidget(tabs);
    if (!dlg.exec()) return;
    ed->forEachSelected(QStringLiteral("Format Object"), [&](Item *o) {
        fill.transparency = trans->value() / 100.0;
        if (fill.type != Fill::NoFill || fillColor->current().isNone()) o->fill = fill;
        o->stroke.color = lineColor->current();
        o->stroke.width = lineWidth->value();
        o->stroke.dash = Stroke::Dash(dash->currentIndex());
        o->stroke.compound = Stroke::Compound(compound->currentIndex());
        o->stroke.startArrow = Arrow(begin->currentIndex());
        o->stroke.endArrow = Arrow(end->currentIndex());
        o->wrap.mode = Wrap::Mode(wrap->currentIndex());
        o->wrap.side = Wrap::Side(side->currentIndex());
        o->wrap.top = dt->value(); o->wrap.bottom = db->value(); o->wrap.left = dl->value(); o->wrap.right = dr->value();
        o->altText = alt->toPlainText();
        o->hyperlink = link->text();
        if (o == it && o->type() != ItemType::Line && o->type() != ItemType::Group) {
            const QPointF c0 = o->rect.center();
            Q_UNUSED(c0);
            const QRectF before = o->rect;
            const QRectF to(x->value(), y->value(), w->value(), h->value());
            if (to.size() != before.size()) o->scaleInto(before, QRectF(before.topLeft(), to.size()));
            o->rect.moveTopLeft(to.topLeft());
            o->rotation = std::fmod(rot->value() + 360, 360.0);
        }
        if (auto *t = dynamic_cast<TextItem *>(o)) {
            t->insets = QMarginsF(tl->value(), tt->value(), tr->value(), tbm->value());
            t->valign = VAlign(va->currentIndex());
            t->autofit = TextItem::Autofit(fit->currentIndex());
            t->columns = cols->value();
            t->columnGap = gap->value();
            t->continuedOn = cOn->isChecked();
            t->continuedFrom = cFrom->isChecked();
            ed->autoGrowText(t);
        }
        if (auto *s = dynamic_cast<ShapeItem *>(o)) {
            s->insets = QMarginsF(tl->value(), tt->value(), tr->value(), tbm->value());
            s->valign = VAlign(va->currentIndex());
        }
        if (auto *pp = dynamic_cast<PictureItem *>(o)) {
            pp->brightness = bright->value();
            pp->contrast = contrast->value();
            pp->recolor = PictureItem::Recolor(recolor->currentIndex());
            pp->transparency = ptrans->value() / 100.0;
            if (pp->recolor == PictureItem::ColorTint && pp->recolorColor.isNone()) pp->recolorColor = ColorRef::scheme(Accent1);
        }
    });
}

// ---------------- Insert Table / Page ----------------
void insertTableDialog(QWidget *p, Editor *ed)
{
    Dlg dlg(p, QStringLiteral("Create Table"));
    auto *form = new QFormLayout();
    auto *rows = new QSpinBox(&dlg.d), *cols = new QSpinBox(&dlg.d);
    rows->setRange(1, 128);
    cols->setRange(1, 128);
    rows->setValue(4);
    cols->setValue(4);
    auto *fmt = new QComboBox(&dlg.d);
    fmt->addItem("None");
    for (int i = 1; i <= 20; ++i) fmt->addItem(QStringLiteral("Table Style %1").arg(i));
    fmt->addItems({"Basic 1", "Basic 2", "Basic 3", "Checkbook Register", "List 1", "List 2", "List 3", "Numbers 1", "Numbers 2"});
    fmt->setCurrentIndex(1);
    form->addRow(QStringLiteral("Number of rows:"), rows);
    form->addRow(QStringLiteral("Number of columns:"), cols);
    form->addRow(QStringLiteral("Table format:"), fmt);
    dlg.v->addLayout(form);
    if (!dlg.exec()) return;
    const QSizeF ps = ed->doc()->pageSize();
    const double w = std::min(ps.width() * 0.8, 72.0 * cols->value()), h = 22.0 * rows->value();
    ed->addItem(ed->newTable(QRectF((ps.width() - w) / 2, (ps.height() - h) / 2, w, h), rows->value(), cols->value(), fmt->currentText()));
}

void insertPageDialog(QWidget *p, Editor *ed)
{
    Dlg dlg(p, QStringLiteral("Insert Page"));
    auto *form = new QFormLayout();
    auto *count = new QSpinBox(&dlg.d);
    count->setRange(1, 999);
    auto *before = new QRadioButton(QStringLiteral("Before current page"), &dlg.d), *after = new QRadioButton(QStringLiteral("After current page"), &dlg.d);
    after->setChecked(true);
    auto *blank = new QRadioButton(QStringLiteral("Insert blank pages"), &dlg.d), *textbox = new QRadioButton(QStringLiteral("Create one text box on each page"), &dlg.d),
         *dup = new QRadioButton(QStringLiteral("Duplicate all objects on page:"), &dlg.d);
    blank->setChecked(true);
    auto *srcPage = new QSpinBox(&dlg.d);
    srcPage->setRange(1, ed->doc()->pages.size());
    srcPage->setValue(ed->currentPage() + 1);
    form->addRow(QStringLiteral("Number of new pages:"), count);
    form->addRow(before);
    form->addRow(after);
    form->addRow(blank);
    form->addRow(textbox);
    auto *dupRow = new QHBoxLayout();
    dupRow->addWidget(dup);
    dupRow->addWidget(srcPage);
    form->addRow(dupRow);
    auto *master = new QComboBox(&dlg.d);
    for (const auto &m : ed->doc()->masters) master->addItem(QStringLiteral("(%1) %2").arg(m->abbr, m->name), m->id);
    form->addRow(QStringLiteral("Master page:"), master);
    dlg.v->addLayout(form);
    if (!dlg.exec()) return;
    int at = before->isChecked() ? ed->currentPage() - 1 : ed->currentPage();
    if (dup->isChecked()) {
        const int src = srcPage->value() - 1;
        ed->beginChange(QStringLiteral("Insert Page"));
        for (int i = 0; i < count->value(); ++i) {
            auto pg = ed->doc()->addPage(at + 1 + i, master->currentData().toString());
            for (const auto &it : ed->doc()->pages[src < at + 1 + i ? src : src + 1]->items) pg->items.push_back(ed->doc()->cloneItem(*it));
        }
        ed->endChange();
        ed->setCurrentPage(at + 1);
        return;
    }
    ed->insertPages(at, count->value(), false, textbox->isChecked(), master->currentData().toString());
}

// ---------------- Symbol ----------------
QStringList recentSymbols() { return Settings::get().value(QStringLiteral("symbols/recent")).toStringList(); }

bool insertSymbol(QWidget *p, Editor *ed, const QString &ch, const QString &font)
{
    if (!ed->isEditingText()) {
        QMessageBox::information(p, QStringLiteral("Symbol"), QStringLiteral("Click in a text box first."));
        return false;
    }
    if (!font.isEmpty()) {
        QTextCharFormat cf = ed->cursor().charFormat();
        cf.setFontFamilies(QStringList{font});
        ed->beginChange(QStringLiteral("Insert Symbol"));
        ed->cursor().insertText(ch, cf);
        ed->endChange();
        ed->textEdited();
    } else {
        ed->insertTextBlock(ch, QStringLiteral("Insert Symbol"));
    }
    QStringList recents = recentSymbols();
    const QString entry = font.isEmpty() ? ch : ch + QLatin1Char('\t') + font;
    recents.removeAll(entry);
    recents.prepend(entry);
    while (recents.size() > 20) recents.removeLast();
    Settings::get().setValue(QStringLiteral("symbols/recent"), recents);
    return true;
}

void symbolDialog(QWidget *p, Editor *ed)
{
    Dlg dlg(p, QStringLiteral("Symbol"), QDialogButtonBox::Close);
    auto *font = new QComboBox(&dlg.d);
    font->addItem(QStringLiteral("(normal text)"));
    font->addItems(QFontDatabase::families());
    auto *subset = new QComboBox(&dlg.d);
    const QVector<QPair<QString, QPair<int, int>>> ranges = {{"Basic Latin", {0x20, 0x7E}}, {"Latin-1 Supplement", {0xA0, 0xFF}}, {"Latin Extended-A", {0x100, 0x17F}},
                                                            {"Greek", {0x370, 0x3FF}}, {"Cyrillic", {0x400, 0x4FF}}, {"General Punctuation", {0x2000, 0x206F}},
                                                            {"Currency Symbols", {0x20A0, 0x20CF}}, {"Letterlike Symbols", {0x2100, 0x214F}}, {"Number Forms", {0x2150, 0x218F}},
                                                            {"Arrows", {0x2190, 0x21FF}}, {"Mathematical Operators", {0x2200, 0x22FF}}, {"Box Drawing", {0x2500, 0x257F}},
                                                            {"Geometric Shapes", {0x25A0, 0x25FF}}, {"Miscellaneous Symbols", {0x2600, 0x26FF}}, {"Dingbats", {0x2700, 0x27BF}}};
    for (const auto &r : ranges) subset->addItem(r.first);
    auto *top = new QHBoxLayout();
    top->addWidget(new QLabel(QStringLiteral("Font:")));
    top->addWidget(font, 1);
    top->addWidget(new QLabel(QStringLiteral("Subset:")));
    top->addWidget(subset, 1);
    dlg.v->addLayout(top);
    auto makeGrid = [&](int rows) {
        auto *g = new QTableWidget(rows, 16, &dlg.d);
        g->horizontalHeader()->hide();
        g->verticalHeader()->hide();
        g->setEditTriggers(QAbstractItemView::NoEditTriggers);
        for (int c = 0; c < 16; ++c) g->setColumnWidth(c, 32);
        return g;
    };
    auto *grid = makeGrid(0);
    grid->setMinimumSize(560, 300);
    // Recently used symbols: one row, each in the font it was used in.
    auto *recent = makeGrid(1);
    recent->setRowHeight(0, 32);
    recent->setFixedHeight(36);
    recent->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    recent->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *code = new QLabel(&dlg.d);
    auto fill = [=] {
        const auto r = ranges[subset->currentIndex()].second;
        const int n = r.second - r.first + 1;
        grid->setRowCount((n + 15) / 16);
        grid->clearContents();
        QFont f = grid->font();
        if (font->currentIndex() > 0) f.setFamily(font->currentText());
        f.setPointSize(14);
        for (int i = 0; i < n; ++i) {
            auto *cell = new QTableWidgetItem(QString(QChar(char16_t(r.first + i))));
            cell->setTextAlignment(Qt::AlignCenter);
            cell->setFont(f);
            cell->setData(Qt::UserRole, r.first + i);
            cell->setData(Qt::UserRole + 1, font->currentIndex() > 0 ? font->currentText() : QString());
            grid->setItem(i / 16, i % 16, cell);
        }
        for (int row = 0; row < grid->rowCount(); ++row) grid->setRowHeight(row, 32);
    };
    auto fillRecent = [=] {
        recent->clearContents();
        const QStringList recents = recentSymbols();
        for (int i = 0; i < recents.size() && i < 16; ++i) {
            const QString ch = recents[i].section(QLatin1Char('\t'), 0, 0), fam = recents[i].section(QLatin1Char('\t'), 1);
            auto *cell = new QTableWidgetItem(ch);
            QFont f = recent->font();
            if (!fam.isEmpty()) f.setFamily(fam);
            f.setPointSize(14);
            cell->setFont(f);
            cell->setTextAlignment(Qt::AlignCenter);
            cell->setData(Qt::UserRole, ch.isEmpty() ? 0 : int(ch.at(0).unicode()));
            cell->setData(Qt::UserRole + 1, fam);
            recent->setItem(0, i, cell);
        }
    };
    QObject::connect(subset, &QComboBox::currentIndexChanged, &dlg.d, fill);
    QObject::connect(font, &QComboBox::currentIndexChanged, &dlg.d, fill);
    fill();
    fillRecent();
    QTableWidget *chosenFrom = grid;
    auto showCode = [=](QTableWidgetItem *it) {
        if (it) code->setText(QStringLiteral("Character code: %1 (Unicode U+%2)").arg(it->data(Qt::UserRole).toInt()).arg(it->data(Qt::UserRole).toInt(), 4, 16, QChar('0')).toUpper());
    };
    QObject::connect(grid, &QTableWidget::currentItemChanged, &dlg.d, [&, showCode](QTableWidgetItem *it) { chosenFrom = grid; showCode(it); });
    QObject::connect(recent, &QTableWidget::currentItemChanged, &dlg.d, [&, showCode](QTableWidgetItem *it) { chosenFrom = recent; showCode(it); });
    auto insert = [&, fillRecent](QTableWidgetItem *it) {
        if (!it || it->text().isEmpty()) return;
        if (insertSymbol(&dlg.d, ed, it->text(), it->data(Qt::UserRole + 1).toString())) fillRecent();
    };
    QObject::connect(grid, &QTableWidget::itemDoubleClicked, &dlg.d, insert);
    QObject::connect(recent, &QTableWidget::itemDoubleClicked, &dlg.d, insert);
    auto *ins = dlg.bb->addButton(QStringLiteral("Insert"), QDialogButtonBox::ActionRole);
    QObject::connect(ins, &QPushButton::clicked, &dlg.d, [&] { insert(chosenFrom->currentItem()); });
    dlg.v->addWidget(grid, 1);
    dlg.v->addWidget(new QLabel(QStringLiteral("Recently used symbols:"), &dlg.d));
    dlg.v->addWidget(recent);
    dlg.v->addWidget(code);
    dlg.exec();
}

// ---------------- Date & Time ----------------
void dateTimeDialog(QWidget *p, Editor *ed)
{
    if (!ed->isEditingText()) {
        QMessageBox::information(p, QStringLiteral("Date and Time"), QStringLiteral("Click in a text box first."));
        return;
    }
    Dlg dlg(p, QStringLiteral("Date and Time"));
    auto *list = new QListWidget(&dlg.d);
    const QStringList formats{"M/d/yyyy", "dddd, MMMM d, yyyy", "MMMM d, yyyy", "M/d/yy", "yyyy-MM-dd", "d-MMM-yy", "M.d.yyyy", "MMM. d, yy", "d MMMM yyyy",
                              "MMMM yy", "MMM-yy", "M/d/yyyy h:mm AP", "M/d/yyyy h:mm:ss AP", "h:mm AP", "h:mm:ss AP", "HH:mm", "HH:mm:ss"};
    const QDateTime now = QDateTime::currentDateTime();
    for (const QString &f : formats) {
        auto *it = new QListWidgetItem(now.toString(f));
        it->setData(Qt::UserRole, f);
        list->addItem(it);
    }
    list->setCurrentRow(0);
    auto *update = new QCheckBox(QStringLiteral("Update automatically"), &dlg.d);
    dlg.v->addWidget(list);
    dlg.v->addWidget(update);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg.d, &QDialog::accept);
    if (!dlg.exec()) return;
    const QString f = list->currentItem()->data(Qt::UserRole).toString();
    if (update->isChecked()) ed->insertField(QStringLiteral("datetime:") + f);
    else ed->insertTextBlock(now.toString(f), QStringLiteral("Insert Date and Time"));
}

// ---------------- Hyperlink / Bookmark ----------------
void hyperlinkDialog(QWidget *p, Editor *ed)
{
    Dlg dlg(p, QStringLiteral("Insert Hyperlink"));
    auto *kind = new QComboBox(&dlg.d);
    kind->addItems({"Existing file or web page", "Place in this document", "Email address"});
    auto *addr = new QLineEdit(&dlg.d);
    auto *display = new QLineEdit(&dlg.d);
    const QTextCharFormat cf = ed->currentCharFormat();
    addr->setText(cf.anchorHref());
    if (ed->isEditingText() && ed->cursor().hasSelection()) display->setText(ed->cursor().selectedText());
    auto *pages = new QComboBox(&dlg.d);
    for (int i = 0; i < ed->doc()->pages.size(); ++i) pages->addItem(QStringLiteral("Page %1").arg(i + 1));
    auto *form = new QFormLayout();
    form->addRow(QStringLiteral("Link to:"), kind);
    form->addRow(QStringLiteral("Text to display:"), display);
    form->addRow(QStringLiteral("Address:"), addr);
    form->addRow(QStringLiteral("Page:"), pages);
    pages->setEnabled(false);
    QObject::connect(kind, &QComboBox::currentIndexChanged, &dlg.d, [=](int i) {
        pages->setEnabled(i == 1);
        addr->setEnabled(i != 1);
        addr->setPlaceholderText(i == 2 ? QStringLiteral("name@example.com") : QStringLiteral("https://"));
    });
    dlg.v->addLayout(form);
    auto *remove = dlg.bb->addButton(QStringLiteral("Remove Link"), QDialogButtonBox::ResetRole);
    bool removing = false;
    QObject::connect(remove, &QPushButton::clicked, &dlg.d, [&] { removing = true; dlg.d.accept(); });
    if (!dlg.exec()) return;
    QString href = kind->currentIndex() == 1 ? QStringLiteral("#page%1").arg(pages->currentIndex() + 1)
                 : kind->currentIndex() == 2 ? "mailto:" + addr->text().remove("mailto:") : addr->text().trimmed();
    if (kind->currentIndex() == 0 && !href.contains("://") && !href.isEmpty() && !QFileInfo::exists(href)) href = "https://" + href;
    if (!ed->isEditingText()) {
        ed->forEachSelected(QStringLiteral("Hyperlink"), [&](Item *it) { it->hyperlink = removing ? QString() : href; });
        return;
    }
    QTextCharFormat f;
    if (removing) {
        f.setAnchor(false);
        f.setAnchorHref(QString());
        f.setFontUnderline(false);
        ed->mergeCharFormat(f, QStringLiteral("Remove Hyperlink"));
        return;
    }
    f.setAnchor(true);
    f.setAnchorHref(href);
    if (!ed->cursor().hasSelection()) {
        ed->beginChange(QStringLiteral("Insert Hyperlink"));
        QTextCharFormat cf2 = ed->cursor().charFormat();
        cf2.merge(f);
        ed->cursor().insertText(display->text().isEmpty() ? href : display->text(), cf2);
        ed->endChange();
        ed->textEdited();
    } else {
        ed->mergeCharFormat(f, QStringLiteral("Insert Hyperlink"));
    }
}

void bookmarkDialog(QWidget *p, Editor *ed)
{
    bool ok = false;
    const QString name = QInputDialog::getText(p, QStringLiteral("Bookmark"), QStringLiteral("Bookmark name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    if (ed->isEditingText()) ed->setCharProperty(QTextFormat::AnchorName, QStringList{name.trimmed()}, QStringLiteral("Bookmark"));
    else ed->forEachSelected(QStringLiteral("Bookmark"), [&](Item *it) { it->name = "bookmark:" + name.trimmed(); });
}

// ---------------- Business Information ----------------
void businessInfoDialog(QWidget *p, Editor *ed)
{
    Document *doc = ed->doc();
    QVector<BusinessInfo> sets = doc->biz;
    int current = doc->bizCurrent;
    Dlg dlg(p, QStringLiteral("Business Information"));
    auto *top = new QHBoxLayout();
    auto *setBox = new QComboBox(&dlg.d);
    for (const auto &b : sets) setBox->addItem(b.setName);
    setBox->setCurrentIndex(current);
    auto *newSet = new QPushButton(QStringLiteral("New…"), &dlg.d);
    auto *delSet = new QPushButton(QStringLiteral("Delete"), &dlg.d);
    top->addWidget(new QLabel(QStringLiteral("Select a Business Information set:")));
    top->addWidget(setBox, 1);
    top->addWidget(newSet);
    top->addWidget(delSet);
    dlg.v->addLayout(top);
    auto *form = new QFormLayout();
    QHash<QString, QWidget *> edits;
    for (const QString &k : BusinessInfo::keys()) {
        QWidget *w;
        if (k == "address") { auto *pe = new QPlainTextEdit(&dlg.d); pe->setFixedHeight(60); w = pe; }
        else w = new QLineEdit(&dlg.d);
        edits[k] = w;
        form->addRow(BusinessInfo::label(k) + ':', w);
    }
    auto *setName = new QLineEdit(&dlg.d);
    form->addRow(QStringLiteral("Business Information set name:"), setName);
    auto *logo = new QLabel(&dlg.d);
    logo->setFixedSize(96, 64);
    logo->setFrameShape(QFrame::StyledPanel);
    logo->setAlignment(Qt::AlignCenter);
    auto *addLogo = new QPushButton(QStringLiteral("Add Logo…"), &dlg.d);
    auto *removeLogo = new QPushButton(QStringLiteral("Remove Logo"), &dlg.d);
    auto *logoRow = new QHBoxLayout();
    logoRow->addWidget(logo);
    logoRow->addWidget(addLogo);
    logoRow->addWidget(removeLogo);
    form->addRow(QStringLiteral("Logo:"), logoRow);
    dlg.v->addLayout(form);
    auto load = [&](int i) {
        const BusinessInfo &b = sets[i];
        for (const QString &k : BusinessInfo::keys()) {
            if (auto *le = qobject_cast<QLineEdit *>(edits[k])) le->setText(b.field(k));
            else static_cast<QPlainTextEdit *>(edits[k])->setPlainText(b.field(k));
        }
        setName->setText(b.setName);
        const QImage img = b.logoImageId.isEmpty() ? QImage() : doc->image(b.logoImageId);
        logo->setPixmap(img.isNull() ? QPixmap() : QPixmap::fromImage(img.scaled(90, 60, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    };
    auto store = [&](int i) {
        BusinessInfo &b = sets[i];
        for (const QString &k : BusinessInfo::keys()) {
            if (auto *le = qobject_cast<QLineEdit *>(edits[k])) b.setField(k, le->text());
            else b.setField(k, static_cast<QPlainTextEdit *>(edits[k])->toPlainText());
        }
        b.setName = setName->text().trimmed().isEmpty() ? b.setName : setName->text().trimmed();
    };
    load(current);
    QObject::connect(setBox, &QComboBox::currentIndexChanged, &dlg.d, [&](int i) {
        if (i < 0) return;
        store(current);
        current = i;
        load(i);
    });
    QObject::connect(newSet, &QPushButton::clicked, &dlg.d, [&] {
        store(current);
        BusinessInfo b;
        b.setName = QStringLiteral("Business Information %1").arg(sets.size() + 1);
        sets << b;
        setBox->addItem(b.setName);
        setBox->setCurrentIndex(sets.size() - 1);
    });
    QObject::connect(delSet, &QPushButton::clicked, &dlg.d, [&] {
        if (sets.size() <= 1) return;
        sets.removeAt(current);
        const QSignalBlocker blk(setBox);
        setBox->removeItem(current);
        current = std::min(current, int(sets.size()) - 1);
        setBox->setCurrentIndex(current);
        load(current);
    });
    QString pendingLogo;
    QObject::connect(addLogo, &QPushButton::clicked, &dlg.d, [&] {
        const QString f = QFileDialog::getOpenFileName(&dlg.d, QStringLiteral("Logo"), QString(), QStringLiteral("Pictures (*.png *.jpg *.jpeg *.gif *.bmp *.svg *.wmf *.emf)"));
        if (f.isEmpty()) return;
        QFile file(f);
        if (!file.open(QIODevice::ReadOnly)) return;
        sets[current].logoImageId = doc->addImage(file.readAll(), QFileInfo(f).suffix().toLower(), f);
        load(current);
    });
    QObject::connect(removeLogo, &QPushButton::clicked, &dlg.d, [&] { sets[current].logoImageId.clear(); load(current); });
    if (!dlg.exec()) return;
    store(current);
    ed->change(QStringLiteral("Business Information"), [&] {
        doc->biz = sets;
        doc->bizCurrent = current;
    });
    Q_UNUSED(pendingLogo);
}

// ---------------- Schemes ----------------
void colorSchemeDialog(QWidget *p, Editor *ed)
{
    ColorScheme s = ed->doc()->colors;
    Dlg dlg(p, QStringLiteral("Create New Color Scheme"));
    auto *form = new QFormLayout();
    QVector<QPushButton *> btns;
    for (int i = 0; i < SlotCount; ++i) {
        auto *b = new QPushButton(&dlg.d);
        b->setFixedSize(80, 26);
        auto paint = [b, &s, i] { b->setStyleSheet(QStringLiteral("background:%1; border:1px solid #888;").arg(s.c[i].name())); };
        paint();
        QObject::connect(b, &QPushButton::clicked, &dlg.d, [b, &s, i, paint] {
            const QColor c = colorsDialog(b, s.c[i], slotName(i));
            if (c.isValid()) { s.c[i] = c; paint(); }
        });
        form->addRow(slotName(i) + ':', b);
        btns << b;
    }
    auto *name = new QLineEdit(QStringLiteral("Custom 1"), &dlg.d);
    form->addRow(QStringLiteral("Color scheme name:"), name);
    dlg.v->addLayout(form);
    if (!dlg.exec()) return;
    s.name = name->text().trimmed().isEmpty() ? QStringLiteral("Custom") : name->text().trimmed();
    ed->change(QStringLiteral("Color Scheme"), [&] { ed->doc()->colors = s; });
}

void fontSchemeDialog(QWidget *p, Editor *ed)
{
    FontScheme s = ed->doc()->fonts;
    Dlg dlg(p, QStringLiteral("Create New Font Scheme"));
    auto *form = new QFormLayout();
    auto *heading = new QComboBox(&dlg.d), *body = new QComboBox(&dlg.d);
    heading->addItems(QFontDatabase::families());
    body->addItems(QFontDatabase::families());
    heading->setCurrentText(s.heading);
    body->setCurrentText(s.body);
    auto *name = new QLineEdit(QStringLiteral("Custom 1"), &dlg.d);
    form->addRow(QStringLiteral("Heading font:"), heading);
    form->addRow(QStringLiteral("Body font:"), body);
    form->addRow(QStringLiteral("Font scheme name:"), name);
    auto *sample = new QLabel(&dlg.d);
    auto upd = [&] {
        sample->setText(QStringLiteral("<span style=\"font-family:'%1'; font-size:18pt\">Heading</span><br><span style=\"font-family:'%2'; font-size:10pt\">Body text, lorem-free: the quick brown fox jumps over the lazy dog.</span>")
                            .arg(heading->currentText(), body->currentText()));
    };
    QObject::connect(heading, &QComboBox::currentTextChanged, &dlg.d, upd);
    QObject::connect(body, &QComboBox::currentTextChanged, &dlg.d, upd);
    upd();
    dlg.v->addLayout(form);
    dlg.v->addWidget(sample);
    if (!dlg.exec()) return;
    s = FontScheme{name->text().trimmed().isEmpty() ? QStringLiteral("Custom") : name->text().trimmed(), heading->currentText(), body->currentText()};
    ed->change(QStringLiteral("Font Scheme"), [&] { ed->doc()->fonts = s; });
}

// ---------------- Fill Effects ----------------
bool fillEffectsDialog(QWidget *p, Editor *ed, Fill &fill, const QString &title)
{
    Fill f = fill;
    Dlg dlg(p, title.isEmpty() ? QStringLiteral("Fill Effects") : title);
    auto *tabs = new QTabWidget(&dlg.d);
    // Gradient.
    auto *g = new QWidget();
    auto *gf = new QFormLayout(g);
    auto *oneTwo = new QComboBox(g);
    oneTwo->addItems({"Two colors", "Preset"});
    auto *c1 = colorPick(ed, f.type == Fill::Gradient ? f.color : ColorRef::scheme(Accent1), false, g);
    auto *c2 = colorPick(ed, f.type == Fill::Gradient ? f.color2 : ColorRef::rgb(Qt::white), false, g);
    auto *gstyle = new QComboBox(g);
    gstyle->addItems({"Linear", "Radial", "Rectangular", "From center"});
    gstyle->setCurrentIndex(int(f.gradType));
    auto *angle = new QSpinBox(g);
    angle->setRange(0, 359);
    angle->setSuffix(QStringLiteral("°"));
    angle->setValue(int(f.angle));
    auto *preset = new QComboBox(g);
    preset->addItems({"Early Sunset", "Late Sunset", "Nightfall", "Daybreak", "Horizon", "Desert", "Ocean", "Calm Water", "Fire", "Fog", "Moss", "Peacock", "Wheat", "Parchment", "Mahogany", "Rainbow", "Gold", "Silver", "Chrome", "Brass"});
    gf->addRow(QStringLiteral("Colors:"), oneTwo);
    gf->addRow(QStringLiteral("Color 1:"), c1);
    gf->addRow(QStringLiteral("Color 2:"), c2);
    gf->addRow(QStringLiteral("Preset:"), preset);
    gf->addRow(QStringLiteral("Shading style:"), gstyle);
    gf->addRow(QStringLiteral("Angle:"), angle);
    tabs->addTab(g, QStringLiteral("Gradient"));
    // Texture / picture.
    auto *t = new QWidget();
    auto *tf = new QVBoxLayout(t);
    auto *texList = new QListWidget(t);
    texList->setViewMode(QListView::IconMode);
    texList->setIconSize(QSize(56, 56));
    const QStringList textures{"Canvas", "Denim", "Linen", "Paper", "Parchment", "Recycled", "Wood", "Marble", "Granite", "Cork", "Sand", "Stationery"};
    for (const QString &n : textures) {
        auto *it = new QListWidgetItem(QIcon(QPixmap::fromImage(proceduralTexture(n, 112))), n);
        texList->addItem(it);
    }
    tf->addWidget(texList);
    auto *otherTex = new QPushButton(QStringLiteral("Other Texture…"), t);
    tf->addWidget(otherTex);
    tabs->addTab(t, QStringLiteral("Texture"));
    auto *pt = new QWidget();
    auto *pv = new QVBoxLayout(pt);
    auto *pickPic = new QPushButton(QStringLiteral("Select Picture…"), pt);
    auto *picLabel = new QLabel(pt);
    picLabel->setFixedSize(200, 140);
    picLabel->setFrameShape(QFrame::StyledPanel);
    auto *tile = new QCheckBox(QStringLiteral("Tile picture as texture"), pt);
    tile->setChecked(f.tile);
    pv->addWidget(pickPic);
    pv->addWidget(picLabel);
    pv->addWidget(tile);
    pv->addStretch(1);
    if (f.type == Fill::Picture && !f.imageId.isEmpty())
        picLabel->setPixmap(QPixmap::fromImage(ed->doc()->image(f.imageId).scaled(200, 140, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    tabs->addTab(pt, QStringLiteral("Picture"));
    // Pattern.
    auto *pa = new QWidget();
    auto *paf = new QFormLayout(pa);
    auto *patList = new QListWidget(pa);
    patList->setViewMode(QListView::IconMode);
    patList->setIconSize(QSize(32, 32));
    const auto pats = patternBrushes();
    for (int i = 0; i < pats.size(); ++i) {
        QPixmap pm(32, 32);
        pm.fill(Qt::white);
        QPainter pp(&pm);
        pp.fillRect(pm.rect(), QBrush(Qt::black, pats[i]));
        pp.end();
        patList->addItem(new QListWidgetItem(QIcon(pm), QString()));
    }
    patList->setCurrentRow(f.type == Fill::Pattern ? f.pattern : 0);
    auto *fg = colorPick(ed, f.type == Fill::Pattern ? f.color : ColorRef::scheme(Main), false, pa);
    auto *bg = colorPick(ed, f.type == Fill::Pattern ? f.color2 : ColorRef::rgb(Qt::white), true, pa);
    paf->addRow(patList);
    paf->addRow(QStringLiteral("Foreground:"), fg);
    paf->addRow(QStringLiteral("Background:"), bg);
    tabs->addTab(pa, QStringLiteral("Pattern"));
    // Tint.
    auto *ti = new QWidget();
    auto *tif = new QFormLayout(ti);
    auto *base = colorPick(ed, f.type == Fill::Solid ? f.color : ColorRef::scheme(Accent1), false, ti);
    auto *tintSlider = new QSlider(Qt::Horizontal, ti);
    tintSlider->setRange(-90, 90);
    tintSlider->setValue(f.color.lighten() ? -f.color.lighten() : f.color.darken());
    tif->addRow(QStringLiteral("Base color:"), base);
    tif->addRow(QStringLiteral("Tint (left) or shade (right):"), tintSlider);
    tabs->addTab(ti, QStringLiteral("Tint"));
    tabs->setCurrentIndex(f.type == Fill::Gradient ? 0 : f.type == Fill::Texture ? 1 : f.type == Fill::Picture ? 2 : f.type == Fill::Pattern ? 3 : 0);
    dlg.v->addWidget(tabs);
    QString texChoice, picChoice = f.imageId;
    QObject::connect(texList, &QListWidget::currentTextChanged, &dlg.d, [&](const QString &n) { texChoice = n; });
    QObject::connect(otherTex, &QPushButton::clicked, &dlg.d, [&] {
        const QString file = QFileDialog::getOpenFileName(&dlg.d, QStringLiteral("Texture"), QString(), QStringLiteral("Pictures (*.png *.jpg *.jpeg *.bmp *.gif)"));
        if (file.isEmpty()) return;
        QFile fl(file);
        if (fl.open(QIODevice::ReadOnly)) texChoice = "file:" + ed->doc()->addImage(fl.readAll(), QFileInfo(file).suffix().toLower(), file);
    });
    QObject::connect(pickPic, &QPushButton::clicked, &dlg.d, [&] {
        const QString file = QFileDialog::getOpenFileName(&dlg.d, QStringLiteral("Picture"), QString(), QStringLiteral("Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.svg)"));
        if (file.isEmpty()) return;
        QFile fl(file);
        if (!fl.open(QIODevice::ReadOnly)) return;
        picChoice = ed->doc()->addImage(fl.readAll(), QFileInfo(file).suffix().toLower(), file);
        picLabel->setPixmap(QPixmap::fromImage(ed->doc()->image(picChoice).scaled(200, 140, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    });
    if (!dlg.exec()) return false;
    switch (tabs->currentIndex()) {
    case 0: {
        Fill n;
        n.type = Fill::Gradient;
        n.gradType = Fill::GradType(std::min(gstyle->currentIndex(), 2));
        n.angle = angle->value();
        if (oneTwo->currentIndex() == 1) {
            static const char *presets[][2] = {{"#5B2C83", "#F28A30"}, {"#1A0B2E", "#E8533B"}, {"#0B1026", "#2B4F81"}, {"#9FC4E8", "#FFF4D6"}, {"#F2B134", "#3A86FF"}, {"#C2925A", "#F4E3C1"},
                                               {"#00456E", "#5BC0EB"}, {"#B8E1F0", "#3A7CA5"}, {"#FFD200", "#C0121B"}, {"#DDE3E8", "#8796A3"}, {"#2E5D34", "#9BBF6A"}, {"#00808C", "#2C2C7A"},
                                               {"#F5DEB3", "#C69C5A"}, {"#F4EDD7", "#D6C29A"}, {"#5C1F0E", "#A0522D"}, {"#E53935", "#1E88E5"}, {"#F7D774", "#A8741A"},
                                               {"#F4F4F4", "#8A8A8A"}, {"#FFFFFF", "#5A5A5A"}, {"#E7C66A", "#8B6914"}};
            const int i = preset->currentIndex();
            n.color = ColorRef::rgb(QColor(presets[i][0]));
            n.color2 = ColorRef::rgb(QColor(presets[i][1]));
        } else {
            n.color = c1->current();
            n.color2 = c2->current();
        }
        n.transparency = f.transparency;
        f = n;
        break;
    }
    case 1: {
        if (texChoice.isEmpty()) return false;
        Fill n;
        n.type = Fill::Texture;
        n.tile = true;
        if (texChoice.startsWith("file:")) n.imageId = texChoice.mid(5);
        else {
            QByteArray png;
            QBuffer b(&png);
            b.open(QIODevice::WriteOnly);
            proceduralTexture(texChoice, 256).save(&b, "PNG");
            n.imageId = ed->doc()->addImage(png, "png", "texture:" + texChoice);
        }
        f = n;
        break;
    }
    case 2: {
        if (picChoice.isEmpty()) return false;
        Fill n;
        n.type = Fill::Picture;
        n.imageId = picChoice;
        n.tile = tile->isChecked();
        f = n;
        break;
    }
    case 3: {
        Fill n;
        n.type = Fill::Pattern;
        n.pattern = std::max(0, patList->currentRow());
        n.color = fg->current();
        n.color2 = bg->current();
        f = n;
        break;
    }
    case 4: {
        const ColorRef b = base->current();
        const int v = tintSlider->value();
        ColorRef c = b.kind() == ColorRef::Scheme ? ColorRef::scheme(b.slot(), v < 0 ? -v : 0, v > 0 ? v : 0) : b;
        if (b.kind() == ColorRef::Rgb) c = ColorRef::rgb(v < 0 ? mix(b.rgbValue(), Qt::white, -v / 100.0) : mix(b.rgbValue(), Qt::black, v / 100.0));
        f = Fill::solid(c);
        break;
    }
    }
    fill = f;
    return true;
}

void shadowDialog(QWidget *p, Editor *ed)
{
    Item *it = ed->single();
    if (!it) return;
    ShadowFx s = it->fx.shadow;
    Dlg dlg(p, QStringLiteral("Shadow"));
    auto *form = new QFormLayout();
    auto *on = new QCheckBox(QStringLiteral("Shadow"), &dlg.d);
    on->setChecked(s.on);
    auto *color = colorPick(ed, s.color, false, &dlg.d);
    auto *trans = new QSlider(Qt::Horizontal, &dlg.d);
    trans->setRange(0, 100);
    trans->setValue(int(s.transparency * 100));
    auto *blur = points(s.blur, &dlg.d, 0, 100);
    auto *dist = points(s.distance, &dlg.d, 0, 200);
    auto *angle = new QSpinBox(&dlg.d);
    angle->setRange(0, 359);
    angle->setSuffix(QStringLiteral("°"));
    angle->setValue(int(s.angle));
    form->addRow(on);
    form->addRow(QStringLiteral("Color:"), color);
    form->addRow(QStringLiteral("Transparency:"), trans);
    form->addRow(QStringLiteral("Blur:"), blur);
    form->addRow(QStringLiteral("Distance:"), dist);
    form->addRow(QStringLiteral("Angle:"), angle);
    dlg.v->addLayout(form);
    if (!dlg.exec()) return;
    ed->forEachSelected(QStringLiteral("Shadow"), [&](Item *x) {
        x->fx.shadow.on = on->isChecked();
        x->fx.shadow.color = color->current();
        x->fx.shadow.transparency = trans->value() / 100.0;
        x->fx.shadow.blur = blur->value();
        x->fx.shadow.distance = dist->value();
        x->fx.shadow.angle = angle->value();
    });
}

// ---------------- TextArt ----------------
void textArtTextDialog(QWidget *p, Editor *ed, const QString &itemId)
{
    auto *w = dynamic_cast<TextArtItem *>(ed->doc()->item(itemId));
    if (!w) return;
    Dlg dlg(p, QStringLiteral("Edit Text Art Text"));
    auto *row = new QHBoxLayout();
    auto *font = new QComboBox(&dlg.d);
    font->addItems(QFontDatabase::families());
    font->setCurrentText(w->font);
    auto *size = new QSpinBox(&dlg.d);
    size->setRange(4, 400);
    size->setValue(int(w->size));
    auto *bold = new QCheckBox(QStringLiteral("Bold"), &dlg.d);
    bold->setChecked(w->bold);
    auto *italic = new QCheckBox(QStringLiteral("Italic"), &dlg.d);
    italic->setChecked(w->italic);
    row->addWidget(new QLabel(QStringLiteral("Font:")));
    row->addWidget(font, 1);
    row->addWidget(new QLabel(QStringLiteral("Size:")));
    row->addWidget(size);
    row->addWidget(bold);
    row->addWidget(italic);
    dlg.v->addLayout(row);
    auto *text = new QPlainTextEdit(w->text, &dlg.d);
    text->setMinimumSize(420, 140);
    QFont tf(w->font);
    tf.setPointSize(16);
    text->setFont(tf);
    QObject::connect(font, &QComboBox::currentTextChanged, text, [text](const QString &f) { QFont x(f); x.setPointSize(16); text->setFont(x); });
    dlg.v->addWidget(text);
    text->selectAll();
    if (!dlg.exec()) return;
    ed->change(QStringLiteral("Edit Text Art Text"), [&] {
        const double oldSize = w->size;
        w->text = text->toPlainText().isEmpty() ? QStringLiteral(" ") : text->toPlainText();
        w->font = font->currentText();
        w->bold = bold->isChecked();
        w->italic = italic->isChecked();
        w->size = size->value();
        // A new size scales the frame.
        if (std::abs(oldSize - w->size) > 0.1 && oldSize > 0) {
            const double k = w->size / oldSize;
            const QPointF c = w->rect.center();
            w->rect.setSize(w->rect.size() * k);
            w->rect.moveCenter(c);
        }
    });
}

// ---------------- Calendar ----------------
void calendarDialog(QWidget *p, Editor *ed)
{
    Dlg dlg(p, QStringLiteral("Calendar"));
    auto *form = new QFormLayout();
    auto *month = new QComboBox(&dlg.d);
    for (int m = 1; m <= 12; ++m) month->addItem(QLocale().monthName(m));
    month->setCurrentIndex(QDate::currentDate().month() - 1);
    auto *year = new QSpinBox(&dlg.d);
    year->setRange(1900, 2200);
    year->setValue(QDate::currentDate().year());
    auto *style = new QComboBox(&dlg.d);
    style->addItems({"Classic grid", "Banded", "Minimal", "Bold header", "One per page (12 months)"});
    form->addRow(QStringLiteral("Month:"), month);
    form->addRow(QStringLiteral("Year:"), year);
    form->addRow(QStringLiteral("Design:"), style);
    dlg.v->addLayout(form);
    if (!dlg.exec()) return;
    const QRectF content = QRectF(QPointF(0, 0), ed->doc()->pageSize()).marginsRemoved(ed->doc()->setup.margins);
    ed->beginChange(QStringLiteral("Insert Calendar"));
    QStringList ids;
    if (style->currentIndex() == 4) {
        for (int m = 1; m <= 12; ++m) {
            if (m > 1) ed->doc()->addPage(ed->currentPage() + m - 1, ed->doc()->pages[ed->currentPage()]->masterId);
            ItemList items = makeCalendar(*ed->doc(), content, year->value(), m, 1);
            for (auto &it : items) ed->doc()->pages[ed->currentPage() + m - 1]->items.push_back(it);
        }
    } else {
        ItemList items = makeCalendar(*ed->doc(), QRectF(content.left(), content.top() + content.height() * 0.35, content.width(), content.height() * 0.6),
                                      year->value(), month->currentIndex() + 1, style->currentIndex());
        for (auto &it : items) { ed->surfaceItems().push_back(it); ids << it->id; }
    }
    ed->endChange();
    ed->select(ids);
}

// ---------------- Styles ----------------
void styleDialog(QWidget *p, Editor *ed, const QString &styleName)
{
    Document *doc = ed->doc();
    const TextStyle *existing = styleName.isEmpty() ? nullptr : doc->style(styleName);
    TextStyle s = existing ? *existing : TextStyle();
    if (!existing) {
        s.name = QStringLiteral("New Style");
        s.basedOn = QStringLiteral("Normal");
        s.chr = ed->currentCharFormat();
        s.blk = ed->currentBlockFormat();
    }
    Dlg dlg(p, existing ? QStringLiteral("Modify Style") : QStringLiteral("New Style"));
    auto *form = new QFormLayout();
    auto *name = new QLineEdit(s.name, &dlg.d);
    auto *basedOn = new QComboBox(&dlg.d), *next = new QComboBox(&dlg.d);
    basedOn->addItem(QStringLiteral("(none)"));
    for (const auto &st : doc->styles) { basedOn->addItem(st.name); next->addItem(st.name); }
    basedOn->setCurrentText(s.basedOn.isEmpty() ? QStringLiteral("(none)") : s.basedOn);
    next->setCurrentText(s.next.isEmpty() ? s.name : s.next);
    auto *charOnly = new QCheckBox(QStringLiteral("Character style (applies to selected text only)"), &dlg.d);
    charOnly->setChecked(s.charOnly);
    auto *font = new QComboBox(&dlg.d);
    font->addItem(QStringLiteral("(scheme font)"));
    font->addItems(QFontDatabase::families());
    if (s.chr.hasProperty(QTextFormat::FontFamilies)) font->setCurrentText(s.chr.fontFamilies().toStringList().value(0));
    auto *size = new DecimalSpin(&dlg.d);
    size->setRange(1, 999);
    size->setValue(s.chr.hasProperty(QTextFormat::FontPointSize) ? s.chr.fontPointSize() : 11);
    auto *bold = new QCheckBox(QStringLiteral("Bold"), &dlg.d), *italic = new QCheckBox(QStringLiteral("Italic"), &dlg.d);
    bold->setChecked(s.chr.fontWeight() >= QFont::DemiBold);
    italic->setChecked(s.chr.fontItalic());
    auto *color = colorPick(ed, s.chr.stringProperty(tp::ColorRefP).isEmpty() ? ColorRef::scheme(Main) : ColorRef::fromString(s.chr.stringProperty(tp::ColorRefP)), false, &dlg.d);
    auto *before = points(s.blk.topMargin(), &dlg.d), *after = points(s.blk.bottomMargin(), &dlg.d);
    auto *align = new QComboBox(&dlg.d);
    align->addItems({"Left", "Center", "Right", "Justify"});
    const Qt::Alignment al = s.blk.alignment() & Qt::AlignHorizontal_Mask;
    align->setCurrentIndex(al == Qt::AlignHCenter ? 1 : al == Qt::AlignRight ? 2 : al == Qt::AlignJustify ? 3 : 0);
    form->addRow(QStringLiteral("Style name:"), name);
    form->addRow(QStringLiteral("Based on:"), basedOn);
    form->addRow(QStringLiteral("Style for the next paragraph:"), next);
    form->addRow(charOnly);
    form->addRow(QStringLiteral("Font:"), font);
    form->addRow(QStringLiteral("Size:"), size);
    auto *bi = new QHBoxLayout();
    bi->addWidget(bold);
    bi->addWidget(italic);
    form->addRow(bi);
    form->addRow(QStringLiteral("Color:"), color);
    form->addRow(QStringLiteral("Alignment:"), align);
    form->addRow(QStringLiteral("Space before:"), before);
    form->addRow(QStringLiteral("Space after:"), after);
    dlg.v->addLayout(form);
    if (!dlg.exec()) return;
    const QString newName = name->text().trimmed().isEmpty() ? s.name : name->text().trimmed();
    s.name = newName;
    s.basedOn = basedOn->currentIndex() == 0 ? QString() : basedOn->currentText();
    s.next = next->currentText();
    s.charOnly = charOnly->isChecked();
    if (font->currentIndex() > 0) s.chr.setFontFamilies(QStringList{font->currentText()});
    else s.chr.clearProperty(QTextFormat::FontFamilies);
    s.chr.setFontPointSize(size->value());
    s.chr.setFontWeight(bold->isChecked() ? QFont::Bold : QFont::Normal);
    s.chr.setFontItalic(italic->isChecked());
    s.chr.setProperty(tp::ColorRefP, color->current().toString());
    const Qt::Alignment als[] = {Qt::AlignLeft, Qt::AlignHCenter, Qt::AlignRight, Qt::AlignJustify};
    s.blk.setAlignment(als[align->currentIndex()]);
    s.blk.setTopMargin(before->value());
    s.blk.setBottomMargin(after->value());
    s.blk.setProperty(tp::StyleName, s.name);
    ed->change(existing ? QStringLiteral("Modify Style") : QStringLiteral("New Style"), [&] {
        bool replaced = false;
        for (auto &st : doc->styles)
            if (st.name == styleName && existing) { st = s; replaced = true; }
        if (!replaced) doc->styles << s;
        // Restyle paragraphs that use this style.
        if (existing) {
            for (auto it = doc->stories.begin(); it != doc->stories.end(); ++it) {
                QTextDocument *d = (*it)->doc.get();
                for (QTextBlock b = d->begin(); b.isValid(); b = b.next()) {
                    if (b.blockFormat().stringProperty(tp::StyleName) != styleName) continue;
                    QTextCursor c(b);
                    QTextBlockFormat bf = b.blockFormat();
                    for (auto pi = s.blk.properties().cbegin(); pi != s.blk.properties().cend(); ++pi) bf.setProperty(pi.key(), pi.value());
                    c.setBlockFormat(bf);
                    c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                    c.mergeCharFormat(s.chr);
                }
            }
        }
    });
}

// ---------------- Word count ----------------
void wordCountDialog(QWidget *p, Editor *ed)
{
    Document *d = ed->doc();
    int words = 0, chars = 0, charsNoSpace = 0, paras = 0, lines = 0, boxes = 0, pics = 0;
    for (auto it = d->stories.cbegin(); it != d->stories.cend(); ++it) {
        const QString t = (*it)->doc->toPlainText();
        words += t.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).size();
        chars += t.size();
        charsNoSpace += QString(t).remove(QRegularExpression("\\s")).size();
        paras += (*it)->doc->blockCount();
        lines += t.count('\n') + 1;
    }
    d->forEachItem([&](Item *it, int, const QString &) {
        if (it->type() == ItemType::Text) ++boxes;
        if (it->type() == ItemType::Picture) ++pics;
    });
    QString sel;
    if (ed->isEditingText() && ed->cursor().hasSelection())
        sel = QStringLiteral("<br><br><b>Selection</b><br>Words: %1").arg(ed->cursor().selectedText().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).size());
    QMessageBox::information(p, QStringLiteral("Word Count"), QStringLiteral("<b>Publication</b><br>Pages: %1<br>Words: %2<br>Characters (no spaces): %3<br>Characters (with spaces): %4"
                                                                             "<br>Paragraphs: %5<br>Text boxes: %6<br>Pictures: %7%8")
                                                                  .arg(d->pages.size()).arg(words).arg(charsNoSpace).arg(chars).arg(paras).arg(boxes).arg(pics).arg(sel));
    Q_UNUSED(lines);
}

// ---------------- Options ----------------
void optionsDialog(QWidget *p, Editor *ed)
{
    Settings &st = Settings::get();
    Dlg dlg(p, QStringLiteral("JeffPub 79 Options"));
    auto *tabs = new QTabWidget(&dlg.d);
    auto *gen = new QWidget();
    auto *gf = new QFormLayout(gen);
    auto *name = new QLineEdit(st.userName(), gen);
    auto *initials = new QLineEdit(st.value("user/initials").toString(), gen);
    auto *theme = new QComboBox(gen);
    theme->addItems({"Use system setting", "Light", "Dark"});
    theme->setCurrentIndex(st.value("ui/theme", 0).toInt());
    gf->addRow(QStringLiteral("User name:"), name);
    gf->addRow(QStringLiteral("Initials:"), initials);
    gf->addRow(QStringLiteral("Theme:"), theme);
    tabs->addTab(gen, QStringLiteral("General"));
    auto *proof = new QWidget();
    auto *pf = new QFormLayout(proof);
    auto *asType = new QCheckBox(QStringLiteral("Check spelling as you type"), proof);
    asType->setChecked(ed->view.spelling);
    auto *ignoreUpper = new QCheckBox(QStringLiteral("Ignore words in UPPERCASE"), proof);
    ignoreUpper->setChecked(st.value("proof/ignoreUpper", true).toBool());
    auto *ignoreNum = new QCheckBox(QStringLiteral("Ignore words that contain numbers"), proof);
    ignoreNum->setChecked(st.value("proof/ignoreNumbers", true).toBool());
    auto *autocorrect = new QCheckBox(QStringLiteral("Replace text as you type (AutoCorrect)"), proof);
    autocorrect->setChecked(st.value("proof/autocorrect", true).toBool());
    auto *quotes = new QCheckBox(QStringLiteral("Replace straight quotes with smart quotes"), proof);
    auto *autoformat = new QCheckBox(QStringLiteral("AutoFormat as you type (dashes, fractions, ordinals, automatic lists)"), proof);
    autoformat->setChecked(st.value("proof/autoformat", true).toBool());
    quotes->setChecked(st.value("proof/smartQuotes", true).toBool());
    pf->addRow(asType);
    pf->addRow(ignoreUpper);
    pf->addRow(ignoreNum);
    pf->addRow(autocorrect);
    pf->addRow(quotes);
    pf->addRow(autoformat);
    tabs->addTab(proof, QStringLiteral("Proofing"));
    auto *save = new QWidget();
    auto *sf = new QFormLayout(save);
    auto *recover = new QSpinBox(save);
    recover->setRange(1, 120);
    recover->setValue(st.autoRecoverMinutes());
    recover->setSuffix(QStringLiteral(" minutes"));
    auto *backup = new QCheckBox(QStringLiteral("Always create backup copy"), save);
    backup->setChecked(st.value("save/backup", false).toBool());
    sf->addRow(QStringLiteral("Save AutoRecover information every:"), recover);
    sf->addRow(backup);
    tabs->addTab(save, QStringLiteral("Save"));
    auto *adv = new QWidget();
    auto *af = new QFormLayout(adv);
    auto *units = new QComboBox(adv);
    units->addItems({"Inches", "Centimeters", "Millimeters", "Points", "Picas"});
    units->setCurrentIndex(int(st.unit()));
    auto *nudge = measure(st.nudge(), adv, 0.1, 720);
    auto *recent = new QSpinBox(adv);
    recent->setRange(0, 50);
    recent->setValue(st.value("recentCount", 25).toInt());
    auto *wholeWord = new QCheckBox(QStringLiteral("When selecting, automatically select entire word"), adv);
    wholeWord->setChecked(st.value("edit/wholeWord", true).toBool());
    auto *dragText = new QCheckBox(QStringLiteral("Allow text to be dragged and dropped"), adv);
    dragText->setChecked(st.value("edit/dragText", true).toBool());
    auto *hyphenate = new QCheckBox(QStringLiteral("Automatically hyphenate in new text boxes"), adv);
    hyphenate->setChecked(st.value("edit/hyphenate", true).toBool());
    af->addRow(QStringLiteral("Measurement units:"), units);
    af->addRow(QStringLiteral("Arrow keys nudge objects by:"), nudge);
    af->addRow(QStringLiteral("Show this number of Recent Publications:"), recent);
    af->addRow(wholeWord);
    af->addRow(dragText);
    af->addRow(hyphenate);
    tabs->addTab(adv, QStringLiteral("Advanced"));
    dlg.v->addWidget(tabs);
    if (!dlg.exec()) return;
    st.setValue("user/name", name->text());
    st.setValue("user/initials", initials->text());
    st.setValue("ui/theme", theme->currentIndex());
    QGuiApplication::styleHints()->setColorScheme(theme->currentIndex() == 0 ? Qt::ColorScheme::Unknown : theme->currentIndex() == 1 ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark);
    st.setValue("proof/ignoreUpper", ignoreUpper->isChecked());
    st.setValue("proof/ignoreNumbers", ignoreNum->isChecked());
    st.setValue("proof/autocorrect", autocorrect->isChecked());
    st.setValue("proof/smartQuotes", quotes->isChecked());
    st.setValue("proof/autoformat", autoformat->isChecked());
    st.setValue("save/autoRecoverMinutes", recover->value());
    st.setValue("save/backup", backup->isChecked());
    st.setUnit(Unit(units->currentIndex()));
    st.setValue("edit/nudge", nudge->value());
    st.setValue("recentCount", recent->value());
    st.setValue("edit/wholeWord", wholeWord->isChecked());
    st.setValue("edit/dragText", dragText->isChecked());
    st.setValue("edit/hyphenate", hyphenate->isChecked());
    ed->setView([&](ViewOptions &v) { v.spelling = asType->isChecked(); });
}

// ---------------- Page numbers ----------------
void pageNumberDialog(QWidget *p, Editor *ed)
{
    Dlg dlg(p, QStringLiteral("Page Number"));
    auto *form = new QFormLayout();
    auto *pos = new QComboBox(&dlg.d);
    pos->addItems({"Top left", "Top center", "Top right", "Bottom left", "Bottom center", "Bottom right"});
    pos->setCurrentIndex(4);
    auto *format = new QComboBox(&dlg.d);
    format->addItems({"1, 2, 3", "i, ii, iii", "I, II, III", "a, b, c"});
    auto *first = new QCheckBox(QStringLiteral("Show page number on first page"), &dlg.d);
    first->setChecked(true);
    form->addRow(QStringLiteral("Position:"), pos);
    form->addRow(QStringLiteral("Number format:"), format);
    form->addRow(first);
    dlg.v->addLayout(form);
    if (!dlg.exec()) return;
    Document *d = ed->doc();
    const QString code = QStringList{"page", "page:roman", "page:ROMAN", "page:alpha"}[format->currentIndex()];
    MasterPage *m = d->masterFor(*d->pages[ed->currentPage()]);
    if (!m) m = d->masters.first().get();
    const QSizeF ps = d->pageSize();
    const QRectF content = QRectF(QPointF(0, 0), ps).marginsRemoved(d->setup.margins);
    const int pi = pos->currentIndex();
    const double y = pi < 3 ? std::max(6.0, content.top() - 28) : std::min(ps.height() - 30, content.bottom() + 6);
    ed->change(QStringLiteral("Page Number"), [&] {
        auto t = std::static_pointer_cast<TextItem>(ed->newTextBox(QRectF(content.left(), y, content.width(), 22)));
        t->name = QStringLiteral("Page Number");
        QTextCursor c(d->storyDoc(t->storyId));
        QTextBlockFormat bf;
        bf.setAlignment(pi % 3 == 0 ? Qt::AlignLeft : pi % 3 == 1 ? Qt::AlignHCenter : Qt::AlignRight);
        c.mergeBlockFormat(bf);
        QTextCharFormat f;
        f.setProperty(tp::Field, code);
        c.insertText(QString(QChar::ObjectReplacementCharacter), f);
        m->items.push_back(t);
        if (!first->isChecked() && d->pages.size() > 1) {
            // A master page without the number for page 1.
            auto noNum = std::make_shared<MasterPage>();
            noNum->id = QStringLiteral("T");
            noNum->abbr = QStringLiteral("T");
            noNum->name = QStringLiteral("Title Page (no page number)");
            noNum->grid = m->grid;
            for (const auto &it : m->items) if (it != t) noNum->items.push_back(d->cloneItem(*it));
            if (!d->master("T")) d->masters << noNum;
            d->pages[0]->masterId = QStringLiteral("T");
        }
    });
}

// ---------------- Properties ----------------
void documentPropertiesDialog(QWidget *p, Editor *ed, int tab)
{
    DocProps pr = ed->doc()->props;
    Dlg dlg(p, QStringLiteral("Properties"));
    auto *tabs = new QTabWidget(&dlg.d);
    auto *sum = new QWidget();
    auto *form = new QFormLayout(sum);
    auto *title = new QLineEdit(pr.title), *subject = new QLineEdit(pr.subject), *author = new QLineEdit(pr.author), *manager = new QLineEdit(pr.manager);
    auto *company = new QLineEdit(pr.company), *category = new QLineEdit(pr.category), *keywords = new QLineEdit(pr.keywords);
    auto *comments = new QPlainTextEdit(pr.comments);
    form->addRow(QStringLiteral("Title:"), title);
    form->addRow(QStringLiteral("Subject:"), subject);
    form->addRow(QStringLiteral("Author:"), author);
    form->addRow(QStringLiteral("Manager:"), manager);
    form->addRow(QStringLiteral("Company:"), company);
    form->addRow(QStringLiteral("Category:"), category);
    form->addRow(QStringLiteral("Keywords:"), keywords);
    form->addRow(QStringLiteral("Comments:"), comments);
    tabs->addTab(sum, QStringLiteral("Summary"));
    auto *print = new QWidget();
    auto *pf = new QFormLayout(print);
    auto *model = new QComboBox(print);
    model->addItems({"RGB (best for desktop printers and screens)", "Single color (spot)", "Spot colors", "Process colors (CMYK)", "Process plus spot colors"});
    model->setCurrentIndex(int(ed->doc()->print.model));
    auto *embed = new QCheckBox(QStringLiteral("Embed TrueType fonts when saving PDF"), print);
    embed->setChecked(ed->doc()->print.embedFonts);
    pf->addRow(QStringLiteral("Color model:"), model);
    // Spot colors: inks the print shop mixes, by name and color. They show
    // in every color drop-down, and print on plates of their own.
    QVector<QColor> spotColors = ed->doc()->print.spotColors;
    QStringList spotNames;
    for (int i = 0; i < spotColors.size(); ++i) spotNames << ed->doc()->print.spotName(i);
    auto *spotBox = new QWidget(print);
    auto *sv = new QVBoxLayout(spotBox);
    sv->setContentsMargins(0, 0, 0, 0);
    auto *spotList = new QListWidget(spotBox);
    spotList->setIconSize(QSize(28, 16));
    spotList->setMinimumHeight(110);
    auto fillSpots = [=, &spotColors, &spotNames] {
        spotList->clear();
        for (int i = 0; i < spotColors.size(); ++i) {
            QPixmap pm(28, 16);
            pm.fill(spotColors[i]);
            spotList->addItem(new QListWidgetItem(QIcon(pm), spotNames[i]));
        }
    };
    fillSpots();
    auto *sr = new QHBoxLayout();
    auto *addSpot = new QPushButton(QStringLiteral("Add…"), spotBox), *editSpot = new QPushButton(QStringLiteral("Modify…"), spotBox),
         *delSpot = new QPushButton(QStringLiteral("Remove"), spotBox);
    sr->addWidget(addSpot);
    sr->addWidget(editSpot);
    sr->addWidget(delSpot);
    sr->addStretch(1);
    sv->addWidget(spotList);
    sv->addLayout(sr);
    pf->addRow(QStringLiteral("Spot colors:"), spotBox);
    // Name and color of one spot color.
    auto askSpot = [&](QString *name, QColor *color) {
        QDialog sd(&dlg.d);
        sd.setWindowTitle(QStringLiteral("Spot Color"));
        auto *f = new QFormLayout(&sd);
        auto *n = new QLineEdit(*name, &sd);
        n->setPlaceholderText(QStringLiteral("The ink's name, as the print shop knows it"));
        auto *c = new QPushButton(&sd);
        QColor chosen = *color;
        auto paintBtn = [c, &chosen] { c->setStyleSheet(QStringLiteral("background:%1; border:1px solid #888; min-width:80px; min-height:22px;").arg(chosen.name())); };
        paintBtn();
        QObject::connect(c, &QPushButton::clicked, &sd, [&] {
            const QColor got = colorsDialog(&sd, chosen, QStringLiteral("Spot Color"));
            if (got.isValid()) { chosen = got; paintBtn(); }
        });
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &sd);
        QObject::connect(bb, &QDialogButtonBox::accepted, &sd, &QDialog::accept);
        QObject::connect(bb, &QDialogButtonBox::rejected, &sd, &QDialog::reject);
        f->addRow(QStringLiteral("Name:"), n);
        f->addRow(QStringLiteral("Color:"), c);
        f->addRow(bb);
        if (sd.exec() != QDialog::Accepted) return false;
        *name = n->text().trimmed();
        *color = chosen;
        return true;
    };
    QObject::connect(addSpot, &QPushButton::clicked, &dlg.d, [&] {
        if (model->currentIndex() == int(PrintInfo::SingleSpot) && !spotColors.isEmpty()) return;
        QString name = QStringLiteral("Spot color %1").arg(spotColors.size() + 1);
        QColor color(0, 90, 170);
        if (!askSpot(&name, &color)) return;
        spotColors << color;
        spotNames << name;
        fillSpots();
    });
    QObject::connect(editSpot, &QPushButton::clicked, &dlg.d, [&] {
        const int i = spotList->currentRow();
        if (i < 0) return;
        if (askSpot(&spotNames[i], &spotColors[i])) fillSpots();
    });
    QObject::connect(delSpot, &QPushButton::clicked, &dlg.d, [&] {
        const int i = spotList->currentRow();
        if (i < 0) return;
        spotColors.remove(i);
        spotNames.removeAt(i);
        fillSpots();
    });
    auto showSpots = [=] {
        const auto m = PrintInfo::ColorModel(model->currentIndex());
        spotBox->setEnabled(m == PrintInfo::SingleSpot || m == PrintInfo::SpotColors || m == PrintInfo::ProcessPlusSpot);
    };
    QObject::connect(model, &QComboBox::currentIndexChanged, &dlg.d, showSpots);
    showSpots();
    pf->addRow(embed);
    tabs->addTab(print, QStringLiteral("Commercial Print"));
    tabs->setCurrentIndex(std::clamp(tab, 0, 1));
    dlg.v->addWidget(tabs);
    if (!dlg.exec()) return;
    ed->change(QStringLiteral("Properties"), [&] {
        DocProps &d = ed->doc()->props;
        d.title = title->text(); d.subject = subject->text(); d.author = author->text(); d.manager = manager->text();
        d.company = company->text(); d.category = category->text(); d.keywords = keywords->text(); d.comments = comments->toPlainText();
        ed->doc()->print.model = PrintInfo::ColorModel(model->currentIndex());
        ed->doc()->print.embedFonts = embed->isChecked();
        ed->doc()->print.spotColors = spotColors;
        ed->doc()->print.spotNames = spotNames;
    });
}

// ---------------- Measurement toolbar ----------------
void measurementWindow(QWidget *p, Editor *ed)
{
    static QPointer<QDialog> win;
    if (win) { win->raise(); win->show(); return; }
    win = new QDialog(p, Qt::Tool);
    win->setWindowTitle(QStringLiteral("Measurement"));
    win->setAttribute(Qt::WA_DeleteOnClose);
    auto *form = new QFormLayout(win);
    auto *x = measure(0, win, -10000), *y = measure(0, win, -10000), *w = measure(0, win, 1), *h = measure(0, win, 1);
    x->setMinimum(-10000);
    y->setMinimum(-10000);
    auto *rot = new DecimalSpin(win);
    rot->setRange(-360, 360);
    rot->setSuffix(QStringLiteral("°"));
    auto *track = new DecimalSpin(win);
    track->setRange(0, 600);
    track->setSuffix(QStringLiteral("%"));
    auto *kern = new DecimalSpin(win);
    kern->setRange(-600, 600);
    kern->setSuffix(QStringLiteral(" pt"));
    auto *scale = new QSpinBox(win);
    scale->setRange(1, 600);
    scale->setSuffix(QStringLiteral("%"));
    auto *line = new DecimalSpin(win);
    line->setRange(0.1, 20);
    line->setSuffix(QStringLiteral(" sp"));
    form->addRow(QStringLiteral("Horizontal position:"), x);
    form->addRow(QStringLiteral("Vertical position:"), y);
    form->addRow(QStringLiteral("Width:"), w);
    form->addRow(QStringLiteral("Height:"), h);
    form->addRow(QStringLiteral("Rotation:"), rot);
    form->addRow(QStringLiteral("Tracking:"), track);
    form->addRow(QStringLiteral("Text scaling:"), scale);
    form->addRow(QStringLiteral("Kerning:"), kern);
    form->addRow(QStringLiteral("Line spacing:"), line);
    auto refresh = [=] {
        Item *it = ed->single();
        const bool on = it && it->type() != ItemType::Line && it->type() != ItemType::Group;
        for (QWidget *wd : QList<QWidget *>{x, y, w, h, rot}) wd->setEnabled(on);
        if (on) {
            x->setPoints(it->rect.x()); y->setPoints(it->rect.y()); w->setPoints(it->rect.width()); h->setPoints(it->rect.height());
            if (!rot->hasFocus()) { const QSignalBlocker b(rot); rot->setValue(it->rotation); }
        }
        const QTextCharFormat cf = ed->currentCharFormat();
        if (!track->hasFocus()) { const QSignalBlocker b(track); track->setValue(tp::trackingOf(cf)); }
        if (!kern->hasFocus()) { const QSignalBlocker b(kern); kern->setValue(tp::kerningOf(cf)); }
        if (!scale->hasFocus()) { const QSignalBlocker b(scale); scale->setValue(cf.hasProperty(QTextFormat::FontStretch) ? cf.fontStretch() : 100); }
        const QTextBlockFormat bf = ed->currentBlockFormat();
        if (!line->hasFocus()) { const QSignalBlocker b(line); line->setValue(bf.lineHeightType() == QTextBlockFormat::ProportionalHeight ? bf.lineHeight() / 100 : 1.0); }
    };
    QObject::connect(ed, &Editor::selectionChanged, win, refresh);
    QObject::connect(ed, &Editor::changed, win, refresh);
    QObject::connect(ed, &Editor::textCursorChanged, win, refresh);
    auto applyGeom = [=] {
        Item *it = ed->single();
        if (!it || it->type() == ItemType::Line || it->type() == ItemType::Group) return;
        ed->change(QStringLiteral("Measurement"), [&] {
            const QRectF before = it->rect;
            if (std::abs(w->value() - before.width()) > 0.01 || std::abs(h->value() - before.height()) > 0.01)
                it->scaleInto(before, QRectF(before.topLeft(), QSizeF(w->value(), h->value())));
            it->rect.moveTopLeft(QPointF(x->value(), y->value()));
            it->rotation = rot->value();
        });
    };
    for (QDoubleSpinBox *s : {static_cast<QDoubleSpinBox *>(x), static_cast<QDoubleSpinBox *>(y), static_cast<QDoubleSpinBox *>(w), static_cast<QDoubleSpinBox *>(h), static_cast<QDoubleSpinBox *>(rot)})
        QObject::connect(s, &QDoubleSpinBox::editingFinished, win, applyGeom);
    QObject::connect(track, &QDoubleSpinBox::editingFinished, win, [=] {
        QTextCharFormat f;
        f.setProperty(tp::Tracking, track->value());
        ed->mergeCharFormat(f, QStringLiteral("Tracking"));
    });
    QObject::connect(kern, &QDoubleSpinBox::editingFinished, win, [=] {
        QTextCharFormat f;
        f.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        f.setFontLetterSpacing(kern->value());
        ed->mergeCharFormat(f, QStringLiteral("Kerning"));
    });
    QObject::connect(scale, &QSpinBox::editingFinished, win, [=] {
        QTextCharFormat f;
        f.setFontStretch(scale->value());
        ed->mergeCharFormat(f, QStringLiteral("Text Scaling"));
    });
    QObject::connect(line, &QDoubleSpinBox::editingFinished, win, [=] { ed->setLineSpacing(QTextBlockFormat::ProportionalHeight, line->value() * 100); });
    refresh();
    win->show();
}

// ---------------- Tints ----------------
void tintsDialog(QWidget *p, Editor *ed, const std::function<void(const ColorRef &)> &apply)
{
    Dlg dlg(p, QStringLiteral("Tints"));
    auto *form = new QFormLayout();
    auto *base = new QComboBox(&dlg.d);
    for (int i = 0; i < SlotCount; ++i) base->addItem(slotName(i));
    base->setCurrentIndex(Accent1);
    auto *list = new QListWidget(&dlg.d);
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(36, 24));
    auto fill = [=] {
        list->clear();
        for (int t = -90; t <= 90; t += 10) {
            const ColorRef c = ColorRef::scheme(base->currentIndex(), t < 0 ? -t : 0, t > 0 ? t : 0);
            QPixmap pm(36, 24);
            pm.fill(c.resolve(ed->doc()->colors));
            auto *it = new QListWidgetItem(QIcon(pm), t < 0 ? QStringLiteral("%1% tint").arg(100 + t) : t > 0 ? QStringLiteral("%1% shade").arg(100 - t) : QStringLiteral("Base"));
            it->setData(Qt::UserRole, c.toString());
            list->addItem(it);
        }
    };
    QObject::connect(base, &QComboBox::currentIndexChanged, &dlg.d, fill);
    fill();
    form->addRow(QStringLiteral("Base color:"), base);
    form->addRow(list);
    dlg.v->addLayout(form);
    if (!dlg.exec() || !list->currentItem()) return;
    apply(ColorRef::fromString(list->currentItem()->data(Qt::UserRole).toString()));
}

// ---------------- Hyphenation, paste special, misc ----------------
void hyphenationDialog(QWidget *p, Editor *ed)
{
    // The story of the text box being typed in or selected: all its boxes.
    Document *d = ed->doc();
    const QString id = ed->isEditingText() ? ed->textTarget().itemId : (ed->single() ? ed->single()->id : QString());
    auto *t = dynamic_cast<TextItem *>(d->item(id));
    if (!t) {
        QMessageBox::information(p, QStringLiteral("Hyphenation"), QStringLiteral("Click in a text box first."));
        return;
    }
    const QVector<TextItem *> chain = d->chainOf(t->id);
    Dlg dlg(p, QStringLiteral("Hyphenation"));
    auto *autoH = new QCheckBox(QStringLiteral("Automatically hyphenate this story"), &dlg.d);
    autoH->setChecked(t->hyphenate);
    auto *zone = measure(t->hyphenZone, &dlg.d, 0, 720);
    zone->setToolTip(QStringLiteral("A word is broken only if moving it whole to the next line would leave more space than this."));
    auto *form = new QFormLayout();
    form->addRow(autoH);
    form->addRow(QStringLiteral("Hyphenation zone:"), zone);
    dlg.v->addLayout(form);
    auto *manual = dlg.bb->addButton(QStringLiteral("Insert Optional Hyphen"), QDialogButtonBox::ActionRole);
    manual->setEnabled(ed->isEditingText());
    QObject::connect(manual, &QPushButton::clicked, &dlg.d, [&] { if (ed->isEditingText()) ed->insertTextBlock(QString(QChar(0x00AD)), QStringLiteral("Optional Hyphen")); });
    auto *everywhere = dlg.bb->addButton(QStringLiteral("Add Optional Hyphens"), QDialogButtonBox::ActionRole);
    everywhere->setToolTip(QStringLiteral("Put an optional hyphen at every place each long word can break; they show only where a line breaks."));
    QObject::connect(everywhere, &QPushButton::clicked, &dlg.d, [&] {
        int n = 0;
        QTextDocument *sd = d->storyDoc(t->storyId);
        if (sd) ed->change(QStringLiteral("Hyphenate"), [&] { n = hyphenateStory(sd); });
        Q_EMIT ed->status(QStringLiteral("Added %1 optional hyphens.").arg(n));
    });
    zone->setEnabled(autoH->isChecked());
    QObject::connect(autoH, &QCheckBox::toggled, zone, &QWidget::setEnabled);
    if (!dlg.exec()) return;
    ed->change(QStringLiteral("Hyphenation"), [&] {
        for (TextItem *f : chain) {
            f->hyphenate = autoH->isChecked();
            f->hyphenZone = zone->value();
        }
    });
}

void pasteSpecialDialog(QWidget *p, Editor *ed)
{
    Dlg dlg(p, QStringLiteral("Paste Special"));
    auto *list = new QListWidget(&dlg.d);
    list->addItems({"Keep source formatting", "Keep text only (unformatted text)", "Picture (PNG)"});
    list->setCurrentRow(0);
    dlg.v->addWidget(list);
    if (!dlg.exec()) return;
    const int i = list->currentRow();
    if (i == 1) { ed->paste(true); return; }
    if (i == 2) {
        const QMimeData *md = QApplication::clipboard()->mimeData();
        if (md && md->hasImage()) {
            const bool wasEditing = ed->isEditingText();
            ed->endTextEdit();
            ed->paste();
            Q_UNUSED(wasEditing);
        }
        return;
    }
    ed->paste(false);
}


// Autoflow: while a story doesn't fit, it continues on a new page after
// the last box's page, in a linked text box placed like that box.
int autoflowText(Editor *ed, const QString &boxId)
{
    Document *d = ed->doc();
    auto overflows = [&] {
        const QVector<TextItem *> chain = d->chainOf(boxId);
        if (chain.isEmpty()) return false;
        TextItem *last = chain.last();
        const int pg = d->find(last->id).page;
        if (pg < 0) return false;
        // A fresh layout each time: the boxes change as pages are added.
        LayoutCache cache;
        RenderOptions opt;
        const auto fl = cache.textFrame(*d, *last, pg + 1, opt);
        return fl.layout && fl.layout->overflow();
    };
    int added = 0;
    ed->beginChange(QStringLiteral("Autoflow"));
    while (added < 500 && overflows()) {
        TextItem *last = d->chainOf(boxId).last();
        const int pg = d->find(last->id).page;
        auto page = d->addPage(pg + 1, d->pages[pg]->masterId);
        auto box = std::make_shared<TextItem>();
        box->rect = last->rect;
        box->rotation = last->rotation;
        box->insets = last->insets;
        box->columns = last->columns;
        box->columnGap = last->columnGap;
        box->fill = last->fill;
        box->stroke = last->stroke;
        box->hyphenate = last->hyphenate;
        box->hyphenZone = last->hyphenZone;
        box->storyId = last->storyId;
        last->nextId = box->id;
        page->items.push_back(box);
        ++added;
    }
    ed->endChange();
    return added;
}

// Offer autoflow when inserted text doesn't fit its box.
static void offerAutoflow(QWidget *p, Editor *ed, const QString &boxId)
{
    Document *d = ed->doc();
    const QVector<TextItem *> chain = d->chainOf(boxId);
    if (chain.isEmpty() || d->find(chain.last()->id).page < 0) return;
    TextItem *last = chain.last();
    RenderOptions opt;
    const auto fl = ed->cache().textFrame(*d, *last, d->find(last->id).page + 1, opt);
    if (!fl.layout || !fl.layout->overflow()) return;
    if (QMessageBox::question(p, QStringLiteral("Autoflow"),
                              QStringLiteral("The inserted text doesn't fit in the text box. Do you want to continue it on new pages, in text boxes like this one?"))
        != QMessageBox::Yes)
        return;
    const int n = autoflowText(ed, boxId);
    Q_EMIT ed->status(QStringLiteral("Added %1 page%2 for the rest of the text.").arg(n).arg(n == 1 ? "" : "s"));
}

void insertFileDialog(QWidget *p, Editor *ed)
{
    const QString f = QFileDialog::getOpenFileName(p, QStringLiteral("Insert Text"), QString(),
                                                   QStringLiteral("Text Files (*.txt *.rtf *.html *.htm *.docx *.md);;All Files (*)"));
    if (f.isEmpty()) return;
    if (ed->isEditingText()) {
        ed->beginChange(QStringLiteral("Insert File"));
        QTextDocument tmp;
        loadTextFileInto(&tmp, f);
        ed->cursor().insertFragment(QTextDocumentFragment(&tmp));
        ed->endChange();
        ed->textEdited();
        const QString box = ed->textTarget().itemId;
        if (dynamic_cast<TextItem *>(ed->doc()->item(box))) {
            ed->endTextEdit();
            offerAutoflow(p, ed, box);
        }
        return;
    }
    const QSizeF ps = ed->doc()->pageSize();
    const QRectF content = QRectF(QPointF(0, 0), ps).marginsRemoved(ed->doc()->setup.margins);
    auto t = std::static_pointer_cast<TextItem>(ed->newTextBox(content));
    loadTextFileInto(ed->doc()->storyDoc(t->storyId), f);
    ed->addItem(t);
    offerAutoflow(p, ed, t->id);
}

// ---------------- Mail merge recipients ----------------
void recipientsDialog(QWidget *p, Editor *ed, bool typeNew)
{
    MergeSource m = ed->doc()->merge;
    if (typeNew && m.isEmpty()) {
        m.fields = {"Title", "First Name", "Last Name", "Company Name", "Address Line 1", "Address Line 2", "City", "State", "ZIP Code", "Country or Region", "Home Phone", "Work Phone", "Email Address"};
        m.rows << QStringList(m.fields.size());
        m.include << true;
    }
    if (m.isEmpty()) {
        QMessageBox::information(p, QStringLiteral("Mail Merge Recipients"), QStringLiteral("Select a recipient list first (Mailings > Select Recipients)."));
        return;
    }
    Dlg dlg(p, typeNew ? QStringLiteral("New Address List") : QStringLiteral("Mail Merge Recipients"));
    dlg.d.resize(900, 520);
    auto *search = new QLineEdit(&dlg.d);
    search->setPlaceholderText(QStringLiteral("Filter recipients"));
    dlg.v->addWidget(search);
    auto *table = new QTableWidget(m.rows.size(), m.fields.size() + 1, &dlg.d);
    QStringList headers{QString()};
    headers << m.fields;
    table->setHorizontalHeaderLabels(headers);
    for (int r = 0; r < m.rows.size(); ++r) {
        auto *check = new QTableWidgetItem();
        check->setCheckState(m.include.value(r, true) ? Qt::Checked : Qt::Unchecked);
        table->setItem(r, 0, check);
        for (int c = 0; c < m.fields.size(); ++c) table->setItem(r, c + 1, new QTableWidgetItem(m.rows[r].value(c)));
    }
    table->setSortingEnabled(true);
    dlg.v->addWidget(table, 1);
    auto *row = new QHBoxLayout();
    auto *add = new QPushButton(QStringLiteral("New Entry"), &dlg.d);
    auto *del = new QPushButton(QStringLiteral("Delete Entry"), &dlg.d);
    auto *addField = new QPushButton(QStringLiteral("Customize Columns…"), &dlg.d);
    auto *dups = new QPushButton(QStringLiteral("Find Duplicates"), &dlg.d);
    auto *validate = new QPushButton(QStringLiteral("Validate Addresses"), &dlg.d);
    row->addWidget(add);
    row->addWidget(del);
    row->addWidget(addField);
    row->addWidget(dups);
    row->addWidget(validate);
    row->addStretch(1);
    dlg.v->addLayout(row);
    QObject::connect(search, &QLineEdit::textChanged, &dlg.d, [=](const QString &q) {
        for (int r = 0; r < table->rowCount(); ++r) {
            bool match = q.isEmpty();
            for (int c = 1; c < table->columnCount() && !match; ++c)
                if (table->item(r, c) && table->item(r, c)->text().contains(q, Qt::CaseInsensitive)) match = true;
            table->setRowHidden(r, !match);
        }
    });
    QObject::connect(add, &QPushButton::clicked, &dlg.d, [=] {
        table->setSortingEnabled(false);
        const int r = table->rowCount();
        table->insertRow(r);
        auto *check = new QTableWidgetItem();
        check->setCheckState(Qt::Checked);
        table->setItem(r, 0, check);
        for (int c = 1; c < table->columnCount(); ++c) table->setItem(r, c, new QTableWidgetItem());
        table->setCurrentCell(r, 1);
        table->editItem(table->item(r, 1));
        table->setSortingEnabled(true);
    });
    QObject::connect(del, &QPushButton::clicked, &dlg.d, [=] { if (table->currentRow() >= 0) table->removeRow(table->currentRow()); });
    QObject::connect(addField, &QPushButton::clicked, &dlg.d, [=] {
        bool ok = false;
        const QString n = QInputDialog::getText(table, QStringLiteral("Add Field"), QStringLiteral("Field name:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || n.trimmed().isEmpty()) return;
        const int c = table->columnCount();
        table->insertColumn(c);
        table->setHorizontalHeaderItem(c, new QTableWidgetItem(n.trimmed()));
    });
    QObject::connect(dups, &QPushButton::clicked, &dlg.d, [=] {
        QHash<QString, int> seen;
        int n = 0;
        for (int r = 0; r < table->rowCount(); ++r) {
            QString key;
            for (int c = 1; c < table->columnCount(); ++c) key += (table->item(r, c) ? table->item(r, c)->text().trimmed().toLower() : QString()) + '|';
            if (seen.contains(key)) { table->item(r, 0)->setCheckState(Qt::Unchecked); ++n; }
            else seen.insert(key, r);
        }
        QMessageBox::information(table, QStringLiteral("Find Duplicates"), QStringLiteral("Unchecked %1 duplicate entries.").arg(n));
    });
    QObject::connect(validate, &QPushButton::clicked, &dlg.d, [=] {
        int bad = 0;
        int zipCol = -1, addrCol = -1;
        for (int c = 1; c < table->columnCount(); ++c) {
            const QString h = table->horizontalHeaderItem(c)->text().toLower();
            if (h.contains("zip") || h.contains("postal")) zipCol = c;
            if (h.contains("address") && addrCol < 0) addrCol = c;
        }
        for (int r = 0; r < table->rowCount(); ++r) {
            const QString zip = zipCol > 0 && table->item(r, zipCol) ? table->item(r, zipCol)->text().trimmed() : QString();
            const QString addr = addrCol > 0 && table->item(r, addrCol) ? table->item(r, addrCol)->text().trimmed() : QString();
            const bool ok = !addr.isEmpty() && (zip.isEmpty() || QRegularExpression("^\\d{5}(-\\d{4})?$").match(zip).hasMatch() || zip.size() >= 3);
            for (int c = 1; c < table->columnCount(); ++c)
                if (table->item(r, c)) table->item(r, c)->setBackground(ok ? QBrush() : QBrush(QColor(255, 220, 200)));
            bad += !ok;
        }
        QMessageBox::information(table, QStringLiteral("Validate Addresses"), bad ? QStringLiteral("%1 entries have a missing address or a ZIP code that doesn't look right (highlighted).").arg(bad)
                                                                                  : QStringLiteral("All entries have an address and a well-formed ZIP code."));
    });
    if (!dlg.exec()) return;
    MergeSource out;
    out.path = m.path;
    out.pictureField = m.pictureField;
    for (int c = 1; c < table->columnCount(); ++c) out.fields << table->horizontalHeaderItem(c)->text();
    for (int r = 0; r < table->rowCount(); ++r) {
        QStringList rowv;
        bool any = false;
        for (int c = 1; c < table->columnCount(); ++c) {
            const QString v = table->item(r, c) ? table->item(r, c)->text() : QString();
            any |= !v.isEmpty();
            rowv << v;
        }
        if (!any) continue;
        out.rows << rowv;
        out.include << (table->item(r, 0) && table->item(r, 0)->checkState() == Qt::Checked);
    }
    if (typeNew) {
        const QString path = askSavePath(p, QStringLiteral("Save Address List"), QStringLiteral("Recipients.csv"), QStringLiteral("CSV (*.csv)"));
        if (!path.isEmpty()) { saveMergeCsv(out, path); out.path = path; }
    }
    ed->change(QStringLiteral("Recipients"), [&] { ed->doc()->merge = out; });
}

void mergeFieldDialog(QWidget *p, Editor *ed, int kind)
{
    if (ed->doc()->merge.isEmpty()) {
        QMessageBox::information(p, QStringLiteral("Mail Merge"), QStringLiteral("Select a recipient list first."));
        return;
    }
    const QString code = kind == 0 ? QStringLiteral("mergeblock:address") : QStringLiteral("mergeblock:greeting");
    if (ed->isEditingText()) { ed->insertField(code); return; }
    const QSizeF ps = ed->doc()->pageSize();
    auto t = std::static_pointer_cast<TextItem>(ed->newTextBox(QRectF(ps.width() / 2 - 144, ps.height() / 2 - 40, 288, kind == 0 ? 80 : 28)));
    QTextCursor c(ed->doc()->storyDoc(t->storyId));
    QTextCharFormat f;
    f.setProperty(tp::Field, code);
    c.insertText(QString(QChar::ObjectReplacementCharacter), f);
    ed->addItem(t);
}

void spellingDialog(QWidget *p, Editor *ed);   // defined in proofing.cpp
void thesaurusDialog(QWidget *p, Editor *ed);

} // namespace jp
