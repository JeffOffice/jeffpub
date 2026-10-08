#include "io/overprint.h"

#include "io/qtpdf.h"

#include <QHash>
#include <QRegularExpression>
#include <QVector>

#include <cmath>

namespace jp {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0'; }
bool isDelim(char c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }

// The end of the string, array or dictionary that starts at i.
qsizetype skipString(const QByteArray &d, qsizetype i)
{
    int depth = 0;
    for (; i < d.size(); ++i) {
        if (d[i] == '\\') ++i;
        else if (d[i] == '(') ++depth;
        else if (d[i] == ')' && --depth == 0) return i + 1;
    }
    return d.size();
}

qsizetype skipNested(const QByteArray &d, qsizetype i)
{
    int depth = 0;
    while (i < d.size()) {
        const char c = d[i];
        if (c == '(') { i = skipString(d, i); continue; }
        if (c == '[' || (c == '<' && i + 1 < d.size() && d[i + 1] == '<')) { ++depth; i += c == '[' ? 1 : 2; continue; }
        if (c == ']' || (c == '>' && i + 1 < d.size() && d[i + 1] == '>')) {
            i += c == ']' ? 1 : 2;
            if (--depth == 0) return i;
            continue;
        }
        if (c == '<') { const qsizetype e = d.indexOf('>', i); i = e < 0 ? d.size() : e + 1; continue; }
        ++i;
    }
    return d.size();
}

enum class Space { Other, Cmyk, Gray };

struct State {
    double det = 1;              // the scale of the drawing, squared (|det| of the CTM)
    Space fillSpace = Space::Gray, strokeSpace = Space::Gray;
    bool fillBlack = true, strokeBlack = true;   // PDF starts in black
    double fontSize = 0;
    int op = 0;                  // overprint now on: 1 for lines, 2 for fills, 3 both
};

class Pass {
public:
    Pass(const OverprintSettings &o, const QHash<QByteArray, Space> &spaces) : m_o(o), m_spaces(spaces) {}

