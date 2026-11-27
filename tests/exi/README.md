# EXI golden tests

The tests of sxml's EXI codec (`include/sxml/exi.h`; D333) against the
`exi_oracle`, EXIficient 1.0.4. Nothing here needs Java: the oracle's output
is checked in.

| Path | What |
| --- | --- |
| `handmade/` | Documents written for EXI's corners: namespaces, Unicode, whitespace, repeated values, deep nesting, many attributes, a large document |
| `inputs/` | Every document of the corpus, normalized (UTF-8, no DTD, no whitespace-only text outside `xml:space="preserve"`) |
| `golden/<set>/<id>.exi` | EXIficient's stream of input `<id>` with the options of `<set>` |
| `golden/decoded/<hash>.xml` | EXIficient's decoding of those streams, stored once per content |
| `manifest.tsv` | One case per line: set, id, input, options, the oracle's outcome, the decoding |
| `passing.txt` | The `exi_ratchet`: the checks that pass and must keep passing |
| `exi_golden.c` | The driver, ctest `exi/golden` and `exi/golden-self-check` |

`tools/exi-oracle/make-golden.py` writes `inputs/`, `golden/` and
`manifest.tsv`; it is deterministic, so running it again changes nothing
unless the vendored suites, the handmade documents or the option sets
changed.

## The checks

Each case is two checks:

- `<set>/<id>/decode`: sxml decodes the golden stream, with the case's
  options known out of band, and the result is `fn:deep-equal` to
  EXIficient's decoding (with comments, processing instructions and prefixes
  compared when the options preserve them).
- `<set>/<id>/encode`: sxml encodes the input with the case's options, and
  the stream is the golden one byte for byte. With the compression
  alignment (not canonical), whose DEFLATE output may differ between
  implementations, sxml's stream is decoded again and compared as in
  decode. A case the oracle refused (content strict grammars cannot
  represent) must be refused with `SXML_STATUS_DYNAMIC_ERROR`.

`exi_golden ROOT --verbose --filter=TEXT` shows why checks fail;
`exi_golden ROOT --update` adds the checks that now pass to `passing.txt`
(it never removes one, and refuses while a listed check fails).
`--self-check` tests the comparison itself, with no EXI code.

## Options

An option specification is `key[=value]` pairs separated by `;`, read by both
the oracle and the driver:

| Key | Value | EXI option |
| --- | --- | --- |
| `alignment` | `bit-packed`, `byte-alignment`, `pre-compression`, `compression` | alignment |
| `preserve` | a list of `comments`, `pis`, `dtd`, `prefixes`, `lexical-values` | the fidelity options |
| `strict` | | strict |
| `fragment` | | fragment |
| `self-contained` | a list of `{uri}local` names | selfContained, and the elements made self-contained |
| `block-size`, `value-max-length`, `value-partition-capacity` | a number | blockSize, valueMaxLength, valuePartitionCapacity |
| `schema` | a schema document's path | schema-informed grammars (schemaId, with `header=schema-id`, is the path) |
| `xsd-types-only` | | the empty schemaId: built-in XML Schema types only |
| `header` | a list of `cookie`, `options`, `schema-id` | what the header carries |
| `canonical`, `utc-time` | | Canonical EXI and its utcTime option |

## Gating a packet

`tools/exi/check-packet.sh` runs everything a Phase 8 packet must pass (see
its header and `docs/tasks/phase-8.md`).
