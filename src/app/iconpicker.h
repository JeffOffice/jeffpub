#pragma once
// Insert > Icons: the bundled Lucide icons, inserted as editable artwork
// shapes that take the line color and scale like pictures.

#include "core/items.h"

#include <QPair>
#include <QStringList>
#include <QVector>

class QWidget;

namespace jp {

class Editor;

// The bundled icons' names with their search words, in name order.
const QVector<QPair<QString, QStringList>> &iconCatalog();
// A searchable grid of the icons: the names chosen, or none when canceled.
QStringList pickIcons(QWidget *parent);
// An icon as an editable shape `size` points square centered at `center`,
// its lines drawn in `color` and as heavy as the icon's own at that size.
ItemPtr iconItem(const QString &name, const QPointF &center, double size, const ColorRef &color);
// Puts icons on the current page, an inch square each, in rows across its middle.
void insertIcons(Editor *ed, const QStringList &names);

} // namespace jp
