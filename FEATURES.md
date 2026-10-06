# Feature list

The features a complete desktop publishing program needs, and where JeffPub 79 stands on each. A box is checked only when the feature works in JeffPub 79. Last reviewed October 5, 2026.

Legend: `[x]` done · `[ ]` not yet · `[~]` partial (the note says what's missing) · **(alt)** the usual way to do this depends on a proprietary online service, so JeffPub 79 provides an open replacement with the same purpose.

## File (backstage)

### Info
- [x] Business Information: create, edit, and switch between named sets (name, tagline, contact person, title, address, phone/fax/email, logo)
- [x] Commercial Print Information: color model (RGB, process CMYK, spot colors, process + spot) and embedded-font management *(spot colors are named inks: they print on their own separation plates and are Separation colors in PDFs; a process-color publication makes a CMYK PDF, with colors entered as CMYK kept exactly)*
- [x] Run Design Checker from Info
- [x] Publication properties (title, author, subject, keywords, comments)

### New
- [x] Template gallery with live previews and categories: Award Certificates, Banners, Brochures, Business Cards, Business Forms, Calendars, Catalogs, Email, Envelopes, Flyers, Gift Certificates, Greeting Cards, Invitation Cards, Labels, Letterhead, Menus, Newsletters, Paper Folding Projects, Postcards, Programs, Resumes, Signs, With Compliments Cards *(48 designs, at least two in every category)*
- [x] Customize a template before creating it: color scheme, font scheme, business information, and template options (for example "include logo" or "mailing address") *(the New page shows the options each template has: "Include logo" on 15 templates, which places the business logo and makes room for it, and "Include mailing address" on postcards)*
- [x] Blank page sizes, More Blank Page Sizes, Create New Page Size
- [x] Save as a template and reuse your own templates ("My Templates")

### Open / Save / Save As / Close
- [x] Open JeffPub publications (`.jpub`) and recent publications, with pinning
- [~] Open `.pub` files (versions from 1998 to 2021), including linked text boxes, pictures, shapes, and tables *(partial: text, pictures, shapes and tables open; Publisher's shapes come back as JeffPub's own where they match, and picture crops, transparency, brightness, contrast and recoloring are kept; checked against PDFs of the same files, most covers and pages now match closely)*
- [~] Save as `.pub` *(partial; each part checked by opening the saved file in another .pub program. Saved: pages and master pages, text boxes and linked text boxes, character formatting (font, size, bold, italic, underline, strikethrough, small and all caps, superscript and subscript, color, letter spacing and tracking), paragraphs (alignment, indents, spacing, line spacing, tab stops with leaders, bullets and numbering with their fonts, right-to-left direction), every shape (as Publisher's own where it matches, otherwise an exact outline), lines with dashes and arrowheads, text in shapes, rotation and flips, pictures with cropping, transparency, brightness, contrast, recoloring and a clear color, pictures cut to a shape, tables with merged cells, fills (solid, gradient, picture, texture and pattern) with transparency, borders with transparency, shadows, Text Art, named paragraph styles (each paragraph keeps its style), process (CMYK) colors. Saving reports objects it leaves out)*
- [x] Save, Save As, AutoRecover, and backup on save
- [x] Close and prompt to save changes

### Print
- [x] Print with printer selection, copies, page ranges (all, current, custom), and printer properties
- [x] Layouts: one page per sheet, multiple pages per sheet, multiple copies per sheet, booklet (side-fold and top-fold), and tiled printing for banners and posters
- [x] Paper size, one-sided or two-sided (flip on long or short edge), color, grayscale, or composite
- [x] Print preview with ruler, page navigation, front/back view, and "show numbers"
- [x] Crop marks, bleed marks, registration marks, density bars, color bars, job information, and "allow bleeds"
- [x] Separations (CMYK and spot plates): a plate per process ink and per spot color; tints of a spot color print as lighter ink on its plate, and spot colors are knocked out of the process plates
- [x] Mail merge and catalog merge printing

### Share and Export
- [x] Create PDF (standard, minimum size, high quality, commercial press; page ranges; document properties; PDF/A; open the PDF after saving it)
- [x] Save pages as pictures: PNG, JPEG, GIF, TIFF, and BMP at a chosen resolution
- [x] Web page export (single-file HTML)
- [x] Pack and Go: Save for a Commercial Printer, Save for Another Computer (bundle fonts and linked pictures)
- [x] Save for a Photo Printer
- [x] **(alt)** Email: send the current page as a picture, or the publication as an attachment. JeffPub 79 creates a ready-to-send `.eml`, PDF, or HTML file instead of sending through a mail program.

### Options
- [x] General (user name and initials, UI theme), Proofing (AutoCorrect of common misspellings, symbols, sentence capitals and two initial capitals; smart quotes; spelling options), Save (AutoRecover interval and location, backup copies), Language, and Advanced (auto-select entire word, drag-and-drop text, hyphenation in new text boxes, automatic hyphenation zone, measurement units, recent file count, nudge amount, print and display options)

## Home tab
- [x] Clipboard: Paste (Keep Source Formatting, Keep Text Only, Paste Special), Cut, Copy, Format Painter (single use and locked)
- [x] Font: font face with scheme fonts first, size, Grow Font, Shrink Font, Clear All Formatting, Bold, Italic, Underline (styles), Strikethrough, Subscript, Superscript, Change Case, Character Spacing, Font Color (scheme colors, tints, more colors, eyedropper), and the Font dialog
- [x] Paragraph: Bullets (gallery and custom bullet character), Numbering (formats and starting number), Decrease and Increase Indent, Special Characters (show ¶), Align Left, Center, Right, Justify, and Distribute, Line Spacing, Paragraph Spacing, and the Paragraph dialog (indents, spacing, line breaks, tabs, baseline alignment)
- [x] Styles: gallery, New Style, Modify Style, Import Styles (from a publication), and Styles by example
- [x] Objects: Draw Text Box, Table, Pictures, Shapes
- [x] Arrange: Wrap Text, Bring Forward or to Front, Send Backward or to Back, Group, Ungroup, Align and Distribute (relative to margin guides or objects), Rotate and Flip
- [x] Editing: Find, Replace (match case, whole word, search direction), Select (Select All Text in Text Box, Select All, Select Objects, Select Object by Type)

## Insert tab
- [x] Pages: Insert Blank Page, Insert Duplicate Page, Insert Page dialog (count, before/after, blank, duplicate, or one text box per page), Catalog Pages (a catalog area repeated for each item of a product list, in rows and columns, with preview and merging to a new publication, PDF or printer)
- [x] Tables: grid picker and Insert Table dialog
- [x] Illustrations: Pictures (from file), Online Pictures **(alt: Openverse and Wikimedia Commons search with license info)**, Shapes (full gallery), Picture Placeholder, Barcode (book ISBN from ISBN-10 or -13, with a price add-on in US, Canadian, Australian or New Zealand dollars or pounds, or no suggested price; EAN-13, UPC-A and EAN-8 with 2- or 5-digit add-ons; Code 128; Code 39; magnification and bar height; vector bars and digits on a white quiet zone)
- [x] Building Blocks: Page Parts (headings, pull quotes, sidebars, stories, tables of contents), Calendars (by month and year, with options), Borders & Accents, Advertisements, and saving your own building blocks to the library
- [x] Text: Draw Text Box, Business Information fields, Text Art, Insert File (text, RTF, HTML, and `.docx` documents into a text box), Symbol (recently used symbols, kept between sessions, in the Symbol drop-down and the dialog, and the full character map), Date & Time (formats, update automatically), Object (embed a file)
- [x] Links: Hyperlink (web, email, place in this document, new document), Bookmark
- [x] Header & Footer: Header, Footer (open the master page), Page Number (position, format, start number)

## Page Design tab
- [x] Template: Change Template, Options
- [x] Page Setup: Margins (margin guide presets, and Custom Margins opening the Margin Guides settings), Orientation, Size (presets, the user's saved custom sizes, Create New Page Size, and editing or deleting saved sizes), and the Page Setup dialog (layout type: one page per sheet, booklet, envelope, folded card, multiple pages per sheet, labels; for several pages per sheet, the sheet's side and top margins and the gaps between pages, kept apart from the margin guides, with how many fit)
- [x] Printing several pages per sheet (business cards, labels) places them by the publication's own side and top margins and gaps
- [x] Guides: built-in guide layouts, Grid and Baseline Guides dialog (columns, rows, spacing, center guide, baseline spacing and offset), Add Horizontal and Vertical Ruler Guides (each new one beside the last), Ruler Guides dialog (add, move and remove guides by position, or add a series at even spacing), Clear All Ruler Guides
- [x] Layout: Align To Guides, Align To Objects
- [x] Pages: Delete, Move, Rename, Master Pages (apply or none)
- [x] Schemes: color scheme gallery (90+ schemes), Create New Color Scheme, font schemes, Create New Font Scheme, Update Font Scheme
- [x] Page Background: solid and gradient presets and More Backgrounds (gradient, texture, pattern, picture, tint), Apply Image as Background

## Mailings tab
- [x] Mail Merge and Email Merge
- [x] Step-by-Step Mail Merge Wizard (task pane)
- [x] Select Recipients: Type a New List, Use an Existing List (CSV, tab-delimited, `.xlsx` spreadsheets, and `.jpub` tables), Select from Contacts **(alt: vCard `.vcf` import)**
- [x] Edit Recipient List: sort, filter, find duplicates, validate addresses, include or exclude records
- [x] Address Block, Greeting Line, Insert Merge Field, Picture Field
- [x] Preview Results with record navigation and Find Recipient
- [x] Finish & Merge: Merge to Printer, Merge to New Publication, Merge to Email **(alt: one `.eml` per recipient)**, Export recipient list
- [x] Catalog merge (repeating product layouts from a data source)

## Review tab
- [x] Spelling (check as you type, dialog with suggestions, ignore, add to dictionary, and hide spelling errors)
- [x] Thesaurus
- [x] Research **(alt: dictionary lookup)**
- [x] Translate **(alt: opens the selected text in LibreTranslate, an open-source translation service)**
- [~] Language: set proofing language for selected text *(partial: US English dictionaries only)*

## View tab
- [x] Views: Normal, Master Page
- [x] Layout: Single Page, Two-Page Spread
- [x] Show: Boundaries, Guides, Fields, Rulers, Page Navigation, Scratch Area, Baselines, Graphics Manager
- [x] Zoom: zoom box, 100%, Whole Page, Page Width, Selected Objects, and Ctrl+mouse-wheel
- [x] Window: New Window, Arrange All, Cascade, Switch Windows

## Master Page tab (while editing master pages)
- [x] Add Master Page, Duplicate, Rename, Delete, Two-Page Master, Apply To, Close Master Page
- [x] Header and footer editing with Show Header/Footer and Insert Page Number

## Drawing Tools / Shape Format tab
- [x] Insert Shapes gallery, Edit Shape (Change Shape and Edit Points), Draw Text Box
- [x] Shape Styles gallery, Shape Fill (colors, picture, gradient, texture, pattern, no fill), Shape Outline (color, weight, dashes, arrows, pattern), Change Shape
- [x] Shape Effects: Shadow, Reflection, Glow, Soft Edges, Bevel, 3-D Rotation
- [x] Shadow Effects and 3-D Effects (classic galleries and nudge controls)
- [x] Arrange group and Size group (height and width)
- [x] Add text inside shapes
- [x] Shape adjustment handles (yellow diamonds)

## Text Box Tools Format tab
- [x] Text: Text Fit (Best Fit, Shrink Text On Overflow, Grow Text Box to Fit, Do Not Autofit), Text Direction (vertical), Hyphenation (automatic and manual)
- [x] Alignment: nine-way alignment, Columns (count and spacing), Margins (presets and custom)
- [x] Linking: Create Link (pitcher cursor), Break, Previous, Next, overflow indicator, autoflow
- [x] Font group mirrored from Home
- [x] Effects: Shadow, Outline, Emboss, Engrave, Text Fill (solid and gradient), Text Outline, Text Effects (shadow, reflection, glow, bevel)
- [x] Typography: Drop Cap (gallery and custom: lines, size, font, color), Number Style (lining, old-style, proportional, tabular), Ligatures, Stylistic Sets, Stylistic Alternates, Swash, Small Caps, True Small Caps
- [x] Arrange and Size

## Picture Tools Format tab
- [x] Insert: Change Picture, Swap (between picture frames), Insert Pictures
- [x] Adjust: Corrections (brightness and contrast presets), Recolor (presets, more variations, set transparent color), Compress Pictures, Reset Picture
- [x] Picture Styles gallery, Picture Border, Picture Effects, Caption gallery, Picture Shape (any closed shape)
- [x] Crop (handles, drag picture inside frame), Crop to Shape, Fit, Fill, Clear Crop, Pan picture
- [x] Arrange and Size
- [x] Picture placeholders with click-to-insert
- [x] Scratch-area picture tray behavior (pictures parked on the scratch area): inserting several pictures at once puts them beside the page as thumbnails, Arrange Thumbnails tidies them, and Swap trades two pictures
- [x] Graphics Manager pane (list, status of linked or embedded pictures, relink, save a copy)

## Table Tools Design and Layout tabs
- [x] Table Formats gallery, Fill, Borders (style, weight, color, which borders), Diagonals
- [x] Insert rows and columns above, below, left, and right; Delete rows, columns, and table; Select cell, row, column, and table; View Gridlines
- [x] Merge Cells, Split Cells, Diagonals
- [x] Cell alignment (nine-way), Text Direction, Hyphenation, Cell Margins
- [x] Size: column widths and row heights by dragging and by value, Grow to Fit Text, Lock
- [x] Tab moves between cells, and Tab in the last cell adds a row

## Text Art Tools Format tab
- [x] Edit Text dialog (text, font, size, bold, italic)
- [x] Spacing, Even Height, Vertical Text, Align Text
- [x] Text Art Styles gallery (30 classic plus modern styles), Shape Fill, Shape Outline, Change Shape (Text Art transforms: arch, circle, button, wave, slant, inflate, deflate, fade, triangle, chevron, cascade, can, and others)
- [x] Shadow Effects and 3-D Effects

## Object behavior and canvas
- [x] Frames: text box, picture, shape, line, arrow, table, Text Art, group, embedded object
- [x] Select (click, Shift or Ctrl-click, marquee, Tab cycling, select through groups)
- [x] Move, resize (aspect lock with Shift, from center with Ctrl), rotate (15° steps with Shift), flip, nudge with arrows and Alt+arrows, and Ctrl-drag to copy
- [x] Snap to margin and grid guides, ruler guides, other objects, and ruler marks, with visual alignment guides
- [x] Layering, grouping, and locking (lock position and size)
- [x] Text wrapping: None, Square, Tight, Through, Top and Bottom, Edit Wrap Points, and wrap distances
- [x] Format Object dialog: Colors and Lines, Size (with rotation and scale), Layout (position, wrap, distances), Picture, Text Box (margins, autofit, columns, vertical alignment), and Alt Text
- [x] Measurement toolbar: x, y, width, height, rotation, tracking (percent), text scaling, kerning (points), and line spacing
- [x] Measurements to three decimal places in every unit (8.125", 1.234 cm, 10.125 pt), shown without trailing zeros and kept without rounding
- [x] Scratch area shared across pages
- [x] Rulers in in, cm, mm, pt, and pica, with movable origin and drag-out guides
- [x] Context menus for every object type
- [x] Undo and redo with full history
- [x] Two-page spreads and facing pages

## Text engine
- [x] Rich text: fonts, sizes, color (scheme-aware), highlight, underline styles, strike, sub and superscript, all caps, small caps, tracking (a percentage, with Publisher's Very Tight to Very Loose presets) and kerning (points) together, automatic pair kerning (for fonts 14 pt and above unless set otherwise), and scaling
- [x] Paragraphs: alignment, indents (left, right, first line, hanging), spacing before and after, line spacing (single, multiple, exact, at least), keep with next, keep lines together, widow and orphan control, start in next text box
- [x] Tabs with leaders (left, center, right, decimal)
- [x] Bullets and numbering, multilevel
- [x] Text styles (paragraph and character) with Based On and Style for Following Paragraph
- [x] Linked text frames across pages, columns inside frames, text wrap around objects, vertical alignment, autofit
- [x] Continued-on and continued-from notices
- [x] Fields: page number, page count, date and time, business information, and mail merge
- [x] Hyphenation and optional hyphens, nonbreaking spaces and hyphens
- [x] Find and Replace across all stories
- [x] AutoCorrect (replacement list, capitalization rules) and AutoFormat-as-you-type
- [x] Right-to-left and complex script text via Qt's shaping engine, with a right-to-left paragraph direction (Home tab and the Paragraph dialog)

## Color and graphics
- [x] Color schemes with 8 slots (Main, Accents 1–5, Hyperlink, Followed Hyperlink) and tints and shades of every scheme color
- [x] Custom colors (RGB, HSL, CMYK, PANTONE-style spot color entry), recent colors, eyedropper, and transparency on fills, lines, and text *(spot colors are entered by name and value, since the PANTONE library itself is proprietary; colors entered as CMYK keep their exact ink amounts)*
- [x] Fill effects: gradient (linear, radial, rectangular, path, with presets), texture, pattern, picture (stretch or tile), and tint
- [x] Picture formats: PNG, JPEG, GIF, BMP, TIFF, WebP, SVG, and EMF/WMF **(EMF/WMF import is limited on Linux)**

## Platform and packaging
- [x] Builds and runs on Linux (x86_64) and Windows 10/11 (x86_64)
- [x] Windows installer and portable zip
- [x] Linux packages: `.deb` (Debian, Ubuntu) and AppImage
- [ ] macOS app (Apple silicon and Intel) in a `.dmg`, signed and notarized so it opens without warnings
- [x] File associations for `.jpub`
- [x] Light and dark interface themes
- [x] Standard desktop publishing keyboard shortcuts (Ctrl+B, Ctrl+Shift+>, F9, Ctrl+Shift+L, Ctrl+M, Ctrl+Shift+N, and others)
- [x] Bundled open fonts, including metric-compatible replacements for Calibri, Cambria, Arial, Times New Roman, and Courier New
