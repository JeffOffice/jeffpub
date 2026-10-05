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
ab = height, b7 = 0; a text box adds 27 = its text id.

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
| MCLD | Text frame layout per story |
| FONT | Font table |
| SGP, INK, PL | Small fixed sections; PL is the text color list |
| TCD | Table cell text ends (tables only) |

A blank file has no TEXT, FDPC, FDPP, BTEC, BTEP, STRS or MCLD sections.
