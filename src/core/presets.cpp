#include "core/presets.h"

namespace jp {

void applyTableFormatCells(TableItem *t, const QString &format, const std::function<void(TableCell &, bool, bool)> &textStyler)
{
    if (!t) return;
    t->format = format;
    struct Fmt { int header; int band; int border; bool firstCol; bool boxOnly; };
    int n = format.section(' ', -1).toInt();
    static const QStringList named = {"None", "Basic 1", "Basic 2", "Basic 3", "Checkbook Register", "List 1", "List 2", "List 3", "Numbers 1", "Numbers 2"};
    if (named.contains(format)) n = 100 + named.indexOf(format);
    Fmt f{Accent1, Accent1, Accent1, false, false};
    const int slots[] = {Accent1, Accent2, Accent3, Accent4, Main};
    if (n >= 1 && n < 100) {
        const int k = (n - 1) % 5, variant = (n - 1) / 5;
        f = Fmt{slots[k], slots[k], variant == 2 ? -1 : slots[k], variant == 3, variant == 1};
    } else if (n == 100) {
        f = Fmt{-1, -1, -1, false, false};
    } else if (n > 100) {
        const int k = n - 100;
        f = Fmt{k % 2 ? Main : Accent1, k % 3 ? Accent5 : -1, k % 2 ? Main : Accent4, k == 4, k == 5 || k == 1};
    }
    for (int r = 0; r < t->rows; ++r)
        for (int c = 0; c < t->cols; ++c) {
            TableCell &cell = t->cell(r, c);
            const bool head = t->header && r == 0 && f.header >= 0;
            const bool firstCol = f.firstCol && c == 0 && r > 0;
            if (head) cell.fill = Fill::solid(ColorRef::scheme(f.header));
            else if (firstCol) cell.fill = Fill::solid(ColorRef::scheme(f.header, 60));
            else if (f.band >= 0 && t->banded && r % 2 == 0) cell.fill = Fill::solid(ColorRef::scheme(f.band, 82));
            else cell.fill = Fill::none();
            const Stroke s = f.border >= 0 ? Stroke::line(ColorRef::scheme(f.border, 0, 15), 0.75) : Stroke::none();
            cell.border = CellBorder();
            if (f.boxOnly) {
                if (r == 0) cell.border.top = s;
                if (r == t->rows - 1) cell.border.bottom = s;
                if (c == 0) cell.border.left = s;
                if (c == t->cols - 1) cell.border.right = s;
                if (r == 0 && t->header) cell.border.bottom = s;
            } else {
                cell.border.top = cell.border.bottom = cell.border.left = cell.border.right = s;
            }
            if (textStyler) textStyler(cell, head, firstCol);
        }
}

// Six rows of styles per scheme slot: subtle, moderate, intense, outline,
// gradient and light-with-accent-border.
void shapeStylePreset(int row, int slot, Fill *fill, Stroke *stroke, Effects *fx)
{
    const ColorRef base = ColorRef::scheme(slot);
    fx->shadow.on = false;
    switch (row) {
    case 0: *fill = Fill::solid(ColorRef::scheme(slot, 80)); *stroke = Stroke::line(base, 1); break;
    case 1: *fill = Fill::solid(base); *stroke = Stroke::line(ColorRef::scheme(slot, 0, 30), 1); break;
    case 2: *fill = Fill::solid(ColorRef::scheme(slot, 0, 20)); *stroke = Stroke::none(); fx->shadow.on = true; fx->shadow.blur = 4; fx->shadow.distance = 3; break;
    case 3: *fill = Fill::none(); *stroke = Stroke::line(base, 2.25); break;
    case 4: *fill = Fill::gradient(ColorRef::scheme(slot, 40), ColorRef::scheme(slot, 0, 25), 90); *stroke = Stroke::none(); break;
    default: *fill = Fill::solid(ColorRef::rgb(Qt::white)); *stroke = Stroke::line(base, 3); stroke->compound = Stroke::Double; break;
    }
}

void pictureStylePreset(int k, PictureItem *pic)
{
    pic->maskShape = QStringLiteral("rect");
    pic->stroke = Stroke::none();
    pic->fx = Effects();
    const ColorRef white = ColorRef::rgb(Qt::white), dark = ColorRef::rgb(QColor(40, 40, 40));
    switch (k) {
    case 0: break;
    case 1: pic->stroke = Stroke::line(dark, 1); break;
    case 2: pic->stroke = Stroke::line(white, 6); pic->fx.shadow.on = true; pic->fx.shadow.blur = 6; break;
    case 3: pic->maskShape = "roundRect"; break;
    case 4: pic->maskShape = "roundRect"; pic->stroke = Stroke::line(white, 4); pic->fx.shadow.on = true; break;
    case 5: pic->maskShape = "ellipse"; break;
    case 6: pic->maskShape = "ellipse"; pic->stroke = Stroke::line(white, 5); pic->fx.shadow.on = true; break;
    case 7: pic->stroke = Stroke::line(dark, 8); break;
    case 8: pic->fx.shadow.on = true; pic->fx.shadow.blur = 10; pic->fx.shadow.distance = 6; break;
    case 9: pic->fx.softEdge = 10; break;
    case 10: pic->fx.reflection.on = true; pic->fx.reflection.size = 0.4; break;
    case 11: pic->maskShape = "snip2diag"; pic->stroke = Stroke::line(ColorRef::scheme(Accent1), 2); break;
    case 12: pic->maskShape = "octagon"; break;
    case 13: pic->maskShape = "hexagon"; pic->stroke = Stroke::line(ColorRef::scheme(Accent2), 2); break;
    case 14: pic->stroke = Stroke::line(ColorRef::scheme(Accent1), 4); pic->stroke.compound = Stroke::Double; break;
    case 15: pic->fx.rot3d.y = 25; pic->fx.rot3d.perspective = 40; pic->fx.shadow.on = true; break;
    case 16: pic->maskShape = "round1"; pic->stroke = Stroke::line(white, 3); break;
    case 17: pic->fx.glow.on = true; pic->fx.glow.size = 8; break;
    case 18: pic->fx.bevel.type = 1; pic->fx.bevel.width = 6; break;
    default: pic->maskShape = "plaque"; pic->stroke = Stroke::line(ColorRef::scheme(Accent3), 2); break;
    }
}

Fill backgroundPreset(const QString &id)
{
    if (id == QLatin1String("none")) return Fill();
    const QStringList p = id.split('.');
    if (p.value(0) == QLatin1String("solid")) return Fill::solid(ColorRef::scheme(p.value(1).toInt(), p.value(2).toInt()));
    if (p.value(0) == QLatin1String("grad")) {
        const int slot = p.value(1).toInt(), k = p.value(2).toInt();
        Fill f = Fill::gradient(ColorRef::scheme(slot, k == 2 ? 70 : 0), ColorRef::scheme(slot, k == 0 ? 85 : k == 1 ? 50 : 95), k == 1 ? 0 : 90);
        if (k == 2) f.gradType = Fill::Radial;
        return f;
    }
    return Fill();
}

} // namespace jp
