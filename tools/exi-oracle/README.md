# The EXI oracle

EXIficient 1.0.4 (https://github.com/EXIficient/exificient), the EXI
implementation of the W3C EXI working group's editors, used to make the golden
streams of `tests/exi/` (the `exi_oracle`; D333). A development tool only:
nothing sxml builds, ships or tests at run time needs Java.

- `oracle.sh batch MANIFEST OUT`: builds `Oracle.java` on first use (Maven
  fetches EXIficient into `~/.m2`; the classes go to `build/`, which git
  ignores) and runs it. For each line of MANIFEST (`id`, input file, option
  specification; tests/exi/README.md, "Options") it writes `OUT/<id>.exi`
  and `OUT/<id>.xml` (that stream decoded again) and a line of
  `OUT/results.txt`.
- `make-golden.py [--sxml=PATH]`: selects the corpus, normalizes it with the
  `sxml` command, runs the oracle over every option set and writes
  `tests/exi/inputs/`, `tests/exi/golden/` and `tests/exi/manifest.tsv`.

Two behaviours of EXIficient that the corpus works around, so that its
streams and sxml's can be compared byte for byte:

- its SAX encoder drops whitespace-only text; the inputs are normalized
  without it (outside `xml:space="preserve"`);
- it drops `xsi:schemaLocation` attributes unless told otherwise; the oracle
  sets `INCLUDE_XSI_SCHEMALOCATION`, since sxml keeps every attribute.

Needs Java 17 or later and Maven.
