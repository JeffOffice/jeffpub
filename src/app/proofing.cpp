// Spelling (Hunspell + LibreOffice's en_US dictionary), thesaurus (WordNet
// based th_en_US_v2) and automatic hyphenation (Liang's algorithm with the
// en_US TeX patterns).

#include "app/appfuncs.h"
#include "app/dialogs.h"
#include "app/editor.h"
#include "app/settings.h"
#include "text/hyphenation.h"
#include "text/textprops.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <hunspell/hunspell.hxx>
#include <memory>

namespace jp {

static QString dictDir()
{
    const QString app = QCoreApplication::applicationDirPath();
    QStringList dirs{app + "/dict", app + "/../share/jeffpub79/dict", app + "/../dict"};
#ifdef JP_SOURCE_DIR
    dirs << QStringLiteral(JP_SOURCE_DIR) + "/resources/dict";
#endif
    for (const QString &d : dirs)
        if (QFile::exists(d + "/en_US.dic")) return d;
    return {};
}

// ---------------- spelling ----------------
class Speller {
public:
    static Speller &get()
    {
        static Speller s;
        return s;
    }
    bool ok() const { return bool(m_h); }
    bool check(const QString &w)
    {
        if (!m_h || w.isEmpty()) return true;
        if (m_ignore.contains(w.toLower())) return true;
        Settings &st = Settings::get();
        if (st.value("proof/ignoreUpper", true).toBool() && w == w.toUpper() && w.size() > 1) return true;
        if (st.value("proof/ignoreNumbers", true).toBool() && w.contains(QRegularExpression("\\d"))) return true;
        return m_h->spell(w.toStdString());
    }
    QStringList suggest(const QString &w)
    {
        QStringList out;
        if (!m_h) return out;
        for (const auto &s : m_h->suggest(w.toStdString())) out << QString::fromStdString(s);
        return out;
    }
    void ignore(const QString &w) { m_ignore.insert(w.toLower()); }
    void add(const QString &w)
    {
        if (!m_h) return;
        m_h->add(w.toStdString());
        QFile f(userDict());
        if (f.open(QIODevice::Append)) f.write((w + "\n").toUtf8());
    }

private:
    Speller()
    {
        const QString d = dictDir();
        if (d.isEmpty()) return;
        m_h = std::make_unique<Hunspell>(QFile::encodeName(d + "/en_US.aff").constData(), QFile::encodeName(d + "/en_US.dic").constData());
        QFile f(userDict());
        if (f.open(QIODevice::ReadOnly))
            for (const QByteArray &line : f.readAll().split('\n'))
                if (!line.trimmed().isEmpty()) m_h->add(line.trimmed().toStdString());
    }
    static QString userDict()
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        return dir + "/custom.dic";
    }
    std::unique_ptr<Hunspell> m_h;
    QSet<QString> m_ignore;
};

static const QRegularExpression &wordRe()
{
    static const QRegularExpression re(QStringLiteral("[\\p{L}][\\p{L}\\p{M}'’]*"));
    return re;
}

// Misspelled ranges in a story, skipping fields and "no proofing" text.
QVector<QPair<int, int>> misspellings(QTextDocument *doc)
{
    QVector<QPair<int, int>> out;
    if (!Speller::get().ok()) return out;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const QString text = b.text();
        auto it = wordRe().globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            QString w = m.captured();
            while (w.endsWith('\'') || w.endsWith(QChar(0x2019))) w.chop(1);
            if (w.size() < 2) continue;
            QTextCursor c(doc);
            c.setPosition(b.position() + m.capturedStart() + 1);
            if (c.charFormat().boolProperty(tp::NoProof)) continue;
            if (!Speller::get().check(w)) out << qMakePair(b.position() + int(m.capturedStart()), b.position() + int(m.capturedStart() + w.size()));
        }
    }
    return out;
}

QStringList spellingSuggestions(const QString &w) { return Speller::get().suggest(w); }
void spellingAdd(const QString &w) { Speller::get().add(w); }
void spellingIgnore(const QString &w) { Speller::get().ignore(w); }

