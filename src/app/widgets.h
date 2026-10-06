#pragma once
// Ribbon controls: scheme-aware color picker, galleries, font and size boxes,
// measurement spin boxes and the Insert Table grid.

#include "core/color.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QIcon>
#include <QToolButton>
#include <functional>

class QListWidget;
class QListWidgetItem;
class QGridLayout;

namespace jp {

// ---------- colors ----------
class ColorPopup : public QFrame {
    Q_OBJECT
public:
    ColorPopup(const ColorScheme &s, bool allowNone, const QString &noneLabel, QWidget *parent = nullptr);
    void showBelow(QWidget *anchor);
    static void addRecent(const QColor &c);

Q_SIGNALS:
    void picked(const ColorRef &c);
    void morePicked();
    void eyedropper();   // the user wants to pick a color from the window

private:
    QWidget *swatch(const ColorRef &c, const QColor &shown, const QString &tip);
    ColorScheme m_scheme;
};

class ColorButton : public QToolButton {
    Q_OBJECT
public:
    ColorButton(const QString &iconName, const QString &text, bool allowNone, const QString &noneLabel, QWidget *parent = nullptr);
    void setScheme(const ColorScheme &s) { m_scheme = s; refreshIcon(); }
    void setCurrent(const ColorRef &c) { m_current = c; refreshIcon(); }
    ColorRef current() const { return m_current; }
    void setExtraActions(const QList<QAction *> &actions) { m_extra = actions; }
    // Draw as a form field: a rounded box with a swatch, the color's name and
    // a chevron, the way dialogs show colors.
    void setWell(bool on);
    QSize sizeHint() const override;

Q_SIGNALS:
    void colorPicked(const ColorRef &c);

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    void refreshIcon();
    void openPopup();
    bool m_well = false;
    QString m_icon;
    bool m_allowNone;
    QString m_noneLabel;
    ColorScheme m_scheme;
    ColorRef m_current;
    QList<QAction *> m_extra;
};

// ---------- gallery ----------
struct GalleryItem {
    QString id;
    QString tip;
    QIcon icon;
    QString group;
};

class Gallery : public QFrame {
    Q_OBJECT
public:
    Gallery(const QSize &itemSize, int visibleColumns, QWidget *parent = nullptr);
    void setItems(const QVector<GalleryItem> &items);
    void setItemsProvider(const std::function<QVector<GalleryItem>()> &fn) { m_provider = fn; }
    void setFooterActions(const QList<QAction *> &a) { m_footer = a; }
    void setCurrent(const QString &id);
    void reload();   // refill the inline row from the provider

Q_SIGNALS:
    void activated(const QString &id);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void openMore();
    void scroll(int dir);
    QVector<GalleryItem> m_items;
    std::function<QVector<GalleryItem>()> m_provider;
    QList<QAction *> m_footer;
    QListWidget *m_list;
    QSize m_itemSize;
    int m_cols;
    QString m_current;
};

// A drop-down button that opens a grid of items (shapes, TextArt, page parts...).
class GalleryButton : public QToolButton {
    Q_OBJECT
public:
    GalleryButton(const QIcon &icon, const QString &text, const QSize &itemSize, int columns, bool large, QWidget *parent = nullptr);
    void setItemsProvider(const std::function<QVector<GalleryItem>()> &fn) { m_provider = fn; }
    void setFooterActions(const QList<QAction *> &a) { m_footer = a; }

Q_SIGNALS:
    void activated(const QString &id);

protected:
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void paintEvent(QPaintEvent *e) override;
    bool event(QEvent *e) override;

private:
    void openGrid();
    bool m_large = false;
    std::function<QVector<GalleryItem>()> m_provider;
    QList<QAction *> m_footer;
    QSize m_itemSize;
    int m_cols;
};

QFrame *galleryPopup(const QVector<GalleryItem> &items, const QSize &itemSize, int columns, const QList<QAction *> &footer,
                     const std::function<void(const QString &)> &onPick, QWidget *parent);

// ---------- text ----------
class FontCombo : public QComboBox {
    Q_OBJECT
public:
    explicit FontCombo(QWidget *parent = nullptr);
    void setSchemeFonts(const QString &heading, const QString &body);
    void setCurrentFamily(const QString &family);

Q_SIGNALS:
    void familyChosen(const QString &family);   // "+Heading", "+Body" or a family name

private:
    void populate();
    QString m_heading, m_body;
    bool m_updating = false;
};

class SizeCombo : public QComboBox {
    Q_OBJECT
public:
    explicit SizeCombo(QWidget *parent = nullptr);
    void setSize(double pt);

Q_SIGNALS:
    void sizeChosen(double pt);

private:
    bool m_updating = false;
};

// Number box that shows up to three decimal places, dropping trailing
// zeros, and keeps six, so what's typed (8.125) isn't rounded.
class DecimalSpin : public QDoubleSpinBox {
    Q_OBJECT
public:
    explicit DecimalSpin(QWidget *parent = nullptr);
    QString textFromValue(double v) const override;
};

// Spin box that shows and accepts measurements in the user's units.
class MeasureSpin : public QDoubleSpinBox {
    Q_OBJECT
public:
    explicit MeasureSpin(QWidget *parent = nullptr);
    QString textFromValue(double v) const override;
    double valueFromText(const QString &text) const override;
    QValidator::State validate(QString &input, int &pos) const override;
    void setPoints(double pt);
};

// Hover grid for Insert > Table.
class TableGrid : public QFrame {
    Q_OBJECT
public:
    explicit TableGrid(QWidget *parent = nullptr);
    QSize sizeHint() const override;

Q_SIGNALS:
    void picked(int rows, int cols);

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;

private:
    int m_rows = 0, m_cols = 0;
};

QWidget *popupFor(QWidget *content, QWidget *anchor);

// Eyedropper: a crosshair over `window` until a click picks the color under
// it (from a snapshot of the window) or Escape cancels.
void pickColorFromWindow(QWidget *window, std::function<void(const QColor &)> done);

// The Colors dialog: a Standard tab of swatches and a Custom tab that takes
// RGB, HSL or CMYK values (a CMYK color keeps its ink amounts) and a hex
// code, with transparency. Returns an invalid color when canceled.
QColor colorsDialog(QWidget *parent, const QColor &current, const QString &title = QStringLiteral("Colors"));

} // namespace jp
