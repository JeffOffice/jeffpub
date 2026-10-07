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
    Tracking,                                    // double: percent of normal letter spacing (100 when unset)
    LineSize,                                    // double: layout only, the (Qt) size a run's line spacing counts at
    InlineObject,                                // QString item JSON: an object set in the text, run text is U+FFFC

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
    BulletSize,                                  // double points: a list marker's own size (list format)
    TocLevel,                                    // int: a table of contents' paragraph (0 its title, 1-3 an entry), rebuilt by Update
};

// Copies every property of `from` onto `to`. QTextFormat::properties()
// builds a new map on each call, so a loop must walk one copy of it: begin()
// of one call and end() of another belong to two maps (and the first is
// gone by then).
inline void setProperties(QTextFormat &to, const QTextFormat &from)
{
    const QMap<int, QVariant> props = from.properties();
    for (auto it = props.cbegin(); it != props.cend(); ++it) to.setProperty(it.key(), it.value());
}

// Publisher's two kinds of letter spacing. Tracking is a percentage (100
// normal); older files kept it as Qt percentage spacing. Kerning is space in
// points added after each letter, kept as Qt absolute spacing.
inline double trackingOf(const QTextCharFormat &f)
{
    if (f.hasProperty(Tracking)) return f.property(Tracking).toDouble();
    if (f.hasProperty(QTextFormat::FontLetterSpacing) && f.fontLetterSpacingType() == QFont::PercentageSpacing) return f.fontLetterSpacing();
    return 100;
}
inline double kerningOf(const QTextCharFormat &f)
{
    return f.hasProperty(QTextFormat::FontLetterSpacing) && f.fontLetterSpacingType() == QFont::AbsoluteSpacing ? f.fontLetterSpacing() : 0;
}

} // namespace jp::tp
