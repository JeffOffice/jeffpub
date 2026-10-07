# The .jpub file format

A `.jpub` file is JeffPub 79's own publication format. It is open: this
document describes every part of it, so any program can read or write one.
This describes format version 1, which JeffPub 79 has written since its
first preview.

## At a glance

A `.jpub` file is a ZIP archive holding:

| Entry | What it is |
|---|---|
| `mimetype` | The text `application/x-jeffpub`, always the first entry, uncompressed |
| `document.json` | The whole publication: pages, objects, text, styles, colors and settings |
| `images/<id>.<ext>` | Each picture the publication uses, as the original file (`.png`, `.jpg`, `.svg`, ...) |
| `thumbnail.png` | Optional: a small picture of the first page, for file browsers |

JeffPub writes every entry uncompressed ("stored"). Readers should also
accept compressed ("deflated") entries, because other ZIP tools may
recompress a file. Entry names are UTF-8.

### Conventions

- **Units.** Every length is in points (1/72 inch) unless this document says
  otherwise. Angles are in degrees.
- **Coordinates.** Each page's origin is its top-left corner, with *x* going
  right and *y* going down.
- **Rectangles** are arrays `[x, y, width, height]`. **Margins and insets** are
  arrays `[left, top, right, bottom]`. **Sizes** are `[width, height]`.
- **Defaults.** Many keys are left out when they hold their default value,
  which is given here. A reader must treat a missing key as its default.
- **Unknown keys.** A reader must ignore keys it doesn't know, so later
  versions can add keys without breaking older readers.
- **Ids** are short strings unique within the file (`"p3f1"`, `"s12"`). They
  have no meaning beyond linking things together.

### Colors

A color is written as a string, in one of these forms:

| Form | Meaning |
|---|---|
| `none` | No color |
| `#RRGGBB` | An RGB color, hexadecimal, upper case |
| `#AARRGGBB` | An RGB color with alpha (opacity) first |
| `@n` | Color `n` of the publication's color scheme: 0 main (text), 1-5 accents 1-5, 6 hyperlink, 7 followed hyperlink |
| `@n+t` | Scheme color `n` lightened to a tint: `t` percent of the way to white |
| `@n-s` | Scheme color `n` darkened to a shade: `s` percent of the way to black |
| `cmyk(c,m,y,k)` | Process (CMYK) inks, each 0-100 percent; a fifth number is alpha, 0-255 |
| `cmyk(...)=#RRGGBB` | Process inks, followed by the RGB color they are shown as on screen |

Scheme colors follow the color scheme: change the scheme and every `@n`
color changes with it.

## document.json

The root object:

