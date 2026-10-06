// Every command JeffPub 79 offers, as QActions used by the ribbon, menus and
// keyboard shortcuts.

#include "io/importers.h"
#include "app/appfuncs.h"
#include "app/mainwindow.h"
#include "render/renderer.h"
#include "app/pagespane.h"

#include "app/dialogs.h"
#include "app/icons.h"
#include "app/settings.h"
#include "app/taskpane.h"
#include "canvas/canvas.h"
#include "render/shapes.h"
#include "render/textart.h"
#include "templates/templates.h"
#include "text/textprops.h"

#include <QAction>
#include <QUrlQuery>
#include <QUrl>
#include <QDesktopServices>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>
#include <QTextDocumentFragment>
#include <QMimeData>

namespace jp {

static TableItem *selTable(Editor *ed)
{
    if (ed->isEditingText()) return dynamic_cast<TableItem *>(ed->doc()->item(ed->textTarget().itemId));
    return dynamic_cast<TableItem *>(ed->single());
}

// Table structure edits keep stories, spans and borders consistent.
static void tableInsertRow(Editor *ed, TableItem *t, int at)
{
    at = std::clamp(at, 0, t->rows);
    const int src = std::clamp(at == t->rows ? at - 1 : at, 0, t->rows - 1);
    QVector<TableCell> row;
    for (int c = 0; c < t->cols; ++c) {
        TableCell n;
        const TableCell &s = t->cell(src, c);
        n.fill = s.fill;
        n.border = s.border;
        n.margins = s.margins;
        n.valign = s.valign;
        n.storyId = ed->doc()->createStory();
        row << n;
    }
    t->cells.insert(at * t->cols, row.size(), TableCell());
    for (int c = 0; c < t->cols; ++c) t->cells[at * t->cols + c] = row[c];
    t->rowH.insert(at, t->rowH.value(src, 18));
    ++t->rows;
    t->syncRect();
}

static void tableInsertCol(Editor *ed, TableItem *t, int at)
{
    at = std::clamp(at, 0, t->cols);
    const int src = std::clamp(at == t->cols ? at - 1 : at, 0, t->cols - 1);
    QVector<TableCell> cells;
    for (int r = 0; r < t->rows; ++r)
        for (int c = 0; c <= t->cols; ++c) {
            if (c == at) {
                TableCell n;
                const TableCell &s = t->cell(r, src);
                n.fill = s.fill;
                n.border = s.border;
                n.margins = s.margins;
                n.storyId = ed->doc()->createStory();
                cells << n;
            }
            if (c < t->cols) cells << t->cell(r, c);
        }
    const double w = t->colW.value(src, 72);
    t->colW.insert(at, w);
    ++t->cols;
    t->cells = cells;
    // Keep the table the same width.
    double total = 0;
    for (double v : t->colW) total += v;
    const double k = (total - w) / total;
    for (double &v : t->colW) v *= k;
    t->syncRect();
}

static void tableDeleteRow(TableItem *t, int r)
{
    if (t->rows <= 1) return;
    t->cells.remove(r * t->cols, t->cols);
    t->rowH.removeAt(r);
    --t->rows;
    t->syncRect();
}

static void tableDeleteCol(TableItem *t, int c)
{
    if (t->cols <= 1) return;
    QVector<TableCell> cells;
    for (int r = 0; r < t->rows; ++r)
        for (int k = 0; k < t->cols; ++k)
            if (k != c) cells << t->cell(r, k);
    const double w = t->colW[c];
    t->colW.removeAt(c);
    --t->cols;
    t->cells = cells;
    for (double &v : t->colW) v += w / t->cols;
    t->syncRect();
}

void MainWindow::createActions()
{
    Editor *ed = m_ed;
    auto needsSel = [ed] { return !ed->selection().isEmpty(); };
    Q_UNUSED(needsSel);

    // ---------------- File / quick access ----------------
    mk("file.new", QStringLiteral("New"), "file-plus", QKeySequence::New, [this] { showBackstage("new"); });
    mk("file.blank", QStringLiteral("New Blank Publication"), "file", QKeySequence(), [this] {
        if (maybeSave()) newPublication(Document::blank(QSizeF(612, 792)));
    });
    mk("file.open", QStringLiteral("Open"), "folder-open", QKeySequence::Open, [this] {
        if (!maybeSave()) return;
        const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Open Publication"), Settings::get().value("dirs/open", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString(),
                                                       QStringLiteral("Publications (*.jpub *.pub);;JeffPub Publications (*.jpub);;.pub Publication Files (*.pub);;All Files (*)"));
        if (p.isEmpty()) return;
        Settings::get().setValue("dirs/open", QFileInfo(p).absolutePath());
        openFile(p);
    });
    mk("file.save", QStringLiteral("Save"), "save", QKeySequence::Save, [this] { save(); });
    mk("file.saveAs", QStringLiteral("Save As"), "save-all", QKeySequence(Qt::Key_F12), [this] { saveAs(); });
    mk("file.saveAsPub", QStringLiteral("Save as .pub File"), "file-output", QKeySequence(), [this] { saveAs("pub"); });
    mk("file.print", QStringLiteral("Print"), "printer", QKeySequence::Print, [this] { showBackstage("print"); });
    mk("file.printNow", QStringLiteral("Print"), "printer", QKeySequence(), [this] { printPublication(); });
    mk("file.exportPdf", QStringLiteral("Create PDF"), "file-text", QKeySequence(), [this] { exportPdfWithOptions(); });
    mk("file.exportImages", QStringLiteral("Save as Picture"), "image-down", QKeySequence(), [this] { exportImages(); });
    mk("file.exportHtml", QStringLiteral("Save as Web Page"), "globe", QKeySequence(), [this] { exportHtml(); });
    mk("file.close", QStringLiteral("Close"), "x", QKeySequence(Qt::CTRL | Qt::Key_F4), [this] {
        if (maybeSave()) newPublication(Document::blank(QSizeF(612, 792)));
    });
    mk("file.properties", QStringLiteral("Properties"), "info", QKeySequence(), [this] { documentPropertiesDialog(this, m_ed); });
    mk("file.options", QStringLiteral("Options"), "settings", QKeySequence(), [this] { optionsDialog(this, m_ed); });
    mk("edit.undo", QStringLiteral("Undo"), "undo-2", QKeySequence::Undo, [ed] { ed->undo(); });
    mk("edit.redo", QStringLiteral("Redo"), "redo-2", QKeySequence(Qt::CTRL | Qt::Key_Y), [ed] { ed->redo(); });

