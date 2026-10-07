/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * This file is part of the libmspub project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef INCLUDED_SHAPEINFO_H
#define INCLUDED_SHAPEINFO_H

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <array>
#include <vector>

#include <boost/optional.hpp>

#include "Arrow.h"
#include "ColorReference.h"
#include "Coordinate.h"
#include "Dash.h"
#include "Fill.h"
#include "Line.h"
#include "MSPUBTypes.h"
#include "Margins.h"
#include "PolygonUtils.h"
#include "Shadow.h"
#include "ShapeType.h"
#include "TableInfo.h"
#include "VerticalAlign.h"

namespace libmspub
{
void noop(const CustomShape *);
struct ShapeInfo
{
  boost::optional<unsigned> m_type;
  boost::optional<unsigned> m_cropType;
  boost::optional<unsigned> m_imgIndex;
  boost::optional<unsigned> m_borderImgIndex;
  boost::optional<Coordinate> m_coordinates;
  std::vector<Line> m_lines;
  boost::optional<unsigned> m_pageSeqNum;
  boost::optional<unsigned> m_textId;
  unsigned m_textChainIndex = 0;
  // JeffPub patch: the shape's sequence number, and for a connector the
  // shapes (by sequence number) and connection sites its ends attach to.
  unsigned m_jpSeqNum = 0;
  boost::optional<unsigned> m_wrap;   // JeffPub patch: text wrapping (0 none, 1 square, 2 tight, 3 through, 4 top and bottom)
  boost::optional<unsigned> m_inlineNum;   // JeffPub patch: an object set in text, by the number its story's EOBJ gives it
  boost::optional<std::array<int, 4> > m_wrapDistances; // JeffPub patch: EMU left, top, right, bottom
  boost::optional<std::pair<unsigned, unsigned> > m_glueStart, m_glueEnd;   // JeffPub 79: the box's place among the boxes sharing its story
  std::map<unsigned, int> m_adjustValuesByIndex;
  std::vector<int> m_adjustValues;
  boost::optional<double> m_rotation;
  boost::optional<std::pair<bool, bool> > m_flips;
  // JeffPub 79: Text Art (words, font, size, spacing, alignment, settings).
  boost::optional<librevenge::RVNGPropertyList> m_textArt;
  boost::optional<Margins> m_margins;
  boost::optional<BorderPosition> m_borderPosition; // Irrelevant except for rectangular shapes
  std::shared_ptr<const Fill> m_fill;
  boost::optional<DynamicCustomShape> m_customShape;
  bool m_stretchBorderArt;
  boost::optional<ColorReference> m_lineBackColor;
  boost::optional<Dash> m_dash;
  boost::optional<TableInfo> m_tableInfo;
  boost::optional<unsigned> m_numColumns;
  boost::optional<unsigned> m_textFlow;   // JeffPub 79: txflTextFlow (text direction)
  unsigned m_columnSpacing;
  boost::optional<Arrow> m_beginArrow;
  boost::optional<Arrow> m_endArrow;
  boost::optional<double> m_lineOpacity; // JeffPub patch: 0x01C1
  boost::optional<std::vector<double>> m_crop; // JeffPub patch: top, bottom, left, right (0x0100-0x0103)
  boost::optional<double> m_pictureFillOpacity; // JeffPub patch: 0x0182 with a picture fill
  boost::optional<std::vector<double> > m_fillInks; // JeffPub patch: the fill's process inks (0x019F, 0x01A6)
  unsigned m_fillInksColor = 0;                     // ...for this fill color as shown (0x019E)
  std::string m_fillSpot;                           // JeffPub patch: the spot ink the fill is (0x01A1), or empty
  boost::optional<VerticalAlign> m_verticalAlign;
  boost::optional<ColorReference> m_pictureRecolor;
  boost::optional<Shadow> m_shadow;
  boost::optional<int> m_innerRotation;
  std::vector<libmspub::Vertex> m_clipPath;
  boost::optional<int> m_pictureBrightness;
  boost::optional<int> m_pictureContrast;
  boost::optional<unsigned> m_pictureFlags;            // JeffPub patch: 0x013F (gray, black and white)
  boost::optional<ColorReference> m_pictureTransparent; // JeffPub patch: 0x0107
  ShapeInfo() : m_type(), m_cropType(), m_imgIndex(), m_borderImgIndex(),
    m_coordinates(), m_lines(), m_pageSeqNum(),
    m_textId(), m_adjustValuesByIndex(), m_adjustValues(),
    m_rotation(), m_flips(), m_margins(), m_borderPosition(),
    m_fill(), m_customShape(), m_stretchBorderArt(false),
    m_lineBackColor(), m_dash(), m_tableInfo(),
    m_numColumns(),
    m_columnSpacing(0), m_beginArrow(), m_endArrow(),
    m_verticalAlign(), m_pictureRecolor(), m_shadow(), m_innerRotation(), m_clipPath(), m_pictureBrightness(), m_pictureContrast()
  {
  }
  std::shared_ptr<const CustomShape> getCustomShape() const
  {
    if (bool(m_customShape))
    {
      return getFromDynamicCustomShape(m_customShape.get());
    }
    if (bool(m_cropType))
    {
      return std::shared_ptr<const CustomShape>(
               libmspub::getCustomShape(m_cropType.get()),
               std::function<void (const CustomShape *)>(noop));
    }
    return std::shared_ptr<const CustomShape>(
             libmspub::getCustomShape(m_type.get_value_or(RECTANGLE)),
             std::function<void (const CustomShape *)>(noop));
  }
};
}
#endif
/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
