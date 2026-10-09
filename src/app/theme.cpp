#include "app/theme.h"

#include "app/icons.h"
#include "app/settings.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleHints>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace jp {

UiTheme *UiTheme::instance()
{
    static UiTheme *t = new UiTheme(qApp);
    return t;
}

namespace {
QPalette darkPalette()
{
    QPalette p;
    const QColor base(0x26, 0x26, 0x26), window(0x2f, 0x2f, 0x2f), text(0xe8, 0xe8, 0xe8);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, window);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, QColor(0xC2, 0x4A, 0x8A));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(0xE0, 0x7A, 0xB0));
    p.setColor(QPalette::PlaceholderText, QColor(0x9a, 0x9a, 0x9a));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x80, 0x80, 0x80));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x80, 0x80, 0x80));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x80, 0x80, 0x80));
    return p;
}

bool g_applying = false;   // main thread only

// Windows' high contrast setting (JP_HIGH_CONTRAST=1 or 0 overrides it, for
// checks on any system).
bool systemHighContrast()
{
    if (qEnvironmentVariableIsSet("JP_HIGH_CONTRAST")) return qEnvironmentVariableIntValue("JP_HIGH_CONTRAST") != 0;
#ifdef Q_OS_WIN
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
#else
    return false;
#endif
}

// The system's own colors for high contrast: on Windows, the colors the
// chosen contrast theme sets; elsewhere (checks), white on black with cyan
// highlights, as Windows' black contrast theme.
QPalette highContrastPalette()
{
    QColor window(Qt::black), text(Qt::white), base(Qt::black), button(Qt::black), buttonText(Qt::white), highlight(0x1A, 0xEB, 0xFF),
        highlightText(Qt::black), disabled(0x3F, 0xF2, 0x3F), link(0xFF, 0xFF, 0x00);
#ifdef Q_OS_WIN
    if (!qEnvironmentVariableIsSet("JP_HIGH_CONTRAST")) {
        auto sys = [](int i) {
            const DWORD c = GetSysColor(i);
            return QColor(GetRValue(c), GetGValue(c), GetBValue(c));
        };
        window = sys(COLOR_WINDOW);
        text = sys(COLOR_WINDOWTEXT);
        base = sys(COLOR_WINDOW);
        button = sys(COLOR_BTNFACE);
        buttonText = sys(COLOR_BTNTEXT);
        highlight = sys(COLOR_HIGHLIGHT);
        highlightText = sys(COLOR_HIGHLIGHTTEXT);
        disabled = sys(COLOR_GRAYTEXT);
        link = sys(COLOR_HOTLIGHT);
    }
#endif
    QPalette p;
    for (QPalette::ColorGroup g : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(g, QPalette::Window, window);
        p.setColor(g, QPalette::WindowText, g == QPalette::Disabled ? disabled : text);
        p.setColor(g, QPalette::Base, base);
        p.setColor(g, QPalette::AlternateBase, base);
        p.setColor(g, QPalette::Text, g == QPalette::Disabled ? disabled : text);
        p.setColor(g, QPalette::Button, button);
        p.setColor(g, QPalette::ButtonText, g == QPalette::Disabled ? disabled : buttonText);
        p.setColor(g, QPalette::Highlight, highlight);
        p.setColor(g, QPalette::HighlightedText, highlightText);
        p.setColor(g, QPalette::ToolTipBase, window);
        p.setColor(g, QPalette::ToolTipText, text);
        p.setColor(g, QPalette::Link, link);
        p.setColor(g, QPalette::PlaceholderText, disabled);
        p.setColor(g, QPalette::BrightText, text);
    }
    return p;
}
} // namespace

void applyUiTheme(int choice)
{
    if (g_applying) return;
    g_applying = true;
    QStyleHints *hints = QGuiApplication::styleHints();
    // Following the system leaves the scheme to it; a choice pins it.
    hints->setColorScheme(choice == 1 ? Qt::ColorScheme::Light : choice == 2 ? Qt::ColorScheme::Dark : Qt::ColorScheme::Unknown);
    // High contrast overrides the choice: the system's colors, and none of
    // JeffPub's own (no style sheet).
    const bool contrast = systemHighContrast();
    setUiHighContrast(contrast);
    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        if (contrast) {
            const QPalette pal = highContrastPalette();
            setUiDark(pal.color(QPalette::Window).lightness() < 128);
            app->setPalette(pal);
            app->setStyleSheet(QString());
        } else {
            const bool dark = choice == 2 || (choice == 0 && hints->colorScheme() == Qt::ColorScheme::Dark);
            setUiDark(dark);   // not every platform honors setColorScheme, so record the choice
            app->setPalette(dark ? darkPalette() : app->style()->standardPalette());
            app->setStyleSheet(modernStyleSheet());
        }
    }
    g_applying = false;
    Q_EMIT UiTheme::instance()->changed();
}

namespace {
// Windows tells every window when its colors or contrast change; one
// re-apply covers them all.
class ThemeChangeWatcher : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::ThemeChange && o->isWidgetType() && static_cast<QWidget *>(o)->isWindow() && !m_pending && !g_applying) {
            m_pending = true;
            QMetaObject::invokeMethod(this, [this] {
                m_pending = false;
                if (systemHighContrast() != uiHighContrast()) applyUiTheme(Settings::get().value(QStringLiteral("ui/theme"), 0).toInt());
            }, Qt::QueuedConnection);
        }
        return false;
    }

private:
    bool m_pending = false;
};
} // namespace

void followSystemTheme()
{
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, UiTheme::instance(), [] {
        if (Settings::get().value(QStringLiteral("ui/theme"), 0).toInt() == 0) applyUiTheme(0);
    });
    qApp->installEventFilter(new ThemeChangeWatcher(UiTheme::instance()));
}

} // namespace jp