    // ---------------- Clipboard ----------------
    mk("edit.cut", QStringLiteral("Cut"), "scissors", QKeySequence::Cut, [ed] { ed->cut(); });
    mk("edit.copy", QStringLiteral("Copy"), "copy", QKeySequence::Copy, [ed] { ed->copy(); });
    mk("edit.paste", QStringLiteral("Paste"), "clipboard-paste", QKeySequence::Paste, [ed] { ed->paste(); });
    mk("edit.pasteText", QStringLiteral("Keep Text Only"), "clipboard-type", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), [ed] { ed->paste(true); });
    mk("edit.pasteSpecial", QStringLiteral("Paste Special…"), "clipboard-list", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V), [this] { pasteSpecialDialog(this, m_ed); });
    mk("edit.duplicate", QStringLiteral("Duplicate"), "copy-plus", QKeySequence(Qt::CTRL | Qt::Key_D), [ed] { ed->duplicateSelection(); });
    mk("edit.delete", QStringLiteral("Delete Object"), "trash-2", QKeySequence(), [ed] { ed->deleteSelection(); });
    mk("edit.formatPainter", QStringLiteral("Format Painter"), "paintbrush", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), [ed] {
        if (ed->tool() == Tool::FormatPainter) { ed->setTool(Tool::Select); return; }
        ed->painterItem = QJsonObject();
        ed->painterHasText = false;
        if (Item *it = ed->single()) {
            const QJsonObject o = it->toJson();
            for (const char *k : {"fill", "stroke", "fx"})
                if (o.contains(k)) ed->painterItem[k] = o[k];
            if (!o.contains("fill")) ed->painterItem["fill"] = Fill().toJson();
            if (!o.contains("stroke")) ed->painterItem["stroke"] = Stroke().toJson();
        }
        if (ed->isEditingText() || (ed->single() && ed->single()->hasText())) {
            ed->painterText = ed->currentCharFormat();
            ed->painterHasText = true;
        }
        ed->painterLocked = QApplication::keyboardModifiers() & Qt::ShiftModifier;
        ed->setTool(Tool::FormatPainter);
    });
    mk("edit.selectAll", QStringLiteral("Select All"), "text-select", QKeySequence::SelectAll, [ed] {
        if (ed->isEditingText()) {
            QTextCursor c = ed->cursor();
            c.select(QTextCursor::Document);
            ed->setCursor(c);
            return;
        }
        QStringList ids;
        for (const auto &it : ed->surfaceItems()) ids << it->id;
        ed->select(ids);
    });
    mk("edit.selectText", QStringLiteral("Select All Text in Text Box"), "text-select", QKeySequence(), [ed] {
        if (Item *it = ed->single(); it && it->hasText()) {
            if (!ed->isEditingText()) ed->beginTextEdit(it->id);
            QTextCursor c = ed->cursor();
            c.select(QTextCursor::Document);
            ed->setCursor(c);
        }
    });
    mk("edit.selectObjects", QStringLiteral("Select Objects"), "mouse-pointer-2", QKeySequence(), [ed] { ed->setTool(Tool::Select); ed->endTextEdit(); });
    for (const auto &[id, name, type] : {std::tuple{"sel.text", "Text Boxes", ItemType::Text}, {"sel.pictures", "Pictures", ItemType::Picture},
                                          {"sel.shapes", "Shapes", ItemType::Shape}, {"sel.tables", "Tables", ItemType::Table}, {"sel.textart", "Text Art", ItemType::TextArt}}) {
        const ItemType t = type;
        mk(id, QStringLiteral("Select All %1").arg(name), "", QKeySequence(), [ed, t] {
            QStringList ids;
            for (const auto &it : ed->surfaceItems())
                if (it->type() == t) ids << it->id;
            ed->select(ids);
        });
    }
    mk("edit.find", QStringLiteral("Find"), "search", QKeySequence::Find, [this] { showTaskPane("find"); });
    mk("edit.replace", QStringLiteral("Replace"), "replace", QKeySequence(Qt::CTRL | Qt::Key_H), [this] { showTaskPane("replace"); });

    // ---------------- Font ----------------
    mk("fmt.bold", QStringLiteral("Bold"), "bold", QKeySequence::Bold, [ed] { ed->toggleBold(); }, true);
    mk("fmt.italic", QStringLiteral("Italic"), "italic", QKeySequence::Italic, [ed] { ed->toggleItalic(); }, true);
    mk("fmt.underline", QStringLiteral("Underline"), "underline", QKeySequence::Underline, [ed] { ed->toggleUnderline(); }, true);
    mk("fmt.underlineDouble", QStringLiteral("Double Underline"), "", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D), [ed] {
        QTextCharFormat f;
        f.setUnderlineStyle(QTextCharFormat::SingleUnderline);
        f.setProperty(QTextFormat::UserProperty + 60, true);
        ed->toggleUnderline();
    });
    mk("fmt.underlineDotted", QStringLiteral("Dotted Underline"), "", QKeySequence(), [ed] { ed->toggleUnderline(QTextCharFormat::DotLine); });
    mk("fmt.underlineDash", QStringLiteral("Dashed Underline"), "", QKeySequence(), [ed] { ed->toggleUnderline(QTextCharFormat::DashUnderline); });
    mk("fmt.underlineWave", QStringLiteral("Wavy Underline"), "", QKeySequence(), [ed] { ed->toggleUnderline(QTextCharFormat::WaveUnderline); });
    mk("fmt.strike", QStringLiteral("Strikethrough"), "strikethrough", QKeySequence(), [ed] { ed->toggleStrike(); }, true);
    mk("fmt.sub", QStringLiteral("Subscript"), "subscript", QKeySequence(Qt::CTRL | Qt::Key_Equal), [ed] { ed->toggleScript(false); }, true);
    mk("fmt.sup", QStringLiteral("Superscript"), "superscript", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Plus), [ed] { ed->toggleScript(true); }, true);
    mk("fmt.grow", QStringLiteral("Grow Font"), "a-arrow-up", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Greater), [ed] { ed->growFont(1); });
    mk("fmt.shrink", QStringLiteral("Shrink Font"), "a-arrow-down", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Less), [ed] { ed->growFont(-1); });
    mk("fmt.clear", QStringLiteral("Clear All Formatting"), "remove-formatting", QKeySequence(Qt::CTRL | Qt::Key_Space), [ed] { ed->clearFormatting(); });
    mk("fmt.smallCaps", QStringLiteral("Small Caps"), "case-upper", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K), [ed] {
        QTextCharFormat f;
        f.setFontCapitalization(ed->currentCharFormat().fontCapitalization() == QFont::SmallCaps ? QFont::MixedCase : QFont::SmallCaps);
        ed->mergeCharFormat(f, QStringLiteral("Small Caps"));
    }, true);
    mk("fmt.allCaps", QStringLiteral("All Caps"), "case-upper", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), [ed] {
        QTextCharFormat f;
        f.setFontCapitalization(ed->currentCharFormat().fontCapitalization() == QFont::AllUppercase ? QFont::MixedCase : QFont::AllUppercase);
        ed->mergeCharFormat(f, QStringLiteral("All Caps"));
    }, true);
    const char *caseNames[] = {"Sentence case.", "lowercase", "UPPERCASE", "Capitalize Each Word", "tOGGLE cASE"};
    for (int i = 0; i < 5; ++i) mk(QStringLiteral("case.%1").arg(i), QString::fromLatin1(caseNames[i]), "", QKeySequence(), [ed, i] { ed->changeCase(i); });
    mk("case.cycle", QStringLiteral("Change Case"), "case-sensitive", QKeySequence(Qt::SHIFT | Qt::Key_F3), [ed] {
        static int next = 0;
        ed->changeCase(next);
        next = (next + 1) % 3;
    });
    const QPair<const char *, double> spacing[] = {{"Very Tight", -3}, {"Tight", -1.5}, {"Normal", 0}, {"Loose", 1.5}, {"Very Loose", 3}};
    for (const auto &s : spacing) {
        const double v = s.second;
        mk(QStringLiteral("spacing.%1").arg(QString::fromLatin1(s.first)), QString::fromLatin1(s.first), "", QKeySequence(), [ed, v] {
            QTextCharFormat f;
            f.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            f.setFontLetterSpacing(v);
            ed->mergeCharFormat(f, QStringLiteral("Character Spacing"));
        });
    }
    mk("fmt.spacingDialog", QStringLiteral("More Spacing…"), "", QKeySequence(), [this] { characterSpacingDialog(this, m_ed); });
    mk("fmt.fontDialog", QStringLiteral("Font…"), "type", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), [this] { fontDialog(this, m_ed); });

    // ---------------- Paragraph ----------------
    mk("para.bullets", QStringLiteral("Bullets"), "list", QKeySequence(), [ed] {
        ed->setList(ed->currentBlockFormat().isValid() && ed->isEditingText() && ed->cursor().block().textList() &&
                            ed->cursor().block().textList()->format().style() < 0 ? 0 : 1);
    }, true);
    mk("para.numbers", QStringLiteral("Numbering"), "list-ordered", QKeySequence(), [ed] {
        ed->setList(ed->isEditingText() && ed->cursor().block().textList() && ed->cursor().block().textList()->format().style() > 0 ? 0 : 2, 1);
    }, true);
    for (const auto &[id, ch] : {std::pair{"bullet.disc", "•"}, {"bullet.circle", "◦"}, {"bullet.square", "▪"}, {"bullet.diamond", "❖"},
                                 {"bullet.arrow", "➢"}, {"bullet.check", "✔"}, {"bullet.star", "★"}, {"bullet.dash", "–"}}) {
        const QString c = QString::fromUtf8(ch);
        mk(id, c, "", QKeySequence(), [ed, c] { ed->setList(1, 0, c); });
    }
    const char *numNames[] = {"1. 2. 3.", "a. b. c.", "A. B. C.", "i. ii. iii.", "I. II. III.", "1) 2) 3)", "(1) (2) (3)"};
    const int numFmt[] = {1, 2, 3, 4, 5, 6, 7};
    for (int i = 0; i < 7; ++i) {
        const int f = numFmt[i];
        mk(QStringLiteral("number.%1").arg(i), QString::fromLatin1(numNames[i]), "", QKeySequence(), [ed, f] { ed->setList(2, f); });
    }
    mk("para.listNone", QStringLiteral("None"), "", QKeySequence(), [ed] { ed->setList(0); });
    mk("para.bulletsDialog", QStringLiteral("Bullets and Numbering…"), "list-plus", QKeySequence(), [this] { bulletsDialog(this, m_ed, false); });
    mk("para.indentDec", QStringLiteral("Decrease Indent"), "indent-decrease", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M), [ed] { ed->changeIndent(-1); });
    mk("para.indentInc", QStringLiteral("Increase Indent"), "indent-increase", QKeySequence(Qt::CTRL | Qt::Key_M), [ed] { ed->changeIndent(1); });
    mk("para.special", QStringLiteral("Special Characters"), "pilcrow", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Y), [ed] {
        ed->setView([](ViewOptions &v) { v.special = !v.special; });
    }, true);
    mk("para.left", QStringLiteral("Align Left"), "align-left", QKeySequence(Qt::CTRL | Qt::Key_L), [ed] { ed->setAlignment(Qt::AlignLeft); }, true);
    mk("para.center", QStringLiteral("Center"), "align-center", QKeySequence(Qt::CTRL | Qt::Key_E), [ed] { ed->setAlignment(Qt::AlignHCenter); }, true);
    mk("para.right", QStringLiteral("Align Right"), "align-right", QKeySequence(Qt::CTRL | Qt::Key_R), [ed] { ed->setAlignment(Qt::AlignRight); }, true);
    mk("para.justify", QStringLiteral("Justify"), "align-justify", QKeySequence(Qt::CTRL | Qt::Key_J), [ed] { ed->setAlignment(Qt::AlignJustify); }, true);
    mk("para.distribute", QStringLiteral("Distribute"), "align-horizontal-space-between", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J), [ed] {
        QTextBlockFormat f;
        f.setAlignment(Qt::AlignJustify);
        f.setProperty(tp::Distribute, true);
        ed->mergeBlockFormat(f, QStringLiteral("Distribute"));
    }, true);
    const double spacings[] = {1.0, 1.15, 1.5, 2.0, 2.5, 3.0};
    for (double s : spacings)
        mk(QStringLiteral("ls.%1").arg(s), QString::number(s), "", QKeySequence(), [ed, s] { ed->setLineSpacing(QTextBlockFormat::ProportionalHeight, s * 100); });
    mk("para.spaceBefore0", QStringLiteral("0 pt Before"), "", QKeySequence(), [ed] { ed->setParagraphSpacing(0, -1); });
    mk("para.spaceBefore6", QStringLiteral("6 pt Before"), "", QKeySequence(), [ed] { ed->setParagraphSpacing(6, -1); });
    mk("para.spaceBefore12", QStringLiteral("12 pt Before"), "", QKeySequence(), [ed] { ed->setParagraphSpacing(12, -1); });
    mk("para.spaceAfter0", QStringLiteral("0 pt After"), "", QKeySequence(), [ed] { ed->setParagraphSpacing(-1, 0); });
    mk("para.spaceAfter6", QStringLiteral("6 pt After"), "", QKeySequence(), [ed] { ed->setParagraphSpacing(-1, 6); });
    mk("para.spaceAfter12", QStringLiteral("12 pt After"), "", QKeySequence(), [ed] { ed->setParagraphSpacing(-1, 12); });
    mk("para.dialog", QStringLiteral("Paragraph…"), "pilcrow", QKeySequence(), [this] { paragraphDialog(this, m_ed, 0); });
    mk("para.tabs", QStringLiteral("Tabs…"), "", QKeySequence(), [this] { paragraphDialog(this, m_ed, 2); });

    // ---------------- Styles ----------------
    mk("style.new", QStringLiteral("New Style…"), "plus", QKeySequence(), [this] { styleDialog(this, m_ed); });
    mk("style.modify", QStringLiteral("Modify Style…"), "pencil", QKeySequence(), [this] { styleDialog(this, m_ed, m_ed->currentStyleName().isEmpty() ? QStringLiteral("Normal") : m_ed->currentStyleName()); });
    mk("style.import", QStringLiteral("Import Styles…"), "import", QKeySequence(), [this] {
        const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Import Styles"), QString(), QStringLiteral("Publications (*.jpub *.pub)"));
        if (p.isEmpty()) return;
        QString err;
        auto src = loadAnyPublication(p, &err);
        if (!src) { QMessageBox::warning(this, QStringLiteral("Import Styles"), err); return; }
        m_ed->change(QStringLiteral("Import Styles"), [&] {
            for (const auto &s : src->styles) {
                bool replaced = false;
                for (auto &mine : m_ed->doc()->styles)
                    if (mine.name == s.name) { mine = s; replaced = true; }
                if (!replaced) m_ed->doc()->styles << s;
            }
        });
    });
    mk("style.byExample", QStringLiteral("New Style by Example"), "", QKeySequence(), [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("New Style"), QStringLiteral("Name the style based on the selected text:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        m_ed->change(QStringLiteral("New Style"), [&] {
            TextStyle s;
            s.name = name.trimmed();
            s.chr = m_ed->currentCharFormat();
            s.blk = m_ed->currentBlockFormat();
            s.blk.setProperty(tp::StyleName, s.name);
            s.basedOn = QStringLiteral("Normal");
            s.next = s.name;
            m_ed->doc()->styles << s;
        });
        m_ed->applyStyle(name.trimmed());
    });

    // ---------------- Objects / Insert ----------------
    mk("ins.textbox", QStringLiteral("Draw Text Box"), "text-cursor-input", QKeySequence(), [ed] { ed->setTool(Tool::Text); });
    mk("ins.tableDialog", QStringLiteral("Insert Table…"), "table", QKeySequence(), [this] { insertTableDialog(this, m_ed); });
    mk("ins.drawTable", QStringLiteral("Draw Table"), "pencil-ruler", QKeySequence(), [ed] { ed->setTool(Tool::Table); });
    mk("ins.picture", QStringLiteral("Pictures"), "image", QKeySequence(), [this] { insertPictureFromFile(); });
    mk("ins.onlinePicture", QStringLiteral("Online Pictures"), "globe", QKeySequence(), [this] { showTaskPane("online"); });
    mk("ins.placeholder", QStringLiteral("Picture Placeholder"), "image-plus", QKeySequence(), [ed] {
        auto pic = std::make_shared<PictureItem>();
        const QSizeF ps = ed->doc()->pageSize();
        pic->rect = QRectF(ps.width() / 2 - 108, ps.height() / 2 - 72, 216, 144);
        pic->imgRect = QRectF(0, 0, 216, 144);
        ed->addItem(pic);
    });
    mk("ins.textart", QStringLiteral("Text Art"), "type", QKeySequence(), [ed] { ed->setTool(Tool::TextArt); });
    mk("ins.bizInfo", QStringLiteral("Edit Business Information…"), "contact", QKeySequence(), [this] { businessInfoDialog(this, m_ed); });
    mk("ins.file", QStringLiteral("Insert File"), "file-input", QKeySequence(), [this] { insertFileDialog(this, m_ed); });
    mk("ins.symbol", QStringLiteral("Symbol"), "omega", QKeySequence(), [this] { symbolDialog(this, m_ed); });
    mk("ins.datetime", QStringLiteral("Date & Time"), "calendar-clock", QKeySequence(), [this] { dateTimeDialog(this, m_ed); });
    mk("ins.object", QStringLiteral("Object"), "paperclip", QKeySequence(), [this] {
        // Embedded objects become pictures of their content where JeffPub can render it.
        insertPictureFromFile();
    });
    mk("ins.link", QStringLiteral("Link"), "link", QKeySequence(Qt::CTRL | Qt::Key_K), [this] { hyperlinkDialog(this, m_ed); });
    mk("ins.bookmark", QStringLiteral("Bookmark"), "bookmark", QKeySequence(), [this] { bookmarkDialog(this, m_ed); });
    mk("ins.header", QStringLiteral("Header"), "panel-top", QKeySequence(), [this] {
        const Page *pg = m_ed->doc()->pages[m_ed->currentPage()].get();
        m_ed->setMasterView(pg->masterId.isEmpty() ? m_ed->doc()->masters.first()->id : pg->masterId);
        // Put the caret in (or create) a header box at the top margin.
        const QRectF content = QRectF(QPointF(0, 0), m_ed->doc()->pageSize()).marginsRemoved(m_ed->doc()->setup.margins);
        for (const auto &it : m_ed->surfaceItems())
            if (it->type() == ItemType::Text && it->rect.top() <= content.top() + 2 && it->rect.bottom() < content.top() + 60) { m_ed->beginTextEdit(it->id); return; }
        auto t = m_ed->newTextBox(QRectF(content.left(), std::max(9.0, content.top() - 30), content.width(), 24));
        t->name = QStringLiteral("Header");
        m_ed->addItem(t);
        m_ed->beginTextEdit(t->id);
    });
    mk("ins.footer", QStringLiteral("Footer"), "panel-bottom", QKeySequence(), [this] {
        const Page *pg = m_ed->doc()->pages[m_ed->currentPage()].get();
        m_ed->setMasterView(pg->masterId.isEmpty() ? m_ed->doc()->masters.first()->id : pg->masterId);
        const QSizeF ps = m_ed->doc()->pageSize();
        const QRectF content = QRectF(QPointF(0, 0), ps).marginsRemoved(m_ed->doc()->setup.margins);
        for (const auto &it : m_ed->surfaceItems())
            if (it->type() == ItemType::Text && it->rect.top() >= content.bottom() - 2) { m_ed->beginTextEdit(it->id); return; }
        auto t = m_ed->newTextBox(QRectF(content.left(), std::min(ps.height() - 33, content.bottom() + 6), content.width(), 24));
        t->name = QStringLiteral("Footer");
        m_ed->addItem(t);
        m_ed->beginTextEdit(t->id);
    });
    mk("ins.pageNumber", QStringLiteral("Insert Page Number"), "hash", QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_P), [this] {
        if (m_ed->isEditingText()) m_ed->insertField(QStringLiteral("page"));
        else pageNumberDialog(this, m_ed);
    });
    mk("ins.pageCount", QStringLiteral("Insert Page Count"), "", QKeySequence(), [ed] { ed->insertField(QStringLiteral("pages")); });
    mk("ins.pageNumberFormat", QStringLiteral("Format Page Numbers…"), "", QKeySequence(), [this] { pageNumberDialog(this, m_ed); });
    for (const QString &k : BusinessInfo::keys()) {
        mk("biz." + k, BusinessInfo::label(k), "", QKeySequence(), [this, k] {
            if (m_ed->isEditingText()) { m_ed->insertField("biz:" + k); return; }
            const QSizeF ps = m_ed->doc()->pageSize();
            auto t = std::static_pointer_cast<TextItem>(m_ed->newTextBox(QRectF(ps.width() / 2 - 108, ps.height() / 2 - 18, 216, k == "address" ? 54 : 30)));
            QTextCursor c(m_ed->doc()->storyDoc(t->storyId));
            QTextCharFormat f;
            f.setProperty(tp::Field, "biz:" + k);
            c.insertText(QString(QChar::ObjectReplacementCharacter), f);
            t->autofit = TextItem::GrowBox;
            m_ed->addItem(t);
        });
    }
    mk("biz.logo", QStringLiteral("Logo"), "", QKeySequence(), [this] {
        const QString id = m_ed->doc()->business().logoImageId;
        if (id.isEmpty()) { businessInfoDialog(this, m_ed); return; }
        auto pic = std::make_shared<PictureItem>();
        pic->imageId = id;
        pic->rect = QRectF(72, 72, 108, 108);
        pic->fitImage(m_ed->doc()->imageSize(id), false);
        m_ed->addItem(pic);
    });

    // ---------------- Pages ----------------
    mk("page.insert", QStringLiteral("Insert Blank Page"), "file-plus-2", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [ed] { ed->insertPages(ed->currentPage(), 1, false, false); });
    mk("page.insertDup", QStringLiteral("Insert Duplicate Page"), "copy", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U), [ed] { ed->insertPages(ed->currentPage(), 1, true, false); });
    mk("page.insertDialog", QStringLiteral("Insert Page…"), "files", QKeySequence(), [this] { insertPageDialog(this, m_ed); });
    mk("page.delete", QStringLiteral("Delete Page"), "file-minus", QKeySequence(), [this] {
        if (QMessageBox::question(this, QStringLiteral("Delete Page"), QStringLiteral("Delete page %1?").arg(m_ed->currentPage() + 1)) == QMessageBox::Yes)
            m_ed->deletePage(m_ed->currentPage());
    });
    mk("page.moveUp", QStringLiteral("Move Page Up"), "arrow-up", QKeySequence(), [ed] { ed->movePage(ed->currentPage(), ed->currentPage() - 1); });
    mk("page.moveDown", QStringLiteral("Move Page Down"), "arrow-down", QKeySequence(), [ed] { ed->movePage(ed->currentPage(), ed->currentPage() + 1); });
    mk("page.move", QStringLiteral("Move Page…"), "arrow-up-down", QKeySequence(), [this] {
        bool ok = false;
        const int to = QInputDialog::getInt(this, QStringLiteral("Move Page"), QStringLiteral("Move this page to position:"), m_ed->currentPage() + 1, 1,
                                            m_ed->doc()->pages.size(), 1, &ok);
        if (ok) m_ed->movePage(m_ed->currentPage(), to - 1);
    });
    mk("page.rename", QStringLiteral("Rename Page…"), "pencil-line", QKeySequence(), [this] {
        bool ok = false;
        const QString t = QInputDialog::getText(this, QStringLiteral("Rename Page"), QStringLiteral("Page title:"), QLineEdit::Normal,
                                                m_ed->doc()->pages[m_ed->currentPage()]->title, &ok);
        if (ok) m_ed->renamePage(m_ed->currentPage(), t);
    });
    mk("page.next", QStringLiteral("Next Page"), "chevron-right", QKeySequence(Qt::CTRL | Qt::Key_PageDown), [ed] { ed->setCurrentPage(ed->currentPage() + 1); });
    mk("page.prev", QStringLiteral("Previous Page"), "chevron-left", QKeySequence(Qt::CTRL | Qt::Key_PageUp), [ed] { ed->setCurrentPage(ed->currentPage() - 1); });
    mk("page.goto", QStringLiteral("Go to Page…"), "", QKeySequence(Qt::Key_F5), [this] {
        bool ok = false;
        const int p = QInputDialog::getInt(this, QStringLiteral("Go to Page"), QStringLiteral("Page:"), m_ed->currentPage() + 1, 1, m_ed->doc()->pages.size(), 1, &ok);
        if (ok) m_ed->setCurrentPage(p - 1);
    });

    // ---------------- Arrange ----------------
    mk("arr.front", QStringLiteral("Bring to Front"), "bring-to-front", QKeySequence(Qt::ALT | Qt::Key_F6), [ed] { ed->arrange(Editor::Order::Front); });
    mk("arr.forward", QStringLiteral("Bring Forward"), "arrow-up-from-line", QKeySequence(), [ed] { ed->arrange(Editor::Order::Forward); });
    mk("arr.backward", QStringLiteral("Send Backward"), "arrow-down-to-line", QKeySequence(), [ed] { ed->arrange(Editor::Order::Backward); });
    mk("arr.back", QStringLiteral("Send to Back"), "send-to-back", QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_F6), [ed] { ed->arrange(Editor::Order::Back); });
    mk("arr.group", QStringLiteral("Group"), "group", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G), [ed] {
        if (ed->selectionKind() == "group") ed->ungroupSelection(); else ed->groupSelection();
    });
    mk("arr.ungroup", QStringLiteral("Ungroup"), "ungroup", QKeySequence(), [ed] { ed->ungroupSelection(); });
    mk("arr.relMargins", QStringLiteral("Relative to Margin Guides"), "", QKeySequence(), [] {}, true);
    auto toMargins = [this] { return act("arr.relMargins")->isChecked(); };
    mk("arr.alignLeft", QStringLiteral("Align Left"), "align-start-vertical", QKeySequence(), [ed, toMargins] { ed->align(Editor::Align::Left, toMargins()); });
    mk("arr.alignCenter", QStringLiteral("Align Center"), "align-center-vertical", QKeySequence(), [ed, toMargins] { ed->align(Editor::Align::Center, toMargins()); });
    mk("arr.alignRight", QStringLiteral("Align Right"), "align-end-vertical", QKeySequence(), [ed, toMargins] { ed->align(Editor::Align::Right, toMargins()); });
    mk("arr.alignTop", QStringLiteral("Align Top"), "align-start-horizontal", QKeySequence(), [ed, toMargins] { ed->align(Editor::Align::Top, toMargins()); });
    mk("arr.alignMiddle", QStringLiteral("Align Middle"), "align-center-horizontal", QKeySequence(), [ed, toMargins] { ed->align(Editor::Align::Middle, toMargins()); });
    mk("arr.alignBottom", QStringLiteral("Align Bottom"), "align-end-horizontal", QKeySequence(), [ed, toMargins] { ed->align(Editor::Align::Bottom, toMargins()); });
    mk("arr.distH", QStringLiteral("Distribute Horizontally"), "align-horizontal-distribute-center", QKeySequence(), [ed, toMargins] { ed->distribute(true, toMargins()); });
    mk("arr.distV", QStringLiteral("Distribute Vertically"), "align-vertical-distribute-center", QKeySequence(), [ed, toMargins] { ed->distribute(false, toMargins()); });
    mk("arr.rotR", QStringLiteral("Rotate Right 90°"), "rotate-cw", QKeySequence(), [ed] { ed->rotateSelection(90); });
    mk("arr.rotL", QStringLiteral("Rotate Left 90°"), "rotate-ccw", QKeySequence(), [ed] { ed->rotateSelection(-90); });
    mk("arr.flipH", QStringLiteral("Flip Horizontal"), "flip-horizontal-2", QKeySequence(), [ed] { ed->flipSelection(true); });
    mk("arr.flipV", QStringLiteral("Flip Vertical"), "flip-vertical-2", QKeySequence(), [ed] { ed->flipSelection(false); });
    mk("arr.freeRotate", QStringLiteral("More Rotation Options…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 1); });
    auto setWrap = [ed](Wrap::Mode m) { ed->forEachSelected(QStringLiteral("Wrap Text"), [m](Item *it) { it->wrap.mode = m; }); };
    mk("wrap.none", QStringLiteral("None"), "", QKeySequence(), [setWrap] { setWrap(Wrap::None); }, true);
    mk("wrap.square", QStringLiteral("Square"), "", QKeySequence(), [setWrap] { setWrap(Wrap::Square); }, true);
    mk("wrap.tight", QStringLiteral("Tight"), "", QKeySequence(), [setWrap] { setWrap(Wrap::Tight); }, true);
    mk("wrap.through", QStringLiteral("Through"), "", QKeySequence(), [setWrap] { setWrap(Wrap::Through); }, true);
    mk("wrap.topBottom", QStringLiteral("Top and Bottom"), "", QKeySequence(), [setWrap] { setWrap(Wrap::TopBottom); }, true);
    mk("wrap.edit", QStringLiteral("Edit Wrap Points"), "wrap-points", QKeySequence(), [this] {
        Item *it = m_ed->single();
        if (!it) return;
        if (m_ed->wrapItem == it->id) { m_ed->setWrapItem(QString()); return; }
        // Text wraps tightly around the points; start from the object's outline.
        m_ed->change(QStringLiteral("Edit Wrap Points"), [&] {
            if (it->wrap.mode != Wrap::Tight && it->wrap.mode != Wrap::Through) it->wrap.mode = Wrap::Tight;
            if (it->wrap.points.size() < 3) it->wrap.points = Renderer::defaultWrapPolygon(*m_ed->doc(), *it);
        });
        m_ed->setWrapItem(it->id);
        statusBar()->showMessage(QStringLiteral("Drag a point to change how text wraps. Drag an edge to add a point; Ctrl+click a point to delete it. Press Esc when done."), 8000);
    });
    mk("wrap.more", QStringLiteral("More Layout Options…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 2); });
    mk("obj.lock", QStringLiteral("Lock Position and Size"), "lock", QKeySequence(), [ed] {
        const bool lock = ed->single() ? !ed->single()->locked : true;
        ed->forEachSelected(QStringLiteral("Lock"), [lock](Item *it) { it->locked = lock; });
    }, true);
    mk("obj.format", QStringLiteral("Format Object…"), "settings-2", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 0); });
    mk("obj.sizePos", QStringLiteral("Size and Position…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 1); });
    mk("obj.altText", QStringLiteral("Alt Text…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 5); });
    mk("obj.saveBlock", QStringLiteral("Save as Building Block…"), "package-plus", QKeySequence(), [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Save as Building Block"), QStringLiteral("Name:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        m_ed->copy();
        const QByteArray data = QApplication::clipboard()->mimeData()->data("application/x-jeffpub-items");
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/BuildingBlocks";
        QDir().mkpath(dir);
        QFile f(dir + "/" + name.trimmed() + ".json");
        if (f.open(QIODevice::WriteOnly)) f.write(data);
        statusBar()->showMessage(QStringLiteral("Saved \"%1\" to My Building Blocks.").arg(name.trimmed()), 4000);
    });

    // ---------------- Page Design ----------------
    mk("pd.changeTemplate", QStringLiteral("Change Template"), "layout-template", QKeySequence(), [this] { showBackstage("new"); });
    mk("pd.pageSetup", QStringLiteral("Page Setup…"), "file-cog", QKeySequence(), [this] { pageSetupDialog(this, m_ed); });
    mk("pd.guidesDialog", QStringLiteral("Grid and Baseline Guides…"), "grid-3x3", QKeySequence(), [this] { gridGuidesDialog(this, m_ed); });
    mk("pd.addH", QStringLiteral("Add Horizontal Ruler Guide"), "", QKeySequence(), [ed] {
        ed->change(QStringLiteral("Add Guide"), [ed] { ed->surface()->guides.h << ed->doc()->pageSize().height() / 2; });
    });
    mk("pd.addV", QStringLiteral("Add Vertical Ruler Guide"), "", QKeySequence(), [ed] {
        ed->change(QStringLiteral("Add Guide"), [ed] { ed->surface()->guides.v << ed->doc()->pageSize().width() / 2; });
    });
    mk("pd.clearGuides", QStringLiteral("Clear All Ruler Guides"), "", QKeySequence(), [ed] {
        ed->change(QStringLiteral("Clear Guides"), [ed] { ed->surface()->guides = RulerGuides(); });
    });
    mk("pd.alignGuides", QStringLiteral("Align to Guides"), "magnet", QKeySequence(), [ed] { ed->setView([](ViewOptions &v) { v.snapGuides = !v.snapGuides; }); }, true);
    mk("pd.alignObjects", QStringLiteral("Align to Objects"), "magnet", QKeySequence(), [ed] { ed->setView([](ViewOptions &v) { v.snapObjects = !v.snapObjects; }); }, true);
    mk("pd.portrait", QStringLiteral("Portrait"), "rectangle-vertical", QKeySequence(), [ed] {
        QSizeF s = ed->doc()->setup.size;
        if (s.width() <= s.height()) return;
        ed->change(QStringLiteral("Orientation"), [ed, s] { ed->doc()->setup.size = s.transposed(); ed->doc()->setup.sheet = ed->doc()->setup.sheet.transposed(); });
    });
    mk("pd.landscape", QStringLiteral("Landscape"), "rectangle-horizontal", QKeySequence(), [ed] {
        QSizeF s = ed->doc()->setup.size;
        if (s.width() >= s.height()) return;
        ed->change(QStringLiteral("Orientation"), [ed, s] { ed->doc()->setup.size = s.transposed(); ed->doc()->setup.sheet = ed->doc()->setup.sheet.transposed(); });
    });
    const QPair<const char *, double> margins[] = {{"None", 0}, {"Narrow", 18}, {"Moderate", 36}, {"Wide", 54}, {"Extra Wide", 72}};
    for (const auto &m : margins) {
        const double v = m.second;
        mk(QStringLiteral("margins.%1").arg(QString::fromLatin1(m.first)), QStringLiteral("%1 (%2)").arg(QString::fromLatin1(m.first), Settings::get().format(v)), "",
           QKeySequence(), [ed, v] { ed->change(QStringLiteral("Margins"), [ed, v] { ed->doc()->setup.margins = QMarginsF(v, v, v, v); }); });
    }
    // Margins on the Page Design tab are the margin guides (Publisher's word for them).
    mk("pd.customMargins", QStringLiteral("Custom Margins…"), "", QKeySequence(), [this] { gridGuidesDialog(this, m_ed, 0); });
    mk("pd.newPageSize", QStringLiteral("Create New Page Size…"), "file-plus", QKeySequence(), [this] { createPageSizeDialog(this, m_ed); });
    mk("pd.customSizes", QStringLiteral("Edit Custom Page Sizes…"), "", QKeySequence(), [this] { customPageSizesDialog(this, m_ed); });
    mk("pd.newColorScheme", QStringLiteral("Create New Color Scheme…"), "palette", QKeySequence(), [this] { colorSchemeDialog(this, m_ed); });
    mk("pd.newFontScheme", QStringLiteral("Create New Font Scheme…"), "type", QKeySequence(), [this] { fontSchemeDialog(this, m_ed); });
    mk("pd.updateFonts", QStringLiteral("Update Font Scheme"), "refresh-cw", QKeySequence(), [ed] {
        // Re-applies scheme fonts to text whose fonts were set by styles.
        ed->change(QStringLiteral("Update Font Scheme"), [ed] {
            for (auto &st : ed->doc()->styles) st.chr.clearProperty(QTextFormat::FontFamilies);
        });
    });
    mk("pd.bgNone", QStringLiteral("No Background"), "ban", QKeySequence(), [ed] {
        ed->change(QStringLiteral("Background"), [ed] { ed->surface()->background = Fill(); });
    });
    mk("pd.bgMore", QStringLiteral("More Backgrounds…"), "image", QKeySequence(), [this] {
        Fill f = m_ed->surface()->background;
        if (fillEffectsDialog(this, m_ed, f, QStringLiteral("Format Background")))
            m_ed->change(QStringLiteral("Background"), [&] { m_ed->surface()->background = f; });
    });
    mk("pd.bgAllPages", QStringLiteral("Apply Background to All Pages"), "", QKeySequence(), [ed] {
        const Fill f = ed->surface()->background;
        ed->change(QStringLiteral("Background"), [ed, f] { for (auto &p : ed->doc()->pages) p->background = f; });
    });
    mk("pd.bgImage", QStringLiteral("Apply Image as Background"), "image", QKeySequence(), [this] {
        Item *it = m_ed->single();
        auto *pic = dynamic_cast<PictureItem *>(it);
        if (!pic || pic->imageId.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Apply Image as Background"), QStringLiteral("Select a picture first."));
            return;
        }
        m_ed->change(QStringLiteral("Background"), [&] {
            Fill f;
            f.type = Fill::Picture;
            f.imageId = pic->imageId;
            m_ed->surface()->background = f;
        });
        m_ed->deleteSelection();
    });

    // ---------------- Master pages ----------------
    mk("view.master", QStringLiteral("Master Page"), "layout-panel-top", QKeySequence(Qt::CTRL | Qt::Key_M), [ed] {
        if (!ed->masterView().isEmpty()) { ed->setMasterView(QString()); return; }
        const Page *pg = ed->doc()->pages[ed->currentPage()].get();
        ed->setMasterView(pg->masterId.isEmpty() ? ed->doc()->masters.first()->id : pg->masterId);
    }, true);
    mk("view.normal", QStringLiteral("Normal"), "file", QKeySequence(), [ed] { ed->setMasterView(QString()); }, true);
    mk("mp.add", QStringLiteral("Add Master Page"), "file-plus", QKeySequence(), [ed] { ed->addMaster(false); });
    mk("mp.dup", QStringLiteral("Duplicate"), "copy", QKeySequence(), [ed] { ed->addMaster(true); });
    mk("mp.rename", QStringLiteral("Rename"), "pencil-line", QKeySequence(), [this] {
        MasterPage *m = m_ed->doc()->master(m_ed->masterView());
        if (!m) return;
        bool ok = false;
        const QString n = QInputDialog::getText(this, QStringLiteral("Rename Master Page"), QStringLiteral("Description:"), QLineEdit::Normal, m->name, &ok);
        if (ok && !n.trimmed().isEmpty()) m_ed->change(QStringLiteral("Rename Master Page"), [&] { m->name = n.trimmed(); });
    });
    mk("mp.delete", QStringLiteral("Delete"), "trash-2", QKeySequence(), [ed] { ed->deleteMaster(ed->masterView()); });
    mk("mp.twoPage", QStringLiteral("Two-Page Master"), "book-open", QKeySequence(), [ed] {
        MasterPage *m = ed->doc()->master(ed->masterView());
        if (m) ed->change(QStringLiteral("Two-Page Master"), [m] { m->twoPage = !m->twoPage; });
        ed->notifyLive();
    }, true);
    mk("mp.applyAll", QStringLiteral("Apply to All Pages"), "", QKeySequence(), [ed] {
        const QString id = ed->masterView().isEmpty() ? ed->doc()->masters.first()->id : ed->masterView();
        ed->change(QStringLiteral("Apply Master Page"), [ed, id] { for (auto &p : ed->doc()->pages) p->masterId = id; });
    });
    mk("mp.applyCurrent", QStringLiteral("Apply to Current Page"), "", QKeySequence(), [ed] {
        const QString id = ed->masterView().isEmpty() ? ed->doc()->masters.first()->id : ed->masterView();
        ed->applyMaster(ed->currentPage(), id);
    });
    mk("mp.none", QStringLiteral("None"), "", QKeySequence(), [ed] { ed->applyMaster(ed->currentPage(), QString()); });
    mk("mp.close", QStringLiteral("Close Master Page"), "x", QKeySequence(), [ed] { ed->setMasterView(QString()); });

    // ---------------- Mailings ----------------
    mk("mm.wizard", QStringLiteral("Step-by-Step Mail Merge Wizard"), "wand-sparkles", QKeySequence(), [this] { showTaskPane("mailmerge"); });
    mk("mm.typeNew", QStringLiteral("Type a New List…"), "user-plus", QKeySequence(), [this] { recipientsDialog(this, m_ed, true); });
    mk("mm.existing", QStringLiteral("Use an Existing List…"), "file-spreadsheet", QKeySequence(), [this] {
        const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Select Data Source"), QString(),
                                                       QStringLiteral("Data Sources (*.csv *.txt *.tsv *.xlsx *.vcf);;All Files (*)"));
        if (p.isEmpty()) return;
        MergeSource src;
        QString err;
        if (!loadMergeSource(p, &src, &err)) { QMessageBox::warning(this, QStringLiteral("Select Data Source"), err); return; }
        m_ed->change(QStringLiteral("Select Recipients"), [&] { m_ed->doc()->merge = src; });
        recipientsDialog(this, m_ed, false);
    });
    mk("mm.editList", QStringLiteral("Edit Recipient List"), "user-pen", QKeySequence(), [this] { recipientsDialog(this, m_ed, false); });
    mk("mm.addressBlock", QStringLiteral("Address Block"), "mail-open", QKeySequence(), [this] { mergeFieldDialog(this, m_ed, 0); });
    mk("mm.greeting", QStringLiteral("Greeting Line"), "hand", QKeySequence(), [this] { mergeFieldDialog(this, m_ed, 1); });
    mk("mm.pictureField", QStringLiteral("Picture Field"), "image", QKeySequence(), [this] {
        const QStringList fields = m_ed->doc()->merge.fields;
        if (fields.isEmpty()) { QMessageBox::information(this, QStringLiteral("Picture Field"), QStringLiteral("Select a recipient list first.")); return; }
        bool ok = false;
        const QString f = QInputDialog::getItem(this, QStringLiteral("Picture Field"), QStringLiteral("Field that holds picture file names:"), fields, 0, false, &ok);
        if (!ok) return;
        m_ed->change(QStringLiteral("Picture Field"), [&] { m_ed->doc()->merge.pictureField = f; });
        auto pic = std::make_shared<PictureItem>();
        pic->name = "merge:" + f;
        pic->rect = QRectF(72, 72, 144, 144);
        pic->imgRect = QRectF(0, 0, 144, 144);
        m_ed->addItem(pic);
    });
    mk("mm.preview", QStringLiteral("Preview Results"), "eye", QKeySequence(), [ed] {
        if (ed->mergeRecord() >= 0) ed->setMergeRecord(-1);
        else if (!ed->doc()->merge.isEmpty()) ed->setMergeRecord(ed->doc()->merge.includedRows().value(0, 0));
    }, true);
    auto stepRecord = [ed](int how) {
        const QVector<int> rows = ed->doc()->merge.includedRows();
        if (rows.isEmpty()) return;
        int i = std::max(0, int(rows.indexOf(ed->mergeRecord())));
        if (how == 0) i = 0;
        else if (how == 3) i = rows.size() - 1;
        else i = std::clamp(i + (how == 1 ? -1 : 1), 0, int(rows.size()) - 1);
        ed->setMergeRecord(rows[i]);
    };
    mk("mm.first", QStringLiteral("First Record"), "chevrons-left", QKeySequence(), [stepRecord] { stepRecord(0); });
    mk("mm.prev", QStringLiteral("Previous Record"), "chevron-left", QKeySequence(), [stepRecord] { stepRecord(1); });
    mk("mm.next", QStringLiteral("Next Record"), "chevron-right", QKeySequence(), [stepRecord] { stepRecord(2); });
    mk("mm.last", QStringLiteral("Last Record"), "chevrons-right", QKeySequence(), [stepRecord] { stepRecord(3); });
    mk("mm.findRecipient", QStringLiteral("Find Recipient"), "search", QKeySequence(), [this] {
        bool ok = false;
        const QString q = QInputDialog::getText(this, QStringLiteral("Find Recipient"), QStringLiteral("Find:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || q.isEmpty()) return;
        const MergeSource &m = m_ed->doc()->merge;
        for (int r : m.includedRows())
            for (const QString &v : m.rows[r])
                if (v.contains(q, Qt::CaseInsensitive)) { m_ed->setMergeRecord(r); return; }
        statusBar()->showMessage(QStringLiteral("No recipient matches \"%1\".").arg(q), 4000);
    });
    mk("mm.mergeNew", QStringLiteral("Merge to New Publication"), "files", QKeySequence(), [this] {
        if (m_ed->doc()->merge.isEmpty()) { QMessageBox::information(this, QStringLiteral("Merge"), QStringLiteral("Select a recipient list first.")); return; }
        if (!maybeSave()) return;
        newPublication(mergeToNewPublication(*m_ed->doc()));
    });
    mk("mm.mergePrint", QStringLiteral("Merge to Printer…"), "printer", QKeySequence(), [this] {
        if (m_ed->doc()->merge.isEmpty()) { QMessageBox::information(this, QStringLiteral("Merge"), QStringLiteral("Select a recipient list first.")); return; }
        showBackstage("print");
    });
    mk("mm.mergePdf", QStringLiteral("Merge to PDF…"), "file-text", QKeySequence(), [this] {
        if (m_ed->doc()->merge.isEmpty()) { QMessageBox::information(this, QStringLiteral("Merge"), QStringLiteral("Select a recipient list first.")); return; }
        exportPdf(QString(), true);
    });
    mk("mm.mergeEmail", QStringLiteral("Merge to Email…"), "mail", QKeySequence(), [this] {
        mergeToEmailFiles(this, m_ed);
    });
    mk("mm.exportList", QStringLiteral("Export Recipient List…"), "file-down", QKeySequence(), [this] {
        const QString p = askSavePath(this, QStringLiteral("Export Recipient List"), QString(), QStringLiteral("CSV (*.csv)"));
        if (p.isEmpty()) return;
        saveMergeCsv(m_ed->doc()->merge, p);
    });

    // ---------------- Review ----------------
    mk("rev.spelling", QStringLiteral("Spelling"), "spell-check", QKeySequence(Qt::Key_F7), [this] { spellingDialog(this, m_ed); });
    mk("rev.checkAsType", QStringLiteral("Check Spelling as You Type"), "spell-check-2", QKeySequence(), [ed] { ed->setView([](ViewOptions &v) { v.spelling = !v.spelling; }); }, true);
    mk("rev.thesaurus", QStringLiteral("Thesaurus"), "book-a", QKeySequence(Qt::SHIFT | Qt::Key_F7), [this] { thesaurusDialog(this, m_ed); });
    mk("rev.research", QStringLiteral("Research"), "book-open-text", QKeySequence(Qt::ALT | Qt::Key_F7), [this] { showTaskPane("research"); });
    mk("rev.translate", QStringLiteral("Translate"), "languages", QKeySequence(), [this] {
        // The selected text, else the story being edited, else the selected box's text.
        QString text;
        if (m_ed->isEditingText()) {
            const QTextCursor c = m_ed->cursor();
            text = c.hasSelection() ? c.selectedText() : (m_ed->editDoc() ? m_ed->editDoc()->toPlainText() : QString());
        } else if (Item *it = m_ed->single()) {
            QString sid;
            if (auto *t = dynamic_cast<TextItem *>(it)) sid = t->storyId;
            else if (auto *sh = dynamic_cast<ShapeItem *>(it)) sid = sh->storyId;
            if (QTextDocument *d = m_ed->doc()->storyDoc(sid)) text = d->toPlainText();
        }
        text.replace(QChar::ParagraphSeparator, '\n').replace(QChar::LineSeparator, '\n').remove(QChar(0x00AD)).remove(QChar::ObjectReplacementCharacter);
        text = text.trimmed();
        if (text.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Translate"), QStringLiteral("Select the text you want to translate, then choose Translate."));
            return;
        }
        const QList<QPair<QString, QString>> langs = {
            {"English", "en"}, {"Spanish", "es"}, {"French", "fr"}, {"German", "de"}, {"Italian", "it"}, {"Portuguese", "pt"},
            {"Dutch", "nl"}, {"Polish", "pl"}, {"Russian", "ru"}, {"Ukrainian", "uk"}, {"Arabic", "ar"}, {"Hindi", "hi"},
            {"Chinese (Simplified)", "zh-Hans"}, {"Japanese", "ja"}, {"Korean", "ko"}, {"Vietnamese", "vi"}, {"Turkish", "tr"}, {"Greek", "el"}};
        QStringList names;
        for (const auto &l : langs) names << l.first;
        const QString last = Settings::get().value(QStringLiteral("translate/target"), QStringLiteral("Spanish")).toString();
        bool ok = false;
        const QString choice = QInputDialog::getItem(this, QStringLiteral("Translate"),
                                                     QStringLiteral("Translate the selected text into:\n\n"
                                                                    "Your web browser will open LibreTranslate, a free, open-source translation service, "
                                                                    "with this text. Nothing is sent until you click OK."),
                                                     names, std::max(0, int(names.indexOf(last))), false, &ok);
        if (!ok) return;
        Settings::get().setValue(QStringLiteral("translate/target"), choice);
        QString code = QStringLiteral("en");
        for (const auto &l : langs) if (l.first == choice) code = l.second;
        if (text.size() > 5000) text = text.left(5000);   // the service's limit for one request
        QUrl url(QStringLiteral("https://libretranslate.com/"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("source"), QStringLiteral("auto"));
        q.addQueryItem(QStringLiteral("target"), code);
        q.addQueryItem(QStringLiteral("q"), text);
        url.setQuery(q);
        QDesktopServices::openUrl(url);
    });
    mk("rev.language", QStringLiteral("Set Proofing Language…"), "globe", QKeySequence(), [this] {
        bool ok = false;
        const QString lang = QInputDialog::getItem(this, QStringLiteral("Language"), QStringLiteral("Mark selected text as:"),
                                                   {"en-US", "en-GB", "es-ES", "fr-FR", "de-DE", "it-IT", "pt-BR", "nl-NL", "(no proofing)"}, 0, false, &ok);
        if (!ok) return;
        if (lang.startsWith('(')) m_ed->setCharProperty(tp::NoProof, true, QStringLiteral("Language"));
        else m_ed->setCharProperty(tp::Language, lang, QStringLiteral("Language"));
    });
    mk("rev.designChecker", QStringLiteral("Run Design Checker"), "shield-check", QKeySequence(), [this] { showTaskPane("designchecker"); });
    mk("rev.wordCount", QStringLiteral("Word Count"), "whole-word", QKeySequence(), [this] { wordCountDialog(this, m_ed); });
    mk("rev.hyphenation", QStringLiteral("Hyphenation…"), "hyphenation", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H), [this] { hyphenationDialog(this, m_ed); });

    // ---------------- View ----------------
    mk("view.single", QStringLiteral("Single Page"), "file", QKeySequence(), [ed] { ed->setTwoPageSpread(false); }, true);
    mk("view.spread", QStringLiteral("Two-Page Spread"), "book-open", QKeySequence(), [ed] { ed->setTwoPageSpread(true); }, true);
    auto toggle = [ed](bool ViewOptions::*m) { ed->setView([m](ViewOptions &v) { v.*m = !(v.*m); }); };
    mk("view.boundaries", QStringLiteral("Boundaries"), "square-dashed", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), [toggle] { toggle(&ViewOptions::boundaries); }, true);
    mk("view.guides", QStringLiteral("Guides"), "layout-grid", QKeySequence(), [toggle] { toggle(&ViewOptions::guides); }, true);
    mk("view.fields", QStringLiteral("Fields"), "braces", QKeySequence(), [toggle] { toggle(&ViewOptions::fields); }, true);
    mk("view.rulers", QStringLiteral("Rulers"), "ruler", QKeySequence(), [this, toggle] {
        toggle(&ViewOptions::rulers);
        m_canvas->setRulersVisible(m_ed->view.rulers);
    }, true);
    mk("view.pageNav", QStringLiteral("Page Navigation"), "panel-left", QKeySequence(), [this, toggle] {
        toggle(&ViewOptions::pageNav);
        m_pages->setVisible(m_ed->view.pageNav);
    }, true);
    mk("view.scratch", QStringLiteral("Scratch Area"), "layout-dashboard", QKeySequence(), [toggle] { toggle(&ViewOptions::scratch); }, true);
    mk("view.baselines", QStringLiteral("Baselines"), "align-vertical-space-between", QKeySequence(Qt::CTRL | Qt::Key_F7), [toggle] { toggle(&ViewOptions::baselines); }, true);
    mk("view.gridlines", QStringLiteral("View Gridlines"), "grid-2x2", QKeySequence(), [toggle] { toggle(&ViewOptions::gridlines); }, true);
    mk("view.graphics", QStringLiteral("Graphics Manager"), "images", QKeySequence(), [this] {
        if (currentTaskPane() == "graphics") hideTaskPane(); else showTaskPane("graphics");
    }, true);
    mk("view.measurement", QStringLiteral("Measurement"), "ruler-dimension-line", QKeySequence(), [this] { measurementWindow(this, m_ed); });
    mk("zoom.100", QStringLiteral("100%"), "scan", QKeySequence(Qt::Key_F9), [this] {
        if (std::abs(m_canvas->zoom() - 1.0) < 0.01) m_canvas->zoomToFit(Canvas::Fit::WholePage);
        else { m_canvas->zoomToFit(Canvas::Fit::None); m_canvas->setZoom(1.0); }
    });
    mk("zoom.page", QStringLiteral("Whole Page"), "maximize", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L), [this] { m_canvas->zoomToFit(Canvas::Fit::WholePage); });
    mk("zoom.width", QStringLiteral("Page Width"), "move-horizontal", QKeySequence(), [this] { m_canvas->zoomToFit(Canvas::Fit::PageWidth); });
    mk("zoom.selection", QStringLiteral("Selected Objects"), "scan-search", QKeySequence(), [this] { m_canvas->zoomToFit(Canvas::Fit::Selection); });
    mk("zoom.in", QStringLiteral("Zoom In"), "zoom-in", QKeySequence::ZoomIn, [this] { m_canvas->zoomToFit(Canvas::Fit::None); m_canvas->setZoom(m_canvas->zoom() * 1.2); });
    mk("zoom.out", QStringLiteral("Zoom Out"), "zoom-out", QKeySequence::ZoomOut, [this] { m_canvas->zoomToFit(Canvas::Fit::None); m_canvas->setZoom(m_canvas->zoom() / 1.2); });
    mk("win.new", QStringLiteral("New Window"), "app-window", QKeySequence(), [this] {
        auto *w = new MainWindow();
        w->setAttribute(Qt::WA_DeleteOnClose);
        if (!m_ed->filePath().isEmpty()) w->openFile(m_ed->filePath());
        w->show();
    });
    mk("win.arrange", QStringLiteral("Arrange All"), "layout-panel-left", QKeySequence(), [] {
        QList<QWidget *> wins;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<MainWindow *>(w) && w->isVisible()) wins << w;
        if (wins.isEmpty()) return;
        const QRect area = wins.first()->screen()->availableGeometry();
        const int wdt = area.width() / wins.size();
        for (int i = 0; i < wins.size(); ++i) wins[i]->setGeometry(area.left() + i * wdt, area.top(), wdt, area.height());
    });
    mk("win.cascade", QStringLiteral("Cascade"), "layers-2", QKeySequence(), [] {
        int k = 0;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<MainWindow *>(w) && w->isVisible()) { w->move(40 + k * 30, 40 + k * 30); w->raise(); ++k; }
    });

    // ---------------- Shape / drawing tools ----------------
    mk("shape.addText", QStringLiteral("Add Text"), "text-cursor", QKeySequence(), [ed] { if (Item *it = ed->single()) ed->beginTextEdit(it->id); });
    mk("shape.editPoints", QStringLiteral("Edit Points"), "spline", QKeySequence(), [this] {
        auto *s = dynamic_cast<ShapeItem *>(m_ed->single());
        if (!s) return;
        if (m_ed->pointsItem == s->id) { m_ed->setPointsItem(QString()); return; }
        // Convert to a freeform path so its points can be dragged; Ctrl+click
        // a point deletes it, Ctrl+click an edge adds one, Esc finishes.
        if (s->customPath.isEmpty())
            m_ed->change(QStringLiteral("Edit Points"), [&] { s->customPath = shapePath(s->shape, s->rect.size(), s->adj); });
        m_ed->setPointsItem(s->id);
        statusBar()->showMessage(QStringLiteral("Drag a point to reshape. Ctrl+click a point to delete it, or an edge to add one. Press Esc when done."), 8000);
    });
    mk("line.draw", QStringLiteral("Line"), "minus", QKeySequence(), [ed] { ed->setTool(Tool::Line); });
    mk("line.arrow", QStringLiteral("Arrow"), "move-right", QKeySequence(), [ed] { ed->setTool(Tool::Arrow); });
    mk("line.double", QStringLiteral("Double Arrow"), "move-horizontal", QKeySequence(), [ed] { ed->setTool(Tool::DoubleArrow); });
    for (double w : {0.25, 0.5, 0.75, 1.0, 1.5, 2.25, 3.0, 4.5, 6.0}) {
        mk(QStringLiteral("weight.%1").arg(w), QStringLiteral("%1 pt").arg(w), "", QKeySequence(), [ed, w] {
            ed->forEachSelected(QStringLiteral("Line Weight"), [w](Item *it) {
                if (it->stroke.isNone()) it->stroke = Stroke::line(ColorRef::scheme(Main), w);
                it->stroke.width = w;
            });
        });
    }
    for (int d = 0; d < 8; ++d) {
        mk(QStringLiteral("dash.%1").arg(d), dashName(Stroke::Dash(d)), "", QKeySequence(), [ed, d] {
            ed->forEachSelected(QStringLiteral("Dashes"), [d](Item *it) { it->stroke.dash = Stroke::Dash(d); });
        });
    }
    const char *arrowNames[] = {"No Arrow", "Arrow", "Open Arrow", "Stealth Arrow", "Diamond Arrow", "Oval Arrow"};
    for (int a = 0; a < 6; ++a) {
        mk(QStringLiteral("arrowEnd.%1").arg(a), QStringLiteral("End: %1").arg(QString::fromLatin1(arrowNames[a])), "", QKeySequence(), [ed, a] {
            ed->forEachSelected(QStringLiteral("Arrows"), [a](Item *it) { it->stroke.endArrow = Arrow(a); });
        });
        mk(QStringLiteral("arrowStart.%1").arg(a), QStringLiteral("Start: %1").arg(QString::fromLatin1(arrowNames[a])), "", QKeySequence(), [ed, a] {
            ed->forEachSelected(QStringLiteral("Arrows"), [a](Item *it) { it->stroke.startArrow = Arrow(a); });
        });
    }
    mk("fill.effects", QStringLiteral("Fill Effects…"), "paint-bucket", QKeySequence(), [this] {
        Item *it = m_ed->single();
        if (!it) return;
        Fill f = it->fill;
        if (fillEffectsDialog(this, m_ed, f)) m_ed->forEachSelected(QStringLiteral("Fill"), [f](Item *x) { x->fill = f; });
    });
    mk("fill.picture", QStringLiteral("Picture Fill…"), "image", QKeySequence(), [this] {
        const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Picture Fill"), QString(), QStringLiteral("Pictures (*.png *.jpg *.jpeg *.gif *.bmp *.tif *.webp *.svg)"));
        if (p.isEmpty()) return;
        QFile file(p);
        if (!file.open(QIODevice::ReadOnly)) return;
        const QString id = m_ed->doc()->addImage(file.readAll(), QFileInfo(p).suffix().toLower(), p);
        m_ed->forEachSelected(QStringLiteral("Picture Fill"), [id](Item *x) { x->fill.type = Fill::Picture; x->fill.imageId = id; x->fill.tile = false; });
    });
    mk("fill.none", QStringLiteral("No Fill"), "ban", QKeySequence(), [ed] { ed->forEachSelected(QStringLiteral("Fill"), [](Item *x) { x->fill = Fill(); }); });
    mk("line.none", QStringLiteral("No Outline"), "ban", QKeySequence(), [ed] { ed->forEachSelected(QStringLiteral("Outline"), [](Item *x) { x->stroke.color = ColorRef::none(); }); });
    mk("line.more", QStringLiteral("More Lines…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 0); });
    // Effects
    auto shadowPreset = [ed](int k) {
        ed->forEachSelected(QStringLiteral("Shadow"), [k](Item *it) {
            it->fx.shadow.on = k > 0;
            it->fx.shadow.blur = k == 1 ? 4 : k == 2 ? 0 : 8;
            it->fx.shadow.distance = k == 3 ? 0 : 3;
            it->fx.shadow.angle = k == 4 ? 135 : 45;
            it->fx.shadow.transparency = k == 3 ? 0.5 : 0.6;
        });
    };
    const char *shadowNames[] = {"No Shadow", "Outer Shadow", "Hard Shadow", "Soft Glow Shadow", "Shadow Down-Left"};
    for (int k = 0; k < 5; ++k) mk(QStringLiteral("shadow.%1").arg(k), QString::fromLatin1(shadowNames[k]), "", QKeySequence(), [shadowPreset, k] { shadowPreset(k); });
    mk("shadow.options", QStringLiteral("Shadow Options…"), "", QKeySequence(), [this] { shadowDialog(this, m_ed); });
    for (int g : {0, 5, 8, 11, 18}) {
        mk(QStringLiteral("glow.%1").arg(g), g ? QStringLiteral("%1 pt Glow").arg(g) : QStringLiteral("No Glow"), "", QKeySequence(), [ed, g] {
            ed->forEachSelected(QStringLiteral("Glow"), [g](Item *it) { it->fx.glow.on = g > 0; it->fx.glow.size = g; });
        });
    }
    for (int s : {0, 2, 5, 10, 25}) {
        mk(QStringLiteral("soft.%1").arg(s), s ? QStringLiteral("%1 pt").arg(s) : QStringLiteral("No Soft Edges"), "", QKeySequence(), [ed, s] {
            ed->forEachSelected(QStringLiteral("Soft Edges"), [s](Item *it) { it->fx.softEdge = s; });
        });
    }
    const char *reflNames[] = {"No Reflection", "Tight Reflection", "Half Reflection", "Full Reflection"};
    for (int r = 0; r < 4; ++r) {
        mk(QStringLiteral("refl.%1").arg(r), QString::fromLatin1(reflNames[r]), "", QKeySequence(), [ed, r] {
            ed->forEachSelected(QStringLiteral("Reflection"), [r](Item *it) {
                it->fx.reflection.on = r > 0;
                it->fx.reflection.size = r == 1 ? 0.3 : r == 2 ? 0.5 : 1.0;
                it->fx.reflection.distance = 2;
            });
        });
    }
    const char *bevelNames[] = {"No Bevel", "Circle", "Relaxed Inset", "Cool Slant", "Angle", "Soft Round", "Convex"};
    for (int b = 0; b < 7; ++b) {
        mk(QStringLiteral("bevel.%1").arg(b), QString::fromLatin1(bevelNames[b]), "", QKeySequence(), [ed, b] {
            ed->forEachSelected(QStringLiteral("Bevel"), [b](Item *it) { it->fx.bevel.type = b; it->fx.bevel.width = 4 + b; });
        });
    }
    const QPair<const char *, QPointF> rot3d[] = {{"No Rotation", {0, 0}}, {"Perspective Left", {0, 30}}, {"Perspective Right", {0, -30}},
                                                  {"Perspective Above", {30, 0}}, {"Perspective Below", {-30, 0}}, {"Off Axis", {15, 25}}};
    for (const auto &r : rot3d) {
        const QPointF a = r.second;
        mk(QStringLiteral("rot3d.%1").arg(QString::fromLatin1(r.first)), QString::fromLatin1(r.first), "", QKeySequence(), [ed, a] {
            ed->forEachSelected(QStringLiteral("3-D Rotation"), [a](Item *it) { it->fx.rot3d.x = a.x(); it->fx.rot3d.y = a.y(); it->fx.rot3d.perspective = 30; });
        });
    }

    // ---------------- Text box tools ----------------
    auto setAutofit = [ed](TextItem::Autofit f) {
        Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
        auto *t = dynamic_cast<TextItem *>(it);
        if (!t) return;
        ed->change(QStringLiteral("Text Fit"), [ed, t, f] {
            t->autofit = f;
            if (f == TextItem::GrowBox) ed->autoGrowText(t);
        });
    };
    mk("fit.best", QStringLiteral("Best Fit"), "", QKeySequence(), [setAutofit] { setAutofit(TextItem::BestFit); }, true);
    mk("fit.shrink", QStringLiteral("Shrink Text On Overflow"), "", QKeySequence(), [setAutofit] { setAutofit(TextItem::ShrinkOnOverflow); }, true);
    mk("fit.grow", QStringLiteral("Grow Text Box to Fit"), "", QKeySequence(), [setAutofit] { setAutofit(TextItem::GrowBox); }, true);
    mk("fit.none", QStringLiteral("Do Not Autofit"), "", QKeySequence(), [setAutofit] { setAutofit(TextItem::NoAutofit); }, true);
    mk("tb.textFit", QStringLiteral("Text Fit"), "fold-vertical", QKeySequence(), [] {});
    mk("tb.direction", QStringLiteral("Text Direction"), "arrow-down-wide-narrow", QKeySequence(), [ed] {
        Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
        if (auto *t = dynamic_cast<TextItem *>(it)) ed->change(QStringLiteral("Text Direction"), [t] { t->vertical = !t->vertical; });
    }, true);
    const QPair<const char *, int> valigns[] = {{"Align Top", 0}, {"Align Middle", 1}, {"Align Bottom", 2}};
    for (const auto &v : valigns) {
        const int k = v.second;
        mk(QStringLiteral("valign.%1").arg(k), QString::fromLatin1(v.first), k == 0 ? "align-vertical-justify-start" : k == 1 ? "align-vertical-justify-center" : "align-vertical-justify-end",
           QKeySequence(), [ed, k] {
               Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
               if (auto *t = dynamic_cast<TextItem *>(it)) ed->change(QStringLiteral("Vertical Alignment"), [t, k] { t->valign = VAlign(k); });
               else if (auto *s = dynamic_cast<ShapeItem *>(it)) ed->change(QStringLiteral("Vertical Alignment"), [s, k] { s->valign = VAlign(k); });
               else if (auto *tb = dynamic_cast<TableItem *>(it); tb && ed->isEditingText()) {
                   const auto &tt = ed->textTarget();
                   ed->change(QStringLiteral("Cell Alignment"), [tb, tt, k] { tb->cell(tt.row, tt.col).valign = VAlign(k); });
               }
           }, true);
    }
    for (int c = 1; c <= 4; ++c) {
        mk(QStringLiteral("cols.%1").arg(c), c == 1 ? QStringLiteral("One Column") : QStringLiteral("%1 Columns").arg(c), c == 1 ? "square" : c == 2 ? "columns-2" : "columns-3",
           QKeySequence(), [ed, c] {
               Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
               if (auto *t = dynamic_cast<TextItem *>(it)) ed->change(QStringLiteral("Columns"), [t, c] { t->columns = c; });
           });
    }
    mk("cols.more", QStringLiteral("More Columns…"), "", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 3); });
    const QPair<const char *, double> tbMargins[] = {{"None", 0}, {"Narrow", 2.88}, {"Moderate", 7.2}, {"Wide", 14.4}};
    for (const auto &m : tbMargins) {
        const double v = m.second;
        mk(QStringLiteral("tbmargin.%1").arg(QString::fromLatin1(m.first)), QString::fromLatin1(m.first), "", QKeySequence(), [ed, v] {
            Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
            if (auto *t = dynamic_cast<TextItem *>(it)) ed->change(QStringLiteral("Margins"), [t, v] { t->insets = QMarginsF(v, v, v, v); });
            else if (auto *s = dynamic_cast<ShapeItem *>(it)) ed->change(QStringLiteral("Margins"), [s, v] { s->insets = QMarginsF(v, v, v, v); });
        });
    }
    mk("tb.link", QStringLiteral("Create Link"), "link-2", QKeySequence(), [ed] {
        Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
        if (!dynamic_cast<TextItem *>(it)) return;
        ed->endTextEdit();
        ed->linkSource = it->id;
        ed->setTool(Tool::Link);
        ed->linkSource = it->id;
    });
    mk("tb.break", QStringLiteral("Break"), "unlink-2", QKeySequence(), [ed] {
        Item *it = ed->isEditingText() ? ed->doc()->item(ed->textTarget().itemId) : ed->single();
        if (it) ed->breakLink(it->id);
    });
    mk("tb.prev", QStringLiteral("Previous"), "arrow-left", QKeySequence(), [ed] {
        Item *it = ed->single();
        if (!it) return;
        if (TextItem *p = ed->doc()->prevFrame(it->id)) {
            const auto loc = ed->doc()->find(p->id);
            if (loc.page >= 0) ed->setCurrentPage(loc.page);
            ed->select(p->id);
        }
    });
    mk("tb.next", QStringLiteral("Next"), "arrow-right", QKeySequence(), [ed] {
        auto *t = dynamic_cast<TextItem *>(ed->single());
        if (!t || t->nextId.isEmpty()) return;
        const auto loc = ed->doc()->find(t->nextId);
        if (loc.page >= 0) ed->setCurrentPage(loc.page);
        ed->select(t->nextId);
    });
    mk("tb.contOn", QStringLiteral("Include \"Continued on page…\""), "", QKeySequence(), [ed] {
        if (auto *t = dynamic_cast<TextItem *>(ed->single())) ed->change(QStringLiteral("Continued Notice"), [t] { t->continuedOn = !t->continuedOn; });
    }, true);
    mk("tb.contFrom", QStringLiteral("Include \"Continued from page…\""), "", QKeySequence(), [ed] {
        if (auto *t = dynamic_cast<TextItem *>(ed->single())) ed->change(QStringLiteral("Continued Notice"), [t] { t->continuedFrom = !t->continuedFrom; });
    }, true);
    for (int lines : {0, 2, 3, 4, 5}) {
        mk(QStringLiteral("dropcap.%1").arg(lines), lines ? QStringLiteral("Drop Cap %1 Lines").arg(lines) : QStringLiteral("No Drop Cap"), "", QKeySequence(),
           [ed, lines] { ed->setDropCap(lines); });
    }
    mk("dropcap.custom", QStringLiteral("Custom Drop Cap…"), "", QKeySequence(), [this] { dropCapDialog(this, m_ed); });
    auto boolProp = [ed](int prop, const QString &label) {
        const bool on = !ed->currentCharFormat().boolProperty(prop);
        ed->setCharProperty(prop, on, label);
    };
    mk("tb.shadow", QStringLiteral("Shadow"), "", QKeySequence(), [boolProp] { boolProp(tp::Shadow, QStringLiteral("Text Shadow")); }, true);
    mk("tb.outline", QStringLiteral("Outline"), "", QKeySequence(), [ed] {
        if (!ed->currentCharFormat().stringProperty(tp::OutlineRef).isEmpty()) ed->clearCharProperty(tp::OutlineRef, QStringLiteral("Text Outline"));
        else ed->setCharProperty(tp::OutlineRef, ColorRef::scheme(Main).toString(), QStringLiteral("Text Outline"));
    }, true);
    mk("tb.emboss", QStringLiteral("Emboss"), "", QKeySequence(), [boolProp] { boolProp(tp::Emboss, QStringLiteral("Emboss")); }, true);
    mk("tb.engrave", QStringLiteral("Engrave"), "", QKeySequence(), [boolProp] { boolProp(tp::Engrave, QStringLiteral("Engrave")); }, true);
    mk("tb.trueSmallCaps", QStringLiteral("True Small Caps"), "", QKeySequence(), [boolProp] { boolProp(tp::TrueSmallCaps, QStringLiteral("Small Caps")); }, true);
    mk("tb.swash", QStringLiteral("Swash"), "", QKeySequence(), [boolProp] { boolProp(tp::Swash, QStringLiteral("Swash")); }, true);
    mk("tb.alternates", QStringLiteral("Stylistic Alternates"), "", QKeySequence(), [boolProp] { boolProp(tp::Alternates, QStringLiteral("Stylistic Alternates")); }, true);
    const char *numStyles[] = {"Default", "Lining", "Old-style"};
    for (int i = 0; i < 3; ++i) mk(QStringLiteral("numstyle.%1").arg(i), QString::fromLatin1(numStyles[i]), "", QKeySequence(), [ed, i] { ed->setCharProperty(tp::NumberStyle, i, QStringLiteral("Number Style")); });
    const char *numSpacing[] = {"Default Spacing", "Proportional", "Tabular"};
    for (int i = 0; i < 3; ++i) mk(QStringLiteral("numspacing.%1").arg(i), QString::fromLatin1(numSpacing[i]), "", QKeySequence(), [ed, i] { ed->setCharProperty(tp::NumberSpacing, i, QStringLiteral("Number Spacing")); });
    const char *ligNames[] = {"Standard Ligatures", "No Ligatures", "All Ligatures"};
    for (int i = 0; i < 3; ++i) mk(QStringLiteral("lig.%1").arg(i), QString::fromLatin1(ligNames[i]), "", QKeySequence(), [ed, i] { ed->setCharProperty(tp::Ligatures, i, QStringLiteral("Ligatures")); });
    for (int i = 0; i <= 20; ++i) mk(QStringLiteral("ss.%1").arg(i), i ? QStringLiteral("Stylistic Set %1").arg(i) : QStringLiteral("Default Set"), "", QKeySequence(), [ed, i] { ed->setCharProperty(tp::StylisticSet, i, QStringLiteral("Stylistic Set")); });

    // ---------------- Pictures ----------------
    mk("pic.change", QStringLiteral("Change Picture"), "image-up", QKeySequence(), [this] {
        if (Item *it = m_ed->single(); it && it->type() == ItemType::Picture) insertPictureFromFile(it->id);
    });
    mk("pic.remove", QStringLiteral("Remove Picture"), "image-off", QKeySequence(), [ed] {
        if (auto *p = dynamic_cast<PictureItem *>(ed->single())) ed->change(QStringLiteral("Remove Picture"), [p] { p->imageId.clear(); });
    });
    mk("pic.swap", QStringLiteral("Swap"), "arrow-left-right", QKeySequence(), [ed] {
        const auto sel = ed->selectedItems();
        if (sel.size() != 2) { Q_EMIT ed->status(QStringLiteral("Select two pictures to swap.")); return; }
        auto *a = dynamic_cast<PictureItem *>(sel[0]);
        auto *b = dynamic_cast<PictureItem *>(sel[1]);
        if (!a || !b) return;
        ed->change(QStringLiteral("Swap Pictures"), [&] {
            std::swap(a->imageId, b->imageId);
            a->fitImage(ed->doc()->imageSize(a->imageId), true);
            b->fitImage(ed->doc()->imageSize(b->imageId), true);
        });
    });
    for (int b : {-40, -20, 0, 20, 40}) {
        for (int c : {-40, -20, 0, 20, 40}) {
            mk(QStringLiteral("corr.%1.%2").arg(b).arg(c), QStringLiteral("Brightness %1%, Contrast %2%").arg(b > 0 ? "+" + QString::number(b) : QString::number(b)).arg(c > 0 ? "+" + QString::number(c) : QString::number(c)),
               "", QKeySequence(), [ed, b, c] {
                   ed->forEachSelected(QStringLiteral("Corrections"), [b, c](Item *it) {
                       if (auto *p = dynamic_cast<PictureItem *>(it)) { p->brightness = b; p->contrast = c; }
                   });
               });
        }
    }
    const char *recolorNames[] = {"No Recolor", "Grayscale", "Sepia", "Washout", "Black and White"};
    for (int r = 0; r < 5; ++r) {
        mk(QStringLiteral("recolor.%1").arg(r), QString::fromLatin1(recolorNames[r]), "", QKeySequence(), [ed, r] {
            ed->forEachSelected(QStringLiteral("Recolor"), [r](Item *it) { if (auto *p = dynamic_cast<PictureItem *>(it)) p->recolor = PictureItem::Recolor(r); });
        });
    }
    for (int s = 1; s <= 5; ++s) {
        const ColorRef c = ColorRef::scheme(s);
        mk(QStringLiteral("recolor.slot%1").arg(s), QStringLiteral("%1 Tint").arg(slotName(s)), "", QKeySequence(), [ed, c] {
            ed->forEachSelected(QStringLiteral("Recolor"), [c](Item *it) {
                if (auto *p = dynamic_cast<PictureItem *>(it)) { p->recolor = PictureItem::ColorTint; p->recolorColor = c; }
            });
        });
    }
    mk("pic.transparent", QStringLiteral("Set Transparent Color"), "pipette", QKeySequence(), [this] {
        auto *p = dynamic_cast<PictureItem *>(m_ed->single());
        if (!p) return;
        const QImage img = m_ed->doc()->image(p->imageId);
        if (img.isNull()) return;
        // Use the corner pixel, which is the background in most clip art and logos.
        const QColor c = img.pixelColor(0, 0);
        m_ed->change(QStringLiteral("Set Transparent Color"), [p, c] { p->hasTransparentColor = true; p->transparentColor = c; });
    });
    mk("pic.compress", QStringLiteral("Compress Pictures…"), "minimize-2", QKeySequence(), [this] {
        compressPicturesDialog(this, m_ed);
    });
    mk("pic.reset", QStringLiteral("Reset Picture"), "rotate-ccw", QKeySequence(), [ed] {
        ed->forEachSelected(QStringLiteral("Reset Picture"), [ed](Item *it) {
            if (auto *p = dynamic_cast<PictureItem *>(it)) {
                p->brightness = p->contrast = 0;
                p->recolor = PictureItem::NoRecolor;
                p->hasTransparentColor = false;
                p->transparency = 0;
                p->fx = Effects();
                p->fitImage(ed->doc()->imageSize(p->imageId), true);
            }
        });
    });
    mk("pic.crop", QStringLiteral("Crop"), "crop", QKeySequence(), [ed] {
        Item *it = ed->single();
        if (!it || it->type() != ItemType::Picture) return;
        ed->setCropItem(ed->cropItem == it->id ? QString() : it->id);
    }, true);
    mk("pic.fit", QStringLiteral("Fit"), "shrink", QKeySequence(), [ed] {
        ed->forEachSelected(QStringLiteral("Fit"), [ed](Item *it) { if (auto *p = dynamic_cast<PictureItem *>(it)) p->fitImage(ed->doc()->imageSize(p->imageId), false); });
    });
    mk("pic.fill", QStringLiteral("Fill"), "expand", QKeySequence(), [ed] {
        ed->forEachSelected(QStringLiteral("Fill"), [ed](Item *it) { if (auto *p = dynamic_cast<PictureItem *>(it)) p->fitImage(ed->doc()->imageSize(p->imageId), true); });
    });
    mk("pic.clearCrop", QStringLiteral("Clear Crop"), "eraser", QKeySequence(), [ed] {
        ed->forEachSelected(QStringLiteral("Clear Crop"), [ed](Item *it) {
            auto *p = dynamic_cast<PictureItem *>(it);
            if (!p) return;
            // Grow the frame to show the whole picture.
            const QPointF tl = p->transform().map(p->imgRect.topLeft());
            const QPointF br = p->transform().map(p->imgRect.bottomRight());
            const QPointF c = (tl + br) / 2;
            p->rect = QRectF(c.x() - p->imgRect.width() / 2, c.y() - p->imgRect.height() / 2, p->imgRect.width(), p->imgRect.height());
            p->imgRect = QRectF(QPointF(0, 0), p->rect.size());
        });
    });
    mk("pic.saveAs", QStringLiteral("Save as Picture…"), "image-down", QKeySequence(), [this] {
        Item *it = m_ed->single();
        if (!it) return;
        const QString p = askSavePath(this, QStringLiteral("Save as Picture"), QString(), QStringLiteral("PNG (*.png);;JPEG (*.jpg)"));
        if (p.isEmpty()) return;
        PaintContext ctx;
        ctx.doc = m_ed->doc();
        ctx.cache = &m_ed->cache();
        ctx.opt.output = true;
        Renderer::renderItemsToImage(ctx, {m_ed->doc()->itemPtr(it->id)}, 300.0 / 72).save(p);
    });
    mk("pic.caption", QStringLiteral("Caption"), "captions", QKeySequence(), [ed] {
        auto *p = dynamic_cast<PictureItem *>(ed->single());
        if (!p) return;
        ed->beginChange(QStringLiteral("Caption"));
        auto t = std::static_pointer_cast<TextItem>(ed->newTextBox(QRectF(p->rect.left(), p->rect.bottom() + 4, p->rect.width(), 22), QStringLiteral("Caption describing picture or graphic.")));
        ed->doc()->storyDoc(t->storyId);
        QTextCursor c(ed->doc()->storyDoc(t->storyId));
        c.select(QTextCursor::Document);
        if (const TextStyle *s = ed->doc()->style("Caption")) { c.mergeCharFormat(s->chr); c.mergeBlockFormat(s->blk); }
        ed->surfaceItems().push_back(t);
        p->caption = t->id;
        ed->endChange();
        ed->select(t->id);
    });
    mk("pic.shapeRect", QStringLiteral("Rectangle"), "", QKeySequence(), [ed] { ed->forEachSelected(QStringLiteral("Picture Shape"), [](Item *it) { if (auto *p = dynamic_cast<PictureItem *>(it)) p->maskShape = "rect"; }); });
    mk("obj.transparency", QStringLiteral("Transparency…"), "blend", QKeySequence(), [this] { formatObjectDialog(this, m_ed, 4); });

    // ---------------- Tables ----------------
    auto tableEdit = [this](const QString &label, const std::function<void(TableItem *, int, int)> &fn) {
        TableItem *t = selTable(m_ed);
        if (!t) return;
        const int r = m_ed->isEditingText() ? m_ed->textTarget().row : 0;
        const int c = m_ed->isEditingText() ? m_ed->textTarget().col : 0;
        m_ed->endTextEdit();
        m_ed->change(label, [&] { fn(t, r, c); });
        m_ed->select(t->id);
    };
    mk("tbl.insAbove", QStringLiteral("Insert Above"), "between-horizontal-end", QKeySequence(), [this, tableEdit] { tableEdit(QStringLiteral("Insert Row"), [this](TableItem *t, int r, int) { tableInsertRow(m_ed, t, r); }); });
    mk("tbl.insBelow", QStringLiteral("Insert Below"), "between-horizontal-start", QKeySequence(), [this, tableEdit] { tableEdit(QStringLiteral("Insert Row"), [this](TableItem *t, int r, int) { tableInsertRow(m_ed, t, r + 1); }); });
    mk("tbl.insLeft", QStringLiteral("Insert Left"), "between-vertical-end", QKeySequence(), [this, tableEdit] { tableEdit(QStringLiteral("Insert Column"), [this](TableItem *t, int, int c) { tableInsertCol(m_ed, t, c); }); });
    mk("tbl.insRight", QStringLiteral("Insert Right"), "between-vertical-start", QKeySequence(), [this, tableEdit] { tableEdit(QStringLiteral("Insert Column"), [this](TableItem *t, int, int c) { tableInsertCol(m_ed, t, c + 1); }); });
    mk("tbl.delRow", QStringLiteral("Delete Rows"), "", QKeySequence(), [tableEdit] { tableEdit(QStringLiteral("Delete Row"), [](TableItem *t, int r, int) { tableDeleteRow(t, r); }); });
    mk("tbl.delCol", QStringLiteral("Delete Columns"), "", QKeySequence(), [tableEdit] { tableEdit(QStringLiteral("Delete Column"), [](TableItem *t, int, int c) { tableDeleteCol(t, c); }); });
    mk("tbl.delTable", QStringLiteral("Delete Table"), "", QKeySequence(), [this] {
        if (TableItem *t = selTable(m_ed)) { m_ed->endTextEdit(); m_ed->deleteItems({t->id}); }
    });
    mk("tbl.merge", QStringLiteral("Merge Cells"), "table-cells-merge", QKeySequence(), [this] {
        TableItem *t = selTable(m_ed);
        if (!t || !m_ed->isEditingText()) { Q_EMIT m_ed->status(QStringLiteral("Click in a cell, then choose Merge Cells to merge it with the cell to its right.")); return; }
        const int r = m_ed->textTarget().row, c = m_ed->textTarget().col;
        TableCell &cell = t->cell(r, c);
        const int next = c + cell.colSpan;
        if (next >= t->cols) return;
        m_ed->endTextEdit();
        m_ed->change(QStringLiteral("Merge Cells"), [&] {
            TableCell &right = t->cell(r, next);
            // Move the right cell's text into this one.
            QTextDocument *dst = m_ed->doc()->storyDoc(cell.storyId), *src = m_ed->doc()->storyDoc(right.storyId);
            if (dst && src && !src->toPlainText().isEmpty()) {
                QTextCursor dc(dst);
                dc.movePosition(QTextCursor::End);
                dc.insertBlock();
                dc.insertFragment(QTextDocumentFragment(src));
            }
            cell.colSpan += right.colSpan;
            for (int k = next; k < c + cell.colSpan; ++k) t->cell(r, k).covered = true;
        });
        m_ed->select(t->id);
    });
    mk("tbl.split", QStringLiteral("Split Cells"), "table-cells-split", QKeySequence(), [this] {
        TableItem *t = selTable(m_ed);
        if (!t || !m_ed->isEditingText()) return;
        const int r = m_ed->textTarget().row, c = m_ed->textTarget().col;
        m_ed->endTextEdit();
        m_ed->change(QStringLiteral("Split Cells"), [&] {
            TableCell &cell = t->cell(r, c);
            for (int rr = r; rr < r + cell.rowSpan; ++rr)
                for (int cc = c; cc < c + cell.colSpan; ++cc)
                    if (rr != r || cc != c) t->cell(rr, cc).covered = false;
            cell.rowSpan = cell.colSpan = 1;
        });
        m_ed->select(t->id);
    });
    mk("tbl.diagDown", QStringLiteral("Divide Down"), "", QKeySequence(), [this] {
        TableItem *t = selTable(m_ed);
        if (!t || !m_ed->isEditingText()) return;
        const auto tt = m_ed->textTarget();
        m_ed->change(QStringLiteral("Diagonals"), [&] { t->cell(tt.row, tt.col).diagonal = 1; });
    });
    mk("tbl.diagUp", QStringLiteral("Divide Up"), "", QKeySequence(), [this] {
        TableItem *t = selTable(m_ed);
        if (!t || !m_ed->isEditingText()) return;
        const auto tt = m_ed->textTarget();
        m_ed->change(QStringLiteral("Diagonals"), [&] { t->cell(tt.row, tt.col).diagonal = 2; });
    });
    mk("tbl.diagNone", QStringLiteral("No Division"), "", QKeySequence(), [this] {
        TableItem *t = selTable(m_ed);
        if (!t || !m_ed->isEditingText()) return;
        const auto tt = m_ed->textTarget();
        m_ed->change(QStringLiteral("Diagonals"), [&] { t->cell(tt.row, tt.col).diagonal = 0; });
    });
    mk("tbl.selectTable", QStringLiteral("Select Table"), "", QKeySequence(), [this] { if (TableItem *t = selTable(m_ed)) { m_ed->endTextEdit(); m_ed->select(t->id); } });
    mk("tbl.selectCell", QStringLiteral("Select Cell"), "", QKeySequence(), [this] {
        if (!m_ed->isEditingText()) return;
        QTextCursor c = m_ed->cursor();
        c.select(QTextCursor::Document);
        m_ed->setCursor(c);
    });
    mk("tbl.grow", QStringLiteral("Grow to Fit Text"), "", QKeySequence(), [this] {
        if (TableItem *t = selTable(m_ed)) m_ed->change(QStringLiteral("Grow to Fit Text"), [this, t] { t->growToFit = !t->growToFit; m_ed->fitTableRows(t); });
    }, true);
    mk("tbl.distributeRows", QStringLiteral("Distribute Rows"), "", QKeySequence(), [this] {
        if (TableItem *t = selTable(m_ed)) m_ed->change(QStringLiteral("Distribute Rows"), [t] {
            double total = 0;
            for (double h : t->rowH) total += h;
            for (double &h : t->rowH) h = total / t->rows;
        });
    });
    mk("tbl.distributeCols", QStringLiteral("Distribute Columns"), "", QKeySequence(), [this] {
        if (TableItem *t = selTable(m_ed)) m_ed->change(QStringLiteral("Distribute Columns"), [t] {
            double total = 0;
            for (double w : t->colW) total += w;
            for (double &w : t->colW) w = total / t->cols;
        });
    });
    for (const auto &[id, label, which] : {std::tuple{"border.all", "All Borders", 0}, {"border.outside", "Outside Borders", 1}, {"border.inside", "Inside Borders", 2},
                                            {"border.none", "No Border", 3}, {"border.top", "Top Border", 4}, {"border.bottom", "Bottom Border", 5},
                                            {"border.left", "Left Border", 6}, {"border.right", "Right Border", 7}}) {
        const int w = which;
        mk(id, QString::fromLatin1(label), "", QKeySequence(), [this, w] {
            TableItem *t = selTable(m_ed);
            if (!t) return;
            const Stroke s = w == 3 ? Stroke::none() : currentBorderStroke();
            const bool inCell = m_ed->isEditingText();
            const int r0 = inCell ? m_ed->textTarget().row : 0, c0 = inCell ? m_ed->textTarget().col : 0;
            const int r1 = inCell ? r0 : t->rows - 1, c1 = inCell ? c0 : t->cols - 1;
            m_ed->change(QStringLiteral("Borders"), [&] {
                for (int r = r0; r <= r1; ++r)
                    for (int c = c0; c <= c1; ++c) {
                        CellBorder &b = t->cell(r, c).border;
                        const bool top = r == r0, bottom = r == r1, left = c == c0, right = c == c1;
                        switch (w) {
                        case 0: case 3: b.top = b.bottom = b.left = b.right = s; break;
                        case 1: if (top) b.top = s; if (bottom) b.bottom = s; if (left) b.left = s; if (right) b.right = s; break;
                        case 2: if (!top) b.top = s; if (!bottom) b.bottom = s; if (!left) b.left = s; if (!right) b.right = s; break;
                        case 4: if (top) b.top = s; break;
                        case 5: if (bottom) b.bottom = s; break;
                        case 6: if (left) b.left = s; break;
                        case 7: if (right) b.right = s; break;
                        }
                    }
            });
        });
    }

    // ---------------- TextArt ----------------
    mk("wa.edit", QStringLiteral("Edit Text"), "pencil", QKeySequence(), [this] { if (Item *it = m_ed->single(); it && it->type() == ItemType::TextArt) editTextArt(it->id); });
    for (const auto &[id, label, v] : {std::tuple{"waspace.vt", "Very Tight", 0.8}, {"waspace.t", "Tight", 0.9}, {"waspace.n", "Normal", 1.0}, {"waspace.l", "Loose", 1.2}, {"waspace.vl", "Very Loose", 1.5}}) {
        const double s = v;
        mk(id, QString::fromLatin1(label), "", QKeySequence(), [ed, s] {
            ed->forEachSelected(QStringLiteral("Spacing"), [s](Item *it) { if (auto *w = dynamic_cast<TextArtItem *>(it)) w->spacing = s; });
        });
    }
    mk("wa.even", QStringLiteral("Even Height"), "move-vertical", QKeySequence(), [ed] {
        ed->forEachSelected(QStringLiteral("Even Height"), [](Item *it) { if (auto *w = dynamic_cast<TextArtItem *>(it)) w->evenHeight = !w->evenHeight; });
    }, true);
    mk("wa.vertical", QStringLiteral("Text Art Vertical Text"), "arrow-down-narrow-wide", QKeySequence(), [ed] {
        ed->forEachSelected(QStringLiteral("Vertical Text"), [](Item *it) {
            if (auto *w = dynamic_cast<TextArtItem *>(it)) { w->vertical = !w->vertical; w->rect.setSize(w->rect.size().transposed()); }
        });
    }, true);
    const char *waAlign[] = {"Left Align", "Center", "Right Align", "Word Justify", "Letter Justify", "Stretch Justify"};
    for (int i = 0; i < 6; ++i) {
        mk(QStringLiteral("waalign.%1").arg(i), QString::fromLatin1(waAlign[i]), "", QKeySequence(), [ed, i] {
            ed->forEachSelected(QStringLiteral("Align Text"), [i](Item *it) { if (auto *w = dynamic_cast<TextArtItem *>(it)) w->align = i; });
        });
    }
}

} // namespace jp
