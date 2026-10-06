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

} // namespace jp
