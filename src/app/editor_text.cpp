// Text formatting commands. When editing text they apply to the selection
// (or the word at the cursor); when text boxes are selected but not being
// edited they apply to all of their text.

#include "app/editor.h"

#include "text/textprops.h"

#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>

namespace jp {

QVector<QTextCursor> Editor::formatTargets() const
{
    QVector<QTextCursor> out;
    if (isEditingText()) {
        out << m_cursor;
        return out;
    }
    QSet<QString> seen;
    auto addStory = [&](const QString &sid) {
        if (sid.isEmpty() || seen.contains(sid)) return;
        seen.insert(sid);
        if (QTextDocument *d = m_doc->storyDoc(sid)) {
            QTextCursor c(d);
            c.select(QTextCursor::Document);
            out << c;
        }
    };
    std::function<void(Item *)> visit = [&](Item *it) {
        switch (it->type()) {
        case ItemType::Text: addStory(static_cast<TextItem *>(it)->storyId); break;
        case ItemType::Shape: addStory(static_cast<ShapeItem *>(it)->storyId); break;
        case ItemType::Table: for (const auto &c : static_cast<TableItem *>(it)->cells) addStory(c.storyId); break;
        case ItemType::Group: for (const auto &c : static_cast<GroupItem *>(it)->children) visit(c.get()); break;
        default: break;
        }
    };
    for (Item *it : selectedItems()) visit(it);
    return out;
}

QTextCharFormat Editor::currentCharFormat() const
{
    if (isEditingText()) {
        QTextCursor c = m_cursor;
        if (c.hasSelection() && c.position() > c.anchor()) {
            // Format of the first selected character.
            QTextCursor f(c.document());
            f.setPosition(c.selectionStart() + 1);
            return f.charFormat();
        }
        return c.charFormat();
    }
    const auto t = formatTargets();
    if (t.isEmpty()) return QTextCharFormat();
    QTextCursor f(t.first().document());
    f.setPosition(std::min(1, t.first().document()->characterCount() - 1));
    return f.charFormat();
}

QTextBlockFormat Editor::currentBlockFormat() const
{
    if (isEditingText()) return m_cursor.blockFormat();
    const auto t = formatTargets();
    return t.isEmpty() ? QTextBlockFormat() : t.first().document()->begin().blockFormat();
}

QString Editor::currentStyleName() const { return currentBlockFormat().stringProperty(tp::StyleName); }

static void selectWordIfCollapsed(QTextCursor &c)
{
    if (c.hasSelection()) return;
    QTextCursor w = c;
    w.select(QTextCursor::WordUnderCursor);
    if (w.hasSelection() && !w.selectedText().trimmed().isEmpty()) c = w;
}

void Editor::mergeCharFormat(const QTextCharFormat &f, const QString &label)
{
    auto targets = formatTargets();
    if (targets.isEmpty()) return;
    beginChange(label);
    if (isEditingText()) {
        QTextCursor c = m_cursor;
        selectWordIfCollapsed(c);
        if (c.hasSelection()) c.mergeCharFormat(f);
        m_cursor.mergeCharFormat(f);   // also affects text typed next
        if (!c.hasSelection() || c.selectionStart() == c.block().position()) c.mergeBlockCharFormat(f);
    } else {
        for (QTextCursor &c : targets) {
            c.mergeCharFormat(f);
            c.mergeBlockCharFormat(f);
        }
    }
    endChange();
    textCursorChanged();
}

void Editor::mergeBlockFormat(const QTextBlockFormat &f, const QString &label)
{
    auto targets = formatTargets();
    if (targets.isEmpty()) return;
    beginChange(label);
    for (QTextCursor &c : targets) c.mergeBlockFormat(f);
    if (isEditingText()) m_cursor.mergeBlockFormat(f);
    endChange();
    Q_EMIT textCursorChanged();
}

void Editor::setCharProperty(int prop, const QVariant &v, const QString &label)
{
    QTextCharFormat f;
    f.setProperty(prop, v);
    mergeCharFormat(f, label);
}

void Editor::clearCharProperty(int prop, const QString &label)
{
    auto targets = formatTargets();
    if (targets.isEmpty()) return;
    beginChange(label);
    for (QTextCursor c : targets) {
        if (isEditingText()) selectWordIfCollapsed(c);
        // Walk fragments in the selection and remove the property.
        const int s = c.selectionStart(), e = c.selectionEnd();
        QTextDocument *d = c.document();
        for (QTextBlock b = d->findBlock(s); b.isValid() && b.position() <= e; b = b.next()) {
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment fr = it.fragment();
                const int fs = std::max(s, fr.position()), fe = std::min(e, fr.position() + fr.length());
                if (fs >= fe) continue;
                QTextCursor fc(d);
                fc.setPosition(fs);
                fc.setPosition(fe, QTextCursor::KeepAnchor);
                QTextCharFormat cf = fr.charFormat();
                cf.clearProperty(prop);
                fc.setCharFormat(cf);
            }
            QTextCursor bc(b);
            QTextCharFormat bcf = b.charFormat();
            bcf.clearProperty(prop);
            bc.setBlockCharFormat(bcf);
        }
    }
    if (isEditingText()) {
        QTextCharFormat cf = m_cursor.charFormat();
        cf.clearProperty(prop);
        m_cursor.setCharFormat(cf);
    }
    endChange();
    Q_EMIT textCursorChanged();
}

