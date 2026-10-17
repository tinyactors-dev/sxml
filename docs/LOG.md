# Weekly log

One short entry per week, newest first: what passed, what's next, what is blocked.

## Week of 2026-10-19

- 2.11 done (D140): fn:hash (MD5, SHA-1, SHA-256, BLAKE3, CRC-32 in
  `src/hash.c`, a new hash part), fn:decode-from-uri, fn:parse-uri and
  fn:build-uri (uri part; one percent decoder, one escaping table),
  fn:parse-QName and fn:resolve-QName (qname part). XP40 (release) per set
  before to after: fn-parse-uri 0 to 74/74, fn-build-uri 0 to 48/48,
  fn-decode-from-uri 0 to 29/29, fn-parse-QName 6 (wrong errors) to 19/19,
  fn-resolve-QName 4 to 8/8 run (23 XQuery-only), fn-hash 1 to 54/54. Full
  run with these: XP40 29,766 passed, slice 198/198, no set below the
  committed dashboard. Seen at HEAD 9f5168d, not 2.11's: the QT4 driver
  does not build (xquery_adapter.c of 3.13 against 2.12's processor
  changes: mpdecimal.h include path, document loader configuration).
- 4.5 done (D139): the stylesheet reader. `src/xslt_ast.c` (builder,
  import precedence, text form), `src/xslt_read.c`
  (`sxml_xslt_read_stylesheet`: modules through the XML parser, one
  explicit stack over all modules, element and attribute tables from the
  4.0 schema and syntax summaries, static contexts, 3.11.1 stripping,
  simplified modules, include/import with 4.0's include-cycle rule, XTSE
  checks), `src/xslt_static.c` (use-when, static variables and shadow
  attributes through the one evaluator, XSLT's five static functions).
  Scratch harness over the 8,712 in-scope xslt40-test cases: 6,933 read,
  348 fail with an expected code, 1,323 read but expect a later
  (dynamic or lowering) error, 108 wrong: 55 `~T` type patterns the
  pattern grammar lacks, 17 xsl:record (no AST kind), 36 single causes.
  Sets: use-when 97/102, static 39/48 (6 later errors), shadow 10/10,
  include 21/23 (embedded modules), import 41/41, forwards 23/23. No
  sanitizer report over the whole run; debug, debug-gcc, wasm,
  clang-tidy and ast-grep clean on the new files. XP40 untouched (no
  shared file changed).

- 2.17 done (D136): node, sequence, string and type functions. Node part:
  siblings, innermost, outermost, nilled, lang, id, element-with-id,
  idref, path (every 4.0 option, JNode keys), location; the name functions
  take xnode()?; fn:doc takes fn:parse-xml's options. String part:
  pad-string, index-of-substring. Sequence part: starts-/ends-/contains-
  subsequence (resumable). JSON part: parse-xml-fragment, strip-space=
  "all" (a stripping tree builder), system-properties. New type part:
  type-of, schema-type, atomic-/node-type-annotation (schema-type
  records over the registry). One options reader for every options map
  (`src/function_library_options.c`). Collations in min/max, highest/
  lowest, sort-by, array:sort(-by). XP40 (release, same machine) 28,476
  to 29,593 with the others' work since; per set before to after:
  fn-pad-string 2/22 to 22/22, fn-index-of-substring 1 to 21/21,
  fn-siblings 3 to 18/18, fn-innermost 8 to 57/58, fn-outermost 8 to 55/55,
  fn-nilled 5 to 14/14, fn-lang 6 to 38/38, fn-id 7 to 17/17, fn-idref 7 to
  10/10, fn-location 1 to 5/5, fn-path 6 to 60/60, fn-doc 42 to 54/60,
  fn-parse-xml 27 to 31/42, fn-parse-xml-fragment 9 to 36/39,
  fn-system-properties 1 to 23/23, fn-type-of 0 to 31/31, fn-schema-type 0
  to 9/9, fn-*-type-annotation 0 to 4/4, fn-starts-/ends-/contains-
  subsequence 12 to 95/95, fn-highest 29 to 30/30, fn-lowest 29 to 31/31,
  fn-sort-by 50 to 56/56, array-sort 27 to 29/29, array-sort-by 43 to
  48/48. Lost: fn-function-lookup-350/354/358 (function-lookup of the
  focus-dependent id#1, element-with-id#1, idref#1 is XPDY0002, D115's open
  lowering item; they passed only while the functions were missing).
  Survey: missing functions are 2.15's (CSV), load-xquery-module,
  transform and D97's non-goals. Slice 198/198; debug, debug-gcc, wasm
  pass; lint clean. Proposed: the decimal-zero comparison fix in
  `src/xdm_atomic.c` (`0.0 eq 0e0` is false), a descending range.

- 3.8 done (D137): group by and window clauses. Grouping specifications
  are segment bindings, keys rebound atomized (XQST0094, XPTY0004), then
  the internal group-by row over tuple arrays (atomic-equal keys,
  collation keys, implicit timezone); window clauses are segment clauses
  like for, a MAP over the window row's window arrays; one
  condition-closure procedure serves window conditions and while (D126's
  closure code left `src/xquery_lower.c`). XQ40 (release, 3.13's driver):
  prod-GroupByClause 0 to 35/40, prod-WindowClause 0 to 125/158 (all
  errors prologs); For, Let, Where, OrderBy, Count, While unchanged. XP40
  29,472 passed, slice 198/198. debug, debug-gcc, wasm (new tests),
  clang-tidy clean. Open: `sxml_sort_finish` (src/evaluator.c) fails with
  INVALID_ARGUMENT on an empty input (use-case-groupby-Q4).

- 2.12 done (D131): resources and the evaluation environment. New part
  `resource` (unparsed-text, -lines, -available, unparsed-binary,
  json-doc, collection, uri-collection, environment variables,
  default-language); fn:trace and fn:message reach the trace sink,
  `sxml_stream_trace_sink`. Static decimal formats and collations are
  checked and copied into the IR unit and read lexically (accepting
  2.9's and 2.13's proposals), plus 2.6's O(1) range comparison. XP40
  sets (release, before at 24fc6ac): unparsed-text 54 to 94/94,
  -lines 39 to 58/58, -available 7 to 57/57, unparsed-binary 16 to
  21/21, json-doc 36 to 73/77, collection 18 to 24/25,
  environment-variable 6 to 12/12, available-environment-variables 2 to
  10/10, default-language 0 to 5/5, message 3 to 14/14, format-number
  252 to 285/286. XP40 28,791 of 30,992 run (CONFORMANCE.md 23,146);
  slice 198/198; debug, debug-gcc, wasm, clang-tidy clean.

- 3.13 done (D134): `sxml-qt4 run --spec=XQ40` runs XQuery main
  modules through a temporary adapter (`tests/qt4/driver/xquery_adapter.c`,
  the signatures of xquery.h over the internal `sxml_xquery_compile`;
  packet 3.12 replaces it). `<module>` files served for XQUERY_MODULE,
  declared="true" params bound without a host declaration, collection
  queries evaluated, serialization-matches and assert-serialization-error
  through the serializer, feature serialization satisfied; the
  conformance report gives the D95 gate rate and the XQ-only rate. First
  XQ40 numbers (release, one process per test set): 33,374 of 47,313
  passed, gate 33,356/41,174 = 81.0 %, XQ-only 4,310/10,723 run =
  40.2 %; 5,969 errors are prologs (3.10), 179 group by (3.8). XP40
  29,029 before and after. New ctest `qt4/xquery-slice` (186 XQ-only
  cases, baseline 186) and golden `qt4/run_xquery`; debug, debug-gcc,
  wasm pass.

- 2.10 done (D130): elements to maps and back, part `element_map`.
  fn:element-to-map chooses layouts in document order and builds values
  in reverse document order over per-node element slots; plans group by
  name with XDM maps as indices; fn:map-to-element on a frame stack into
  a self-destroying arena tree, xml layout through the document loader.
  fn-element-to-map 30 to 179/181 run, fn-element-to-map-plan 0 to
  20/20, fn-map-to-element 75 (wrong errors) to 224/228. XP40 passed
  28,856 of 30,992 run in the run of this commit; slice 198/198; debug,
  debug-gcc, wasm, release 151/151, lint clean.

- 2.14 done (D129): date formatting. `src/format_date.c` (module
  header `src/format_date_internal.h`) scans date pictures by hand and
  numbers components through the D119 fn:format-integer engine;
  `src/function_library_format_date.c` adds fn:format-dateTime,
  fn:format-date and fn:format-time. Every component and modifier,
  width modifiers, fractional seconds, timezones (numeric, military,
  GMT), English names, calendars AD and ISO, fallback markers
  `[Calendar: AD]` and `[Language: en]` (D97). XP40 (debug driver):
  fn-format-date 20 to 124/124, fn-format-time 19 to 94/94,
  fn-format-dateTime 5 to 80/80 (all run cases pass); XP40 (release)
  28,534 passed, slice 198/198; debug, debug-gcc, wasm, lint clean. Test
  `format_date`: engine and API cases, allocation-failure loops.

- 2.6b done (D133): the 16 built-in record types of F&O 4.0, appendix C
  (`src/function_library_records.c`), declared once per type registry
  when the function library initializes; bodies return annotated records
  (`sxml_function_library_make_record`), generator and compiled-regex
  function items take their field types as signatures; coercion to a
  nominative record type; ?take is xs:nonNegativeInteger; INF idiv 0 is
  FOAR0001. fn-parts-of-dateTime 8 to 20/20, fn-divide-decimals 19 to
  21/21, fn-random-number-generator 50 to 54/54 run, fn-regex 27 to
  29/30 (-007 needs XPTY0004 for a dynamic call on ()),
  fn-matching-segments 37 to 38/38 run, op-numeric-integer-divide wrong
  errors 2 to 0. XP40 29,029 passed, slice 198/198; debug and debug-gcc
  pass (HEAD's tests/xslt_pattern_test.c fails -Wshadow under GCC, not
  this packet).

- 3.7 done (D126): XQuery lowering part `xquery` (`src/xquery_lower.c`,
  `src/xquery_lower_internal.h`): FLWOR as clause segments (MAP, LET,
  CONDITIONAL) with order by, count and while over tuple arrays (SORT,
  the unpacking MAP's position, fn:take-while with a closure), trace,
  typeswitch, switch, try/catch, ordered/unordered, pragmas, validate
  (XQST0075), string constructors; internal `sxml_xquery_compile`.
  Scratch harness (XQ40, 0 before): For 139/212, Let 158/189, Where
  68/85, OrderBy 133/210, Count 13/13, While 69/83, Typeswitch 80/88,
  Switch 34/58, TryCatch 190/212; the rest need prologs (3.10) or group
  by (3.8). XP40 27,842 (not touched: XQuery only), slice 198/198;
  debug 150/150, debug-gcc and wasm build and pass, lint clean.

- 3.4 done (D128): HTML, text, JSON and CSV methods and fn:serialize on
  the 3.3 pipeline (escape styles, a raw-text context; HTML a mode of the
  XML node writer, `src/serialize_html.c` for void elements, boolean
  attributes, raw text, PIs; JSON frames on a heap stack, nodes by
  sub-serialization, RFC 8785 canonical form; CSV from maps or arrays);
  `sxml_serialization_parameter_reader` for maps and parameter documents
  (two arenas); SERE0006 and SERE0003 added. fn-serialize 48 to 165/194
  (32 left need expath binary, parse-csv, unparsed-text); method-* are
  XQ-only (unit cases until 3.10/3.13). XP40 otherwise unchanged (the
  driver's adaptive path is untouched); slice 198/198; debug, debug-gcc,
  wasm serialize tests pass, lint clean on the packet's files.

- 3.9 done (D127): constructor lowering, part `construct` in
  `src/xquery_lower_construct.c`. Direct elements: SEQUENCE content of one
  atom per attribute and piece of content; namespace declaration
  attributes lowered as an embedding with an extended static context;
  in-scope declarations sent by nested elements; XQST0040. Computed
  constructors with literal or computed names. Constr sets 0 to 932/1,165
  (scratch harness); all remaining run failures but 7 need prologs (3.10).

- 2.13 done (D116): collations and comparison options. `src/collation.c`:
  codepoint, Unicode and HTML ASCII case-insensitive, UCA with fallback
  (UTS #10 levels over weights from Unicode properties: strength,
  alternate, maxVariable, caseFirst, caseLevel, backwards), keys and
  substring matching by collation units; `sxml_collation_is_supported`
  (the QT4 driver uses it; FULLY-NORMALIZED accepted). One resolver for
  every collation argument; fn:collation, -available, -key; collation-
  aware compare, index-of, distinct/duplicate-values, all-equal/-different,
  sort, contains-token, substring functions. fn:deep-equal: every 4.0
  option, items-equal as a resumable body, unordered comparisons by
  backtracking trials. XP40 passed 26,170 to 26,510, none lost; FOCH0002
  failures 280 to 12; fn-compare 188 to 221/225, fn-duplicate-values
  111/113, fn-deep-equal 327 to 395/449 (function-item equivalence and
  default collation Proposed), fn-collation 0 to 89/89, fn-collation-key
  0 to 35/35, misc-UCACollation 70 to 87/87. Slice 198/198; debug,
  debug-gcc, wasm 143/143, lint clean.

- 4.4 done (D118), Early packet of Phase 4: XSLT 4.0 patterns in
  `src/xslt_pattern.c` with the module header
  `src/xslt_pattern_internal.h`. Front end `sxml_xslt_pattern_build_ast`
  on the D111 parser part seam (19 pattern kinds, field PATTERN, flag
  OPEN appended to the AST; XTSE0340 for syntax and forbidden outermost
  calls). Patterns become match expressions (XPath ASTs of xs:boolean,
  read right to left as 6.3.2.3 describes, the formal definition for
  intersect/except and filtered parenthesized patterns) lowered through
  the embedding entry; match functions for template rules; default
  priorities (6.4, all 4.0 forms). Tests: 51 patterns against a fixed
  document by table, 43 item cases (atomic, map, array, sequence
  patterns), 60 priorities, 14 static errors, match function, allocation
  failures, 3,000 nested parentheses. Suite: 19,441 pattern attributes,
  19,288 parse, all build a match expression, 18,916 lower with the
  stylesheet's namespaces alone (the rest need XSLT functions or schema
  types). `debug`, `debug-gcc`, `wasm` all pass, lint clean; slice
  198/198; XPath behaviour untouched (kinds only appended).

- 2.7 done (D125): fn:divide-decimals over a new decimal operation
  (divisor exponent shifted, libmpdec divint/rem, precision fitted to the
  digit limit). Release XP40 23,146 (docs/CONFORMANCE.md) to 26,975 with
  the others' work since; fn-divide-decimals 4 to 19 of 21; math-*,
  fn-round, fn-round-half-to-even, fn-parse-integer, op-numeric-* already
  at 100% of run cases. Slice 198/198; `debug`, `debug-gcc`, `wasm`
  142/142; lint clean. Open (Proposed): F&O named record types
  (divide-decimals-019 to -021, random-number-generator-35a to -37a);
  take(-1) as XPTY0004 needs a unit-test change.
- 3.6b done (D124): IR extensions accepted from D109/D110.
  `sxml_ir_unit_add_name_tests`, `_add_catch_clauses`,
  `_add_namespace_bindings` and accessors; `construction_flags`
  (no-preserve, no-inherit, preserves-types, xslt-rules) and
  `name_namespaces` on `sxml_ir_node_construction`, honoured by
  `src/evaluator_construct.c`; the verifier checks TRY_CATCH and the
  constructor kinds (only APPLY_TEMPLATES stays reserved); the text form
  prints clauses, bindings and flags (other dumps unchanged). 4 new IR
  cases, 10 new constructor-mode cases (also under allocation failure);
  the try and constructor tests use the adders. `debug`, `debug-gcc`,
  `wasm` 143/143, lint clean, slice 198/198 (release). Release
  `evaluator_construct_test` passes at HEAD with gcc and clang -O3, -O2
  ASan+UBSan and valgrind: the reported release failure did not
  reproduce.

- 2.8 done (D122): date and time functions in the new part
  `src/function_library_temporal.c`: adjust-dateTime/date/time-to-timezone,
  build-dateTime (record fields coerced by the body), parts-of-dateTime,
  days-in-month, seconds, duration-to-seconds, unix-dateTime,
  civil-timezone (no tz database: implicit timezone, UTC names, else
  FODT0004) and parse-ietf-date (hand scanner); the `*-from-dateTime`
  rows take every Gregorian type. XP40 26,678 to 27,185 passed in a
  release snapshot, none lost; slice 198/198. fn-adjust-* 21 to 133 of
  133, build-dateTime 49 to 90/90, parse-ietf-date 37 to 105/105,
  seconds 6 to 24/24, duration-to-seconds 4 to 18/18, unix-dateTime 4 to
  22/22, days-in-month 4 to 16/18, the seven `*-from-dateTime` sets now
  all pass, parts-of-dateTime 4 to 8/20 (needs F&O named record types,
  Proposed), civil-timezone 1 to 4/9. `debug`, `debug-gcc`, `wasm`
  pass, lint clean.

- 2.9 done (D119): number formatting. One picture engine
  (`src/format.c`, `src/format_internal.h`) for fn:format-integer (digit
  patterns in any Nd family, radix 2 to 36, letters, Roman, English
  words and ordinals, fallback to "1") and fn:format-number (sub-pictures,
  exponents, percent and per-mille, marker:rendition properties, exact
  half-even rounding on digit text); decimal formats checked by
  `sxml_decimal_format_apply` for 2.14, 3.x and 4.10. Part `format` with
  options maps. fn-format-integer 13 to 87 passed (all run),
  fn-format-number 20 to 252 of 286 run; 33 failures wait for static
  decimal formats in the evaluator (Proposed). XP40 27,308 passed in the
  snapshot, no set lost; slice 198/198; `debug`, `debug-gcc`, `wasm`
  126/126 (the `evaluator` failure under `debug` is HEAD's), lint clean.

- 3.3 done (D120): XML and XHTML output methods. Serializer split into
  `src/serialize.c` (pipeline: character maps, normalization through
  `sxml_unicode_normalize`, escaping, line endings, UTF-8/UTF-16/US-ASCII/
  ISO-8859-1 with references; sequence normalizer; adaptive; dispatch,
  other methods UNSUPPORTED_FEATURE) and `src/serialize_xml.c`
  (declaration, doctype, namespaces against written bindings with
  undeclarations, indentation with suppress-indentation, indent-unit,
  indent-attributes, CDATA sections, XHTML void elements, HTML5 doctype,
  prefix normalization, content-type meta, URI escaping, Canonical XML;
  adaptive nodes incl. stored namespace nodes, D98). 13 new unit cases
  modelled on method-xml/method-xhtml, allocation-failure loop, 20,000
  nested elements. `debug`, `debug-gcc`, `wasm` all pass, lint and
  call-cycle check clean; slice 198/198; XP40 26,951 passed before and
  after (release runs of the snapshot at HEAD, no test changed). Open:
  the parameter reader of D106 (3.4), SERE0005/0006.
- 3.2 done (D121): XQuery front end. `sxml_xquery_build_ast` in
  `src/xquery_parse.c`, the XQuery parser part on the XPath walker:
  tables, keyword tokens, XQuery decoding (references with XQST0090,
  doubled delimiters and braces, attribute value normalization,
  end-of-line handling) and the D29 builder checks (XQST0118, XQST0040,
  XQST0071, XQST0022, XQST0070, XQST0085, boundary whitespace, XQST0031,
  XQST0087, %constructor record fields). XQ40 front end: before (scratch
  part, 3.1) 43,663 valid build with no checks; after 43,663 valid and
  145 modules build, 70 with their expected static error, 772/772
  XPST0003 rejected, 0 false accepts. XP40 26,951 passed at HEAD (no
  XPath path changed; CONFORMANCE.md 23,146), slice 198/198. `debug`,
  `debug-gcc`, `wasm` 126/126, lint clean.

- 2.6 done (D115): evaluator, paths, references. fn:jkey/fn:jvalue in
  new part `src/function_library_jnode.c` (moved from 2.17); selector
  interpretation of non-navigational steps from JNodes (XPDY0002 when
  focus-dependent), QName keys for prefixed names and wildcards (and
  EQName jnode() key tests, D113's proposal), attribute/namespace axes
  from JNodes XPTY0004, lookups through jvalue; numeric predicates of
  several numbers; maps/arrays as typed functions; fold fixpoint early
  exit; drop-while over a range stays a range. Release XP40, same
  driver: 23,698 to 23,910 passed before drop-while (1 former wrong error
  lost: PathExpr-J-461, fn:name on JNodes); prod-PathExpr.J 71 to 160 of
  175, prod-AxisStep.J 30 to 64 of 127, fn-fold-left 19 to 21/21,
  fold-right 22 to 24/24, fn-drop-while 17 to 18/18, prod-Predicate 188
  to 199/199, fn-jkey 4 to 14, fn-jvalue 4 to 15, fn-jtree 11 to 29,
  fn-function-lookup 499 to 500 of 587 (rest: missing functions; focus
  functions need lowering). Slice 198/198; `debug`, `debug-gcc`, `wasm`
  125/125; lint clean. Open: O(1) range comparison (RangeExpr-408h/410h)
  waits for `tests/xpath_test.c` (Proposed); reverse of a range (2.17);
  "/" on JNodes (lowering treat type); driver assert-eq on JNodes.
- 4.3 done (D120): value templates. `grammars/value_template`
  (inherits xpath; own scanner adds the fixed part and a zero-width
  start token), `sxml_xslt_build_value_template_ast` to an XPATH root
  with a STRING_TEMPLATE body through the parser part seam, XTSE0350 and
  XTSE0370 from a lexical scan after a syntax error. Every grammar now
  joins the sxml link line (`grammars/CMakeLists.txt`).
  `qt4-parse-check.py --spec XT40 --value-templates`: 12,244 in scope,
  12,232 accepted, 0 false rejects (one uses dropped `record(d?)`).
  `debug`, `debug-gcc`, `wasm` 126/126, lint clean; slice 198/198; XP40
  26,678 passed with and without the packet (sxml-qt4 links none of it).
- 4.1 done (D117): the XSLT 4.0 test driver `sxml-xslt-test`
  (`tests/xslt/driver/`), a sibling of sxml-qt4 that links its library.
  `list` resolves every case of `third_party/xslt40-test` (13,867, no
  problem; 8,387 applicable, 8,728 in the D95 scope) and shows its plan:
  principal stylesheet, package or embedded stylesheet, secondary
  modules, static and stylesheet parameters, global context item,
  invocation, delivery, assertion kinds. `run` reports through the QT4
  report: skip, "unsupported: assert-warning", errors for malformed
  cases, else "XSLT not implemented" until 4.15. Slice
  `tests/xslt/slice.txt` (154 cases over the twelve core sets), baseline
  0. Proposed: report name and XSLT areas in the QT4 report. `debug`,
  `debug-gcc`, `wasm` (xslt and qt4 tests), lint clean on the new files;
  debug ctest 141/141; no library or QT4-driver change, so XP40 and the
  qt4 slice (198) are untouched.

- 3.5 done (D110): node constructors in the evaluator. Seven
  constructor kinds and SHALLOW_COPY, one self-destroying arena tree per
  node, nested nodes copied; content convention for lowering (a SEQUENCE
  content lists enclosed expressions); names, PI, comment and namespace
  errors; strip, preserve/inherit fixed until the IR carries modes
  (proposed with a namespace-binding adder and verifier support).
  Deviation: `text {""}` is the empty sequence. 62 hand-built IR cases
  with allocation-failure loops, 20,000-deep arrays flattened. `debug`,
  `debug-gcc`, `wasm` 121/121, lint clean; slice 198/198; XP40 23,698
  passed before and after (release run of the snapshot at HEAD).
- 2.5 done (D113): lowering completions. for member and for key/value
  (array:split / map:entries collections, positions across all arrays
  or maps, XQST0089), destructuring let in its three forms, dynamic node
  tests (absent-focus selector, atomic-equal against node-name,
  local-name or jkey), the item axis (child step or a wrapper array;
  parent and order deviate, Proposed), constructor functions with a
  `$value := .` parameter (keyword arguments, `xs:T()`, `xs:T#0`),
  method calls, but with (map:merge, no coercion to the record type),
  jnode() selectors for decimals, doubles, negatives and QNames. Only
  computed node constructors stay unlowered. XP40 23,698 to 24,206
  passed, none lost; slice 198/198; prod-ForClause.map 7 to 56,
  .member 0 to 33, prod-LetClause 67 to 137, prod-MethodCall 0 to 12,
  misc-ConstructorKeywords and misc-FocusConstructors 5 to 153 each,
  prod-ButWithExpr 0 to 10 (needs parts-of-dateTime), prod-AxisStep.J
  30 (98 once fn:jkey/fn:jvalue land, 2.17). prod-NamedFunctionRef's 72
  failures are missing library functions. `debug`, `debug-gcc`, `wasm`
  119/119, lint clean.

- 3.1 done (D111): query AST and parser part seam. 66 XQuery kinds and
  24 fields in `src/xpath_ast_internal.h` (keyword detail, XQuery flags,
  boundary whitespace and namespace declaration attribute marks, PI
  payload), AST and IR of five XQuery examples written out;
  `sxml_ast_build_with_parser_part` runs the XPath walker with a part's
  grammar, tables and hooks (apply_token, describe_payload, finish_node,
  `sxml_ast_front_end_raise` for any err: code); `sxml_xquery_build_ast`
  declared for 3.2, which adds `sxml_grammar_xquery` to the link line.
  All 32,928 XP40 queries build byte-identical ASTs; a scratch part with
  only the tables builds all 43,663 valid XQ40 queries and 145 library
  modules. XP40 23,698 passed (CONFORMANCE.md 23,146; no AST changed),
  slice 198/198. `debug`, `debug-gcc`, `wasm` 119/119, lint clean.

- 2.4 done (D107): Unicode 16.0 tables and functions. `src/unicode.c`
  (UAX #15 NFC/NFD/NFKC/NFKD and FULLY-NORMALIZED; UAX #29 extended
  grapheme clusters with GB9c; HTML character reference names) over
  `src/unicode_tables.c` from `tools/generate-unicode-tables.py`
  (separate from the regex tables: no property in both);
  `src/function_library_unicode.c`: fn:char, fn:characters,
  fn:graphemes, fn:normalize-unicode. All 79,860 NormalizationTest.txt
  checks (plus part 1 identities) and all 1,093 GraphemeBreakTest.txt
  lines pass in a scratch probe; 13 unit cases with an
  allocation-failure loop. QT4 (release): fn-graphemes 0 to 918 of 918,
  fn-normalize-unicode 11 to 39 of 41 (the two left are "FULLY-NORMALIZED
  unsupported" variants the driver still runs; 44 of 44 once it declares
  the form), fn-char 24 to 47 of 48 (char-001 needs fn:json-doc),
  fn-characters 1 to 8 of 8; XP40 24,704 passed on 79ae2d0 plus 2.4, no
  test set lost; slice 198/198. `wasm` grows by about 143 KB (136 KB
  data, mostly decompositions and the 2,125 names). `debug`, `debug-gcc`,
  `wasm`, `release` 121/121, lint clean.
- 2.3 done (D105, D114): the regex engine covers the F&O 4.0 dialect
  (\p/\P categories and blocks, \i \c, \b \B, lookahead/lookbehind in
  both spellings, named groups and \k<name>, XSD 1.1 hyphens, case-blind
  matching through the fn:lower-case/upper-case tables plus 94 extra
  pairs), no recursion; tables regenerated from Unicode 16.0 (about 25
  KB). New part `src/function_library_regex.c` with fn:matches,
  fn:replace ($<name>), fn:tokenize (moved from the string part),
  fn:analyze-string, fn:matching-segments, fn:regex. Before/after
  (release): fn-matches.re 580/1082 -> 1082, fn-matches 155/200 -> 198,
  fn-replace 166/187 -> 185, fn-tokenize 67/71 -> 68, fn-analyze-string
  0/30 -> 29, fn-matching-segments 0/38 -> 36, fn-regex 0/30 -> 25;
  XP40 23,146 -> 23,793 (dashboard run 09e1fed vs private run on
  HEAD plus these files); FORX0002 561 -> 0; slice 198/198. `debug`,
  `debug-gcc`, `wasm`, `release` all pass, lint clean. Open:
  `a{2147483647}` is LIMIT_EXCEEDED (counted repetition is expanded);
  named record types for fn:regex/fn:matching-segments.

- 2.5 done (D113): lowering completions. for member and for key/value
  (array:split / map:entries collections, positions across all arrays
  or maps, XQST0089), destructuring let in its three forms, dynamic node
  tests (absent-focus selector, atomic-equal against node-name,
  local-name or jkey), the item axis (child step or a wrapper array;
  parent and order deviate, Proposed), constructor functions with a
  `$value := .` parameter (keyword arguments, `xs:T()`, `xs:T#0`),
  method calls, but with (map:merge, no coercion to the record type),
  jnode() selectors for decimals, doubles, negatives and QNames. Only
  computed node constructors stay unlowered. XP40 23,698 to 24,206
  passed, none lost; slice 198/198; prod-ForClause.map 7 to 56,
  .member 0 to 33, prod-LetClause 67 to 137, prod-MethodCall 0 to 12,
  misc-ConstructorKeywords and misc-FocusConstructors 5 to 153 each,
  prod-ButWithExpr 0 to 10 (needs parts-of-dateTime), prod-AxisStep.J
  30 (98 once fn:jkey/fn:jvalue land, 2.17). prod-NamedFunctionRef's 72
  failures are missing library functions. `debug`, `debug-gcc`, `wasm`
  119/119, lint clean.

- 4.0 done (D112): the XSLT API, header-only. `xslt.h`:
  `sxml_xslt_stylesheet` (quintet) from the principal module's bytes or
  through the resolver (XSLT_MODULE, XSLT_PACKAGE), static parameters at
  compile time; `sxml_xslt_output_definition` (D106 parameters plus
  build-tree and "no method given"); `sxml_xslt_stylesheet_transform`
  with the shared dynamic context (global context item, stylesheet
  parameters, xsl:message through the trace sink), apply-templates,
  call-template and function call invocations, raw/tree/serialized
  delivery, a result document handler (open/close) and its default, the
  `sxml_xslt_result_document_collector`. `debug`, `debug-gcc`, `wasm`
  119/119 (slice included), lint clean; no source changed, so XP40
  counts cannot move.

- 3.6 done (D109), Early packet of Phase 3: TRY_CATCH in
  `src/evaluator_try.c` as one catching task: name tests and wildcards,
  first matching clause, `$err:map` bound to the clause's slot (keys
  code, description, value, module, line-number, column-number; no
  entry when empty), finally (XQTY0153; its error replaces the first;
  unmatched and catch-clause errors raised again after it), lazy values
  read inside the try, an own error record when the caller passes none.
  17 unit cases over hand-built IR (resumable native calls, limits,
  allocation-failure loops). Proposed: IR adders for catch clauses and
  name tests, verifier support. `debug`, `debug-gcc`, `wasm` 120/120,
  lint clean; slice 198/198; XP40 23,698 passed in the snapshot (XPath
  cannot produce TRY_CATCH, so unchanged by this packet).

- 3.0 done (D106): the XQuery and serialization API, header-only.
  `xquery.h`: `sxml_xquery_query` (quintet) compiled with the shared
  static context, library modules through the XQUERY_MODULE resolver
  kind, evaluation with the shared dynamic context, the prolog's
  serialization parameters returned by value. `serialize.h`: methods
  xml, xhtml, html, text, json, csv; one struct with every
  Serialization 4.0 parameter (tri-state `sxml_serialization_flag`,
  encodings UTF-8/UTF-16/US-ASCII/ISO-8859-1, character maps, element
  name lists); `sxml_serialization_parameter_reader` for
  output:serialization-parameters elements and option maps. `debug`,
  `debug-gcc`, `wasm` 119/119 (slice included), lint clean; no source
  changed, so XP40 counts cannot move.

- 2.16 done (D108): QT4 driver for Phase 2. Environments decimal-format
  (D104 properties, prefixed names), collation (D97 URIs run, default
  sets the default collation), resource and collection (new resource
  table: TEXT/BINARY by URI with the resource's encoding, COLLECTION and
  URI_COLLECTION as sequences, default collection) are applied; only
  collections with a query wait (3.13). Applicability reports the D95
  scope: `list --spec=XP40` 31,543 in scope, `--spec=XQ40` 41,174. JSON
  report gains `areas` and `in_scope`/`in_scope_passed` counts (format
  version 1, additive). New dependencies satisfied: simple-uca-fallback,
  non_empty_sequence_collection, arbitraryPrecisionDecimal, NFC/NFD/NFKC/
  NFKD. Goldens `run_environments`, `run_environments_json`,
  `list_environments`; unit tests for scope and the resource table.
  XP40 23,146 to 24,451 passed (none lost), not run 18,448 to 16,321;
  fn-format-number 20 to 27 passed (not run 72 to 22),
  fn-unparsed-text 40 to 54, -lines 23 to 39, -available 6 to 7,
  fn-collection 10 to 18, fn-json-doc 7 to 36, misc-BuiltInKeywords 0 to
  206; slice 198/198. `debug`, `debug-gcc`, `wasm` 122/122, lint clean.
- 2.1 done (D103): lowering seam. `src/xpath_lower_internal.h` exports
  the lowerer (lifecycle quintet), its building blocks and the frame
  machine; frames point at procedure descriptors, so core and part
  procedures run alike. Lowering parts (`src/<name>_lower[_<part>].c`
  defining `sxml_xpath_lowering_part_<part>`) are registered by
  `cmake/SxmlParts.cmake` (none yet); a part's rule comes before the core
  for its language. Embedding entry (from a step or from outside) lowers
  an XPath AST in the caller's scope, focus and the AST's static context;
  `sxml_xpath_lower` uses it. Probe parts and kinds in `tests/ir_test.c`
  (7 new cases with an allocation-failure loop). Slice dumps unchanged;
  `debug`, `debug-gcc`, `wasm` 119/119, lint clean; slice 198/198; XP40
  23,146 passed before and after (release run of the snapshot).
- 2.0 done (D101): part registries by glob. CMake generates the library
  part list (per language: XPath, XQuery, XSLT) and the evaluator part
  list; processor and fuzz fixture take the XPath list. Machine API in
  `src/evaluator_machine_internal.h` (part tasks, catching tasks, global
  initializer hook); stubs construct, try, template, global return
  UNSUPPORTED_FEATURE. Probes verified a new part, an XSLT-only part and a
  catching part need no other edit. `debug`, `debug-gcc`, `wasm` 119/119,
  lint clean (89 files, no cycle); slice 198/198; XP40 unchanged: 23,146
  passed, 5,209 failed, 510 errors (before and after).

- 2.2 done (D104): the public API for Phase 2, header-only. Static
  context: decimal formats (name, properties as text with 4.0's
  marker:rendition form), default collation URI, collation bindings,
  `sxml_collation_is_supported`. Dynamic context: default language,
  environment variables, trace sink. Resources: kinds TEXT, BINARY,
  COLLECTION, URI_COLLECTION, XQUERY_MODULE, XSLT_MODULE, XSLT_PACKAGE,
  form SEQUENCE, request fields for encoding, default collection, module
  namespace and package version. `sxml_trace_sink` for fn:trace,
  fn:message, xsl:message, with `sxml_stream_trace_sink`. version.h:
  `SXML_VERSION_ENCODE`, `SXML_VERSION_NUMBER`, `SXML_API_VERSION`
  (0.2.0). 2.12 defines the two new functions. `debug`, `debug-gcc`,
  `wasm` 117/117, lint clean; XP40 23,146 passed before and after, slice
  198/198.

- 4.2 done (D102), Early packet of Phase 4: the stylesheet AST,
  `src/xslt_ast_internal.h` (header only). One AST per stylesheet:
  modules (pre-order, import precedence computed at seal), one node kind
  per element of the XSLT 4.0 schema plus literal result elements, text
  nodes, extension, unknown and data elements; attribute kinds per
  schema attribute with values read into typed forms (expressions,
  patterns, value templates and types as their own `sxml_ast`); a
  resolved static context per element; declaration list; text form.
  for-each-group-001 and a template rule, function and import written
  out. Builder functions go to 4.5 (`src/xslt_ast.c`). Header compiles
  alone under clang, gcc and wasi-sdk with the project flags and is
  clang-tidy clean; `debug`, `debug-gcc`, `wasm` 117/117, lint clean.
  No translation unit includes it yet, so XP40 and the slice are
  unchanged (198).

- 1.29 done, Phase 1 gate review (D98-D100). Rubric pass over all public
  headers and the core modules; no writable globals, no allocation or
  printing outside their places, no recursion through function pointers
  (call-cycle check now also links the QT4 driver: 85 files, no cycle).
  Substrate fixed before the freeze: base URI of parentless roots
  (`sxml_arena_tree_configuration.base_uri`), parentless namespace nodes
  stored as a tree's root; then frozen (D99). Local gate on `09e1fed`:
  `debug`, `debug-gcc`, `release`, `wasm` 117/117 ctest, `fuzz` 5/5,
  `tools/lint.sh` clean, slice 198/198 (sanitizers and `wasm`); five fuzz
  targets 300 s each, clean; dashboard regenerated: XP40 23,146 passed
  (800 wrong error), 5,209 failed, 510 errors, 18,448 not run (up from
  21,443 by 1.23b). `wasm` CLI 1,268,420 stripped / 385,760 gzip, inside
  the D93 budget; 1.23b added 55 KB stripped without logging it.
  **Blocked**: CI refuses every job for billing since 2026-10-16; the CI
  gate boxes stay open and macOS, Windows, MSVC are unverified.

- 1.28 done (D93): WebAssembly slice. `tools/run-wasi.mjs` preopens the
  source tree as well as ".", so `xml_conformance` runs under WASI
  (601/175/948) instead of skipping; `qt4/slice` 198/198 under `wasm`
  (117/117 ctest). Sizes, stripped / gzip: `sxml` CLI 1,213,261 / 358,020,
  `sxml-qt4` 1,264,250 / 376,528; `libsxml.a` 947,910. Slice: native
  release ~0.95 s, `wasm` ~1.9 s. Budget: CLI < 1.5 MB stripped, < 450 KB
  gzip; not yet enforced in CI.
- Phases 2-4 planned (D95-D97): `docs/tasks/phase-2.md` (21 packets:
  seams, regex, Unicode, collations, formatting, resources, driver, JS
  package and release 0.2.0), `phase-3.md` (16: AST, front end,
  serializers, constructors, try/catch, FLWOR, group/window, prolog,
  modules, API, CLI) and `phase-4.md` (19: API, driver for xslt40-test,
  stylesheet AST and reader, value templates, patterns, templates and
  modes, grouping, keys, numbering, output, accumulators, packages).
  Targets (D95): XP40 >= 90%, XQ40 >= 85% (XQ-only >= 75%), XSLT 4.0 in
  scope (about 8,700 cases) >= 70%. Early packets of Phases 3 and 4 may
  start after the Phase 1 gate (D96).
- 1.27 done (D94): `tools/run-qt4.sh` (one `sxml-qt4` process per test
  set, parallel, timeout, crash recorded per set) and
  `tools/conformance-report.py` (dashboard, merged JSON, slice-baseline
  check) generate `docs/CONFORMANCE.md`. XP40, release: 47,313 cases,
  21,443 passed (1,007 wrong error), 6,933 failed, 489 errors, 18,448 not
  run (74.3 % of run cases pass); slice 198/198; 737 sets, none crashed;
  24 s on 4 cores. CI: jobs `conformance` (blocking on the slice,
  artifacts) and `fuzz` (60 s per target, D86); runs on main are no
  longer cancelled in progress.
- 1.25 done (D92): `sxml-qt4 run` (`test_run.c`, `assertion.c`,
  `report.c`, `reason.c`, `command_run.c`). Each test in its own processor
  and 512 MiB budget allocator, 10^8 steps, leak check per test; every
  assertion kind (assert-xml by parse-and-deep-equal; serialization ones
  not-run); outcomes pass / wrong-error / fail / error / not-run; text and
  JSON reports. Slice: **198/198 pass** (`debug`, `debug-gcc`, `wasm`);
  ctest `qt4/slice` against `tests/qt4/slice-baseline.txt` (198), goldens
  `qt4/run_*`. Whole catalog, XP40, release, one process per test set (20
  s on 3 cores; no crash, no leak): 47,313 cases: 21,443 passed (1,007
  wrong error), 6,933 failed, 489 errors, 18,448 not run (XQ-only and
  unsupported dependencies). By area (cases: passed/failed/errors/not
  run): fn 17,250: 8,374/5,751/90/3,035; prod 14,932: 7,859/521/391/6,161;
  op 4,789: 3,765/14/8/1,002; misc 3,042: 159/305/0/2,578; app 3,034:
  318/95/0/2,621; array 537: 456/35/0/46; map 361: 334/17/0/10; xs 227:
  178/14/0/35; math 181: 0/181/0/0; upd, ft, bin, method, expath: not run.
  Causes: XPST0017 unknown function 5,596 (+148 keyword arguments);
  FORX0002 regex category/block escapes 405, lookaround 42, name escapes
  32; FOCH0002 collations 100; XPTY0004 110; errors: not lowered in Phase
  1 (171 constructs, 157 for member/key-value/destructuring let, 43
  dynamic node tests, 24 item axis), 15 step/memory limits (fold-left/
  right-022/023, RangeExpr-408/410, cbcl-matches-004/038). Most failing
  sets: fn-graphemes 918, fn-matches.re 502, fn-format-number 216,
  fn-compare 204, fn-function-lookup 165, fn-map-to-element 153,
  fn-element-to-map 151, misc-ConstructorKeywords and
  misc-FocusConstructors 148 each, fn-serialize 146,
  prod-NamedFunctionRef 144, fn-deep-equal 120, fn-duplicate-values 106,
  fn-format-date 104, prod-PathExpr.J 104, prod-AxisStep.J 97.
- 1.23b follow-ups (D91): 62 library functions added (new parts
  `src/function_library_math.c` and `src/function_library_qname.c`, rows
  in the node, string, uri, sequence and context parts), found by listing
  the XP40 fn-/map-/array-/math- test sets whose function the library
  lacks (139 functions, 5,796 cases before; 77 and 4,165 after; the list
  is in the task file). Adaptive output writes the type annotation's name
  (`xs:dateTimeStamp(...)`, qtspecs PR 2981). The static base URI is
  lexical: IR units carry it, so a function item keeps its own.
  fn:string-join, sort, distinct-values, deep-equal, count of a lazy
  sequence and the regex functions charge evaluation steps (D89).
- 1.23 done (D88, D89): processor and XPath API. `src/processor.c`
  (clocks, table resolver, processor in one block with pool, registry,
  heap and the whole function library, document parsing, the
  per-evaluation document loader: fn:doc stable per URI, FODC0002/0006),
  `src/xpath.c` (compile, evaluate with its own dynamic context so
  evaluations nest, sequence types from text). Static base URI passed to
  fn:doc, parse-xml, json-to-xml and the new fn:static-base-uri and
  fn:resolve-uri (`src/function_library_uri.c`, RFC 3986 resolution
  shared from `src/xdm_node.c`). Limit comments fixed (heap, not C
  stack); P9 accepted as D89: an evaluation step bound (default 10^9).
  `sxml xpath` (`--context-item`, `--param`, `--method=adaptive`) with 19
  golden tests. Tests `processor` (7) and `xpath` (15, 30 slice queries
  through public headers). `debug`, `debug-gcc`, `wasm` pass (111 tests).
- 1.26 follow-up (D90): map:merge merges into its largest map, so folds
  that merge one entry into an accumulator, on either side, are O(n log n)
  (were O(n^2): 6 s and 18 s under ASan on two QT4 seeds). New internal
  `sxml_map_put_first` (`src/xdm_map_internal.h`). Scaling test
  `map_merge_scales`.
- 1.26 done (D86, P9): fuzz targets. `tests/fuzz/` (xml_parse, xpath_compile,
  xpath_evaluate, regex, cast; per-input leak checks and sxml-error
  statuses), preset `fuzz`, `tools/fuzz.sh`, `tools/fuzz-seeds.py` (xmlconf
  and QT4 seeds). 10 minutes each, 4 at once: cast 1.66M inputs, regex
  784k, xml 80k, compile 35k: clean; evaluate: clean but for 2 QT4 seeds
  that time out (fold-left over map:merge, 6 s and 18 s under ASan:
  map:merge re-inserts every entry; P9 for unbounded evaluation). Fixed:
  UTF-16 surrogates broken by the D80-to-D81 rename (xml/encodings failed
  on HEAD); null+0 for empty tokenized attribute values (xml.c) and empty
  lexical forms (cast_lexical.c); O(depth^2) content walk in xml.c (100000
  levels: 600 s to 7 s); choice map key types rejected by the type
  registry (fn:replace's signature); comparisons walking ranges for dates
  (evaluator.c). Each with a regression test. CI 60 s per target waits for
  1.27 (`ci.yml`).
- 1.22 done (D82): JSON, parse-xml, adaptive output. `grammars/json`
  (strict RFC 8259 tree-sitter grammar; D12 kept, reasons in D82),
  `src/json.c` (one cursor loop into JSON events; value sink and XML
  sink; xml-to-json writer with FOJS0006/0007 checks and a key index),
  `src/function_library_json.c` (parse-json, json-to-xml, xml-to-json,
  jtree, parse-xml through the document loader), `src/serialize.c`
  (output buffer, adaptive method on a heap stack). 12 cases in
  `tests/json_test.c` (all 22 JSON/parse-xml/JNode slice lines, 100,000
  nesting levels, allocation-failure loops), 8 in `tests/serialize_test.c`.
  `debug`, `debug-gcc`, `wasm` pass except the `xml` test (fails at HEAD,
  not 1.22); lint clean, no call cycles. Not yet: a `fallback` function,
  strip-space="all".
- XML fix (D87, P8 accepted): the position tracker moves back in place
  instead of restarting from byte 0, so parsing is linear: `sxml-qt4 list
  tests/qt4/torture-slice.txt` 23.3 s -> 0.76 s (release). The `xml` ctest
  failure (UTF-16 surrogates, 0xD810 for 0xD800) and its 600 s depth case
  (ts_tree_cursor_current_depth) were fixed in d44b472 and 14ece34; `xml`
  now runs 6.7 s debug, 8.5 s debug-gcc, 0.8 s wasm. Open: tree-sitter
  parses about 3.5 MB/s (9.6 MB in 2.7 s, release).
- 1.18b done (D85): follow-ups from 1.20/1.21. Dynamic calls on a
  sequence of functions (evaluator's `call_each` body) and dynamic partial
  application per function; maps and arrays become JNodes on the left of
  `/` and as step origins; the coercion rules on an explicit stack
  (choices/unions by alternative, typed arrays, maps, records, JNodes, full
  implicit-casting table, function coercion even for instances), so
  fn:replace's `$replacement` has its F&O 4.0 type again; integer ranges
  match in O(1) (codepoints-to-string(65 to 2^32) fails fast, FOCH0001);
  a left-out body-supplied parameter before a keyword argument gets ();
  array:index-of; static namespaces kept in the IR unit for run-time
  casts to xs:QName. Scratch QT4 comparison over ~70 sets: no regressions;
  DynamicFunctionCall 33 -> 65/73, RecordType 12 -> 16, ChoiceItemType
  23 -> 27, PathExpr-J 30 -> 48.
- 1.24 done (D83, P8): the QT4 driver `sxml-qt4` in `tests/qt4/driver/`
  (catalog, environments, applicability, test lists, `list` command) on
  the CLI framework, reading the suite with sxml's own XML parser through
  the public API. `sxml-qt4 list tests/qt4/torture-slice.txt`: 198/198
  resolve and apply to XP40 (`debug`, `debug-gcc`, `wasm`). Whole suite:
  47,313 cases, 32,928 XP40 by spec (as `docs/QT4_CATALOG.md`), 28,865
  applicable after dependencies and environments. Tests: `qt4_driver`
  (unit, in-memory files, allocation-failure loop) and 14 `qt4/*` golden
  tests. Blocked-ish: P8, the XML parser's positions are O(n^2), so the
  slice listing takes 160-200 s under ASan (13 s release, 21 s wasm).
- 1.20 done (D78): core function library. `src/function_library.c`
  (index over the configured parts, type text resolved on first find,
  xs: constructors, `sxml_function_table_find`), parts numeric, string,
  sequence, node, context, error, plus `function_library_deep_equal.c`
  (explicit stack) and `function_library_case.c` (Unicode 16.0 case maps,
  `tools/generate-case-tables.py`). ~110 functions; fn:sort and fn:replace
  resumable. 18 cases in `tests/function_library_test.c` (slice lines,
  allocation-failure loops); `debug`, `debug-gcc`, `wasm`: function_library
  passes (the new `xml` test fails on gcc/wasm, not 1.20); lint clean, no
  call cycles. Scratch QT4 run of the F&O sets touched: about 93% of
  applicable cases (rest: collations, regex seed limits, evaluator gaps).
- 1.14 done (D81, D69): the XML parser. `src/xml.c` (CST walk over
  explicit entity frames, all 16 builder checks, namespaces, events),
  `src/xml_encoding.c` (UTF-8, UTF-16, ISO-8859-1, US-ASCII; positions),
  `src/xml_entity.c` (DTD tables). W3C suite through the library: valid
  601/601, invalid 175/175, not-wf 948/948 (`xml_conformance` ctest, ~6 s
  under ASan; skipped under WASI). QT4: 1046/1051 `*.xml` parse, the 5
  others are bad on purpose. `sxml check [--format=text|json] FILE...`
  with 9 golden tests; the CLI framework gained `sxml_cli_file_reader`.
  P6 accepted as D69 (`ignores_parent_namespaces`).
- 1.21 done (D79, D80): higher-order, map and array functions.
  `src/function_library_higher_order.c` (F&O 4.0 2.5 except fn:sort, 13,
  fn:op; traversal, fold and sort engines shared through
  `src/function_library_higher_order_internal.h`), `src/function_library_map.c`
  (all of 14.4; map:build/merge as one resumable assembly),
  `src/function_library_array.c` (15.2 except array:index-of). Every
  function-calling row is a resumable body: fold-right over 10^6 items in
  constant C stack. 10 cases in `tests/function_library_higher_order_test.c`
  (about 50 slice lines, the 4.0 additions, depth limit, allocation-failure
  loop) over a test table (1.20's library not yet committed); `debug`,
  `debug-gcc`, `wasm`: 61/61; lint clean, no call cycles. Open:
  DynamicFunctionCall-146 needs the evaluator to call a sequence of
  functions; array:index-of waits for 1.20's deep-equal.
- 1.18 done (D75, D76, D77, D73): the evaluator. `src/evaluator.c`
  (explicit-stack machine: tasks for expressions, calls and native steps
  in allocator memory, tail calls reuse frames, depth as LIMIT_EXCEEDED),
  `src/evaluator_function.c` (atomization, EBV, type matching with
  backtracking for choices, coercion incl. function coercion),
  `src/evaluator_path.c` (XNode and JNode axes, document order, node
  sets). Resumable native bodies for 1.21 in
  `function_library_internal.h`. P4, P5, P7 accepted. 19 cases in
  `tests/evaluator_test.c` (about 90 slice lines without library calls,
  100k-deep recursion, 100k fold-left steps, allocation-failure loop);
  `debug`, `debug-gcc`, `wasm`: 60/60; lint clean, no call cycles.
- D71 for lowering, type resolution and type text: `src/xpath_lower.c` is a
  machine of frames in allocator memory (procedures with resumable steps),
  `src/xpath_static.c` and `src/ir.c` use 64-frame stacks; lowering's
  default depth is 10,000 (heap) instead of 32 (wasm stack). IR dumps of
  all 29,653 lowerable XP40 queries unchanged (464 unsupported, 2,292
  static errors, no leaks); 100,000 nested levels compile (wasm: 3.3 s for
  the suite). misc-no-recursion on, call-cycle allowlist empty.
- D71 for the regex seed (D53 amended): `src/regex.c` parses in one
  loop with a stack of group frames (16 inline, then `stack_allocator`),
  emits each atom once and copies it for quantifiers, reads and matches
  class subtraction chains in loops; misc-no-recursion clean. Fixed a
  write past the arrays for a trailing x{0}. Groups and subtraction
  nested 10,000 deep compile and match (also on wasm); 400,000 random
  regexes behave as before; regex 26/26, `debug`, `debug-gcc`, `wasm`:
  59/59; lint clean.
- D71 for the value model: `src/xdm_map.c` (insert, remove, replace
  value, key search, build) and `src/xdm_array.c` (join, split) walk their
  AVL trees in loops over fixed 96-entry paths on the C stack (no tree
  that fits in memory is higher than 91); `sxml_node_make_base_uri` is a
  loop; the dump helpers of `tests/tree_test.c` and
  `tests/xpath_parse_test.c` use an explicit stack and parent links.
  Indirect recursion through cell classes audited: releases only queue,
  concatenations hold no concatenations; only caller-supplied lazy
  generators can nest. `debug`, `debug-gcc`, `wasm`: 59/59; lint and
  misc-no-recursion clean.
- D74: libmpdec's transpose buffers (up to 128 KiB on the stack) moved to
  the heap through the decimal scope, marked "sxml patch"; long products
  (10^6000 * 10^6000) no longer trap on the 64 KiB wasm stack. Audit:
  largest libmpdec frame now 1.2 KiB on wasm, measured worst stack of
  multiply and divide 2.9 KiB, flat up to 16 million digits; recursion
  bounded, no digit cap needed. Four new cases in `tests/decimal_test.c`
  (10,000 and 60,000 digits, allocation failures); `debug`, `debug-gcc`,
  `wasm`: 59/59; lint clean.
- D71 for types and casts: `src/types.c` evaluates subtype questions as an
  and/or tree of subtype goals on a fixed explicit stack (192 frames, no
  allocation, as the header promises; last sub-goals replace their frame)
  and searches union members for derivation on a stack of 256;
  `src/cast.c` casts union, choice and list targets on a heap stack of
  cast frames (at most 33). Behaviour and limits unchanged except that
  choices of choices searched past 192 frames now answer false (D58).
  New tests nest function types, choices, enumerations and document tests
  10,000 deep and choice targets 10,000 deep; `debug`, `debug-gcc`,
  `wasm`: 59/59; lint and misc-no-recursion clean on these files.
- 1.19 done (D68, D72): operators. `src/operator.c` (promotion, numeric
  operators with int64 overflow into libmpdec, exact double idiv/mod,
  unary), `src/operator_temporal.c` (duration, date and time arithmetic,
  PR2628 scaling), `src/operator_compare.c` (value comparisons via
  fn:compare, general comparisons with the PR2218 untyped rules, GNode
  comparisons). P2 and P3 accepted into `xdm.h` (D68); recursion removed
  from `xdm_atomic.c` (D71). The 22 arithmetic and comparison slice lines
  hold as unit cases; 15 cases in `tests/operator_test.c`; `debug`,
  `debug-gcc`, `wasm`: 59/59; lint clean. Open: libmpdec FNT
  multiplication overflows the wasm stack (D72).
- 1.13 done (D70): `src/xdm_node.c` (node items, string value, typed
  value derived or from the tree, base URI through xml:base with RFC 3986
  resolution), `src/xdm_json_node.c` (JNodes on demand, identity and
  document order of generic nodes by child-position paths). op-union-406
  and fn-root-29 hold as unit cases; 12 cases in `tests/xdm_node_test.c`;
  `debug`, `debug-gcc`, `wasm`: 58/58; lint clean. Open: P7.
- 1.12 done (D63): static analysis and lowering. `src/xpath_static.c`
  (static context, names, sequence and item types, also for function
  library rows), `src/xpath_lower.c` (AST to IR: ANF blocks, chains in a
  loop, focus slots, captures, closures for placeholders and
  focus-dependent references, `sxml_xpath_compile`), `src/ir.c` (unit,
  text form, pass list), `src/ir_verify.c`. XPST0008/0017/0081/0051/0080/
  0039/0152/0021 with locations; K-QuantExprWithout-79, fn-filter-003,
  map-merge-001 fail statically; 28 slice and 46 construct IR dumps match
  golden text. With a permissive table, 29,653 of 32,409 valid XP40
  queries lower and verify (the rest: test environments, unsupported
  4.0 constructs). 23 cases in `tests/xpath_static_test.c` and
  `tests/ir_test.c`; `debug`, `debug-gcc`, `wasm`: 57/57; lint clean.
- 1.11 done (D67): trees. `src/tree.c` (wrapper, reference count and
  owner, namespace nodes on demand, document order, iterative
  `copy_node`), `src/tree_axis.c` (all 17 axes incl. -or-self),
  `src/arena_tree.c` (node segments, text store, text merging, duplicate
  attribute policies with a hash index, namespace fixup, limits,
  locations, unparsed entities, self-destruction). Node items now retain
  their tree (1.7 hand-off). 27 cases in `tests/tree_test.c` (64-row axis
  table) and `tests/arena_tree_test.c`; `debug`, `debug-gcc`, `wasm`:
  55/55; lint clean. Open: P6 (no-inherit copies).
- 1.15 done (D66): `src/cast.c`, `src/cast_lexical.c`,
  `src/cast_internal.h`: the F&O 4.0 casting table, lexical forms of every
  primitive type, canonical text (XPath rules for xs:double/xs:float,
  shortest digits), integer facets, xs:NMTOKENS, unions, enumerations and
  choices, cast and castable (FORG0001, FOCA0002, XPTY0004, FONS0004,
  FODT0001/2, XPST0080). The 17 cast slice lines pass as unit cases; 17
  cases in `tests/cast_test.c` incl. an allocation-failure loop;
  `debug`, `debug-gcc`, `wasm`: 53/53; lint clean.
- 1.10 done (D64): `src/xdm_array.c`, persistent arrays as AVL trees of
  flat (made arrays, O(1) get), run (part of a flat node, no copying)
  and branch nodes; append, put, insert, remove and slice are split plus
  join, O(log n). `src/xdm_function.c`: function items (signature must be
  a non-wildcard function type; captures retained, body retained once and
  released with the cell), arity check (XPTY0004), maps and arrays called
  as map:get and array:get (FOAY0001, also beyond `int64_t`). 8 cases in
  `tests/xdm_array_test.c` (random updates against a model with old
  versions checked, 10^5 appends with allocations per append bounded,
  10^5-deep nesting, failure loop, byte limit) and 6 in
  `tests/xdm_function_test.c`; `debug`, `debug-gcc`, `wasm`: 52/52; lint
  clean.
- 1.9 done (D61): `src/xdm_map.c`, maps and records. Entry blocks keep
  entries in order with a hash index (more than 8 entries) built while
  making, for the three duplicate key policies (XQDY0137); put and remove
  give persistent maps (entry tree by stamp, key tree by hash; O(log n),
  old versions intact); records hold field values, find fields through
  the registry and make their xs:string keys on first need; put on a
  record gives a map. map-merge-401/-402 order, map-get-014/-015 (NaN)
  and map-put-011/-024/-025 hold as unit cases. 9 cases in
  `tests/xdm_map_test.c` incl. an allocation-failure loop and a byte
  limit; `debug`, `debug-gcc`, `wasm`: 50/50; lint clean. Open: P3.
- 1.8 done (D62): `src/xdm_sequence.c`, integer ranges (`int64_t` bounds,
  `uint64_t` indices; 1 to 10^11 counts right on `wasm`), flattened
  concatenations with a part-end cache (binary search, lazy parts read
  only as far as needed), memoized lazy sequences whose kept error is
  raised again on every later read, `sxml_sequence_test_emptiness`.
  16 cases in `tests/xdm_sequence_test.c` incl. an allocation-failure
  loop; `debug`, `debug-gcc`, `wasm`: 49/49; lint clean. Seen: wasm
  `xpath_parse` segfaults intermittently (about 1 run in 8) on HEAD too.
- 1.7 done (D60): item heap and atomic core. `src/xdm_internal.h` is the
  shared interface for 1.8-1.11 and 1.15 (item layout, cell classes with
  sequence read operations, iterative release, byte accounting incl.
  decimal coefficients, `sxml_atomic_item_make_taking_decimal`, static
  error records). `src/xdm.c`: heap, raised error, vectors, iterator,
  builder; `src/xdm_atomic.c`: every primitive and built-in derived type
  with value-space checks, atomic-equal, hash (1, 1.0, 1.0e0, 1.0f hash
  alike), fn:compare per PR2256. 36 cases in `tests/xdm_test.c` and
  `tests/xdm_atomic_test.c`; `debug`, `debug-gcc`, `wasm`: 48/48; lint
  clean. Open: tree retention hand-off to 1.11; P2 (heap in
  `sxml_atomic_comparison`).
- 1.6 done (D58): type registry, all of `types.h`: static built-in
  descriptions (names interned per pool), interned registered item types
  in a normal form, nominative record types (declare, define, recursive),
  record field and enumeration value indexes, derivation, and the
  XPath 4.0 §3.3 subtype relations (choices, unions, enumerations, node,
  JNode, function, map, array, record rules; transitivity folded into the
  rules) with a depth and step budget. map-put-002-hof holds.
  `tests/types_test.c`: 28 cases incl. allocation-failure loops; `debug`,
  `debug-gcc`, `wasm`: 44/44 tests; lint clean.
- 1.16 done (D59): XPath front end, `src/xpath_parse.c` (CST to AST in one
  iterative cursor walk; XPST0003 with location for ERROR/MISSING, A.3.2
  delimitation and prefixed lookup keys; literals and names decoded;
  sequence types through `() instance of`) and `src/xpath_ast.c` (AST
  lifecycle and builder). All 198 slice queries and all 32,542 valid XP40
  queries build; the two D29 false accepts are now rejected. 32 unit cases
  in `tests/xpath_ast_test.c` and `tests/xpath_parse_test.c`; `debug`,
  `debug-gcc`, `wasm` pass, lint clean.
- 1.17 done (D53): regex seed. `src/regex_internal.h` is the interface
  the D9 engine keeps (flags with FORX0001; immutable compiled regex with
  FORX0002 and exact memory requirements; per-thread matcher with
  `match_at`, `find`, `find_next` over disjoint matching segments, step
  and backtrack-stack limits). Hand-written recursive-descent parser
  rather than a tree-sitter grammar (reasons in D53). Subset: classes with
  subtraction, `\s \d \w` (Unicode 16.0 tables), greedy and reluctant
  quantifiers, groups, alternation, `^ $`, back-references, flags
  `s m i x q c`; the rest is FORX0002 "not supported". fn-tokenize-1a
  and -35 pass as unit cases. `debug`, `debug-gcc`, `wasm`: 43/43; lint
  clean.
- 1.2 done (D57): libmpdec 4.0.1 vendored and patched (scoped
  allocation of D42, size_t-based configuration, WASI), `src/decimal.c`:
  parse, canonical text, exact add/subtract/multiply, divide (34 digits),
  idiv, mod, round (all nine modes), compare, int64/uint64/double/float
  conversions. `tests/decimal_test.c`: 24 cases incl. allocation failures
  and two heaps on two threads. `debug`, `debug-gcc`, `wasm`: 42/42; lint
  clean. WebAssembly growth: +89 KB stripped (+35 KB gzip).
- 1.4 and 1.5 done (D52, D55), design only: `src/xpath_ast_internal.h`
  (one kind per visible XPath production, children by grammar field,
  fixed-capacity arrays sized from the CST, front-end entry points for
  1.16), `src/ir_internal.h` (units, atoms, slots, explicit focus slots,
  reserved XQuery/XSLT kinds; worked IR for inline-fn-005, `/a/b[2]`, a
  FLWOR and an apply-templates), `src/ir_pass_internal.h` (pass seam,
  verifier invariants), `src/evaluator_internal.h` (dynamic context,
  evaluate, coercion, document loader) and
  `src/function_library_internal.h` (native rows with F&O type text, one
  part per file, function table interface). Headers compile alone under
  Clang and GCC with the project flags; clang-tidy and ast-grep clean.
- 1.1 done (D54): name pool, all of `name.h`: hash-indexed interning of
  URIs, prefixes and names, static well-known namespaces, find without
  interning, byte limit; `src/name_internal.h` gives identifier limits.
  `tests/name_test.c`: 18 cases incl. allocation-failure and limit loops.
  `debug`, `debug-gcc`, `wasm`: 41/41 tests; lint clean.
- 1.0 done: `src/*.c` and `tests/*_test.c` registered by glob (D45);
  `sxml` links the xml and xpath grammars and `sxml_mpdecimal` once
  vendored; `tests/qt4/driver`, `tests/fuzz` (`SXML_BUILD_FUZZERS`) and
  `third_party/mpdecimal` join when their `CMakeLists.txt` exists. P1
  accepted as D50: `SXML_STATUS_INPUT_OUTPUT_FAILED`, used by the CLI
  platform layer. `debug`, `debug-gcc`, `wasm`: 40/40 tests; lint clean.
- 1.3 done (D51): public API headers `xml.h` (reusable XML parser into
  any tree builder, or check only; diagnostics with line and column),
  `processor.h` (processor owning pool, registry and heap; limits; resource
  resolver for `fn:doc` with a table resolver; system and fixed clocks;
  `sxml_processor_parse_document`), `xpath.h` (static and dynamic context
  configurations shared with later XQuery and XSLT, compiled expressions,
  evaluation, SequenceType from text) and `serialize.h` (output stream,
  output buffer, adaptive method). Design only: header check under
  `debug`, `debug-gcc` and `wasm`, lint clean. Implemented by 1.14, 1.22
  and 1.23; the driver (1.24) and the CLI's `check` and `xpath` use them.
- CLI (D44, `docs/CLI.md`): CLI framework in `apps/cli_framework/`
  (command tables, parser, help, "did you mean", exit codes 0-4, output
  streams) and the `sxml` program in `apps/sxml/` with `help`, `--help`,
  `--version` and `version [--format=text|json]` only. 4 unit suites and
  30 golden end-to-end tests in `tests/cli/` pass under `debug`,
  `debug-gcc`, `release` and `wasm`; lint clean, new rule
  `input-output-outside-platform-layer`. Next: `sxml check` after 1.14,
  `sxml xpath` after 1.23. Proposed P1: an input/output status.
- Phase 1 planned: `docs/tasks/phase-1.md` splits the torture slice into
  30 packets with owned files, dependencies and an 11-wave parallel plan.
  Decisions: glob-based registration and file ownership (D45), internal
  tree-walking ANF-like IR with a pass seam (D46), public `xml.h`,
  `processor.h`, `xpath.h`, `serialize.h` (D47), regex seed (D48),
  libmpdec in Phase 1 (D49). The 198 slice queries use about 53 `fn:`
  functions, 9 `map:` and 9 `array:` functions and 17 `xs:` constructors.
- 0.8 done (review agent, D11): rubric run over all ten headers. Fixed:
  wildcard name tests in node tests; lookup of namespace URIs without
  interning; fn:empty on lazy sequences no longer swallows errors
  (`sxml_sequence_test_emptiness`); coercion is the caller's in dynamic
  calls; computed error descriptions and values kept by the item heap
  (`sxml_item_heap_raise_error`, D41); duplicate attribute policy for XQuery
  versus XSLT builders; ownership verbs, iterator ABI and comment wrapping
  (D43). libmpdec hooks confirmed process-global; scoped thread-local
  allocator decided (D42). D3 (Phase 0 time box) recorded, D4-D10 confirmed.
  6/6 suites pass under `debug`, `debug-gcc`, `wasm`; lint clean; CI green.
  Phase 0 gate passed by agent review.
- 0.7 done: substrate headers `name.h` (name pool, D31), `types.h` (type
  registry with constant built-in identifiers, interned item types, sequence
  types, D32), `tree.h` (tree interface with preorder node numbering,
  namespace nodes by number, document order by tree identifier, axes, tree
  builder events, arena tree, D39-D40) and `xdm.h` (item heap; 16-byte items
  and sequences; immutable reference-counted values; atomic datums; vector,
  range, concatenation and lazy sequences; function bodies as an interface;
  ordered persistent maps, records, arrays, JNodes; D33-D38). Header order
  and resolver deferral in D30, thread confinement in D35. Design only:
  header check and lints green under Clang and GCC, no implementation.
- Open questions for 0.8: whether `sxml_tree_operations` should be split
  (one optional typed-value op versus a separate schema interface in
  Phase 5); whether decimals should get an inline fast path; how a host
  shares one parsed document between threads (D35 defers it).
- Query grammars (D29): tree-sitter grammars for XPath 4.0
  (`grammars/xpath`), XQuery 4.0 (`grammars/xquery`, inherits XPath) and
  XSLT 4.0 patterns (`grammars/xslt_pattern`, inherits XPath), with one shared
  external scanner (`grammars/common/query_scanner.h`) and corpus tests.
  `tools/qt4-parse-check.py` parses the QT4 suite: XP40 0 false rejects of
  32,118 valid queries, XQ40 0 of 43,663; false accepts 2 of 363 and 3 of
  772 XPST0003 queries, left to the tree builder; XSLT patterns 3 of 19,168
  (invalid DocBook patterns). The XSLT test suite still uses type patterns
  (`~T`) that the 13 October 2026 draft has dropped (136 patterns).
- XML 1.0 grammar `grammars/xml` (D28): strict grammar plus external
  scanner (tag stack, text tokens, [2] Char), corpus tests (59 pass).
  W3C XML suite: valid 601/601 and invalid 175/175 parse cleanly, not-wf
  870/948 rejected by the grammar; the rest is listed for the tree builder in
  `grammars/xml/README.md`. QT4 + XSLT 4.0 suites: every well-formed UTF-8
  `*.xml` file (3583 of 3612) parses cleanly. `grammars/CMakeLists.txt`
  builds every generated grammar.
- `string.h` implemented in `src/string.c` (D27): strict UTF-8 validation,
  ASCII flag, codepoint count, `codepoint_at`, iterator, codepoint
  comparison; 29 cases in `tests/string_test.c` pass under `debug`,
  `debug-gcc` and `wasm`; lint clean.
- 0.5–0.6 done: core headers `status.h` (statuses, `SXML_NODISCARD`, error
  record and diagnostic, D21), `view.h` (D19), `memory.h` (views, checked
  arithmetic, memory requirements, allocator interface without reallocate
  (D22), default and counting allocators (D23), arena (D24)) and `string.h`
  (D25, designed, not implemented). Golden module `src/memory.c` +
  `src/allocator.c` (+ `src/status.c`) with 47 unit test cases in
  `tests/memory_test.c`, `allocator_test.c`, `status_test.c`; 5/5 suites pass
  under `debug`, `debug-gcc`, `release`, `wasm` and a shared GCC build; lint
  clean. New lint `pointer-parameter-without-length` (D26). Failure-path
  convention: outputs untouched on failure (D20).
- Open questions for 0.8: should `string.h` be implemented before the Phase 0
  gate; is the borrowed-view lifetime of error records (D21) acceptable for
  host bindings, which will copy them anyway.
- Conformance suites vendored under `third_party/` (D13 revised);
  `tools/update-test-suites.sh` replaces the fetch script.
- 0.1–0.4 done: CMake presets `debug`, `debug-gcc`, `release`, `msvc`, `wasm`
  (D14); test runner with one executable per module (D16); `tools/lint.sh`
  with clang-tidy and ast-grep rules (D17); CI workflow (D18, not yet run on
  GitHub). tree-sitter v0.27.1 vendored and built on every preset, wasm
  included; grammar generation script and `sxml_add_grammar` ready (D15).
  2/2 tests pass under ASan+UBSan (Clang, GCC), release and WASI.
- 0.9 done: `tools/fetch-test-suites.sh` pins qt4tests and xslt40-test (D13);
  `docs/QT4_CATALOG.md` documents the catalog; `tests/qt4/torture-slice.txt`
  lists 198 XP40 tests, checked by `tools/check-test-list.py`.
- Repository bootstrapped with plan, guidelines, glossary and decisions.
- Next: Phase 0 tasks 0.1–0.4.

## Open questions at the Phase 0 gate (task 0.8)

Questions the review left open, for the owner or a later phase. None blocks
Phase 1.

1. **Thread-local scope for libmpdec (D42).** It keeps heaps independent and
   leaves no state between calls, but it is the one thread-local in sxml.
   Accept it, or require the own-decimal fallback?
2. **Phase 0 time box (D3).** Recorded by the review agent because the
   original D3 was never written; the owner may want another length.
3. **Typed values (from 0.7).** Kept as one optional `get_typed_value`
   operation in `sxml_tree_operations`. Phase 5 publishes a separate schema
   interface only if validated trees need more than typed values and the
   `is_id`/`is_idrefs`/`is_nilled` flags; "interfaces are not extended in
   place" applies.
4. **Inline decimals (from 0.7).** Not added: integers that fit `int64_t`
   are inline already (D36); a scaled-integer fast path for short decimals
   can be a later cell format without an interface change.
5. **Sharing one parsed document between threads (D35).** Still deferred;
   the likely shape is an immortal, frozen tree with a read-only name pool.
6. **Error record lifetime for host bindings (from 0.5).** Kept as borrowed
   views (D21, D41); bindings copy what they keep.
7. **Strings without a copy.** `sxml_atomic_item_make` copies text, so
   `fn:substring` and string concatenation cannot share or adopt a buffer.
   If profiling in Phase 2 shows the cost, add a constructor that adopts a
   heap buffer, with a decision record (the substrate is frozen at the
   Phase 1 gate).
8. **Schema-derived atomic types (Phase 5).** The registry has no function
   to register them yet; it will be a new function, with a record.
9. **Document and collection resolution (D30).** Designed with the dynamic
   context in Phase 1, outside the substrate headers.

## Open questions at the Phase 1 gate (packet 1.29)

For the owner. Only the first blocks a gate box.

1. **CI billing.** GitHub Actions refuses every job ("recent account
   payments have failed or your spending limit needs to be increased").
   Restore billing (or move CI elsewhere) so the two CI gate boxes can be
   ticked and macOS, Windows and MSVC are built again; until then those
   platforms are unverified since the Phase 0 gate.
2. **Phase 2 may start with the CI boxes open?** The review ran every check
   locally (D100); CLAUDE.md is the orchestrator's to move to Phase 2.
3. **Substrate freeze (D99).** Accept the rule: additive changes with a
   record, no changes to existing declarations without reopening D99.
4. **The 510 "error" outcomes of XP40** are mostly unlowered constructs
   (UNSUPPORTED_FEATURE, 493); 8 hit LIMIT_EXCEEDED and 7 the 512 MiB
   budget of the driver. D95 asks for zero at the Phase 2 gate; the 15
   resource cases need a look in Phase 2 (2.19 sweeps).
5. **Size budget in CI.** D93's budget is not enforced by a test; 1.23b
   grew the `wasm` CLI by 55 KB unnoticed. Add the size check to CMake or
   `ci.yml` in Phase 2 (2.0 owns the root CMakeLists.txt).
