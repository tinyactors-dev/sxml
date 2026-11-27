<!-- Generated from spec/articles by tools/spec/spec generate. Do not edit: change it with tools/spec/spec (spec/README.md). -->

# Phase 8: EXI

Goal: all of Efficient XML Interchange 1.0 (Second Edition, W3C Recommendation 2014)
and Canonical EXI (W3C Recommendation 2018) in sxml, read and written through
the tree builder and the serializer, shipped in release 0.3.0. A packet starts
when every packet in its "After" list is committed. Phase 8 must not regress
the counts of Phases 2 to 7.

## Rules

- **Headers first.** Packet 8.2 writes every public declaration of the phase;
  later packets change no header in `include/sxml/`, and a need for one is a
  proposal.
- **The ratchet.**`tests/exi/passing.txt` lists the golden cases that pass. The
  `exi/golden` test fails when one of them fails; a packet adds the cases it
  makes pass, and removes none.
- **The gate script.**`tools/exi/check-packet.sh` runs what a packet must pass:
  the debug preset with sanitizers, the EXI unit tests and the ratchet,
  clang-tidy on the changed files, the call-cycle check, and no change under
  `include/sxml/`.

## Packets

Each packet gives what it implements, what it owns, what it needs first
("After") and when it is done.

### 8.1 EXI design and headers
**Summary**: Records D333 and writes every public declaration of Phase 8:
include/sxml/exi.h, the exi method and its parameters in serialize.h, the
namespaces in name.h, the feature, resource kind and processor functions in
processor.h.

**Done** 2026-11-27 After D333.

- **Owns**: new `include/sxml/exi.h`, `src/exi.c` (the lifecycles, encode and
  decode returning SXML_STATUS_UNSUPPORTED_FEATURE until their packets),
  `src/exi_internal.h`; additive changes to `include/sxml/serialize.h`,
  `name.h` (`SXML_NAMESPACE_EXI`, `SXML_NAMESPACE_EXI_FORMAT`; D99),
  `processor.h`, and the switches that list the enumerations they grow.
- **Done when**: every preset builds; the existing tests pass.

### 8.2 EXI guardrails: oracle, golden corpus, ratchet, gate script
**Summary**: Builds the deterministic checks every later packet must pass: the
exi_oracle, the golden corpus of tests/exi, the driver exi_golden with its
exi_ratchet, and tools/exi/check-packet.sh.

**Done** 2026-11-27 After 8.1.

- **Owns**: new `tools/exi-oracle/`, `tests/exi/`, `tools/exi/check-packet.sh`;
  the registration in `tests/CMakeLists.txt`.
- **Done when**: `exi/golden-self-check` passes (the comparison tells equal
  documents from different ones), `exi/golden` runs every case, and the gate
  script passes on the tree.

### 8.3 EXI streams, datatypes and string tables
**Summary**: Bit and byte streams, the built-in EXI datatype representations
and the string tables, the parts both codecs share.