void Editor::toggleBold()
{
    QTextCharFormat f;
    f.setFontWeight(currentCharFormat().fontWeight() >= QFont::DemiBold ? QFont::Normal : QFont::Bold);
    mergeCharFormat(f, QStringLiteral("Bold"));
}

void Editor::toggleItalic()
{
    QTextCharFormat f;
    f.setFontItalic(!currentCharFormat().fontItalic());
    mergeCharFormat(f, QStringLiteral("Italic"));
}

void Editor::toggleUnderline(QTextCharFormat::UnderlineStyle style)
{
    const QTextCharFormat cur = currentCharFormat();
    QTextCharFormat f;
    const bool on = cur.underlineStyle() != QTextCharFormat::NoUnderline && cur.underlineStyle() == style;
    f.setUnderlineStyle(on ? QTextCharFormat::NoUnderline : style);
    mergeCharFormat(f, QStringLiteral("Underline"));
}

void Editor::toggleStrike()
{
    QTextCharFormat f;
    f.setFontStrikeOut(!currentCharFormat().fontStrikeOut());
    mergeCharFormat(f, QStringLiteral("Strikethrough"));
}

void Editor::toggleScript(bool super)
{
    const auto want = super ? QTextCharFormat::AlignSuperScript : QTextCharFormat::AlignSubScript;
    QTextCharFormat f;
    f.setVerticalAlignment(currentCharFormat().verticalAlignment() == want ? QTextCharFormat::AlignNormal : want);
    mergeCharFormat(f, super ? QStringLiteral("Superscript") : QStringLiteral("Subscript"));
}

void Editor::setFontFamily(const QString &family)
{
    QTextCharFormat f;
    if (family.startsWith(QLatin1String("+"))) {
        // "+Heading" / "+Body": follow the font scheme.
        f.setProperty(tp::ThemeFont, family == QLatin1String("+Heading") ? QStringLiteral("major") : QStringLiteral("minor"));
        clearCharProperty(QTextFormat::FontFamilies, QStringLiteral("Font"));
        mergeCharFormat(f, QStringLiteral("Font"));
        return;
    }
    f.setFontFamilies(QStringList{family});
    mergeCharFormat(f, QStringLiteral("Font"));
}

void Editor::setFontSize(double pt)
{
    if (pt <= 0) return;
    QTextCharFormat f;
    f.setFontPointSize(std::clamp(pt, 0.5, 999.0));
    mergeCharFormat(f, QStringLiteral("Font Size"));
}

void Editor::growFont(int dir)
{
    static const double steps[] = {6, 7, 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72, 96, 120, 144, 200, 288};
    auto targets = formatTargets();
    if (targets.isEmpty()) return;
    beginChange(dir > 0 ? QStringLiteral("Grow Font") : QStringLiteral("Shrink Font"));
    for (QTextCursor c : targets) {
        if (isEditingText()) selectWordIfCollapsed(c);
        const int s = c.selectionStart(), e = c.selectionEnd();
        QTextDocument *d = c.document();
        for (QTextBlock b = d->findBlock(s); b.isValid() && b.position() <= e; b = b.next())
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment fr = it.fragment();
                const int fs = std::max(s, fr.position()), fe = std::min(e, fr.position() + fr.length());
                if (fs >= fe) continue;
                const double cur = fr.charFormat().hasProperty(QTextFormat::FontPointSize) ? fr.charFormat().fontPointSize() : 11;
                double next = cur;
                if (dir > 0) { for (double v : steps) if (v > cur + 0.01) { next = v; break; } if (next == cur) next = cur + 12; }
                else { for (int i = int(std::size(steps)) - 1; i >= 0; --i) if (steps[i] < cur - 0.01) { next = steps[i]; break; } if (next == cur) next = std::max(1.0, cur - 1); }
                QTextCursor fc(d);
                fc.setPosition(fs);
                fc.setPosition(fe, QTextCursor::KeepAnchor);
                QTextCharFormat f;
                f.setFontPointSize(next);
                fc.mergeCharFormat(f);
            }
    }
    endChange();
    Q_EMIT textCursorChanged();
}

