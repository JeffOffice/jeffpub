#include "app/help.h"

#include "app/i18n.h"
#include "app/icons.h"
#include "app/mainwindow.h"
#include "app/ribbon.h"

#include <QAction>
#include <QDesktopServices>
#include <QSet>
#include <algorithm>
#include <climits>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QRegularExpression>
#include <QScreen>
#include <QSysInfo>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QToolButton>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace jp {
namespace help {

std::function<bool(const QUrl &)> openUrl = [](const QUrl &u) { return QDesktopServices::openUrl(u); };

namespace {

QString readText(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// The folders to look in, the display language's first ("pt_BR", then "pt").
QStringList languageDirs()
{
    QStringList dirs;
    const QString lang = currentUiLanguage();
    if (!lang.isEmpty() && lang != QLatin1String("en")) {
        dirs << QStringLiteral(":/help/") + lang;
        if (lang.contains(QLatin1Char('_'))) dirs << QStringLiteral(":/help/") + lang.section(QLatin1Char('_'), 0, 0);
    }
    dirs << QStringLiteral(":/help/en");
    return dirs;
}

Topic parse(const QString &id, const QString &md)
{
    static const QRegularExpression title(QStringLiteral("^#\\s+(.+)$"), QRegularExpression::MultilineOption);
    static const QRegularExpression keywords(QStringLiteral("<!--\\s*keywords:(.*?)-->"), QRegularExpression::DotMatchesEverythingOption);
    Topic t;
    t.id = id;
    t.markdown = md;
    if (const auto m = title.match(md); m.hasMatch()) t.title = m.captured(1).trimmed();
    if (const auto m = keywords.match(md); m.hasMatch()) t.keywords = m.captured(1).simplified();
    return t;
}

// Lowercase words, for searching.
QStringList words(const QString &s)
{
    static const QRegularExpression split(QStringLiteral("[^\\w]+"), QRegularExpression::UseUnicodePropertiesOption);
    QStringList out = s.toLower().split(split, Qt::SkipEmptyParts);
    return out;
}

// Words a question carries that say nothing about the subject ("how do I
// add a text box"). English only; other languages search with every word.
bool isFiller(const QString &w)
{
    static const QSet<QString> filler = {QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("the"), QStringLiteral("to"),
                                         QStringLiteral("how"), QStringLiteral("do"), QStringLiteral("i"), QStringLiteral("can"),
                                         QStringLiteral("in"), QStringLiteral("of"), QStringLiteral("and"), QStringLiteral("my"),
                                         QStringLiteral("on"), QStringLiteral("for"), QStringLiteral("with"), QStringLiteral("is"),
                                         QStringLiteral("what"), QStringLiteral("where"), QStringLiteral("make"), QStringLiteral("use")};
    return filler.contains(w);
}

bool hasPrefix(const QStringList &tokens, const QString &w)
{
    for (const QString &t : tokens)
        if (t.startsWith(w)) return true;
    return false;
}

int countPrefix(const QStringList &tokens, const QString &w)
{
    int n = 0;
    for (const QString &t : tokens)
        if (t.startsWith(w)) ++n;
    return n;
}

struct Indexed {
    QString id, titleLower;
    QStringList title, keywords, body;
};

struct Cache {
    QString lang;
    QHash<QString, Topic> topics;
    QList<Indexed> index;
};

Cache &cache()
{
    static Cache c;
    if (c.lang != currentUiLanguage()) c = Cache{currentUiLanguage(), {}, {}};
    return c;
}

} // namespace

QStringList topicIds()
{
    QStringList ids;
    for (const QString &f : QDir(QStringLiteral(":/help/en")).entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name)) ids << f.chopped(3);
    return ids;
}

Topic topic(const QString &id)
{
    Cache &c = cache();
    if (auto it = c.topics.constFind(id); it != c.topics.constEnd()) return *it;
    Topic t;
    if (!id.isEmpty() && !id.contains(QLatin1Char('/')) && !id.contains(QLatin1Char('.')))
        for (const QString &dir : languageDirs()) {
            const QString md = readText(dir + QLatin1Char('/') + id + QStringLiteral(".md"));
            if (!md.isEmpty()) {
                t = parse(id, md);
                break;
            }
        }
    c.topics.insert(id, t);
    return t;
}

QString contextTopic(const QString &key)
{
    static const QJsonObject map = QJsonDocument::fromJson(readText(QStringLiteral(":/help/context.json")).toUtf8()).object();
    return map.value(key).toString();
}

QStringList searchTopics(const QString &query)
{
    Cache &c = cache();
    if (c.index.isEmpty())
        for (const QString &id : topicIds()) {
            if (id == QLatin1String("index")) continue;   // the contents lists everything
            const Topic t = topic(id);
            QString body = t.markdown;
            body.remove(QRegularExpression(QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption));
            body.remove(QRegularExpression(QStringLiteral("\\]\\([^)]*\\)")));   // link targets aren't words
            c.index << Indexed{id, t.title.toLower(), words(t.title), words(t.keywords), words(body)};
        }
    QStringList all = words(query), wanted;
    for (const QString &w : all)
        if (!isFiller(w)) wanted << w;
    if (wanted.isEmpty()) wanted = all;
    if (wanted.isEmpty()) return {};
    struct Scored { int matched, score; QString id; };
    QList<Scored> found;
    for (const Indexed &t : c.index) {
        int matched = 0, score = 0, titleHits = 0;
        for (const QString &w : wanted) {
            const bool inTitle = hasPrefix(t.title, w), inKeywords = hasPrefix(t.keywords, w);
            const int inBody = std::min(countPrefix(t.body, w), 5);
            if (inTitle || inKeywords || inBody) ++matched;
            titleHits += inTitle;
            score += (inTitle ? 10 : 0) + (inKeywords ? 6 : 0) + inBody;
            if (!t.title.isEmpty() && t.title.first().startsWith(w)) score += 4;   // the title's subject
        }
        // A title mostly about the words beats one that mentions them in passing
        // ("Print a publication" over "PDF files and print shops" for "print").
        if (!t.title.isEmpty()) score += 6 * titleHits / int(t.title.size());
        if (t.titleLower.contains(query.trimmed().toLower())) score += 20;
        if (matched) found << Scored{matched, score, t.id};
    }
    // Topics with every word first; failing that, those with the most.
    int best = 0;
    for (const auto &s : found) best = std::max(best, s.matched);
    QList<Scored> kept;
    for (const auto &s : found)
        if (s.matched == best && (best == wanted.size() || best * 2 >= wanted.size())) kept << s;
    std::stable_sort(kept.begin(), kept.end(), [](const Scored &a, const Scored &b) { return a.score > b.score; });
    QStringList ids;
    for (const auto &s : kept) ids << s.id;
    return ids;
}

namespace {

// Where each command sits on the ribbon: "Home > Font", or with the menu
// that holds it, "Insert > Text > Text Art".
QHash<QString, QString> commandPlaces(MainWindow *win, QHash<QString, int> *order = nullptr)
{
    QHash<QString, QString> places;
    Ribbon *r = win ? win->ribbon() : nullptr;
    if (!r) return places;
    int n = 0;
    std::function<void(QMenu *, const QString &)> walkMenu = [&](QMenu *m, const QString &where) {
        for (QAction *a : m->actions()) {
            if (a->menu()) walkMenu(a->menu(), where + QStringLiteral(" > ") + QString(a->text()).remove(QLatin1Char('&')));
            else if (!a->objectName().isEmpty() && !places.contains(a->objectName())) {
                places.insert(a->objectName(), where);
                if (order) order->insert(a->objectName(), n++);
            }
        }
    };
    for (int i = 0; i < r->tabCount(); ++i) {
        RibbonTab *page = r->tabPage(i);
        if (!page) continue;
        for (RibbonGroup *g : page->groups()) {
            const QString where = r->tabTitle(i) + QStringLiteral(" > ") + g->title();
            for (QToolButton *b : g->findChildren<QToolButton *>()) {
                if (b == g->launcher()) continue;
                QAction *a = b->defaultAction();
                if (a && !a->objectName().isEmpty() && !places.contains(a->objectName())) {
                    places.insert(a->objectName(), where);
                    if (order) order->insert(a->objectName(), n++);
                }
                if (QMenu *m = b->menu()) walkMenu(m, where + QStringLiteral(" > ") + QString(b->text()).remove(QLatin1Char('&')).simplified());
            }
        }
    }
    for (QToolButton *b : r->quickAccessButtons())
        if (QAction *a = b->defaultAction(); a && !places.contains(a->objectName())) {
            places.insert(a->objectName(), QCoreApplication::translate("Help", "Quick Access Toolbar"));
            if (order) order->insert(a->objectName(), n++);
        }
    return places;
}

QString commandName(const QAction *a)
{
    QString s = a->text();
    s.remove(QLatin1Char('&'));
    if (s.endsWith(QLatin1String("..."))) s.chop(3);
    return s.simplified();
}

QString cell(QString s) { return s.replace(QLatin1Char('|'), QStringLiteral("\\|")); }

// Every command with a keyboard shortcut, as a Markdown table in ribbon order.
QString shortcutTable(MainWindow *win)
{
    QHash<QString, int> order;
    const QHash<QString, QString> places = commandPlaces(win, &order);
    struct Row { int order; QString name, keys, where; };
    QList<Row> rows;
    QSet<QString> seen;
    for (const QString &id : win->actionIds()) {
        QAction *a = win->act(id);
        if (!a || a->shortcuts().isEmpty()) continue;
        QStringList keys;
        for (const QKeySequence &k : a->shortcuts()) keys << k.toString(QKeySequence::NativeText);
        const QString name = commandName(a), k = keys.join(QStringLiteral(", "));
        if (name.isEmpty() || seen.contains(name + k)) continue;
        seen.insert(name + k);
        rows << Row{order.value(id, INT_MAX), name, k, places.value(id)};
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.order != b.order ? a.order < b.order : a.name < b.name; });
    QString md = QStringLiteral("| %1 | %2 | %3 |\n|---|---|---|\n")
                     .arg(QCoreApplication::translate("Help", "Command"), QCoreApplication::translate("Help", "Keys"),
                          QCoreApplication::translate("Help", "On the ribbon"));
    for (const Row &r : rows) md += QStringLiteral("| %1 | %2 | %3 |\n").arg(cell(r.name), cell(r.keys), cell(r.where));
    return md;
}

} // namespace

