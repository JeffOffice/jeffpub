# The .pub file format: notes for the writer

What JeffPub 79 has learned about `.pub` files (versions 2003 and later), from
libmspub's reader and from comparing small sample files that differ in one
thing. `tools/pubdump.py FILE.pub` prints every structure described here.

Units: lengths are EMU (914400 per inch, 12700 per point) unless noted.

## Container

A compound file (`src/io/cfb.cpp`) with these streams:

| Stream | Contents |
|---|---|
| `Contents` | Document, pages, shapes, tables, palette, fonts: a block tree (below) |
| `Quill/QuillSub/CONTENTS` | All text, with character and paragraph formatting and style sheets |
| `Escher/EscherStm` | Drawing records: shape geometry, fill, line, anchors |
| `Escher/EscherDelayStm` | Picture data referenced by the drawing records (empty if none) |
| `\x01CompObj`, `\x03Internal`, `Envelope` | Fixed identification streams |
| `\x05SummaryInformation`, `\x05DocumentSummaryInformation` | Standard OLE property sets (title, author; a preview picture) |

Storage class ids: root `00021201-0000-0000-00C0-000000000046`,
`Quill/QuillSub` `08C8F6DA-969D-11D1-8E02-00C04FB6FECE`. Empty storages
`Objects` and `VBA` are present.

## Blocks (Contents stream)

A block is `id (u8)`, `type (u8)`, then data whose size the type decides:

| Type | Data |
|---|---|
| 0x00, 0x05, 0x08, 0x0A, 0x78 | none (0x08 is a true flag; 0x78 a placeholder) |
| 0x07, 0x10, 0x12, 0x18, 0x1A | 2 bytes |
| 0x20, 0x22, 0x58, 0x68, 0x70, 0xB8 | 4 bytes (0x68/0x70 hold sequence numbers) |
| 0x28 / 0x38 / 0x48 | 8 / 16 / 24 bytes |
| 0x80, 0x82, 0x88, 0x8A, 0x90, 0x98, 0xA0 | u32 length (counting itself), then nested blocks |
| 0xC0 | u32 length, then a NUL-terminated UTF-16 string |

0x88 is a record of fields; 0xA0, 0x90 and 0x98 are lists whose items are
blocks with id 0.

### Header and chunk directory

At 0x1A, a u32 is the offset of the trailer. The trailer is a u32 length and
three blocks; the one of type 0x90 is the chunk directory. Each item in the
directory has a sequence number in order, the document being 256 (items
before it are 0x78 placeholders). A chunk reference is an 0x88 record:
02 = chunk type, 04 = offset in the stream, 05 = parent sequence number. A
chunk is a u32 length followed by blocks.

The header region before the first chunk also holds a file-information
record: magic `e8ac`, the stream length, the trailer offset, and the path
the file was last saved to. Not fully decoded yet.

### Chunks in a blank one-page Letter publication

| Seq | Type | What |
|---|---|---|
| 256 | 0x44 document | 01 page count?, 02 list of page seqs, 12 page size (01 width, 02 height), links to the chunks below |
| 263 | 0x43 page | master page "A" ("Master Page A") |
| 266 | 0x43 page | the page: 01 shape count, 02 list of shape seqs, 0d its master (263) |
| 269, 272, 275, 279 | 0x43 page | special pages (different sizes; not shown as pages) |
| 264… | 0x60, 0x77 | per-page settings; 0x77 holds web-form defaults |
| 282 | 0x5B | text index: 01 → 0x61, 02 → 0x65, 04 → font chunk |
| 283 | 0x61 | text id → shape: records {01 text id, 03 shape seq} |
| 284 | 0x65 | stories: records {01 text id, 07, 08, 09} |
| 285 | 0x5C palette | 8 colors, scheme name |
| 287, 288 | 0x4B, 0x4F | print settings (paper size, printer name) |
| 289 | 0x4C | layout guides and margins |
| 291 | 0x4A | bullet symbols (Symbol font) |
| 292 | 0x8A | page setup: paper name ("Letter"), sizes, margins |
| 293… | 0x01 shape, 0x10 table, 0x63 cells | objects on pages |

