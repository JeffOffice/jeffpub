#include "core/fonts.h"

#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
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

QStringList bundledFontDirs()
{
    const QString app = QCoreApplication::applicationDirPath();
    // Beside the program (Windows), the Linux packages' share folder, and a
    // macOS app's Resources folder.
    QStringList dirs{app + "/fonts", app + "/../share/jeffpub/fonts", app + "/../fonts", app + "/../Resources/fonts"};
#ifdef JP_SOURCE_DIR
    dirs << QStringLiteral(JP_SOURCE_DIR) + "/resources/fonts";
#endif
    return dirs;
}

int loadBundledFonts()
{
    int n = 0;
    for (const QString &d : bundledFontDirs()) {
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
        {"Segoe UI", "Selawik"}, {"Segoe UI Light", "Selawik Light"}, {"Segoe UI Semilight", "Selawik Semilight"}, {"Segoe UI Semibold", "Selawik Semibold"}, {"Comic Sans MS", "Comic Relief"}, {"Garamond", "EB Garamond"},
        {"Book Antiqua", "TeX Gyre Pagella"}, {"Palatino Linotype", "TeX Gyre Pagella"}, {"Palatino", "TeX Gyre Pagella"},
        {"Bookman Old Style", "TeX Gyre Bonum"}, {"Century Schoolbook", "TeX Gyre Schola"}, {"Century Gothic", "TeX Gyre Adventor"},
        {"Gill Sans MT", "Cabin"}, {"Gill Sans", "Cabin"}, {"Abadi", "Cabin"}, {"Abadi MT Condensed Light", "Archivo Narrow"},
        {"Tw Cen MT", "Jost"}, {"Futura", "Jost"}, {"AG_Futura", "Jost"}, {"Futura Md BT", "Jost"}, {"Futura Bk BT", "Jost"},
        {"Franklin Gothic Book", "Libre Franklin"}, {"Franklin Gothic Medium", "Libre Franklin"}, {"Franklin Gothic Demi", "Libre Franklin"},
        {"Franklin Gothic Demi Cond", "Libre Franklin"}, {"Franklin Gothic Medium Cond", "Libre Franklin"}, {"Franklin Gothic Heavy", "Libre Franklin"},
        {"Arial Narrow", "Liberation Sans Narrow"}, {"Arial Black", "Archivo Black"}, {"Impact", "Anton"},
        {"Verdana", "DejaVu Sans"}, {"Tahoma", "DejaVu Sans"}, {"Trebuchet MS", "Open Sans"}, {"Corbel", "Carlito"}, {"Candara", "Cabin"},
        {"Constantia", "Gelasio"}, {"Sylfaen", "Gelasio"}, {"Consolas", "IBM Plex Mono"},
        {"Rockwell", "Arvo"}, {"Rockwell Condensed", "Roboto Slab"}, {"Rockwell Extra Bold", "Alfa Slab One"},
        {"Agency FB", "Archivo Narrow"}, {"Castellar", "Cinzel"}, {"Copperplate Gothic Bold", "Cinzel"}, {"Copperplate Gothic Light", "Cinzel"},
        {"Old English Text MT", "UnifrakturMaguntia"}, {"CloisterBlack BT", "UnifrakturMaguntia"}, {"Brush Script MT", "Kaushan Script"},
        {"Lucida Handwriting", "Merienda"}, {"Juice ITC", "Mountains of Christmas"}, {"Monotype Corsiva", "Dancing Script"}, {"Edwardian Script ITC", "Great Vibes"},
        {"Bodoni MT", "Playfair Display"}, {"Baskerville Old Face", "Tinos"}, {"Elephant", "Abril Fatface"},
        {"Bernard MT Condensed", "Abril Fatface"}, {"Haettenschweiler", "Anton"}, {"Gill Sans Ultra Bold", "Archivo Black"},
        {"Perpetua", "Crimson Text"}, {"Goudy Old Style", "Crimson Text"}, {"Lucida Sans", "DejaVu Sans"}, {"Lucida Sans Unicode", "DejaVu Sans"},
        {"Microsoft Sans Serif", "Arimo"}, {"MS Sans Serif", "Arimo"}, {"MS Serif", "Tinos"}, {"Script MT Bold", "Pacifico"},
        {"Imprint MT Shadow", "EB Garamond"}, {"OCR A Extended", "IBM Plex Mono"}, {"Kristen ITC", "Comic Relief"}, {"Papyrus", "Mountains of Christmas"},
        // Measured against each Windows font and Publisher's own test pages (Oct 8):
        // the nearest bundled face of the same kind.
        {"Algerian", "Cinzel"},
        {"Arial Rounded MT Bold", "Nunito"},
        {"Bahnschrift", "IBM Plex Sans"},
        {"Bahnschrift Condensed", "Archivo Narrow"},
        {"Bahnschrift Light", "IBM Plex Sans"},
        {"Bahnschrift Light Condensed", "Archivo Narrow"},
        {"Bahnschrift Light SemiCondensed", "Archivo Narrow"},
        {"Bahnschrift SemiBold", "IBM Plex Sans"},
        {"Bahnschrift SemiBold Condensed", "Archivo Narrow"},
        {"Bahnschrift SemiBold SemiCondensed", "Archivo Narrow"},
        {"Bahnschrift SemiCondensed", "Archivo Narrow"},
        {"Bahnschrift SemiLight", "IBM Plex Sans"},
        {"Bahnschrift SemiLight Condensed", "Archivo Narrow"},
        {"Bahnschrift SemiLight SemiCondensed", "Archivo Narrow"},
        {"Bauhaus 93", "Jost"},
        {"Bell MT", "Tinos"},
        {"Berlin Sans FB", "Jost"},
        {"Berlin Sans FB Demi", "Jost"},
        {"Blackadder ITC", "Great Vibes"},
        {"Bodoni MT Black", "Abril Fatface"},
        {"Bodoni MT Condensed", "Playfair Display"},
        {"Bodoni MT Poster Compressed", "Anton"},
        {"Bradley Hand ITC", "Comic Neue"},
        {"Britannic Bold", "Lato"},
        {"Broadway", "Abril Fatface"},
        {"Californian FB", "Crimson Text"},
        {"Calisto MT", "Tinos"},
        {"Cambria Math", "Caladea"},
        {"Candara Light", "Cabin"},
        {"Cascadia Code", "IBM Plex Mono"},
        {"Cascadia Code ExtraLight", "IBM Plex Mono"},
        {"Cascadia Code Light", "IBM Plex Mono"},
        {"Cascadia Code SemiBold", "IBM Plex Mono"},
        {"Cascadia Code SemiLight", "IBM Plex Mono"},
        {"Cascadia Mono", "IBM Plex Mono"},
        {"Cascadia Mono ExtraLight", "IBM Plex Mono"},
        {"Cascadia Mono Light", "IBM Plex Mono"},
        {"Cascadia Mono SemiBold", "IBM Plex Mono"},
        {"Cascadia Mono SemiLight", "IBM Plex Mono"},
        {"Centaur", "EB Garamond"},
        {"Century", "TeX Gyre Schola"},
        {"Chiller", "Mountains of Christmas"},
        {"Colonna MT", "Cormorant Garamond"},
        {"Cooper Black", "Fraunces"},
        {"Corbel Light", "Carlito"},
        {"Curlz MT", "Mountains of Christmas"},
        {"Dubai", "Carlito"},
        {"Dubai Light", "Carlito"},
        {"Dubai Medium", "Carlito"},
        {"Engravers MT", "Cinzel"},
        {"Eras Bold ITC", "Selawik"},
        {"Eras Demi ITC", "Selawik Semibold"},
        {"Eras Light ITC", "Selawik Light"},
        {"Eras Medium ITC", "Selawik"},
        {"Felix Titling", "Cinzel"},
        {"Footlight MT Light", "Cormorant Garamond"},
        {"Forte", "Kaushan Script"},
        {"Freestyle Script", "Great Vibes"},
        {"French Script MT", "Great Vibes"},
        {"Gabriola", "Great Vibes"},
        {"Gigi", "Lobster"},
        {"Gill Sans MT Condensed", "Archivo Narrow"},
        {"Gill Sans MT Ext Condensed Bold", "Oswald"},
        {"Gill Sans Ultra Bold Condensed", "Anton"},
        {"Gloucester MT Extra Condensed", "EB Garamond"},
        {"Goudy Stout", "Alfa Slab One"},
        {"Harlow Solid Italic", "Lobster"},
        {"Harrington", "Playfair Display"},
        {"High Tower Text", "Crimson Text"},
        {"Informal Roman", "Dancing Script"},
        {"Ink Free", "Comic Neue"},
        {"Jokerman", "Permanent Marker"},
        {"Kunstler Script", "Great Vibes"},
        {"Lucida Bright", "Merriweather"},
        {"Lucida Calligraphy", "Merienda"},
        {"Lucida Fax", "Arvo"},
        {"Lucida Sans Typewriter", "Cousine"},
        {"Magneto", "Archivo Black"},
        {"Maiandra GD", "Selawik"},
        {"Matura MT Script Capitals", "Merienda"},
        {"Mistral", "Caveat"},
        {"Modern No. 20", "Tinos"},
        {"MS Reference Sans Serif", "DejaVu Sans"},
        {"MV Boli", "Comic Relief"},
        {"Niagara Engraved", "Anton"},
        {"Niagara Solid", "Anton"},
        {"Onyx", "Cormorant Garamond"},
        {"Palace Script MT", "Great Vibes"},
        {"Parchment", "Great Vibes"},
        {"Perpetua Titling MT", "Cinzel"},
        {"Playbill", "Anton"},
        {"Poor Richard", "EB Garamond"},
        {"Pristina", "Caveat"},
        {"Rage Italic", "Kaushan Script"},
        {"Ravie", "Bungee"},
        {"Segoe Print", "Comic Relief"},
        {"Segoe Script", "Merienda"},
        {"Segoe UI Black", "Libre Franklin"},
        {"Segoe UI Variable Display", "Selawik"},
        {"Segoe UI Variable Display Light", "Selawik Light"},
        {"Segoe UI Variable Display Semibold", "Selawik Semibold"},
        {"Segoe UI Variable Display Semilight", "Selawik Semilight"},
        {"Segoe UI Variable Small", "Selawik"},
        {"Segoe UI Variable Small Light", "Selawik Light"},
        {"Segoe UI Variable Small Semibold", "Selawik Semibold"},
        {"Segoe UI Variable Small Semilight", "Selawik Semilight"},
        {"Segoe UI Variable Text", "Selawik"},
        {"Segoe UI Variable Text Light", "Selawik Light"},
        {"Segoe UI Variable Text Semibold", "Selawik Semibold"},
        {"Segoe UI Variable Text Semilight", "Selawik Semilight"},
        {"Showcard Gothic", "Archivo Black"},
        {"Snap ITC", "Bungee"},
        {"Stencil", "Archivo Black"},
        {"Tempus Sans ITC", "Comic Neue"},
        {"Tw Cen MT Condensed", "Archivo Narrow"},
        {"Tw Cen MT Condensed Extra Bold", "Oswald"},
        {"Viner Hand ITC", "Kaushan Script"},
        {"Vivaldi", "Great Vibes"},
        {"Vladimir Script", "Great Vibes"},
        {"Wide Latin", "Alfa Slab One"},
    };
    return t;
}

