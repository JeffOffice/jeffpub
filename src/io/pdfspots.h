#pragma once
// Spot colors in PDFs: Qt writes every color as process (CMYK), so after a
// PDF is written, colors that are a publication's spot colors (or tints of
// them) are turned into Separation colors named for their inks.

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector>

namespace jp {

bool addPdfSpotColors(const QString &path, const QVector<QColor> &colors, const QStringList &names, QString *error = nullptr);

} // namespace jp
