#pragma once
// Barcodes drawn as exact bars: book ISBNs (EAN-13 with an optional price
// add-on), EAN-13, UPC-A and EAN-8 (with 2- or 5-digit add-ons), Code 128 and
// Code 39. make() lays a barcode out in points, ready to become shapes.

#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

namespace jp::barcode {

enum class Type { Isbn, Ean13, UpcA, Ean8, Code128, Code39 };

struct Options {
    Type type = Type::Isbn;
    QString data;              // the ISBN, the digits or the text
    QString addOn;             // EAN/UPC/ISBN: "" or 2 or 5 digits printed beside the code
    double magnification = 1;  // 1 = the standard size (0.33 mm bars for EAN and UPC)
    double barHeight = 1;      // share of the standard bar height (books often print 0.8)
    bool text = true;          // the digits under (and the ISBN above) the bars
    bool code39Check = false;  // Code 39: add the mod-43 check character
};

struct Label {
    QString text;
    QPointF baseline;          // where the text's baseline is centered
    double size = 9;           // points
};

struct Layout {
    QVector<QRectF> bars;      // points, from the top-left of the barcode with its quiet zones
    QVector<Label> labels;
    QSizeF size;               // the whole barcode, quiet zones included
    QString encoded;           // what the bars hold, check digits included
    QString error;             // why the data can't be drawn (then nothing else is set)
};

Layout make(const Options &o);

// The 13 digits of an ISBN typed as 10 or 13 digits (spaces and hyphens
// allowed, X as an ISBN-10's check), or empty with the reason in *error.
QString isbn13(const QString &input, QString *error);
// "ISBN " and the ISBN as typed when it has hyphens, else as 13 digits.
QString isbnCaption(const QString &input);

// The 5-digit price add-on of a book: the currency's digit and the price in
// cents (US$19.95 -> 51995), 90000 for no suggested price.
enum class Currency { None, UsDollar, CanadianDollar, Pound, AustralianDollar, NewZealandDollar };
QString priceAddOn(Currency c, double price, QString *error);

// The check digit of an EAN/UPC number given without it.
int eanCheckDigit(const QString &digits);

} // namespace jp::barcode
