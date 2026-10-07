#include "core/svg.h"

#include <QHash>
#include <QRegularExpression>
#include <QTransform>
#include <QXmlStreamReader>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace jp::svg {

namespace {

const QString svgNs = QStringLiteral("http://www.w3.org/2000/svg");
const QString xlinkNs = QStringLiteral("http://www.w3.org/1999/xlink");

struct Node {
    QString name;
    QHash<QString, QString> attrs;
    QVector<int> children;
    QString text;               // a <style> element's CSS
};

// Reads numbers the way SVG writes them: "1.5.5" is 1.5 then .5, "1-2" is 1
// then -2, and commas count as spaces.
class Scanner {
public:
    explicit Scanner(const QString &s) : m_s(s) {}
    bool atEnd() { skip(); return m_i >= m_s.size(); }
    QChar peek() { skip(); return m_i < m_s.size() ? m_s[m_i] : QChar(); }
    QChar take() { skip(); return m_i < m_s.size() ? m_s[m_i++] : QChar(); }
    bool number(double *out)
    {
        skip();
        const qsizetype n = m_s.size();
        qsizetype i = m_i;
        if (i < n && (m_s[i] == '+' || m_s[i] == '-')) ++i;
        bool digits = false;
        while (i < n && m_s[i] >= '0' && m_s[i] <= '9') { ++i; digits = true; }
        if (i < n && m_s[i] == '.')
            for (++i; i < n && m_s[i] >= '0' && m_s[i] <= '9'; ++i) digits = true;
        if (!digits) return false;
        if (i < n && (m_s[i] == 'e' || m_s[i] == 'E')) {
            qsizetype j = i + 1;
            if (j < n && (m_s[j] == '+' || m_s[j] == '-')) ++j;
            if (j < n && m_s[j] >= '0' && m_s[j] <= '9') {
                while (j < n && m_s[j] >= '0' && m_s[j] <= '9') ++j;
                i = j;
            }
        }
        *out = QStringView(m_s).mid(m_i, i - m_i).toDouble();
        m_i = i;
        return true;
    }
    // Arc flags are single digits that may run together: "a1 1 0 001 1".
    bool flag(bool *out)
    {
        skip();
        if (m_i < m_s.size() && (m_s[m_i] == '0' || m_s[m_i] == '1')) {
            *out = m_s[m_i++] == '1';
            return true;
        }
        return false;
    }

private:
    void skip() { while (m_i < m_s.size() && (m_s[m_i].isSpace() || m_s[m_i] == ',')) ++m_i; }
    const QString &m_s;
    qsizetype m_i = 0;
};

QVector<double> numbers(const QString &s)
{
    QVector<double> v;
    Scanner sc(s);
    double x;
    while (sc.number(&x)) v << x;
    return v;
}

// A length in user units; `ref` is what 100% means.
double length(const QString &in, double ref, double def = 0)
{
    const QString s = in.trimmed();
    static const QRegularExpression re(QStringLiteral("^([+-]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][+-]?\\d+)?)\\s*([a-zA-Z%]*)$"));
    const auto m = re.match(s);
    if (!m.hasMatch()) return def;
    const double v = m.captured(1).toDouble();
    const QString u = m.captured(2).toLower();
    if (u.isEmpty() || u == QLatin1String("px")) return v;
    if (u == QLatin1String("pt")) return v * 4.0 / 3.0;
    if (u == QLatin1String("pc")) return v * 16;
    if (u == QLatin1String("in")) return v * 96;
    if (u == QLatin1String("cm")) return v * 96 / 2.54;
    if (u == QLatin1String("mm")) return v * 96 / 25.4;
    if (u == QLatin1String("em")) return v * 16;
    if (u == QLatin1String("ex")) return v * 8;
    if (u == QLatin1String("%")) return v * ref / 100;
    return v;
}

QTransform transformOf(const QString &s)
{
    QTransform m;
    static const QRegularExpression re(QStringLiteral("(matrix|translate|scale|rotate|skewX|skewY)\\s*\\(([^)]*)\\)"));
    for (auto it = re.globalMatch(s); it.hasNext();) {
        const auto mt = it.next();
        const QVector<double> v = numbers(mt.captured(2));
        const QString f = mt.captured(1);
        QTransform t;
        if (v.isEmpty()) continue;
        if (f == QLatin1String("matrix") && v.size() >= 6) t = QTransform(v[0], v[1], v[2], v[3], v[4], v[5]);
        else if (f == QLatin1String("translate")) t.translate(v[0], v.value(1));
        else if (f == QLatin1String("scale")) t.scale(v[0], v.size() > 1 ? v[1] : v[0]);
        else if (f == QLatin1String("rotate")) {
            if (v.size() >= 3) t.translate(v[1], v[2]);
            t.rotate(v[0]);
            if (v.size() >= 3) t.translate(-v[1], -v[2]);
        } else if (f == QLatin1String("skewX")) t.shear(std::tan(qDegreesToRadians(v[0])), 0);
        else if (f == QLatin1String("skewY")) t.shear(0, std::tan(qDegreesToRadians(v[0])));
        // The first transform in the list is the outermost.
        m = t * m;
    }
    return m;
}

struct Style {
    QString fill = QStringLiteral("black"), stroke = QStringLiteral("none");
    double strokeWidth = 1, fillOpacity = 1, strokeOpacity = 1, opacity = 1;
    Qt::PenCapStyle cap = Qt::FlatCap;
    Qt::PenJoinStyle join = Qt::MiterJoin;
    bool evenOdd = false, hidden = false;
    QColor color;
    QVector<double> dashes;
    double dashOffset = 0;
};

const QStringList &styleProperties()
{
    static const QStringList p = {QStringLiteral("fill"), QStringLiteral("stroke"), QStringLiteral("stroke-width"),
                                  QStringLiteral("fill-opacity"), QStringLiteral("stroke-opacity"), QStringLiteral("opacity"),
                                  QStringLiteral("stroke-linecap"), QStringLiteral("stroke-linejoin"), QStringLiteral("fill-rule"),
                                  QStringLiteral("visibility"), QStringLiteral("display"), QStringLiteral("color"),
                                  QStringLiteral("stroke-dasharray"), QStringLiteral("stroke-dashoffset")};
    return p;
}

using Decls = QVector<QPair<QString, QString>>;

Decls declarations(const QString &css)
{
    Decls d;
    for (const QString &part : css.split(';')) {
        const qsizetype colon = part.indexOf(':');
        if (colon < 0) continue;
        QString value = part.mid(colon + 1).trimmed();
        value.remove(QLatin1String("!important")).squeeze();
        d << qMakePair(part.left(colon).trimmed().toLower(), value.trimmed());
    }
    return d;
}

struct Rule {
    QString element, id;
    QStringList classes;
    int specificity = 0;
    Decls decls;
};

QVector<Rule> styleSheet(QString css)
{
    static const QRegularExpression comments(QStringLiteral("/\\*.*?\\*/"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression block(QStringLiteral("([^{}]+)\\{([^}]*)\\}"));
    static const QRegularExpression simple(QStringLiteral("^([a-zA-Z][\\w-]*|\\*)?((?:[.#][\\w-]+)*)$"));
    static const QRegularExpression part(QStringLiteral("([.#])([\\w-]+)"));
    css.remove(comments);
    QVector<Rule> rules;
    for (auto it = block.globalMatch(css); it.hasNext();) {
        const auto b = it.next();
        const Decls decls = declarations(b.captured(2));
        for (QString sel : b.captured(1).split(',')) {
            // Only the last simple selector counts: "svg .a" is read as ".a".
            sel = sel.trimmed().split(QRegularExpression(QStringLiteral("[\\s>+~]+"))).last();
            const auto m = simple.match(sel);
            if (!m.hasMatch() || sel.isEmpty() || sel.startsWith('@')) continue;
            Rule r;
            r.element = m.captured(1) == QLatin1String("*") ? QString() : m.captured(1);
            for (auto pi = part.globalMatch(m.captured(2)); pi.hasNext();) {
                const auto p = pi.next();
                if (p.captured(1) == QLatin1String("#")) r.id = p.captured(2);
                else r.classes << p.captured(2);
            }
            r.specificity = (r.id.isEmpty() ? 0 : 100) + 10 * int(r.classes.size()) + (r.element.isEmpty() ? 0 : 1);
            r.decls = decls;
            rules << r;
        }
    }
    std::stable_sort(rules.begin(), rules.end(), [](const Rule &a, const Rule &b) { return a.specificity < b.specificity; });
    return rules;
}

QColor rgbFunction(const QString &v)
{
    static const QRegularExpression re(QStringLiteral("^rgba?\\(([^)]*)\\)$"));
    static const QRegularExpression sep(QStringLiteral("[\\s,/]+"));
    const auto m = re.match(v);
    if (!m.hasMatch()) return {};
    const QStringList parts = m.captured(1).split(sep, Qt::SkipEmptyParts);
    if (parts.size() < 3) return {};
    auto channel = [](const QString &p) {
        return std::clamp(p.endsWith('%') ? p.chopped(1).toDouble() * 2.55 : p.toDouble(), 0.0, 255.0);
    };
    QColor c(qRound(channel(parts[0])), qRound(channel(parts[1])), qRound(channel(parts[2])));
    if (parts.size() > 3) c.setAlphaF(float(std::clamp(parts[3].endsWith('%') ? parts[3].chopped(1).toDouble() / 100 : parts[3].toDouble(), 0.0, 1.0)));
    return c;
}

QColor plainColor(const QString &in, const QColor &current)
{
    const QString v = in.trimmed();
    if (v.compare(QLatin1String("currentColor"), Qt::CaseInsensitive) == 0) return current;
    if (v.startsWith(QLatin1String("rgb"), Qt::CaseInsensitive)) return rgbFunction(v.toLower());
    // CSS puts alpha last (#RRGGBBAA); Qt reads it first.
    if (v.startsWith('#') && (v.size() == 9 || v.size() == 5)) {
        const QString h = v.size() == 9 ? v.mid(1) : QString(v[1]) + v[1] + v[2] + v[2] + v[3] + v[3] + v[4] + v[4];
        QColor c = QColor::fromString(QStringView(QString('#' + h.left(6))));
        c.setAlpha(h.mid(6, 2).toInt(nullptr, 16));
        return c;
    }
    return QColor::fromString(QStringView(v));
}

} // namespace

void arcTo(QPainterPath &path, QPointF p0, double rx, double ry, double phiDeg, bool large, bool sweep, QPointF p1)
{
    if (rx == 0 || ry == 0) { path.lineTo(p1); return; }
    if (p0 == p1) return;
    rx = std::abs(rx); ry = std::abs(ry);
    const double phi = qDegreesToRadians(phiDeg), c = std::cos(phi), s = std::sin(phi);
    const double dx = (p0.x() - p1.x()) / 2, dy = (p0.y() - p1.y()) / 2;
    const double x1 = c * dx + s * dy, y1 = -s * dx + c * dy;
    double lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lam > 1) { rx *= std::sqrt(lam); ry *= std::sqrt(lam); }
    double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double k = den == 0 ? 0 : std::sqrt(std::max(0.0, num / den));
    if (large == sweep) k = -k;
    const double cx1 = k * rx * y1 / ry, cy1 = -k * ry * x1 / rx;
    const double cx = c * cx1 - s * cy1 + (p0.x() + p1.x()) / 2, cy = s * cx1 + c * cy1 + (p0.y() + p1.y()) / 2;
    auto ang = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
    const double t1 = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
    double dt = ang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    if (!sweep && dt > 0) dt -= 2 * M_PI;
    if (sweep && dt < 0) dt += 2 * M_PI;
    const int segs = std::max(1, int(std::ceil(std::abs(dt) / (M_PI / 2))));
    const double d = dt / segs;
    const double alpha = 4.0 / 3.0 * std::tan(d / 4);
    double t = t1;
    for (int i = 0; i < segs; ++i) {
        const double c1 = std::cos(t), s1 = std::sin(t), c2 = std::cos(t + d), s2 = std::sin(t + d);
        auto pt = [&](double x, double y) { return QPointF(cx + c * rx * x - s * ry * y, cy + s * rx * x + c * ry * y); };
        path.cubicTo(pt(c1 - alpha * s1, s1 + alpha * c1), pt(c2 + alpha * s2, s2 - alpha * c2), i == segs - 1 ? p1 : pt(c2, s2));
        t += d;
    }
}

QPainterPath pathData(const QString &d)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    Scanner sc(d);
    QPointF cur, start, ctrl;
    QChar cmd;
    bool lastCubic = false, lastQuad = false, drawn = false;
    // A subpath of zero length ("M12 16h0") draws a dot with round or square
    // ends, but QPainterPath drops a line to where it already is; such a line
    // gets a length too small to see.
    auto lineTo = [&](const QPointF &to) {
        if (to != cur) { path.lineTo(to); drawn = true; }
        else if (!drawn) { path.lineTo(to + QPointF(0.01, 0)); drawn = true; }
        cur = to;
    };
    while (!sc.atEnd()) {
        const QChar c = sc.peek();
        if (c.isLetter()) cmd = sc.take();
        else if (cmd.isNull()) break;
        const bool rel = cmd.isLower();
        const QPointF base = rel ? cur : QPointF();
        double v[6];
        auto read = [&](int k) {
            for (int i = 0; i < k; ++i)
                if (!sc.number(&v[i])) return false;
            return true;
        };
        bool cubic = false, quad = false;
        switch (cmd.toUpper().unicode()) {
        case 'M':
            if (!read(2)) return path;
            cur = base + QPointF(v[0], v[1]);
            path.moveTo(cur);
            start = cur;
            drawn = false;
            // Further pairs after a move are lines.
            cmd = rel ? QChar('l') : QChar('L');
            break;
        case 'L':
            if (!read(2)) return path;
            lineTo(base + QPointF(v[0], v[1]));
            break;
        case 'H':
            if (!read(1)) return path;
            lineTo(QPointF(rel ? cur.x() + v[0] : v[0], cur.y()));
            break;
        case 'V':
            if (!read(1)) return path;
            lineTo(QPointF(cur.x(), rel ? cur.y() + v[0] : v[0]));
            break;
        case 'C': {
            if (!read(6)) return path;
            const QPointF c1 = base + QPointF(v[0], v[1]);
            ctrl = base + QPointF(v[2], v[3]);
            cur = base + QPointF(v[4], v[5]);
            path.cubicTo(c1, ctrl, cur);
            cubic = true;
            drawn = true;
            break;
        }
        case 'S': {
            if (!read(4)) return path;
            const QPointF c1 = lastCubic ? 2 * cur - ctrl : cur;
            ctrl = base + QPointF(v[0], v[1]);
            cur = base + QPointF(v[2], v[3]);
            path.cubicTo(c1, ctrl, cur);
            cubic = true;
            drawn = true;
            break;
        }
        case 'Q':
            if (!read(4)) return path;
            ctrl = base + QPointF(v[0], v[1]);
            cur = base + QPointF(v[2], v[3]);
            path.quadTo(ctrl, cur);
            quad = true;
            drawn = true;
            break;
        case 'T':
            if (!read(2)) return path;
            ctrl = lastQuad ? 2 * cur - ctrl : cur;
            cur = base + QPointF(v[0], v[1]);
            path.quadTo(ctrl, cur);
            quad = true;
            drawn = true;
            break;
        case 'A': {
            bool large = false, sweep = false;
            if (!read(3) || !sc.flag(&large) || !sc.flag(&sweep) || !sc.number(&v[3]) || !sc.number(&v[4])) return path;
            const QPointF to = base + QPointF(v[3], v[4]);
            if (to == cur) break;
            arcTo(path, cur, v[0], v[1], v[2], large, sweep, to);
            cur = to;
            drawn = true;
            break;
        }
        case 'Z':
            if (!drawn) lineTo(start);
            path.closeSubpath();
            cur = start;
            // Numbers can't follow a close without a new command.
            cmd = QChar();
            break;
        default:
            return path;
        }
        lastCubic = cubic;
        lastQuad = quad;
    }
    return path;
}

