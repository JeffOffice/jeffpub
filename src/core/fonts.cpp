#include "core/fonts.h"

#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QImageReader>

Q_GUI_EXPORT int qt_defaultDpiY();

namespace jp {

double fontPointFactor()
{
    const int dpi = qt_defaultDpiY();
    return dpi > 0 ? 72.0 / dpi : 1.0;
}


static QStringList g_families;

static QStringList fontDirs()
{
    const QString app = QCoreApplication::applicationDirPath();
    QStringList dirs{app + "/fonts", app + "/../share/jeffpub79/fonts", app + "/../fonts"};
#ifdef JP_SOURCE_DIR
    dirs << QStringLiteral(JP_SOURCE_DIR) + "/resources/fonts";
#endif
    return dirs;
}

int loadBundledFonts()
{
    int n = 0;
    for (const QString &d : fontDirs()) {
        QDir dir(d);
        if (!dir.exists()) continue;
        for (const QString &f : dir.entryList({"*.ttf", "*.otf"}, QDir::Files)) {
            const int id = QFontDatabase::addApplicationFont(dir.filePath(f));
            if (id < 0) continue;
            ++n;
            for (const QString &fam : QFontDatabase::applicationFontFamilies(id))
                if (!g_families.contains(fam)) g_families << fam;
        }
        if (n) break;
    }
    g_families.sort();
    return n;
}

QStringList bundledFamilies() { return g_families; }

// Metric-compatible replacements first; otherwise the closest open design.
static const QHash<QString, QString> &table()
{
    static const QHash<QString, QString> t{
        {"Arial", "Arimo"}, {"Helvetica", "Arimo"}, {"Arial Unicode MS", "Arimo"}, {"Liberation Sans", "Arimo"},
        {"Times New Roman", "Tinos"}, {"Times", "Tinos"}, {"Liberation Serif", "Tinos"},
        {"Courier New", "Cousine"}, {"Courier", "Cousine"}, {"Liberation Mono", "Cousine"}, {"Lucida Console", "Cousine"},
        {"Calibri", "Carlito"}, {"Calibri Light", "Carlito"}, {"Cambria", "Caladea"}, {"Georgia", "Gelasio"},
        {"Segoe UI", "Open Sans"}, {"Comic Sans MS", "Comic Neue"}, {"Garamond", "EB Garamond"},
        {"Book Antiqua", "TeX Gyre Pagella"}, {"Palatino Linotype", "TeX Gyre Pagella"}, {"Palatino", "TeX Gyre Pagella"},
        {"Bookman Old Style", "TeX Gyre Bonum"}, {"Century Schoolbook", "TeX Gyre Schola"}, {"Century Gothic", "TeX Gyre Adventor"},
        {"Gill Sans MT", "Cabin"}, {"Gill Sans", "Cabin"}, {"Abadi", "Cabin"}, {"Abadi MT Condensed Light", "Archivo Narrow"},
        {"Tw Cen MT", "Jost"}, {"Futura", "Jost"}, {"AG_Futura", "Jost"}, {"Futura Md BT", "Jost"}, {"Futura Bk BT", "Jost"},
        {"Franklin Gothic Book", "Libre Franklin"}, {"Franklin Gothic Medium", "Libre Franklin"}, {"Franklin Gothic Demi", "Libre Franklin"},
        {"Franklin Gothic Demi Cond", "Libre Franklin"}, {"Franklin Gothic Medium Cond", "Libre Franklin"},
        {"Arial Narrow", "Arimo"}, {"Arial Black", "Archivo Black"}, {"Impact", "Anton"},
        {"Verdana", "DejaVu Sans"}, {"Tahoma", "DejaVu Sans"}, {"Trebuchet MS", "PT Sans"}, {"Corbel", "Open Sans"}, {"Candara", "Cabin"},
        {"Constantia", "Gelasio"}, {"Sylfaen", "Gelasio"}, {"Consolas", "IBM Plex Mono"},
        {"Rockwell", "Arvo"}, {"Rockwell Condensed", "Roboto Slab"}, {"Rockwell Extra Bold", "Alfa Slab One"},
        {"Agency FB", "Saira Condensed"}, {"Castellar", "Cinzel"}, {"Copperplate Gothic Bold", "Cinzel"}, {"Copperplate Gothic Light", "Cinzel"},
        {"Old English Text MT", "UnifrakturMaguntia"}, {"CloisterBlack BT", "UnifrakturMaguntia"}, {"Brush Script MT", "Kaushan Script"},
        {"Lucida Handwriting", "Merienda"}, {"Juice ITC", "Mountains of Christmas"}, {"Monotype Corsiva", "Great Vibes"}, {"Edwardian Script ITC", "Great Vibes"},
        {"Bodoni MT", "Playfair Display"}, {"Baskerville Old Face", "Libre Baskerville"}, {"Elephant", "Abril Fatface"},
        {"Bernard MT Condensed", "Anton"}, {"Haettenschweiler", "Anton"}, {"Gill Sans Ultra Bold", "Archivo Black"},
        {"Perpetua", "Crimson Text"}, {"Goudy Old Style", "Crimson Text"}, {"Lucida Sans", "Open Sans"}, {"Lucida Sans Unicode", "Open Sans"},
        {"Microsoft Sans Serif", "Arimo"}, {"MS Sans Serif", "Arimo"}, {"MS Serif", "Tinos"}, {"Script MT Bold", "Satisfy"},
        {"Imprint MT Shadow", "EB Garamond"}, {"OCR A Extended", "IBM Plex Mono"}, {"Kristen ITC", "Comic Neue"}, {"Papyrus", "Cinzel"},
    };
    return t;
}

// Condensed originals keep their width by scaling the substitute horizontally.
static const QHash<QString, int> &stretches()
{
    static const QHash<QString, int> t{
        {"Arial Narrow", 82}, {"Lucida Handwriting", 114}, {"Juice ITC", 80}, {"Franklin Gothic Demi Cond", 75}, {"Franklin Gothic Medium Cond", 75}, {"Rockwell Condensed", 72},
        {"Abadi MT Condensed Light", 85}, {"Gill Sans MT Condensed", 75}, {"Tw Cen MT Condensed", 75}, {"Bernard MT Condensed", 90},
    };
    return t;
}

QString interchangeFontName(const QString &family)
{
    // Only the open fonts drawn to the same character widths as a standard
    // font; text laid out with one lays out the same with the other.
    static const QHash<QString, QString> t{
        {"Arimo", "Arial"}, {"Tinos", "Times New Roman"}, {"Cousine", "Courier New"},
        {"Carlito", "Calibri"}, {"Caladea", "Cambria"}, {"Gelasio", "Georgia"},
    };
    return t.value(family, family);
}

int substituteStretch(const QString &family)
{
    const int s = stretches().value(family, 100);
    if (s == 100 || QFontDatabase::hasFamily(family)) return 100;
    return s;
}

int substituteWeight(const QString &family)
{
    static QHash<QString, int> cache;
    auto it = cache.constFind(family);
    if (it != cache.constEnd()) return *it;
    int w = 0;
    if (!QFontDatabase::hasFamily(family)) {
        const QString f = family.toLower();
        // Franklin Gothic's weights run heavy: its Medium looks like a SemiBold.
        if (f.startsWith("franklin gothic") && f.contains("medium")) w = 600;
        else if (f.startsWith("franklin gothic") && f.contains("demi")) w = 700;
        else if (f.contains("black") || f.contains("heavy")) w = 900;
        else if (f.contains("extrabold") || f.contains("ultra bold")) w = 800;
        else if (f.contains("demi") || f.contains("semibold") || f.contains("semi bold")) w = 600;
        else if (f.contains("medium")) w = 500;
        else if (f.contains("light")) w = 300;
        else if (f.contains("thin")) w = 100;
    }
    cache.insert(family, w);
    return w;
}

bool isSymbolFont(const QString &family)
{
    static const QStringList fonts = {QStringLiteral("Symbol"), QStringLiteral("Wingdings"), QStringLiteral("Wingdings 2"), QStringLiteral("Wingdings 3"),
                                      QStringLiteral("Webdings")};
    return fonts.contains(family, Qt::CaseInsensitive);
}

QString symbolToUnicode(const QString &family, uint code)
{
    if (code >= 0xF000 && code <= 0xF0FF) code -= 0xF000;
    // The pictures most used as bullets and check boxes.
    static const QHash<uint, uint> symbol = {
        {0xB7, 0x2022}, {0xA7, 0x2663}, {0xA8, 0x2666}, {0xA9, 0x2665}, {0xAA, 0x2660}, {0xAE, 0x2192}, {0xDE, 0x21D2},
        {0xB0, 0x00B0}, {0xD7, 0x22C5}, {0xE0, 0x25CA}, {0x2A, 0x2217}, {0xC4, 0x2297}, {0xC5, 0x2295}, {0x6F, 0x03BF}};
    static const QHash<uint, uint> wingdings = {
        {0x6C, 0x25CF}, {0x6E, 0x25A0}, {0x6F, 0x25A1}, {0x71, 0x2751}, {0x72, 0x2752}, {0x75, 0x25C6}, {0x76, 0x2756},
        {0x77, 0x2B25}, {0x9F, 0x2022}, {0xA1, 0x25CB}, {0xA7, 0x25AA}, {0xA8, 0x25FB}, {0xAB, 0x2605}, {0xD8, 0x27A2},
        {0xE8, 0x2794}, {0xF0, 0x21E8}, {0xFB, 0x2718}, {0xFC, 0x2714}, {0xFD, 0x2612}, {0xFE, 0x2611}};
    const QString f = family.toLower();
    const QHash<uint, uint> *map = f == QLatin1String("symbol") ? &symbol : f == QLatin1String("wingdings") ? &wingdings : nullptr;
    if (!map || !map->contains(code)) return {};
    return QString(QChar(char16_t(map->value(code))));
}

uint unicodeToSymbol(QChar c, QString *family)
{
    for (const QString &f : {QStringLiteral("Symbol"), QStringLiteral("Wingdings")})
        for (uint code = 0x20; code <= 0xFF; ++code)
            if (symbolToUnicode(f, code) == QString(c)) {
                if (family) *family = f;
                return 0xF000 + code;
            }
    return 0;
}

QString substituteFor(const QString &family)
{
    if (QFontDatabase::hasFamily(family)) return {};
    return table().value(family);
}

void installFontSubstitutions()
{
    const auto &t = table();
    for (auto it = t.cbegin(); it != t.cend(); ++it) {
        if (QFontDatabase::hasFamily(it.key())) continue;
        QStringList subs{it.value()};
        // Sensible generic fallbacks after the named substitute.
        subs << (it.value().contains("Mono") || it.value() == "Cousine" ? QStringLiteral("Cousine") : QStringLiteral("Arimo"));
        QFont::insertSubstitutions(it.key(), subs);
    }
}

} // namespace jp

namespace jp {

void initCore()
{
    static bool done = false;
    if (done) return;
    done = true;
    QImageReader::setAllocationLimit(2048);   // megabytes; print covers carry 600 dpi photos
    loadBundledFonts();
    installFontSubstitutions();
}

} // namespace jp