void spellingDialog(QWidget *p, Editor *ed)
{
    if (!Speller::get().ok()) {
        QMessageBox::warning(p, QStringLiteral("Spelling"), QStringLiteral("The English dictionary is missing from this installation."));
        return;
    }
    // Stories in reading order.
    struct S { QString itemId, storyId; int page; int row = -1, col = -1; };
    QVector<S> stories;
    QSet<QString> seen;
    Document *d = ed->doc();
    for (int pg = 0; pg < d->pages.size(); ++pg)
        walkItems(d->pages[pg]->items, [&](const ItemPtr &it) {
            if (it->type() == ItemType::Text) {
                auto *t = static_cast<TextItem *>(it.get());
                if (!seen.contains(t->storyId)) { seen.insert(t->storyId); stories << S{d->chainOf(t->id).value(0, t)->id, t->storyId, pg}; }
            } else if (it->type() == ItemType::Shape && !static_cast<ShapeItem *>(it.get())->storyId.isEmpty()) {
                stories << S{it->id, static_cast<ShapeItem *>(it.get())->storyId, pg};
            } else if (it->type() == ItemType::Table) {
                auto *tb = static_cast<TableItem *>(it.get());
                for (int r = 0; r < tb->rows; ++r)
                    for (int c = 0; c < tb->cols; ++c) stories << S{tb->id, tb->cell(r, c).storyId, pg, r, c};
            }
        });
    QDialog dlg(p);
    dlg.setWindowTitle(QStringLiteral("Check Spelling"));
    auto *v = new QVBoxLayout(&dlg);
    auto *notIn = new QLabel(&dlg);
    auto *change = new QLineEdit(&dlg);
    auto *sugg = new QListWidget(&dlg);
    v->addWidget(new QLabel(QStringLiteral("Not in dictionary:")));
    v->addWidget(notIn);
    v->addWidget(new QLabel(QStringLiteral("Change to:")));
    v->addWidget(change);
    v->addWidget(new QLabel(QStringLiteral("Suggestions:")));
    v->addWidget(sugg);
    auto *row = new QHBoxLayout();
    auto *ignore = new QPushButton(QStringLiteral("Ignore"), &dlg), *ignoreAll = new QPushButton(QStringLiteral("Ignore All"), &dlg);
    auto *chg = new QPushButton(QStringLiteral("Change"), &dlg), *chgAll = new QPushButton(QStringLiteral("Change All"), &dlg);
    auto *add = new QPushButton(QStringLiteral("Add"), &dlg), *close = new QPushButton(QStringLiteral("Close"), &dlg);
    for (auto *b : {ignore, ignoreAll, chg, chgAll, add, close}) row->addWidget(b);
    v->addLayout(row);
    QObject::connect(sugg, &QListWidget::currentTextChanged, change, &QLineEdit::setText);
    int si = 0, pos = 0;
    QString word;
    std::function<bool()> next = [&]() -> bool {
        for (; si < stories.size(); ++si, pos = 0) {
            QTextDocument *doc = d->storyDoc(stories[si].storyId);
            if (!doc) continue;
            for (const auto &m : misspellings(doc)) {
                if (m.first < pos) continue;
                const S &s = stories[si];
                if (s.page != ed->currentPage()) ed->setCurrentPage(s.page);
                ed->beginTextEdit(s.itemId, m.first, s.row, s.col);
                QTextCursor c = ed->cursor();
                c.setPosition(m.first);
                c.setPosition(m.second, QTextCursor::KeepAnchor);
                ed->setCursor(c);
                word = c.selectedText();
                notIn->setText(QStringLiteral("<b>%1</b>").arg(word.toHtmlEscaped()));
                sugg->clear();
                sugg->addItems(Speller::get().suggest(word));
                change->setText(sugg->count() ? sugg->item(0)->text() : word);
                pos = m.second;
                return true;
            }
        }
        QMessageBox::information(&dlg, QStringLiteral("Check Spelling"), QStringLiteral("The spelling check is complete."));
        dlg.accept();
        return false;
    };
    QObject::connect(ignore, &QPushButton::clicked, &dlg, [&] { next(); });
    QObject::connect(ignoreAll, &QPushButton::clicked, &dlg, [&] { Speller::get().ignore(word); next(); });
    QObject::connect(add, &QPushButton::clicked, &dlg, [&] { Speller::get().add(word); next(); });
    QObject::connect(chg, &QPushButton::clicked, &dlg, [&] {
        ed->beginChange(QStringLiteral("Spelling"));
        ed->cursor().insertText(change->text());
        ed->endChange();
        pos = ed->cursor().position();
        next();
    });
    QObject::connect(chgAll, &QPushButton::clicked, &dlg, [&] {
        const QString from = word, to = change->text();
        ed->change(QStringLiteral("Spelling"), [&] {
            for (const S &s : stories) {
                QTextDocument *doc = d->storyDoc(s.storyId);
                QTextCursor c = doc->find(from, 0, QTextDocument::FindCaseSensitively | QTextDocument::FindWholeWords);
                while (!c.isNull()) {
                    c.insertText(to);
                    c = doc->find(from, c, QTextDocument::FindCaseSensitively | QTextDocument::FindWholeWords);
                }
            }
        });
        next();
    });
    QObject::connect(close, &QPushButton::clicked, &dlg, &QDialog::reject);
    if (!next()) return;
    dlg.exec();
}

