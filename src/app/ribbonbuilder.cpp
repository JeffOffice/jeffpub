#include "app/ribbonbuilder.h"

#include "app/icons.h"
#include "app/ribbon.h"

#include <QAction>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QRegularExpression>
#include <QToolButton>
#include <QAbstractButton>
#include <QComboBox>
#include <QAbstractSpinBox>

namespace jp {

namespace {

// Everything the file shows goes through the "Ribbon" translation context.
QString tr(const QString &s) { return QCoreApplication::translate("Ribbon", s.toUtf8().constData()); }

class Builder {
public:
    Builder(Ribbon *r, const RibbonParts &parts, QObject *owner)
        : m_r(r), m_p(parts), m_owner(owner)
    {
        // Menus hang off the window, as they did when the code built them.
        m_menuParent = qobject_cast<QWidget *>(owner);
        if (!m_menuParent) m_menuParent = r->window();
    }

    void build(const QJsonObject &root)
    {
        allow(root, {"fileKeytip", "quickAccess", "templates", "tabs"}, QStringLiteral("the file"));
        m_r->setFileKeytip(keytipOf(root, "fileKeytip"));
        m_templates = root.value(QLatin1String("templates")).toObject();
        for (const QJsonValue &v : root.value(QLatin1String("quickAccess")).toArray()) quickAccess(v);
        const QJsonArray tabs = root.value(QLatin1String("tabs")).toArray();
        if (tabs.isEmpty()) problem(QStringLiteral("the file has no tabs"));
        for (const QJsonValue &v : tabs) tab(v.toObject());
    }

    QStringList problems() const { return m_problems; }

private:
    // ---- reporting ----
    void problem(const QString &what)
    {
        const QString line = m_where.isEmpty() ? what : QStringLiteral("%1 (in %2)").arg(what, m_where);
        if (!m_problems.contains(line)) m_problems << line;
    }

    // A misspelled key would silently do nothing, so unknown keys are problems.
    void allow(const QJsonObject &o, std::initializer_list<const char *> keys, const QString &what)
    {
        for (auto it = o.begin(); it != o.end(); ++it) {
            bool known = false;
            for (const char *k : keys) known = known || it.key() == QLatin1String(k);
            if (!known) problem(QStringLiteral("unknown key \"%1\" in %2").arg(it.key(), what));
        }
    }

    bool flag(const QJsonObject &o, const char *key)
    {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined()) return false;
        if (!v.isBool()) problem(QStringLiteral("\"%1\" must be true or false").arg(QLatin1String(key)));
        return v.toBool();
    }

    QString str(const QJsonObject &o, const char *key)
    {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined()) return QString();
        if (!v.isString()) problem(QStringLiteral("\"%1\" must be a string").arg(QLatin1String(key)));
        return v.toString();
    }

    QString keytipOf(const QJsonObject &o, const char *key = "keytip")
    {
        static const QRegularExpression valid(QStringLiteral("^[A-Z0-9]{1,2}$"));
        const QString k = str(o, key);
        if (!k.isEmpty() && !valid.match(k).hasMatch()) problem(QStringLiteral("KeyTip \"%1\" isn't one or two of A-Z and 0-9").arg(k));
        return k;
    }

    void tagged(QObject *w, const QJsonObject &o)
    {
        const QString k = keytipOf(o);
        if (w && !k.isEmpty()) setKeytip(w, k);
    }

    // ---- commands, menus, widgets ----
    QAction *command(const QJsonValue &v)
    {
        if (v.isString()) {
            const QString id = v.toString();
            if (QAction *a = m_inline.value(id)) return a;
            if (QAction *a = m_p.action ? m_p.action(id) : nullptr) return a;
            problem(QStringLiteral("unknown command \"%1\"").arg(id));
            return nullptr;
        }
        if (!v.isObject()) {
            problem(QStringLiteral("a command is an id or an {id, text, icon} object"));
            return nullptr;
        }
        const QJsonObject o = v.toObject();
        allow(o, {"id", "text", "icon"}, QStringLiteral("a command"));
        const QString id = str(o, "id"), text = str(o, "text"), iconName = str(o, "icon");
        if (id.isEmpty() || text.isEmpty()) {
            problem(QStringLiteral("a ribbon-only command needs an id and text"));
            return nullptr;
        }
        if (!iconName.isEmpty() && !QFile::exists(QStringLiteral(":/icons/%1.svg").arg(iconName)))
            problem(QStringLiteral("unknown icon \"%1\"").arg(iconName));
        if (QAction *a = m_inline.value(id)) {
            // A template repeats its definitions; any other disagreement is a typo.
            if (a->text() != tr(text)) problem(QStringLiteral("command \"%1\" is defined twice with different text").arg(id));
            return a;
        }
        if (m_p.action && m_p.action(id)) {
            problem(QStringLiteral("command \"%1\" is already a command; refer to it by id").arg(id));
            return nullptr;
        }
        auto *a = new QAction(tr(text), m_owner);
        if (!iconName.isEmpty()) a->setIcon(icon(iconName));
        a->setObjectName(id);   // the command's id, as for every other command
        if (const auto fn = m_p.triggers.value(id)) QObject::connect(a, &QAction::triggered, a, [fn] { fn(); });
        m_inline.insert(id, a);
        return a;
    }

