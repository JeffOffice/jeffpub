#pragma once
// File import/export helpers used by commands: Insert File, mail-merge data
// sources, printing layouts, and opening any supported publication.

#include "core/document.h"

#include <QJsonObject>
#include <QImage>
#include <QRectF>

class QPainter;

class QTextDocument;
class QPrinter;
class QWidget;

namespace jp {

class Editor;

bool loadTextFileInto(QTextDocument *doc, const QString &path);
QString docxToHtml(const QByteArray &docx);

bool loadMergeSource(const QString &path, MergeSource *out, QString *error);
bool saveMergeCsv(const MergeSource &m, const QString &path);
QVector<QStringList> parseDelimited(const QString &text, QChar sep);
std::unique_ptr<Document> mergeToNewPublication(const Document &src);
void mergeToEmailFiles(QWidget *parent, Editor *ed);

std::unique_ptr<Document> loadAnyPublication(const QString &path, QString *error);

// Printing. opts keys: layout ("one", "booklet", "bookletTop", "multiple", "tiled"),
// copiesPerSheet, grayscale, merged, separations (with plates: "CMYK" subset),
// and the printer's marks: cropMarks, bleedMarks, registration, densityBars,
// colorBars, jobInfo, allowBleeds.
void printDocument(Editor *ed, QPrinter *printer, const QJsonObject &opts);

// Printer's marks drawn outside a page's trim edge (all in points). They need
// about 48 pt of paper around the page.
struct PrinterMarks {
    bool crop = false, bleed = false, registration = false, density = false, colorBars = false, jobInfo = false;
    double bleedSize = 9;                // how far bleeds extend past the trim
    int plate = -1;                      // separations: draw swatches as this plate's ink
    static PrinterMarks fromJson(const QJsonObject &o);
    bool any() const { return crop || bleed || registration || density || colorBars || jobInfo; }
};
constexpr double kMarksMargin = 48;
void drawPrinterMarks(QPainter *p, const QRectF &page, const PrinterMarks &m, const QString &jobInfo);

// One process-color plate (0 cyan, 1 magenta, 2 yellow, 3 black) of a page
// image, as grayscale where black is full ink.
QImage separationPlate(const QImage &rgb, int plate);
QString plateName(int plate);
QVector<QVector<int>> bookletOrder(int pages);   // sheet sides -> page indices (-1 blank)

bool exportPublisher(const Document &doc, const QString &path, QString *error);
void compressPicturesDialog(QWidget *parent, Editor *ed);

} // namespace jp