QString displayMarkdown(const Topic &t, MainWindow *win)
{
    QString md = t.markdown;
    if (win && md.contains(QLatin1String("<!-- shortcuts -->"))) md.replace(QLatin1String("<!-- shortcuts -->"), shortcutTable(win));
    md.remove(QRegularExpression(QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption));
    return md;
}

QUrl supportUrl()
{
    QUrl u(QStringLiteral("https://github.com/JeffOffice/jeffpub/issues/new"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("labels"), QStringLiteral("bug"));
    q.addQueryItem(QStringLiteral("body"), QStringLiteral("**What happened?**\n\n\n**What did you do just before it happened?**\n\n\n"
                                                          "JeffPub %1 on %2 (%3)\n")
                                               .arg(QStringLiteral(JP_VERSION), QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture()));
    u.setQuery(q);
    return u;
}

QUrl feedbackUrl()
{
    QUrl u(QStringLiteral("https://github.com/JeffOffice/jeffpub/issues/new"));
    QUrlQuery q;
    // GitHub applies the label only for the project's own members, so the
    // title says it too.
    q.addQueryItem(QStringLiteral("labels"), QStringLiteral("enhancement"));
    q.addQueryItem(QStringLiteral("title"), QStringLiteral("Idea: "));
    q.addQueryItem(QStringLiteral("body"), QStringLiteral("**Your idea**\n\n\n**How it would help you**\n\n\nJeffPub %1\n").arg(QStringLiteral(JP_VERSION)));
    u.setQuery(q);
    return u;
}

QUrl whatsNewUrl() { return QUrl(QStringLiteral("https://github.com/JeffOffice/jeffpub/releases/tag/v" JP_VERSION)); }

} // namespace help

