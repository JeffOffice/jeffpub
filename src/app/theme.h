#pragma once
// The light or dark look of the program, switchable while it runs.

#include <QObject>

namespace jp {

// Tells the parts that keep colors of their own (style sheets set once)
// that the look changed, so they redo them.
class UiTheme : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    static UiTheme *instance();
Q_SIGNALS:
    void changed();
};

// 0 follows the system, 1 light, 2 dark: sets the palette, the program's
// style sheet and uiDark(), then emits UiTheme::changed().
void applyUiTheme(int choice);

// Follows the system's light or dark setting while it runs, when the
// setting (ui/theme) says to.
void followSystemTheme();

} // namespace jp