    QMenu *menu(const QJsonValue &spec, QWidget *parent)
    {
        if (spec.isUndefined()) return nullptr;
        if (spec.isString()) {
            const QString s = spec.toString();
            if (!s.startsWith(QLatin1Char('@'))) {
                problem(QStringLiteral("a menu is a list, or \"@name\" for one built in code (not \"%1\")").arg(s));
                return nullptr;
            }
            const QString name = s.mid(1);
            const auto it = m_p.menus.constFind(name);
            if (it == m_p.menus.constEnd()) {
                problem(QStringLiteral("unknown menu \"%1\"").arg(name));
                return nullptr;
            }
            QMenu *m = (*it)(parent);
            if (!m) problem(QStringLiteral("menu \"%1\" was not created").arg(name));
            return m;
        }
        if (!spec.isArray()) {
            problem(QStringLiteral("a menu is a list, or \"@name\" for one built in code"));
            return nullptr;
        }
        auto *m = new QMenu(parent);
        fill(m, spec.toArray());
        return m;
    }

    void fill(QMenu *m, const QJsonArray &entries)
    {
        for (const QJsonValue &e : entries) {
            if (e.isString() && e.toString() == QLatin1String("-")) {
                m->addSeparator();
            } else if (e.isString()) {
                if (QAction *a = command(e)) m->addAction(a);
            } else if (e.isObject() && e.toObject().contains(QLatin1String("section"))) {
                const QJsonObject o = e.toObject();
                allow(o, {"section"}, QStringLiteral("a menu section"));
                m->addSection(tr(str(o, "section")));
            } else if (e.isObject() && e.toObject().contains(QLatin1String("submenu"))) {
                const QJsonObject o = e.toObject();
                allow(o, {"submenu", "icon", "menu"}, QStringLiteral("a submenu"));
                const QString title = tr(str(o, "submenu")), iconName = str(o, "icon");
                if (!iconName.isEmpty() && !QFile::exists(QStringLiteral(":/icons/%1.svg").arg(iconName)))
                    problem(QStringLiteral("unknown icon \"%1\"").arg(iconName));
                QMenu *sub = iconName.isEmpty() ? m->addMenu(title) : m->addMenu(icon(iconName), title);
                if (!o.value(QLatin1String("menu")).isArray()) problem(QStringLiteral("submenu \"%1\" needs a \"menu\" list").arg(title));
                else fill(sub, o.value(QLatin1String("menu")).toArray());
            } else if (QAction *a = command(e)) {
                m->addAction(a);
            }
        }
    }

    QWidget *widget(const QString &name)
    {
        const auto it = m_p.widgets.constFind(name);
        if (it == m_p.widgets.constEnd()) {
            problem(QStringLiteral("unknown widget \"%1\"").arg(name));
            return nullptr;
        }
        QWidget *w = (*it)();
        if (!w) problem(QStringLiteral("widget \"%1\" was not created").arg(name));
        return w;
    }

    // The menu of an item that has one, and whether it splits.
    QMenu *itemMenu(const QJsonObject &o, bool *split)
    {
        *split = flag(o, "split");
        QMenu *m = menu(o.value(QLatin1String("menu")), m_menuParent);
        if (*split && o.value(QLatin1String("menu")).isUndefined()) problem(QStringLiteral("\"split\" needs a \"menu\""));
        return m;
    }

    // ---- layout ----
    void quickAccess(const QJsonValue &v)
    {
        QJsonValue cmd = v;
        QString k;
        if (v.isObject()) {
            allow(v.toObject(), {"cmd", "keytip"}, QStringLiteral("a quick access button"));
            cmd = v.toObject().value(QLatin1String("cmd"));
            k = keytipOf(v.toObject());
        }
        QAction *a = command(cmd);
        if (!a) return;
        QToolButton *b = m_r->addQuickAccess(a);
        if (!k.isEmpty()) setKeytip(b, k);
    }