// ---------------- the view ----------------
namespace {
enum { KindRole = Qt::UserRole, IdRole };
constexpr int kTopic = 1, kCommand = 2;

QToolButton *navButton(const QString &iconName, const QString &name, const QString &tip, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setAccessibleName(name);
    b->setFocusPolicy(Qt::TabFocus);
    return b;
}
} // namespace

HelpView::HelpView(MainWindow *win, QWidget *parent, bool commands) : QWidget(parent), m_win(win), m_commands(commands && win)
{
    setObjectName(QStringLiteral("jpHelpView"));
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    auto *bar = new QHBoxLayout();
    m_back = navButton(QStringLiteral("arrow-left"), tr("Back"), tr("Back to the topic before"), this);
    m_forward = navButton(QStringLiteral("arrow-right"), tr("Forward"), tr("Forward again"), this);
    m_home = navButton(QStringLiteral("house"), tr("Contents"), tr("Help's contents"), this);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("jpHelpSearch"));
    m_search->setPlaceholderText(m_commands ? tr("Search Help and commands") : tr("Search Help"));
    m_search->setAccessibleName(tr("Search Help"));
    m_search->setAccessibleDescription(m_commands ? tr("Type what you want to do. Down goes to the topics and commands found, and Enter opens or runs the first.")
                                           : tr("Type what you want to do. Down goes to the topics found, and Enter opens the first."));
    m_search->setClearButtonEnabled(true);
    m_search->installEventFilter(this);
    bar->addWidget(m_back);
    bar->addWidget(m_forward);
    bar->addWidget(m_home);
    bar->addWidget(m_search, 1);
    v->addLayout(bar);
    m_results = new QListWidget(this);
    m_results->setObjectName(QStringLiteral("jpHelpResults"));
    m_results->setAccessibleName(tr("Search results"));
    m_results->setWordWrap(true);
    m_results->hide();
    m_results->installEventFilter(this);
    v->addWidget(m_results, 1);
    m_browser = new QTextBrowser(this);
    m_browser->setObjectName(QStringLiteral("jpHelpTopic"));
    m_browser->setAccessibleName(tr("Help topic"));
    m_browser->setOpenLinks(false);
    m_browser->setFrameShape(QFrame::NoFrame);
    m_browser->document()->setDocumentMargin(10);
    v->addWidget(m_browser, 1);

    connect(m_back, &QToolButton::clicked, this, [this] {
        if (m_history.isEmpty()) return;
        m_ahead.prepend(m_current);
        go(m_history.takeLast(), false);
    });
    connect(m_forward, &QToolButton::clicked, this, [this] {
        if (m_ahead.isEmpty()) return;
        m_history << m_current;
        go(m_ahead.takeFirst(), false);
    });
    connect(m_home, &QToolButton::clicked, this, [this] { showTopic(QStringLiteral("index")); });
    connect(m_search, &QLineEdit::textChanged, this, &HelpView::showResults);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        for (int i = 0; i < m_results->count(); ++i)
            if (m_results->item(i)->flags() & Qt::ItemIsEnabled && m_results->item(i)->data(KindRole).toInt()) return activate(m_results->item(i));
    });
    // A click opens or runs a result, and Return (eventFilter); not
    // itemActivated too, which a double-click or a one-click desktop would
    // send as well, running a command twice.
    connect(m_results, &QListWidget::itemClicked, this, &HelpView::activate);
    connect(m_browser, &QTextBrowser::anchorClicked, this, [this](const QUrl &u) {
        if (u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("https") || u.scheme() == QLatin1String("mailto")) {
            help::openUrl(u);
            return;
        }
        QString id = u.path();
        if (id.startsWith(QLatin1String("./"))) id = id.mid(2);
        if (id.endsWith(QLatin1String(".md"))) id.chop(3);
        if (!id.isEmpty()) showTopic(id);
    });
    updateButtons();
}

