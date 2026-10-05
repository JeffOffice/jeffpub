#pragma once
// Fill, outline, effects and text-wrap settings shared by all frame types.

#include "core/color.h"

#include <QBrush>
#include <QJsonObject>
#include <QPen>
#include <QPolygonF>
#include <QRectF>
#include <QVector>
#include <functional>

class QImage;

namespace jp {

struct GradientStop {
    double pos = 0;
    ColorRef color;
    double transparency = 0;
};

// Looks up a picture by id (used by picture/texture fills).
using ImageLookup = std::function<QImage(const QString &)>;

struct Fill {
    enum Type { NoFill, Solid, Gradient, Picture, Texture, Pattern };
    enum GradType { Linear, Radial, Rectangular, PathGrad };

    Type type = NoFill;
    ColorRef color;
    double transparency = 0;       // 0 opaque .. 1 clear

    GradType gradType = Linear;
    double angle = 90;             // degrees, 90 = top to bottom
    QVector<GradientStop> stops;   // when empty, color -> color2

    ColorRef color2;               // gradient end, pattern background
    QString imageId;               // picture and texture fills
    bool tile = false;
    double tileScale = 1.0;
    int pattern = 0;               // index into patternBrushes()

    static Fill none() { return Fill(); }
    static Fill solid(const ColorRef &c, double t = 0) { Fill f; f.type = Solid; f.color = c; f.transparency = t; return f; }
    static Fill gradient(const ColorRef &a, const ColorRef &b, double angle = 90) {
        Fill f; f.type = Gradient; f.color = a; f.color2 = b; f.angle = angle; return f;
    }
    bool isNone() const { return type == NoFill || (type == Solid && color.isNone()); }

    QBrush brush(const QRectF &r, const ColorScheme &s, const ImageLookup &img = {}) const;
    QJsonObject toJson() const;
    static Fill fromJson(const QJsonObject &o);
    bool operator==(const Fill &o) const { return toJson() == o.toJson(); }
};

enum class Arrow { None, Triangle, Open, Stealth, Diamond, Oval };

struct Stroke {
    enum Dash { SolidLine, RoundDot, SquareDot, DashLine, DashDot, LongDash, LongDashDot, LongDashDotDot };
    enum Compound { Single, Double, ThickThin, ThinThick, Triple };

    ColorRef color;
    double width = 0.75;           // points
    Dash dash = SolidLine;
    Compound compound = Single;
    double transparency = 0;
    Qt::PenJoinStyle join = Qt::MiterJoin;
    Qt::PenCapStyle cap = Qt::FlatCap;
    Arrow startArrow = Arrow::None, endArrow = Arrow::None;
    int startSize = 1, endSize = 1; // 0 small, 1 medium, 2 large

    static Stroke none() { return Stroke(); }
    static Stroke line(const ColorRef &c, double w = 0.75) { Stroke s; s.color = c; s.width = w; return s; }
    bool isNone() const { return color.isNone() || width <= 0; }

    QPen pen(const ColorScheme &s) const;
    QJsonObject toJson() const;
    static Stroke fromJson(const QJsonObject &o);
    bool operator==(const Stroke &o) const { return toJson() == o.toJson(); }
};

struct ShadowFx {
    bool on = false;
    ColorRef color = ColorRef::rgb(Qt::black);
    double transparency = 0.6;
    double blur = 4;       // points
    double distance = 3;   // points
    double angle = 45;     // degrees; 45 = down-right
    bool inner = false;
    QPointF offset() const;
};
struct GlowFx { bool on = false; ColorRef color = ColorRef::scheme(Accent1, 40); double size = 6; double transparency = 0.4; };
struct ReflectionFx { bool on = false; double transparency = 0.5; double size = 0.5; double distance = 0; double blur = 0.5; };
struct BevelFx { int type = 0; double width = 6; double height = 6; };       // 0 none, 1 circle, 2 relaxed, 3 cool slant, 4 angle, 5 soft round, 6 convex, 7 slope, 8 divot, 9 riblet, 10 hard edge, 11 art deco, 12 cross
struct Rotation3D { double x = 0, y = 0, perspective = 0; };

struct Effects {
    ShadowFx shadow;
    GlowFx glow;
    double softEdge = 0;   // points
    ReflectionFx reflection;
    BevelFx bevel;
    Rotation3D rot3d;
    bool any() const { return shadow.on || glow.on || softEdge > 0 || reflection.on || bevel.type || rot3d.x || rot3d.y; }
    QJsonObject toJson() const;
    static Effects fromJson(const QJsonObject &o);
};

struct Wrap {
    enum Mode { None, Square, Tight, Through, TopBottom };
    enum Side { Both, LeftOnly, RightOnly, Largest };
    Mode mode = Square;
    Side side = Both;
    double top = 2.88, bottom = 2.88, left = 2.88, right = 2.88; // 0.04 in
    QPolygonF points;     // custom wrap points (frame-local), used by Tight/Through when edited
    QJsonObject toJson() const;
    static Wrap fromJson(const QJsonObject &o);
};

QVector<Qt::BrushStyle> patternBrushes();
QString dashName(Stroke::Dash d);

} // namespace jp
