#include <QTabBar>
#include <QApplication>
#include "app/icons.h"

#include "render/shapes.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QStyleHints>
#include <QSvgRenderer>

namespace jp {

static int g_uiDark = -1;   // -1: not decided, follow the system
bool uiDark()
{
    if (g_uiDark >= 0) return g_uiDark == 1;
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}
void setUiDark(bool dark) { g_uiDark = dark ? 1 : 0; }

static bool g_highContrast = false;
bool uiHighContrast() { return g_highContrast; }
void setUiHighContrast(bool on) { g_highContrast = on; }

// In high contrast, the system's colors (the palette holds them) rather
// than JeffPub's own.
QColor uiText()
{
    if (g_highContrast) return QGuiApplication::palette().color(QPalette::WindowText);
    return uiDark() ? QColor(0xDF, 0xE3, 0xEA) : QColor(0x2A, 0x30, 0x3B);
}

QColor uiAccent()
{
    if (g_highContrast) return QGuiApplication::palette().color(QPalette::Highlight);
    return uiDark() ? QColor(0xE0, 0x6A, 0xA5) : QColor(0x9E, 0x1F, 0x63);
}

// Surfaces of the program's own panels; in high contrast, the system's.
static QColor role(QPalette::ColorRole r) { return QGuiApplication::palette().color(r); }
QColor uiWindow() { return g_highContrast ? role(QPalette::Window) : uiDark() ? QColor(0x1E, 0x22, 0x29) : QColor(0xFB, 0xFB, 0xFC); }
QColor uiPanel() { return g_highContrast ? role(QPalette::Window) : uiDark() ? QColor(0x17, 0x1A, 0x1F) : QColor(0xF1, 0xF2, 0xF5); }
QColor uiCard() { return g_highContrast ? role(QPalette::Base) : uiDark() ? QColor(0x22, 0x26, 0x2D) : QColor(Qt::white); }
QColor uiCardPressed() { return g_highContrast ? role(QPalette::Base) : uiDark() ? QColor(0x2C, 0x31, 0x3A) : QColor(0xF3, 0xF4, 0xF6); }
// Borders; in high contrast as strong as the text, so they show.
QColor uiLine() { return g_highContrast ? role(QPalette::WindowText) : uiDark() ? QColor(0x2F, 0x34, 0x3D) : QColor(0xE3, 0xE5, 0xEA); }

QColor focusRingColor()
{
    if (g_highContrast) return QGuiApplication::palette().color(QPalette::Highlight);
    return uiDark() ? QColor(0x7F, 0xB2, 0xFF) : QColor(0x1A, 0x5F, 0xD6);   // a blue that reads on the ribbon and on accents
}

// Style sheets can only load arrow images from files, so write small tinted
// chevrons to the temp folder once per color.
static QString chevronFile(const QString &dir, const QColor &c)
{
    const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("jeffpub-chevron-%1-%2.svg").arg(dir, c.name().mid(1)));
    if (!QFile::exists(path)) {
        const QString d = dir == "up" ? QStringLiteral("M6 15l6-6 6 6") : dir == "check" ? QStringLiteral("M5 12.5l4.5 4.5L19 7.5") : QStringLiteral("M6 9l6 6 6-6");
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24' fill='none' stroke='%1' "
                                   "stroke-width='2.2' stroke-linecap='round' stroke-linejoin='round'><path d='%2'/></svg>")
                        .arg(c.name(), d).toUtf8());
    }
    return QDir::fromNativeSeparators(path);
}

