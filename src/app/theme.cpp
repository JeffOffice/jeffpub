#include "app/theme.h"

#include "app/icons.h"
#include "app/settings.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleHints>

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
} // namespace

void applyUiTheme(int choice)
{
    if (g_applying) return;
    g_applying = true;
    QStyleHints *hints = QGuiApplication::styleHints();
    // Following the system leaves the scheme to it; a choice pins it.
    hints->setColorScheme(choice == 1 ? Qt::ColorScheme::Light : choice == 2 ? Qt::ColorScheme::Dark : Qt::ColorScheme::Unknown);
    const bool dark = choice == 2 || (choice == 0 && hints->colorScheme() == Qt::ColorScheme::Dark);
    setUiDark(dark);   // not every platform honors setColorScheme, so record the choice
    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        app->setPalette(dark ? darkPalette() : app->style()->standardPalette());
        app->setStyleSheet(modernStyleSheet());
    }
    g_applying = false;
    Q_EMIT UiTheme::instance()->changed();
}

void followSystemTheme()
{
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, UiTheme::instance(), [] {
        if (Settings::get().value(QStringLiteral("ui/theme"), 0).toInt() == 0) applyUiTheme(0);
    });
}

} // namespace jp