void Editor::clearFormatting()
{
    auto targets = formatTargets();
    if (targets.isEmpty()) return;
    beginChange(QStringLiteral("Clear All Formatting"));
    for (QTextCursor c : targets) {
        if (isEditingText() && !c.hasSelection()) c.select(QTextCursor::BlockUnderCursor);
        const int s = c.selectionStart(), e = c.selectionEnd();
        QTextDocument *d = c.document();
        for (QTextBlock b = d->findBlock(s); b.isValid() && b.position() <= e; b = b.next()) {
            const QString st = b.blockFormat().stringProperty(tp::StyleName);
            const TextStyle *ts = m_doc->style(st.isEmpty() ? QStringLiteral("Normal") : st);
            QTextCharFormat base = ts ? ts->chr : QTextCharFormat();
            QTextCursor bc(d);
            bc.setPosition(std::max(s, b.position()));
            bc.setPosition(std::min(e, b.position() + b.length() - 1), QTextCursor::KeepAnchor);
            QTextCharFormat keepFields;
            // Keep fields working.
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment fr = it.fragment();
                const int fs = std::max(bc.selectionStart(), fr.position()), fe = std::min(bc.selectionEnd(), fr.position() + fr.length());
                if (fs >= fe) continue;
                QTextCursor fc(d);
                fc.setPosition(fs);
                fc.setPosition(fe, QTextCursor::KeepAnchor);
                QTextCharFormat nf = base;
                const QString field = fr.charFormat().stringProperty(tp::Field);
                if (!field.isEmpty()) nf.setProperty(tp::Field, field);
                fc.setCharFormat(nf);
            }
        }
    }
    endChange();
    Q_EMIT textCursorChanged();
}

void Editor::setTextColor(const ColorRef &c)
{
    if (c.isNone()) { clearCharProperty(tp::ColorRefP, QStringLiteral("Font Color")); return; }
    QTextCharFormat f;
    f.setProperty(tp::ColorRefP, c.toString());
    f.clearForeground();
    mergeCharFormat(f, QStringLiteral("Font Color"));
}

void Editor::setHighlight(const ColorRef &c)
{
    if (c.isNone()) { clearCharProperty(tp::HighlightRefP, QStringLiteral("Highlight")); return; }
    setCharProperty(tp::HighlightRefP, c.toString(), QStringLiteral("Highlight"));
}

void Editor::changeCase(int mode)
{
    auto targets = formatTargets();
    if (targets.isEmpty()) return;
    beginChange(QStringLiteral("Change Case"));
    for (QTextCursor c : targets) {
        if (isEditingText()) selectWordIfCollapsed(c);
        const int s = c.selectionStart(), e = c.selectionEnd();
        QTextDocument *d = c.document();
        bool newSentence = true;
        // Replace character by character to keep formatting.
        for (int pos = s; pos < e; ++pos) {
            const QChar ch = d->characterAt(pos);
            if (ch == QChar::ObjectReplacementCharacter) continue;
            QChar out = ch;
            switch (mode) {
            case 0: out = newSentence ? ch.toUpper() : ch.toLower(); break;
            case 1: out = ch.toLower(); break;
            case 2: out = ch.toUpper(); break;
            case 3: {
                const QChar prev = pos > 0 ? d->characterAt(pos - 1) : QChar(' ');
                out = (prev.isSpace() || prev == QChar::ParagraphSeparator || pos == s && !prev.isLetter()) ? ch.toUpper() : ch.toLower();
                break;
            }
            case 4: out = ch.isUpper() ? ch.toLower() : ch.toUpper(); break;
            }
            if (ch.isLetter()) newSentence = false;
            if (ch == '.' || ch == '!' || ch == '?' || ch == QChar::ParagraphSeparator) newSentence = true;
            if (out != ch) {
                QTextCursor fc(d);
                fc.setPosition(pos);
                fc.setPosition(pos + 1, QTextCursor::KeepAnchor);
                const QTextCharFormat cf = fc.charFormat();
                fc.insertText(QString(out), cf);
            }
        }
    }
    endChange();
    Q_EMIT textCursorChanged();
}