    void tab(const QJsonObject &o)
    {
        allow(o, {"name", "keytip", "context", "color", "groups"}, QStringLiteral("a tab"));
        const QString name = str(o, "name"), context = str(o, "context"), colorName = str(o, "color");
        if (name.isEmpty()) {
            problem(QStringLiteral("a tab needs a name"));
            return;
        }
        m_where = name;
        if (m_r->tab(name)) problem(QStringLiteral("two tabs are named \"%1\"").arg(name));
        QColor color;
        if (!context.isEmpty()) {
            color = m_p.colors.value(colorName);
            if (!color.isValid()) problem(QStringLiteral("unknown color \"%1\"").arg(colorName));
        }
        RibbonTab *t = m_r->addTab(name, context, color, tr(name));
        m_r->setTabKeytip(t, keytipOf(o));
        const QJsonArray groups = o.value(QLatin1String("groups")).toArray();
        if (groups.isEmpty()) problem(QStringLiteral("a tab needs groups"));
        for (const QJsonValue &g : groups) group(t, g.toObject());
        t->finish();
        m_where.clear();
    }

    void group(RibbonTab *t, QJsonObject o)
    {
        QString k;
        if (o.contains(QLatin1String("use"))) {
            allow(o, {"use", "keytip"}, QStringLiteral("a group that uses a template"));
            const QString use = str(o, "use");
            k = keytipOf(o);
            const QJsonObject tpl = m_templates.value(use).toObject();
            if (tpl.isEmpty()) {
                problem(QStringLiteral("unknown template \"%1\"").arg(use));
                return;
            }
            o = tpl;
            if (!k.isEmpty()) o.insert(QStringLiteral("keytip"), k);
        }
        allow(o, {"name", "keytip", "launcher", "launcherTip", "launcherKeytip", "items"}, QStringLiteral("a group"));
        const QString name = str(o, "name");
        if (name.isEmpty()) {
            problem(QStringLiteral("a group needs a name"));
            return;
        }
        const QString tabName = m_where.section(QLatin1String(" / "), 0, 0);
        m_where = tabName + QStringLiteral(" / ") + name;
        RibbonGroup *g = t->addGroup(tr(name));
        tagged(g, o);
        for (const QJsonValue &it : o.value(QLatin1String("items")).toArray()) item(g, it.toObject());
        if (o.contains(QLatin1String("launcher"))) {
            const QString l = str(o, "launcher");
            const auto fn = m_p.launchers.value(l);
            if (!fn) {
                problem(QStringLiteral("unknown launcher \"%1\"").arg(l));
            } else {
                const QString tip = str(o, "launcherTip");
                g->setLauncher(fn, tip.isEmpty() ? QString() : tr(tip));
                const QString lk = keytipOf(o, "launcherKeytip");
                if (!lk.isEmpty()) setKeytip(g->launcher(), lk);
            }
        } else if (o.contains(QLatin1String("launcherTip")) || o.contains(QLatin1String("launcherKeytip"))) {
            problem(QStringLiteral("a launcher tip or KeyTip needs a \"launcher\""));
        }
        m_where = tabName;
    }

    void item(RibbonGroup *g, const QJsonObject &o)
    {
        bool split = false;
        if (o.contains(QLatin1String("large"))) {
            allow(o, {"large", "menu", "split", "keytip"}, QStringLiteral("a large button"));
            QMenu *m = itemMenu(o, &split);
            if (QAction *a = command(o.value(QLatin1String("large")))) tagged(g->addLarge(a, m, split), o);
        } else if (o.contains(QLatin1String("small"))) {
            allow(o, {"small", "menu", "split", "iconOnly", "keytip"}, QStringLiteral("a small button"));
            QMenu *m = itemMenu(o, &split);
            const bool iconOnly = flag(o, "iconOnly");
            if (QAction *a = command(o.value(QLatin1String("small")))) tagged(g->addSmall(a, m, split, iconOnly), o);
        } else if (o.contains(QLatin1String("row"))) {
            allow(o, {"row"}, QStringLiteral("a row"));
            QList<QWidget *> widgets;
            for (const QJsonValue &v : o.value(QLatin1String("row")).toArray())
                if (QWidget *w = rowItem(v.toObject())) widgets << w;
            g->addRow(widgets);
        } else if (o.contains(QLatin1String("widget"))) {
            allow(o, {"widget", "keytip"}, QStringLiteral("a widget"));
            if (QWidget *w = widget(str(o, "widget"))) {
                g->addWidget(w);
                tagged(w, o);
            }
        } else if (o.contains(QLatin1String("separator"))) {
            allow(o, {"separator"}, QStringLiteral("a separator"));
            g->addSeparator();
        } else if (o.contains(QLatin1String("column"))) {
            allow(o, {"column"}, QStringLiteral("a column"));
            g->beginColumn();
        } else {
            problem(QStringLiteral("an item is large, small, row, widget, separator or column"));
        }
    }