// ---------------- thesaurus ----------------
class Thesaurus {
public:
    static Thesaurus &get()
    {
        static Thesaurus t;
        return t;
    }
    QStringList lookup(const QString &word)
    {
        load();
        // Try the word as typed, then its base form ("running" -> "run").
        for (const QString &key : baseForms(word.toLower())) {
            const QStringList out = lookupExact(key, word);
            if (!out.isEmpty()) return out;
        }
        return {};
    }

private:
    QStringList lookupExact(const QString &key, const QString &word) const
    {
        QStringList out;
        auto it = m_index.find(key);
        if (it == m_index.end()) return out;
        qsizetype p = it.value();
        // Line: word|count, then count lines "(pos)|syn|syn".
        const qsizetype eol = m_data.indexOf('\n', p);
        const int count = m_data.mid(p, eol - p).trimmed().split('|').value(1).toInt();
        p = eol + 1;
        for (int i = 0; i < count && p > 0 && p < m_data.size(); ++i) {
            const qsizetype e = m_data.indexOf('\n', p);
            const QList<QByteArray> parts = m_data.mid(p, (e < 0 ? m_data.size() : e) - p).trimmed().split('|');
            const QString pos = QString::fromUtf8(parts.value(0));
            for (int k = 1; k < parts.size(); ++k) {
                const QString s = QString::fromUtf8(parts[k]).trimmed();
                if (!s.isEmpty() && s.compare(word, Qt::CaseInsensitive) != 0 && s.compare(key, Qt::CaseInsensitive) != 0)
                    out << QStringLiteral("%1 %2").arg(s, pos);
            }
            if (e < 0) break;
            p = e + 1;
        }
        out.removeDuplicates();
        return out;
    }
    static QStringList baseForms(const QString &w)
    {
        QStringList f{w};
        auto add = [&](const QString &s) { if (s.size() >= 2 && !f.contains(s)) f << s; };
        auto undouble = [&](const QString &stem) {   // "runn" -> "run"
            if (stem.size() >= 3 && stem.back() == stem.at(stem.size() - 2)) add(stem.chopped(1));
        };
        if (w.endsWith(QLatin1String("'s"))) add(w.chopped(2));
        if (w.endsWith(QLatin1String("ies"))) add(w.chopped(3) + 'y');
        if (w.endsWith(QLatin1String("ied"))) add(w.chopped(3) + 'y');
        if (w.endsWith(QLatin1String("es"))) add(w.chopped(2));
        if (w.endsWith('s') && !w.endsWith(QLatin1String("ss"))) add(w.chopped(1));
        if (w.endsWith(QLatin1String("ing"))) { add(w.chopped(3)); add(w.chopped(3) + 'e'); undouble(w.chopped(3)); }
        if (w.endsWith(QLatin1String("ed"))) { add(w.chopped(2)); add(w.chopped(1)); undouble(w.chopped(2)); }
        if (w.endsWith(QLatin1String("ier"))) add(w.chopped(3) + 'y');
        if (w.endsWith(QLatin1String("iest"))) add(w.chopped(4) + 'y');
        if (w.endsWith(QLatin1String("er"))) { add(w.chopped(2)); add(w.chopped(1)); undouble(w.chopped(2)); }
        if (w.endsWith(QLatin1String("est"))) { add(w.chopped(3)); add(w.chopped(2)); undouble(w.chopped(3)); }
        if (w.endsWith(QLatin1String("ily"))) add(w.chopped(3) + 'y');
        if (w.endsWith(QLatin1String("ly"))) add(w.chopped(2));
        return f;
    }

