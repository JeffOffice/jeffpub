#pragma once
// Custom QTextFormat properties JeffPub stores alongside Qt's own.

#include <QTextFormat>
#include <QTextListFormat>

namespace jp {
// Qt's list styles are all negative; these three are bullets, the rest numbers.
inline bool isBulletList(QTextListFormat::Style s)
{
    return s == QTextListFormat::ListDisc || s == QTextListFormat::ListCircle || s == QTextListFormat::ListSquare;
}
} // namespace jp

namespace jp::tp {

enum : int {
    // character
    ColorRefP = QTextFormat::UserProperty + 1,   // QString ColorRef for the text color
    HighlightRefP,                               // QString ColorRef for highlight
    ThemeFont,                                   // QString "major" or "minor": follows the font scheme
    Field,                                       // QString field code, run text is U+FFFC
    CharStyle,                                   // QString character style name
    Shadow,                                      // bool
    Emboss,                                      // bool
    Engrave,                                     // bool
    OutlineRef,                                  // QString ColorRef for text outline
    OutlineWidth,                                // double points
    Language,                                    // QString BCP-47
    NumberStyle,                                 // int: 0 default, 1 lining, 2 old-style
    NumberSpacing,                               // int: 0 default, 1 proportional, 2 tabular
    Ligatures,                                   // int: 0 standard, 1 none, 2 all
    StylisticSet,                                // int 0..20
    Swash,                                       // bool
    Alternates,                                  // bool
    TrueSmallCaps,                               // bool
    TextFill,                                    // QString Fill JSON (gradient text)
    GlowRef,                                     // QString ColorRef, glow on text
    NoProof,                                     // bool, skip spelling
    KernAbove,                                   // double: automatic pair kerning from this size up (points; 14 when unset)

    // block
    StyleName = QTextFormat::UserProperty + 100, // QString paragraph style
    DropCapLines,                                // int
    DropCapChars,                                // int
    DropCapFont,                                 // QString
    DropCapColor,                                // QString ColorRef
    DropCapUpper,                                // bool: raise above first line instead of dropping
    KeepWithNext,                                // bool
    KeepTogether,                                // bool
    WidowControl,                                // bool
    StartInNextBox,                              // bool
    AlignToBaseline,                             // bool
    Distribute,                                  // bool: distributed alignment
    BulletChar,                                  // QString custom bullet
    BulletFont,                                  // QString
    BulletColor,                                 // QString ColorRef
    ListLevel,                                   // int
    NumberFormat,                                // int: 0 bullet, 1 "1.", 2 "a.", 3 "A.", 4 "i.", 5 "I.", 6 "1)", 7 "(1)"
    NumberStart,                                 // int
    ListId,                                      // QString: blocks with the same id share numbering
    TabLeaders,                                  // QString per-tab leader characters
};

} // namespace jp::tp