| Key | Type | Meaning |
|---|---|---|
| `format` | string | Always `"jeffpub"` |
| `version` | number | `1` |
| `setup` | object | Page size and how pages print ([Page setup](#page-setup)) |
| `masters` | array | Master pages ([Pages](#pages-and-master-pages)) |
| `pages` | array | The pages, in order |
| `scratch` | array | Objects on the scratch area beside the pages ([Objects](#objects)) |
| `stories` | array | The text of every text box, shape, table cell and note ([Stories](#stories)) |
| `styles` | array | Named paragraph and character styles ([Styles](#styles)) |
| `colorScheme` | object | `name` and `colors`: 8 color strings, in the slot order above |
| `fontScheme` | object | `name`, `heading` and `body` font families |
| `images` | array | One entry per picture ([Pictures](#pictures)) |
| `business` | array | Business information sets ([Business information](#business-information)) |
| `businessCurrent` | number | Index of the set in use |
| `merge` | object | Mail merge data ([Mail merge](#mail-merge)) |
| `catalog` | object | Only when the publication is a catalog: its merge area |
| `props` | object | File properties ([Properties](#properties)) |
| `template` | string | The template it was made from, if any |
| `templateOptions` | object | That template's choices (free form) |
| `facing` | boolean | Pages show as two-page spreads |
| `pubFonts` | array | When the publication came from a `.pub` file: the font names it used, kept as they are when saving back to `.pub` |
| `print` | object | Color output settings ([Printing colors](#printing-colors)) |

### Page setup

| Key | Type | Meaning |
|---|---|---|
| `size` | size | The page's width and height |
| `margins` | margins | Margin guides |
| `sizeName` | string | The size's name as shown, such as `"Letter"` |
| `layout` | number | 0 one page per sheet, 1 booklet, 2 several per sheet, 3 envelope, 4 folded card, 5 labels |
| `fold` | number | Folded cards: 0 side fold quarter sheet, 1 top fold quarter sheet, 2 side fold half sheet, 3 top fold half sheet |
| `sheet` | size | The paper the pages print on |
| `gridRows`, `gridCols` | number | Several per sheet and labels: pages across and down |
| `gapH`, `gapV` | number | Space between those pages |
| `sideMargin`, `topMargin` | number | Space from the sheet's edges to the first page |

### Pages and master pages

Every page and master page has:

| Key | Type | Meaning |
|---|---|---|
| `id` | string | Its id |
| `items` | array | Its objects, back to front ([Objects](#objects)) |
| `background` | object | A fill behind everything ([Fills](#fills)); none when left out |
| `guidesH`, `guidesV` | array | Ruler guides: *y* positions of horizontal ones, *x* positions of vertical ones |

A page also has `master` (the id of its master page, or empty for none) and
an optional `title`. A master page also has `name`, `abbr` (its one-letter
abbreviation), `twoPage` (a two-page master), and `grid`, its layout guides:

| Key | Meaning |
|---|---|
| `cols`, `rows`, `colGap`, `rowGap` | Column and row guides and the gaps between them |
| `center` | A center guide between columns |
| `baseline`, `baselineOffset` | Baseline guides: spacing and the first one's offset (0 = none) |
| `hBaseline`, `hBaselineOffset` | The same for vertical text |

### Objects

Every object is a JSON object with a `type`: `text`, `picture`, `shape`,
`line`, `table`, `textart` or `group`. All of them have:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `type` | string | | The kind of object |
| `id` | string | | Its id |
| `name` | string | empty | Its name in the Selection pane |
| `alt` | string | empty | Alternative text, for screen readers and exports |
| `link` | string | empty | A hyperlink the whole object opens |
| `rect` | rectangle | | Its frame, before rotation |
| `rot` | number | 0 | Rotation clockwise about the frame's center |
| `flipH`, `flipV` | boolean | false | Mirrored left to right, top to bottom |
| `locked` | boolean | false | Can't be moved or resized |
| `fill` | object | none | What fills it ([Fills](#fills)) |
| `stroke` | object | none | Its outline ([Lines](#lines)) |
| `fx` | object | none | Shadow, glow and other effects ([Effects](#effects)) |
| `wrap` | object | | How text flows around it ([Text wrapping](#text-wrapping)) |

Objects that hold text point to a story by its id. Positions inside an
object (paths, picture placement) are *frame-local*: relative to the
frame's top-left corner, before rotation.

#### text: text box

| Key | Default | Meaning |
|---|---|---|
| `story` | | The story it shows |
| `next` | empty | The next box the story continues in |
| `insets` | | Space between the frame and the text |
| `columns`, `columnGap` | 1, 9 | Columns and the space between them |
| `valign` | 0 | Vertical alignment: 0 top, 1 middle, 2 bottom |
| `autofit` | 0 | 0 none, 1 best fit, 2 shrink text on overflow, 3 grow the box to fit |
| `fitScale` | 1 | The text scale best fit chose, so the file opens exactly as saved |
| `vertical` | false | Text runs top to bottom |
| `hyph` | true | Hyphenate automatically |
| `hyphZone` | 18 | The hyphenation zone |
| `contOn`, `contFrom` | false | Show "Continued on page" and "Continued from page" notices |

A story flowing through linked boxes appears in each box in turn: the first
box is the one no other box names as `next`.

#### picture

| Key | Default | Meaning |
|---|---|---|
| `image` | empty | The picture's id ([Pictures](#pictures)); empty for a picture placeholder |
| `imgRect` | | Where the whole picture sits, frame-local. Parts outside the frame are cropped |
| `mask` | `rect` | A shape the picture is cut to (a shape name, as for shapes) |
| `brightness`, `contrast` | 0 | Each from -100 to 100 |
| `recolor` | 0 | 0 none, 1 grayscale, 2 sepia, 3 washout, 4 black and white, 5 tinted with `recolorColor` |
| `recolorColor` | | The tint, when `recolor` is 5 |
| `transparentColor` | none | A color made see-through |
| `transparency` | 0 | 0 opaque to 1 invisible |
| `caption` | empty | The caption design it was inserted with |

#### shape

| Key | Default | Meaning |
|---|---|---|
| `shape` | `rect` | The shape's name: `rect`, `ellipse`, `roundRect`, `star5`, ... (the names JeffPub's Shapes gallery uses) |
| `adj` | none | The shape's adjustment handles, each 0 to 1 |
| `path` | none | Its own outline, frame-local, replacing the named shape ([Paths](#paths)) |
| `winding` | false | The outline fills by the nonzero winding rule instead of even-odd |
| `story` | empty | Text inside the shape |
| `insets`, `valign` | | As for text boxes |

The shape `art` is artwork (an icon, or an SVG picture turned into shapes):
it always has a `path`, its line weight scales when it is resized, and its
corners keep its proportions.

**Paths** are arrays of elements `[type, x, y]`: type 0 moves to a point,
1 draws a line to it, and 2 starts a cubic Bézier curve: a type-2 element
is the first control point, and the two elements after it (type 3) are the
second control point and the end point.

#### line

`p1` and `p2`, each `[x, y]`, are its ends on the page. Its look is in
`stroke`, including arrowheads.

#### table

| Key | Meaning |
|---|---|
| `rows`, `cols` | Its size |
| `colW`, `rowH` | Each column's width and each row's height |
| `cells` | `rows` × `cols` cells, row by row |
| `format` | The table design applied, such as `"Table Style 1"` |
| `grow` | Rows grow to fit their text |
| `lockSize` | Typing never changes the table's size |
| `header`, `banded` | The design's header row and banded rows are on |

Each cell has `story` and `margins`, and may have `fill`, `rs` and `cs` (it
spans that many rows and columns), `covered` (hidden under another cell's
span), `diag` (a diagonal line: 1 top-left to bottom-right, 2 the other
way), `valign`, and `border`, with a [line](#lines) for each side present:
`t`, `b`, `l`, `r`.

#### textart

| Key | Meaning |
|---|---|
| `text` | The words |
| `font`, `size`, `bold`, `italic` | The lettering |
| `even` | All letters the same height |
| `vertical` | Letters stacked top to bottom |
| `spacing` | Letter spacing, as a percent of normal |
| `align` | Alignment within the frame |
| `transform`, `adj` | The text shape (such as an arch) and its adjustment |
| `style` | The Text Art style it was made with |

Its fill, outline and effects are the object's own `fill`, `stroke` and `fx`.

#### group

`children` holds the grouped objects, each with its own position on the
page. A barcode made by Insert > Barcode is a group with a `barcode`
object holding the settings it was made with, so it can be edited.

### Fills

| Key | Meaning |
|---|---|
| `type` | `none`, `solid`, `gradient`, `picture`, `texture` or `pattern` |
| `color` | The color (the first color of a gradient, the foreground of a pattern) |
| `transparency` | 0 opaque to 1 clear (default 0) |
| `gradType` | Gradients: `linear`, `radial`, `rectangular` or `path` |
| `angle` | Linear gradients: the direction, 90 = top to bottom |
| `color2` | Gradients without stops: the second color. Patterns: the background |
| `stops` | Gradients: `pos` (0-1), `color` and `t` (transparency) for each stop |
| `image`, `tile`, `tileScale` | Picture and texture fills: the picture's id, whether it repeats, and at what scale |
| `pattern` | Pattern fills: which pattern, by number |

### Lines

| Key | Default | Meaning |
|---|---|---|
| `color` | | The line's color |
| `width` | 0.75 | Its weight |
| `dash` | 0 | 0 solid, 1 round dots, 2 square dots, 3 dashes, 4 dash-dot, 5 long dashes, 6 long dash-dot, 7 long dash-dot-dot |
| `compound` | 0 | 0 single, 1 double, 2 thick-thin, 3 thin-thick, 4 triple |
| `transparency` | 0 | 0 opaque to 1 clear |
| `join` | 0 | Corners: 0 mitered, 64 beveled, 128 rounded |
| `cap` | 0 | Ends: 0 flat, 16 square, 32 round |
| `startArrow`, `endArrow` | 0 | 0 none, 1 triangle, 2 open, 3 stealth, 4 diamond, 5 oval |
| `startSize`, `endSize` | 1 | Arrowhead size: 0 small, 1 medium, 2 large |

### Effects

Each effect is present only when it is on.

| Key | Contents |
|---|---|
| `shadow` | `color`, `t` (transparency), `blur`, `dist` (distance), `angle`, `inner` (an inner shadow) |
| `glow` | `color`, `size`, `t` |
| `softEdge` | A number: how far the edges fade |
| `reflection` | `t`, `size` (percent of the object), `dist`, `blur` |
| `bevel` | `type` (a bevel style, by number), `w`, `h` |
| `rot3d` | `x`, `y` (rotation about each axis) and `p` (perspective) |

### Text wrapping

| Key | Meaning |
|---|---|
| `mode` | 0 none, 1 square, 2 tight, 3 through, 4 top and bottom |
| `side` | 0 both sides, 1 left only, 2 right only, 3 the larger side |
| `t`, `b`, `l`, `r` | Distance from the text |
| `points` | Edited wrap points, frame-local, as a flat list `x1, y1, x2, y2, ...` |

## Stories

Each story is an object with `id`, `blocks` (its paragraphs, in order) and,
when it has lists, `lists`.

A block has:

| Key | Meaning |
|---|---|
| `f` | The paragraph's formatting |
| `cf` | The paragraph's own character formatting (for an empty paragraph) |
| `list` | If the paragraph is a list item: its list's index in `lists` |
| `r` | The runs: pieces of text that share formatting, each `{"t": text, "f": formatting}` |

Paragraphs never contain line ends: each block is one paragraph. A line
break within a paragraph is U+2028. Optional hyphens are U+00AD.

### Formatting

A formatting object maps a property number (written as a decimal string) to
a typed value `[type, value]`:

| Type | Value |
|---|---|
| `b` | true or false |
| `i` | a whole number |
| `d` | a number |
| `s` | a string |
| `sl` | a list of strings |
| `c` | a color, `#AARRGGBB` |
| `br` | a fill color, `#AARRGGBB`, or `""` for none |
| `pen` | an outline: color (or `""` for none) and width |
| `len` | a length: type (1 fixed, 2 percent) and amount |
| `tabs` | tab stops: each `[position, type, delimiter]`, type 0 left, 1 right, 2 center, 3 decimal |

The property numbers are the same as the Qt toolkit's (`QTextFormat`). These
are the ones JeffPub uses:

| Number | Property | Value |
|---|---|---|
| 4112 | alignment (paragraph) | `i`: 1 left, 2 right, 4 centered, 8 justified |
| 2049 | direction (paragraph) | `i`: 0 left to right, 1 right to left |
| 4144, 4145 | space before, after (paragraph) | `d` |
| 4146, 4147 | left, right indent (paragraph) | `d` |
| 4148 | first-line indent (paragraph) | `d` |
| 4149 | tab stops (paragraph) | `tabs` |
| 4168, 4169 | line spacing amount and kind (paragraph) | `d`, `i`: 0 single, 1 proportional (percent), 2 fixed, 3 at least, 4 extra |
| 28672 | page break policy | `i` |
| 8160 | capitalization | `i`: 0 normal, 1 all caps, 3 small caps |
| 8161 | letter spacing (character) | `d` |
| 8165 | kerning on | `b` |
| 8167 | font families | `sl` |
| 8193 | font size in points | `d` |
| 8195 | weight | `i`: 400 normal, 700 bold |
| 8196 | italic | `b` |
| 8197 | underline (old form) | `b` |
| 8198 | overline | `b` |
| 8199 | strikethrough | `b` |
| 8227 | underline style | `i` |
| 8224 | underline color | `c` |
| 8225 | superscript or subscript | `i`: 1 superscript, 2 subscript |
| 8240 | is a link | `b` |
| 8241 | link address | `s` |
| 2081 | text color | `br` |
| 2080 | highlight | `br` |
| 8226 | text outline (JeffPub sets it while laying out, from properties 1048585-1048586; a file may also hold it) | `pen` |
| 12288 | list style (list formats) | `i`: -1 disc, -2 circle, -3 square, -4 decimal, -5 a, -6 A, -7 i, -8 I |
| 12289 | list indent (list formats) | `i` |
| 12292 | first number (list formats) | `i` |

Properties from 1048577 (0x100001) up are JeffPub's own:

| Number | Property | Value |
|---|---|---|
| 1048577 | text color as a [color string](#colors) (follows the scheme) | `s` |
| 1048578 | highlight as a color string | `s` |
| 1048579 | font follows the font scheme: `major` (headings) or `minor` (body) | `s` |
| 1048580 | a field (see below); its run is U+FFFC | `s` |
| 1048581 | character style name | `s` |
| 1048582, 1048583, 1048584 | shadow, emboss, engrave | `b` |
| 1048585, 1048586 | text outline color (color string) and width | `s`, `d` |
| 1048587 | language, as a BCP 47 tag (`en-US`, `de-DE`) | `s` |
| 1048588 | number style: 0 default, 1 lining, 2 old-style | `i` |
| 1048589 | number spacing: 0 default, 1 proportional, 2 tabular | `i` |
| 1048590 | ligatures: 0 standard, 1 none, 2 all | `i` |
| 1048591 | stylistic set, 0-20 | `i` |
| 1048592, 1048593, 1048594 | swash, alternates, true small caps | `b` |
| 1048595 | a gradient or picture fill for the letters (a [fill](#fills) as JSON text) | `s` |
| 1048596 | glow color (color string) | `s` |
| 1048597 | don't check spelling | `b` |
| 1048598 | kerning from this size up | `d` |
| 1048599 | tracking, percent of normal spacing | `d` |
| 1048676 | paragraph style name | `s` |
| 1048677, 1048678 | drop cap: lines tall, letters | `i` |
| 1048679, 1048680 | drop cap font and color (color string) | `s` |
| 1048681 | drop cap raised instead of dropped | `b` |
| 1048682, 1048683, 1048684 | keep with next, keep lines together, widow and orphan control | `b` |
| 1048685 | start in the next text box | `b` |
| 1048686 | align to the baseline guides | `b` |
| 1048687 | distributed alignment | `b` |
| 1048688, 1048689, 1048690 | bullet character, its font and its color (color string) | `s` |
| 1048691 | list level | `i` |
| 1048692 | numbering: 0 bullet, 1 "1.", 2 "a.", 3 "A.", 4 "i.", 5 "I.", 6 "1)", 7 "(1)" | `i` |
| 1048693 | first number | `i` |
| 1048694 | list id: paragraphs with the same id share numbering | `s` |
| 1048695 | tab leader characters, one per tab stop | `s` |
| 1048696 | the list marker's own size (list formats) | `d` |
| 1048697 | a table of contents paragraph: 0 its title, 1-3 an entry's level | `i` |

A writer may store any other `QTextFormat` property the same way. A reader
may ignore properties it doesn't support.

### Fields

A field is a single U+FFFC character whose formatting holds a field code
(property 1048580). The program replaces it with what the code names:

| Code | Shows |
|---|---|
| `page`, `page:roman`, `page:ROMAN`, `page:alpha` | The page's number |
| `pages` | The number of pages |
| `date`, `time`, `datetime` (with an optional `:format`) | Today's date or time, in a Qt date format such as `MMMM d, yyyy` |
| `biz:<key>` | A business information field (`biz:address:oneline` puts a multi-line value on one line) |
| `merge:<field>` | A mail merge field |
| `mergeblock:address`, `mergeblock:greeting` | An address block or greeting line |
| `footnote:<story id>`, `endnote:<story id>` | A note's number; the note's text is the story named |

## Styles

Each style has `name`, `charOnly` (a character style, applying only to
letters), `basedOn` (the style it inherits from), `next` (the style for the
paragraph after it), and its formatting: `chr` for characters and `blk` for
the paragraph, as [formatting objects](#formatting).

## Pictures

Each entry in `images` has `id`, `format` (the file's type: `png`, `jpg`,
`gif`, `svg`, `webp`, `bmp`, `tif`, `wmf`, `emf`), `w` and `h` (its size in
pixels), `source` (the file it was inserted from) and `linked` (it is shown
from that file instead of the copy). The picture itself is the ZIP entry
`images/<id>.<format>`. Pictures no object uses are not saved.

## Business information

Each set has `setName`, the fields `name`, `tagline`, `person`, `title`,
`address`, `phone`, `fax`, `email` and `web`, and optionally `logo` (a
picture's id).

## Mail merge

`path` (the data file it came from), `fields` (column names), `rows` (each
an array of values, in field order), `include` (whether each row is used)
and `pictureField` (the column naming each record's picture, for catalogs).

A catalog's `catalog` object has `page` (the id of the page holding the
merge area), `x`, `y`, `w`, `h` (the area) and `rows`, `cols` (records
across and down).

## Properties

`title`, `subject`, `author`, `manager`, `company`, `category`, `keywords`,
`comments`, and `created` and `modified` as ISO 8601 dates.

## Printing colors

`model`: 0 RGB, 1 a single spot color, 2 spot colors, 3 process (CMYK), 4
process plus spot colors. `spots`: each spot ink's `name` and `color`.
`embedFonts`: embed fonts in PDFs and `.pub` files.

## An example

The smallest useful publication: one Letter page with one text box.

```json
{
  "format": "jeffpub", "version": 1,
  "setup": {"size": [612, 792], "margins": [36, 36, 36, 36], "sizeName": "Letter", "layout": 0},
  "masters": [{"id": "m1", "name": "Master Page A", "abbr": "A", "twoPage": false, "items": []}],
  "pages": [{"id": "p1", "master": "m1", "items": [
    {"type": "text", "id": "t1", "rect": [72, 72, 468, 100], "insets": [7.2, 3.6, 7.2, 3.6],
     "wrap": {"mode": 1}, "story": "s1"}
  ]}],
  "stories": [{"id": "s1", "blocks": [
    {"r": [{"t": "Hello, "}, {"t": "world", "f": {"8195": ["i", 700]}}]}
  ]}],
  "colorScheme": {"name": "Classic", "colors": ["#000000", "#1F4E79", "#C55A11", "#548235", "#7F6000", "#7030A0", "#0563C1", "#954F72"]},
  "fontScheme": {"name": "Plain", "heading": "Carlito", "body": "Carlito"}
}
```