Drawing read(const QByteArray &svg, const QColor &currentColor)
{
    Drawing out;
    std::vector<Node> nodes;
    QVector<int> stack;
    QXmlStreamReader r(svg);
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            const QString ns = r.namespaceUri().toString();
            if (!ns.isEmpty() && ns != svgNs) {
                r.skipCurrentElement();
                continue;
            }
            Node n;
            n.name = r.name().toString();
            for (const QXmlStreamAttribute &a : r.attributes()) {
                const QString ans = a.namespaceUri().toString();
                if (ans.isEmpty() || ans == xlinkNs) n.attrs.insert(a.name().toString(), a.value().toString());
            }
            nodes.push_back(n);
            const int idx = int(nodes.size()) - 1;
            if (!stack.isEmpty()) nodes[stack.last()].children << idx;
            stack << idx;
        } else if (r.isEndElement()) {
            if (!stack.isEmpty()) stack.removeLast();
        } else if (r.isCharacters() && !stack.isEmpty() && nodes[stack.last()].name == QLatin1String("style")) {
            nodes[stack.last()].text += r.text();
        }
    }
    if (nodes.empty() || nodes[0].name != QLatin1String("svg")) return out;

    QHash<QString, int> byId;
    QVector<Rule> rules;
    for (int i = 0; i < int(nodes.size()); ++i) {
        if (const QString id = nodes[i].attrs.value(QStringLiteral("id")); !id.isEmpty()) byId.insert(id, i);
        if (nodes[i].name == QLatin1String("style")) rules += styleSheet(nodes[i].text);
    }
    std::stable_sort(rules.begin(), rules.end(), [](const Rule &a, const Rule &b) { return a.specificity < b.specificity; });

    // The declarations that apply to a node, weakest first.
    auto declsOf = [&](const Node &n) {
        Decls d;
        for (const QString &p : styleProperties())
            if (n.attrs.contains(p)) d << qMakePair(p, n.attrs.value(p));
        const QStringList classes = n.attrs.value(QStringLiteral("class")).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (const Rule &rule : std::as_const(rules)) {
            if (!rule.element.isEmpty() && rule.element != n.name) continue;
            if (!rule.id.isEmpty() && rule.id != n.attrs.value(QStringLiteral("id"))) continue;
            bool all = true;
            for (const QString &c : rule.classes) all = all && classes.contains(c);
            if (all) d += rule.decls;
        }
        d += declarations(n.attrs.value(QStringLiteral("style")));
        return d;
    };

    // A gradient paints with its first color.
    QHash<QString, QColor> gradients;
    std::function<QColor(int, int)> gradientColor = [&](int idx, int depth) -> QColor {
        const Node &g = nodes[idx];
        for (int ch : g.children) {
            if (nodes[ch].name != QLatin1String("stop")) continue;
            QString color = QStringLiteral("black");
            double opacity = 1;
            const Decls d = [&] {
                Decls s;
                for (const QString &p : {QStringLiteral("stop-color"), QStringLiteral("stop-opacity")})
                    if (nodes[ch].attrs.contains(p)) s << qMakePair(p, nodes[ch].attrs.value(p));
                return s + declarations(nodes[ch].attrs.value(QStringLiteral("style")));
            }();
            for (const auto &[k, v] : d) {
                if (k == QLatin1String("stop-color")) color = v;
                else if (k == QLatin1String("stop-opacity")) opacity = v.endsWith('%') ? v.chopped(1).toDouble() / 100 : v.toDouble();
            }
            QColor c = plainColor(color, currentColor);
            if (c.isValid()) c.setAlphaF(float(c.alphaF() * std::clamp(opacity, 0.0, 1.0)));
            return c;
        }
        QString href = g.attrs.value(QStringLiteral("href"));
        if (href.startsWith('#') && depth < 8)
            if (const int t = byId.value(href.mid(1), -1); t >= 0) return gradientColor(t, depth + 1);
        return {};
    };
    for (auto it = byId.cbegin(); it != byId.cend(); ++it)
        if (nodes[it.value()].name.endsWith(QLatin1String("Gradient"))) gradients.insert(it.key(), gradientColor(it.value(), 0));

    const Node &root = nodes[0];
    const QVector<double> vb = numbers(root.attrs.value(QStringLiteral("viewBox")));
    const QString wAttr = root.attrs.value(QStringLiteral("width")), hAttr = root.attrs.value(QStringLiteral("height"));
    const bool hasBox = vb.size() == 4 && vb[2] > 0 && vb[3] > 0;
    double w = wAttr.endsWith('%') ? 0 : length(wAttr, 0), h = hAttr.endsWith('%') ? 0 : length(hAttr, 0);
    if (hasBox) {
        out.viewBox = QRectF(vb[0], vb[1], vb[2], vb[3]);
        if (w <= 0 && h <= 0) { w = vb[2]; h = vb[3]; }
        else if (w <= 0) w = h * vb[2] / vb[3];
        else if (h <= 0) h = w * vb[3] / vb[2];
    } else {
        if (w <= 0) w = 300;
        if (h <= 0) h = 150;
        out.viewBox = QRectF(0, 0, w, h);
    }
    out.size = QSizeF(w * 0.75, h * 0.75);
    const double vw = out.viewBox.width(), vh = out.viewBox.height(), diag = std::sqrt((vw * vw + vh * vh) / 2);

    auto colorOf = [&](const QString &paint, const QColor &current) -> QColor {
        const QString v = paint.trimmed();
        if (v.isEmpty() || v == QLatin1String("none") || v == QLatin1String("transparent")) return {};
        if (v.startsWith(QLatin1String("url("))) {
            const qsizetype close = v.indexOf(')');
            const QString id = v.mid(4, close - 4).trimmed().remove('"').remove('\'').mid(1);
            if (const QColor g = gradients.value(id); g.isValid()) return g;
            const QString fallback = v.mid(close + 1).trimmed();
            return fallback.isEmpty() ? QColor() : plainColor(fallback, current);
        }
        return plainColor(v, current);
    };

    // Each element's declarations and transform, worked out once: <use>
    // can draw the same element many times.
    std::vector<Decls> nodeDecls(nodes.size());
    std::vector<QTransform> nodeTransforms(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodeDecls[i] = declsOf(nodes[i]);
        if (const QString t = nodes[i].attrs.value(QStringLiteral("transform")); !t.isEmpty()) nodeTransforms[i] = transformOf(t);
    }

    auto apply = [&](Style &st, int idx, bool *display) {
        for (const auto &[k, v] : nodeDecls[idx]) {
            if (v == QLatin1String("inherit")) continue;
            if (k == QLatin1String("fill")) st.fill = v;
            else if (k == QLatin1String("stroke")) st.stroke = v;
            else if (k == QLatin1String("stroke-width")) st.strokeWidth = std::max(0.0, length(v, diag, st.strokeWidth));
            else if (k == QLatin1String("fill-opacity") || k == QLatin1String("stroke-opacity") || k == QLatin1String("opacity")) {
                const double o = std::clamp(v.endsWith('%') ? v.chopped(1).toDouble() / 100 : v.toDouble(), 0.0, 1.0);
                if (k == QLatin1String("fill-opacity")) st.fillOpacity = o;
                else if (k == QLatin1String("stroke-opacity")) st.strokeOpacity = o;
                else st.opacity *= o;
            } else if (k == QLatin1String("stroke-linecap"))
                st.cap = v == QLatin1String("round") ? Qt::RoundCap : v == QLatin1String("square") ? Qt::SquareCap : Qt::FlatCap;
            else if (k == QLatin1String("stroke-linejoin"))
                st.join = v == QLatin1String("round") ? Qt::RoundJoin : v == QLatin1String("bevel") ? Qt::BevelJoin : Qt::MiterJoin;
            else if (k == QLatin1String("fill-rule")) st.evenOdd = v == QLatin1String("evenodd");
            else if (k == QLatin1String("visibility")) st.hidden = v != QLatin1String("visible");
            else if (k == QLatin1String("display")) *display = v != QLatin1String("none");
            else if (k == QLatin1String("color")) { if (const QColor c = plainColor(v, st.color); c.isValid()) st.color = c; }
            else if (k == QLatin1String("stroke-dasharray")) {
                st.dashes.clear();
                for (const QString &part : v.split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts))
                    st.dashes << std::max(0.0, length(part, diag));
                // An odd list repeats to make dash-gap pairs; all zeros is solid.
                if (st.dashes.size() % 2) st.dashes += st.dashes;
                if (std::all_of(st.dashes.cbegin(), st.dashes.cend(), [](double x) { return x <= 0; })) st.dashes.clear();
            } else if (k == QLatin1String("stroke-dashoffset")) st.dashOffset = length(v, diag);
        }
    };

    auto add = [&](const QPainterPath &local, const QTransform &t, const Style &st, bool fillable) {
        if (st.hidden || local.isEmpty()) return;
        Element e;
        e.path = t.map(local);
        e.path.setFillRule(st.evenOdd ? Qt::OddEvenFill : Qt::WindingFill);
        if (fillable) e.fill = colorOf(st.fill, st.color);
        e.stroke = st.strokeWidth > 0 ? colorOf(st.stroke, st.color) : QColor();
        if (e.fill.isValid()) e.fill.setAlphaF(float(e.fill.alphaF() * st.fillOpacity * st.opacity));
        if (e.stroke.isValid()) e.stroke.setAlphaF(float(e.stroke.alphaF() * st.strokeOpacity * st.opacity));
        e.strokeWidth = st.strokeWidth * std::sqrt(std::abs(t.determinant()));
        e.cap = st.cap;
        e.join = st.join;
        const double scale = std::sqrt(std::abs(t.determinant()));
        for (double x : st.dashes) e.dashes << x * scale;
        e.dashOffset = st.dashOffset * scale;
        if (e.fill.isValid() || e.stroke.isValid()) out.elements << e;
    };

    auto attr = [](const Node &n, const char *name, double ref) { return length(n.attrs.value(QLatin1String(name)), ref); };

    // `use` is the <use> element drawing a symbol, whose size it sets.
    // <use> can repeat a group that repeats another, ten times at each of
    // nine levels making a billion parts; the work stops at a fixed count.
    int budget = 200000;
    std::function<void(int, QTransform, Style, int, const Node *)> walk = [&](int idx, QTransform ctm, Style st, int depth, const Node *use) {
        if (depth > 32 || --budget < 0 || out.elements.size() >= 100000) {
            out.skipped = true;
            return;
        }
        const Node &n = nodes[idx];
        bool display = true;
        apply(st, idx, &display);
        if (!display) return;
        ctm = nodeTransforms[idx] * ctm;
        const QString &name = n.name;
        auto children = [&](const QTransform &t) {
            for (int ch : n.children) walk(ch, t, st, depth + 1, nullptr);
        };
        if (name == QLatin1String("g") || name == QLatin1String("a") || (name == QLatin1String("svg") && idx == 0)) {
            children(ctm);
        } else if (name == QLatin1String("switch")) {
            if (!n.children.isEmpty()) walk(n.children.first(), ctm, st, depth + 1, nullptr);
        } else if (name == QLatin1String("svg") || (name == QLatin1String("symbol") && use)) {
            // A nested drawing (or a symbol drawn by <use>) maps its own box into place.
            const QVector<double> box = numbers(n.attrs.value(QStringLiteral("viewBox")));
            QTransform t;
            if (!use) t.translate(attr(n, "x", vw), attr(n, "y", vh));
            const Node &sized = use && use->attrs.contains(QStringLiteral("width")) ? *use : n;
            const double nw = attr(sized, "width", vw), nh = attr(sized, "height", vh);
            if (box.size() == 4 && box[2] > 0 && box[3] > 0 && nw > 0 && nh > 0) {
                const double s = std::min(nw / box[2], nh / box[3]);
                t.translate((nw - box[2] * s) / 2, (nh - box[3] * s) / 2);
                t.scale(s, s);
                t.translate(-box[0], -box[1]);
            }
            children(t * ctm);
        } else if (name == QLatin1String("use")) {
            const QString href = n.attrs.value(QStringLiteral("href"));
            const int target = href.startsWith('#') ? byId.value(href.mid(1), -1) : -1;
            if (target < 0 || target == idx) return;
            QTransform t;
            t.translate(attr(n, "x", vw), attr(n, "y", vh));
            walk(target, t * ctm, st, depth + 1, &n);
        } else if (name == QLatin1String("path")) {
            add(pathData(n.attrs.value(QStringLiteral("d"))), ctm, st, true);
        } else if (name == QLatin1String("rect")) {
            const double x = attr(n, "x", vw), y = attr(n, "y", vh), rw = attr(n, "width", vw), rh = attr(n, "height", vh);
            if (rw <= 0 || rh <= 0) return;
            const bool hasRx = n.attrs.contains(QStringLiteral("rx")), hasRy = n.attrs.contains(QStringLiteral("ry"));
            double rx = attr(n, "rx", vw), ry = attr(n, "ry", vh);
            if (hasRx && !hasRy) ry = rx;
            if (hasRy && !hasRx) rx = ry;
            rx = std::clamp(rx, 0.0, rw / 2);
            ry = std::clamp(ry, 0.0, rh / 2);
            QPainterPath p;
            if (rx > 0 && ry > 0) p.addRoundedRect(QRectF(x, y, rw, rh), rx, ry);
            else p.addRect(QRectF(x, y, rw, rh));
            add(p, ctm, st, true);
        } else if (name == QLatin1String("circle") || name == QLatin1String("ellipse")) {
            const bool circle = name == QLatin1String("circle");
            const double rx = circle ? attr(n, "r", diag) : attr(n, "rx", vw), ry = circle ? rx : attr(n, "ry", vh);
            if (rx <= 0 || ry <= 0) return;
            QPainterPath p;
            p.addEllipse(QPointF(attr(n, "cx", vw), attr(n, "cy", vh)), rx, ry);
            add(p, ctm, st, true);
        } else if (name == QLatin1String("line")) {
            const QPointF a(attr(n, "x1", vw), attr(n, "y1", vh)), b(attr(n, "x2", vw), attr(n, "y2", vh));
            QPainterPath p;
            p.moveTo(a);
            p.lineTo(a == b ? b + QPointF(0.01, 0) : b);   // a dot, as in pathData
            add(p, ctm, st, false);
        } else if (name == QLatin1String("polyline") || name == QLatin1String("polygon")) {
            const QVector<double> v = numbers(n.attrs.value(QStringLiteral("points")));
            if (v.size() < 4) return;
            QPainterPath p;
            p.moveTo(v[0], v[1]);
            for (int i = 2; i + 1 < v.size(); i += 2) p.lineTo(v[i], v[i + 1]);
            if (name == QLatin1String("polygon")) p.closeSubpath();
            add(p, ctm, st, true);
        } else if (name == QLatin1String("text") || name == QLatin1String("image") || name == QLatin1String("foreignObject")) {
            out.skipped = true;
        }
        // defs, symbol outside <use>, gradients, clip paths, masks, patterns,
        // markers, style, title and metadata draw nothing.
    };

    Style st;
    st.color = currentColor;
    walk(0, QTransform(), st, 0, nullptr);
    return out;
}

