#include "core/color.h"

#include <QHash>
#include <algorithm>
#include <cmath>
#include <QStringList>

namespace jp {

QColor mix(const QColor &a, const QColor &b, double t)
{
    // Tints and shades of ink colors stay ink colors: toward no ink, or
    // toward black ink.
    if (a.spec() == QColor::Cmyk && (b == QColor(Qt::white) || b == QColor(Qt::black))) {
        const bool black = b == QColor(Qt::black);
        auto to = [t](float v, float target) { return float(v + (target - v) * t); };
        return QColor::fromCmykF(to(a.cyanF(), 0), to(a.magentaF(), 0), to(a.yellowF(), 0), to(a.blackF(), black ? 1 : 0), a.alphaF());
    }
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t), float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

QColor contrastText(const QColor &bg)
{
    const double l = 0.2126 * bg.redF() + 0.7152 * bg.greenF() + 0.0722 * bg.blueF();
    return l > 0.55 ? QColor(0x1a, 0x1a, 0x1a) : QColor(Qt::white);
}

QColor ColorRef::resolve(const ColorScheme &s) const
{
    switch (m_kind) {
    case None: return QColor(Qt::transparent);
    case Rgb: return m_rgb;
    case Scheme: {
        QColor c = s.slot(m_slot);
        if (m_lighten > 0) c = mix(c, Qt::white, m_lighten / 100.0);
        if (m_darken > 0) c = mix(c, Qt::black, m_darken / 100.0);
        return c;
    }
    }
    return QColor();
}

QString colorToString(const QColor &c)
{
    if (c.spec() == QColor::Cmyk) {
        auto pct = [](float v) { return QString::number(std::round(v * 10000.0) / 100.0, 'g', 8); };
        QString s = QStringLiteral("cmyk(%1,%2,%3,%4").arg(pct(c.cyanF()), pct(c.magentaF()), pct(c.yellowF()), pct(c.blackF()));
        if (c.alpha() != 255) s += QStringLiteral(",%1").arg(c.alpha());
        return s + QLatin1Char(')');
    }
    return c.alpha() == 255 ? c.name(QColor::HexRgb).toUpper() : c.name(QColor::HexArgb).toUpper();
}

QColor colorFromString(const QString &s)
{
    if (s.startsWith(QLatin1String("cmyk(")) && s.endsWith(QLatin1Char(')'))) {
        const QStringList v = s.mid(5, s.size() - 6).split(QLatin1Char(','));
        if (v.size() < 4) return QColor();
        auto f = [&](int i) { return float(std::clamp(v[i].trimmed().toDouble() / 100.0, 0.0, 1.0)); };
        QColor c = QColor::fromCmykF(f(0), f(1), f(2), f(3));
        if (v.size() > 4) c.setAlpha(std::clamp(v[4].trimmed().toInt(), 0, 255));
        return c;
    }
    return QColor(s);
}

QString ColorRef::toString() const
{
    switch (m_kind) {
    case None: return QStringLiteral("none");
    case Rgb: return colorToString(m_rgb);
    case Scheme: {
        QString s = QStringLiteral("@%1").arg(m_slot);
        if (m_lighten) s += QStringLiteral("+%1").arg(m_lighten);
        if (m_darken) s += QStringLiteral("-%1").arg(m_darken);
        return s;
    }
    }
    return {};
}

ColorRef ColorRef::fromString(const QString &s)
{
    if (s.isEmpty() || s == QLatin1String("none")) return none();
    if (s.startsWith('#') || s.startsWith(QLatin1String("cmyk("))) return rgb(colorFromString(s));
    if (s.startsWith('@')) {
        int i = 1, slot = 0, lighten = 0, darken = 0;
        while (i < s.size() && s[i].isDigit()) slot = slot * 10 + s[i++].digitValue();
        while (i < s.size()) {
            const QChar op = s[i++];
            int v = 0;
            while (i < s.size() && s[i].isDigit()) v = v * 10 + s[i++].digitValue();
            if (op == '+') lighten = v; else if (op == '-') darken = v;
        }
        return scheme(slot, lighten, darken);
    }
    return rgb(QColor(s));
}

QString slotName(int slot)
{
    static const char *n[] = {"Main", "Accent 1", "Accent 2", "Accent 3", "Accent 4", "Accent 5", "Hyperlink", "Followed Hyperlink"};
    return (slot >= 0 && slot < SlotCount) ? QString::fromLatin1(n[slot]) : QString();
}

QString ColorRef::displayName() const
{
    if (m_kind == None) return QStringLiteral("No Color");
    if (m_kind == Rgb && m_rgb.spec() == QColor::Cmyk)
        return QStringLiteral("C %1 M %2 Y %3 K %4").arg(std::round(m_rgb.cyanF() * 1000) / 10).arg(std::round(m_rgb.magentaF() * 1000) / 10)
            .arg(std::round(m_rgb.yellowF() * 1000) / 10).arg(std::round(m_rgb.blackF() * 1000) / 10);
    if (m_kind == Rgb) return m_rgb.name().toUpper();
    QString n = slotName(m_slot);
    if (m_lighten) n += QStringLiteral(" (Tint %1%)").arg(100 - m_lighten);
    if (m_darken) n += QStringLiteral(" (Shade %1%)").arg(100 - m_darken);
    return n;
}

// Name, Main, Accent 1–5. Hyperlink colors are derived.
static const char *kSchemes[][7] = {
    {"Ink", "#1E2430", "#1F5FAD", "#4F9DDE", "#E0A43A", "#7A8594", "#F2F5F9"},
    {"Harbor", "#12313F", "#1B7A8C", "#6BB8C4", "#F2A65A", "#8FA3AD", "#EEF6F7"},
    {"Orchard", "#2D2A1F", "#5E7F2E", "#A9C25D", "#D9573B", "#A39A84", "#F6F4EA"},
    {"Berry", "#2A1724", "#8E2A5E", "#D46BA0", "#F1B24A", "#8D7884", "#FAF0F5"},
    {"Citrus", "#26261A", "#D97B00", "#F6C431", "#3C8D5A", "#8C8A73", "#FFF9E8"},
    {"Slate", "#222831", "#3A4750", "#6F8796", "#D72323", "#A9B4BC", "#EEF0F1"},
    {"Meadow", "#1F2E24", "#2F7D4F", "#8BC28C", "#F0C24B", "#95A79A", "#F1F7F2"},
    {"Sunset", "#2B1B17", "#D4572A", "#F29E4C", "#6A4C93", "#A08F86", "#FDF2EA"},
    {"Lagoon", "#0F2A3D", "#0081A7", "#00AFB9", "#F07167", "#8AA6B5", "#FDFCEB"},
    {"Plum", "#261A2E", "#5B2A86", "#9A79C6", "#F2C14E", "#8E8496", "#F5F0FA"},
    {"Brick", "#2B1D1A", "#A23E2C", "#D9835E", "#3E6D7E", "#9C8B85", "#F7EFEA"},
    {"Regatta", "#141B2D", "#1D3461", "#376996", "#C9A227", "#8B93A6", "#F4F2EC"},
    {"Graphite", "#111111", "#333333", "#777777", "#C0392B", "#A6A6A6", "#F2F2F2"},
    {"Tropic", "#12302A", "#0B8457", "#3FC1A0", "#F9A03F", "#8FA89F", "#EFFAF5"},
    {"Lilac", "#2C2438", "#7A5CA8", "#C3A6E0", "#E86F8F", "#9C93A8", "#F7F3FB"},
    {"Earth", "#2E2A24", "#7B5E3B", "#B08D57", "#5F7A61", "#A69C8F", "#F4EFE6"},
    {"Cherry", "#2A1214", "#B3122E", "#E9536B", "#2E4A62", "#9E8A8C", "#FBEFF0"},
    {"Glacier", "#1B2733", "#2C6E91", "#94C9E0", "#5FAD8E", "#93A3B0", "#F0F7FB"},
    {"Marigold", "#2B2210", "#E2A100", "#F4D35E", "#0D3B66", "#A2987D", "#FAF6E8"},
    {"Seventy-Nine", "#2A1E14", "#C8553D", "#F28F3B", "#588B8B", "#A1907E", "#FFF5E6"},
    {"Alpine Lake", "#17263A", "#2A6F97", "#61A5C2", "#A9D6E5", "#89A0B0", "#F1F8FB"},
    {"Autumn Field", "#2E2216", "#9C4F1E", "#D98E32", "#6B7F2A", "#A5937E", "#FAF3E7"},
    {"Basalt", "#1C1C1E", "#3D3D42", "#6E6E76", "#E76F51", "#A1A1A8", "#F3F3F4"},
    {"Bayside", "#132A33", "#25707C", "#E9C46A", "#E76F51", "#8DA4AA", "#F4F9F9"},
    {"Birch", "#2B2A26", "#6D6A5F", "#B5AE9B", "#4E7D6B", "#A9A597", "#F6F4EE"},
    {"Blue Ridge", "#18233A", "#2B4C7E", "#567EBB", "#9FB8DA", "#8792A8", "#EEF2F9"},
    {"Bramble", "#2A1A20", "#6C2A3B", "#A85A6E", "#5E7348", "#977F86", "#F8F0F2"},
    {"Buttercup", "#2C2617", "#E5B700", "#F5DB6E", "#5C6F9E", "#A69E80", "#FFFBEA"},
    {"Canyon", "#2F1E16", "#B5502E", "#E09553", "#4D6E7B", "#A48A7C", "#FBF1EA"},
    {"Carnival", "#1F1A33", "#E63946", "#F4A261", "#2A9D8F", "#8C88A0", "#FFF6EE"},
    {"Cedar", "#232A23", "#3F5B3E", "#7E9A6A", "#B5652A", "#959E92", "#F2F5EF"},
    {"Chalkboard", "#1E2A26", "#2F4F46", "#E8E3D3", "#F2B134", "#8B9A94", "#F4F3EE"},
    {"Cobalt", "#101828", "#1849A9", "#528BFF", "#FEC84B", "#8590A6", "#EFF4FF"},
    {"Coral Reef", "#1D2B35", "#F26B5B", "#FFB38A", "#2EA7A0", "#93A1A8", "#FFF4F0"},
    {"Cranberry", "#2A1316", "#8B1E3F", "#C8486B", "#3B5249", "#9C8589", "#F9EEF1"},
    {"Dusk", "#1E1B2E", "#4A3F7A", "#8D7BC4", "#F4A259", "#8F8AA3", "#F4F2FA"},
    {"Espresso", "#22160F", "#5A3A22", "#9C6B43", "#C9A66B", "#9A8A7E", "#F7F1EA"},
    {"Fern", "#1B2A1F", "#3A7D44", "#9DC88D", "#F1B24A", "#8FA192", "#F1F8F1"},
    {"Fiesta", "#271B1B", "#D7263D", "#F46036", "#1B998B", "#9C8C8C", "#FFF3EC"},
    {"Fjord", "#142630", "#1F4E5F", "#5B8FA3", "#C5A880", "#8B9EA6", "#EFF5F7"},
    {"Flamingo", "#2B1A22", "#E05780", "#F7A6C0", "#2C7DA0", "#A08B94", "#FFF1F5"},
    {"Forest", "#14231A", "#1E5631", "#4C9A2A", "#A4DE02", "#86978A", "#F0F7EE"},
    {"Garnet", "#24121A", "#73122D", "#B2324F", "#D4A15A", "#988189", "#F8EEF1"},
    {"Harvest", "#2B2112", "#B3741B", "#E3B04B", "#6B4226", "#A49680", "#FBF5E8"},
    {"Heather", "#28212C", "#7D6B91", "#B9A9C9", "#6F9A8D", "#9D95A3", "#F6F3F8"},
    {"Honeycomb", "#2A2112", "#C88A12", "#F2C14E", "#3A3A3A", "#A39A85", "#FEF8E7"},
    {"Indigo Night", "#0F1226", "#283593", "#5C6BC0", "#FFB300", "#868AA8", "#EEF0FA"},
    {"Ivy", "#17251D", "#2D6A4F", "#74C69D", "#D8F3DC", "#89A095", "#F2F9F5"},
    {"Jade", "#13261F", "#00876C", "#6CC4A1", "#E5C687", "#88A39A", "#EEF8F4"},
    {"Juniper", "#1D2A2A", "#3E6259", "#86A397", "#C56C4A", "#93A19D", "#F2F6F4"},
    {"Kingfisher", "#10262E", "#0077B6", "#00B4D8", "#F77F00", "#87A2AD", "#EEF8FC"},
    {"Lavender Fields", "#29233A", "#6A5ACD", "#B8A9E8", "#7FB069", "#9A94AE", "#F5F3FC"},
    {"Lemonade", "#2A2A16", "#E9C400", "#F9E784", "#E85D75", "#A3A083", "#FFFDE9"},
    {"Lighthouse", "#1A2230", "#C1272D", "#2B4C7E", "#F2C14E", "#8F95A3", "#F4F6F9"},
    {"Mango", "#2B1E10", "#F08A24", "#FFC15E", "#2E8B57", "#A39380", "#FFF6E9"},
    {"Maple", "#2B1812", "#A63A1A", "#D96F32", "#F2C14E", "#A18A80", "#FAF0EA"},
    {"Midnight", "#0B0F1A", "#1B263B", "#415A77", "#E0E1DD", "#7C8796", "#EEF1F5"},
    {"Mint", "#173029", "#2BB673", "#A8E6CF", "#FF8B94", "#8EA89E", "#F0FBF6"},
    {"Moss", "#232617", "#5A6B2E", "#9CA65C", "#D9A441", "#9B9E88", "#F5F6EC"},
    {"Mulberry", "#24121F", "#6B2D5C", "#A55E8F", "#E8B04B", "#98838F", "#F7EFF4"},
    {"Nautical", "#101F33", "#1B3A5C", "#C1272D", "#E9D8A6", "#8592A3", "#F3F5F8"},
    {"Nectarine", "#2B1A14", "#F25C3D", "#FFA987", "#3F7CAC", "#A08C85", "#FFF3EE"},
    {"Nordic", "#1F2933", "#52606D", "#9AA5B1", "#E12D39", "#A3ACB5", "#F5F7FA"},
    {"Oasis", "#18282A", "#2A9D8F", "#E9C46A", "#F4A261", "#8FA3A2", "#F3FAF8"},
    {"Ochre", "#2A2011", "#B9770E", "#E2B659", "#3E5C76", "#A39583", "#FAF4E6"},
    {"Olive Grove", "#24261A", "#6B7B3A", "#A7B467", "#8C4A2F", "#9EA08B", "#F6F7EE"},
    {"Orchid", "#2A1B2D", "#9B4F96", "#D49BD0", "#4F9D8F", "#9E8EA0", "#FAF1FA"},
    {"Paprika", "#2A1611", "#C0392B", "#E67E22", "#27AE60", "#A28A83", "#FDF0EC"},
    {"Parchment", "#2E2A22", "#7A6A4F", "#B9A57F", "#8A3324", "#A69D8C", "#F7F2E7"},
    {"Peacock", "#10262B", "#005F73", "#0A9396", "#EE9B00", "#87A0A4", "#EEF7F7"},
    {"Pebble", "#2A2A2A", "#5E5E5E", "#9E9E9E", "#3C8DAD", "#B0B0B0", "#F4F4F4"},
    {"Pine", "#13211B", "#1B4D3E", "#4E8C6E", "#C9A227", "#859A90", "#EFF6F2"},
    {"Poppy", "#2A1313", "#D62828", "#F77F00", "#003049", "#A28686", "#FFF1EC"},
    {"Prairie", "#2C2618", "#A68A3E", "#D9C17A", "#6A8D73", "#A49E8B", "#FAF7EC"},
    {"Raspberry", "#2A1220", "#C2185B", "#F06292", "#00897B", "#A0848F", "#FDEEF4"},
    {"Redwood", "#2A1714", "#7F2E21", "#B85C38", "#5C7F67", "#A08883", "#F8EFEC"},
    {"Riviera", "#14243A", "#2E86AB", "#F6AE2D", "#F26419", "#8C9BAA", "#F1F7FB"},
    {"Saffron", "#2B2010", "#F4A300", "#FFD166", "#8338EC", "#A39882", "#FFF8E6"},
    {"Sage", "#232A25", "#6B8F71", "#AAC0AA", "#D1A87C", "#9AA49C", "#F3F7F3"},
    {"Sandstone", "#2E241C", "#B07D54", "#DDB892", "#7F5539", "#A69686", "#FAF4EC"},
    {"Sapphire", "#0F1A2E", "#0F52BA", "#6A9CE8", "#E3B23C", "#8693A8", "#EEF3FC"},
    {"Seaglass", "#1A2A2A", "#5FA8A0", "#B8E0D2", "#E07A5F", "#93A5A3", "#F2FAF8"},
    {"Sienna", "#2B1B13", "#A0522D", "#D2875C", "#4A6C6F", "#A08C82", "#FAF1EB"},
    {"Sky", "#172636", "#3A86FF", "#8EC5FF", "#FFBE0B", "#8E9CAD", "#F0F6FF"},
    {"Spruce", "#152224", "#2F5D62", "#5E8B7E", "#DFEEEA", "#8C9E9C", "#F1F6F5"},
    {"Storm", "#1A1F26", "#4A5568", "#718096", "#ECC94B", "#A0AEC0", "#F4F6F8"},
    {"Tangerine", "#2B1B10", "#FF7F11", "#FFB627", "#1B998B", "#A39383", "#FFF4E6"},
    {"Teal Tide", "#0F2626", "#00796B", "#4DB6AC", "#FF7043", "#86A09E", "#EDF7F6"},
    {"Terracotta", "#2C1C16", "#C46A4A", "#E8A87C", "#41729F", "#A59086", "#FBF1EB"},
    {"Thistle", "#28202C", "#8E7DBE", "#C9BBE6", "#99C1B9", "#9E96A6", "#F7F4FB"},
    {"Tidewater", "#13272E", "#3D7E8C", "#A3C9C7", "#D96C06", "#8EA2A6", "#F1F7F7"},
    {"Twilight", "#1A1528", "#5E4B8B", "#B27092", "#F2A65A", "#958CA5", "#F6F2F9"},
    {"Vineyard", "#22121A", "#5B1A3A", "#8E4A6B", "#9CAF5B", "#97818B", "#F6EEF2"},
    {"Wildflower", "#2A2030", "#C04E7A", "#F2A541", "#5B8E7D", "#A094A3", "#FCF2F6"},
    {"Willow", "#1F2A22", "#7FA36A", "#C6D8AF", "#6C5B7B", "#9AA597", "#F4F8F0"},
    {"Wisteria", "#262039", "#8566AA", "#C6B4E1", "#E8A0BF", "#9B93AC", "#F7F4FC"},
    {"Zinnia", "#2A1A1A", "#E4572E", "#FFC914", "#17BEBB", "#A38E8E", "#FFF5EE"},
};

const QVector<ColorScheme> &builtinColorSchemes()
{
    static QVector<ColorScheme> list = [] {
        QVector<ColorScheme> out;
        for (const auto &row : kSchemes) {
            ColorScheme s;
            s.name = QString::fromLatin1(row[0]);
            for (int i = 0; i < 6; ++i) s.c[i] = QColor(QLatin1String(row[i + 1]));
            // Hyperlinks: a dark, saturated blue-ish derived from Accent 1 for readability.
            QColor h = s.c[Accent1];
            s.c[Hyperlink] = h.lightnessF() > 0.45 ? h.darker(150) : h;
            QColor f = QColor::fromHslF(std::fmod(h.hslHueF() + 0.12f + 1.f, 1.f), 0.45f, 0.38f);
            s.c[Followed] = f;
            out.push_back(s);
        }
        return out;
    }();
    return list;
}

const ColorScheme *findColorScheme(const QString &name)
{
    for (const auto &s : builtinColorSchemes())
        if (s.name.compare(name, Qt::CaseInsensitive) == 0) return &s;
    return nullptr;
}

static const char *kFontSchemes[][3] = {
    {"Classic Sans", "Caladea", "Carlito"},
    {"Archival", "Georgia", "Georgia"},
    {"Bookish", "EB Garamond", "EB Garamond"},
    {"Civic", "Libre Baskerville", "Open Sans"},
    {"Editorial", "Playfair Display", "Source Serif 4"},
    {"Elegant", "Cormorant Garamond", "Lato"},
    {"Friendly", "Poppins", "Lora"},
    {"Headline", "Oswald", "Roboto"},
    {"Modern", "Montserrat", "Open Sans"},
    {"Poster", "Bebas Neue", "Raleway"},
    {"Retro", "Bungee", "Work Sans"},
    {"Script", "Great Vibes", "Libre Baskerville"},
    {"Slab", "Zilla Slab", "Nunito"},
    {"Typewriter", "Courier Prime", "Courier Prime"},
    {"Basic", "Liberation Sans", "Liberation Serif"},
    {"Newsprint", "Merriweather", "Merriweather"},
    {"Chalk", "Permanent Marker", "Nunito"},
    {"Handwritten", "Caveat", "Lora"},
    {"Festive", "Lobster", "Open Sans"},
    {"Seaside", "Pacifico", "Raleway"},
    {"Monument", "Cinzel", "EB Garamond"},
    {"Bold Display", "Abril Fatface", "Lato"},
    {"Workshop", "Alfa Slab One", "Work Sans"},
    {"Heavy", "Archivo Black", "Roboto"},
    {"Literary", "Fraunces", "Source Serif 4"},
    {"Dance", "Dancing Script", "Lato"},
    {"Playful", "Comic Neue", "Comic Neue"},
    {"Clean", "Raleway", "Lato"},
    {"Corporate", "Liberation Sans", "Liberation Sans"},
    {"Traditional", "Liberation Serif", "Liberation Serif"},
    {"Technical", "IBM Plex Sans", "IBM Plex Sans"},
    {"Contrast", "Archivo Black", "Libre Baskerville"},
    {"Studio", "Work Sans", "Work Sans"},
    {"Gazette", "Oswald", "Merriweather"},
    {"Boutique", "Cormorant Garamond", "Montserrat"},
    {"Market", "Bebas Neue", "Lora"},
    {"Campus", "Zilla Slab", "Open Sans"},
    {"Heritage", "Libre Baskerville", "Libre Baskerville"},
    {"Gallery", "Fraunces", "Work Sans"},
    {"Picnic", "Caveat", "Nunito"},
};

const QVector<FontScheme> &builtinFontSchemes()
{
    static QVector<FontScheme> list = [] {
        QVector<FontScheme> out;
        for (const auto &row : kFontSchemes)
            out.push_back({QString::fromLatin1(row[0]), QString::fromLatin1(row[1]), QString::fromLatin1(row[2])});
        return out;
    }();
    return list;
}

const FontScheme *findFontScheme(const QString &name)
{
    for (const auto &s : builtinFontSchemes())
        if (s.name.compare(name, Qt::CaseInsensitive) == 0) return &s;
    return nullptr;
}

} // namespace jp
