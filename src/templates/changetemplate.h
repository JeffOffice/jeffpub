#pragma once
// Page Design > Change Template: another design applied to an open
// publication. The text and pictures move into the new design, each story
// into the box with the same role, and what has no place waits in Extra
// Content.
//
// A role (Item::role) says what a box is for. The built-in designs tag
// their boxes when they are built; a box without a tag (one you drew, or one
// from a .pub file) gets the role its text and place suggest.

#include "core/document.h"
#include "templates/templates.h"

#include <QHash>
#include <memory>

namespace jp {

namespace role {
inline const QString Title = QStringLiteral("title");               // the headline (a text box or a TextArt)
inline const QString Subtitle = QStringLiteral("subtitle");         // a second line of display type on the first page
inline const QString Heading = QStringLiteral("heading");           // display type below the first page
inline const QString Body = QStringLiteral("body");                 // running text
inline const QString Date = QStringLiteral("date");                 // when, and often where
inline const QString Address = QStringLiteral("address");           // the street address, phone, email, and web address
inline const QString Organization = QStringLiteral("organization"); // the organization's name and tagline
inline const QString Person = QStringLiteral("person");             // a person's name and title
inline const QString Caption = QStringLiteral("caption");
inline const QString Quote = QStringLiteral("quote");               // a pull quote
inline const QString Recipient = QStringLiteral("recipient");       // the mailing address block of a mail merge
inline const QString Label = QStringLiteral("label");               // other short text
inline const QString Picture = QStringLiteral("picture");           // a picture or its placeholder
inline const QString Logo = QStringLiteral("logo");                 // the business logo
} // namespace role

// The role of every text box (the first box of each story), TextArt, and
// picture on the pages: its tag, else the one its text and place suggest.
// Keyed by item id.
QHash<QString, QString> contentRoles(const Document &d);
// Writes the roles contentRoles() finds into the boxes that have no tag.
void tagRoles(Document &d);

// Options for building a design to apply to `current`: the gallery's choices
// stand, and a scheme the gallery left at "(template default)" stays the
// publication's own.
TemplateOptions optionsForChange(const Document &current, TemplateOptions o);

struct ChangeReport {
    int stories = 0, pictures = 0;               // moved into the design
    int extraStories = 0, extraPictures = 0;     // with no place in it, now in Extra Content
    int extraObjects = 0;                        // tables, shapes, lines, and groups that were not the old design's own, there too
};

// `design` with `current`'s stories and pictures in it, its business
// information, properties, and mail merge list, and the Extra Content it
// already had. The result takes the design's page size and pages.
std::unique_ptr<Document> applyDesign(const Document &current, std::unique_ptr<Document> design, ChangeReport *report = nullptr);

} // namespace jp
