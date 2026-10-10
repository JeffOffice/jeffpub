/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * This file is part of the libmspub project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef INCLUDED_MSPUBTYPES_H
#define INCLUDED_MSPUBTYPES_H

#include <string>
#include <vector>

#include <boost/optional.hpp>

#include "ListInfo.h"
#include "MSPUBBlockID.h"
#include "MSPUBBlockType.h"
#include "MSPUBConstants.h"
#include "MSPUBContentChunkType.h"

namespace libmspub
{
enum BorderPosition
{
  INSIDE_SHAPE,
  HALF_INSIDE_SHAPE,
  OUTSIDE_SHAPE
};

enum SuperSubType
{
  NO_SUPER_SUB,
  SUPERSCRIPT,
  SUBSCRIPT
};

enum class Underline
{
  None,
  Single,
  WordsOnly,
  Double,
  Dotted,
  Thick,
  Dash,
  DotDash,
  DotDotDash,
  Wave,
  ThickWave,
  ThickDot,
  ThickDash,
  ThickDotDash,
  ThickDotDotDash,
  LongDash,
  ThickLongDash,
  DoubleWave,
};

enum Alignment
{
  LEFT = 0,
  CENTER = 2,
  RIGHT = 1,
  JUSTIFY = 6
};

struct EscherContainerInfo
{
  unsigned short initial;
  unsigned short type;
  unsigned long contentsLength;
  unsigned long contentsOffset;
};

struct MSPUBBlockInfo
{
  MSPUBBlockInfo() : id(0), type(0), startPosition(0), dataOffset(0), dataLength(0), data(0), stringData() { }
  unsigned id;
  unsigned type;
  unsigned long startPosition;
  unsigned long dataOffset;
  unsigned long dataLength;
  unsigned data;
  std::vector<unsigned char> stringData;
};

struct ContentChunkReference
{
  ContentChunkReference() : type(0), offset(0), end(0), seqNum(0), parentSeqNum(0) { }
  ContentChunkReference(unsigned t, unsigned long o, unsigned long e, unsigned sn, unsigned psn) :
    type(t), offset(o), end(e), seqNum(sn), parentSeqNum(psn) {}
  unsigned type;
  unsigned long offset;
  unsigned long end; //offset of the last element plus one.
  unsigned seqNum;
  unsigned parentSeqNum;
};

struct QuillChunkReference
{
  QuillChunkReference() : length(0), offset(0), id(0), name(), name2() { }
  unsigned long length;
  unsigned long offset;
  unsigned short id;
  std::string name;
  std::string name2;
};

struct CharacterStyle
{
  CharacterStyle() :
    underline(), italic(), bold(),
    textSizeInPt(), colorIndex(-1), fontIndex(), superSubType(NO_SUPER_SUB)
    , outline(false)
    , shadow(false)
    , smallCaps(false)
    , allCaps(false)
    , emboss(false)
    , engrave(false)
    , textScale()
    , lcid()
  {
  }
  boost::optional<Underline> underline;
  bool italic;
  bool bold;
  boost::optional<double> textSizeInPt;
  int colorIndex;
  boost::optional<unsigned> fontIndex;
  SuperSubType superSubType;
  bool outline;
  bool shadow;
  bool smallCaps;
  bool allCaps;
  bool emboss;
  bool engrave;
  boost::optional<double> textScale;
  // JeffPub: text outline (character "line" container 0x59): color index and width in EMUs.
  int outlineColorIndex = -1;
  unsigned outlineWidthEmu = 0;
  // JeffPub: strikethrough (0x10), spacing added between letters in EMUs
  // (0x1B, "kerning" in Publisher's dialog) and tracking in tenths of a
  // percent (0x1F, 1000 = normal).
  bool strike = false;
  int letterSpacingEmu = 0;
  bool hasLetterSpacing = false;   // JeffPub: set by the run itself (0 can be its own value)
  // JeffPub patch: a field in the text (00 low byte 5 with 22 = -1: the page
  // number; -7 the next page's, -6 the previous page's).
  int jpField = 0;
  int jpFieldArg = 0;
  int trackingPerMille = 0;
  boost::optional<unsigned> lcid;
};

enum LineSpacingType
{
  LINE_SPACING_SP,
  LINE_SPACING_PT
};

struct LineSpacingInfo
{
  LineSpacingType m_type;
  double m_amount;
  LineSpacingInfo() : m_type(LINE_SPACING_SP), m_amount(1)
  {
  }
  LineSpacingInfo(LineSpacingType type, double amount) :
    m_type(type), m_amount(amount)
  {
  }
};

struct ParagraphStyle
{
  boost::optional<Alignment> m_align;
  boost::optional<unsigned> m_defaultCharStyleIndex;
  boost::optional<LineSpacingInfo> m_lineSpacing;
  int m_alignRaw = -1;   // JeffPub: the raw alignment code, for diagnostics
  boost::optional<unsigned> m_spaceBeforeEmu;
  boost::optional<unsigned> m_spaceAfterEmu;
  boost::optional<int> m_firstLineIndentEmu;
  boost::optional<unsigned> m_leftIndentEmu;
  boost::optional<unsigned> m_rightIndentEmu;
  boost::optional<ListInfo> m_listInfo;
  std::vector<unsigned> m_tabStopsInEmu;
  // JeffPub: each tab's alignment (0 left, 1 right, 2 center, 3 decimal),
  // and the list as Publisher stores it: kind (23 bullet, else a numbering
  // style), bullet character, and number punctuation (high half of 0x58).
  std::vector<int> m_tabAligns;
  std::vector<int> m_tabLeaders;   // each tab's leader character, 0 for none
  int m_listKind = -1;
  int m_listChar = 0;
  int m_listDelim = -1;
  unsigned m_listSizeEmu = 0;   // the bullet or number's size (0x02)
  int m_listFontIndex = -1;     // the bullet's font in the font table (0x03)
  boost::optional<unsigned> m_dropCapLines;
  boost::optional<unsigned> m_dropCapLetters;
  int m_direction = -1;   // JeffPub: 0x3A, 0xF3FF on a right-to-left paragraph
  ParagraphStyle() :
    m_align(), m_defaultCharStyleIndex(), m_lineSpacing(), m_spaceBeforeEmu(),
    m_spaceAfterEmu(), m_firstLineIndentEmu(), m_leftIndentEmu(),
    m_rightIndentEmu(), m_listInfo(), m_tabStopsInEmu(), m_dropCapLines(), m_dropCapLetters()
  {
  }
};

struct TextSpan
{
  TextSpan(const std::vector<unsigned char> &c, const CharacterStyle &s) : chars(c), style(s) { }
  std::vector<unsigned char> chars;
  CharacterStyle style;
};

struct TextParagraph
{
  TextParagraph(const std::vector<TextSpan> &sp, const ParagraphStyle &st) : spans(sp), style(st) { }
  std::vector<TextSpan> spans;
  ParagraphStyle style;
};

struct Color
{
  Color() : r(0), g(0), b(0) { }
  Color(unsigned char red, unsigned char green, unsigned char blue) : r(red), g(green), b(blue) { }
  unsigned char r, g, b;
};

enum PageType
{
  MASTER,
  NORMAL,
  DUMMY_PAGE
};

enum ImgType
{
  UNKNOWN,
  PNG,
  JPEG,
  WMF,
  EMF,
  TIFF,
  DIB,
  PICT,
  JPEGCMYK
};

} // namespace libmspub

#endif /* INCLUDED_MSPUBTYPES_H */
/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
