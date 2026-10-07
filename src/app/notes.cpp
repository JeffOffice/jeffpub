#include "app/notes.h"

#include "app/editor.h"
#include "core/document.h"
#include "text/textprops.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

namespace jp {

namespace {

QString noteField(const QTextCharFormat &f, bool *endnote)
{
    const QString code = f.stringProperty(tp::Field);
    const bool foot = code.startsWith(QLatin1String("footnote:")), end = code.startsWith(QLatin1String("endnote:"));
    if (!foot && !end) return {};
    if (endnote) *endnote = end;
    return code.section(QLatin1Char(':'), 1);
}

} // namespace

QString noteAtCursor(Editor *ed, bool *endnote)
{
    if (!ed->isEditingText()) return {};
    QTextCursor c = ed->cursor();
    c.clearSelection();
    // The reference just before the cursor, then the one just after it.
    const QString before = noteField(c.charFormat(), endnote);
    if (!before.isEmpty() && c.position() > c.block().position()) return before;
    if (c.movePosition(QTextCursor::NextCharacter)) return noteField(c.charFormat(), endnote);
    return {};
}

QString addNote(Editor *ed, bool endnote, const QString &text)
{
    if (!ed->isEditingText()) return {};
    Document *d = ed->doc();
    ed->flushTyping();
    ed->beginChange(endnote ? QStringLiteral("Insert Endnote") : QStringLiteral("Insert Footnote"));
    QTextCursor &c = ed->cursor();
    c.clearSelection();
    QTextCharFormat here = c.charFormat();
    here.clearProperty(tp::Field);
    // The note's text: the text's font a size smaller (80%, at least 7 pt).
    const QString storyId = d->createStory(text);
    if (QTextDocument *nd = d->storyDoc(storyId)) {
        QTextCharFormat nf = here;
        nf.setVerticalAlignment(QTextCharFormat::AlignNormal);
        const double size = here.hasProperty(QTextFormat::FontPointSize) ? here.fontPointSize() : 11;
        nf.setFontPointSize(std::max(7.0, std::round(size * 0.8 * 2) / 2));
        QTextCursor all(nd);
        all.select(QTextCursor::Document);
        all.mergeCharFormat(nf);
        all.mergeBlockCharFormat(nf);
    }
    QTextCharFormat ref = here;
    ref.setProperty(tp::Field, (endnote ? QStringLiteral("endnote:") : QStringLiteral("footnote:")) + storyId);
    ref.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
    c.insertText(QString(QChar::ObjectReplacementCharacter), ref);
    c.setCharFormat(here);
    ed->endChange();
    ed->textEdited();
    return storyId;
}

void setNoteText(Editor *ed, const QString &storyId, const QString &text)
{
    QTextDocument *nd = ed->doc()->storyDoc(storyId);
    if (!nd) return;
    ed->beginChange(QStringLiteral("Edit Note"));
    QTextCursor all(nd);
    QTextCharFormat keep = nd->begin().begin().atEnd() ? nd->begin().charFormat() : nd->begin().begin().fragment().charFormat();
    all.select(QTextCursor::Document);
    all.insertText(text, keep);
    ed->endChange();
    ed->textEdited();
}

void noteDialog(QWidget *parent, Editor *ed, bool endnote)
{
    if (!ed->isEditingText() || ed->textTarget().row >= 0) return;
    bool isEnd = endnote;
    const QString existing = noteAtCursor(ed, &isEnd);
    QTextDocument *nd = existing.isEmpty() ? nullptr : ed->doc()->storyDoc(existing);
    QDialog dlg(parent);
    const QString kind = isEnd ? QStringLiteral("Endnote") : QStringLiteral("Footnote");
    dlg.setWindowTitle(nd ? QStringLiteral("Edit %1").arg(kind) : QStringLiteral("Insert %1").arg(kind));
    auto *v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel(isEnd ? QStringLiteral("Endnote text (listed under Notes at the end of the story):")
                                  : QStringLiteral("Footnote text (at the bottom of the column with its number):"), &dlg));
    auto *edit = new QPlainTextEdit(&dlg);
    if (nd) edit->setPlainText(nd->toPlainText());
    edit->setMinimumSize(420, 140);
    v->addWidget(edit);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    edit->setFocus();
    if (dlg.exec() != QDialog::Accepted) return;
    const QString text = edit->toPlainText().trimmed();
    if (nd) setNoteText(ed, existing, text);
    else if (!text.isEmpty()) addNote(ed, isEnd, text);
}

} // namespace jp