void Editor::setAlignment(Qt::Alignment a)
{
    QTextBlockFormat f;
    f.setAlignment(a);
    f.setProperty(tp::Distribute, false);
    mergeBlockFormat(f, QStringLiteral("Alignment"));
}

void Editor::setDirection(Qt::LayoutDirection d)
{
    QTextBlockFormat f;
    f.setLayoutDirection(d);
    const Qt::Alignment al = currentBlockFormat().alignment() & Qt::AlignHorizontal_Mask;
    if (d == Qt::RightToLeft && (al == 0 || al == Qt::AlignLeft || al == Qt::AlignLeading)) f.setAlignment(Qt::AlignRight);
    if (d == Qt::LeftToRight && (al == Qt::AlignRight || al == Qt::AlignTrailing)) f.setAlignment(Qt::AlignLeft);
    mergeBlockFormat(f, QStringLiteral("Text Direction"));
}

void Editor::setLineSpacing(int type, double value)
{
    QTextBlockFormat f;
    f.setLineHeight(value, type);
    mergeBlockFormat(f, QStringLiteral("Line Spacing"));
}

void Editor::setParagraphSpacing(double before, double after)
{
    QTextBlockFormat f;
    if (before >= 0) f.setTopMargin(before);
    if (after >= 0) f.setBottomMargin(after);
    mergeBlockFormat(f, QStringLiteral("Paragraph Spacing"));
}

void Editor::changeIndent(int dir)
{
    auto targets = formatTargets();
    if (targets.isEmpty()) return;
    beginChange(dir > 0 ? QStringLiteral("Increase Indent") : QStringLiteral("Decrease Indent"));
    for (QTextCursor c : targets) {
        QTextDocument *d = c.document();
        for (QTextBlock b = d->findBlock(c.selectionStart()); b.isValid() && b.position() <= c.selectionEnd(); b = b.next()) {
            QTextBlockFormat bf = b.blockFormat();
            bf.setLeftMargin(std::max(0.0, bf.leftMargin() + dir * 18.0));
            QTextCursor(b).setBlockFormat(bf);
        }
    }
    endChange();
    Q_EMIT textCursorChanged();
}

void Editor::setList(int kind, int format, const QString &bullet, int start)
{
    auto targets = formatTargets();
    if (targets.isEmpty()) return;
    beginChange(kind == 0 ? QStringLiteral("Remove List") : kind == 1 ? QStringLiteral("Bullets") : QStringLiteral("Numbering"));
    for (QTextCursor c : targets) {
        QTextDocument *d = c.document();
        QTextBlock first = d->findBlock(c.selectionStart()), last = d->findBlock(c.selectionEnd());
        if (kind == 0) {
            for (QTextBlock b = first; b.isValid(); b = b.next()) {
                if (QTextList *l = b.textList()) l->remove(b);
                QTextBlockFormat bf = b.blockFormat();
                bf.setIndent(0);
                bf.setTextIndent(0);
                bf.setLeftMargin(std::max(0.0, bf.leftMargin() - 18));
                QTextCursor(b).setBlockFormat(bf);
                if (b == last) break;
            }
            continue;
        }
        QTextListFormat lf;
        if (kind == 1) {
            lf.setStyle(QTextListFormat::ListDisc);
            if (!bullet.isEmpty()) lf.setProperty(tp::BulletChar, bullet);
        } else {
            static const QTextListFormat::Style styles[] = {QTextListFormat::ListDecimal, QTextListFormat::ListDecimal, QTextListFormat::ListLowerAlpha,
                                                           QTextListFormat::ListUpperAlpha, QTextListFormat::ListLowerRoman, QTextListFormat::ListUpperRoman,
                                                           QTextListFormat::ListDecimal, QTextListFormat::ListDecimal};
            lf.setStyle(styles[std::clamp(format, 0, 7)]);
            lf.setNumberSuffix(format == 6 || format == 7 ? QStringLiteral(")") : QStringLiteral("."));
            if (format == 7) lf.setNumberPrefix(QStringLiteral("("));
            lf.setStart(std::max(1, start));
            lf.setProperty(tp::NumberFormat, format);
        }
        lf.setIndent(0);
        QTextList *list = nullptr;
        for (QTextBlock b = first; b.isValid(); b = b.next()) {
            if (QTextList *old = b.textList()) old->remove(b);
            QTextBlockFormat bf = b.blockFormat();
            bf.setLeftMargin(std::max(bf.leftMargin(), 18.0));
            bf.setTextIndent(-18);
            bf.setIndent(0);
            QTextCursor bc(b);
            bc.setBlockFormat(bf);
            if (!list) list = bc.createList(lf);
            else list->add(b);
            if (b == last) break;
        }
    }
    endChange();
    Q_EMIT textCursorChanged();
}