    // The drawing with overprint switches; `count` places that overprint.
    QByteArray run(const QByteArray &d, int *count)
    {
        QVector<QPair<qsizetype, int>> inserts;   // where, and which switch
        QVector<State> stack;
        State s;
        double tmDet = 1;
        QVector<double> nums;
        QByteArray name;
        qsizetype operandStart = -1, pathStart = -1;
        auto want = [&](qsizetype at, int need) {
            if (need == s.op) return;
            inserts.append({at, need});
            if (need) ++*count;
            s.op = need;
        };
        auto black = [&](Space sp, const QVector<double> &v) {
            const double t = m_o.threshold / 100.0 - 1e-6;
            if (sp == Space::Cmyk && v.size() >= 4)
                return v[0] < 0.005 && v[1] < 0.005 && v[2] < 0.005 && v[3] >= t;
            if (sp == Space::Gray && !v.isEmpty()) return 1.0 - v[0] >= t;
            return false;
        };
        qsizetype i = 0;
        while (i < d.size()) {
            const char c = d[i];
            if (isSpace(c)) { ++i; continue; }
            if (c == '%') {
                while (i < d.size() && d[i] != '\n' && d[i] != '\r') ++i;
                continue;
            }
            const qsizetype start = i;
            if (operandStart < 0) operandStart = start;
            if (c == '/') {
                ++i;
                while (i < d.size() && !isSpace(d[i]) && !isDelim(d[i])) ++i;
                name = d.mid(start + 1, i - start - 1);
                continue;
            }
            if (c == '(') { i = skipString(d, i); continue; }
            if (c == '<' && (i + 1 >= d.size() || d[i + 1] != '<')) {   // a hex string
                const qsizetype e = d.indexOf('>', i);
                i = e < 0 ? d.size() : e + 1;
                continue;
            }
            if (c == '[' || c == '<') { i = skipNested(d, i); continue; }
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
                while (i < d.size() && !isSpace(d[i]) && !isDelim(d[i])) ++i;
                nums.append(d.mid(start, i - start).toDouble());
                continue;
            }
            // An operator.
            while (i < d.size() && !isSpace(d[i]) && !isDelim(d[i])) ++i;
            const QByteArray op = d.mid(start, i - start);
            const qsizetype before = operandStart;
            operandStart = -1;
            if (op == "BI") {   // an inline picture: its data runs to "EI"
                const qsizetype id = d.indexOf("ID", i);
                qsizetype ei = id < 0 ? -1 : d.indexOf("EI", id + 3);
                while (ei > 0 && ei + 2 < d.size() && !isSpace(d[ei + 2])) ei = d.indexOf("EI", ei + 2);
                want(before, 0);
                i = ei < 0 ? d.size() : ei + 2;
            } else if (op == "q") {
                stack.append(s);
            } else if (op == "Q") {
                if (!stack.isEmpty()) {
                    // The switch made inside goes with the state it was made in.
                    s = stack.takeLast();
                }
            } else if (op == "cm" && nums.size() >= 6) {
                const auto &m = nums;
                s.det *= std::abs(m[m.size() - 6] * m[m.size() - 3] - m[m.size() - 5] * m[m.size() - 4]);
            } else if (op == "cs" || op == "CS") {
                const Space sp = m_spaces.value(name, name == "DeviceCMYK" ? Space::Cmyk : name == "DeviceGray" ? Space::Gray : Space::Other);
                // A new color space starts at its own black (CMYK and gray).
                (op == "cs" ? s.fillSpace : s.strokeSpace) = sp;
                (op == "cs" ? s.fillBlack : s.strokeBlack) = sp != Space::Other;
            } else if (op == "sc" || op == "scn") {
                s.fillBlack = black(s.fillSpace, nums);
            } else if (op == "SC" || op == "SCN") {
                s.strokeBlack = black(s.strokeSpace, nums);
            } else if (op == "k" || op == "K" || op == "g" || op == "G") {
                const bool fill = op == "k" || op == "g";
                const Space sp = op.toLower() == "k" ? Space::Cmyk : Space::Gray;
                (fill ? s.fillSpace : s.strokeSpace) = sp;
                (fill ? s.fillBlack : s.strokeBlack) = black(sp, nums);
            } else if (op == "rg" || op == "RG") {
                (op == "rg" ? s.fillSpace : s.strokeSpace) = Space::Other;
                (op == "rg" ? s.fillBlack : s.strokeBlack) = false;
            } else if (op == "BT") {
                tmDet = 1;
            } else if (op == "Tf" && !nums.isEmpty()) {
                s.fontSize = std::abs(nums.last());
            } else if (op == "Tm" && nums.size() >= 6) {
                const auto &m = nums;
                tmDet = std::abs(m[m.size() - 6] * m[m.size() - 3] - m[m.size() - 5] * m[m.size() - 4]);
            } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
                const double size = s.fontSize * std::sqrt(tmDet * s.det);
                want(before, m_o.text && s.fillBlack && size < m_o.textBelow - 1e-6 ? 2 : 0);
            } else if (op == "m" || op == "re") {
                if (pathStart < 0) pathStart = before;
            } else if (op == "S" || op == "s" || op == "f" || op == "F" || op == "f*" || op == "B" || op == "B*" || op == "b" || op == "b*" ||
                       op == "n") {
                // A path's switch goes before the path: nothing but its
                // own construction may come between it and its painting.
                const bool strokes = op == "S" || op == "s" || op.startsWith('B') || op.startsWith('b');
                const bool fills = op != "S" && op != "s" && op != "n";
                const int need = (strokes && m_o.lines && s.strokeBlack ? 1 : 0) | (fills && m_o.fills && s.fillBlack ? 2 : 0);
                if (op != "n") want(pathStart >= 0 ? pathStart : before, need);
                pathStart = -1;
            } else if (op == "Do" || op == "sh") {
                want(before, 0);   // pictures and shadings print as they are
            }
            nums.clear();
            name.clear();
        }
        if (inserts.isEmpty()) return d;
        // The switches, in order of position (a path's switch can be
        // recorded after a later operand's).
        std::stable_sort(inserts.begin(), inserts.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        QByteArray out;
        out.reserve(d.size() + inserts.size() * 12);
        qsizetype at = 0;
        for (const auto &[pos, need] : inserts) {
            out += d.mid(at, pos - at);
            out += "/JPop" + QByteArray::number(need) + " gs\n";
            at = pos;
        }
        out += d.mid(at);
        return out;
    }

private:
    OverprintSettings m_o;
    QHash<QByteArray, Space> m_spaces;
};

