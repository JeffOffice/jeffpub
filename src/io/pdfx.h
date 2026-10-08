#pragma once
// PDF/X, the PDFs commercial printers ask for.
//
// PDF/X-1a:2001 is made from a PDF Qt wrote in its CMYK mode with
// transparency flattened: pictures become CMYK, the RGB color spaces Qt
// declares map to CMYK, every page gets its trim and bleed boxes, links
// (annotations) go, and the file names the printing condition it was
// prepared for. Those conditions are registered with the ICC, so no color
// profile is embedded.
//
// PDF/X-4 (ISO 15930-7:2010) keeps transparency. Qt writes it (its PDF/X-4
// version, in CMYK) with the printing condition's color profile in the
// output intent and the identification in the XMP metadata; the same pass
// then turns pictures into CMYK and sets the boxes. The profile is ECI's,
// downloaded on first use (its terms let anyone use and embed it but not
// pass it on), or the printer's own.

#include "core/document.h"

#include <QByteArray>
#include <QRectF>
#include <QString>
#include <QStringList>

namespace jp {

struct PdfXCondition {
    QString name;         // shown in the dialog
    QString identifier;   // OutputConditionIdentifier, in the ICC registry
    QString condition;    // OutputCondition
    QString info;         // the usual profile for it
    bool x4 = false;      // PDF/X-4, its profile embedded
    // Where a PDF/X-4 profile comes from: a zip from its maker, checked by
    // SHA-256, and the profile in it (also checked).
    QString profileUrl, profileFile;
    QByteArray zipSha256, profileSha256;
    QString profileSize;  // for the question before downloading
    bool ownProfile = false;   // the printer's own profile, from a file
};
const QVector<PdfXCondition> &pdfXConditions();

// The profile from a downloaded zip, when the zip and the profile in it are
// the ones expected; empty, with the reason, otherwise.
QByteArray pdfXProfileFromZip(const PdfXCondition &c, const QByteArray &zip, QString *error);

struct PdfXOptions {
    int condition = 0;   // index into pdfXConditions()
    QRectF trim;         // PDF points, from each page's bottom left
    QRectF bleed;        // contains trim, inside the page
    OverprintSettings overprint;   // the publication's black overprinting
};

// Rewrites the PDF at path in place. False, with the reason, when something
// in it can't be made PDF/X-1a (transparency left, a font not embedded).
bool makePdfX1a(const QString &path, const PdfXOptions &o, QString *error);
// The same for the PDF/X-4 Qt wrote: pictures in CMYK (their see-through
// parts kept), trim and bleed, no links over the page.
bool makePdfX4(const QString &path, const PdfXOptions &o, QString *error);

} // namespace jp
