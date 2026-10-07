#pragma once
// Pages as SVG drawings: everything as vectors (pictures embedded), and
// letters as their outlines, so a page looks the same in any program
// whether or not it has the fonts.

#include "render/renderer.h"

#include <QByteArray>
#include <QString>

namespace jp {

QByteArray pageSvg(const PaintContext &ctx, int pageIndex, const QString &title);
bool writePageSvg(const PaintContext &ctx, int pageIndex, const QString &path, const QString &title, QString *error);

} // namespace jp
