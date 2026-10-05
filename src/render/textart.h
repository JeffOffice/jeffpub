#pragma once
// TextArt: text converted to outlines, warped through a transform (arch,
// circle, wave, inflate...), then stretched to the frame.

#include "core/items.h"

#include <QPainterPath>

namespace jp {

struct TextArtTransform {
    QString id;
    QString name;
};

const QVector<TextArtTransform> &textArtTransforms();
QPainterPath textArtPath(const TextArtItem &w, const QSizeF &size);

struct TextArtStyle {
    QString id;
    QString name;
    QString font;
    bool bold = false, italic = false;
    Fill fill;
    Stroke stroke;
    ShadowFx shadow;
    QString transform = QStringLiteral("plain");
    bool evenHeight = false;
    bool vertical = false;
};

const QVector<TextArtStyle> &textArtStyles();
void applyTextArtStyle(TextArtItem &w, const TextArtStyle &s);

} // namespace jp
