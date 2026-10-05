#pragma once
// Built-in style presets shared by galleries and commands: table formats,
// shape styles, picture styles and page backgrounds.

#include "core/items.h"

#include <functional>

namespace jp {

// Applies fills and borders; textStyler (optional) formats each cell's text.
void applyTableFormatCells(TableItem *t, const QString &format, const std::function<void(TableCell &, bool header, bool firstColumn)> &textStyler);
void shapeStylePreset(int row, int slot, Fill *fill, Stroke *stroke, Effects *fx);
void pictureStylePreset(int k, PictureItem *pic);
Fill backgroundPreset(const QString &id);

} // namespace jp