A shape chunk (rectangle): 04 = 259, 0c/0d = 8 zero bytes, aa = width,
ab = height, b7 = 0; a text box (or a shape holding text) adds 27 = its
text id, and 35 = vertical alignment (1 middle, 2 bottom; left out for
top). Publisher doesn't use the drawing property 0x0087 for this.

## Drawing records (EscherStm)

Standard Escher-format drawing records: one `DggContainer`, then a `DgContainer` per
page (Dg instance = drawing id; data = shape count and last shape id). Each
shape is an `SpContainer` with `Sp` (instance = shape type: 1 rectangle,
202 text box; data = shape id, flags 0x0A00), `OPT` and `TertiaryOPT`
property tables, a `ClientAnchor` (blocks 01–04 = left, top, right, bottom,
relative to the page center), and `ClientData` (block 01 type 0x68 = the
shape's sequence number in Contents). Text boxes add `0x0080` = text id in
OPT and an empty ClientTextbox (0xF00D).

Table cell formats are shapes with a ClientAnchor naming the table (see
`third_party/libmspub/src/TableInfo.h`).

Turned shapes put the angle in OPT 0x0004 (clockwise degrees, 16.16). The
anchor is the unturned frame, except that between 45° and 135° (and 225°
and 315°) it holds the frame turned a quarter about its center; the
Contents size (aa, ab) matches the anchor. Sp flags 0x40 and 0x80 flip.
Publisher flips first, then turns; a shape flipped one way (not both) is
turned counterclockwise by the stored angle, so JeffPub (which always turns
clockwise) stores the negative angle for it. Checked in Publisher.

### Lines

A line is an `Sp` of type 32 (straight connector), flags 0x0B00 plus the
flips that say which corner it starts from; the anchor is the box it spans.
OPT holds the line props (0x01C0 color, 0x01CB width, 0x01FF 0x00080008,
0x01CE dashes, 0x01D0/0x01D1 start and end arrowheads with sizes in
0x01D2-0x01D5) and 0x0303 = 0. Its Contents chunk is type 0x20:
`02:08, 03:08, 04:10 = 256, 0c, 0d, b7 = 0`, directory version 0x0102.

### Pictures

Each image is one `BSE` in a `BStoreContainer` (after `Dgg`), instance =
type (5 JPEG, 6 PNG): two type bytes, a 16-byte id (MD4 of the data),
tag 0x00FF, record size, use count, offset into `EscherDelayStm`, and four
zero bytes. The delay stream holds the image records (0xF01E PNG with
instance 0x6E0, 0xF01D JPEG with 0x46A, or 0x6E2 for CMYK): the id, a 0xFF
byte, then the file. The shape is an `Sp` of type 1 whose OPT has 0x007F =
0x00800080, 0x4104 = the store entry (from 1), 0xC105 = the name without
extension (UTF-16, complex), 0x0106 = 1, 0x033F = 0x00100010, and crops in
0x0100-0x0103 (16.16 fractions). The Contents chunk (type 1, directory
version 0x0102, 0b = 1) is `02:08, 03:08, 0c, 0d, 34 = 0, 3a → name chunk,
aa, ab`; the name chunk (type 0x66, parent the picture) holds `03` = the
file name.

## Text (Quill/QuillSub/CONTENTS)

Starts with a `CHNKINK` index of sections, each with a 4-letter name, an id,
an offset and a length:

| Section | What |
|---|---|
| TEXT | All stories' text, UTF-16, paragraphs end with `\r` |
| STSH ×3 | Style sheets (the second holds the styles; ~16 KB in a blank file) |
| FDPC / FDPP | Character / paragraph formatting runs (512-byte pages) |
| SYID | Text ids of the stories, in order |
| BTEC / BTEP | Indexes into FDPC / FDPP |
| STRS | Story lengths |
| MCLD | Text frame layout per story (below); required once a table is present |
| FONT | Font table |
| SGP, INK, PL | Small fixed sections; PL is the text color list (below) |
| TCD | Table cell text ends (tables only) |

A blank file has no TEXT, FDPC, FDPP, BTEC, BTEP, STRS or MCLD sections.

### Text color

A run's color is two records that name the same entry in the PL list:
`44:8a {00:22 index}` (the color) and `58:8a {00:12 1, 01:22 index,
02:22 100000}` (the text fill: solid, that color, 100% opaque). Publisher
draws the fill, so a run with only 0x44 keeps the Normal style's fill
(index 0) and shows in the main color.

PL is a u32 count, 0x28, 0, then length-prefixed entries. Entry 0 is the
scheme's main color (`01:22 0x08000000`, 02/04/05 = 0xFFFFFFFF, 03 =
0x20000000); entry 1 is black (`01:22 0`). A custom color is
`01:22 BGR, 02:22 0xFFFFFFFF, 03:22 0x20000000`; a scheme color is
`01:22 BGR, 02:22 0x08000000 + slot` plus an 03 value not yet decoded.
Every entry ends with `06:82` holding two zero bytes. Files that use only
scheme colors may have no PL section at all.

