#pragma once
// PDF/X-1a:2001, the PDF commercial printers ask for, made from a PDF Qt
// wrote in its CMYK mode with transparency flattened: pictures become CMYK,
// the RGB color spaces Qt declares map to CMYK, every page gets its trim and
// bleed boxes, links (annotations) go, and the file names the printing
// condition it was prepared for. The conditions offered are registered with
// the ICC, so no color profile is embedded.

#include <QRectF>
#include <QString>
#include <QStringList>

namespace jp {

struct PdfXCondition {
    QString name;         // shown in the dialog
    QString identifier;   // OutputConditionIdentifier, in the ICC registry
    QString condition;    // OutputCondition
    QString info;         // the usual profile for it
};
const QVector<PdfXCondition> &pdfXConditions();

struct PdfXOptions {
    int condition = 0;   // index into pdfXConditions()
    QRectF trim;         // PDF points, from each page's bottom left
    QRectF bleed;        // contains trim, inside the page
};

// Rewrites the PDF at path in place. False, with the reason, when something
// in it can't be made PDF/X-1a (transparency left, a font not embedded).
bool makePdfX1a(const QString &path, const PdfXOptions &o, QString *error);

} // namespace jp
