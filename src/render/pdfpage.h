#pragma once
// PDF pages through PDFium. A page placed in a publication is a picture
// whose image is a one-page PDF (format "pdf"): on screen it's drawn from a
// raster (PDFium's own), and in PDFs, printing and SVG it's drawn as the
// page's own paths, glyph outlines and pictures, so it stays sharp.

#include <QByteArray>
#include <QImage>
#include <QRectF>
#include <QRegion>
#include <QSizeF>

#include <memory>

class QPainter;

namespace jp {

class PdfDocument {
public:
    explicit PdfDocument(const QByteArray &bytes);   // keeps its own copy
    ~PdfDocument();
    PdfDocument(const PdfDocument &) = delete;
    PdfDocument &operator=(const PdfDocument &) = delete;

    bool isValid() const;
    int pageCount() const;
    QSizeF pageSize(int page) const;             // points, as the page shows (turned pages turned)
    QByteArray extractPage(int page) const;      // the page alone, as a one-page PDF
    QImage render(int page, const QSize &px) const;   // PDFium's raster, white behind it
    // Draws the page's objects into target as vectors: paths, text (each
    // glyph's outline), pictures, form contents; shadings come from a raster.
    // The drawing is checked against PDFium's own (once per page), and
    // wherever they differ (hidden layers, blending, glyphs it can't name)
    // PDFium's raster is drawn there instead.
    void play(QPainter *p, int page, const QRectF &target) const;
    double rasterShare(int page) const;   // how much of the page play() draws from the raster, 0-1

    static bool looksLikePdf(const QByteArray &bytes);
    // The document for these bytes, kept for the next ask (a few at a time)
    // so each page is checked once however often it's drawn.
    static std::shared_ptr<const PdfDocument> shared(const QByteArray &bytes);

private:
    struct Check {
        QSize size;        // the check's pixels
        QRegion raster;    // in those pixels: where the raster is used
    };
    Check check(int page) const;
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace jp
