#pragma once
// Black overprinting in files for a printer: black text below a size, black
// lines and (when asked) black fills print over the inks under them, so a
// plate that shifts leaves no white edge. The publication's settings, as in
// the other program's Overprinting Settings, applied to a CMYK PDF Qt wrote.

#include "core/document.h"

namespace jp {

class QtPdf;

// Adds the overprint switches to every page's drawing. Returns how many
// places now overprint.
int applyOverprint(QtPdf &pdf, const OverprintSettings &o);

} // namespace jp