**Done** 2026-11-27 After 8.2. EXI 1.0, sections 7 ("Representing Event
Content") and 7.3 ("String Table").

- **Mechanism**: `src/exi_stream.c`, a bit reader over a byte view and a bit
  writer over allocator memory, both with byte alignment (n-bit unsigned
  integers in whole bytes, least significant first, section 7.1.9);
  `src/exi_datatype.c`, Binary, Boolean, Decimal, Float, Integer, Unsigned
  Integer, Date-Time, n-bit Unsigned Integer, Enumeration, List and String,
  each in both directions, with their canonical forms (Canonical EXI, 4.5);
  `src/exi_string_table.c`, the URI, prefix, local-name and value partitions,
  global and local, with valueMaxLength and valuePartitionCapacity (its
  round-robin replacement, 7.3.3), their initial entries (Appendix D).
- **Owns**: those files, `tests/exi_stream_test.c`,
  `tests/exi_datatype_test.c`, `tests/exi_string_table_test.c`, their part of
  `src/exi_internal.h`.
- **Done when**: the unit tests cover every datatype at its limits (unsigned
  integers past 64 bits, negative zero decimals, float special values, every
  date-time kind), and the gate script passes.

### 8.4 EXI decoder with built-in grammars
**Summary**: Decodes schema-less streams in bit-packed and byte-aligned
alignment into tree builder events, with every fidelity option and the fragment
option.

**Done** 2026-11-27 After 8.3. EXI 1.0, sections 5 (the header, options out of
band), 6, 8.1 to 8.4 (built-in document, fragment and element grammars, with
learning) and 8.3 (event codes as the fidelity options prune them).

- **Mechanism**: `src/exi_grammar.c`, the grammar machine both codecs run;
  `src/exi_decoder.c`, the event loop to the builder, the NS events (prefixes
  ns0, ns1, ... without preserves_prefixes), the limits of `sxml_exi_limits`.
- **Owns**: those files, `tests/exi_decoder_test.c`, their part of
  `src/exi_internal.h`, `sxml_exi_decoder_decode` in `src/exi.c`.
- **Done when**: every decode check of the sets bit-packed, bit-packed-full,
  byte-alignment-full, string-limits, string-limits-zero and fragment passes
  and is in the ratchet.

### 8.5 EXI encoder with built-in grammars
**Summary**: Encodes a tree as a schema-less stream, bit-packed or
byte-aligned, byte for byte as the oracle does.

**Done** 2026-11-27 After 8.4. EXI 1.0, sections 5, 6 and 8, from the encoder's
side; namespace declarations from in-scope namespaces (D333).

- **Mechanism**: `src/exi_encoder.c`, a walk of the tree over an explicit stack
  (D71) that sends events through the grammar machine of 8.4.
- **Owns**: that file, `tests/exi_encoder_test.c`, `sxml_exi_encoder_encode` in
  `src/exi.c`.
- **Done when**: every encode check of the sets of 8.4 passes and is in the
  ratchet.

### 8.6 EXI pre-compression
**Summary**: The pre-compression alignment in both directions: the structure
channel, value channels by qualified name, blocks of blockSize values.

**Done** 2026-11-27 After 8.5. EXI 1.0, section 9 ("EXI Compression") but
DEFLATE.

- **Mechanism**: `src/exi_channel.c`: the encoder buffers a block's values per
  `exi_channel` and writes them in channel order; the decoder reads the
  structure channel first, then hands values out of the channels.
- **Owns**: that file, `tests/exi_channel_test.c`.
- **Done when**: every decode and encode check of the pre-compression sets
  passes and is in the ratchet.

### 8.7 DEFLATE of our own
**Summary**: An RFC 1951 inflater and deflater written for sxml, with no
third-party code: stored, fixed and dynamic Huffman blocks, a 32 KiB window,
bounded output.

**Done** 2026-11-27 After 8.2. RFC 1951.

- **Mechanism**: `src/inflate.c` (every block type, the length and distance
  codes, checks for every malformed case, output bounded by the caller) and
  `src/deflate.c` (LZ77 with hash chains over the 32 KiB window, dynamic
  Huffman blocks with lengths limited to 15 bits, stored blocks when they are
  smaller), behind `src/deflate_internal.h`.
- **Owns**: those files, `tests/deflate_test.c`, `tests/inflate_test.c`,
  `tests/deflate/` (vectors made once with another implementation and checked
  in: streams to inflate, including every block type and edge case).
- **Done when**: inflate decodes every vector; deflate's output inflates back
  to its input for every vector and a generated corpus; malformed streams are
  refused, never read out of bounds; the gate script passes.

### 8.8 EXI compression
**Summary**: The compression alignment in both directions: pre-compression's
channels, each block's streams compressed with our DEFLATE.

**Done** 2026-11-27 After 8.6 and 8.7. EXI 1.0, section 9.3: the structure
channel and channels of more than 100 values compressed one by one, the smaller
channels together.

- **Owns**: the compression part of `src/exi_channel.c`.
- **Done when**: every decode and encode check of the compression sets passes
  and is in the ratchet.

### 8.9 EXI schema-informed grammars
**Summary**: Builds the schema-informed grammars of EXI 1.0, section 8.5, from
a compiled schema, decodes and encodes with them, strict and not, with typed
values and the datatypeRepresentationMap.

After 8.5. EXI 1.0, sections 7.1 (built-in datatypes by schema type), 7.4
(datatypeRepresentationMap) and 8.5 (schema-informed document, fragment,
element fragment and type grammars, normalization, event codes, the undeclared
productions of non-strict grammars); the components of `src/schema_internal.h`.

- **Mechanism**: `src/exi_schema_grammar.c`, the proto-grammars of every
  particle, term and attribute use, normalized and numbered, built once per
  schema and kept with it; xsi:type and xsi:nil; the built-in types only
  grammars of the empty schemaId.
- **Owns**: that file, `tests/exi_schema_grammar_test.c`.
- **Done when**: every decode and encode check of the schema and xsd-types-only
  sets passes but header and canonical (8.10, 8.11), and the strict cases the
  oracle refused are refused.

### 8.10 EXI header options document
**Summary**: Writes and reads the options document of the EXI header, with the
schema-informed grammar of the EXI options schema, and the cookie.

After 8.9. EXI 1.0, section 5 and Appendix C (the options schema).

- **Mechanism**: `src/exi_header.c`; the options schema compiled from text
  embedded in the library and its grammar built by 8.9, once per encoder or
  decoder.
- **Done when**: the decode and encode checks of the header-options and
  schema-header sets pass and are in the ratchet.

### 8.11 Canonical EXI
**Summary**: The encoder's canonical form: event choice, the ordering of
attributes and namespace declarations, whitespace, canonical values, utcTime
and omitOptionsDocument.

After 8.10. Canonical EXI, sections 3 and 4.

- **Owns**: the canonical part of `src/exi_encoder.c` and `src/exi_datatype.c`.
- **Done when**: every encode check of the canonical sets passes byte for byte
  and is in the ratchet; omitOptionsDocument is tested by a unit test.

### 8.12 EXI self-contained elements
**Summary**: The selfContained option in both directions: SC events, their byte
alignment and their own string tables and grammars.

After 8.10. EXI 1.0, section 8.5.4.4.1 and the selfContained option.

- **Done when**: a golden set with selfContained (added to
  `tools/exi-oracle/make-golden.py`) passes; the gate script passes.

### 8.13 EXI in the processor: functions, the output method, schemaId
**Summary**: The feature sxml_exi_feature, exi:serialize and exi:parse, the
output method exi:exi with its parameters, sxml_processor_parse_exi_document,
the schemaId resolver, and the options reader.

**Done** 2026-11-27 After 8.11.

- **Mechanism**: `src/function_library_exi.c` (as
  `src/function_library_binary.c`, D198), the method in `src/serialize.c`, its
  parameters in the serialization parameter reader, `fn:serialize` refusing the
  method with SEPM0016, the options reader of an exi:header element, the
  processor's cache of compiled schemas per schemaId.
- **Done when**: XQuery and XSLT tests (`tests/exi_function_test.c`) serialize
  and parse through every path; the gate script passes.

### 8.14 EXI in the sxml command
**Summary**: The commands sxml encode and sxml decode, and --input-format on
the commands that read documents.

After 8.13. The rules of CLI.

- **Owns**: new `apps/sxml/command_encode.c`, `command_decode.c`, their golden
  tests in `tests/cli/`, their sections in `apps/sxml/README.md`.
- **Done when**: the golden tests pass; encode refuses to write to a terminal;
  README and help agree.

### 8.15 EXI in the host shim and the JavaScript package
**Summary**: The shim's encode and decode calls and the JavaScript package's
processor.encodeEXI and processor.parseEXI, within the size budget.

After 8.13.

- **Owns**: their parts of `bindings/host/` and `bindings/javascript/`, their
  tests, the budgets the size test names (D272).
- **Done when**: the package tests pass under node; the size test passes.

### 8.16 EXI fuzzing and documentation
**Summary**: Fuzz targets for the decoder, inflate and the options reader; the
book's EXI page; the changelog.

After 8.13.

- **Owns**: new `tests/fuzz/exi_decode_fuzz.c`, `inflate_fuzz.c`; a page of
  `docs/book`; the Unreleased section of `CHANGELOG.md`.
- **Done when**: the targets build and run two minutes each here without a
  finding; the book's examples are checked.

### 8.17 Phase 8 gate and release 0.3.0
**Summary**: Reviews the gate of Phase 8 and prepares release 0.3.0 by
docs/RELEASE.md, for the owner to tag.

After every other packet of Phase 8.

- **Done when**: the gate's boxes are reviewed; the version is 0.3.0 in its
  four places; the changelog section is dated; the release commit is on main.

## Gate

- [ ] Every golden case of `tests/exi/manifest.tsv` that EXIficient accepts
      passes: decoded as EXIficient decodes it, and encoded to the same bytes
      (compression decoded instead), and every case EXIficient refuses is
      refused.
- [ ] The counts of Phases 2 to 7 have not dropped, and `sxml.wasm` is within
      its budget.
- [ ] Each decoder entry point (the stream, DEFLATE, the options document) has
      a fuzz target, run briefly here and fully on the owner's machine.
- [ ] The command, the host shim and the JavaScript package reach the codecs,
      each with tests, and the book has an EXI page.
