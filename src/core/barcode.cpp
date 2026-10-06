#include "core/barcode.h"

#include <QRegularExpression>
#include <cmath>
#include <cstring>

namespace jp::barcode {

namespace {

constexpr double kMm = 72.0 / 25.4;
constexpr double kModule = 0.33 * kMm;   // EAN/UPC bar width at 100%
constexpr double kCapEm = 0.716;         // cap height of the digits' font (Arimo), in ems

// EAN digit patterns, 7 modules each ('1' = bar): odd parity (L), even (G);
// the right half uses L's complement (R).
const char *const kL[10] = {"0001101", "0011001", "0010011", "0111101", "0100011", "0110001", "0101111", "0111011", "0110111", "0001011"};
const char *const kG[10] = {"0100111", "0110011", "0011011", "0100001", "0011101", "0111001", "0000101", "0010001", "0001001", "0010111"};
// The parities of an EAN-13's left six digits, by its first digit.
const char *const kFirst[10] = {"LLLLLL", "LLGLGG", "LLGGLG", "LLGGGL", "LGLLGG", "LGGLLG", "LGGGLL", "LGLGLG", "LGLGGL", "LGGLGL"};
// The parities of a 5-digit add-on, by its checksum; of a 2-digit one, by its value mod 4.
const char *const kAddOn5[10] = {"GGLLL", "GLGLL", "GLLGL", "GLLLG", "LGGLL", "LLGGL", "LLLGG", "LGLGL", "LGLLG", "LLGLG"};
const char *const kAddOn2[4] = {"LL", "LG", "GL", "GG"};

QString rCode(int d)
{
    QString s = QString::fromLatin1(kL[d]);
    for (QChar &c : s) c = c == '1' ? '0' : '1';
    return s;
}

// Code 128: bar and space widths of symbols 0-106 (106 is the stop, 7 elements).
const char *const k128[107] = {
    "212222", "222122", "222221", "121223", "121322", "131222", "122213", "122312", "132212", "221213", "221312", "231212", "112232", "122132",
    "122231", "113222", "123122", "123221", "223211", "221132", "221231", "213212", "223112", "312131", "311222", "321122", "321221", "312212",
    "322112", "322211", "212123", "212321", "232121", "111323", "131123", "131321", "112313", "132113", "132311", "211313", "231113", "231311",
    "112133", "112331", "132131", "113123", "113321", "133121", "313121", "211331", "231131", "213113", "213311", "213131", "311123", "311321",
    "331121", "312113", "312311", "332111", "314111", "221411", "431111", "111224", "111422", "121124", "121421", "141122", "141221", "112214",
    "112412", "122114", "122411", "142112", "142211", "241211", "221114", "413111", "241112", "134111", "111242", "121142", "121241", "114212",
    "124112", "124211", "411212", "421112", "421211", "212141", "214121", "412121", "111143", "111341", "131141", "114113", "114311", "411113",
    "411311", "113141", "114131", "311141", "411131", "211412", "211214", "211232", "2331112"};

// Code 39: nine elements (bar, space, ..., bar), '1' = wide.
const char kChars39[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-. $/+%*";
const char *const k39[44] = {"000110100", "100100001", "001100001", "101100000", "000110001", "100110000", "001110000", "000100101", "100100100",
                             "001100100", "100001001", "001001001", "101001000", "000011001", "100011000", "001011000", "000001101", "100001100",
                             "001001100", "000011100", "100000011", "001000011", "101000010", "000010011", "100010010", "001010010", "000000111",
                             "100000110", "001000110", "000010110", "110000001", "011000001", "111000000", "010010001", "110010000", "011010000",
                             "010000101", "110000100", "011000100", "010101000", "010100010", "010001010", "000101010", "010010100"};

QString digitsOnly(const QString &s)
{
    QString d;
    for (QChar c : s)
        if (c.isDigit()) d += c;
    return d;
}

bool allDigits(const QString &s)
{
    for (QChar c : s)
        if (!c.isDigit() || c.unicode() > '9') return false;
    return !s.isEmpty();
}

// Adds bars for a module string ('1' = bar) starting at module x.
void addModules(Layout &l, const QString &modules, double x0, double top, double height, double unit)
{
    for (int i = 0; i < modules.size();) {
        if (modules[i] != '1') { ++i; continue; }
        int j = i;
        while (j < modules.size() && modules[j] == '1') ++j;
        l.bars << QRectF(x0 + i * unit, top, (j - i) * unit, height);
        i = j;
    }
}

// An EAN/UPC add-on's modules (start 1011, digits with 01 between).
QString addOnModules(const QString &digits)
{
    QString parity;
    if (digits.size() == 5) {
        int sum = 0;
        for (int i = 0; i < 5; ++i) sum += digits[i].digitValue() * (i % 2 == 0 ? 3 : 9);
        parity = QString::fromLatin1(kAddOn5[sum % 10]);
    } else {
        parity = QString::fromLatin1(kAddOn2[digits.toInt() % 4]);
    }
    QString m = QStringLiteral("1011");
    for (int i = 0; i < digits.size(); ++i) {
        if (i) m += QStringLiteral("01");
        const int d = digits[i].digitValue();
        m += QString::fromLatin1(parity[i] == 'G' ? kG[d] : kL[d]);
    }
    return m;
}

Layout eanUpc(const Options &o, QString digits, const QString &caption)
{
    Layout l;
    const bool ean8 = o.type == Type::Ean8, upc = o.type == Type::UpcA;
    const int n = ean8 ? 8 : upc ? 12 : 13;
    if (!allDigits(digits) || (digits.size() != n && digits.size() != n - 1)) {
        l.error = QStringLiteral("%1 takes %2 digits (or %3 without the check digit).")
                      .arg(ean8 ? QStringLiteral("EAN-8") : upc ? QStringLiteral("UPC-A") : QStringLiteral("EAN-13")).arg(n).arg(n - 1);
        return l;
    }
    const int check = eanCheckDigit(digits.left(n - 1));
    if (digits.size() == n && digits[n - 1].digitValue() != check) {
        l.error = QStringLiteral("The check digit should be %1, not %2.").arg(check).arg(digits[n - 1]);
        return l;
    }
    digits = digits.left(n - 1) + QString::number(check);
    const QString addOn = digitsOnly(o.addOn);
    if (!o.addOn.trimmed().isEmpty() && addOn.size() != 2 && addOn.size() != 5) {
        l.error = QStringLiteral("An add-on has 2 or 5 digits.");
        return l;
    }
    // The bars, as modules: UPC-A is an EAN-13 starting with 0.
    const QString ean = upc ? QStringLiteral("0") + digits : digits;
    QString m = QStringLiteral("101");
    QString guards = QStringLiteral("111");   // which modules reach below the others
    auto plain = [](int k) { return QString(k, QLatin1Char('0')); };
    if (ean8) {
        for (int i = 0; i < 4; ++i) m += QString::fromLatin1(kL[ean[i].digitValue()]);
        m += QStringLiteral("01010");
        for (int i = 4; i < 8; ++i) m += rCode(ean[i].digitValue());
        guards += plain(28) + QStringLiteral("11111") + plain(28);
    } else {
        const QString parity = QString::fromLatin1(kFirst[ean[0].digitValue()]);
        for (int i = 1; i <= 6; ++i) m += QString::fromLatin1(parity[i - 1] == 'G' ? kG[ean[i].digitValue()] : kL[ean[i].digitValue()]);
        m += QStringLiteral("01010");
        for (int i = 7; i <= 12; ++i) m += rCode(ean[i].digitValue());
        // UPC-A prints its first and last digits outside, their bars as long as the guards.
        guards += upc ? QStringLiteral("1111111") + plain(35) + QStringLiteral("11111") + plain(35) + QStringLiteral("1111111")
                      : plain(42) + QStringLiteral("11111") + plain(42);
    }
    m += QStringLiteral("101");
    guards += QStringLiteral("111");

    const double x = kModule * o.magnification;
    const double h = 22.85 * kMm * o.magnification * std::clamp(o.barHeight, 0.2, 2.0) * (ean8 ? 18.23 / 22.85 : 1.0);
    const double digitSize = 2.75 * kMm * o.magnification / kCapEm;
    const double capH = digitSize * kCapEm;
    const double leftQuiet = (ean8 ? 7 : upc ? 9 : 11) * x, rightQuiet = (ean8 ? 7 : upc ? 9 : 7) * x;
    const bool hasCaption = o.text && !caption.isEmpty();
    const double captionSize = digitSize * 0.8;
    const double top = (hasCaption ? captionSize * kCapEm + 3 * x : 0) + 2 * x;
    const double x0 = leftQuiet;
    // Normal bars, then the guards (and UPC's outer digits) reaching 5 modules lower.
    QString normal = m, longer = m;
    for (int i = 0; i < m.size(); ++i) {
        if (guards[i] == '1') normal[i] = '0';
        else longer[i] = '0';
    }
    addModules(l, normal, x0, top, h, x);
    addModules(l, longer, x0, top, h + 5 * x, x);
    double width = x0 + m.size() * x + rightQuiet;
    const double digitBase = top + h + 0.5 * x + capH;
    if (o.text) {
        auto put = [&](const QString &t, double cx, double size) { l.labels << Label{t, QPointF(cx, digitBase), size}; };
        if (ean8) {
            put(ean.mid(0, 4), x0 + (3 + 14) * x, digitSize);
            put(ean.mid(4, 4), x0 + (3 + 28 + 5 + 14) * x, digitSize);
        } else if (upc) {
            put(ean.mid(1, 1), x0 - 4.5 * x, digitSize * 0.75);
            put(ean.mid(2, 5), x0 + (3 + 7 + 17.5) * x, digitSize);
            put(ean.mid(7, 5), x0 + (3 + 42 + 5 + 17.5) * x, digitSize);
            put(ean.mid(12, 1), x0 + (95 + 4.5) * x, digitSize * 0.75);
        } else {
            put(ean.left(1), x0 - 6 * x, digitSize);
            put(ean.mid(1, 6), x0 + (3 + 21) * x, digitSize);
            put(ean.mid(7, 6), x0 + (3 + 42 + 5 + 21) * x, digitSize);
        }
        if (hasCaption) l.labels << Label{caption, QPointF(x0 + m.size() * x / 2, top - 3 * x), captionSize};
    }
    if (!addOn.isEmpty()) {
        // The add-on: 9 modules on, its digits above its bars, which end with the guards.
        const QString am = addOnModules(addOn);
        const double ax = x0 + m.size() * x + 9 * x;
        const double aTop = top + (o.text ? capH + 1.5 * x : 0);
        addModules(l, am, ax, aTop, top + h + 5 * x - aTop, x);
        if (o.text) l.labels << Label{addOn, QPointF(ax + am.size() * x / 2, top + capH), digitSize};
        width = ax + am.size() * x + 5 * x;
    }
    l.encoded = ean + (addOn.isEmpty() ? QString() : QStringLiteral(" ") + addOn);
    l.size = QSizeF(width, digitBase + (o.text ? capH * 0.35 : 5.5 * x - capH) + 2 * x);
    if (!o.text) l.size.setHeight(top + h + 5 * x + 2 * x);
    return l;
}

Layout code128(const Options &o)
{
    Layout l;
    const QString s = o.data;
    if (s.isEmpty()) { l.error = QStringLiteral("Type the text the barcode holds."); return l; }
    for (QChar c : s)
        if (c.unicode() < 32 || c.unicode() > 126) {
            l.error = QStringLiteral("Code 128 holds plain letters, digits and symbols (\"%1\" isn't one).").arg(c);
            return l;
        }
    // Code set B for text, C (two digits to a symbol) for runs of four or
    // more digits, or a whole text of an even number of digits.
    QVector<int> codes;
    int set = 0;   // 1 = B, 2 = C
    auto digitRun = [&](int i) { int k = i; while (k < s.size() && s[k].isDigit() && s[k].unicode() <= '9') ++k; return k - i; };
    for (int i = 0; i < s.size();) {
        int run = digitRun(i);
        const bool wholeEven = i == 0 && run == s.size() && run % 2 == 0;
        if (run >= 4 || wholeEven || (set == 2 && run >= 2)) {
            if (run % 2 == 1) {
                // An odd run: its first digit in set B, the rest in pairs.
                if (set != 1) { codes << (codes.isEmpty() ? 104 : 100); set = 1; }
                codes << s[i].unicode() - 32;
                ++i;
                --run;
            }
            if (set != 2) { codes << (codes.isEmpty() ? 105 : 99); set = 2; }
            for (; run >= 2; run -= 2, i += 2) codes << s.mid(i, 2).toInt();
            continue;
        }
        if (set != 1) { codes << (codes.isEmpty() ? 104 : 100); set = 1; }
        codes << s[i].unicode() - 32;
        ++i;
    }
    int sum = codes[0];
    for (int i = 1; i < codes.size(); ++i) sum += i * codes[i];
    codes << sum % 103 << 106;
    QString m;
    for (int c : codes) {
        const char *w = k128[c];
        bool bar = true;
        for (const char *p = w; *p; ++p, bar = !bar) m += QString(*p - '0', bar ? QLatin1Char('1') : QLatin1Char('0'));
    }
    const double x = 0.33 * kMm * o.magnification;
    const double quiet = 10 * x;
    const double h = std::max(18.0, 0.15 * m.size() * x) * std::clamp(o.barHeight, 0.2, 2.0);
    const double size = 2.75 * kMm * o.magnification / kCapEm;
    addModules(l, m, quiet, 2 * x, h, x);
    if (o.text) l.labels << Label{s, QPointF(quiet + m.size() * x / 2, 2 * x + h + 0.5 * x + size * kCapEm), size};
    l.size = QSizeF(m.size() * x + 2 * quiet, 2 * x + h + (o.text ? 0.5 * x + size * kCapEm + size * 0.25 : 0) + 2 * x);
    l.encoded = s;
    return l;
}

Layout code39(const Options &o)
{
    Layout l;
    QString s = o.data.toUpper();
    if (s.isEmpty()) { l.error = QStringLiteral("Type the text the barcode holds."); return l; }
    QVector<int> idx;
    for (QChar c : s) {
        const char *p = c.unicode() < 128 ? strchr(kChars39, char(c.unicode())) : nullptr;
        if (!p || c == '*' || !c.unicode()) {
            l.error = QStringLiteral("Code 39 holds capital letters, digits, spaces and - . $ / + % (\"%1\" isn't one).").arg(c);
            return l;
        }
        idx << int(p - kChars39);
    }
    if (o.code39Check) {
        int sum = 0;
        for (int i : idx) sum += i;
        idx << sum % 43;
        s += QLatin1Char(kChars39[sum % 43]);
    }
    idx.prepend(43);
    idx << 43;
    const double x = 0.33 * kMm * o.magnification, wide = 3 * x;
    const double quiet = 10 * x;
    const double h = std::max(18.0, 0.15 * idx.size() * 16 * x) * std::clamp(o.barHeight, 0.2, 2.0);
    double cx = quiet;
    for (int k = 0; k < idx.size(); ++k) {
        const char *p = k39[idx[k]];
        for (int e = 0; e < 9; ++e) {
            const double w = p[e] == '1' ? wide : x;
            if (e % 2 == 0) l.bars << QRectF(cx, 2 * x, w, h);
            cx += w;
        }
        if (k + 1 < idx.size()) cx += x;   // the gap between characters
    }
    const double size = 2.75 * kMm * o.magnification / kCapEm;
    if (o.text) l.labels << Label{QStringLiteral("*%1*").arg(s), QPointF((cx + quiet) / 2, 2 * x + h + 0.5 * x + size * kCapEm), size};
    l.size = QSizeF(cx + quiet, 2 * x + h + (o.text ? 0.5 * x + size * kCapEm + size * 0.25 : 0) + 2 * x);
    l.encoded = s;
    return l;
}

} // namespace

int eanCheckDigit(const QString &digits)
{
    int sum = 0;
    for (int i = 0; i < digits.size(); ++i) {
        const int fromRight = digits.size() - i;   // 1 for the digit next to the check
        sum += digits[i].digitValue() * (fromRight % 2 == 1 ? 3 : 1);
    }
    return (10 - sum % 10) % 10;
}

QString isbn13(const QString &input, QString *error)
{
    QString s;
    for (QChar c : input)
        if (c.isDigit() || c == 'X' || c == 'x') s += c.toUpper();
        else if (c != '-' && c != ' ' && c.unicode() != 0x2010 && c.unicode() != 0x2011) {
            if (error) *error = QStringLiteral("An ISBN has only digits (and hyphens).");
            return {};
        }
    if (s.size() == 10) {
        int sum = 0;
        for (int i = 0; i < 9; ++i) {
            if (!s[i].isDigit()) {
                if (error) *error = QStringLiteral("Only an ISBN-10's last character can be X.");
                return {};
            }
            sum += s[i].digitValue() * (10 - i);
        }
        const int check = (11 - sum % 11) % 11;
        const QChar want = check == 10 ? QChar('X') : QChar('0' + check);
        if (s[9] != want) {
            if (error) *error = QStringLiteral("This ISBN-10's check digit should be %1, not %2.").arg(want).arg(s[9]);
            return {};
        }
        const QString body = QStringLiteral("978") + s.left(9);
        return body + QString::number(eanCheckDigit(body));
    }
    if (s.size() == 13 && allDigits(s)) {
        if (!s.startsWith(QLatin1String("978")) && !s.startsWith(QLatin1String("979"))) {
            if (error) *error = QStringLiteral("An ISBN-13 starts with 978 or 979.");
            return {};
        }
        const int check = eanCheckDigit(s.left(12));
        if (s[12].digitValue() != check) {
            if (error) *error = QStringLiteral("This ISBN's check digit should be %1, not %2.").arg(check).arg(s[12]);
            return {};
        }
        return s;
    }
    if (error) *error = QStringLiteral("An ISBN has 10 or 13 digits.");
    return {};
}

QString isbnCaption(const QString &input)
{
    const QString t = input.trimmed();
    QString err;
    const QString digits = isbn13(t, &err);
    if (digits.isEmpty()) return {};
    // Hyphens as typed, if they are hyphenated as a 13-digit ISBN.
    if (t.contains(QLatin1Char('-')) && digitsOnly(t).size() == 13) return QStringLiteral("ISBN ") + t;
    return QStringLiteral("ISBN ") + digits;
}

QString priceAddOn(Currency c, double price, QString *error)
{
    if (c == Currency::None) return QStringLiteral("90000");
    const int cents = int(std::lround(price * 100));
    if (cents <= 0) {
        if (error) *error = QStringLiteral("Type the price.");
        return {};
    }
    // Prices past 99.99 are printed as 99.99 (the add-on has four digits for them).
    const int shown = std::min(cents, 9999);
    const QChar lead = c == Currency::UsDollar ? '5' : c == Currency::CanadianDollar ? '6' : c == Currency::Pound ? '0'
                     : c == Currency::AustralianDollar ? '3' : '4';
    return QString(lead) + QStringLiteral("%1").arg(shown, 4, 10, QLatin1Char('0'));
}

Layout make(const Options &o)
{
    switch (o.type) {
    case Type::Isbn: {
        QString err;
        const QString d = isbn13(o.data, &err);
        if (d.isEmpty()) {
            Layout l;
            l.error = err;
            return l;
        }
        Options e = o;
        e.type = Type::Ean13;
        return eanUpc(e, d, isbnCaption(o.data));
    }
    case Type::Ean13:
    case Type::UpcA:
    case Type::Ean8:
        return eanUpc(o, digitsOnly(o.data).size() == o.data.trimmed().size() ? o.data.trimmed() : o.data.trimmed().remove(QRegularExpression(QStringLiteral("[\\s-]"))), QString());
    case Type::Code128:
        return code128(o);
    case Type::Code39:
        return code39(o);
    }
    return {};
}

} // namespace jp::barcode