QString modernStyleSheet()
{
    const bool d = uiDark();
    const QString accent = uiAccent().name(), accentHover = uiAccent().lighter(d ? 115 : 112).name();
    const QString field = d ? "#22262d" : "#ffffff", border = d ? "#3a404a" : "#cfd3da", borderHover = d ? "#4a515c" : "#b4bac4";
    const QString button = d ? "#2a2f37" : "#ffffff", buttonHover = d ? "#323843" : "#f3f4f6", text = uiText().name();
    const QString handle = d ? "rgba(255,255,255,0.22)" : "rgba(0,0,0,0.22)", handleHover = d ? "rgba(255,255,255,0.38)" : "rgba(0,0,0,0.38)";
    QString css = QStringLiteral(R"(
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QDateTimeEdit, QComboBox {
    background: %1; border: 1px solid %2; border-radius: 6px; padding: 4px 8px; color: %8;
    selection-background-color: %5; selection-color: white;
}
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover { border-color: %3; }
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border: 1px solid %5; }
QComboBox { padding-right: 22px; min-height: 20px; }
QComboBox QLineEdit, QAbstractSpinBox QLineEdit { border: none; padding: 0; margin: 0; background: transparent; border-radius: 0; }
QComboBox:editable { padding-left: 6px; padding-right: 0px; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::down-arrow { image: url(%12); width: 12px; height: 12px; }
QSpinBox, QDoubleSpinBox, QDateTimeEdit { padding-right: 20px; }
QSpinBox::up-button, QDoubleSpinBox::up-button, QDateTimeEdit::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 18px; border: none; margin: 2px 2px 0 0; }
QSpinBox::down-button, QDoubleSpinBox::down-button, QDateTimeEdit::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 18px; border: none; margin: 0 2px 2px 0; }
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover { background: %6; border-radius: 3px; }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow, QDateTimeEdit::up-arrow { image: url(%13); width: 9px; height: 9px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow, QDateTimeEdit::down-arrow { image: url(%12); width: 9px; height: 9px; }
QComboBox QAbstractItemView { border: 1px solid %2; border-radius: 6px; background: %1; padding: 4px; outline: 0;
    selection-background-color: %5; selection-color: white; }
QPushButton { background: %4; border: 1px solid %2; border-radius: 6px; padding: 5px 14px; color: %8; min-height: 20px; }
QPushButton:hover { background: %6; border-color: %3; }
QPushButton:pressed { background: %2; }
QPushButton:focus { border-color: %5; }
QPushButton:disabled { color: %9; }
QPushButton[primary="true"] { background: %5; border: 1px solid %5; color: white; font-weight: 600; }
QPushButton[primary="true"]:hover { background: %7; border-color: %7; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: %10; border-radius: 3px; min-height: 30px; margin: 0 2px; }
QScrollBar::handle:horizontal { background: %10; border-radius: 3px; min-width: 30px; margin: 2px 0; }
QScrollBar::handle:hover { background: %11; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QCheckBox::indicator, QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid %3; background: %1; }
QCheckBox::indicator { border-radius: 4px; }
QRadioButton::indicator { border-radius: 8px; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: %5; }
QCheckBox::indicator:checked { background: %5; border-color: %5; image: url(%14); }
QRadioButton::indicator:checked { background: %1; border: 4px solid %5; width: 8px; height: 8px; }
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled { background: %4; border-color: %2; }
QToolTip { background: %4; color: %8; border: 1px solid %2; border-radius: 6px; padding: 5px 8px; }
QMenu { background: %1; border: 1px solid %2; border-radius: 8px; padding: 5px; }
QMenu::item { padding: 6px 22px 6px 26px; border-radius: 5px; color: %8; }
QMenu::item:selected { background: %6; }
QMenu::separator { height: 1px; background: %2; margin: 5px 8px; }
QMenu::icon { padding-left: 6px; }
)");
    return css.arg(field, border, borderHover, button, accent, buttonHover, accentHover, text, d ? "#6b7280" : "#9ca3af")
        .arg(handle, handleHover, chevronFile(QStringLiteral("down"), uiText()), chevronFile(QStringLiteral("up"), uiText()), chevronFile(QStringLiteral("check"), QColor(Qt::white)));
}

namespace {

QByteArray svgSource(const QString &name)
{
    static QHash<QString, QByteArray> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return *it;
    QFile f(QStringLiteral(":/icons/%1.svg").arg(name));
    QByteArray data;
    if (f.open(QIODevice::ReadOnly)) data = f.readAll();
    cache.insert(name, data);
    return data;
}

class SvgEngine : public QIconEngine {
public:
    SvgEngine(QString name, QColor bar = QColor()) : m_name(std::move(name)), m_bar(bar) {}
    QIconEngine *clone() const override { return new SvgEngine(*this); }
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        QColor c = uiText();
        if (mode == QIcon::Disabled) c.setAlphaF(0.35f);
        if (mode == QIcon::Selected || mode == QIcon::Active) c = uiText();
        QByteArray svg = svgSource(m_name);
        svg.replace("currentColor", c.name(QColor::HexRgb).toLatin1());
        if (mode == QIcon::Disabled) svg.replace("<svg", "<svg opacity=\"0.4\"");
        // Thinner strokes at large sizes read lighter and cleaner.
        if (rect.width() >= 28) svg.replace("stroke-width=\"2\"", "stroke-width=\"1.5\"");
        QSvgRenderer r(svg);
        QRectF target = rect;
        if (m_bar.isValid()) target = QRectF(rect.x(), rect.y(), rect.width(), rect.height() * 0.78);
        const double s = std::min(target.width(), target.height());
        r.render(p, QRectF(target.center().x() - s / 2, target.top(), s, s));
        if (m_bar.isValid()) {
            const QRectF bar(rect.x() + rect.width() * 0.08, rect.bottom() - rect.height() * 0.2 + 1, rect.width() * 0.84, rect.height() * 0.2);
            if (m_bar.alpha() == 0) {
                p->setPen(QPen(QColor(150, 150, 150), 1, Qt::DotLine));
                p->setBrush(Qt::NoBrush);
                p->drawRect(bar);
            } else {
                p->fillRect(bar, m_bar);
            }
        }
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1.0); }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pm(size * scale);
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

private:
    QString m_name;
    QColor m_bar;
};

class DrawEngine : public QIconEngine {
public:
    explicit DrawEngine(std::function<void(QPainter *, const QRectF &)> fn) : m_fn(std::move(fn)) {}
    QIconEngine *clone() const override { return new DrawEngine(*this); }
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        p->save();
        if (mode == QIcon::Disabled) p->setOpacity(0.4);
        p->setRenderHint(QPainter::Antialiasing);
        m_fn(p, rect);
        p->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1.0); }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pm(size * scale);
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

