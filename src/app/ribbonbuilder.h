#pragma once
// Builds the ribbon from resources/ribbon.json. The file says what is on the
// ribbon and where; C++ says what the controls do. That keeps the layout (and
// the KeyTips and the strings to translate) in one readable place, while the
// behavior stays in named factories that the main window registers.
//
//   {
//     "fileKeytip": "F",                          the File button's KeyTip
//     "quickAccess": [ {"cmd": "file.save", "keytip": "1"}, ... ],   (a bare "id" works too)
//     "templates": { "Arrange": <group>, ... },   groups several tabs share
//     "tabs": [ <tab>, ... ]
//   }
//
//   <tab>   {"name": "Home", "keytip": "H", "groups": [ <group> | {"use": "Arrange"} ... ],
//            "context": "Text Box Tools", "color": "textBox"}      context and color: contextual tabs
//            A tab's name is also how code finds it; its translation is what the header shows.
//            A context is an identifier that code shows and hides tabs by (never displayed),
//            and a color is a name from the parts' `colors`.
//   <group> {"name": "Clipboard", "keytip": "ZC", "items": [ <item> ... ],
//            "launcher": "font", "launcherTip": "Font", "launcherKeytip": "FN"}
//            {"use": "Arrange", "keytip": "ZA"} takes a template; "keytip" there replaces its own.
//
//   <item> maps one to one onto a RibbonGroup method, so the layout is what the calls would make:
//     {"large": <cmd>, "menu": <menu>, "split": true, "keytip": "V"}          addLarge
//     {"small": <cmd>, "menu": <menu>, "split": true, "iconOnly": true, ...}  addSmall
//     {"row": [ <rowItem> ... ]}                                              addRow
//     {"widget": "fontCombo", "keytip": "FF"}                                 addWidget (its own column)
//     {"separator": true}                                                     addSeparator
//     {"column": true}                                                        beginColumn
//   <rowItem>
//     {"icon": <cmd>, "menu": ..., "split": ..., "keytip": ...}   small button showing only its icon
//     {"button": <cmd>, "menu": ..., "split": ..., "keytip": ...} small button with its text
//     {"widget": "name", "keytip": ...}                           a control from the main window
//     {"dropdown": {"icon": "chevron-down", "tip": "..."}, "menu": <menu>, "keytip": ...}
//                                                                  a bare menu button (no command)
//     {"label": "Height"}                                         plain text, no KeyTip
//
//   <cmd>   "edit.paste": an existing command (the parts' `action`), or
//           {"id": "ribbon.changeCase", "text": "Change Case", "icon": "case-sensitive"}: a command
//           that only exists on the ribbon, usually to open a menu. The loader creates it once
//           (objectName = id) and every later use of the id shares it. What it does when
//           clicked, if anything, is the parts' `triggers` entry with that id.
//   <menu>  [ "edit.paste",                          a command
//             "-",                                   a separator
//             {"section": "Weight"},                 a separator with a heading
//             {"submenu": "Shadow", "icon": "square", "menu": <menu>} ... ]
//           or "@name": a menu built in code (dynamic menus, or ones holding widgets).
//
// A KeyTip is one or two characters, A-Z or 0-9. Every user-visible string in the file (tab,
// group, label and submenu names, inline command text, tips) passes through
// QCoreApplication::translate("Ribbon", ...), so a translator can reach it once the app has
// translations; lupdate doesn't read JSON, so a generated source file will have to list them.

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QString>
#include <functional>

class QAction;
class QMenu;
class QObject;
class QWidget;

namespace jp {

class Ribbon;

// What the file may name; each is looked up when the file uses it.
struct RibbonParts {
    std::function<QAction *(const QString &id)> action;                  // existing commands (null when unknown)
    QHash<QString, std::function<QWidget *()>> widgets;                  // custom controls by name
    QHash<QString, std::function<QMenu *(QWidget *owner)>> menus;        // menus built in code; `owner` parents the menu
    QHash<QString, std::function<void()>> launchers;                     // dialog launchers
    QHash<QString, std::function<void()>> triggers;                      // what a ribbon-only command does, by its id
    QHash<QString, QColor> colors;                                       // contextual tab colors by name
};

// Builds the tabs, groups and controls in `json` into `r` (which should be empty).
// Commands the file creates are children of `actionOwner`. On any problem it returns
// false and fills *error with one line for each: every unknown command, widget, menu,
// launcher, template, color or icon, and every malformed entry. It still builds what it can.
bool buildRibbon(Ribbon *r, const QByteArray &json, const RibbonParts &parts, QObject *actionOwner, QString *error);

} // namespace jp