// Condensed originals keep their width by scaling the substitute horizontally.
static const QHash<QString, int> &stretches()
{
    static const QHash<QString, int> t{
        {"Lucida Handwriting", 114}, {"Franklin Gothic Medium Cond", 75}, 
        {"Abadi MT Condensed Light", 85}, 
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
        {"Comic Relief", "Comic Sans MS"}, {"Selawik", "Segoe UI"}, {"Liberation Sans Narrow", "Arial Narrow"},
    };
    return t.value(family, family);
}

// Stand-ins drawn to the originals' widths, for each style (regular, bold,
// italic, bold italic; 0 where nothing was measured, which takes the
// regular or bold one): the letters narrowed or widened (percent) and the
// word space (ems). Measured from the character widths of the fonts embedded
// in Publisher's PDFs of 284 publications, weighted by how often each
// character appears in them, against the stand-ins (Oct 7): Cabin's letters
// run 5% wider than Gill Sans MT's and 10% narrower than its bold, and its
// spaces 24% narrower.
// They hold only for the stand-in they were measured with (macOS draws Gill
// Sans MT with its own Gill Sans, which needs none).
struct StandInWidths { const char *standIn; int stretch[4]; double space[4]; };
static const QHash<QString, StandInWidths> &standInWidths()
{
    static const QHash<QString, StandInWidths> t{
        {"Gill Sans MT", {"Cabin", {95, 109, 92, 103}, {0.278, 0.278, 0.278, 0.278}}},
        {"Franklin Gothic Book", {"Libre Franklin", {89, 0, 85, 0}, {0.25, 0, 0.25, 0}}},
        {"Franklin Gothic Demi", {"Libre Franklin", {89, 0, 88, 0}, {0.25, 0, 0.25, 0}}},
        // Heavy against Libre Franklin ExtraBold: 3.4% wider over a sample
        // of ordinary text (Oct 8, the Windows font's advances).
        {"Franklin Gothic Heavy", {"Libre Franklin", {97, 0, 0, 0}, {0, 0, 0, 0}}},
        // (Agency FB's letters stay: asked to narrow, its stand-in Saira
        // Condensed switches to a narrower face of its own instead.)
        {"Agency FB", {"Archivo Narrow", {85, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Garamond", {"EB Garamond", {0, 101, 88, 0}, {0.25, 0.25, 0.25, 0}}},
        {"Imprint MT Shadow", {"EB Garamond", {110, 0, 0, 0}, {0.25, 0, 0, 0}}},
        {"Castellar", {"Cinzel", {116, 0, 0, 0}, {0.36, 0, 0, 0}}},
        {"Arial Rounded MT Bold", {"Nunito", {103, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Abadi", {"Cabin", {0, 0, 0, 0}, {0.302, 0, 0, 0}}},
        // Measured with Qt against each Windows font's advances over a sample of
        // ordinary text (Oct 8).
        {"Algerian", {"Cinzel", {97, 95, 0, 0}, {0, 0, 0, 0}}},
        {"Bahnschrift", {"IBM Plex Sans", {0, 95, 0, 0}, {0, 0, 0, 0}}},
        {"Bahnschrift Condensed", {"Archivo Narrow", {90, 84, 0, 0}, {0, 0, 0, 0}}},
        {"Bahnschrift Light", {"IBM Plex Sans", {0, 95, 0, 0}, {0, 0, 0, 0}}},
        {"Bahnschrift Light Condensed", {"Archivo Narrow", {90, 84, 0, 0}, {0, 0, 0, 0}}},
        {"Bahnschrift Light SemiCondensed", {"Archivo Narrow", {106, 99, 0, 0}, {0, 0, 0, 0}}},
        {"Bahnschrift SemiBold", {"IBM Plex Sans", {95, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Bahnschrift SemiBold Condensed", {"Archivo Narrow", {84, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Bahnschrift SemiCondensed", {"Archivo Narrow", {106, 99, 0, 0}, {0, 0, 0, 0}}},
        {"Bahnschrift SemiLight", {"IBM Plex Sans", {0, 95, 0, 0}, {0, 0, 0, 0}}},
        {"Bahnschrift SemiLight Condensed", {"Archivo Narrow", {90, 84, 0, 0}, {0, 0, 0, 0}}},
        {"Bahnschrift SemiLight SemiCondensed", {"Archivo Narrow", {106, 99, 0, 0}, {0, 0, 0, 0}}},
        {"Baskerville Old Face", {"Tinos", {97, 92, 0, 0}, {0, 0, 0, 0}}},
        {"Bauhaus 93", {"Jost", {94, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Bell MT", {"Tinos", {0, 102, 94, 0}, {0, 0, 0, 0}}},
        {"Berlin Sans FB", {"Jost", {97, 101, 0, 0}, {0, 0, 0, 0}}},
        {"Berlin Sans FB Demi", {"Jost", {94, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Bernard MT Condensed", {"Abril Fatface", {84, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Blackadder ITC", {"Great Vibes", {107, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Bodoni MT", {"Playfair Display", {95, 90, 0, 102}, {0, 0, 0, 0}}},
        {"Bodoni MT Black", {"Abril Fatface", {112, 0, 119, 0}, {0, 0, 0, 0}}},
        {"Bodoni MT Condensed", {"Playfair Display", {64, 73, 67, 74}, {0, 0, 0, 0}}},
        {"Bodoni MT Poster Compressed", {"Anton", {60, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Broadway", {"Abril Fatface", {119, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Brush Script MT", {"Kaushan Script", {84, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Calibri Light", {"Carlito", {0, 96, 99, 0}, {0, 0, 0, 0}}},
        {"Californian FB", {"Crimson Text", {103, 99, 94, 0}, {0, 0, 0, 0}}},
        {"Calisto MT", {"Tinos", {105, 103, 94, 96}, {0, 0, 0, 0}}},
        {"Cambria", {"Caladea", {104, 102, 0, 0}, {0, 0, 0, 0}}},
        {"Cambria Math", {"Caladea", {104, 95, 0, 0}, {0, 0, 0, 0}}},
        {"Candara", {"Cabin", {0, 0, 104, 98}, {0, 0, 0, 0}}},
        {"Candara Light", {"Cabin", {0, 96, 103, 0}, {0, 0, 0, 0}}},
        {"Cascadia Code", {"IBM Plex Mono", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Cascadia Code ExtraLight", {"IBM Plex Mono", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Cascadia Code Light", {"IBM Plex Mono", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Cascadia Code SemiBold", {"IBM Plex Mono", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Cascadia Code SemiLight", {"IBM Plex Mono", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Cascadia Mono", {"IBM Plex Mono", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Cascadia Mono ExtraLight", {"IBM Plex Mono", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Cascadia Mono Light", {"IBM Plex Mono", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Cascadia Mono SemiBold", {"IBM Plex Mono", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Cascadia Mono SemiLight", {"IBM Plex Mono", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Centaur", {"EB Garamond", {96, 87, 0, 0}, {0, 0, 0, 0}}},
        {"Century", {"TeX Gyre Schola", {0, 89, 0, 0}, {0, 0, 0, 0}}},
        {"Chiller", {"Mountains of Christmas", {82, 79, 0, 0}, {0, 0, 0, 0}}},
        {"Colonna MT", {"Cormorant Garamond", {107, 105, 0, 0}, {0, 0, 0, 0}}},
        {"Consolas", {"IBM Plex Mono", {92, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Constantia", {"Gelasio", {98, 92, 93, 89}, {0, 0, 0, 0}}},
        {"Cooper Black", {"Fraunces", {104, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Copperplate Gothic Bold", {"Cinzel", {102, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Copperplate Gothic Light", {"Cinzel", {0, 98, 0, 0}, {0, 0, 0, 0}}},
        {"Corbel", {"Carlito", {0, 104, 98, 101}, {0, 0, 0, 0}}},
        {"Corbel Light", {"Carlito", {97, 95, 93, 0}, {0, 0, 0, 0}}},
        {"Curlz MT", {"Mountains of Christmas", {107, 102, 0, 0}, {0, 0, 0, 0}}},
        {"Dubai", {"Carlito", {0, 102, 0, 0}, {0, 0, 0, 0}}},
        {"Dubai Light", {"Carlito", {98, 95, 0, 0}, {0, 0, 0, 0}}},
        {"Dubai Medium", {"Carlito", {102, 99, 0, 0}, {0, 0, 0, 0}}},
        {"Edwardian Script ITC", {"Great Vibes", {91, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Elephant", {"Abril Fatface", {108, 0, 110, 0}, {0, 0, 0, 0}}},
        {"Engravers MT", {"Cinzel", {141, 138, 0, 0}, {0, 0, 0, 0}}},
        {"Eras Bold ITC", {"Selawik", {107, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Eras Demi ITC", {"Selawik Semibold", {102, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Eras Medium ITC", {"Selawik", {0, 93, 0, 0}, {0, 0, 0, 0}}},
        {"Felix Titling", {"Cinzel", {105, 103, 0, 0}, {0, 0, 0, 0}}},
        {"Footlight MT Light", {"Cormorant Garamond", {106, 104, 0, 0}, {0, 0, 0, 0}}},
        {"Forte", {"Kaushan Script", {107, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Franklin Gothic Demi Cond", {"Libre Franklin", {78, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Franklin Gothic Medium", {"Libre Franklin", {90, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Freestyle Script", {"Great Vibes", {85, 0, 0, 0}, {0, 0, 0, 0}}},
        {"French Script MT", {"Great Vibes", {96, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Gill Sans MT Condensed", {"Archivo Narrow", {83, 78, 0, 0}, {0, 0, 0, 0}}},
        {"Gill Sans MT Ext Condensed Bold", {"Oswald", {60, 60, 0, 0}, {0, 0, 0, 0}}},
        {"Gill Sans Ultra Bold", {"Archivo Black", {115, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Gill Sans Ultra Bold Condensed", {"Anton", {112, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Gloucester MT Extra Condensed", {"EB Garamond", {72, 65, 0, 0}, {0, 0, 0, 0}}},
        {"Goudy Old Style", {"Crimson Text", {0, 95, 0, 0}, {0, 0, 0, 0}}},
        {"Goudy Stout", {"Alfa Slab One", {160, 160, 0, 0}, {0, 0, 0, 0}}},
        {"Haettenschweiler", {"Anton", {77, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Harrington", {"Playfair Display", {96, 94, 0, 0}, {0, 0, 0, 0}}},
        {"High Tower Text", {"Crimson Text", {107, 98, 0, 0}, {0, 0, 0, 0}}},
        {"Informal Roman", {"Dancing Script", {103, 100, 0, 0}, {0, 0, 0, 0}}},
        {"Ink Free", {"Comic Neue", {0, 97, 0, 0}, {0, 0, 0, 0}}},
        {"Jokerman", {"Permanent Marker", {97, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Juice ITC", {"Mountains of Christmas", {80, 76, 0, 0}, {0, 0, 0, 0}}},
        {"Kristen ITC", {"Comic Relief", {105, 99, 0, 0}, {0, 0, 0, 0}}},
        {"Kunstler Script", {"Great Vibes", {88, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Lucida Bright", {"Merriweather", {102, 0, 107, 110}, {0, 0, 0, 0}}},
        {"Lucida Calligraphy", {"Merienda", {109, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Lucida Fax", {"Arvo", {107, 105, 102, 103}, {0, 0, 0, 0}}},
        {"Lucida Sans", {"DejaVu Sans", {97, 88, 0, 0}, {0, 0, 0, 0}}},
        {"Lucida Sans Unicode", {"DejaVu Sans", {97, 83, 0, 0}, {0, 0, 0, 0}}},
        {"Maiandra GD", {"Selawik", {0, 94, 0, 0}, {0, 0, 0, 0}}},
        {"Matura MT Script Capitals", {"Merienda", {90, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Microsoft Sans Serif", {"Arimo", {0, 92, 0, 0}, {0, 0, 0, 0}}},
        {"Mistral", {"Caveat", {98, 96, 0, 0}, {0, 0, 0, 0}}},
        {"Modern No. 20", {"Tinos", {0, 93, 0, 0}, {0, 0, 0, 0}}},
        {"Monotype Corsiva", {"Dancing Script", {0, 97, 0, 0}, {0, 0, 0, 0}}},
        {"MS Reference Sans Serif", {"DejaVu Sans", {0, 87, 0, 0}, {0, 0, 0, 0}}},
        {"MV Boli", {"Comic Relief", {104, 98, 0, 0}, {0, 0, 0, 0}}},
        {"Niagara Engraved", {"Anton", {60, 60, 0, 0}, {0, 0, 0, 0}}},
        {"Niagara Solid", {"Anton", {60, 60, 0, 0}, {0, 0, 0, 0}}},
        {"Old English Text MT", {"UnifrakturMaguntia", {103, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Onyx", {"Cormorant Garamond", {60, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Palace Script MT", {"Great Vibes", {78, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Papyrus", {"Mountains of Christmas", {117, 111, 0, 0}, {0, 0, 0, 0}}},
        {"Parchment", {"Great Vibes", {63, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Perpetua", {"Crimson Text", {93, 99, 91, 104}, {0, 0, 0, 0}}},
        {"Perpetua Titling MT", {"Cinzel", {106, 114, 0, 0}, {0, 0, 0, 0}}},
        {"Playbill", {"Anton", {63, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Poor Richard", {"EB Garamond", {95, 86, 0, 0}, {0, 0, 0, 0}}},
        {"Pristina", {"Caveat", {96, 94, 0, 0}, {0, 0, 0, 0}}},
        {"Rage Italic", {"Kaushan Script", {91, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Ravie", {"Bungee", {113, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Rockwell", {"Arvo", {96, 0, 90, 92}, {0, 0, 0, 0}}},
        {"Rockwell Condensed", {"Roboto Slab", {71, 86, 0, 0}, {0, 0, 0, 0}}},
        {"Rockwell Extra Bold", {"Alfa Slab One", {111, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Script MT Bold", {"Pacifico", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe Print", {"Comic Relief", {110, 103, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe Script", {"Merienda", {112, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe UI", {"Selawik", {0, 0, 98, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Black", {"Libre Franklin", {103, 105, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Light", {"Selawik Light", {0, 0, 98, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Semibold", {"Selawik Semibold", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Semilight", {"Selawik Semilight", {0, 0, 98, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Variable Display", {"Selawik", {98, 91, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Variable Display Light", {"Selawik Light", {96, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Variable Display Semibold", {"Selawik Semibold", {97, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Variable Display Semilight", {"Selawik Semilight", {96, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Variable Small", {"Selawik", {104, 97, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Variable Small Light", {"Selawik Light", {105, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Variable Small Semibold", {"Selawik Semibold", {102, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Variable Small Semilight", {"Selawik Semilight", {105, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Variable Text", {"Selawik", {0, 93, 0, 0}, {0, 0, 0, 0}}},
        {"Segoe UI Variable Text Semibold", {"Selawik Semibold", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Snap ITC", {"Bungee", {97, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Stencil", {"Archivo Black", {98, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Sylfaen", {"Gelasio", {96, 82, 0, 0}, {0, 0, 0, 0}}},
        {"Tahoma", {"DejaVu Sans", {85, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Tempus Sans ITC", {"Comic Neue", {0, 98, 0, 0}, {0, 0, 0, 0}}},
        {"Trebuchet MS", {"Open Sans", {98, 96, 105, 103}, {0, 0, 0, 0}}},
        {"Tw Cen MT", {"Jost", {94, 89, 0, 83}, {0, 0, 0, 0}}},
        {"Tw Cen MT Condensed", {"Archivo Narrow", {82, 90, 0, 0}, {0, 0, 0, 0}}},
        {"Tw Cen MT Condensed Extra Bold", {"Oswald", {94, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Viner Hand ITC", {"Kaushan Script", {117, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Vivaldi", {"Great Vibes", {108, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Vladimir Script", {"Great Vibes", {110, 0, 0, 0}, {0, 0, 0, 0}}},
        {"Wide Latin", {"Alfa Slab One", {157, 0, 0, 0}, {0, 0, 0, 0}}},
    };
    return t;
}

// The table's entry for a missing font when its stand-in is the one drawn.
static const StandInWidths *measuredStandIn(const QString &family)
{
#ifdef Q_OS_MACOS
    // Qt's macOS text engine narrows a font differently (Gill Sans MT's
    // stand-in came out 6% narrower than on Windows and Linux): until that's
    // measured there, the Mac keeps the stand-ins' own widths.
    Q_UNUSED(family);
    return nullptr;
#endif
    const auto w = standInWidths().constFind(family);
    if (w == standInWidths().constEnd()) return nullptr;
    static QHash<QString, bool> drawn;   // main thread only, like all layout
    auto it = drawn.constFind(family);
    if (it == drawn.constEnd()) {
        QFont f(family);
        f.setFamilies({family});
        it = drawn.insert(family, QFontInfo(f).family().compare(QLatin1String(w->standIn), Qt::CaseInsensitive) == 0);
    }
    return *it ? &*w : nullptr;
}

// A style's measured value: bold italic falls back to bold, then italic or
// bold to regular.
template <class T>
static T byStyle(const T (&v)[4], bool bold, bool italic)
{
    const int i = (bold ? 1 : 0) + (italic ? 2 : 0);
    if (v[i]) return v[i];
    if (i == 3 && v[1]) return v[1];
    return v[0];
}

int substituteStretch(const QString &family, bool bold, bool italic)
{
    if (QFontDatabase::hasFamily(family)) return 100;
    if (const StandInWidths *w = measuredStandIn(family))
        if (const int s = byStyle(w->stretch, bold, italic)) return s;
    return stretches().value(family, 100);
}

double substituteSpaceEm(const QString &family, bool bold, bool italic)
{
    if (QFontDatabase::hasFamily(family)) return 0;
    if (const StandInWidths *w = measuredStandIn(family))
        if (const double em = byStyle(w->space, bold, italic); em > 0) return em;
    // AG_Futura's spaces are half an em (word gaps in reference PDFs of
    // book covers at 11, 16 and 36 pt); its stand-in Jost's are 0.3 em.
    if (family.compare(QLatin1String("AG_Futura"), Qt::CaseInsensitive) == 0) return 0.5;
    return 0;
}

int substituteWeight(const QString &family)
{
    static QHash<QString, int> cache;
    auto it = cache.constFind(family);
    if (it != cache.constEnd()) return *it;
    int w = 0;
    if (!QFontDatabase::hasFamily(family)) {
        const QString f = family.toLower();
        // Fonts whose names say nothing of the weight their stand-in is drawn at.
        static const QHash<QString, int> byName{
        {"arial rounded mt bold", 700},
        {"bauhaus 93", 700},
        {"britannic bold", 700},
        {"copperplate gothic bold", 700},
        {"eras bold itc", 700},
        {"gill sans mt ext condensed bold", 700},
        {"tw cen mt condensed extra bold", 700},
        };
        // Franklin Gothic's weights run heavy: its Medium looks like a SemiBold.
        if (const auto o = byName.constFind(f); o != byName.constEnd()) w = *o;
        else if (f.startsWith("franklin gothic") && f.contains("medium")) w = 600;
        else if (f.startsWith("franklin gothic") && f.contains("demi")) w = 700;
        else if (f.startsWith("franklin gothic") && f.contains("heavy")) w = 800;   // Libre Franklin ExtraBold
        else if (f.contains("black") || f.contains("heavy")) w = 900;
        else if (f.contains("extrabold") || f.contains("extra bold") || f.contains("ultra bold")) w = 800;
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
