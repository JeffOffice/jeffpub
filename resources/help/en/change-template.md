# Change a publication's template
<!-- keywords: change template, apply template, another design, new design, restyle, switch template, extra content, leftover text, leftover pictures, placeholder, roles, create a new publication, move text and pictures -->

**Change Template** gives the publication you have open the look of a different design, and moves your text and pictures into it. You do not retype anything. Everything is one step that you can undo.

## Change the template

1. Click **Page Design > Template > Change Template**. The File view opens on a gallery of designs, headed **Change Template**.
2. Pick a design the way you do for a new publication: choose a group on the left, or type in the **Search templates** box, then click a design. See [start a new publication](create).
3. Use the panel on the right if you want to. **Color scheme** and **Font scheme** start at **(keep current)**, which keeps the colors and fonts you have. Pick a scheme there only if you want the new design to change them. **Business information** picks the set of contact details the design fills in. **Include logo** and **Include mailing address** show only for designs that use them.
4. Click **Change Template**. You can also double-click the design.
5. Choose where the new design goes, then click **OK**:
   - **Apply template to the current publication** changes the publication you have open.
   - **Create a new publication with my text and graphics** opens a second window with the result. The publication you started from is not touched.

The gallery shows designs only. To change the paper size of a publication without changing its design, use [page setup](page-setup).

## Where your text and pictures go

A **story** is the text in one text box, or in a chain of linked text boxes. Every text box in a design has a **role**, which says what the box is for. The roles are:

- Title (the headline) and subtitle
- Heading, for large type further into the publication
- Body text
- Date, which also holds a place or a time
- Address and contact details: the street address, phone, email, and web address
- Organization, which is your organization's name and tagline
- Person, which is a person's name and title
- Caption and pull quote
- Mailing address, the block that mail merge fills in
- Short text, for small labels

The designs that come with JeffPub carry these roles already. For a box you drew yourself, or one from a .pub file, JeffPub works the role out from its text (a date, a phone number), its style (Caption or Quote), and its size.

Each of your stories goes into the new design's box with the same role. For example, the story you wrote in the body of a flyer goes into the body box of a newsletter, and your organization's name goes into the masthead. If the new design has several boxes with the same role, your stories fill them in reading order: page by page, top to bottom, left to right.

Your story keeps its words, and its bold, italic, links, and fields. It takes on the new design's type, color, and spacing, so it looks like the rest of the design. The headline of a design that uses Text Art takes the first paragraph of your title story. The paragraphs after it go to Extra Content.

Your pictures go into the design's **placeholder pictures**, which are the stand-in pictures a design shows until you add your own. Your first picture takes the first placeholder in reading order, the next picture takes the next one, and so on. Each picture is cropped to fill its frame, and keeps its own brightness, contrast, and recoloring. Placeholders you have no picture for keep the design's stand-in.

A few things stay as they are:

- Text you never typed over is the old design's sample text. It is not carried over, and the new design shows its own sample text there.
- The page size, margins, pages, and master pages come from the new design. A publication that has more pages than the design keeps the design's pages, and the rest of your content goes to Extra Content. JeffPub does not add pages for it.
- Tables, shapes, lines, Text Art, and groups of objects that the old design made, and that you did not change, stay behind with it. Anything you drew, and anything you changed (a table with a cell you typed in, a shape whose words you edited, a line you moved), goes to Extra Content, whole.
- Your business information, document properties, styles, mail merge list, and the objects on the scratch area stay with the publication.

## Extra Content

Nothing you made is thrown away. Content with no place in the new design waits in the **Extra Content** pane, which opens beside the page when the change is done. It lists each leftover story, picture, table, shape, line, and group.

- Click an item, then click **Place on Page** to put it on the page you are looking at. A story becomes a text box that fits its text, a table or group comes back whole, and a picture or object that is too big is made smaller to fit the page. You can also double-click an item, or drag it onto the page.
- Click an item, then click **Discard** to remove it for good. (You can still undo.)

The pane stays with the publication until you close the publication, and it is saved in the publication's file. (A .pub file has no place for it, so saving as .pub leaves it out.) To open it again, click **Page Design > Template > Extra Content**. The button is dimmed when nothing is left over.

## Undo

**Undo** (Ctrl+Z) brings the old design back, and takes the leftovers out of Extra Content again. Placing or discarding an item is a step of its own.

## Related

- [Start a new publication](create)
- [Color and font schemes](color-schemes)
- [Page size, margins, and layout](page-setup)
- [The Page Design tab](tab-page-design)