## Tables

A table is a Contents chunk of type 0x10 (directory version 0x0102, 0b =
0x16): `02:08, 04:10 = 259, 0c, 0d, 27 = text id, 2a:08, 66 = rows, 67 =
columns, 68 = width, 69 = height, 6b → cell list, 6d = edges, 70 =
0xFFFFFFFD, b7 = 0`. The edges list (0x90) holds a record per column, then
per row: `01` = where it ends, `02` = its size.

The cell list (type 0x63, parent the table, directory 08 and 0b = 1) is
`01:18 count, 02 list`; each cell has `01` first row, `02` last row, `03`
first column, `04` last column (zero fields left out), `0a`-`0d` margins
(left, right, top, bottom) and `0e = 114300`. A merged cell is one entry
spanning its rows and columns; the cells under it are not listed. Cells
can come in any order; their text follows the list order.

All the cells' text is one story. Its `0x65` record adds `03:10 = 0`, and
it has no `0x61` entry. A `TCD ` section (kind `PLC `, id = the story's
index in SYID) holds: cells - 1, 0, 0xFF00, then each cell's end within
the story (the position of its last paragraph mark; the last value is the
story length).

The drawing has an `Sp` of type 201 with OPT 0x0080 = text id, insets 0,
0x017F = 0x00300000, and a ClientTextbox. Each cell fill and each ruled
line is a shape of type 1 in the table-format drawing (drawing 3) whose
ClientAnchor names the table (`02:68`):

- Fill: `03` column, `04` row; OPT 0x0181 = color, 0x01BF = 0x001F001C.
- Line: `01` = 1 across or 2 down, then the grid box it runs along as grid
  lines: `04` first row, `05` first column, `06` last row, `07` last
  column. OPT 0x0181 = color, 0x01CB = width, 0x01FF = 0x001F0006.

### Frame layout (MCLD)

Publisher won't open a file with a table unless this section is present.
It starts with the last entry number, the entry count and the entry
numbers (1, 2, ...); each story's `0x65` record names its entry in field
`07`. Then, per story in order: a length-prefixed `{00:0a, 01:22 =
228600}`, a u32 frame count, and a length-prefixed record per frame (a
text box has one; a table has one per cell, in cell-list order):

| Field | Value |
|---|---|
| 00, 01, 02, 03 | Top, left, bottom, right in layout units: 147 per inch, absolute, with the page center at 110185200 EMU. Starts round up, ends round down. |
| 04, 05 | Width, height (EMU) |
| 06, 07, 08, 09 | Margins: top, left, bottom, right |
| 0a | 82676 for a text box, 0 for a cell |
| 12 | 0 for a text box, 219456000 for a cell |
| 16 | 9525 for a text box, 0 for a cell |
| 0b:1a 0, 0d 0, 11 0, 13:12 255, 14:0a, 15 1, 18 0, 1a:02, 1d:8a {00:22 = -4} | Fixed |

### Linked text boxes

A chain of linked boxes is one story. Every box has its own shape chunk
and drawing shape, all naming the same text id (`27`, OPT 0x0080 and the
ClientTextbox). The `0x61` map has a record per box in chain order, with
`02` = its place in the chain (left out for the first); the story's `0x65`
record has `02:18` = the number of boxes; its MCLD entry lists a frame per
box in chain order. The boxes can be on different pages. Each box's shape
chunk also links its neighbors: `28` = its place in the chain, `36:68` =
the box before, `37:68` = the box after, and `2d:08` on the last box.
Without these links Publisher hangs while laying out the text.