void HelpView::showTopic(const QString &id)
{
    if (!m_search->text().isEmpty()) {
        const QSignalBlocker b(m_search);
        m_search->clear();
    }
    m_results->hide();
    m_browser->show();
    if (id == m_current) return;
    if (!m_current.isEmpty()) m_history << m_current;
    m_ahead.clear();
    go(id, true);
}

void HelpView::go(const QString &id, bool)
{
    m_current = id;
    const help::Topic t = help::topic(id);
    if (t.isNull()) m_browser->setMarkdown(tr("# Not found\n\nThere is no help topic called \"%1\".").arg(id));
    else m_browser->document()->setMarkdown(help::displayMarkdown(t, m_win), QTextDocument::MarkdownDialectGitHub);
    // Headings sized for a narrow pane (Markdown's are a page's), and space
    // above each section's heading so the sections read apart.
    const double base = QFontInfo(m_browser->font()).pointSizeF();   // the font may be set in pixels
    for (QTextBlock b = m_browser->document()->begin(); b.isValid(); b = b.next()) {
        const int level = b.blockFormat().headingLevel();
        if (!level) continue;
        QTextCursor c(b);
        QTextBlockFormat bf = b.blockFormat();
        bf.setTopMargin(level == 1 ? 0 : base);
        bf.setBottomMargin(base * 0.4);
        c.setBlockFormat(bf);
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            QTextCharFormat cf = f.charFormat();
            cf.clearProperty(QTextFormat::FontSizeAdjustment);   // the importer's relative step
            cf.setFontPointSize(base * (level == 1 ? 1.5 : level == 2 ? 1.2 : 1.05));
            cf.setFontWeight(level == 1 ? QFont::Bold : QFont::DemiBold);
            c.setPosition(f.position());
            c.setPosition(f.position() + f.length(), QTextCursor::KeepAnchor);
            c.setCharFormat(cf);
        }
    }
    m_browser->setAccessibleDescription(t.title);
    m_results->hide();
    m_browser->show();
    updateButtons();
}