    void load()
    {
        if (m_loaded) return;
        m_loaded = true;
        QFile f(dictDir() + "/th_en_US_v2.dat");
        if (!f.open(QIODevice::ReadOnly)) return;
        m_data = f.readAll();
        qsizetype p = m_data.indexOf('\n') + 1;   // skip encoding line
        while (p > 0 && p < m_data.size()) {
            const qsizetype e = m_data.indexOf('\n', p);
            if (e < 0) break;
            const QByteArray line = m_data.mid(p, e - p).trimmed();   // tolerate CRLF
            if (!line.startsWith('(')) {
                const int bar = line.indexOf('|');
                if (bar > 0) m_index.insert(QString::fromUtf8(line.left(bar)).toLower(), p);
            }
            p = e + 1;
        }
    }
    bool m_loaded = false;
    QByteArray m_data;
    QHash<QString, qsizetype> m_index;
};

QStringList thesaurusLookup(const QString &word) { return Thesaurus::get().lookup(word.trimmed()); }

void thesaurusDialog(QWidget *p, Editor *ed)
{
    QString word;
    if (ed->isEditingText()) {
        QTextCursor c = ed->cursor();
        if (!c.hasSelection()) {
            c.select(QTextCursor::WordUnderCursor);
            ed->setCursor(c);
        }
        word = c.selectedText().trimmed();
    }
    QDialog dlg(p);
    dlg.setWindowTitle(QStringLiteral("Thesaurus"));
    auto *v = new QVBoxLayout(&dlg);
    auto *q = new QLineEdit(word, &dlg);
    auto *list = new QListWidget(&dlg);
    v->addWidget(new QLabel(QStringLiteral("Look up:")));
    v->addWidget(q);
    v->addWidget(list);
    auto fill = [&] {
        list->clear();
        const QStringList syn = thesaurusLookup(q->text());
        if (syn.isEmpty()) list->addItem(q->text().trimmed().isEmpty() ? QStringLiteral("(Type a word to look up)") : QStringLiteral("(No suggestions)"));
        list->addItems(syn);
    };
    QObject::connect(q, &QLineEdit::returnPressed, &dlg, fill);
    // Look up as you type.
    auto *typing = new QTimer(&dlg);
    typing->setSingleShot(true);
    typing->setInterval(250);
    QObject::connect(typing, &QTimer::timeout, &dlg, fill);
    QObject::connect(q, &QLineEdit::textEdited, typing, qOverload<>(&QTimer::start));
    fill();
    if (list->count() && !list->item(0)->text().startsWith('(')) list->setCurrentRow(0);
    auto *bb = new QDialogButtonBox(&dlg);
    // Look up the chosen synonym in turn.
    auto *again = bb->addButton(QStringLiteral("Look Up"), QDialogButtonBox::ActionRole);
    QObject::connect(again, &QPushButton::clicked, &dlg, [&] {
        if (!list->currentItem() || list->currentItem()->text().startsWith('(')) return;
        q->setText(list->currentItem()->text().section(QStringLiteral(" ("), 0, 0));
        fill();
    });
    auto *insert = bb->addButton(QStringLiteral("Insert"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(insert, &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    v->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted || !list->currentItem() || !ed->isEditingText()) return;
    QString s = list->currentItem()->text();
    s = s.section(" (", 0, 0);
    if (s.startsWith('(')) return;
    ed->beginChange(QStringLiteral("Thesaurus"));
    ed->cursor().insertText(s);
    ed->endChange();
    ed->textEdited();
}

// ---------------- hyphenation (Liang) ----------------
// Hyphenation patterns live in text/hyphenation so layout can use them too.
struct Hyphenator {
    static Hyphenator &get() { static Hyphenator h; return h; }
    QVector<int> points(const QString &word) { return hyphenationPoints(word); }
};

int hyphenateStory(QTextDocument *doc)
{
    int n = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const QString text = b.text();
        QVector<QPair<int, QString>> words;
        auto it = wordRe().globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            if (m.captured().size() >= 6 && !m.captured().contains(QChar(0x00AD))) words << qMakePair(int(m.capturedStart()), m.captured());
        }
        for (int i = words.size() - 1; i >= 0; --i) {
            const QVector<int> pts = Hyphenator::get().points(words[i].second);
            for (int k = pts.size() - 1; k >= 0; --k) {
                QTextCursor c(doc);
                c.setPosition(b.position() + words[i].first + pts[k]);
                c.insertText(QString(QChar(0x00AD)));
                ++n;
            }
        }
    }
    return n;
}

} // namespace jp