ItemPtr shapes(const Drawing &d, const QRectF &frame, bool merge, const QColor &current, const ColorRef &currentRef)
{
    if (!d.isValid() || d.elements.isEmpty() || frame.isEmpty()) return {};
    const double sx = frame.width() / d.viewBox.width(), sy = frame.height() / d.viewBox.height(), k = std::sqrt(sx * sy);
    if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(k) || sx > 1e6 || sy > 1e6) return {};
    QTransform toFrame;
    toFrame.scale(sx, sy);
    toFrame.translate(-d.viewBox.x(), -d.viewBox.y());
    auto ref = [&](const QColor &c) {
        if (current.isValid() && !currentRef.isNone() && c.rgb() == current.rgb()) return currentRef;
        return ColorRef::rgb(QColor(c.rgb()));
    };
    QVector<std::shared_ptr<ShapeItem>> made;
    for (const Element &e : d.elements) {
        Fill fill = Fill::none();
        if (e.fill.isValid()) {
            fill = Fill::solid(ref(e.fill));
            fill.transparency = 1 - e.fill.alphaF();
        }
        Stroke stroke = Stroke::none();
        if (e.stroke.isValid()) {
            stroke = Stroke::line(ref(e.stroke), e.strokeWidth * k);
            stroke.transparency = 1 - e.stroke.alphaF();
            stroke.cap = e.cap;
            stroke.join = e.join;
            // The nearest of the dash styles a shape's line can have.
            if (e.dashes.size() >= 4) stroke.dash = Stroke::DashDot;
            else if (!e.dashes.isEmpty()) {
                const double r = e.dashes[0] / std::max(1e-6, e.strokeWidth);
                stroke.dash = r < 0.2 ? Stroke::RoundDot : r <= 1.5 ? Stroke::SquareDot : r <= 4.5 ? Stroke::DashLine : Stroke::LongDash;
            }
        }
        const QPainterPath path = toFrame.map(e.path);   // relative to the frame's corner
        if (merge && !made.isEmpty() && made.last()->fill == fill && made.last()->stroke == stroke) {
            made.last()->customPath.addPath(path);
            continue;
        }
        auto s = std::make_shared<ShapeItem>();
        s->shape = QStringLiteral("art");
        s->fill = fill;
        s->stroke = stroke;
        s->customPath = path;
        made << s;
    }
    if (made.size() == 1) {
        // One shape keeps the drawing's whole frame, margins and all.
        made[0]->rect = frame;
        return made[0];
    }
    auto group = std::make_shared<GroupItem>();
    for (const auto &s : std::as_const(made)) {
        // Each part gets a frame around its own outline (at least a point wide).
        QRectF b = s->customPath.boundingRect();
        if (b.width() < 1) b.adjust(-(1 - b.width()) / 2, 0, (1 - b.width()) / 2, 0);
        if (b.height() < 1) b.adjust(0, -(1 - b.height()) / 2, 0, (1 - b.height()) / 2);
        s->customPath.translate(-b.topLeft());
        s->rect = b.translated(frame.topLeft());
        group->children.push_back(s);
    }
    group->syncRect();
    return group;
}

} // namespace jp::svg