void HelpView::updateButtons()
{
    m_back->setEnabled(!m_history.isEmpty());
    m_forward->setEnabled(!m_ahead.isEmpty());
}

void HelpView::focusTopic() { m_browser->setFocus(Qt::OtherFocusReason); }

void HelpView::search(const QString &text) { m_search->setText(text); }

void HelpView::showResults(const QString &text)
{
    m_results->clear();
    if (text.trimmed().isEmpty()) {
        m_results->hide();
        m_browser->show();
        return;
    }
    auto heading = [this](const QString &s) {
        auto *it = new QListWidgetItem(s, m_results);
        QFont f = m_results->font();
        f.setBold(true);
        it->setFont(f);
        it->setFlags(Qt::NoItemFlags);
        it->setData(Qt::AccessibleTextRole, s);
    };
    const QStringList topics = help::searchTopics(text);
    if (!topics.isEmpty()) heading(tr("Help topics"));
    for (const QString &id : topics.mid(0, 10)) {
        auto *it = new QListWidgetItem(icon(QStringLiteral("book-open")), help::topic(id).title, m_results);
        it->setData(KindRole, kTopic);
        it->setData(IdRole, id);
    }
    // Commands, matched by their names and where they are on the ribbon.
    if (m_commands) {
        const QHash<QString, QString> places = help::commandPlaces(m_win);
        QStringList all = help::words(text), wanted;
        for (const QString &w : all)
            if (!help::isFiller(w)) wanted << w;
        if (wanted.isEmpty()) wanted = all;
        struct Found { int score; QAction *a; QString name, where; };
        QList<Found> found;
        QSet<QString> seen;
        for (const QString &id : m_win->actionIds()) {
            QAction *a = m_win->act(id);
            if (!a || a->isSeparator()) continue;
            const QString name = help::commandName(a);
            if (name.isEmpty()) continue;
            const QString where = places.value(id);
            const QStringList nameWords = help::words(name), whereWords = help::words(where);
            int score = 0;
            bool every = true;
            for (const QString &w : wanted) {
                const bool inName = help::hasPrefix(nameWords, w), inWhere = help::hasPrefix(whereWords, w);
                if (!inName && !inWhere) { every = false; break; }
                score += inName ? 10 : 3;
            }
            if (!every || seen.contains(name + where)) continue;
            seen.insert(name + where);
            if (name.toLower().contains(text.trimmed().toLower())) score += 20;
            if (!where.isEmpty()) score += 2;   // on the ribbon: findable again
            found << Found{score, a, name, where};
        }
        std::stable_sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.score > b.score; });
        if (!found.isEmpty()) heading(tr("Commands"));
        for (const Found &f : found.mid(0, 10)) {
            const bool on = f.a->isEnabled();
            const QString label = f.where.isEmpty() ? f.name : tr("%1  (%2)").arg(f.name, f.where);
            auto *it = new QListWidgetItem(f.a->icon(), on ? label : tr("%1, not available now").arg(label), m_results);
            it->setData(KindRole, kCommand);
            it->setData(IdRole, f.a->objectName());
            it->setToolTip(f.a->toolTip());
            if (!on) it->setFlags(it->flags() & ~Qt::ItemIsEnabled);
        }
    }
    if (m_results->count() == 0) {
        auto *it = new QListWidgetItem(tr("Nothing found. Try other words, or open the contents."), m_results);
        it->setFlags(Qt::NoItemFlags);
    }
    m_browser->hide();
    m_results->show();
}

