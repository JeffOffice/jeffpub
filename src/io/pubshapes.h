#pragma once
// Preset shapes in .pub files: which of JeffPub's shapes Publisher has as a
// shape type of its own (the drawing format's shape number).

#include <QString>

namespace jp {

// Publisher's shape number for a preset, or -1 when Publisher has no such
// shape (the shape is then saved as a freeform outline).
int pubShapeType(const QString &preset);

// The preset for one of Publisher's shape numbers, or an empty string.
QString presetForPubShapeType(int type);

// Publisher's Text Art shape number for a Text Art transform (plain text
// when unknown), and back (an empty string when the number isn't Text Art).
int pubTextArtType(const QString &transform);
QString textArtTransformForPubType(int type);

// .pub files have no sepia setting: a sepia picture is saved recolored to
// this brown, and a picture recolored to it reads back as sepia.
constexpr unsigned kPubSepia = 0x704214;   // RGB

} // namespace jp
