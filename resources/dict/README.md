# Bundled dictionaries

JeffPub checks spelling and hyphenates with these dictionaries from the [LibreOffice dictionaries project](https://github.com/LibreOffice/dictionaries), one folder per language as LibreOffice lays them out. The files are unchanged. Each remains under its own license, recorded in the README and license files beside it. None is covered by JeffPub's GPL.

| Folder | Contents | License |
|---|---|---|
| `en` | English spelling for the United States, Canada, and Australia (`en_US`, `en_CA`, `en_AU`, from [SCOWL](http://wordlist.aspell.net/)) | SCOWL license, permissive (`README_en_US.txt`, `README_en_CA.txt`, `README_en_AU.txt`) |
| `en` | English spelling for the United Kingdom (`en_GB`, Marco A. G. Pinto and others) | LGPL (`README_en_GB.txt`) |
| `en` | American and British hyphenation (`hyph_en_US.dic`, `hyph_en_GB.dic`) | BSD-style (`README_hyph_en_US.txt`, `README_hyph_en_GB.txt`) |
| `en` | English thesaurus (`th_en_US_v2.dat`, from [WordNet](https://wordnet.princeton.edu/)) | WordNet license (`WordNet_license.txt`) |
| `es` | Spanish spelling for Spain and Mexico (`es_ES`, `es_MX`, by Santiago Bosio and others) and hyphenation (`hyph_es.dic`) | GPL-3.0+, LGPL-3.0+, or MPL-1.1+, your choice (`LICENSE.md`, `README_hunspell_es.txt`, `README_hyph_es.txt`) |
| `fr_FR` | French spelling (`fr`, from [Grammalecte](https://grammalecte.net/)) | MPL-2.0 (`README_dict_fr.txt`) |
| `fr_FR` | French hyphenation (`hyph_fr.dic`) | LGPL (`README_hyph_fr.txt`) |
| `de` | German spelling (`de_DE_frami`, from [igerman98](https://www.j3e.de/ispell/igerman98/)) | GPL-2.0 or GPL-3.0 (`README_de_DE_frami.txt`, `COPYING_GPLv2`, `COPYING_GPLv3`) |
| `de` | German hyphenation (`hyph_de_DE.dic`) | LGPL-2.0+ (`README_hyph_de.txt`, `COPYING_LGPL_v2.0.txt`) |
| `it_IT` | Italian spelling (`it_IT`) | GPL-3.0 (`README_it_IT.txt`) |
| `it_IT` | Italian hyphenation (`hyph_it_IT.dic`) | LGPL (`README_hyph_it_IT.txt`) |
| `nl_NL` | Dutch spelling and hyphenation (from [OpenTaal](https://www.opentaal.org/)) | BSD (revised) or CC BY 3.0 (`LICENSE.txt`, `README.md`) |
| `pt_BR` | Brazilian Portuguese spelling and hyphenation (from [VERO](http://pt-br.libreoffice.org/projetos/projeto-vero-verificador-ortografico/)) | LGPL-3.0 or MPL (`README_pt_BR.txt`, `README_hyph_pt_BR.txt`, `README_en.txt`) |
| `pt_PT` | European Portuguese spelling and hyphenation | GPL-2.0+ (`LICENSES.txt`, `README_pt_PT.txt`, `README_hyph_pt_PT.txt`) |

Which dictionary checks a piece of text follows the language set on it (Review > Language). Text in a language without a dictionary here isn't checked or hyphenated. The table of languages and files is in `src/text/dictionaries.cpp`.
