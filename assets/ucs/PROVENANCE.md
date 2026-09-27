# UCS list provenance

- File: `ucs_v8.2.1.csv` — the Universal Category System (UCS) v8.2.1, English list.
- Authors: Tim Nielsen, Justin Drury, Kai Paquin and the UCS community,
  https://universalcategorysystem.com/ (the list is released as **public domain**; the site
  states "public domain" and the list is distributed for free use by any tool).
- Obtained 2026-09-27 as the machine-readable CSV bundled by the MIT-licensed
  `jmrsound/ucs-tools` project (`src/ucs_tools/data/ucs_v8.2.1.csv`), which states the same
  public-domain status for the data. Columns kept verbatim:
  `Category, SubCategory, CatID, CatShort, Explanations, Synonyms`.
- SHA-256: `aebc8bf4f8b0dd7cafc1231c25b6664250087ab1acb8d4e6ed18ef8bf9d47986`
- 753 CatIDs in 82 categories.
- `core/src/ucs_data.cpp` is generated from this file by `tools/gen_ucs_table.py` and
  committed; updating UCS = replace the CSV, rerun the generator, update this note.
