#include "render/textart.h"

#include "core/fonts.h"

#include <QCoreApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>

namespace jp {

const QVector<TextArtTransform> &textArtTransforms()
{
    static const QVector<TextArtTransform> t = {
        {"plain", QCoreApplication::translate("Render", "Plain Text")}, {"archUp", QCoreApplication::translate("Render", "Arch Up")},
        {"archDown", QCoreApplication::translate("Render", "Arch Down")}, {"circle", QCoreApplication::translate("Render", "Circle")},
        {"button", QCoreApplication::translate("Render", "Button")}, {"waveUp", QCoreApplication::translate("Render", "Wave 1")},
        {"waveDown", QCoreApplication::translate("Render", "Wave 2")}, {"doubleWave", QCoreApplication::translate("Render", "Double Wave")},
        {"inflate", QCoreApplication::translate("Render", "Inflate")}, {"deflate", QCoreApplication::translate("Render", "Deflate")},
        {"inflateTop", QCoreApplication::translate("Render", "Inflate Top")}, {"inflateBottom", QCoreApplication::translate("Render", "Inflate Bottom")},
        {"deflateTop", QCoreApplication::translate("Render", "Deflate Top")}, {"deflateBottom", QCoreApplication::translate("Render", "Deflate Bottom")},
        {"slantUp", QCoreApplication::translate("Render", "Slant Up")}, {"slantDown", QCoreApplication::translate("Render", "Slant Down")},
        {"triangleUp", QCoreApplication::translate("Render", "Triangle Up")}, {"triangleDown", QCoreApplication::translate("Render", "Triangle Down")},
        {"chevronUp", QCoreApplication::translate("Render", "Chevron Up")}, {"chevronDown", QCoreApplication::translate("Render", "Chevron Down")},
        {"fadeRight", QCoreApplication::translate("Render", "Fade Right")}, {"fadeLeft", QCoreApplication::translate("Render", "Fade Left")},
        {"fadeUp", QCoreApplication::translate("Render", "Fade Up")}, {"fadeDown", QCoreApplication::translate("Render", "Fade Down")},
        {"cascadeUp", QCoreApplication::translate("Render", "Cascade Up")}, {"cascadeDown", QCoreApplication::translate("Render", "Cascade Down")},
        {"canUp", QCoreApplication::translate("Render", "Can Up")}, {"canDown", QCoreApplication::translate("Render", "Can Down")},
        {"ringInside", QCoreApplication::translate("Render", "Ring Inside")}, {"ringOutside", QCoreApplication::translate("Render", "Ring Outside")},
    };
    return t;
}

// Builds the text outline in a unit-ish box: x in [0, totalW], y in [0, lineH * lines].
static QPainterPath textOutline(const TextArtItem &w, double *outW, double *outH)
{
    QFont f(w.font);
    f.setPixelSize(100);   // 1 em = 100 units
    // A missing font's spaces as wide as its own, where its stand-in's differ.
    const double spaceEm = substituteSpaceEm(w.font);
    f.setBold(w.bold);
    f.setItalic(w.italic);
    const QFontMetricsF fm(f);
    const QStringList lines = w.text.isEmpty() ? QStringList{QStringLiteral(" ")} : w.text.split('\n');
    QPainterPath path;
    double maxW = 0;
    const double lineH = fm.ascent() + fm.descent();
    QVector<double> widths;
    QVector<QPainterPath> linePaths;
    // Each line's letters where they sit, for justifying by words or letters.
    struct Glyph { QPainterPath path; double x; bool space; };
    QVector<QVector<Glyph>> glyphs(lines.size());
    for (int li = 0; li < lines.size(); ++li) {
        const QString &line = lines[li];
        QPainterPath lp;
        if (w.vertical) {
            double y = 0;
            for (const QChar ch : line) {
                QPainterPath cp;
                const double cw = fm.horizontalAdvance(ch);
                cp.addText(-cw / 2, y + fm.ascent(), f, QString(ch));
                lp.addPath(cp);
                y += lineH * w.spacing;
            }
            widths << lineH;
        } else {
            double x = 0;
            for (int i = 0; i < line.size(); ++i) {
                const QString ch = line.mid(i, 1);
                QPainterPath cp;
                cp.addText(0, fm.ascent(), f, ch);
                if (w.evenHeight && !ch.trimmed().isEmpty()) {
                    // Scale each glyph to the full cap height.
                    const QRectF br = cp.boundingRect();
                    if (br.height() > 1) {
                        QTransform t;
                        t.translate(0, fm.descent() * 0 + 0);
                        t.translate(0, fm.ascent());
                        t.scale(1, fm.ascent() / br.height());
                        t.translate(0, -br.bottom());
                        cp = t.map(cp);
                    }
                }
                lp.addPath(cp.translated(x, 0));
                glyphs[li].append({cp, x, ch.at(0).isSpace()});
                x += (spaceEm > 0 && ch == QLatin1String(" ") ? spaceEm * 100 : fm.horizontalAdvance(ch)) * w.spacing;
            }
            widths << x;
        }
        linePaths << lp;
        maxW = std::max(maxW, widths.last());
    }
    for (int li = 0; li < linePaths.size(); ++li) {
        double dx = 0;
        if (!w.vertical) {
            if (w.align == 1) dx = (maxW - widths[li]) / 2;
            else if (w.align == 2) dx = maxW - widths[li];
        }
        if (w.vertical) path.addPath(linePaths[li].translated(maxW * 0 + li * lineH * 1.1 + lineH / 2, 0));
        else {
            QPainterPath lp = linePaths[li];
            const double extra = maxW - widths[li];
            const auto &g = glyphs[li];
            if (w.align == 5 && widths[li] > 0 && extra > 0) {
                // Stretch Justify: the letters themselves widen.
                QTransform t;
                t.scale(maxW / widths[li], 1);
                lp = t.map(lp);
            } else if ((w.align == 3 || w.align == 4) && extra > 0) {
                // Word Justify: the spaces between words widen; Letter Justify:
                // every gap between letters. (Spaces at the ends don't count.)
                int first = 0, last = int(g.size()) - 1;
                while (first <= last && g[first].space) ++first;
                while (last >= first && g[last].space) --last;
                int gaps = 0;
                for (int i = first + 1; i <= last; ++i) gaps += w.align == 4 ? 1 : (g[i].space && !g[i - 1].space);
                if (gaps > 0) {
                    lp = QPainterPath();
                    int passed = 0;
                    for (int i = 0; i < g.size(); ++i) {
                        if (i > first && i <= last && (w.align == 4 || (g[i].space && !g[i - 1].space))) ++passed;
                        lp.addPath(g[i].path.translated(g[i].x + extra * passed / gaps, 0));
                    }
                }
            }
            path.addPath(lp.translated(dx, li * lineH));
        }
    }
    const QRectF br = path.boundingRect();
    *outW = std::max(1.0, w.vertical ? br.right() : maxW);
    *outH = std::max(1.0, w.vertical ? br.bottom() : lineH * lines.size());
    return path;
}

QPainterPath textArtPath(const TextArtItem &w, const QSizeF &size)
{
    double tw = 1, th = 1;
    QPainterPath src = textOutline(w, &tw, &th);
    const double k = std::clamp(w.transformAdj, 0.0, 1.0);
    const QString t = w.transform_;

    // Publisher's circle: the line runs clockwise round the ellipse the frame
    // holds, from just above 9 o'clock to just below it (its default angle,
    // -179°, from the shape's definition), shrunk or grown as a whole until
    // the text, spaces and all, fills the path. The path runs 0.31 of a
    // capital's height above the baseline, so letters may reach past the
    // frame, as in Publisher (measured on Publisher's PDFs of two book covers:
    // start angle, where the text ends, the letters' inner and outer edges).
    if (t == "circle" && !w.vertical) {
        const double rx = size.width() / 2, ry = size.height() / 2;
        const double start = qDegreesToRadians(-179.0), sweep = qDegreesToRadians(358.0);
        constexpr int kSteps = 720;
        QVector<double> along(kSteps + 1, 0.0);   // path length up to each step
        for (int i = 1; i <= kSteps; ++i) {
            const double a0 = start + sweep * (i - 1) / kSteps, a1 = start + sweep * i / kSteps;
            along[i] = along[i - 1] + std::hypot(rx * (std::cos(a1) - std::cos(a0)), ry * (std::sin(a1) - std::sin(a0)));
        }
        const double len = along[kSteps];
        if (len <= 0) return QPainterPath();
        const double sc = len / tw;   // text units to points
        QFont f(w.font);
        f.setPixelSize(100);   // as textOutline
        f.setBold(w.bold);
        f.setItalic(w.italic);
        const QFontMetricsF fm(f);
        const double lineH = fm.ascent() + fm.descent();
        const double baseline = th / 2 - lineH / 2 + fm.ascent();   // the middle line's
        const double onPath = baseline - 0.31 * fm.capHeight();
        auto place = [&](const QPointF &q) {
            const double d = std::clamp(q.x() * sc, 0.0, len);
            const int i = int(std::upper_bound(along.cbegin(), along.cend(), d) - along.cbegin());
            const int hi = std::clamp(i, 1, kSteps);
            const double f = (d - along[hi - 1]) / std::max(1e-9, along[hi] - along[hi - 1]);
            const double a = start + sweep * (hi - 1 + f) / kSteps;
            const double off = (onPath - q.y()) * sc;   // outward from the path
            return QPointF(rx + (rx + off) * std::cos(a), ry + (ry + off) * std::sin(a));
        };
        QPainterPath res;
        res.setFillRule(Qt::OddEvenFill);
        for (const QPolygonF &pl : src.toSubpathPolygons()) {
            QPolygonF bent;
            for (int i = 0; i < pl.size(); ++i) {
                const QPointF a = pl[i], b = pl[(i + 1) % pl.size()];
                const int steps = std::max(1, int(std::hypot(b.x() - a.x(), b.y() - a.y()) / (tw * 0.002)));
                for (int k = 0; k < steps; ++k) bent << place(a + (b - a) * (double(k) / steps));
            }
            res.addPolygon(bent);
            res.closeSubpath();
        }
        res.setFillRule(Qt::WindingFill);
        return res;
    }

    // Warp maps normalized (u, v) in [0,1]² to an arbitrary plane; the result
    // is stretched to the frame afterwards.
    std::function<QPointF(double, double)> warp;
    if (t == "archUp" || t == "archDown" || t == "circle" || t == "ringInside" || t == "ringOutside" || t == "button") {
        const double span = (t == "circle" ? 1.9 * M_PI : t == "button" ? 1.0 * M_PI : (0.6 + k * 1.4) * M_PI / 1.0 * 0.5 + 0.4);   // circle: vertical text
        const double thick = 0.35;
        const bool down = (t == "archDown" || t == "ringInside");
        warp = [=](double u, double v) {
            const double ang = (u - 0.5) * span;
            const double r = down ? (1 - thick) + v * thick : 1 - v * thick;
            return down ? QPointF(std::sin(ang) * r, std::cos(ang) * r) : QPointF(std::sin(ang) * r, -std::cos(ang) * r);
        };
    } else if (t == "waveUp" || t == "waveDown") {
        const double a = 0.12 + k * 0.25, s = t == "waveUp" ? 1 : -1;
        warp = [=](double u, double v) { return QPointF(u, v + s * a * std::sin(2 * M_PI * u)); };
    } else if (t == "doubleWave") {
        const double a = 0.08 + k * 0.2;
        warp = [=](double u, double v) { return QPointF(u, v + a * std::sin(4 * M_PI * u)); };
    } else if (t == "inflate" || t == "deflate" || t == "inflateTop" || t == "inflateBottom" || t == "deflateTop" || t == "deflateBottom") {
        const double a = 0.25 + k * 0.5;
        const bool infl = t.startsWith("inflate");
        const int part = t.endsWith("Top") ? 1 : t.endsWith("Bottom") ? 2 : 0;
        warp = [=](double u, double v) {
            const double bump = std::sin(M_PI * u) * a * (infl ? 1 : -1);
            double top = 0, bot = 1;
            if (part != 2) top = -bump * 0.5;
            if (part != 1) bot = 1 + bump * 0.5;
            return QPointF(u, top + (bot - top) * v);
        };
    } else if (t == "slantUp" || t == "slantDown") {
        const double a = 0.3 + k * 0.6, s = t == "slantUp" ? -1 : 1;
        warp = [=](double u, double v) { return QPointF(u, v + s * a * (u - 0.5)); };
    } else if (t == "triangleUp" || t == "triangleDown") {
        const bool up = t == "triangleUp";
        warp = [=](double u, double v) {
            const double peak = 1 - std::abs(u - 0.5) * 2;
            const double top = up ? -peak * 0.5 : 0, bot = up ? 1 : 1 + peak * 0.5;
            return QPointF(u, top + (bot - top) * v);
        };
    } else if (t == "chevronUp" || t == "chevronDown") {
        const double a = 0.25 + 0.5 * k, s = t == "chevronUp" ? -1 : 1;
        warp = [=](double u, double v) { return QPointF(u, v + s * a * (1 - std::abs(u - 0.5) * 2)); };
    } else if (t == "fadeRight" || t == "fadeLeft") {
        const double a = 0.3 + 0.5 * k;
        const bool right = t == "fadeRight";
        warp = [=](double u, double v) {
            const double sc = 1 - a * (right ? u : 1 - u);
            return QPointF(u, 0.5 + (v - 0.5) * sc);
        };
    } else if (t == "fadeUp" || t == "fadeDown") {
        const double a = 0.3 + 0.5 * k;
        const bool up = t == "fadeUp";
        warp = [=](double u, double v) {
            const double sc = 1 - a * (up ? 1 - v : v);
            return QPointF(0.5 + (u - 0.5) * sc, v);
        };
    } else if (t == "cascadeUp" || t == "cascadeDown") {
        const double s = t == "cascadeUp" ? 1 : -1;
        warp = [=](double u, double v) {
            const double sc = 0.4 + 0.6 * (s > 0 ? u : 1 - u);
            return QPointF(u, (1 - sc) + v * sc);
        };
    } else if (t == "canUp" || t == "canDown") {
        const double a = 0.2 + 0.4 * k, s = t == "canUp" ? -1 : 1;
        warp = [=](double u, double v) { return QPointF(u, v + s * a * std::sin(M_PI * u)); };
    }

    QPainterPath out;
    if (!warp) {
        out = src;
    } else {
        QTransform norm;
        norm.scale(1.0 / tw, 1.0 / th);
        const QList<QPolygonF> polys = src.toSubpathPolygons(norm);
        out.setFillRule(Qt::OddEvenFill);
        for (const QPolygonF &pl : polys) {
            QPolygonF dense;
            for (int i = 0; i < pl.size(); ++i) {
                const QPointF a = pl[i], b = pl[(i + 1) % pl.size()];
                const int steps = std::max(1, int(std::hypot(b.x() - a.x(), b.y() - a.y()) / 0.01));
                for (int s = 0; s < steps; ++s) {
                    const double f = double(s) / steps;
                    const QPointF m = a + (b - a) * f;
                    dense << warp(m.x(), m.y());
                }
            }
            out.addPolygon(dense);
            out.closeSubpath();
        }
    }
    // Stretch to the frame.
    const QRectF br = out.boundingRect();
    if (br.width() <= 0 || br.height() <= 0) return QPainterPath();
    QTransform fit;
    fit.scale(size.width() / br.width(), size.height() / br.height());
    fit.translate(-br.left(), -br.top());
    QPainterPath res = fit.map(out);
    res.setFillRule(Qt::WindingFill);
    return res;
}

static TextArtStyle mk(const char *id, const char *name, const char *font, Fill fill, Stroke stroke, bool shadow = false,
                       const char *transform = "plain", bool bold = false, bool italic = false)
{
    TextArtStyle s;
    s.id = QString::fromLatin1(id);
    s.name = QCoreApplication::translate("Render", name);
    s.font = QString::fromLatin1(font);
    s.fill = fill;
    s.stroke = stroke;
    s.shadow.on = shadow;
    s.shadow.distance = 4;
    s.shadow.blur = 2;
    s.shadow.transparency = 0.55;
    s.transform = QString::fromLatin1(transform);
    s.bold = bold;
    s.italic = italic;
    return s;
}

const QVector<TextArtStyle> &textArtStyles()
{
    static const QVector<TextArtStyle> v = [] {
        const ColorRef a1 = ColorRef::scheme(Accent1), a2 = ColorRef::scheme(Accent2), a3 = ColorRef::scheme(Accent3), main = ColorRef::scheme(Main);
        const ColorRef white = ColorRef::rgb(Qt::white), black = ColorRef::rgb(QColor(30, 30, 30));
        QVector<TextArtStyle> s;
        s << mk("fill-main", QT_TRANSLATE_NOOP("Render", "Fill: Main"), "Archivo Black", Fill::solid(main), Stroke::none());
        s << mk("fill-a1", QT_TRANSLATE_NOOP("Render", "Fill: Accent 1"), "Archivo Black", Fill::solid(a1), Stroke::none());
        s << mk("fill-a1-shadow", QT_TRANSLATE_NOOP("Render", "Fill: Accent 1, Shadow"), "Archivo Black", Fill::solid(a1), Stroke::none(), true);
        s << mk("outline-a1", QT_TRANSLATE_NOOP("Render", "Outline: Accent 1"), "Archivo Black", Fill::solid(white), Stroke::line(a1, 2));
        s << mk("fill-a2-outline", QT_TRANSLATE_NOOP("Render", "Fill: Accent 2, Outline: Main"), "Bebas Neue", Fill::solid(a2), Stroke::line(main, 1.5));
        s << mk("grad-a1-a2", QT_TRANSLATE_NOOP("Render", "Gradient: Accent 1 to Accent 2"), "Archivo Black", Fill::gradient(a1, a2, 90), Stroke::none());
        s << mk("grad-gold", QT_TRANSLATE_NOOP("Render", "Gradient: Gold"), "Abril Fatface", Fill::gradient(ColorRef::rgb(QColor("#F7D774")), ColorRef::rgb(QColor("#A8741A")), 90),
                Stroke::line(ColorRef::rgb(QColor("#7A5210")), 1), true);
        s << mk("grad-silver", QT_TRANSLATE_NOOP("Render", "Gradient: Silver"), "Archivo Black", Fill::gradient(ColorRef::rgb(QColor("#F4F4F4")), ColorRef::rgb(QColor("#8A8A8A")), 90),
                Stroke::line(ColorRef::rgb(QColor("#5A5A5A")), 1), true);
        s << mk("grad-sunset", QT_TRANSLATE_NOOP("Render", "Gradient: Sunset"), "Lobster", Fill::gradient(ColorRef::rgb(QColor("#FFB347")), ColorRef::rgb(QColor("#E0405E")), 0), Stroke::none(), true);
        s << mk("grad-ocean", QT_TRANSLATE_NOOP("Render", "Gradient: Ocean"), "Archivo Black", Fill::gradient(ColorRef::rgb(QColor("#56CCF2")), ColorRef::rgb(QColor("#1B3F8B")), 90), Stroke::none());
        s << mk("rainbow", QT_TRANSLATE_NOOP("Render", "Rainbow"), "Archivo Black", [] {
            Fill f; f.type = Fill::Gradient; f.angle = 0;
            const char *c[] = {"#E53935", "#FB8C00", "#FDD835", "#43A047", "#1E88E5", "#8E24AA"};
            for (int i = 0; i < 6; ++i) f.stops << GradientStop{i / 5.0, ColorRef::rgb(QColor(c[i])), 0};
            return f; }(), Stroke::none());
        s << mk("arch-a1", QT_TRANSLATE_NOOP("Render", "Arch: Accent 1"), "Archivo Black", Fill::solid(a1), Stroke::none(), false, "archUp");
        s << mk("arch-down-a2", QT_TRANSLATE_NOOP("Render", "Arch Down: Accent 2"), "Archivo Black", Fill::solid(a2), Stroke::none(), false, "archDown");
        s << mk("wave-a3", QT_TRANSLATE_NOOP("Render", "Wave: Accent 3"), "Bungee", Fill::solid(a3), Stroke::line(main, 1), false, "waveUp");
        s << mk("inflate-a1", QT_TRANSLATE_NOOP("Render", "Inflate: Accent 1"), "Anton", Fill::solid(a1), Stroke::none(), true, "inflate");
        s << mk("slant-main", QT_TRANSLATE_NOOP("Render", "Slant Up: Main"), "Oswald", Fill::solid(main), Stroke::none(), false, "slantUp", true);
        s << mk("fade-right", QT_TRANSLATE_NOOP("Render", "Fade Right: Accent 1"), "Archivo Black", Fill::solid(a1), Stroke::none(), false, "fadeRight");
        s << mk("chevron", QT_TRANSLATE_NOOP("Render", "Chevron Up: Accent 2"), "Bebas Neue", Fill::solid(a2), Stroke::line(main, 1), false, "chevronUp");
        s << mk("circle", QT_TRANSLATE_NOOP("Render", "Circle: Accent 1"), "Archivo Black", Fill::solid(a1), Stroke::none(), false, "circle");
        s << mk("triangle", QT_TRANSLATE_NOOP("Render", "Triangle Up: Accent 1"), "Anton", Fill::solid(a1), Stroke::none(), false, "triangleUp");
        s << mk("can", QT_TRANSLATE_NOOP("Render", "Can Up: Main"), "Archivo Black", Fill::solid(main), Stroke::none(), false, "canUp");
        s << mk("cascade", QT_TRANSLATE_NOOP("Render", "Cascade Up: Accent 1"), "Archivo Black", Fill::gradient(a1, a2, 0), Stroke::none(), false, "cascadeUp");
        s << mk("script", QT_TRANSLATE_NOOP("Render", "Script: Accent 1"), "Great Vibes", Fill::solid(a1), Stroke::none(), true);
        s << mk("retro", QT_TRANSLATE_NOOP("Render", "Retro: Accent 2"), "Bungee", Fill::solid(a2), Stroke::line(black, 2), true);
        s << mk("chalk", QT_TRANSLATE_NOOP("Render", "Chalk: White"), "Permanent Marker", Fill::solid(white), Stroke::line(black, 0.5));
        s << mk("classic-serif", QT_TRANSLATE_NOOP("Render", "Classic Serif"), "Playfair Display", Fill::solid(main), Stroke::none(), true, "plain", true);
        s << mk("pop", QT_TRANSLATE_NOOP("Render", "Pop: Accent 3"), "Alfa Slab One", Fill::solid(a3), Stroke::line(main, 2.5), true);
        s << mk("deflate", QT_TRANSLATE_NOOP("Render", "Deflate: Accent 2"), "Anton", Fill::solid(a2), Stroke::none(), false, "deflate");
        s << mk("button", QT_TRANSLATE_NOOP("Render", "Button: Main"), "Archivo Black", Fill::solid(main), Stroke::none(), false, "button");
        s << mk("double-wave", QT_TRANSLATE_NOOP("Render", "Double Wave: Accent 1"), "Lobster", Fill::gradient(a1, a3, 90), Stroke::none(), true, "doubleWave");
        return s;
    }();
    return v;
}

void applyTextArtStyle(TextArtItem &w, const TextArtStyle &s)
{
    w.font = s.font;
    w.bold = s.bold;
    w.italic = s.italic;
    w.fill = s.fill;
    w.stroke = s.stroke;
    w.fx.shadow = s.shadow;
    w.transform_ = s.transform;
    w.evenHeight = s.evenHeight;
    w.styleId = s.id;
}

} // namespace jp
