#include "io/pubshapes.h"

#include <QHash>

namespace jp {

namespace {

struct Pair { const char *preset; int type; bool same = false; };

// Shapes Publisher has too (and the .pub reader can draw, so they reopen
// in JeffPub). `same` marks those whose standard form looks exactly like
// JeffPub's at every proportion (checked with `jpubtool shapecheck`); the
// others differ in their default proportions, so they're saved as freeforms
// until their handle settings are converted.
const Pair kPairs[] = {
    {"rect", 1, true}, {"roundRect", 2, true}, {"ellipse", 3, true}, {"diamond", 4, true}, {"triangle", 5, true}, {"rtTriangle", 6, true},
    {"parallelogram", 7, true}, {"trapezoid", 8}, {"hexagon", 9, true}, {"octagon", 10}, {"plus", 11}, {"star5", 12},
    {"rightArrow", 13}, {"homePlate", 15}, {"cube", 16}, {"arc", 19}, {"plaque", 21}, {"can", 22}, {"donut", 23},
    {"ribbon", 53}, {"ribbon2", 54}, {"chevron", 55}, {"pentagon", 56}, {"noSmoking", 57}, {"star8", 58},
    {"star16", 59}, {"star32", 60}, {"wave", 64}, {"foldedCorner", 65, true}, {"leftArrow", 66}, {"downArrow", 67},
    {"upArrow", 68}, {"leftRightArrow", 69}, {"upDownArrow", 70}, {"irregularSeal1", 71}, {"irregularSeal2", 72},
    {"lightning", 73}, {"heart", 74}, {"quadArrow", 76}, {"bevel", 84}, {"leftBracket", 85}, {"rightBracket", 86},
    {"leftBrace", 87}, {"rightBrace", 88}, {"leftUpArrow", 89}, {"bentUpArrow", 90}, {"bentArrow", 91},
    {"star24", 92}, {"stripedRightArrow", 93}, {"notchedRightArrow", 94}, {"blockArc", 95}, {"smiley", 96},
    {"verticalScroll", 97}, {"horizontalScroll", 98}, {"circularArrow", 99}, {"uturnArrow", 101},
    {"curvedRightArrow", 102}, {"curvedLeftArrow", 103}, {"curvedUpArrow", 104}, {"curvedDownArrow", 105},
    {"fcProcess", 109, true}, {"fcDecision", 110, true}, {"fcData", 111, true}, {"fcPredefined", 112, true}, {"fcInternalStorage", 113, true},
    {"fcDocument", 114}, {"fcMultidocument", 115}, {"fcTerminator", 116}, {"fcPreparation", 117, true},
    {"fcManualInput", 118, true}, {"fcManualOperation", 119, true}, {"fcConnector", 120, true}, {"fcCard", 121, true},
    {"fcPunchedTape", 122}, {"fcSummingJunction", 123, true}, {"fcOr", 124, true}, {"fcCollate", 125, true}, {"fcSort", 126, true},
    {"fcExtract", 127, true}, {"fcMerge", 128, true}, {"fcStoredData", 130}, {"fcSequential", 131}, {"fcMagneticDisk", 132},
    {"fcDirectAccess", 133}, {"fcDisplay", 134}, {"fcDelay", 135}, {"fcAltProcess", 176}, {"fcOffpage", 177, true},
    {"sun", 183}, {"moon", 184}, {"bracketPair", 185}, {"bracePair", 186}, {"star4", 187}, {"doubleWave", 188},
    {"actionButtonBlank", 189},
};

const QHash<QString, int> &byPreset()
{
    static const QHash<QString, int> h = [] {
        QHash<QString, int> m;
        for (const Pair &p : kPairs)
            if (p.same) m.insert(QString::fromLatin1(p.preset), p.type);
        return m;
    }();
    return h;
}

} // namespace

int pubShapeType(const QString &preset) { return byPreset().value(preset, -1); }

QString presetForPubShapeType(int type)
{
    for (const Pair &p : kPairs)
        if (p.same && p.type == type) return QString::fromLatin1(p.preset);
    return QString();
}

} // namespace jp
