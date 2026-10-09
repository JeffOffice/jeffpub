#pragma once
// A ring around whichever control has the keyboard focus, shown while the
// keyboard is in use (Tab, arrow keys, KeyTips) and hidden after a mouse
// click, for every control at once: the ribbon's buttons, galleries and
// color buttons are drawn by JeffPub and draw no focus of their own.

class QApplication;

namespace jp {

void installFocusRing(QApplication *app);

} // namespace jp