// The color spaces a resource dictionary names: /CSp /DeviceCMYK and so on.
QHash<QByteArray, Space> spacesIn(const QByteArray &resources)
{
    QHash<QByteArray, Space> out;
    static const QRegularExpression re(QStringLiteral("/([^\\s/<>\\[\\]()]+)\\s*/Device(CMYK|Gray|RGB)\\b"));
    auto it = re.globalMatch(QString::fromLatin1(resources));
    while (it.hasNext()) {
        const auto m = it.next();
        out.insert(m.captured(1).toLatin1(), m.captured(2) == QLatin1String("CMYK") ? Space::Cmyk : m.captured(2) == QLatin1String("Gray") ? Space::Gray : Space::Other);
    }
    return out;
}

} // namespace

int applyOverprint(QtPdf &pdf, const OverprintSettings &o)
{
    if (!o.any()) return 0;
    int count = 0;
    int gs[4] = {0, 0, 0, 0};
    static const QRegularExpression pageRe(QStringLiteral("/Type\\s*/Page(?!s)"));
    static const QRegularExpression contentsRe(QStringLiteral("/Contents\\s*(\\[[^\\]]*\\]|\\d+ 0 R)"));
    static const QRegularExpression refRe(QStringLiteral("(\\d+) 0 R"));
    const int objects = pdf.objects.size();
    for (int k = 0; k < objects; ++k) {
        const QByteArray pageDict = QtPdf::dictOf(pdf.objects[k].body);
        if (!QString::fromLatin1(pageDict).contains(pageRe)) continue;
        const int resId = QtPdf::ref(pageDict, "/Resources");
        QtPdf::Obj *res = resId ? pdf.object(resId) : nullptr;
        if (!res) continue;   // Qt keeps each page's resources in an object of their own
        Pass pass(o, spacesIn(res->body));
        const auto cm = contentsRe.match(QString::fromLatin1(pageDict));
        if (!cm.hasMatch()) continue;
        int here = 0;
        auto refs = refRe.globalMatch(cm.captured(1));
        while (refs.hasNext()) {
            QtPdf::Obj *c = pdf.object(refs.next().captured(1).toInt());
            if (!c || !QtPdf::isStream(c->body)) continue;
            bool ok = false;
            const QByteArray data = pdf.streamData(*c, &ok);
            if (!ok) continue;
            int n = 0;
            const QByteArray out = pass.run(data, &n);
            if (out == data) continue;
            const QByteArray dict = QtPdf::dictOf(c->body);
            pdf.setStream(*c, dict, dict.contains("/FlateDecode") ? QtPdf::deflate(out) : out);
            here += n;
        }
        if (!here) continue;
        count += here;
        // The four switches, in this page's resources (adding objects
        // moves the others in memory: the resources are found again).
        for (int s = 0; s < 4; ++s)
            if (!gs[s])
                gs[s] = pdf.addObject("<< /Type /ExtGState /OP " + QByteArray(s & 1 ? "true" : "false") + " /op " + QByteArray(s & 2 ? "true" : "false") +
                                      " /OPM 1 >>\n");
        res = pdf.object(resId);
        QByteArray names;
        for (int s = 0; s < 4; ++s) names += " /JPop" + QByteArray::number(s) + " " + QByteArray::number(gs[s]) + " 0 R";
        QByteArray &body = res->body;
        const qsizetype e = body.indexOf("/ExtGState");
        if (e >= 0) {
            const qsizetype open = body.indexOf("<<", e);
            if (open < 0) continue;
            if (!body.mid(open, body.indexOf(">>", open) - open).contains("/JPop0")) body.insert(open + 2, names);
        } else {
            const qsizetype close = body.lastIndexOf(">>");
            if (close < 0) continue;
            body.insert(close, "/ExtGState <<" + names + " >>\n");
        }
    }
    return count;
}

} // namespace jp
