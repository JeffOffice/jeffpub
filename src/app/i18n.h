#pragma once
// Translations of the program's own text: JeffPub's (built in under
// :/i18n/jeffpub_<language>.qm) and Qt's for its standard dialogs.

#include <QString>
#include <QStringList>

namespace jp {

// The languages JeffPub has a translation for (BCP 47 names, "de", "pt_BR"),
// besides English, which its text is written in.
QStringList availableUiLanguages();

// Loads the translation for the language chosen in Options (ui/language),
// or the system's first language JeffPub has, at startup. Returns the
// language in use ("en" when none applies).
QString installTranslations();

} // namespace jp
