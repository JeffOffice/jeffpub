#pragma once
// Moving the keyboard focus with the arrow keys among a set of controls
// (the ribbon, color and gallery grids), by where they are on screen.

#include <QList>

class QWidget;

namespace jp {

// Inside `scope`, the arrow keys move the focus to the nearest control in
// that direction (controls that use the arrows themselves, such as text
// fields and lists, keep them), and Enter or Space presses the button with
// the focus (opening its menu if it only has one).
void installArrowNavigation(QWidget *scope);

// The control `from` would move to with an arrow key (Qt::Key_Left...),
// among the controls given, or nullptr.
QWidget *nearestInDirection(QWidget *from, int key, const QList<QWidget *> &candidates);

} // namespace jp
