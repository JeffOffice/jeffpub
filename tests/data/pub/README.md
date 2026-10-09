# .pub test files

The `.pub` files here exercise JeffPub's `.pub` reader. Every one is either
a public test file from another open-source project, unchanged, or a file
JeffPub made itself. None is a user's document or one of Publisher's own
designs.

Each copied file was checked on October 8, 2026 to be byte-for-byte
identical to the file in its project's public repository (same Git blob
hash).

| Files | Source | License |
|---|---|---|
| `poi-*.pub` (21 files), `poi-*.txt` (4), and `fuzz/poi-clusterfuzz-testcase-minimized-*.pub` (3 files) | Apache POI's test data, [`test-data/publisher`](https://github.com/apache/poi/tree/trunk/test-data/publisher) (the `poi-` prefix is added here) | Apache License 2.0 |
| `tika-testPUBLISHER.pub` | Apache Tika's test documents, [`testPUBLISHER.pub`](https://github.com/apache/tika/blob/main/tika-parsers/tika-parsers-standard/tika-parsers-standard-modules/tika-parser-microsoft-module/src/test/resources/test-documents/testPUBLISHER.pub) | Apache License 2.0 |
| `lo-fdo59355-1.pub` | LibreOffice's libmspub tests, [`writerperfect/qa/unit/data/draw/libmspub/pass/fdo59355-1.pub`](https://github.com/LibreOffice/core/blob/master/writerperfect/qa/unit/data/draw/libmspub/pass/fdo59355-1.pub) | Mozilla Public License 2.0 |
| `jp-damaged-style-names.pub` | Made by JeffPub from its own styles sample, then damaged on purpose (style names pointing outside their section) | GPL-3.0, as the rest of JeffPub |

Files for new tests are made in a temporary folder by the test itself
(saved with JeffPub's `.pub` writer), not added here.
