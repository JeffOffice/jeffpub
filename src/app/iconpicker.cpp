#include "app/iconpicker.h"

#include "app/editor.h"
#include "app/icons.h"
#include "core/document.h"
#include "core/svg.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <cmath>

namespace jp {

const QVector<QPair<QString, QStringList>> &iconCatalog()
{
    static const QVector<QPair<QString, QStringList>> catalog = [] {
        QVector<QPair<QString, QStringList>> c;
        QFile f(QStringLiteral(":/icon-tags.txt"));
        if (!f.open(QIODevice::ReadOnly)) return c;
        for (const QByteArray &line : f.readAll().split('\n')) {
            if (line.isEmpty() || line.startsWith('#')) continue;
            const QList<QByteArray> parts = line.split('\t');
            QStringList tags;
            if (parts.size() > 1)
                for (const QByteArray &t : parts[1].split(','))
                    if (!t.isEmpty()) tags << QString::fromUtf8(t).toLower();
            c << qMakePair(QString::fromUtf8(parts[0]), tags);
        }
        return c;
    }();
    return catalog;
}

namespace {
QString shownName(const QString &name) { return QString(name).replace('-', ' '); }

// A cell of the grid: the icon, on a rounded accent tint when chosen.
class IconCell : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {52, 52}; }
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF cell = QRectF(opt.rect).adjusted(2.5, 2.5, -2.5, -2.5);
        QColor tint = uiAccent();
        if (opt.state & QStyle::State_Selected) {
            tint.setAlpha(60);
            p->setPen(QPen(uiAccent(), 1.5));
            p->setBrush(tint);
            p->drawRoundedRect(cell, 6, 6);
        } else if (opt.state & QStyle::State_MouseOver) {
            tint.setAlpha(28);
            p->setPen(Qt::NoPen);
            p->setBrush(tint);
            p->drawRoundedRect(cell, 6, 6);
        }
        QRect r(0, 0, 32, 32);
        r.moveCenter(opt.rect.center());
        qvariant_cast<QIcon>(idx.data(Qt::DecorationRole)).paint(p, r);
        p->restore();
    }
};
}

QStringList pickIcons(QWidget *parent)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Insert Icons"));
    auto *v = new QVBoxLayout(&dlg);
    auto *search = new QLineEdit(&dlg);
    search->setPlaceholderText(QStringLiteral("Search icons (for example: arrow, mail, star)"));
    search->setClearButtonEnabled(true);
    v->addWidget(search);
    auto *list = new QListWidget(&dlg);
    list->setViewMode(QListView::IconMode);
    // Icons only, as names cut short help no one: the name shows in its
    // tip and below the grid once chosen.
    list->setIconSize(QSize(32, 32));
    list->setGridSize(QSize(56, 56));
    list->setUniformItemSizes(true);
    list->setItemDelegate(new IconCell(list));
    list->setMouseTracking(true);
    list->setMovement(QListView::Static);
    list->setResizeMode(QListView::Adjust);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setObjectName(QStringLiteral("iconList"));
    for (const auto &[name, tags] : iconCatalog()) {
        auto *it = new QListWidgetItem(icon(name), QString(), list);
        it->setData(Qt::UserRole, name);
        it->setData(Qt::AccessibleTextRole, shownName(name));
        it->setToolTip(tags.isEmpty() ? shownName(name) : shownName(name) + QStringLiteral(" (") + tags.join(QStringLiteral(", ")) + ')');
    }
    v->addWidget(list, 1);
    auto *hint = new QLabel(&dlg);
    v->addWidget(hint);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QPushButton *insert = bb->button(QDialogButtonBox::Ok);
    insert->setText(QStringLiteral("Insert"));
    v->addWidget(bb);

    auto update = [&] {
        QStringList chosen;
        int shown = 0;
        for (int i = 0; i < list->count(); ++i) {
            shown += !list->item(i)->isHidden();
            if (list->item(i)->isSelected()) chosen << shownName(list->item(i)->data(Qt::UserRole).toString());
        }
        insert->setEnabled(!chosen.isEmpty());
        if (chosen.isEmpty()) {
            hint->setText(shown ? QStringLiteral("%1 icons. Ctrl+click or Shift+click to choose several.").arg(shown)
                                : QStringLiteral("No icons match. Try another word."));
            return;
        }
        QString names = chosen.size() > 3 ? chosen.mid(0, 3).join(QStringLiteral(", ")) + QStringLiteral(" and %1 more").arg(chosen.size() - 3)
                                          : chosen.join(QStringLiteral(", "));
        hint->setText(QStringLiteral("Chosen: %1").arg(names));
    };
    // Every word typed must begin a word of the icon's name or of one of its tags.
    QObject::connect(search, &QLineEdit::textChanged, &dlg, [&](const QString &text) {
        const QStringList words = text.toLower().split(QRegularExpression(QStringLiteral("[\\s,-]+")), Qt::SkipEmptyParts);
        const auto &catalog = iconCatalog();
        for (int i = 0; i < list->count(); ++i) {
            const auto &[name, tags] = catalog[i];
            const QString hay = QLatin1Char(' ') + shownName(name) + QLatin1Char(' ') + tags.join(QLatin1Char(' ')).replace('-', ' ');
            bool all = true;
            for (const QString &w : words) all = all && hay.contains(QLatin1Char(' ') + w);
            list->item(i)->setHidden(!all);
            if (!all) list->item(i)->setSelected(false);
        }
        update();
    });
    QObject::connect(list, &QListWidget::itemSelectionChanged, &dlg, update);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, [&](QListWidgetItem *it) {
        it->setSelected(true);
        dlg.accept();
    });
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    update();
    dlg.resize(720, 540);
    search->setFocus();
    if (dlg.exec() != QDialog::Accepted) return {};
    QStringList names;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->isSelected() && !list->item(i)->isHidden()) names << list->item(i)->data(Qt::UserRole).toString();
    return names;
}

ItemPtr iconItem(const QString &name, const QPointF &center, double size, const ColorRef &color)
{
    QFile f(QStringLiteral(":/icons/%1.svg").arg(name));
    if (!f.open(QIODevice::ReadOnly)) return {};
    // Drawn in a color no icon uses, which then becomes `color`.
    const QColor marker(1, 2, 3);
    const svg::Drawing d = svg::read(f.readAll(), marker);
    ItemPtr it = svg::shapes(d, QRectF(center.x() - size / 2, center.y() - size / 2, size, size), true, marker, color);
    if (!it) return {};
    it->name = shownName(name) + QStringLiteral(" icon");
    it->altText = shownName(name);
    return it;
}

void insertIcons(Editor *ed, const QStringList &names)
{
    if (names.isEmpty()) return;
    const QSizeF ps = ed->doc()->pageSize();
    const double size = 72, step = size * 1.25;
    const int perRow = std::max(1, int(ps.width() * 0.8 / step)), n = int(names.size());
    const int rows = (n + perRow - 1) / perRow;
    QStringList made;
    ed->beginChange(names.size() == 1 ? QStringLiteral("Insert Icon") : QStringLiteral("Insert Icons"));
    for (int i = 0; i < n; ++i) {
        const int row = i / perRow, inRow = std::min(perRow, n - row * perRow);
        const QPointF c(ps.width() / 2 + (i % perRow - (inRow - 1) / 2.0) * step, ps.height() / 2 + (row - (rows - 1) / 2.0) * step);
        if (ItemPtr it = iconItem(names[i], c, size, ColorRef::scheme(Main))) {
            ed->surfaceItems().push_back(it);
            made << it->id;
        }
    }
    ed->endChange();
    ed->select(made);
}

} // namespace jp