### Preset shapes and freeforms

Most shapes use the shape chunk of type 1 (as rectangles do); the drawing
`Sp` instance is the shape number (2 rounded rectangle, 5 triangle, 12
star, ...), with handle settings in OPT 0x0147 onward. JeffPub writes a
preset as Publisher's own shape only where the two look the same at every
proportion (`src/io/pubshapes.cpp`; `jpubtool shapecheck` measures it);
anything else is a freeform: `Sp` instance 0 with OPT 0x0142/0x0143 = the
coordinate space (the frame in EMU), 0x0144 = 4, 0xC145 = the points
(count, count, 8, then 32-bit x/y pairs) and 0xC146 = the segments (count,
count, 2, then: 0x4000 move, n lines, 0x2000 + n curves, 0x6001 close,
0x8000 end). The .pub reader takes a segment's count from its low byte, so
runs stay under 256. A freeform whose outline reaches past its frame gets
a frame grown evenly around the same center. Parts within one path (up to
a 0x8000) fill alternately, so holes cut through; a shape whose parts
merge on screen is written as its merged outline plus, when it has an
outline, each part twice more (an even number of layers leaves the fill
alone but draws the inner lines, like a smiley's eyes).

### Text Art

A Text Art shape uses the same chunk type as lines (0x20: `02, 03, 04:10 = 259,
0c, 0d, b7 = 0`). Its `Sp` instance is the warp (136 plain, 144 arch up,
146 circle, 156 wave, 160 inflate, 172 slant up, ... up to 175), and OPT
holds `0xC0C0` = the words and `0xC0C5` = the font (UTF-16 with a closing
zero), `0x00C2` alignment (0 stretch, 1 center (default), 2 left, 3 right,
4 letter justify, 5 word justify), `0x00C3` size and `0x00C4` spacing
(16.16; defaults 36 pt and 1.0), and `0x00FF` settings: the high half marks
which are set, the low half has bit 4 italic, 5 bold, 7 even height, 8 best
fit, 10 stretch, 12 kern, 13 vertical, 14 Text Art.

### Paragraph and character settings

Seen in Publisher's files and written by JeffPub (paragraph runs, FDPP):
`0c:22` first-line indent (EMU, negative for a hanging indent), `0d:22`
left and `0e:22` right indent, `12:22` space before, `13:22` space after,
`34:22` line spacing: in points as eighths of an EMU plus 1, or in lines as
what would be EMU at 96 pt (a multiple of 4) plus 2 (1 line = 1219202;
Normal's 1.19 lines = 1450850). JeffPub writes spacing and line spacing on
every paragraph so Normal's own values (6 pt after, 1.19 lines) don't apply.

Character runs (FDPC): `1e:12 = 1` underline, `10:0a` strikethrough,
`13:0a` small caps, `14:0a` all caps, `0f:12` 1 superscript, 2 subscript,
`1b:22` space added between letters in EMU (Publisher's "kerning"), `1f:1a`
tracking in tenths of a percent (1000 normal, 1250 very loose).

Tab stops (FDPP `32:82`): `27:1a` count, then `28:8a` with a record per
stop: `00:20` position (EMU), `01:10` alignment in the low byte (left left
out, 1 right, 2 center, 3 decimal), `02:18 = 46`. Where the leader is kept
is not known yet.

Lists (FDPP): `57:8a {00:22 kind, 01:22 bullet character, 02:22 0}` where
kind 23 is a bulleted list (0xB7 = the Symbol font's round bullet) and
otherwise the numbering style (0 1 2 3, 1 I II, 2 i ii, 3 A B, 4 a b);
numbered lists add `58:22` with the punctuation in the high half (2 "1.",
0 "1)", 1 "(1)"). Publisher also writes `02:22` (the text size), `03:1a =
31`, and a ¼" hanging indent (`0c` -228600, `0d` 228600).