void HelpView::activate(QListWidgetItem *it)
{
    if (!it || !(it->flags() & Qt::ItemIsEnabled)) return;
    const QString id = it->data(IdRole).toString();
    if (it->data(KindRole).toInt() == kTopic) {
        showTopic(id);
        focusTopic();
    } else if (it->data(KindRole).toInt() == kCommand && m_commands) {
        if (QAction *a = m_win->act(id); a && a->isEnabled()) a->trigger();
    }
}

bool HelpView::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_search && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Down && m_results->isVisible()) {
            for (int i = 0; i < m_results->count(); ++i)
                if (m_results->item(i)->flags() & Qt::ItemIsEnabled) {
                    m_results->setCurrentRow(i);
                    break;
                }
            m_results->setFocus(Qt::TabFocusReason);
            return true;
        }
        if (ke->key() == Qt::Key_Escape && !m_search->text().isEmpty()) {
            m_search->clear();
            return true;
        }
    }
    // Return opens or runs the chosen result everywhere (a Mac list only
    // activates its rows with Cmd+O).
    if (o == m_results && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && m_results->currentItem()) {
            activate(m_results->currentItem());
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

// ---------------- over a dialog ----------------
HelpWindow::HelpWindow(MainWindow *win, QWidget *owner) : QDialog(owner)
{
    setWindowTitle(tr("Help"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    // Commands can't run while a dialog holds the window, so this one only
    // finds topics (the backstage is no dialog: there they can).
    m_view = new HelpView(win, this, !qobject_cast<QDialog *>(owner));
    v->addWidget(m_view);
    resize(440, 600);
    // Beside its owner when there's room, else over its right edge.
    const QRect o = owner->window()->frameGeometry();
    const QRect avail = owner->screen() ? owner->screen()->availableGeometry() : QRect();
    QPoint at(o.right() + 8, o.top());
    if (avail.isValid() && at.x() + width() > avail.right()) at.setX(std::max(avail.left(), o.right() - width()));
    move(at);
}

} // namespace jp
