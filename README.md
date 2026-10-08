# JeffPub

> **JeffPub is an independent project. It is not affiliated with, endorsed by, sponsored by, or supported by Microsoft Corporation.** See [Legal notices](#legal-notices).

JeffPub is a free, open-source desktop publishing app for **Windows, macOS and Linux**. Its features are modeled on those of Microsoft Publisher 2021, and it opens Publisher files. Use it to lay out flyers, newsletters, brochures, business cards, certificates, and other print pieces on a page. Text, pictures, shapes, and tables sit in frames that you can move and resize wherever you like.

## Why JeffPub exists

Microsoft is retiring Publisher. Microsoft 365 subscribers lost access to Publisher on October 1, 2026, and support for the standalone Publisher 2021 ends on October 13, 2026 ([Microsoft's announcement](https://support.microsoft.com/en-us/publisher/microsoft-publisher-will-no-longer-be-supported-after-october-2026)). Microsoft suggests converting existing publications to PDF or Word before then.

JeffPub was started in response. It gives the people, small businesses, schools and publishers who built years of work in Publisher a free, open-source program that keeps opening, editing and printing those `.pub` files, on Windows and Linux, without a subscription and without depending on any one company's product plans.

> **Status:** beta (0.5): the features are in place and are being polished. Downloads are on the [Releases page](https://github.com/JeffOffice/jeffpub/releases): a Windows installer, a Mac disk image (not yet signed, so macOS asks before opening it the first time), and for Linux a `.deb` package (Debian, Ubuntu and their relatives) and an AppImage that runs on most other distributions. JeffPub opens and saves `.pub` files from Publisher 98 to 2021.
>
> On Debian or Ubuntu, install the package with `sudo apt install ./jeffpub_<version>_amd64.deb`. For the AppImage, make the file executable and run it.

## What it will do

- **Page layout.** Free-form frames on a page: text boxes, pictures, shapes, tables, and Text Art headings. You can move, resize, rotate, group, align, and layer them, and they snap to margins and guides.
- **Linked text boxes.** A long story flows from one text box to the next, across columns and pages, the way newsletters need.
- **Master pages** for headers, footers, and page numbers that repeat on every page.
- **Color and font schemes.** Change one scheme and the whole publication recolors or changes fonts.
- **Templates and building blocks**, including flyers, newsletters, brochures, cards, calendars, and certificates.
- **Mail merge** from a CSV list.
- **Design Checker**, which finds text that doesn't fit its box, objects that run off the page, and low-resolution pictures.
- **Print and export** to PDF and PNG.
- Rulers, guides, a pages pane, zoom, and a two-page spread view.
- **Microsoft Publisher files.** Opens `.pub` files from Publisher 98 through Publisher 2021, and saves publications back to `.pub` so they can be shared with Publisher users. JeffPub also has its own open format, `.jpub`.

## Privacy

JeffPub checks GitHub for new versions when it starts (you can turn that off in File > Options).

Unless you turn them off, it also sends anonymous usage statistics once a day while it's in use (and once more the day it's updated): a random number made up when it was installed (it says nothing about you or your computer), its version, your operating system and its version, the language it's set to, how many times it was started, and how many times each of its commands was used. It never sends your files, their names, or anything in them, and the server doesn't keep IP addresses. You choose in Windows setup or the first time JeffPub starts, and can change it anytime in File > Options. The collector's code is in [server/telemetry](server/telemetry).

## Built with

- **C++17** and **Qt 6** (Widgets and Graphics View), built with **CMake**
- Qt provides the cross-platform UI, the rich-text layout engine, PDF output, and native printing on Windows, macOS and Linux.

## License

Copyright © 2026 JeffOffice LLC.

JeffPub is free software under the [GNU General Public License v3.0](LICENSE). You may use, study, change, and share it. If you distribute a modified version, you must share its source under the same license.

## Legal notices

**No affiliation with Microsoft.** JeffPub is an independent, community-made program. It is not a Microsoft product. It is not affiliated with, authorized, sponsored, endorsed, licensed, or supported by Microsoft Corporation or any of its subsidiaries. Microsoft has not reviewed or approved this project. Do not contact Microsoft for help with JeffPub, and do not contact this project for help with Microsoft products.

**Trademarks.** Microsoft, Microsoft Publisher, Microsoft 365, Office, Windows, Arial, Times New Roman, Calibri, Georgia and the other Microsoft product and font names mentioned in this project are trademarks or registered trademarks of Microsoft Corporation or of their respective owners, in the United States and other countries. Qt is a trademark of The Qt Company Ltd. Linux is a registered trademark of Linus Torvalds. All other trademarks belong to their owners. This project uses these names only to describe compatibility, for example that JeffPub can open files created by Microsoft Publisher. That use does not imply any relationship with, or endorsement by, the trademark owners. "JeffPub" is the name of this project and is not a Microsoft name or product.

**No Microsoft code or artwork.** JeffPub contains no source code, program files, icons, logos, templates, clip art, or fonts from Microsoft. Its interface uses its own name, design and the open-source Lucide icon set. Its templates and sample text are original; the sample business details in them are fictitious. Where a publication asks for a Microsoft font that isn't installed, JeffPub substitutes a freely licensed font with similar metrics; it does not ship Microsoft fonts.

**File compatibility.** Support for reading `.pub` files is built on the open-source [libmspub](https://wiki.documentfoundation.org/DLP/Libraries/libmspub) and [librevenge](https://sourceforge.net/p/libwpd/librevenge/) libraries, extended through independent analysis of publications the project's contributors created or own. This interoperability work is not based on any confidential Microsoft information. Converting files between programs can change how they look. Always check converted files, and keep your originals.

**No warranty.** JeffPub is provided "as is", without warranty of any kind, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose and non-infringement. The authors and contributors are not liable for any claim, damages, data loss or other liability arising from the use of this software. See sections 15 and 16 of the [GPL](LICENSE) for the full terms.

**Your content.** You are responsible for the text, pictures and other material you put into your publications, and for having the rights to use them. Links to online picture libraries are provided for convenience; check each picture's license before you use it.

## Third-party software

JeffPub includes or downloads these components, each under its own license:

| Component | Use | License |
|---|---|---|
| [Qt 6](https://www.qt.io/) | User interface, text layout, PDF and printing | LGPL-3.0 |
| [libmspub](https://wiki.documentfoundation.org/DLP/Libraries/libmspub) (modified) | Reading `.pub` files | MPL-2.0 |
| [librevenge](https://sourceforge.net/p/libwpd/librevenge/) (modified) | Document interfaces and OLE2 streams | MPL-2.0 or LGPL-2.1+ |
| [Hunspell](https://hunspell.github.io/) | Spelling checker | MPL-1.1, GPL-2.0+ or LGPL-2.1+ |
| English dictionaries: US, Canada and Australia ([SCOWL](http://wordlist.aspell.net/)), United Kingdom | Spelling | SCOWL license; LGPL (United Kingdom) |
| English hyphenation patterns (American and British) | Hyphenation | BSD-style |
| [WordNet](https://wordnet.princeton.edu/) thesaurus | Synonyms | WordNet license |
| Spanish dictionaries (Spain, Mexico) and hyphenation patterns | Spelling, hyphenation | GPL-3.0+, LGPL-3.0+ or MPL-1.1+ |
| French dictionary ([Grammalecte](https://grammalecte.net/)) and hyphenation patterns | Spelling, hyphenation | MPL-2.0; patterns LGPL |
| German dictionary ([igerman98](https://www.j3e.de/ispell/igerman98/)) and hyphenation patterns | Spelling, hyphenation | GPL-2.0 or GPL-3.0; patterns LGPL-2.0+ |
| Italian dictionary and hyphenation patterns | Spelling, hyphenation | GPL-3.0; patterns LGPL |
| Dutch dictionary and hyphenation patterns ([OpenTaal](https://www.opentaal.org/)) | Spelling, hyphenation | BSD (revised) or CC BY 3.0 |
| Brazilian Portuguese dictionary and hyphenation patterns ([VERO](http://pt-br.libreoffice.org/projetos/projeto-vero-verificador-ortografico/)) | Spelling, hyphenation | LGPL-3.0 or MPL |
| European Portuguese dictionary and hyphenation patterns | Spelling, hyphenation | GPL-2.0+ |
| [Lucide](https://lucide.dev/) icons and their search keywords | Interface icons and Insert > Icons | ISC |
| Open fonts | Fonts and substitutes | SIL Open Font License, GUST Font License, Bitstream Vera, GPL v2 with font exceptions (Liberation Sans Narrow); see `resources/fonts/README.md` |
| [zlib](https://zlib.net/) | Compression (when the system has none) | zlib license |
| [PDFium](https://pdfium.googlesource.com/pdfium/), built by [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries) | Placing PDF pages | BSD-3-Clause, parts Apache-2.0; the build MIT |
| Built into PDFium: [FreeType](https://freetype.org/), [HarfBuzz](https://harfbuzz.github.io/), [ICU](https://icu.unicode.org/), [Little CMS](https://www.littlecms.com/), [libjpeg-turbo](https://libjpeg-turbo.org/), [OpenJPEG](https://www.openjpeg.org/), [libpng](http://www.libpng.org/pub/png/libpng.html), [zlib](https://zlib.net/), [Abseil](https://abseil.io/), Anti-Grain Geometry 2.3, [dragonbox](https://github.com/jk-jeon/dragonbox), [fast_float](https://github.com/fastfloat/fast_float), [simdutf](https://github.com/simdutf/simdutf), [LLVM libc](https://libc.llvm.org/) | Fonts, text, color and pictures in PDF pages | FreeType License; Old MIT; Unicode License v3; MIT; IJG and BSD-3-Clause; BSD-2-Clause; PNG Reference Library License v2; zlib; Apache-2.0; AGG 2.3 license; Apache-2.0 with LLVM Exceptions or BSL-1.0; MIT; MIT; Apache-2.0 with LLVM Exceptions |
| [MinGW-w64](https://www.mingw-w64.org/) runtime (Windows) | C++ runtime DLLs | GPL-3.0 with the [GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.html); winpthreads MIT |
| [NSIS](https://nsis.sourceforge.io/) (Windows) | Installer | zlib/libpng license |
| [ICU](https://icu.unicode.org/) (in PDFium; with Qt in the Linux packages) | Unicode text support | Unicode License v3 |
| [LibreTranslate](https://libretranslate.com/) | Translate opens it in the browser; not bundled | AGPL-3.0 |
| [PSO Coated v3](https://www.eci.org/) color profile, from the European Color Initiative | The printing condition in PDF/X-4 files; downloaded from ECI the first time you make one (after asking); not bundled | ECI's terms: free to use and embed in PDFs, not to pass on |
| [International ISBN Agency](https://www.isbn-international.org/range_file_generation) range table | Placing the hyphens in ISBNs from every country; downloaded from the agency when you make an ISBN barcode, monthly at most; not bundled | The agency's terms (it doesn't allow republishing the table) |

The dictionaries come unchanged from the [LibreOffice dictionaries project](https://github.com/LibreOffice/dictionaries); each language's authors, license and license text are in [resources/dict](resources/dict/README.md). Changes made to libmspub and librevenge are listed in [third_party/README.md](third_party/README.md). PDFium's license texts, and those of the libraries built into it, are in [third_party/pdfium/licenses](third_party/pdfium/licenses) and ship with the program (in its `licenses/pdfium` folder; on Linux in `/usr/share/doc/jeffpub/pdfium`).