private:
    std::function<void(QPainter *, const QRectF &)> m_fn;
};

} // namespace

QIcon icon(const QString &name) { return QIcon(new SvgEngine(name)); }
QIcon colorBarIcon(const QString &name, const QColor &bar) { return QIcon(new SvgEngine(name, bar)); }
QIcon drawnIcon(const std::function<void(QPainter *, const QRectF &)> &draw) { return QIcon(new DrawEngine(draw)); }

QIcon shapeIcon(const QString &shapeId)
{
    return drawnIcon([shapeId](QPainter *p, const QRectF &r) {
        const QRectF box = r.adjusted(r.width() * 0.1, r.height() * 0.1, -r.width() * 0.1, -r.height() * 0.1);
        QPainterPath path = shapePath(shapeId, box.size());
        path.translate(box.topLeft());
        const ShapeDef *d = shapeDef(shapeId);
        p->setPen(QPen(uiText(), std::max(1.0, r.width() / 22)));
        p->setBrush(d && d->open ? Qt::NoBrush : QBrush(QColor(uiAccent().red(), uiAccent().green(), uiAccent().blue(), 40)));
        p->drawPath(path);
    });
}

QIcon lineToolIcon(const QString &kind)
{
    return drawnIcon([kind](QPainter *p, const QRectF &r) {
        const QRectF b = r.adjusted(r.width() * 0.16, r.height() * 0.2, -r.width() * 0.16, -r.height() * 0.2);
        const double w = std::max(1.0, r.width() / 22);
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(uiText(), w, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        p->setBrush(Qt::NoBrush);
        QPainterPath path;
        const QPointF a = b.bottomLeft(), z = b.topRight();
        const double mx = b.center().x();
        if (kind.startsWith(QLatin1String("elbow"))) {
            path.moveTo(a);
            path.lineTo(mx, a.y());
            path.lineTo(mx, z.y());
            path.lineTo(z);
        } else if (kind.startsWith(QLatin1String("curved"))) {
            path.moveTo(a);
            path.cubicTo(QPointF(mx, a.y()), QPointF(mx, z.y()), z);
        } else if (kind == QLatin1String("curve")) {
            path.moveTo(b.left(), b.center().y());
            path.cubicTo(QPointF(b.left() + b.width() * 0.3, b.top() - b.height() * 0.4), QPointF(b.left() + b.width() * 0.7, b.bottom() + b.height() * 0.4),
                         QPointF(b.right(), b.center().y()));
        } else if (kind == QLatin1String("freeform")) {
            path.moveTo(b.left(), b.bottom());
            path.lineTo(b.left() + b.width() * 0.25, b.top());
            path.lineTo(b.left() + b.width() * 0.55, b.top() + b.height() * 0.6);
            path.lineTo(b.right(), b.top() + b.height() * 0.15);
            path.lineTo(b.right() - b.width() * 0.1, b.bottom());
            path.closeSubpath();
        } else {
            path.moveTo(b.left(), b.center().y());
            const int n = 5;
            for (int i = 0; i < n; ++i) {
                const double x0 = b.left() + b.width() * i / n, x1 = b.left() + b.width() * (i + 1) / n;
                path.cubicTo(QPointF(x0 + (x1 - x0) * 0.3, b.top()), QPointF(x0 + (x1 - x0) * 0.7, b.bottom()), QPointF(x1, b.center().y() + (i % 2 ? -1 : 1) * b.height() * 0.15));
            }
        }
        p->drawPath(path);
        // Arrowheads: at the end, and at the start too for a double arrow.
        auto head = [&](const QPointF &tip, const QPointF &from) {
            const QPointF d = tip - from;
            const double L = std::hypot(d.x(), d.y());
            if (L < 0.01) return;
            const QPointF u = d / L, n(-u.y(), u.x());
            const double len = r.width() * 0.2;
            p->setBrush(uiText());
            p->drawPolygon(QPolygonF({tip, tip - u * len + n * len * 0.45, tip - u * len - n * len * 0.45}));
        };
        const bool arrow = kind.endsWith(QLatin1String("Arrow")), dbl = kind.endsWith(QLatin1String("Double"));
        if (arrow || dbl) head(z, QPointF(mx, z.y()));
        if (dbl) head(a, QPointF(mx, a.y()));
    });
}

namespace {
// Dialog tabs never scroll: a tab bar asks for room for every tab, so the
// dialog opens wide enough to show them all instead of adding side arrows.
class TabBarPolish : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Polish)
            if (auto *tb = qobject_cast<QTabBar *>(o)) {
                tb->setUsesScrollButtons(false);
                tb->setElideMode(Qt::ElideNone);
            }
        return QObject::eventFilter(o, e);
    }
};
} // namespace

void installUiPolish()
{
    static bool done = false;
    if (done || !qApp) return;
    done = true;
    qApp->installEventFilter(new TabBarPolish(qApp));
}

} // namespace jp
