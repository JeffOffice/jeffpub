# Translations

JeffPub's own text is written in American English. A translation is a
`jeffpub_<language>.ts` file here (for example `jeffpub_de.ts` or
`jeffpub_pt_BR.ts`), made and edited with Qt Linguist; the build turns each
into a `.qm` file inside the program, and JeffPub uses the one for the
system's language, or the one chosen in File > Options > General > Display
language.

- `jeffpub.ts` lists every string marked for translation, with no language.
  Update it after changing the program's text:
  `cmake --build build --target jeffpub_lupdate`.
- To start a language, copy `jeffpub.ts` to `jeffpub_<language>.ts`, set its
  language in Qt Linguist (Edit > Translation File Settings), translate,
  and rebuild.
- The ribbon's labels come from `resources/ribbon.json`; they are listed for
  translators in `src/app/ribbon_strings.cpp`, which a test keeps in step
  with the file.
