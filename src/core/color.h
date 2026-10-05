#pragma once
// Scheme-aware colors. A ColorRef either names a fixed RGB color or one of the
// eight color-scheme slots, optionally lightened (tint) or darkened (shade).
// Switching the publication's color scheme recolors every scheme reference.

#include <QColor>
#include <QString>
#include <QVector>

namespace jp {

enum SchemeSlot { Main = 0, Accent1, Accent2, Accent3, Accent4, Accent5, Hyperlink, Followed, SlotCount };

struct ColorScheme {
    QString name;
    QColor c[SlotCount];
    QColor slot(int i) const { return (i >= 0 && i < SlotCount) ? c[i] : QColor(Qt::black); }
};

struct FontScheme {
    QString name;
    QString heading;  // "major" font
    QString body;     // "minor" font
};

class ColorRef {
public:
    enum Kind { None, Rgb, Scheme };

    ColorRef() = default;
    static ColorRef none() { return ColorRef(); }
    static ColorRef rgb(const QColor &c) { ColorRef r; r.m_kind = Rgb; r.m_rgb = c; return r; }
    static ColorRef scheme(int slot, int lighten = 0, int darken = 0) {
        ColorRef r; r.m_kind = Scheme; r.m_slot = slot; r.m_lighten = lighten; r.m_darken = darken; return r;
    }

    Kind kind() const { return m_kind; }
    bool isNone() const { return m_kind == None; }
    int slot() const { return m_slot; }
    int lighten() const { return m_lighten; }
    int darken() const { return m_darken; }
    QColor rgbValue() const { return m_rgb; }

    QColor resolve(const ColorScheme &s) const;
    QString toString() const;                    // "none", "#RRGGBB", "@1", "@1+40", "@1-25"
    static ColorRef fromString(const QString &s);
    QString displayName() const;                 // "Accent 1 (Tint 40%)"

    bool operator==(const ColorRef &o) const {
        return m_kind == o.m_kind && (m_kind != Rgb || m_rgb == o.m_rgb) &&
               (m_kind != Scheme || (m_slot == o.m_slot && m_lighten == o.m_lighten && m_darken == o.m_darken));
    }
    bool operator!=(const ColorRef &o) const { return !(*this == o); }

private:
    Kind m_kind = None;
    QColor m_rgb;
    int m_slot = 0;
    int m_lighten = 0;  // percent of white mixed in
    int m_darken = 0;   // percent of black mixed in
};

QString slotName(int slot);
const QVector<ColorScheme> &builtinColorSchemes();
const ColorScheme *findColorScheme(const QString &name);
const QVector<FontScheme> &builtinFontSchemes();
const FontScheme *findFontScheme(const QString &name);

QColor mix(const QColor &a, const QColor &b, double t);  // t = share of b
QColor contrastText(const QColor &bg);

} // namespace jp
