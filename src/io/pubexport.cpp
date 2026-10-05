// Saving as a .pub file. The writer is being built on the structures
// libmspub documents; until it produces files that open everywhere, saving reports that plainly instead of writing a broken file.

#include "io/importers.h"

namespace jp {

bool exportPublisher(const Document &doc, const QString &path, QString *error)
{
    Q_UNUSED(doc);
    Q_UNUSED(path);
    if (error)
        *error = QStringLiteral("Saving as a .pub file isn't available yet. Save as a JeffPub Publication (.jpub) or export a PDF for now.");
    return false;
}

} // namespace jp