    QWidget *rowItem(const QJsonObject &o)
    {
        QWidget *w = nullptr;
        bool split = false;
        if (o.contains(QLatin1String("icon")) || o.contains(QLatin1String("button"))) {
            const bool iconOnly = o.contains(QLatin1String("icon"));
            allow(o, {iconOnly ? "icon" : "button", "menu", "split", "keytip"}, QStringLiteral("a row button"));
            QMenu *m = itemMenu(o, &split);
            QAction *a = command(o.value(QLatin1String(iconOnly ? "icon" : "button")));
            if (!a) return nullptr;
            QToolButton *b = ribbonButton(a, false, nullptr);
            if (iconOnly) b->setToolButtonStyle(Qt::ToolButtonIconOnly);
            if (m) {
                b->setMenu(m);
                b->setPopupMode(split ? QToolButton::MenuButtonPopup : QToolButton::InstantPopup);
            }
            w = b;
        } else if (o.contains(QLatin1String("widget"))) {
            allow(o, {"widget", "keytip"}, QStringLiteral("a row widget"));
            w = widget(str(o, "widget"));
        } else if (o.contains(QLatin1String("dropdown"))) {
            allow(o, {"dropdown", "menu", "keytip"}, QStringLiteral("a dropdown"));
            const QJsonObject d = o.value(QLatin1String("dropdown")).toObject();
            allow(d, {"icon", "tip"}, QStringLiteral("a dropdown"));
            const QString iconName = str(d, "icon"), tip = str(d, "tip");
            if (!iconName.isEmpty() && !QFile::exists(QStringLiteral(":/icons/%1.svg").arg(iconName)))
                problem(QStringLiteral("unknown icon \"%1\"").arg(iconName));
            auto *b = new QToolButton();
            b->setIcon(icon(iconName));
            b->setAutoRaise(true);
            b->setPopupMode(QToolButton::InstantPopup);
            if (QMenu *m = menu(o.value(QLatin1String("menu")), b)) b->setMenu(m);
            else problem(QStringLiteral("a dropdown needs a menu"));
            if (tip.isEmpty()) problem(QStringLiteral("a dropdown needs a tip (its name for screen readers)"));
            b->setToolTip(tr(tip));
            b->setAccessibleName(tr(tip));
            w = b;
        } else if (o.contains(QLatin1String("label"))) {
            allow(o, {"label"}, QStringLiteral("a label"));
            return new QLabel(tr(str(o, "label")));   // plain text: no KeyTip
        } else {
            problem(QStringLiteral("a row item is icon, button, widget, dropdown or label"));
            return nullptr;
        }
        tagged(w, o);
        return w;
    }

    Ribbon *m_r;
    const RibbonParts &m_p;
    QObject *m_owner;
    QWidget *m_menuParent = nullptr;
    QJsonObject m_templates;
    QHash<QString, QAction *> m_inline;   // commands the file made, by id
    QStringList m_problems;
    QString m_where;                      // "Home / Font", for messages
};

} // namespace

// A control with no name of its own for screen readers (a spin box, an
// icon-only dropdown) takes its tooltip's, without the shortcut.
static void nameUnnamedControls(Ribbon *r)
{
    static const QRegularExpression shortcut(QStringLiteral("\\s*\\([^()]*\\)$"));
    for (QWidget *w : r->findChildren<QWidget *>()) {
        if (!w->accessibleName().isEmpty() || w->toolTip().isEmpty()) continue;
        auto *b = qobject_cast<QAbstractButton *>(w);
        if (b && !b->text().isEmpty()) continue;
        if (!b && !qobject_cast<QAbstractSpinBox *>(w) && !qobject_cast<QComboBox *>(w)) continue;
        w->setAccessibleName(w->toolTip().remove(shortcut).remove(QLatin1Char('&')));
    }
}

bool buildRibbon(Ribbon *r, const QByteArray &json, const RibbonParts &parts, QObject *actionOwner, QString *error)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (doc.isNull() || !doc.isObject()) {
        if (error) *error = QStringLiteral("ribbon file isn't valid JSON: %1 (at byte %2)").arg(pe.errorString()).arg(pe.offset);
        return false;
    }
    Builder b(r, parts, actionOwner);
    b.build(doc.object());
    nameUnnamedControls(r);
    if (b.problems().isEmpty()) return true;
    if (error) *error = b.problems().join(QLatin1Char('\n'));
    return false;
}

} // namespace jp