void Editor::applyStyle(const QString &name)
{
    const TextStyle *s = m_doc->style(name);
    if (!s) return;
    auto targets = formatTargets();
    if (targets.isEmpty()) return;
    beginChange(QStringLiteral("Apply Style"));
    for (QTextCursor c : targets) {
        if (s->charOnly) {
            if (isEditingText()) selectWordIfCollapsed(c);
            c.mergeCharFormat(s->chr);
            continue;
        }
        QTextDocument *d = c.document();
        for (QTextBlock b = d->findBlock(c.selectionStart()); b.isValid() && b.position() <= c.selectionEnd(); b = b.next()) {
            QTextCursor bc(b);
            QTextBlockFormat bf = b.blockFormat();
            for (auto it = s->blk.properties().cbegin(); it != s->blk.properties().cend(); ++it) bf.setProperty(it.key(), it.value());
            bf.setProperty(tp::StyleName, name);
            bc.setBlockFormat(bf);
            bc.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            QTextCharFormat cf = s->chr;
            cf.setProperty(tp::CharStyle, QString());
            bc.mergeCharFormat(cf);
            bc.mergeBlockCharFormat(cf);
            // Style fonts follow the scheme unless the style names a font.
            if (!s->chr.hasProperty(QTextFormat::FontFamilies)) {
                for (auto it = b.begin(); !it.atEnd(); ++it) {
                    const QTextFragment fr = it.fragment();
                    QTextCursor fc(d);
                    fc.setPosition(fr.position());
                    fc.setPosition(fr.position() + fr.length(), QTextCursor::KeepAnchor);
                    QTextCharFormat ff = fr.charFormat();
                    ff.clearProperty(QTextFormat::FontFamilies);
                    fc.setCharFormat(ff);
                }
            }
        }
    }
    if (isEditingText()) m_cursor.setCharFormat(m_cursor.block().charFormat());
    endChange();
    Q_EMIT textCursorChanged();
}

void Editor::setDropCap(int lines, int chars, const QString &font)
{
    QTextBlockFormat f;
    f.setProperty(tp::DropCapLines, lines);
    f.setProperty(tp::DropCapChars, chars);
    if (!font.isEmpty()) f.setProperty(tp::DropCapFont, font);
    // Drop caps apply to the first paragraph of the selection only.
    if (isEditingText()) {
        beginChange(QStringLiteral("Drop Cap"));
        m_cursor.mergeBlockFormat(f);
        endChange();
        Q_EMIT textCursorChanged();
        return;
    }
    auto targets = formatTargets();
    if (targets.isEmpty()) return;
    beginChange(QStringLiteral("Drop Cap"));
    for (QTextCursor c : targets) QTextCursor(c.document()->begin()).mergeBlockFormat(f);
    endChange();
}

void Editor::insertText(const QString &t)
{
    if (!isEditingText()) return;
    if (!m_typing) {
        beginChange(QStringLiteral("Typing"));
        m_typing = true;
    }
    m_cursor.insertText(t);
    textEdited();
}

void Editor::editText(const std::function<void(QTextCursor &)> &fn)
{
    if (!isEditingText()) return;
    if (!m_typing) {
        beginChange(QStringLiteral("Typing"));
        m_typing = true;
    }
    fn(m_cursor);
    textEdited();
}

void Editor::retargetText(const QString &itemId)
{
    if (!isEditingText() || itemId == m_text.itemId) return;
    m_text.itemId = itemId;
    m_sel = QStringList{itemId};
    Q_EMIT selectionChanged();
}

void Editor::insertField(const QString &code)
{
    if (!isEditingText()) return;
    beginChange(QStringLiteral("Insert Field"));
    QTextCharFormat f = m_cursor.charFormat();
    f.setProperty(tp::Field, code);
    m_cursor.insertText(QString(QChar::ObjectReplacementCharacter), f);
    f.clearProperty(tp::Field);
    m_cursor.setCharFormat(f);
    endChange();
    textEdited();
}

void Editor::insertTextBlock(const QString &text, const QString &label)
{
    if (!isEditingText()) return;
    beginChange(label);
    m_cursor.insertText(text);
    endChange();
    textEdited();
}

} // namespace jp
