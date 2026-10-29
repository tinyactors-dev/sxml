<!-- Generated from spec/articles by tools/spec/spec generate. Do not edit: change it with tools/spec/spec (spec/README.md). -->

# Glossary

One spelling per concept. Identifiers use these words exactly. Add a term
(`tools/spec/spec add term NAME --definition=...`) before using it in code;
`tools/spec/spec show NAME` and `find WORDS` look one up. Terms follow the XDM
4.0 and XPath 4.0 specifications.

| Term in code | Meaning |
| --- | --- |
| `absolute_uri` | A URI with a scheme, after resolving a reference against a base URI (RFC 3986, section 5) |
| `abstract_pattern` | A sch:pattern or sch:group with abstract="true": rules with placeholders and its declared parameters, some with default values, compiled only through its pattern instances (5.4.11, 6.4; D315) |
| `abstract_rule` | A sch:rule with abstract="true", in sch:rules or (as older schemas write) in a schematron pattern: variables and assertions that a sch:extends rule="ID" puts into a rule in its place (5.4.15, 5.4.16; D315) |
| `abstract_syntax_tree` | What a front end builds from a concrete syntax tree: one node per visible grammar production, children by grammar field, names as written, literals decoded; `ast` allowed as a module name only (`sxml_ast_node`) |
| `accumulator` | The value a fold carries from one call of its function item to the next (`fn:fold-left`, `fn:scan`) |
| `accumulator_scope` | The xslt accumulators applicable to one tree when they are restricted: for the initial match selection, those use-accumulators of the initial mode names; kept in the evaluation memo (XSLT 4.0, 19.2; D155) |
| `accumulator_values` | The map of one xslt accumulator for one tree, from each node's generate-id to its pre- and post-descent values, built once per evaluation and kept in the evaluation memo (D155) |
| `active_pattern` | A schematron pattern the phase in force includes; only patterns that the compiled phase can make active are compiled (D315) |
| `adoption_agency` | The algorithm that closes misnested formatting elements by moving nodes already built (13.2.6.4.7, "the adoption agency algorithm") |
| `alignment` | A power of two that an address must be a multiple of |
| `all_matches` | The set of matches a full-text selection gives over the tokens of a node, with their positions (XQuery and XPath Full Text 3.0, "AllMatches") |
| `allocation_number` | Position of an allocate call among all calls to one allocator, from 1 |
| `allocation_request` | Byte count and alignment of one allocation |
| `allocator` | The memory interface: state plus operations table |
| `alphabet` | The letters a numbering by letters counts with (`a` to `z`, Greek `α` to `ω`), named by its first letter: 1 is the first letter, after the last come two letters (bijective numeration) |
| `api_version` | The release whose interface the headers declare, as one integer (`SXML_API_VERSION`) |
| `applicability` | Whether a QT4 test case can run for a target: its spec, its other dependencies and its environment are supported; otherwise the reason it is skipped |
| `arena` | Bump allocator whose allocations are released together |
| `arena_tree` | The built-in tree: nodes in arrays indexed by node index, built by a tree builder |
| `argument_list` | The command-line arguments after the program or command name, as byte views |
| `argument_placeholder` | A `?` in an argument list, making the call a partial application |
| `arithmetic_operator` | What an arithmetic expression computes: add, subtract, multiply, divide, integer divide or modulo (XPath 4.0, section 4.9) |
| `arity` | Number of parameters of a function item |
| `arity_range` | The arities a function accepts: from its required parameters up to all its parameters (XSLT 4.0, 10.3.1; XQuery 4.0, 5.18) |
| `array` | An XDM array; members are sequences |
| `array_member` | One member of an array |
| `array_node` | A cell of an array's persistent balanced tree (`src/xdm_array.c`): a flat node, a run node or a branch node; an array item's cell is the root array node |
| `article` | One XML file under `spec/articles/`: a decision, proposal, term, packet, log entry or document, with a kind and an identifier; read with queries and written with node updates through `tools/spec/spec`, never as text (`spec/README.md`) |
| `ascii_member_set` | The ASCII codepoints a regex character class contains, negation, subtraction and case variants included: 128 bits per class, found when the regex is compiled, so matching an ASCII character is one bit test (internal, `src/regex.c`, D310) |
| `assembly` | How `map:build` and `map:merge` add entries one by one to a map, combining duplicate keys by the `duplicates` option |
| `assertion_copy` | The copy of an element's recorded subtree that the assertions of its complex type evaluate on: nothing outside the element, the element itself annotated xs:anyType (XSD 1.1 Part 1, 3.13.4.1) |
| `assertion_entry` | A backtrack-stack entry marking where a lookahead or lookbehind assertion started; the assertion's end and backtracking find it (D105) |
| `assertion_verdict` | What one QT4 result assertion, or a combination of them (`all-of`, `any-of`, `not`), says about a test's result: pass, wrong error, fail or unsupported |
| `assessment` | How the schema validator assesses one element or attribute: strictly, against a governing type; laxly, where nothing governs it (global declarations are used where they exist, validity not known); or skipped with its subtree (processContents skip) (XSD 1.1 Part 1, 5.2 and 3.10.1) |
| `ast_field` | The role of a child in its AST parent: a field name of the grammar (`left`, `predicate`, `argument`) |
| `ast_memo` | The ASTs one compilation has parsed, keyed by front end, expression text and the parse's limits, so a text met again is parsed once and each use takes a copy (`sxml_ast_memo`, perf-17) |
| `atom` | An IR operand that needs no evaluation: a constant, a slot or a global variable (D46) |
| `atomic_equal` | The context-free equality of map keys (F&O 4.0, section 2.2.1 `fn:atomic-equal`) |
| `atomic_family` | The atomic items that compare with one another: numbers, strings (with `xs:anyURI`, `xs:untypedAtomic`), binary, or one other primitive type |
| `atomic_hash` | A hash consistent with `atomic_equal` |
| `atomic_item` | A value of an atomic type together with its type annotation (XPath 4.0 says "atomic item", not "atomic value") |
| `atomic_text` | The lexical form of an atomic item (its cast to xs:string) as the serializer writes it, inline or in heap memory |
| `atomization` | Replacing nodes by their typed values and arrays by their members, until only atomic items remain (XPath 4.0, section 2.5.3) |
| `attribute_declaration` | The schema component that governs attributes of one name: simple type and value constraint (XSD 1.1 Part 1, 3.2) |
| `attribute_default` | The value an attribute definition supplies when an element does not specify the attribute (XML 1.0, section 3.3.2) |
| `attribute_definition` | One attribute of an attribute-list declaration: its name, type and default (XML 1.0, section 3.3) |
| `attribute_group` | A named set of attribute uses and an attribute wildcard (XSD 1.1 Part 1, 3.6, "attribute group definition") |
| `attribute_marker` | The string put before the key of an attribute in an element map (`@` by default) |
| `attribute_set` | All `xsl:attribute-set` declarations of one name: one IR function of the focus that returns their attributes (XSLT 4.0, 10.2; D150) |
| `attribute_set_invocation` | One name in an `[xsl:]use-attribute-sets` attribute, a call of that attribute set with the current focus (XSLT 4.0, 10.2.1) |
| `attribute_use` | The component that says whether an attribute is required, with which declaration and value constraint, in a complex type or attribute group (XSD 1.1 Part 1, 3.5) |
| `available_document` | A URI and the document node `fn:doc` returns for it (XPath 4.0, "available documents") |
| `axis` | An XPath axis (`SXML_AXIS_CHILD`, ...), walked by an `axis_iterator` |
| `axis_iterator` | Walks the nodes of one axis from an origin node |
| `axis_walk` | Walks the nodes of one axis from an origin node and hands out each with its description, one description per node (internal, `src/tree_axis.c`, D224) |
| `backing_allocator` | The allocator another allocator passes its requests on to |
| `backtrack_stack` | The stack of choices and undo entries of a backtracking matcher; it grows through an allocator within a limit |
| `backwards_compatible_behaviour` | XSLT's processing of an element whose effective version is below 2.0 by the XSLT 1.0 rules (XSLT 4.0, 3.8) |
| `base_output_uri` | The URI relative `href` values of `xsl:result-document` are resolved against; the URI of the principal result (XSLT 4.0, 2.4.6) |
| `base_type` | The type a schema type is derived from by restriction, list or union |
| `base_uri` | `dm:base-uri` of a node |
| `basic_member` | A member type of a union with every union member replaced by its own members, in order: an atomic or list type; the active basic member is the first the value is valid for (XSD 1.1 Part 2, 2.4.1.3) |
| `binary_value` | An `xs:hexBinary` or `xs:base64Binary` item, a sequence of octets; the EXPath Binary functions accept both and return `xs:base64Binary` (EXPath Binary 4.0, 1.2) |
| `binding_collection` | The value a for binding iterates (XPath 4.0, section 4.13.1): its items, the members of its arrays (`for member`) or the entries of its maps (`for key/value`) |
| `block` | One piece of memory an arena takes from its block allocator |
| `block_escape` | `\p{IsX}`: the codepoints of Unicode block X, named as in Blocks.txt without spaces |
| `boolean_attribute` | An HTML attribute whose only value is its name, written in minimized form (`<option selected>`; Serialization 4.0, 7.2) |
| `boundary_whitespace` | Whitespace in direct element content delimited only by tags, direct constructors and enclosed expressions (XQuery 4.0, 4.12.1.4) |
| `branch_node` | An array node with a left and a right subtree; its members are the left's followed by the right's |
| `build_tree` | The `build-tree` setting of an output definition: whether an unserialized result is turned into a document node (XSLT 4.0, 26.1) |
| `built_in_template` | The template function a mode calls for an item no template rule matches: the built-in template rule of its on-no-match (XSLT 4.0, 6.10) |
| `builtin_component` | A component every schema has (XSD 1.1 Part 1, 3.2.7, 3.4.7, 3.16.7): the built-in type definitions, the content of xs:anyType and the four xsi: attribute declarations, added first so their indexes are fixed |
| `builtin_record_type` | A nominative record type of F&O 4.0 (appendix C, `fn:dateTime-record`) that the function library declares in its processor's type registry, so every static context knows it |
| `builtin_schemas` | The schemas a processor compiles for itself and keeps: the built-in components alone (XSD 1.1 Part 1, 3.4.7) and the fn: namespace (F&O 4.0, appendix D); for XPath's functions, the optional feature `sxml_builtin_schemas_feature` (D225) |
| `builtin_type` | A type predefined by XSD, XDM or XPath, with a constant type identifier (`SXML_TYPE_XS_INTEGER`) |
| `but_with` | `A but with B`: record A with the entries of map B put in, use-last (XPath 4.0, section 4.15.4) |
| `byte_count` | A length measured in bytes |
| `byte_offset` | A position measured in bytes from the start of something |
| `byte_order_mark` | U+FEFF at the start of a document, which identifies UTF-8 or the byte order of UTF-16 (XML 1.0, Appendix F) |
| `byte_range` | A byte offset plus a byte count within a view |
| `byte_reader` | A source of a resource's bytes that sxml reads a part at a time into a buffer it lends, until the reader says the input ends (`sxml_byte_reader`, SXML_RESOURCE_FORM_BYTE_READER; D233) |
| `byte_view` | A read-only pointer plus `byte_count` |
| `byte_view_list` | A read-only pointer to byte views plus `byte_view_count` |
| `calendar` | The calendar a date is formatted in, named by a designator such as `AD` or `ISO` (F&O 4.0, section 9.9.4.8) |
| `call_each` | The evaluator's own resumable body that calls a sequence of function items in turn and concatenates the results (a dynamic call on several functions, XPath 4.0, section 4.6.3.1) |
| `caller_focus` | The focus of a static call, passed by lowering as three extra arguments (context value, position, size) to a native function that hands it on (fn:function-lookup binds a context-dependent function it finds to it) |
| `canonical_form` | How an article file is written: indented except inside the elements that hold text, where indentation would add text (`$spec:serialization`), with a final newline; a file not in it was edited as text (`spec:not-canonical`) |
| `canonical_text` | The canonical lexical form of a value as UTF-8 bytes (F&O 4.0, section 19.1.2, casting to `xs:string`) |
| `capture` | A node that a pattern of a tree-sitter query names with `@name`; the last highlights pattern that captures a node gives its `highlight` attribute |
| `captured_group` | The segment a capturing subexpression matched; group 0 is the whole matching segment (F&O 4.0, section 6.1.5) |
| `captured_groups` | XSLT's current captured groups (XSLT 4.0, 17.2): the map from group numbers and names to fn:captured-group-record values, group 0 the match, set in xsl:matching-substring as the context variable regex-groups (D153) |
| `captured_value` | A nonlocal variable value a function item keeps (its closure) |
| `captured_variable` | An atom of the defining function whose value a function item made from an IR function keeps (its `captured_value`) |
| `case_mapping` | The Unicode full case mapping of `fn:upper-case` and `fn:lower-case`: one codepoint to one or more (F&O 4.0, section 5.4) |
| `case_variant` | A character that `i` matches in place of another: same `fn:lower-case` or same `fn:upper-case` (F&O 4.0, section 6.2) |
| `cast` | Converting an atomic item to a target type (F&O 4.0, section 19 "Casting"): `cast as`, constructor functions, and the untypedAtomic casting and promotion of coercion |
| `cast_frame` | A cast to a union, choice or list target on the explicit stack of a cast, waiting for the cast of a member or token (D66) |
| `castability` | Whether a value can be cast to a target type, as `castable as` answers |
| `catching_task` | An evaluator task marked to receive the dynamic or type errors raised above it: the machine unwinds to it instead of ending the evaluation (`try`/`catch`, D101) |
| `cdata_section` | A text node's request to be serialized as CDATA sections (XSLT 4.0, 11.4.3: the cdata attribute of xsl:text and xsl:value-of); a builder and serializer property, not part of XDM (`is_cdata_section`) |
| `cell` | A reference-counted, immutable heap object holding the data of an item or sequence that does not fit inline |
| `cell_class` | The static const operations of one kind of cell: its name, `release_contents` and, for sequence cells, the read interface (`src/xdm_internal.h`) |
| `chameleon_inclusion` | Including (or redefining, overriding) a schema document without a targetNamespace from one with a target namespace: its components take the includer's namespace (XSD 1.1 Part 1, 4.2.3) |
| `character_context` | Where characters are written: markup, text, an attribute value, a URI attribute value or CDATA sections; decides escaping and mapping |
| `character_encoding` | How a document's characters are stored as bytes: UTF-8, UTF-16 (little or big endian), ISO-8859-1 or US-ASCII (`sxml_xml_encoding`, XML 1.0, section 4.3.3) |
| `character_map` | The pairs of the `use-character-maps` parameter: characters and the text written in their place (Serialization 4.0, section 12) |
| `character_mapping` | One pair of a character map: a codepoint and its `replacement_text` |
| `character_pipeline` | What text goes through on its way out: character maps, Unicode normalization, escaping, line endings, encoding (Serialization 4.0, section 4) |
| `character_reference` | `&#N;` or `&#xH;` in an XQuery string literal, BracedURILiteral or direct constructor text, replaced by its character, which must be an XML 1.0 character (XQuery 4.0, 4.3.1.2; XQST0090) |
| `character_reference_name` | An HTML named character reference without "&" and ";" (`nbsp`, `NotEqualTilde`), an argument of `fn:char` (HTML, section 13.5) |
| `character_repetition` | A greedy unbounded repetition (`x*`, `x+`, `x{n,}`) of one regex instruction that reads one character, run as one instruction: it reads as many characters as match and gives them back one at a time on backtracking, two backtrack entries in all instead of two per character (internal, `src/regex.c`, D310) |
| `checking_builder` | The tree builder a validating parse sends its events to besides the parse's own builder: a schema validator whose result builder is the null builder |
| `child_index` | Position of a child among the children of its parent, from 0 |
| `child_slot` | One place in the content of a schema element role: the roles allowed there, once or any number of times, required or not; the children of an element fill its role's slots in order, as the schema for schema documents allows |
| `choice_item_type` | An item type matching items of any of its alternatives (`(A \| B)`) |
| `choice_operand_group` | Operands of which at most one is evaluated (the branches of a conditional), which may each consume a streamed input (XSLT 4.0, 19.8) |
| `chunk_source` | An iterable of byte or text chunks a JavaScript resolver answers an XML document with (`ChunkSource`), read through the host shim's byte reader as the document is parsed (D294) |
| `civil_timezone` | The timezone offset in conventional use at a place and time (F&O 4.0, fn:civil-timezone) |
| `clause_segment` | The FLWOR clauses between two tuple stream clauses, lowered as nested MAP, LET and CONDITIONAL without the tuple stream as a value |
| `cldr_file` | A CLDR XML file read into a table of its leaf elements by distinguished path, with its aliases (src/cldr_internal.h; D221) |
| `cli` | The command-line interface (`apps/`, docs/CLI.md); allowed short form, as the `sxml_cli_` prefix of application code only |
| `cli_invocation` | What a command function receives: program, command, parsed arguments, writers |
| `clock` | Host interface that reads the current date and time and the implicit timezone |
| `clock_reading` | What one reading of a clock gives: the current dateTime and the implicit timezone |
| `codepoint` | A Unicode scalar value, held in `uint32_t` |
| `codepoint_class` | The distinct terminals a codepoint matches, as a bit set; codepoints of one class are alike to the Earley parser. The classes of ASCII are computed once per grammar, the others when the input has them (D283) |
| `codepoint_count` | A length measured in Unicode codepoints |
| `codepoint_iterator` | Walks the codepoints of a string in order |
| `coercion` | The XPath 4.0 coercion rules (section 3.4) applied to a value and a sequence type; `coercion_role` decides the error code |
| `coercion_frame` | An entry on the explicit stack of applying the coercion rules (XPath 4.0, section 3.4) to a value: a sequence, an item, a choice being tried, or the parts of an array, map or record |
| `collation` | Rules for comparing strings, named by a URI; Phase 1 knows only the Unicode codepoint collation (F&O 4.0, section 5.3) |
| `collation_binding` | A host's collation URI standing for a collation sxml implements, added to the statically known collations |
| `collation_element` | The weights one codepoint gives a collation: primary, secondary, tertiary and quaternary, zero where it is ignorable (UTS #10, section 3.2) |
| `collation_element_table` | The mapping of code points and contractions to collation elements, the DUCET or the CLDR root collation (UTS #10, section 3.6) |
| `collation_key` | Bytes whose octet order is the order of strings under a collation (F&O 4.0, `fn:collation-key`) |
| `collation_match` | The bytes of a string that equal a search string under a collation, starting and ending at collation unit boundaries |
| `collation_strength` | How many levels of collation elements a UCA collation compares, primary to identical (UTS #10; the `strength` parameter) |
| `collation_unit` | A collation element that is not ignorable on every compared level; substring matching under a collation compares these (F&O 4.0, section 5.4) |
| `collection` | The sequence of items `fn:collection` returns for a URI |
| `column_number` | Column of a source position in codepoints, from 1; 0 means unknown |
| `combining_class` | The Canonical_Combining_Class of a codepoint, 0 to 254; 0 is a starter (UAX #15) |
| `command` | One verb of a program (`sxml version`) with its options, positionals and function |
| `comparator` | A function item of `fn:sort-with` that orders two items by returning a negative, zero or positive integer |
| `comparison_operator` | What a value or general comparison tests: equal, not equal, less, less or equal, greater, greater or equal |
| `comparison_pair` | Two items, or two runs of children, waiting to be compared on the explicit stack of deep equality |
| `comparison_unit` | One significant child (a node or a merged text run), or one top-level item, as deep equality compares it |
| `compatibility_warning` | A report of a Schematron compile report sink with the code `schematron:compatibility`: a known deviation of real rule sets from ISO/IEC 19757-3:2025 that sxml reads as its authors mean it (a parameter named twice in one pattern instance, a pattern variable read outside its pattern, a rule's let after its assertions), with the clause it deviates from; no error, never in the error record (D331) |
| `compile_report_sink` | The schema report sink of a compilation (`sxml_schema_report_sink`): receives every static and type error of an XSD schema, an XSLT stylesheet or a Schematron schema, in the order found, while compilation goes on; the null sink stops at the first error (D315, D320) |
| `compile_stage` | One step of schema compilation, a function of its own file: read, resolve, simple types, complex types, identity constraints, assertions (D167) |
| `component` | A stylesheet function, named template, attribute set, global variable or parameter, mode, or named item or record type, as packages bind references to it (XSLT 4.0, 3.4.5) |
| `component_instance` | One component as one package contains it, with its visibility there: declared in the package, accepted from a used package, or overriding one (XSLT 4.0, 3.4.5, the "containing package") |
| `component_lowering` | The stylesheet functions and attribute sets of one stylesheet lowering, with the function table each static context gets (D150) |
| `component_specifier` | The letter that starts a variable marker and names the component: `Y`, `M`, `D`, `d`, `F`, `W`, `w`, `H`, `h`, `P`, `m`, `s`, `f`, `Z`, `z`, `C`, `E` |
| `composing_character` | A codepoint of combining class above 0, or the second codepoint of the canonical decomposition of a character not in CompositionExclusions.txt (F&O 4.0, `fn:normalize-unicode`) |
| `composite_cast` | A cast to an array, map or record type (XPath 4.0, sections 4.19.2.4 to 4.19.2.7): the coercion frames run with the cast rules, so an item that does not match is cast to the item type, and a record keeps only its fields |
| `composite_sort_key` | A sort key whose value is a sequence of atomic items compared item by item, the shorter first when one starts the other (XSLT 4.0, 13.1.2) |
| `compositor` | How a model group combines its particles: sequence, choice or all |
| `computation_order` | Before D234, the order in which the global part computed declared variables (by initializer index); globals are now computed on first read (`global_read`) |
| `computed_constructor` | Node constructor written with a keyword: `element #a { ... }` |
| `concrete_syntax_tree` | The tree-sitter parse tree of a query or document, before sxml builds its own structures |
| `condition_closure` | The function item a window's start or end condition is lowered to: its parameters are the condition's window variables, its body the condition (D137) |
| `conditional_content` | A sequence constructor with `xsl:on-empty` or `xsl:on-non-empty` children, whose value depends on whether the other children's value is vacuous (XSLT 4.0, 8.5) |
| `conditional_inclusion` | Ignoring an element of a schema document with its attributes and descendants, as its vc: attributes say (vc:minVersion, vc:maxVersion, vc:typeAvailable, ...), before it is read (XSD 1.1 Part 1, 4.2.2) |
| `conditional_section` | An `<![INCLUDE[ ... ]]>` or `<![IGNORE[ ... ]]>` section of external DTD text; its keyword may be a parameter-entity reference (XML 1.0, section 3.4) |
| `configuration` | Struct describing how an object is set up |
| `conjunct_break` | The Indic_Conjunct_Break property value of a codepoint (None, Linker, Consonant, Extend), for rule GB9c of UAX #29 |
| `constant` | A value made at compile time and kept by a unit |
| `constraint_name` | The name XSD 1.1 gives a rule an error breaks (`cvc-complex-type.2.4`, `src-resolve`; Part 1, appendix C "Outcome Tabulations"): the local name, in no namespace, of the error code schema errors carry |
| `construction_flags` | The bits of an IR constructor that carry the construction and copy-namespaces modes and XSLT's content rules; 0 is XQuery's defaults (`sxml_ir_construction_flag`, D110) |
| `construction_mode` | `strip` or `preserve` (XQuery 4.0 construction declaration): whether constructed and copied nodes become untyped or keep their types |
| `content_frame` | An entry of a node constructor's explicit stack of arrays whose members are being added to a content sequence (D71) |
| `content_key` | The key of an element's simple content or nilled marker in an element map (`#content` by default, `#` prepended against conflicts) |
| `content_let` | A LET whose variable only the content of its block reads, in order: in a delivering task its bound expression delivers too, and the variable holds the empty sequence (D166) |
| `content_model` | The compiled form of a complex type's particle: a deterministic automaton over element names whose transitions are element declarations and schema wildcards, with counters for large maxOccurs (D167) |
| `content_model_counter` | A counter of a content model that counts the repetitions of one particle between its minimum and maximum occurrences, instead of a copy of states per repetition |
| `content_model_cursor` | Where matching an element's children has reached in its content model: a state and the counter values |
| `content_model_position` | One occurrence of an element or wildcard particle in the particle tree as a content model compiles it (small repetitions expanded into copies): a state of the Glushkov automaton before determinization |
| `content_model_transition` | A move of a content model state on an element declaration (with its substitutable members) or a schema wildcard, with counter tests and counter operations |
| `content_recovery` | What the validator does after a child element its parent's content model rejects (cvc-complex-type.2.4): it moves to the nearest state the child is accepted from, as if the elements before it were missing, or else skips the child, so that the rest of the content is still assessed and its errors reported (D277) |
| `content_sequence` | The nodes an element or document constructor adds to its new node: the value of its content with arrays flattened, adjacent atomic items made into one text node and document nodes replaced by their children (XQuery 4.0, "Content"; XSLT 4.0, 5.7.1) |
| `content_specification` | What an element type declaration allows as content: EMPTY, ANY, mixed content or a content model of element types (XML 1.0, [46] contentspec) |
| `content_type_meta` | The meta element the XHTML and HTML methods add as the first child of head (parameter `include-content-type`) |
| `content_type_variety` | What a complex type allows as content: empty, simple, element-only or mixed (XSD 1.1 Part 1, 3.4.1, {content type} {variety}) |
| `context_function` | An XSLT function whose value an enclosing instruction sets lexically: fn:current-group, fn:current-grouping-key, fn:current-merge-group, fn:current-merge-key; lowering reads a variable of the same local name in the XSLT namespace, absent when none is in scope (D152) |
| `context_global` | The global variable of a unit with name 0 that holds the initial context value |
| `context_item` | The item the focus is on |
| `context_position` | The position part of the focus (`fn:position`) |
| `context_size` | The size part of the focus (`fn:last`) |
| `context_value` | The XPath 4.0 context value: the sequence the focus is on; when it is one item, the context item |
| `context_variable` | The variable Q{XSLT}N an XSLT instruction puts in scope for a context function, read by a parameter default (`SXML_PARAMETER_DEFAULT_CONTEXT_VARIABLE`) named N; in a template function, where none is in scope, the entry N of the tunnel parameters it received (its received context variables, scope entry Q{XSLT}context-variables, D262); else the empty sequence (D153) |
| `continuation_byte` | A later byte of the UTF-8 encoding of one codepoint, 0x80 to 0xBF |
| `conversion_plan` | The map that chooses element layouts and content types by element and attribute name (F&O 4.0, 14.6.5); one `plan_entry` per name, `*` the `fallback_layout` |
| `copy_namespaces_mode` | `preserve` or `no-preserve`, `inherit` or `no-inherit` (XQuery 4.0 copy-namespaces declaration): which in-scope namespaces a copied element keeps and whether it takes those of its new parent |
| `counted_loop` | A counted repetition `x{n,m}` compiled as `x` once plus a counter slot that counts its iterations, instead of copies of `x`; used when the copies would pass 256 instructions (D147) |
| `counter_operation` | What a content model transition does to a counter: set it to one (entering the counted particle) or add one (repeating it) |
| `counter_test` | A condition of a content model transition or final state: a content model counter's value lies in a range |
| `counting_allocator` | The allocator test double: counts allocations and fails a chosen one |
| `csv_cursor` | Where reading a CSV text continues: a byte offset and whether it is at the start of a row, after a field that ended its row, or at the end |
| `csv_dialect` | The characters that structure a CSV text, each one character: separator, quote character and optional comment marker, and whether unquoted fields are trimmed of whitespace (F&O 4.0, section 17.5) |
| `csv_token` | What reading a CSV text gives next: a field (its text, quoted or not), the end of a row (a row with no field before it is a blank row) or the end of the input |
| `curly_bracket` | `{` or `}`, the spec's name; not `brace` |
| `current_date_time` | The instant `fn:current-dateTime` returns, read once per evaluation |
| `current_item` | XSLT's current item (fn:current, XSLT 4.0, 20.3.1): the context value the outermost expression started with, the focus of its embedding; in a pattern, the item being matched (D149) |
| `current_mode` | The mode the running template was chosen in; `mode="#current"` (XSLT 4.0, 6.9.2) |
| `current_output_uri` | The URI of the result being written: the base output URI, or the absolute `href` of the `xsl:result-document` being evaluated (XSLT 4.0, 26.3) |
| `current_template_rule` | The template rule whose template is running, absent in a changed focus, functions and globals (XSLT 4.0, 6.11) |
| `data_element` | A top-level element outside the XSLT namespace, ignored by the processor (XSLT 4.0, 3.6.4) |
| `date_time` | The seven components of a date/time datum (XDM 4.0, section 5.4) |
| `datum` | The value of an atomic item, without its type annotation (XDM 4.0, section 5) |
| `datum_kind` | The C representation a datum is read and written in (boolean, integer, string, ...) |
| `decimal` | An `xs:decimal` value, or an `xs:integer` beyond `int64_t`, as a libmpdec number (`sxml_decimal`, `src/decimal_internal.h`; D36, D42) |
| `decimal_format` | A named or unnamed set of properties that `fn:format-number` formats with (XPath 4.0, section 2.2.1, "statically known decimal formats") |
| `decimal_format_property` | One property of a decimal format (decimal-separator, percent, NaN, ...), given as text |
| `decimal_format_settings` | A decimal format with every property decoded: markers as codepoints, renditions as UTF-8, the zero digit and the three plain renditions |
| `decimal_scope` | The thread-local allocator pointer libmpdec allocates through during one decimal operation (D42) |
| `declaration` | A top-level element of a stylesheet module (`xsl:template`, `xsl:function`, ...) |
| `declaration_order` | The order of the declarations of a stylesheet level, with included modules in place of their `xsl:include` |
| `declared_function` | A function declared in a prolog; one IR function of the unit per arity of its arity range |
| `declared_namespace_bindings` | The namespace bindings an element has itself, sent or added by namespace fixup, as opposed to those it inherits from its parent |
| `declared_streamable` | A construct the stylesheet declares streamable: a template rule of a streamable mode, xsl:source-document, xsl:attribute-set, xsl:accumulator or xsl:merge-source with streamable="yes", a stylesheet function with a streamability category (XSLT 4.0, 19.10) |
| `declared_type` | The sequence type a variable or parameter is declared with |
| `decoding_error_rule` | What the one decoder of UTF-8, UTF-16, ISO-8859-1 and US-ASCII octets does with octets that do not decode: fail (the XML parser; text decoding without fallback), or replace each maximal subpart by one U+FFFD and go on (text decoding with fallback; the HTML input stream) (`sxml_xml_decoder`; Unicode 16.0, 3.9.6; D249) |
| `decomposition` | The full canonical or full compatibility decomposition of a codepoint: its UnicodeData.txt mapping applied until nothing changes (UAX #15) |
| `deemed_empty` | Of an item, for `xsl:where-populated`: a document or element node without children, another node or atomic item of zero-length string value, an empty map, or an array whose flattened members all are (XSLT 4.0, 8.5.1) |
| `deep_equality` | `fn:deep-equal` of two sequences: items compared pairwise, into nodes, maps and arrays (F&O 4.0, section 14.2.4) |
| `default_allocator` | The allocator over the C library heap |
| `default_binding` | What an element or attribute is bound to when it is valid: a declaration, an attribute use, or strict, lax or skip; restriction compares them (XSD 1.1 Part 1, 3.4.6.4) |
| `default_collation` | The collation used when a function or operator is given none (XPath 4.0, section 2.2.2) |
| `default_collection` | The collection (or URI collection) for a call without a URI (XPath 4.0, section 2.2.2) |
| `default_element_namespace` | The namespace of unprefixed element and type names in the static context |
| `default_function` | The IR function that computes the default expression of one optional parameter of a declared XQuery function; its parameters are the caller's context value, position and size, of which it reads those its IR function's focus names (`SXML_PARAMETER_DEFAULT_EXPRESSION`, XQuery 4.0, 5.18.3) |
| `default_global` | The hidden global, named Q{urn:sxml:default}dN, whose initializer computes a declared function's default value that reads no focus, once per evaluation; calls of the default function read it instead (D287) |
| `default_language` | The language of the dynamic context, for `fn:default-language` and the formatting functions |
| `default_priority` | The priority of a template rule without a priority attribute, computed from the form of its pattern (XSLT 4.0, 6.4) |
| `deferred_check` | An assertion or identity-constraint check of an element that reads its recorded subtree, queued by the schema validator and run when the outermost recorded element ends |
| `delivering_task` | An evaluator task in content position: when it finishes, its value is added to the construction of the receiving task below instead of being handed to the task below, and a loop's body or a call's body in it delivers each value at once (D166) |
| `derivation_frame` | A union base type on the explicit stack of a derivation question, whose members are tried in turn |
| `derivation_set` | A set of derivation methods (extension, restriction, list, union, substitution) as a bit set: XSD's {final}, {prohibited substitutions}, {disallowed substitutions}, {substitution group exclusions} |
| `descendant_path` | "E//T[P]" rewritten as "E/descendant::T[P]" by the descendant pass when the step is to the children and every predicate selects by the item alone: one FILTER over the descendants instead of one per node (internal, `src/ir_loop_passes.c`, D259) |
| `descending_integer_range` | A sequence representation holding an `integer_range` whose items run from `last_integer` down to `first_integer` (`fn:reverse` of a range, D143) |
| `destructuring_let` | A let binding of several variables to the parts of one value: `$( ... )` items of a sequence, `$[ ... ]` members of an array, `${ ... }` entries of a map by name (XPath 4.0, section 4.13.2) |
| `device_test` | A binding test run on an Android emulator or device, or an iOS simulator or device, rather than on the build host (Phase 6 gate) |
| `diagnostic` | Caller-supplied record receiving position and context of a failure |
| `digest` | The octets a hash algorithm returns for a message: 16 (MD5), 20 (SHA-1), 32 (SHA-256, BLAKE3) or 4 (CRC-32, big-endian) |
| `digit_count` | A number of decimal digits (`maximum_digit_count`, `division_digit_count`) |
| `digit_family` | Ten consecutive Unicode characters of category Nd with digit values 0 to 9; named by its zero digit |
| `digit_pattern` | A primary format token of mandatory digit signs, optional digit signs (`#`) and grouping separators |
| `direct_constructor` | XQuery XML-like node constructor syntax: `<a b="{1}">text</a>`, `<!-- -->`, `<?pi?>` |
| `directory_collection` | The `sxml` command's collection of a local directory (D298): the XML documents (`fn:collection`) or the URIs (`fn:uri-collection`) of its files that a collection URI's `select`, `recurse` and `on-error` parameters choose, sorted by path |
| `disable_output_escaping` | A text node's request to be serialized unescaped (XSLT 4.0, 26.1.8: the disable-output-escaping attribute of xsl:text and xsl:value-of), ignored in temporary output state; a builder and serializer property, not part of XDM (`disables_output_escaping`, D188) |
| `disjoint_matching_segments` | The matching segments left after removing those that overlap an earlier one; what `fn:tokenize` and `fn:replace` work on (F&O 4.0, section 6.1.1) |
| `distinct_terminal` | One of the different terminals of an ixml grammar: the same character set written twice, such as the `a` of two strings, is one distinct terminal (D283) |
| `distinguished_path` | The path of an LDML element from below the root, each step with its distinguishing attributes, as `dates/calendars/calendar[@type='gregorian']/months` (UTS #35, Part 1, section 5.1.1) |
| `document_loader` | Internal interface through which fn:doc and fn:parse-xml obtain document nodes; the processor implements it |
| `document_order` | The total order of nodes defined by XDM |
| `document_preparation` | A step the document loader applies to each document it gets from the resolver before keeping it: XSLT's whitespace stripping (D150) |
| `document_segment` | A part of a document's text that the XML parser parses, walks and frees on its own, so that the concrete syntax tree of one segment at a time exists: the head (the prolog and the start tag of the document element) or a content segment (a run of content items cut between two of them) (D222) |
| `document_uri` | `dm:document-uri` of a document node |
| `dot_segment` | A `.` or `..` path segment, removed by reference resolution (RFC 3986, section 5.2.4) |
| `dotted_rule` | A production of an ixml grammar with a dot that says how much of it an Earley item has read; numbered so that the dotted rules of one production are consecutive, and advancing an item adds one (D283) |
| `dtd_validator` | The validator of the XML parser's DTD validation that sxml provides (`sxml_dtd_validator`): it compiles the schema documents the parser writes from the DTD and checks the document's events with a schema validator (D199) |
| `duplicate_attribute_policy` | What a tree builder does with a second attribute of the same name: reject (XQuery) or replace (XSLT) |
| `duplicate_key_policy` | What building a map does with keys that are atomic-equal: reject, keep first, keep last |
| `duplicates_policy` | The `duplicates` option of `map:build` and `map:merge`: reject, use-first, use-last, use-any, combine, or a function |
| `duration` | A duration datum: months plus seconds and nanoseconds (XDM 4.0, section 5.5) |
| `dynamic_attribute_value` | A flag, role or severity attribute whose value is a variable reference (5.5.6, 5.5.14, 5.5.16) or, under an xslt query binding, an attribute value template (Annexes C, H, J): evaluated for each finding, its string given to the schematron report (D315) |
| `dynamic_context` | XPath dynamic context |
| `dynamic_evaluation` | Compiling and evaluating an XPath expression whose text is computed at run time: xsl:evaluate (XSLT 4.0, 10.5), run as a unit evaluation |
| `dynamic_node_test` | A node test `{E}`: a node passes when its name, local name or ·jkey· is atomic-equal to an item of E's value (XPath 4.0, section 4.7.5) |
| `earley_item` | A state of the Earley parser: a production, the dot (how much of it is read), the input position it started at, and the first way it was derived (its predecessor and child items; D215) |
| `eclipsed` | Said of an `xsl:function` overridden by one of the same name, higher import precedence and an arity range holding all of its own (XSLT 4.0, 10.3.4.1) |
| `edit_distance` | Optimal string alignment distance between two names, ASCII case ignored |
| `effective_boolean_value` | The boolean value of a sequence (XPath 4.0, section 2.5.4) |
| `effective_child` | A child of a Schematron element after inclusion: an sch:include is replaced by the element it names, an sch:extends href by the children of its external declaration, and (when rules are expanded) an sch:extends rule by the children of the abstract rule; a cursor yields them in order with an explicit stack (D323) |
| `effective_facets` | The facets of a simple type with those of its bases it does not overlay, and those built-in types have implicitly, one per kind; what a restriction's own facets are checked against (XSD 1.1 Part 1, 3.16.6.4; D171) |
| `element_declaration` | The schema component that governs elements of one name: type, nillable, abstract, value constraint, substitution group, identity constraints, type alternatives (XSD 1.1 Part 1, 3.3) |
| `element_layout` | How `fn:element-to-map` represents one element (F&O 4.0, 14.6.1): `empty`, `empty-plus`, `simple`, `simple-plus`, `list`, `list-plus`, `record`, `sequence`, `mixed`, `xml`, plus the plan-only `deep-skip` and `error` |
| `element_slot` | The work record `fn:element-to-map` keeps per node of the converted tree: chosen layout and the value built, filled in reverse document order (D71) |
| `element_type` | An element name of a DTD with its element type declaration and attribute-list declarations (XML 1.0, section 3) |
| `embedding` | One XPath AST lowered inside a caller's scope: its variables, a focus of the caller's choice and the AST's own static context (D103) |
| `emptiness` | Whether a sequence has no items, as tested by `sxml_sequence_test_emptiness` (`fn:empty`) |
| `enclosing_mode` | An `xsl:mode` with `xsl:template` children, the only rules of that mode (XSLT 4.0, 6.9.6) |
| `encoded_byte_count` | The number of bytes in the UTF-8 encoding of one codepoint, 1 to 4 |
| `encoding_label` | A name the Encoding Standard gives an encoding (`latin1`, `windows-1251`), matched without case and surrounding whitespace |
| `encoding_name` | The name of a character encoding (`UTF-8`, `ISO-8859-1`), from outside a document or from its encoding declaration |
| `entity_base_uri` | The base URI an element has because it stands at the top of an external parsed entity: the entity's URI, in place of the tree's base URI for it and its descendants before xml:base applies (XML 1.0, section 4.2.2; D210) |
| `entity_expansion` | The text that replacing entity references produces (XML 1.0, section 4.4); bounded for untrusted input |
| `entity_frame` | One level of nested entity expansion in the XML parser: the replacement text being walked and the entity it belongs to |
| `entity_resolver` | The host's interface through which the XML parser retrieves external entities (`sxml_xml_entity_resolver`); the null resolver reads nothing outside the input |
| `entry_block` | A map made from an entry list: its entries in entry order in one cell, with a hash index (D61) |
| `entry_index` | Position of an entry in a map's entry order, from 0 |
| `entry_stamp` | The number a persistent map gives a key when it enters; entry order is stamp order |
| `entry_tree` | The balanced tree of a persistent map's entries ordered by entry stamp, counting entries per subtree so entries are found by entry index |
| `enumeration_type` | An item type matching a fixed set of strings (`enum("a", "b")`) |
| `environment_variable` | A name and value the host exposes to `fn:environment-variable`; sxml never reads the platform's environment |
| `equivalent_expression` | The XPath expression of the same form as an XNode pattern, with its leading steps' axes adjusted, that defines what it matches (XSLT 4.0, 6.3.2.2) |
| `error_code` | The expanded QName of a specification error, such as `err:FOAR0001` |
| `error_collector` | What a stylesheet compilation with a compile report sink writes each error into and asks whether to go on: the reader at the next attribute or element, lowering at the next template rule, stylesheet function or global variable (internal, `sxml_xslt_error_collector`, D320) |
| `error_details` | What a caught error binds in a catch clause, copied out of the error record: code, description, value, module, line and column number; bound as the map `$err:map` (XQuery 4.0, "Try/Catch Expressions"; D109) |
| `error_finding` | A finding whose schematron severity is `fatal` or `error`; without a severity, one whose role or a flag token is `fatal` or `error`, or that has neither role nor flag: it makes the validation fail (`is_error`, D313, D315) |
| `error_record` | A specification error code (such as `FOAR0001`) with details |
| `error_status` | The status an error is raised with: static, dynamic or type error |
| `error_value` | The optional sequence that an error carries (`$err:value`) |
| `escape_style` | How the character pipeline escapes text for a method: XML, Canonical XML, HTML, none (text), JSON strings or CSV fields |
| `escaped_form` | Text whose special characters are kept as JSON escape sequences (option `escape`; attributes `escaped`, `escaped-key`) |
| `evaluation` | One run of a compiled expression or query against a dynamic context |
| `evaluation_depth` | How deeply evaluation nests (function calls, nested expressions); bounded so deep recursion is a limit, not a stack overflow |
| `evaluation_environment` | What the library functions of one evaluation read from the host beyond fn:doc: the resolver for text, binary and collection resources, the trace sink, the default language, environment variables, and the static context's decimal formats and collations; carried by the processor's document loader |
| `evaluation_memo` | Values a native body computes once per evaluation and subject node (an xsl:key's index for one tree), kept in the dynamic context until the evaluation ends (D149) |
| `evaluation_step` | One unit of evaluator work counted against `maximum_evaluation_step_count`: a task the machine runs, or an item pair a general comparison reads |
| `evaluator` | Walks the IR of a sealed unit and computes values |
| `evaluator_part` | One `src/evaluator_<part>.c` file defining `sxml_evaluator_part_<part>`: the IR expression kinds it evaluates and their step functions (D101) |
| `evaluator_task` | One entry of the evaluator's task stack: an expression, call or native step being evaluated, with the state that resumes it when a nested value arrives (D71, D75) |
| `event` | One step given to a tree builder: start element, attribute, text, ... |
| `exact_text` | The canonical decimal text of the exact value of a double, as compared with decimals |
| `excluded_namespace` | A namespace whose namespace nodes literal result elements do not copy (XSLT 4.0, 11.1.3) |
| `exit_code` | The process exit status: success 0, operation failed 1, usage error 2, input/output error 3, internal error 4 |
| `expanded_name` | Namespace URI plus local name |
| `expansion_segment` | A run of an expression's text after placeholders are replaced that comes from one place, the attribute or a sch:param's value, so that a syntax error points into that place (internal, D315) |
| `expected_term` | A term a content model cursor would accept next, for error messages ("expected 'shipTo'") |
| `expected_validity` | The outcome an XSD test prescribes: `valid` or `invalid` are in scope; `notKnown`, `indeterminate`, `runtime-schema-error` and the implementation-defined values are not |
| `explicit_content` | The particle a complex type definition's XML representation gives itself, before the complex types stage combines it with the base type's (XSD 1.1 Part 1, 3.4.2.3.3) |
| `expression_depth` | How deeply the syntax of an expression nests; bounded for untrusted expressions |
| `expression_rule_context` | A rule's context that is no XSLT pattern but a valid XPath expression (`(//item)[1]`, `../x`): it matches the nodes the expression selects from the start nodes (the instance root, a phase's from nodes, a subordinate document's root), compiled as the predicate pattern `.[. intersect $s:context-K]` over a global holding the expression's value (D315) |
| `expression_text` | The source text of an expression or query |
| `extension_instruction` | An element in an extension namespace inside a sequence constructor; one named after a named template calls it, its attributes giving the parameters (XSLT 4.0, 10.1.3); sxml knows no others and runs their fallback |
| `extension_namespace` | A namespace whose elements are extension instructions (XSLT 4.0, 24.3) |
| `external_context` | DTD text read as the external subset is: the external subset, external parameter entities and the text expanded or included from them, where parameter-entity references may stand inside declarations (XML 1.0, section 2.8) |
| `external_declaration` | The element a sch:extends href names (5.4.4): a sch:schema or schematron library under a schema or library, a sch:rule under a rule; its children are put in place of the sch:extends, its attributes are not (D315) |
| `external_entity` | An entity declared with a system identifier: the external subset, an external parameter entity or an external parsed general entity, read only through an entity resolver (XML 1.0, section 4.2.2) |
| `external_subset` | The declarations a document type declaration names by its external identifier, read after the internal subset (XML 1.0, sections 2.8, [30]) |
| `facet` | One constraining facet of a simple type's restriction step (XSD 1.1 Part 2, 4.3): its `facet_kind` (length, pattern, enumeration, minInclusive, ...) and its value as text |
| `fallback_marker` | Text such as `[Calendar: AD]` or `[Language: en]` put before a formatted date when sxml uses another calendar or language than the one asked for |
| `feature_data` | Large data an optional feature reads through the processor's resource resolver, in the data's standard format (DUCET `allkeys.txt`, CLDR rule text, TZif files), at first use, and keeps as its state in the processor (`sxml_optional_feature_read_data`; D184, D189) |
| `field_index` | Position of a field in a record type, from 0 |
| `field_key` | The xs:string key item of one record field, made by a record on first need |
| `field_name` | The name a grammar gives a child of a syntax node by its role, such as `name` or `body`; the `field` attribute of its element |
| `file_catalog` | The QT4 catalog the XSLT driver never loads, over the same suite files: what the QT4 resource table and assertions read files through (D157) |
| `file_description` | What a file system tells about one path: whether it is a file, a directory or absent, its size and its last modification time (`sxml_file_description`; EXPath File 4.0, 3) |
| `file_reader` | Interface through which a command of the sxml program reads a named file or standard input (`sxml_cli_file_reader`) |
| `file_system` | Host interface through which the EXPath File functions reach files and directories: describe, read, write, list, create, delete, move and temporary names, on absolute normalized paths, with the host's separators, current directory and temporary directory (`sxml_file_system`; EXPath File 4.0; D184, D200) |
| `file_writer` | Interface through which a command of the sxml program writes the files its command line names: each write goes to a temporary file beside the target and is committed (moved into place in one step) or abandoned (`sxml_cli_file_writer`, `sxml_cli_file_write`; D217) |
| `filter_index` | An index of a FILTER's input by the `index_key` of a comparison its predicate needs to be true ("=", "eq", "is"), kept in the evaluation memo, so the predicate is tested only on the items whose key compares with the `index_probe` (D224) |
| `finding` | A failed assert or a successful report for one context item: what a Schematron validation finds (D315) |
| `finding_item` | The array a compiled schematron rule adds to the transformation's raw result for one finding or fired rule: its assertion's or rule's index and the subject node, then the texts of the dynamic attribute values, the message, diagnostics and properties; `src/schematron.c` turns each into a schematron report (internal, D315) |
| `fired_rule` | A schematron rule applied to a node it matched (in a schematron pattern only the first matching rule is); reported only when the host asks, as SVRL does (D315) |
| `fixed_namespace_bindings` | The namespace bindings that the `fixed-namespaces` attribute sets for every expression of a stylesheet module (XSLT 4.0, 3.6.1) |
| `fixed_part` | Text of a value template outside its variable parts, with `{{` and `}}` for single curly brackets (XSLT 4.0, 5.6); a string template's are `string_template_fixed_part` |
| `fixpoint` | Of a fold: an accumulator that its function is certain to return unchanged for every further item, which ends the fold early |
| `flat_node` | An array node that holds its members inline, as `sxml_array_make` builds them |
| `focus` | Context item, context position and context size together |
| `focus_constructor` | A constructor function call or reference that leaves its argument out and takes the context value: `xs:int()`, `xs:int#0` (XPath 4.0, section 4.5.1.3) |
| `format_modifier` | The part of a `fn:format-integer` picture after its last semicolon: `c` or `o` with an optional variant in parentheses, then `a` or `t` |
| `formatting_elements` | The HTML parser's list of active formatting elements and markers, which reconstruction reopens (13.2.4.3) |
| `forwards_compatible_mode` | Reading an element whose effective version is above 4.0: unknown XSLT elements and attributes are ignored or kept for fallback (XSLT 4.0, 3.9) |
| `foster_parenting` | Inserting content that is misnested in a table before the table instead (13.2.6.1) |
| `frame` | The slots of one active call of an IR function |
| `free_variable` | A variable an inline function body reads but does not bind; the function captures it |
| `front_end` | The code that turns the text of one language (XPath, XQuery, an XSLT pattern) into an abstract syntax tree |
| `full_module` | `sxml-full.wasm`, the JavaScript package's module that links every optional feature; `sxml.wasm`, the core module, links none (D189) |
| `full_text_score` | The relevance of a match, a number in [0, 1] computed from the matches and their weights (Full Text 3.0, 2.3; D214) |
| `full_text_selection` | The compiled form of an FTSelection: words, connectives, positional filters and match options, run as a match program (XQuery and XPath Full Text 3.0) |
| `function_annotation` | An annotation name with a sequence of atomic items (`%private`) |
| `function_body` | Interface that runs a function item: native C code or an evaluator for the IR |
| `function_call` | What a function body receives: the function item, the arguments, the dynamic context |
| `function_coercion` | Wrapping a function item so that its arguments and result are coerced to a required function type (XPath 4.0, section 3.4.4) |
| `function_entry` | What a function table finds: the target and its signature for one arity |
| `function_item` | A callable item, including maps, arrays and closures |
| `function_library` | The per-processor index over the native function parts, resolving their types |
| `function_parameter` | Name, declared type as text and default of one parameter of a function |
| `function_table` | Interface through which static analysis finds a function by name and arity |
| `function_target` | What a static call or function reference calls: a native function, an IR function or a constructor |
| `function_type` | The item type of function items with given parameter and result types; a function's signature |
| `fuzz_fixture` | The objects every fuzz target makes fresh for each input (counting allocator, name pool, type registry, item heap, function library) and checks for leaks after it |
| `fuzz_target` | A libFuzzer entry point (`tests/fuzz/<name>_fuzz.c`) that feeds generated bytes to one sxml entry point and stops on a crash, a leak or a broken invariant |
| `general_category` | The Unicode general category of a codepoint (Lu, Nd, ...), what a category escape `\p{..}` tests (XSD 1.1 Part 2, appendix G) |
| `general_comparison` | `=`, `!=`, `<`, `<=`, `>`, `>=`: true when some pair of atomized items has the relationship (XPath 4.0, section 4.11.2) |
| `general_entity` | An entity referenced as `&name;` (XML 1.0, section 4): internal (with replacement text), external parsed, or unparsed |
| `generated_page` | A Markdown file under `docs/` that `tools/spec/spec generate` writes from a document (its `file`); read and changed through `tools/spec/spec`, never edited |
| `generated_prefix` | A prefix namespace fixup makes up (`ns0`, `ns1`, ...) when a name's own prefix is taken by another namespace URI |
| `generic_node` | A GNode: a node (XNode) or a JNode (XDM 4.0, section 6) |
| `global_context_item` | The context item of global variables and parameters, one item or absent (XSLT 4.0, 2.4.2) |
| `global_number` | The number of a global variable of the unit (from 1); a static context maps each variable declaration it sees to one |
| `global_read` | An IR expression that computes its global on first need (the initializer of an ABSENT global), then evaluates its body; the global-read pass puts one at the head of each LET chain that reads a global with an initializer (D234) |
| `global_table` | The global variables of one unit in one evaluation, a slot per global number; the evaluation's own unit has the first, a unit evaluated on the same machine (fn:load-xquery-module) one more, kept until the evaluation ends |
| `global_variable` | A variable of a unit outside every function: external, or computed by an initializer function |
| `governing_type` | The type a validation root is assessed against when the caller names one (XQuery `validate type`, XSLT `type`); otherwise the declaration of its name decides (XSD 1.1 Part 1, "governing type definition", 3.3.4) |
| `grammar` | A tree-sitter grammar under `grammars/<language>/`, and the parser generated from it |
| `grammar_of_grammars` | The iXML grammar of iXML grammars, compiled into sxml as a constant table of its XML form; a grammar in ixml notation is parsed with it (D215) |
| `grapheme_break` | The Grapheme_Cluster_Break property value of a codepoint (CR, LF, Extend, ZWJ, L, V, ...; UAX #29, section 3.1) |
| `grapheme_cluster` | An extended grapheme cluster of UAX #29: one user-perceived character, one item of `fn:graphemes` |
| `gregorian_value` | A value of one of the eight Gregorian types: xs:dateTime, xs:date, xs:time, xs:gYear, xs:gYearMonth, xs:gMonth, xs:gMonthDay, xs:gDay (F&O 4.0, section 9.3) |
| `grounding_stream` | A STREAM with axis self: its body runs once on the full stream window of its origin when the origin is the open node, else on the origin; it guards a template rule that is not a streamed template rule (D228) |
| `group_array` | One group of xsl:for-each-group as the array [current group, current grouping key] (D152) |
| `group_frame` | The regex compiler's record of a group whose ")" has not been read (and of the whole regex), kept on an explicit stack instead of recursing (D71) |
| `group_name` | The name of a named capturing subexpression, `(?<name>...)`; `\k<name>` refers back to it and result maps key its group by it (F&O 4.0, section 6.1.5) |
| `group_number` | The number of a capturing subexpression: its capturing left parenthesis counted from 1, left to right |
| `grouping_key` | The atomized value of a grouping variable of a group by clause, empty or one atomic item; tuples with equivalent grouping keys form one group (XQuery 4.0, section 4.13.8) |
| `grouping_method` | How xsl:for-each-group forms groups: by key, by adjacent keys (each composite or not), starting or ending with a pattern (D152) |
| `grouping_pair` | The array [item, value] lowering makes for each item of a population: its grouping keys, or whether it matches the pattern (D152) |
| `grouping_position` | The number of digit signs between a grouping separator and the end of the integer part (or the decimal separator) |
| `growable_array` | An array in allocator memory that doubles its capacity when it must hold more, kept by its owner as elements, count and capacity and grown by the one helper `sxml_allocator_reserve_array` (D249) |
| `guaranteed_streamable` | A declared-streamable construct that the rules of XSLT 4.0, chapter 19 find streamable; one that is not is XTSE3430 |
| `handle` | The address of a C object in WebAssembly memory that a JavaScript object owns; released by `dispose()` or, failing that, by a `FinalizationRegistry` |
| `hash_algorithm` | One of the algorithms `fn:hash` names: MD5, SHA-1, SHA-256, BLAKE3, CRC-32 (F&O 4.0, fn:hash) |
| `hash_index` | An open-addressing table from key hash to entry index, built for an entry block |
| `highlights_query` | A tree-sitter query (the grammar's `queries/highlights.scm`) whose captures name what each node is for highlighting, such as `keyword` or `function.method` |
| `host_shim` | The C layer in `bindings/host/` that the JavaScript, Kotlin and Swift bindings share: handles, flat requests, one error per processor, side results and JSON-shaped input, over the public API only, with the resolver as a state and function (D238) |
| `html_element` | An element the XHTML method recognizes as HTML: in the XHTML namespace, or with HTML5 in no namespace with an HTML5 element name |
| `html_working_tree` | The HTML parser's mutable node table during tree construction (adoption agency, foster parenting), sent to the tree builder once the document ends |
| `id_table` | The IDs and IDREFs a schema validator has met under one validation root, checked at its end (cvc-id) |
| `identity_constraint` | A unique, key or keyref definition: a name, a selector and fields as XPath records, and the referenced key of a keyref (XSD 1.1 Part 1, 3.11) |
| `ideographic_numerals` | The Japanese numbering by ideographs that the token `一` (U+4E00) asks for: digits `一` to `九`, multipliers `十 百 千` within each group of four digits, myriad units `万 億 兆 京`; no zero digit and no `一` before `十 百 千` (`百五十一` is 151, `一万` 10,000) |
| `idle_matcher` | A regex matcher a regex cache entry keeps, reset, while no call uses it, so the next call that finds the regex in the cache needs no new matcher (internal, `src/function_library_regex.c`, D310) |
| `ietf_date` | A date and time in the formats of RFC 822, RFC 850 and asctime that fn:parse-ietf-date reads |
| `immortal` | A cell or tree that is never released (static data); retain and release do nothing to it |
| `implicit_timezone` | The timezone of the dynamic context, used for date/time values without one |
| `import_precedence` | The rank of a stylesheet level: an importing level above what it imports, a later import above an earlier one (post-order of the import tree, XSLT 4.0, 3.10.3) |
| `in_scope_namespaces` | The namespace bindings in effect on an element (XDM 4.0, `dm:namespace-nodes`) |
| `in_scope_schemas` | The compiled schemas a static context imports: their types, element and attribute declarations are XPath 4.0's in-scope schema definitions (section 2.2.1) |
| `inclusion_fixup` | The `xml:base` and `xml:lang` attributes a top element of an included document gets where its base URI or language differs from the include's parent (XInclude 1.0, 4.5.5 and 4.5.6) |
| `incremental_parse` | A parse of the XML parser that stops between events when its builder asks for a pause and goes on when it is resumed (`sxml_xml_incremental_parse`, D219) |
| `index_key` | The operand of a filter index's comparison that is computed from the focus value alone; the IR holds copies of its expressions |
| `index_probe` | The other operand of a filter index's comparison, computed once per evaluation of the FILTER without the focus |
| `inherited_attribute` | An attribute of an ancestor attributed to an attribute use, or governed by a declaration, whose {inheritable} is true: type alternatives see it as the element's own when the element has none of that name (XSD 1.1 Part 1, 3.3.5.6) |
| `initial_context_value` | The context value of the query body and of every variable initializer: a context value declaration's value, else the host's (XQuery 4.0, 5.17) |
| `initial_function` | The stylesheet function a function call invocation calls (XSLT 4.0, 2.4.5) |
| `initial_match_selection` | The value an apply-templates invocation processes (XSLT 4.0, 2.4.3) |
| `initial_mode` | The mode an apply-templates invocation selects template rules in (XSLT 4.0, 2.4.3) |
| `initial_template` | The named template a call-template invocation evaluates, `xsl:initial-template` by default (the spec's "initial named template", XSLT 4.0, 2.4.4) |
| `injection` | A node's text parsed with another source code language, as its injections query says: its tree is written inside the node's element, its document element with a `language` attribute (D288) |
| `injection_regex` | A regex (tree-sitter.json's `injection-regex`) that the language names of injections match to mean a source code language, such as `^(js\|javascript)$` (D288) |
| `injections_query` | A tree-sitter query (the grammar's `queries/injections.scm`) whose captures `injection.content` and `injection.language` mark nodes whose text is code in another language (D288) |
| `input_output` | Reading or writing bytes through the platform; spelled `input_output`, never `io` (`SXML_STATUS_INPUT_OUTPUT_FAILED`) |
| `input_stream` | The decoded characters the HTML tokenizer reads, newlines normalized (13.2.3.5); UTF-8 in sxml |
| `insertion_mode` | The state of HTML tree construction that decides how a token is handled (HTML Living Standard, 13.2.4.1) |
| `insertion_sequence` | The nodes an insert or replace expression copies in, made from its source as element content: atomic runs become text, documents their children (XQuery Update Facility 3.0, 5.4.1) |
| `instance_test` | An XSD test that validates an instance document against its test group's schema: valid means the validation root is valid |
| `instruction` | An XSLT element that may appear in a sequence constructor (`xsl:if`, `xsl:copy`, ...) |
| `instruction_rule` | Which lowering procedure lowers one kind of XSLT instruction (`sxml_xslt_instruction_rule`); each XSLT lowering file lists its own |
| `integer_range` | The bounds of a range sequence (`first_integer` to `last_integer`) |
| `integration_point` | A MathML or SVG element whose content is parsed as HTML (an HTML integration point) or whose text is (a MathML text integration point; 13.2.6) |
| `intermediate_representation` | The shared IR; `ir` allowed as a module name only |
| `internal_subset` | The declarations between `[` and `]` of a document type declaration (XML 1.0, section 2.8) |
| `invocation` | How the main function starts a transformation: a template to call, or templates applied to the global context item in an initial mode (XSLT 4.0, 2.4) |
| `invocation_global` | A global of a stylesheet's IR unit through which the host supplies an invocation's run-time values (the template and tunnel parameter maps, the initial match selection, the initial function's arguments), bound by name as stylesheet parameters are (D157) |
| `invocation_kind` | How a transformation starts: apply-templates, call-template or a function call (XSLT 4.0, 2.4.3 to 2.4.5) |
| `invocation_value` | The value src/xslt.c binds to an invocation global for one transformation (D157) |
| `ir_module` | A static context a unit keeps for code that reads it when it runs: an XQuery library module whose functions the unit holds (its source name and static base URI, D148), or an XSLT element's (base URI, namespaces, default collation, its package's decimal formats, D163), named by a function's module_number or by a STATIC_CALL or node constructor (`sxml_ir_module`) |
| `is_ascii` | The flag of a string whose bytes are all below 0x80 (D6) |
| `is_derived_from` | Schema type derivation: the base type chain reaches the other type (XSD 1.1 Part 1, section 3.16.6) |
| `item` | One XDM item: a node, an atomic item, or a function item (maps and arrays are function items) |
| `item_axis` | The axis `item::`, defined for JNodes: the items of the origin's ·jvalue· as JNodes (XPath 4.0, section 4.7.4) |
| `item_category` | What an item is for choosing template rules: an XNode's node kind, or other for every item that is not an XNode (perf-15) |
| `item_count` | Number of items: `uint64_t` for a sequence (a range may hold more than fits in memory), `size_t` in an `item_view` |
| `item_demand` | How many leading items of an expression's value its consumer reads (fn:exists one, the effective boolean value two, `[k]` k), 0 for all; set on the IR by the demand pass, so a MAP, FILTER, axis step or traversal stops once it has made them (internal, `src/ir_internal.h`, D259) |
| `item_heap` | Per-processor owner of the reference-counted cells that items and sequences point to |
| `item_index` | Position of an item in a sequence, from 0 |
| `item_kind` | Atomic item, node (XNode), JNode, function item, map or array |
| `item_representation` | How an `sxml_item` is stored: which inline value or which kind of cell its payload holds (`SXML_ITEM_REPRESENTATION_*`) |
| `item_requirement` | The item categories and, for elements and attributes, the expanded names a pattern may match, read from its pattern AST; an item outside it does not match the pattern (perf-15) |
| `item_separator` | The text written between the items of a serialized sequence |
| `item_set_number` | The number of an Earley item set among the sets the parser made, in input order; sets are made only for positions some item reaches, so a token's inside has none, and an item's origin is a set number (D290) |
| `item_span` | Consecutive items of a sequence, from a first index up to an end index or to the end (`sxml_item_span`, `sxml_sequence_make_slice`) |
| `item_type` | A class of items, such as `xs:integer`, `element(a)` or `map(xs:string, item())` |
| `item_type_round` | With an import cycle, reading every module's namespaces and named item types only, repeatedly, each time with the public types its imports gave the last time, until no more are found; a module's own reading then sees the found types of its imports not read yet (D287) |
| `item_view` | A read-only pointer to items plus `item_count` |
| `iteration_marker` | The map `{Q{XSLT}iterate: state}` that `xsl:next-iteration` or `xsl:break` gives as the last item of the body's value (D145) |
| `iteration_state` | The array `xsl:iterate` lowering passes from one item to the next: control (0 go on, 1 broken), the item's output, the parameter values (D145) |
| `iteration_step` | What one call to an iterator yields: whether there was an item, and the item |
| `ixml_grammar` | An Invisible XML grammar compiled for the Earley parser: rules, alternatives, terminals and marks |
| `ixml_mark` | How a rule, nonterminal or terminal appears in the XML of a parse: as an element (`^`), an attribute (`@`) or hidden (`-`; the Specification's "deleted") |
| `javascript_package` | The hand-written TypeScript package in `bindings/javascript/` (npm name `@tinyactors/sxml`, D239) that loads `sxml.wasm` in Node.js and browsers (D10, D138) |
| `javascript_shim` | The export files of the JavaScript package (`bindings/javascript/sxml_javascript_exports.c`, `sxml_javascript_full_exports.c` and the resolver over the import `sxml_host.resolve` in `sxml_javascript_host.c`): the names under which `sxml.wasm` and `sxml-full.wasm` export the `host_shim` (D138, D240) |
| `jni_glue` | The one C file of the Kotlin binding (`bindings/kotlin/src/main/c/sxml_kotlin_jni.c`): JNI entry points that turn native requests into host shim requests (D238) |
| `join` / `split` | Concatenate two array trees, keeping balance / cut an array tree before a member index into two |
| `json_event` | One step of walking a JSON text: the start or end of an object or array, a key, or a scalar; what a JSON sink receives |
| `json_key` | The ·jkey· of a JNode: the map key or 1-based array or sequence position under its parent |
| `json_key_index` | The hash index of the keys of the JSON objects (or map elements) open at once, to find duplicate keys in O(1) |
| `json_node` | A JNode: a wrapper that lets maps and arrays be navigated with axes |
| `json_node_test` | An item type over JNodes: `jnode(key, value type)` |
| `json_replacement` | The string value of the fallback function's result for one codepoint, inserted for each of its `unpermitted_escape`s (D143) |
| `json_sink` | Interface receiving the JSON events of one walk: it makes XDM values (`fn:parse-json`) or tree builder events (`fn:json-to-xml`) |
| `json_text` | A text in the JSON syntax of RFC 8259: the `$value` of `fn:parse-json` and `fn:json-to-xml` |
| `json_tree` | A JTree: the JNodes reachable from one root JNode |
| `json_value` | The ·jvalue· of a JNode: the value it wraps |
| `kept_ast` | An XPath AST a stylesheet lowering makes (a short text, a pattern's match expression) and keeps until it ends (D152) |
| `kept_error` | The copy of a generator's error that a lazy sequence keeps and raises again on every later read (D41) |
| `key_index` | The map from key values to nodes of one xsl:key name for one tree, built once per evaluation (XSLT 4.0, 20.2; D149) |
| `key_sequence` | The values an identity constraint's fields give for one node its selector selected, in field order; two are equal member for member by XSD's equality (XSD 1.1 Part 1, 3.11.4) |
| `key_tree` | The balanced tree of a persistent map's keys ordered by key hash, then entry stamp, for lookup by key |
| `keyword` | A fixed word of a language: an XPath keyword (see `keyword_name`), or one of the words an XSLT attribute takes from a fixed list (`shallow-copy`, `strict`) |
| `keyword_argument` | An argument given as `name := value` (XPath 4.0, section 4.5.1.1) |
| `keyword_name` | A keyword used as a name; XPath 4.0 keywords are not reserved |
| `kotlin_package` | The Kotlin binding in `bindings/kotlin/` (package `dev.tinyactors.sxml`, D239): an Android library (AAR) and a JVM library over `libsxml_kotlin.so` |
| `language_library` | A tree-sitter grammar compiled to a shared library (`libtree-sitter-NAME.so`, `.dylib`, `.dll`) that exports `tree_sitter_NAME()`, which the sxml command loads with `--code-language` (D288) |
| `language_module` | A tree-sitter grammar compiled to WebAssembly as a `side_module` (`tree-sitter-NAME.wasm`, as `tree-sitter build --wasm` writes it) that exports `tree_sitter_NAME()`, which the JavaScript package's `module_linker` links into `sxml.wasm` (D293) |
| `language_part_list` | The parts of one part registry that serve one language, generated in a translation unit of its own; the language's entry point names it, so a module links only the parts of the languages it offers (D227) |
| `language_set` | The languages (XPath, XQuery, XSLT) a library, evaluator or lowering part serves; a library or evaluator part without one serves all three (D101, D227) |
| `language_table` | The names and words of one language that the formatting functions use: months, days, eras, ordinal suffixes and spell-out rules |
| `last_descendant` | The node index of the last node in a node's subtree (the node itself when it has none) |
| `lead_byte` | The first byte of the UTF-8 encoding of one codepoint |
| `leading_step` | A step of an XNode pattern that is not on the right of `/` or `//`; it is matched on the axes child-or-top, attribute-or-top or namespace-or-top (XSLT 4.0, 6.3.2.2) |
| `leaf_in_place` | An operand task (a MAP's body, a predicate, a LET's bound expression, a quantified condition, a sort key) whose expression is a leaf in the pushing task's own frame, computed when it would be pushed instead of in a task of its own, charged the step its task would have taken (internal, `src/evaluator.c`, D229) |
| `leo_item` | Leo's transitive item: for an item set and a nonterminal that exactly one item there waits for, as its last symbol, the topmost item that completing the nonterminal reaches up the right recursion, so the chain completes in one step; the items in between are made only for the parse tree (D283) |
| `lexical_form` | A string in the lexical space of a type (XSD 1.1 Part 2): what a cast from `xs:string` parses |
| `library_module` | An XQuery module that starts with a module declaration and is reached by `import module` |
| `library_part` | The native functions of one `src/function_library_<part>.c` file: one `sxml_native_function_view`, named in a function library's configuration |
| `limits` | A struct of bounds on work and memory for untrusted input (`sxml_processor_limits`, `sxml_xml_limits`) |
| `line_end_normalization` | Turning CR LF and lone CR into LF in all text of a document (XML 1.0, section 2.11); in XML 1.1 also NEL, CR NEL and LINE SEPARATOR (XML 1.1, section 2.11) |
| `line_ending` | What the serializer writes for each newline (parameter `line-ending`): LF, CR LF or CR |
| `line_number` | Line of a source position, from 1; 0 means unknown |
| `literal_result_element` | An element of a sequence constructor outside the XSLT namespace, copied to the result (XSLT 4.0, 11.1) |
| `loaded_document` | A document fn:doc loaded during one evaluation, kept so the same URI gives the same node |
| `local_name` | The local part of a name |
| `local_time_type` | What local time is at an instant in a zone: its offset from UTC, whether it is daylight saving time, and its abbreviation (RFC 8536, 3.2, "local time type records"; D209) |
| `local_value` | A date/time value as written, without adjusting it by its timezone (F&O 4.0, section 9.6) |
| `locale_chain` | The CLDR files a language tag is looked up in, from the most specific to root: `de_AT`, `de`, `root` (inheritance by truncation, UTS #35, Part 1, section 4.1) |
| `localization` | The optional feature that gives the formatting functions languages other than English and the calendars BE, JE, OS and AH from CLDR files the host serves (`sxml_localization_feature`; D221) |
| `locals_query` | A tree-sitter query (the grammar's `queries/locals.scm`) whose captures `local.scope`, `local.definition` and `local.reference` mark scopes, the names they define and the names that refer to them; a reference gets its definition's highlight (D288) |
| `location_demand` | What AUTOMATIC decides by, kept in the processor: whether a program has been compiled, whether one that reads source locations has, and the evaluation in progress with its unit's answer (D318) |
| `location_hint` | A URI after `at` in a module import, resolved against the importing module's static base URI; one resolver request each |
| `lookahead_group` | A captured group of a capturing subexpression within a lookahead assertion; it may lie outside the matching segment (F&O 4.0, `fn:analyze-string`) |
| `lookahead_set` | Where an Earley item stands, the distinct terminals that can read the next character, and end of input: FIRST of the rest of its production, with the FOLLOW set of its nonterminal when that rest can be empty. An item whose lookahead set misses the next character's class is not made (D283) |
| `lowerer` | The state of one lowering into one unit: the AST and static context being lowered, the variable scope, the current function and its focus (D103) |
| `lowering` | Translating an AST into IR expressions of a unit |
| `lowering_block` | The LETs bound while one expression is lowered, closed by its result (administrative normal form) |
| `lowering_frame` | One running lowering procedure on the lowering machine's stack, in allocator memory, with its step, locals and destination (D71, D103) |
| `lowering_machine` | The explicit stack of lowering frames that lowers without C recursion (D71) |
| `lowering_override` | One node of one XPath AST that a procedure of the caller lowers instead of the rule of its kind, wherever lowering meets it (`sxml_xpath_lowering_override`, D223) |
| `lowering_part` | A file defining `sxml_xpath_lowering_part_<part>`: AST kinds and the procedures or leaf functions lowering them for some languages, found by CMake's glob (D103) |
| `lowering_procedure` | A construct lowered by frames: a step function and the size of its locals, static data (D103) |
| `machine_transformation` | An XSLT transformation primed for a unit evaluation on a running machine rather than one of its own (`src/xslt_transform_internal.h`, fn:transform, D203) |
| `main_function` | Function 1 of a unit: the body of the expression or query |
| `main_module` | An XQuery module with a query body, the one a host compiles (XQuery 4.0, section 5) |
| `map` | An XDM 4.0 map; entries keep insertion order |
| `map_entry` | One key/value pair of a map |
| `map_node` | One node of an entry tree or a key tree: an entry, its key hash and entry stamp, two subtrees (D61) |
| `marker` | The single character that stands for a decimal format property in a picture string |
| `match_expression` | The XPath AST of xs:boolean, over the item as context value, that is true exactly when the item matches a pattern (D118) |
| `match_function` | The IR function of one parameter, returning xs:boolean, that a template rule's pattern is lowered to |
| `matching_segment` | A segment of the regex input that a regex matches: start and length, held as a byte range (F&O 4.0, section 6.1.1) |
| `member_index` | Position of a member in an array, from 0 |
| `memory_requirements` | Size and alignment an object needs |
| `memory_stream` | The output stream test double: keeps what is written, up to a capacity |
| `memory_view` | A writable pointer plus `byte_count` |
| `merge_cursor` | The reader of one input of MERGE: an anchor document of a merge stream, pulled one selected node at a time, or the grounded sources' tuples; its current tuple is its head (D236) |
| `merge_stream` | A streamed xsl:merge-source of MERGE: the URIs of its anchor documents and the STREAM chain of its select, whose last body makes the tuple of a selected node (D236) |
| `message_context` | The item an assertion's message is evaluated with (sch:value-of, sch:name): the subject node when a subject attribute applies, else the context item (6.3, D315) |
| `message_template` | A validation error's description as static text with placeholders (%e an element, %a an attribute, %v a value, %t a type, %x the expected terms of a content model cursor, %f the facet a value breaks, ...) that the error's fields fill |
| `method_call` | `M =?> N(...)`: the function in entry N of each map of M, called with that map first (XPath 4.0, section 4.15.5) |
| `mode` | An XSLT mode: its template rules and its built-in rule |
| `model_group` | A term that combines particles with a compositor (XSD 1.1 Part 1, 3.8) |
| `model_group_definition` | A named model group, the target of `<xs:group ref>` (XSD 1.1 Part 1, 3.7) |
| `module_import` | An `import module` declaration: a target namespace and its location hints (XQuery 4.0, 5.12) |
| `module_linker` | The JavaScript package's small dynamic linker (`bindings/javascript/src/linker.ts`): reads a side module's `dylink.0`, reserves its memory with `sxml.wasm`'s `malloc` and its slots in the exported function table, instantiates it with sxml's memory, table, stack pointer and C library functions, and runs its relocations (D293) |
| `module_record` | The result of fn:load-xquery-module: the public variables and functions of the loaded module (F&O 4.0, 18.1) |
| `module_set` | The main module and every library module one compilation loads, each once; imports name namespaces, so a module sees every loaded module of a namespace it imports (XQuery 4.0, 5.12.2) |
| `module_uri` | The absolute URI a module was read from: a stylesheet module, or an XQuery library module where it was found; the source name of its expressions and, for XQuery, its static base URI |
| `multiplicity` | How often an option or positional may appear: at most once, repeatable, exactly one, optional, one or more, zero or more |
| `name_format` | The `name-format` option of `fn:element-to-map`: `default`, `eqname`, `local` or `lexical` |
| `name_identifier` | Identifier of an interned expanded name (namespace URI plus local name) in a name pool |
| `name_namespaces` | The namespace bindings an IR constructor resolves a name given as a string with: the statically known namespaces extended by enclosing direct constructors' declarations (D110) |
| `name_parts` | The namespace URI identifier and local name of a name identifier, as returned by the name pool |
| `name_pool` | Per-processor table that interns namespace URIs, prefixes and expanded names into small identifiers |
| `name_table` | The regex compiler's list of the group names read so far, for duplicates and `\k<name>` (D105) |
| `name_test` | Which names a node test or path step matches: any name, one expanded name, or a wildcard |
| `name_token` | One token of a name-list attribute of an XSLT element: a name, a wildcard, or `#default`, `#unnamed`, `#current`, `#all` |
| `named_character_map` | The character map an `xsl:character-map` declares: its `xsl:output-character` mappings after those of the maps it uses, expanded, a later mapping of a character replacing an earlier one (XSLT 4.0, 26.1.6) |
| `namespace_alias` | An `xsl:namespace-alias` pair: literal result elements and attributes in its stylesheet URI are output in its result URI (XSLT 4.0, 11.1.4) |
| `namespace_binding` | A prefix bound to a namespace URI |
| `namespace_constraint` | Which namespaces a schema wildcard allows: any, an enumeration, or all but an enumeration (`not`) |
| `namespace_context` | The namespace bindings, XPath default namespace and base URI that QName values and XPath records of a schema document are read with |
| `namespace_declaration_attribute` | An `xmlns` or `xmlns:p` attribute of a direct element constructor; it makes a namespace binding, not an attribute (XQuery 4.0, 4.12.1.2) |
| `namespace_fixup` | What a tree builder does so every element and attribute name has a binding of its prefix to its namespace URI: adds bindings, undeclares the default namespace, or picks another prefix (XSLT 4.0, section 5.7.3) |
| `namespace_number` | Which in-scope namespace of an element a namespace node is, from 1; 0 for other nodes and for a stored namespace node |
| `namespace_prefix` | The prefix part of a qualified name |
| `namespace_prefix_identifier` | Identifier of an interned prefix; 0 is "no prefix" |
| `namespace_uri` | The namespace part of an expanded name |
| `namespace_uri_identifier` | Identifier of an interned namespace URI; 0 is "no namespace" |
| `native_cleaner` | The Kotlin binding's backstop for objects never closed: a PhantomReference per object and one daemon thread that runs its release action under the processor's lock, as `java.lang.ref.Cleaner` does on the JVM (D247) |
| `native_function` | A function written in C, described by one row of static data (name, parameters, result type, properties, body) |
| `native_request` | The internal Kotlin object a JNI entry point takes as its one argument besides the environment and class: the inputs of one host shim call, and fields the call writes back (handle, error, side results) |
| `native_step` | One run of a resumable native body: it finishes with a value, or asks the evaluator to call a function item and to run the next step with the result |
| `navigational` | Of a step E2 in E1/E2 (XPath 4.0, section 4.7.3): ".", an axis step, a path, or a union, sequence, filter, conditional or let made only of navigational expressions |
| `nesting_depth` | How many modules lie between a stylesheet module and the principal module in the module tree |
| `nilled_marker` | The xs:QName `fn:null` that stands for a nilled element in an element map |
| `node` | An XML tree node (an XNode in XPath 4.0 terms) |
| `node_comparison_operator` | What a GNode comparison tests: `is`, `is-not`, `<<`, `>>`, `precedes-or-is`, `follows-or-is` (XPath 4.0, section 4.11.3) |
| `node_description` | What a tree reports about one node: kind, name, parent, siblings, children, type annotation, flags |
| `node_height` | Of an array node: 0 for leaves, one more than the taller subtree for a branch node (AVL balance) |
| `node_index` | Index of a node within its tree's arena |
| `node_kind` | One of the seven XNode kinds: document, element, attribute, text, comment, processing instruction, namespace |
| `node_reference` | A tree plus a node index (plus a namespace number for namespace nodes); identifies a node |
| `node_segment` | One of the blocks holding an arena tree's nodes; segment k holds 16·2^k nodes, so nodes never move as the tree grows |
| `node_table` | The key sequences and nodes of one key or unique at an element: its own qualified node set and the node tables of its children, a key sequence children give with different nodes left out; what a keyref looks in (XSD 1.1 Part 1, 3.11.5) |
| `node_test` | An item type over XNodes: a node kind, an optional name and an optional type annotation |
| `node_type` | The name a grammar gives one kind of concrete syntax tree node (`additive_expression`), as listed in `src/node-types.json` |
| `nominative_record_type` | A named record type matched only by records annotated with it |
| `non_delimiting_terminal` | A name, keyword or numeric literal terminal; two of them must not touch (XPath 4.0, A.3.2) |
| `normalization_form` | One of NFC, NFD, NFKC, NFKD (UAX #15) or FULLY-NORMALIZED (F&O 4.0, `fn:normalize-unicode`) |
| `notation_declaration` | A named notation with its system and public identifiers (XSD 1.1 Part 1, 3.14) |
| `null_allocator` | An `allocator` whose operations is NULL: it never allocates; in a configuration it means "must not grow" |
| `null_value` | The value that stands for JSON null in `fn:parse-json` (option `null`, the empty sequence by default) |
| `number_format` | The `number-format` option of `fn:parse-json`: `double`, `decimal` or `adaptive` |
| `number_view` | The second order of a filter index whose keys are all xs:untypedAtomic: the keys cast as "=" casts them against a number, by nearest double, made at the first numeric probe value (internal, `src/evaluator_index.c`, D259) |
| `numbering_kind` | How a primary format token numbers: digits, letters, Roman numerals or words, or an unsupported token formatted as `1` |
| `occurrence` | How many items a sequence type allows: zero, one, zero or one, zero or more, one or more |
| `open_content` | Elements a complex type allows besides its particle, matched by a schema wildcard, anywhere (interleave) or after the content (suffix) (XSD 1.1 Part 1, 3.4.1) |
| `open_elements` | The HTML parser's stack of open elements, the current node last (HTML Living Standard, 13.2.4.2) |
| `open_node` | The element (or document node) of a shallow stream window whose content has not been read yet: a STREAM whose origin it is reads that content from the events (D219) |
| `open_result_document` | A result document between the handler's open and close: its URI, effective output definition and target |
| `operand` | One input of a checked arithmetic function (`left_operand`, `right_operand`) |
| `operand_usage` | How a construct uses the nodes of an operand: absorption, inspection, transmission or navigation (XSLT 4.0, 19.3) |
| `operator` | The token that selects what an operator expression computes (`SXML_AST_OPERATOR_PLUS`); in the IR, what a binary or unary expression computes |
| `operator_function` | A function item made by `fn:op`: `fn($x, $y) { $x op $y }` for one binary operator |
| `option` | A named command-line argument: `long_name` (`--format`), optional `short_name` (`-f`), `value_kind`, `value_name` (`FORMAT`), `choices`, `default_value` |
| `option_definition` | One option of a library function that follows the option parameter conventions (F&O 4.0, section 1.7): its name and the type its value is coerced to |
| `optional_feature` | A part of sxml that a host opts into by naming it in the processor configuration, so a program that never names it does not link it: large tables and front ends such as UCA, the timezone database, HTML parsing (`sxml_optional_feature`; D183, D184, D189) |
| `ordering` | Result of a three-way comparison: less, equal or greater |
| `ordinal` | Ordinal numbering (`1st`, `second`), asked for by the `o` format modifier |
| `output_buffer` | The built-in output stream: collects bytes in memory |
| `output_chunk` | The bytes one output event carries: at most the chunk size (64 KiB by default), gathered in a buffer the shim allocates for the call and reuses; the JavaScript package copies each out of the module (D294) |
| `output_declaration` | An XQuery option declaration in the serialization namespace (`declare option output:indent "yes"`, XQuery 4.0, section 5.22) |
| `output_definition` | The serialization parameters and `build-tree` of one `xsl:output` declaration, named or unnamed (XSLT 4.0, 26.1) |
| `output_sink` | What receives an `output_chunk` in the JavaScript package (`OutputSink`): a function, or an object with `write` and optionally `close` and `abort` (D294) |
| `output_stream` | Interface that receives bytes, written in order; a command-line program writes through `sxml_cli_output_stream` |
| `output_writer` | The host shim's receiver of a processor's serializations a chunk at a time (`sxml_host_output_writer`, `sxml_host_set_output`): output events of bytes, and of the bounds of each serialized result document (D294) |
| `overriding_declaration` | A child of `xsl:override`: the using package's replacement of a component of the used package (XSLT 4.0, 3.4.5.3) |
| `package` | An XSLT package: the principal module or the module an `xsl:use-package` read, with the modules it includes and imports; an `xsl:stylesheet` is an implicit package (XSLT 4.0, 3.4; D160) |
| `package_table` | The packages and component instances of one stylesheet lowering, which binds every reference to a component (D160) |
| `package_version` | The versions of an XSLT package that `xsl:use-package` accepts (XSLT 4.0, "Versions of a Package") |
| `pair` | Two values a function relates or compares (`type_pair`, `item_pair`, `node_pair`) |
| `parameter_binding` | A parameter name with the value passed for it (`xsl:with-param`) |
| `parameter_default` | How an omitted parameter gets its value: required, the context value, or supplied by the body |
| `parameter_document` | An `output:serialization-parameters` element giving serialization parameters (Serialization 4.0, section 3.1) |
| `parameter_entity` | An entity referenced as `%name;` inside the DTD (XML 1.0, section 4) |
| `parameter_map` | A map(xs:QName, item()\*) of the parameters supplied to a template: one for the non-tunnel and one for the tunnel parameters (D144) |
| `parentless_namespace` | A namespace node without a parent element (XQuery computed namespace constructor, xsl:namespace outside an element); stored as the root of a tree of its own (D98) |
| `parse_forest` | The Earley parser's shared record of every parse of an input under an ixml grammar; the first tree is serialized, and more than one marks the result ambiguous |
| `parser_part` | What a grammar derived from grammars/xpath adds to the XPath front end's walker: its language, node types, fields and the hooks `apply_token`, `describe_payload` and `finish_node` (`sxml_ast_parser_part`, D111) |
| `parser_part_extension` | A parser part that another names as its extension: the walker looks up node types and fields, and offers tokens, to it after the naming part, so two grammars share one language extension's tables (Full Text's, `sxml_ast_parser_part.extension`, D235) |
| `parser_token` | A concrete syntax tree child that makes no AST node and is absorbed into its parent's detail or flags as the parent is described |
| `part` | One sequence of a concatenation, kept by reference; a concatenation's parts are never empty and never concatenations (`part_index`) |
| `part_end` | The number of items in a concatenation's parts up to and including one part; cached as parts are counted |
| `part_registry` | The list of library parts, evaluator parts or lowering parts that CMake generates from the glob of their files, so a new part needs no edit elsewhere (D101, D103) |
| `part_task` | An evaluator task whose step function an evaluator part supplies, with state the part owns (D101) |
| `partial_application` | A function call with argument placeholders, giving a function item (XPath 4.0, section 4.5.2.5) |
| `partially_applied_function` | The function item `fn:partial-apply` or a placeholder call makes: the wrapped function with some arguments bound, captured with it |
| `particle` | The component that says how often a term may occur: minimum and maximum occurrences and the term (XSD 1.1 Part 1, 3.9) |
| `pass` | An IR-to-IR function over a unit, run in a fixed list before the unit is sealed |
| `pattern` | An XSLT 4.0 match pattern (XSLT 4.0, section 6.3) |
| `pattern_instance` | A sch:pattern or sch:group with is-a: the abstract pattern it names, compiled with each placeholder replaced by the value of its sch:param, else the abstract pattern's default (D315) |
| `pattern_variable` | A sch:let of a schematron pattern: a global variable under a synthesized name, which the pattern's expressions reach as a placeholder is replaced and its templates through a local binding of the written name, so that it is in scope in that pattern only (3.23; D323) |
| `pattern_walk` | The descent of a compiled schematron pattern's mode: its traversal templates and the template rules of its rules apply templates to the element children of document and element nodes, and to their attributes, text, comment and processing-instruction children only where one of the pattern's rule contexts may match that node kind (perf-14, D326) |
| `pending_cell` | A cell whose last reference is gone, waiting on the item heap's list to be freed without recursion |
| `pending_element` | The start of an element that the schema validator holds back, with its namespace bindings and attributes, until its first content or its end: xsi:type, xsi:nil and the attributes decide its governing type and annotation first |
| `pending_error` | An error of a try or catch clause that a finally clause raises again once it has run without error (D109) |
| `pending_update_list` | The update primitives an updating expression returns, applied together at the end of a snapshot (XQuery Update Facility 3.0, "pending update list"); internal to the evaluator, not an XDM item |
| `percent_decoding` | Replacing `%HH` by its octet and reading the octets as UTF-8, with U+FFFD for what is not (F&O 4.0, fn:decode-from-uri); `form_decoding` turns `+` into a space first |
| `permitted_character` | A codepoint an xs:string can hold: U+0001 to U+D7FF, U+E000 to U+FFFD, U+10000 to U+10FFFF (F&O 4.0, section 17.4.3) |
| `persistent_map` | A map made by put or remove: an entry tree and a key tree that share structure with the map they came from (D61) |
| `phase_selection` | What "#ANY" asks for (5.4.13, 5.5.22): each validation's phase is the first sch:phase in schema order whose when expression is true at the instance document root, else every pattern is active; compiled into one stylesheet (D315) |
| `picture_string` | The second argument of `fn:format-integer` and `fn:format-number`: the pattern a number is written by |
| `place_marker` | The sequence of integers xsl:number formats: its value attribute's, or computed from the selected node's position by level, count and from (XSLT 4.0, 12) |
| `placeholder` | `$name` in an expression where name, the whole name token after `$`, is a parameter in scope: a pattern instance's, else a schema parameter; replaced by its value in one scan, the value not scanned again (Annexes C, H, J; D315) |
| `placement` | The memory an object is placed in by `initialize` |
| `platform_layer` | The only code of the program that touches stdio (`apps/sxml/platform_stdio.c`) |
| `plural_rule` | A condition on a number that gives its plural category (`one`, `two`, `few`, `other`) in a language, here the ordinal ones (UTS #35, Part 3, section 5) |
| `population` | The sequence xsl:for-each-group groups, its select value, in population order (XSLT 4.0, 14.1) |
| `positional` | A command-line argument identified by its place, not a name (POSIX "operand"; `operand` is taken) |
| `positional_filter` | A FILTER whose predicate compares the context position with a value fixed for the filter (`$s[position() > 1]`, `$s[position() = $n to $m]`); over a stored input it is the sequence slice at those positions (`src/evaluator.c`) |
| `post_descent_value` | The value of an xslt accumulator just after the end event of a node (fn:accumulator-after) |
| `posture` | How a construct's value relates to a streamed input: grounded, climbing, striding, crawling or roaming (XSLT 4.0, 19.1) |
| `pragma` | XQuery `(# name contents #)` before an extension expression |
| `pre_descent_value` | The value of an xslt accumulator just after the start event of a node (fn:accumulator-before) |
| `precision` | In rounding, the number of digits kept after the decimal point; negative rounds to a power of ten (`fn:round`) |
| `predeclared_namespace` | A prefix bound in every static context (xml, xs, xsi, fn, map, array, math, err) |
| `predefined_entity_reference` | `&lt;`, `&gt;`, `&amp;`, `&quot;` or `&apos;` in the same places (XQuery 4.0, 4.3.1.2) |
| `prefix_binding` | A namespace prefix and URI by which the DTD's qualified names are resolved when its schema documents are written |
| `prefix_normalization` | XHTML with HTML5: elements in the XHTML, SVG or MathML namespace written unprefixed with a default namespace, and prefixes for those namespaces dropped (Serialization 4.0, section 6) |
| `presentation_modifier` | The first (a primary format token or `n`, `N`, `Nn`) or second (`a`, `t`, `c`, `o`) modifier of a variable marker |
| `primary_composite` | A codepoint with a canonical decomposition of two codepoints that is not Full_Composition_Exclusion; canonical composition joins such pairs (UAX #15) |
| `primary_format_token` | The part of a `fn:format-integer` picture before its last semicolon: a digit pattern or a numbering token such as `a`, `I`, `Ww` |
| `primitive_type` | The primitive atomic type an atomic type is derived from (`xs:integer` gives `xs:decimal`) |
| `principal_module` | The stylesheet module the processor was asked to compile (module 1) |
| `principal_result` | The result of the invocation itself, as opposed to the result documents (XSLT 4.0, 2.4.6) |
| `principal_stream` | The principal result of a transformation that streams its principal source, built as it is made into the result writer's tree builder and serialized as it is built when it is serialized as XML or text, so its tree is never held (D231, D295) |
| `process_contents` | How an item a schema wildcard matched is assessed: strict, lax or skip |
| `processor` | The object (Phase 1) that owns one name pool, type registry and item heap and compiles and runs queries |
| `produced_item` | An item a sequence generator has produced, kept by its lazy sequence so every reading sees it (`produced_item_count`) |
| `program` | A command-line program as a static table: name, summary, commands (`sxml_cli_program`) |
| `prolog` | The declarations of an XQuery module before its query body: setters, namespace declarations, imports, variables, functions, types, options (XQuery 4.0, section 5) |
| `public_declaration` | A function, variable or item type a library module declares without %private: what an import makes visible (XQuery 4.0, 5.16, 5.18) |
| `put_document` | A document or element node fn:put stores, handed to the host's update sink with the absolute URI it resolved to after every changed tree; the host stores it, sxml writes nothing (`sxml_xquery_put_document`; XQuery Update Facility 3.0, 6.1; D205) |
| `qt4_catalog` | The QT4 suite's `catalog.xml` as the driver holds it: its test-set entries and global environments |
| `qt4_collection` | A collection of a QT4 environment for `fn:collection`: sources, resources and queries under one URI |
| `qt4_dependency` | A `<dependency type value satisfied>` of a QT4 test set or test case |
| `qt4_driver` | The QT4 test-suite driver: the developer tool under `tests/qt4/driver/` that reads the QT4 catalog and runs its test cases (D44) |
| `qt4_environment` | The static and dynamic context a QT4 test case runs in: sources, parameters, namespaces, static base URI, resources, collections |
| `qt4_parameter` | A `<param>` of a QT4 environment: an external variable with a `select` expression and an optional declared type |
| `qt4_resource` | A text resource of a QT4 environment, reached by URI (`fn:unparsed-text`, `fn:json-doc`) |
| `qt4_resource_table` | What one QT4 test case's environment serves through the resource resolver: its resources by URI, its collections as sequences, and suite files by `file:///` URI |
| `qt4_scope` | Whether a QT4 test case counts in the denominator of the pass-rate targets (D95): its spec matches and it needs no non-goal; said `in_scope` in reports |
| `qt4_source` | A document of a QT4 environment: the context item (`role="."`), an external variable (`role="$name"`) or a document available by URI |
| `qt4_target` | The language and version a driver run targets: XP40 or XQ40 |
| `qt4_test_case` | One `<test-case>` of a QT4 test set: query, environment, dependencies, result (never `test_case`, which is a unit test) |
| `qt4_test_set` | One test-set file of the QT4 suite (`fn/abs.xml`): local environments, set dependencies and test cases |
| `qualified_name` | Prefix plus local name, as written (the QName); `qname` allowed |
| `qualified_node_set` | The nodes an identity constraint's selector selects (skipped ones left out) whose fields each give one value: the members whose key sequences unique, key and keyref compare (XSD 1.1 Part 1, 3.11.4) |
| `quantified_fusion` | A some/every over a filter or a loop made once and read only by it, rewritten to run over that filter's or loop's input with the predicate or body in its condition, so the items are made as it reads them (internal, `src/ir_loop_passes.c`, D259) |
| `query_binding` | The language of a Schematron schema's expressions, its queryBinding attribute, compared ignoring case: sxml reads xslt, xslt4, xpath, xpath4 and a schema without one as XSLT 4.0 and XPath 4.0, and xslt2, xslt3, xpath2, xpath3 and xpath31 on the same 4.0 processor; every other binding is schematron:query-binding (D313 as amended, D315) |
| `query_run` | In the `sxml` program: one run of a query command (`xpath`, `xquery`, `serialize`): its processor, `--param` bindings and context item (`apps/sxml/query_command.h`) |
| `query_scanner` | The external tree-sitter scanner shared by the XPath, XQuery and XSLT pattern grammars |
| `query_text_xml_version` | The version of XML whose rules XQuery query text is read by, a processor option (`reads_query_text_as_xml_1_1`): XML 1.0 by default, or XML 1.1 for line ends, character references and namespace undeclaration in direct constructors (XQuery 4.0, xgc: xml-version; D275) |
| `quirks_mode` | The document mode the DOCTYPE sets: no-quirks, limited-quirks or quirks (13.2.6.4.1); in the parser it only decides whether a table closes an open p |
| `raised_error` | The error an item heap keeps for an error record whose description or value was computed (`sxml_item_heap_raise_error`) |
| `random_seed` | The value that fixes the numbers of `fn:random-number-generator` |
| `range_variable` | A variable a for, let or quantifier binding binds to an item, member, key, value or part; the position variable is not one |
| `raw_result` | The value an invoked template or function returns, coerced to its declared type, before any tree is built or serialization (XSLT 4.0, 2.4) |
| `raw_text` | Text the serializer writes unescaped whatever the escape style: the content of HTML `script` and `style` elements (character context `RAW_TEXT`) |
| `read_set` | The characters some instructions of a compiled regex may read: an ASCII member set and whether a character outside ASCII may be among them; larger than needed, never smaller. The start filter's first characters and a required literal's prefix are read sets (internal, `src/regex.c`, D310, D311) |
| `reading_order` | The order the module set's static contexts are read in: imported modules before importers, a cycle broken where the walk meets it |
| `reads_source_locations` | Said of a compiled program (an IR unit) that can ask for the source locations of the documents it reads: it calls or refers to fn:location, fn:function-lookup, fn:load-xquery-module, fn:transform or xsl:evaluate, or has schemas it can validate with; under AUTOMATIC its documents keep locations (D313, D318) |
| `receiving_task` | A node constructor's part task while its content is evaluated: the values of the delivering tasks above it go into its construction as they are made (D166) |
| `record` | A map whose keys are fixed by a record type |
| `record_field` | One field of a record type: a name and a sequence type; in `xsl:record`, an attribute whose local name is the field name and whose expression gives the value |
| `record_type` | An item type whose instances are records; a list of fields |
| `recorded_subtree` | The validated copy of an element and its descendants that the schema validator keeps while the element is open, for the XPath of assertions and identity constraints |
| `redefinition` | A component that an xs:redefine or xs:override child gives in place of the component of its name in the document it refers to (XSD 1.1 Part 1, 4.2.4 and 4.2.5) |
| `reference_node` | For the preceding and following axes from an attribute or namespace node: its parent element, whose position decides which nodes come before and after |
| `reference_resolution` | Turning a relative reference into a URI against a base URI (RFC 3986, section 5.2) |
| `regex` | A compiled regular expression of the F&O 4.0 dialect (F&O 4.0, section 6.1), immutable once compiled (`sxml_regex`); never `regexp` or `pattern` (`pattern` is the XSLT match pattern) |
| `regex_cache` | The compiled regexes of one evaluation, keyed by regex text and regex flags, so `fn:matches`, `fn:replace`, `fn:tokenize`, `fn:analyze-string`, `fn:matching-segments` and fn:regex's functions compile a regex once; bounded, the least recently used entry no call is using evicted first, kept in the evaluation memo (internal, `src/function_library_regex.c`, D308) |
| `regex_flags` | The flags of a regular expression, from the `$flags` string: `s` dot-all, `m` multi-line, `i` case-insensitive, `x` whitespace removed, `q` literal, `c` comments (F&O 4.0, section 6.2) |
| `regex_input` | The string a regex matcher searches (`$value` of `fn:tokenize`, the "input string" of F&O 4.0, section 6.1.1) |
| `regex_instruction` | One operation of a compiled regex, run by the backtracking matcher |
| `regex_matcher` | The working state of matching one compiled regex: backtrack stack, captured groups, step count; one per thread (`sxml_regex_matcher`) |
| `regex_text` | The source text of a regular expression (`$pattern` of `fn:matches`, `fn:tokenize`, ...) |
| `registered_language` | A source code language a host gives the processor (`sxml_processor_configuration.source_code_languages`): a tree-sitter grammar and its query texts; one with a built-in language's name replaces it (D288) |
| `regular_token` | A nonterminal of an ixml grammar whose language is regular (characters, character sets, repetitions and such nonterminals, no recursion but a repetition's) and whose XML is its text or nothing: its uses inside are hidden, it has no insertions, and its terminals are all kept or all hidden. Compiled once with the grammar into a token automaton; the Earley parser reads it in one step and completes it at each end the lookahead allows (D290) |
| `relabel` | Coercion that changes an atomic item's type annotation to a derived type whose value space holds the datum, keeping the datum (XPath 4.0, section 3.4.3) |
| `relabelled_cast` | A cast whose result has the source's own text (a string, untyped or URI item to xs:string or xs:untypedAtomic, an item to a type it is derived from): the source's string cell under the target's annotation, not a new parse (internal, `src/cast.c`, D319) |
| `release_contents` | The cell class operation that releases what a cell holds before the heap frees it |
| `rendition` | The string a decimal format property becomes in the formatted result; `%:pc` is marker `%`, rendition `pc` |
| `reorder_group` | A run of primary weights of the root collation that a reorder code moves as a whole: space, punct, symbol, currency, digit, or one or more scripts (UTS #35, Part 5, section 3.13) |
| `replacement_text` | The text an internal entity stands for: its literal value with character references expanded and general entity references kept (XML 1.0, section 4.5) |
| `request` | Struct describing one operation |
| `required_literal` | A run of characters every match of a compiled regex contains, found from its instructions (a CODEPOINT run that every way to the match passes through, outside assertions), with the read set of what a match reads before it; a search looks for its next occurrence first, attempts only the start positions from which the characters up to it are in that read set, and none when it does not occur (internal, `src/regex.c`, D311) |
| `resource` | What a resource resolver delivers for a URI: bytes, a document node, or nothing |
| `resource_entry` | One URI and its bytes in a list served by the table resolver |
| `resource_form` | How a resolver delivers a resource: none, bytes, a document node, or a sequence (collections) |
| `resource_kind` | What a resource is wanted for: an XML document for `fn:doc`, ... |
| `resource_resolver` | Host interface that retrieves resources by URI: documents, text, binary resources, collections, XQuery modules, XSLT modules and packages |
| `restricted_character` | A C0 or C1 control (other than tab, LF, CR and NEL) that an XML 1.1 document may contain only as a character reference (XML 1.1, [2a] RestrictedChar) |
| `result_builder` | The tree builder a schema validator sends its annotated events to; the null builder discards them, for validity checks that build nothing |
| `result_delivery` | How a principal result or result document reaches the host: raw, as a tree, or serialized to an output stream |
| `result_directory` | The directory `sxml transform --result-directory` names: its URI is the base output URI, and only result documents below it are written (D217) |
| `result_document` | A secondary result: what one `xsl:result-document` instruction delivers, identified by its absolute URI (XSLT 4.0, 26.2) |
| `result_document_collector` | The built-in result document handler: keeps every result document in memory, by URI |
| `result_document_handler` | Host interface that receives result documents: it is opened with a document's URI and output definition and closed with its value |
| `result_document_instruction` | What one `xsl:result-document` reads when it runs: its attribute value templates in a fixed order, its namespaces, base URI and character maps |
| `result_tree` | The document node that sequence normalization makes; what the XML, XHTML, HTML and text methods write |
| `result_writer` | The object of one transformation that delivers its result documents and principal result: the stylesheet outputs, the result document handler, the base output URI, the URIs written (XTDE1490) and the current output URI |
| `resumable_native_body` | A native function body written as native steps, so a higher-order function never re-enters the evaluator on the C stack |
| `retain` / `release` | Add / drop one reference to an item, sequence or tree |
| `revalidation` | Schema validation of a tree an update rebuilt, in the revalidation mode the prolog declares (lax or strict; skip does none), whose validated copy becomes the updated tree (XQuery Update Facility 3.0, 3.2.4 upd:revalidate; D205) |
| `rounding_mode` | How a value is rounded to a precision: the `$rounding-mode` of `fn:round` (F&O 4.0) |
| `rule_bucket` | The template rules of a mode that may match items of one item category, of one name or of any name, in the mode's order; apply-templates tries only the rules of an item's two buckets (perf-15) |
| `rule_search` | Which template rules of a mode an apply-templates tries: all of them, those after the current template rule (`xsl:next-match`), or those of the levels the current rule's level imports (`xsl:apply-imports`) |
| `run_node` | An array leaf that names a contiguous part of a flat node's members, so splitting a leaf copies nothing |
| `run_report` | The output of `sxml-qt4 run`: one entry per test case that did not pass, counts per test set and overall, as text or JSON |
| `run_time_closure` | A function item that calls the function item it wraps at the place it was made: the IR function and expression of that place give the static context (its module) the callee reads, and a row that takes the caller's focus gets the focus kept there; made by named references to context-dependent functions and by fn:function-lookup (D287) |
| `sandpit` | The directory of files a QT4 test environment names for EXPath File cases, which the QT4 driver copies to a fresh temporary directory for each case and serves as the current directory (D200) |
| `scaled_integer` | A number written as sign, mantissa and power of ten, (-1)^sign x mantissa x 10^power, which Clinger's fast path turns into a double with one correctly rounded operation (internal, `src/decimal_internal.h`, D229) |
| `scanning_expression` | A relative path that has the form of a motionless pattern, assessed as crawling instead of roaming (XSLT 4.0, 19.8.8.8) |
| `schema` | A compiled XSD 1.1 schema: the schema components assembled from one or more schema documents, with its types registered in the processor's type registry (`sxml_schema`, D165) |
| `schema_assembly` | The read stage's work list of schema documents: each one, known by its absolute URI and target namespace, is read once; xs:include, xs:import, xs:redefine and xs:override add to it (XSD 1.1 Part 1, 4.2; D170) |
| `schema_assertion` | An xs:assert of a complex type or an xs:assertion facet of a simple type: an XPath test that must be true (XSD 1.1 Part 1, 3.13) |
| `schema_aware_test` | A QT4 test case that needs schema awareness: it requires schemaImport, schemaValidation or typedData, or its environment has a schema or a validated source; the dashboard's area "schema-aware" (D174) |
| `schema_compilation` | The state one compilation passes from stage to stage: the configuration, the schema components and the count of errors reported |
| `schema_component` | One of the building blocks of a schema (XSD 1.1 Part 1, 2.2): a type definition, element or attribute declaration, attribute use, model group, particle, wildcard, identity-constraint definition, type alternative, assertion or notation declaration |
| `schema_components` | The store of one schema's components (`sxml_schema_components`, `src/schema_internal.h`, D167): one array per schema table, addressed by index with entry 0 "none", plus the name index and the blocks compile stages allocate |
| `schema_document` | An XML document whose document element is xs:schema (XSD 1.1 Part 1, 3.17.2); read by the XML parser, found through the resource resolver (kind `SCHEMA_DOCUMENT`) |
| `schema_document_source` | Where the configuration's schema document comes from: its text, or the resource resolver |
| `schema_element_role` | The part an element of a schema document plays where it appears (a global or a local element declaration, a group reference, a facet, ...): it decides the element's attributes, its children and the component it is read into (`src/schema_read.c`, D170) |
| `schema_import` | One request for schema components by namespace: XQuery's "import schema" or XSLT's xsl:import-schema, with its location hints or (XSLT) its inline xs:schema element; a stylesheet's schema imports compile into one schema, its in-scope schemas after the host's (`src/xslt_schema.c`, D179) |
| `schema_location_hint` | A URI reference an instance gives for a schema document (XSD 1.1 Part 1, 4.3.2): the second of each pair of an xsi:schemaLocation attribute, for the namespace of the first, or a value of xsi:noNamespaceSchemaLocation, for no namespace; followed only when the host asks (`sxml_schema_configuration.schema_location_hint_node`), and then only for namespaces the schema does not have yet (D278) |
| `schema_name_index` | The hash table from symbol space and expanded name to the global component of that name |
| `schema_normalized_value` | A simple value after the whiteSpace facet of its type (XSD 1.1 Part 2, 4.3.6): what a validated attribute's or simple element's text becomes (XDM 4.0, 7.5.7.4) |
| `schema_parameter` | A sch:param child of sch:schema or of a schematron library (5.4.11): a name and a query fragment, its value attribute or the value the configuration gives instead, replaced as a placeholder in every expression of the schema (D315) |
| `schema_position` | Where a component's XML representation starts: its schema document's index, line and column |
| `schema_reference` | A QName attribute of a schema document (`type`, `base`, `ref`, `substitutionGroup`, `refer`, ...) as read: a symbol space and a name, which the resolve stage turns into a component index |
| `schema_report` | One schema or validation error as a report sink receives it: constraint name, description, location |
| `schema_report_sink` | The interface that receives every schema report of a compilation or validation, so that validation can go on after the first error |
| `schema_scope` | What the languages ask of a static context's in-scope schemas: lookups of types and declarations across them, substitution group members, and schema errors raised again with the language's codes (`src/schema_scope.c`, D174) |
| `schema_table` | One array of the schema components: a component kind (particles, element declarations, ...) or what links them (references, lists, texts, namespace bindings) |
| `schema_test` | An XSD test that compiles the schema documents it names into one schema: valid means it compiles |
| `schema_type` | A type that can annotate a node: complex, atomic, list or union (XDM 4.0, section 4.1.2) |
| `schema_validator` | The tree builder that assesses the events it receives against a schema and passes them on, annotated (type annotation, is-id, is-idrefs, nilled, defaults), to its result builder (D165) |
| `schema_wildcard` | XSD's wildcard component: a namespace constraint, disallowed names and process contents, matching elements or attributes by name (XSD 1.1 Part 1, 3.10); not the XPath `wildcard` name test |
| `schematron` | A compiled Schematron schema (ISO/IEC 19757-3:2025): the schematron patterns of a schematron document that its phase can make active, read into a stylesheet AST and compiled as XSLT, then run over many documents (`sxml_schematron`, include/sxml/schematron.h, D313, D315) |
| `schematron_assertion` | A sch:assert or sch:report: a test and a message; an assert fails when its test is false, a report succeeds when it is true (D315) |
| `schematron_diagnostic` | A sch:diagnostic that an assertion names in its diagnostics attribute: more text, evaluated for the same context item, in each schematron report of it (D315) |
| `schematron_document` | An XML document of a Schematron schema: the one compiled (document element sch:schema) and those its sch:include and sch:extends href elements reach (document element sch:schema or schematron library); read by the XML parser with locations, asked of the resource resolver as XML_DOCUMENT (D315) |
| `schematron_edition` | A schema's schematronEdition attribute (5.5.15): the edition it says it conforms to; any value accepted, never an error, and reported by `sxml_schematron_describe`, the CLI's JSON and SVRL (D315) |
| `schematron_group` | A schematron pattern of the 2025 edition (sch:group, 5.4.5, 6.6) in which a match is attempted against every rule's context, so every matching rule fires, not only the first; SVRL calls it active-group (D315) |
| `schematron_index` | What reading a Schematron schema records once (D323): the schema element, its title, namespaces, schema parameters and variables, phases, schematron patterns, abstract rules and patterns, diagnostics, properties and foreign declarations, each by place, for the build to look up |
| `schematron_library` | A sch:library: a document element holding external declarations (abstract rules, patterns, groups, lets, schema parameters, diagnostics, properties) that a sch:extends href at the top of a schema puts into it in its place (ISO/IEC 19757-3:2025, 5.4.4, 5.4.8; D315) |
| `schematron_outcome` | What a Schematron validation reports at its end: how many findings, how many error findings, and the phase in force (D315) |
| `schematron_pattern` | A sch:pattern or sch:group of a Schematron schema: its rules in order, compiled into one mode of its own; not the XSLT `pattern`, which a rule's context is (D315) |
| `schematron_phase` | A sch:phase: a named set of active patterns with its variables, and optionally a from expression (the nodes its patterns are applied to, 5.5.7) and a when expression (5.5.22); a compilation chooses one, or "#ALL", "#DEFAULT" (the defaultPhase attribute, else "#ALL"), or "#ANY" (D315) |
| `schematron_property` | A sch:property that an assertion names in its properties attribute: a role, a scheme and text, evaluated for the same context item, in each schematron report of it (D315) |
| `schematron_report` | One finding or fired rule as a schematron report sink receives it: kind, id, role, flag, schematron severity, message, schematron diagnostics and properties, the assertion's location in its schematron document and the subject node with its location in the instance (D315) |
| `schematron_report_sink` | The interface that receives every schematron report of one Schematron validation, in order (D315); a compilation's errors go to a schema report sink instead, as XSD's and (from 7.28) XSLT's do |
| `schematron_rule` | A sch:rule: a context (an XSLT pattern, or an expression rule context), optionally a visit-each expression, variables and schematron assertions; compiled into one template rule (D315) |
| `schematron_severity` | The value of an assertion's severity attribute (5.5.16), as written or evaluated; fatal, error, warning and info are reserved; it decides whether a finding is an error finding (D315) |
| `schematron_table` | The static part of a compiled schematron that reports and SVRL read (D323): for each schematron assertion and rule its texts, attributes and place, for each schematron pattern its id, name, role and documents, the phases and their patterns, the schema's title, schemaVersion, schematronEdition and query binding |
| `scratch_array` | A growable array of fixed-size entries in allocator memory for the work lists of a compile stage |
| `seal` | End the building of a unit: nothing in it moves afterwards |
| `seed` | An input a fuzz target starts from, taken from a vendored test suite by `tools/fuzz-seeds.py`; the seeds of one target are its seed corpus |
| `selector` | A non-navigational step from a JNode, or a dynamic node test: its atomized value selects the children whose ·jkey· is atomic-equal to one of its items (XPath 4.0, section 4.7.5.4) |
| `sequence` | An ordered collection of zero or more items |
| `sequence_builder` | Collects items one by one into a new sequence |
| `sequence_cell` | A cell that holds a sequence of two or more items: vector, integer range, concatenation, lazy or sequence slice |
| `sequence_constructor` | The children of a stylesheet element that are evaluated to make a sequence (XSLT 4.0, 5.7) |
| `sequence_generator` | Interface that produces the items of a lazy sequence on demand |
| `sequence_iterator` | Walks the items of a sequence in order |
| `sequence_normalization` | Turning the sequence to serialize into one result tree: JNodes to their values, arrays flattened, atomic items to text, documents replaced by their children, adjacent text merged (Serialization 4.0, section 2) |
| `sequence_normalizer` | Gives the children of the result tree one at a time (`normalized_event`: text or a node to copy), without building the tree |
| `sequence_pattern` | A pattern with an occurrence indicator, or `empty-sequence()`, matching a value inside a map, array or JNode pattern (XSLT 4.0, 6.3.5) |
| `sequence_representation` | How a sequence is stored: empty, single item, vector, integer range, concatenation, lazy, descending integer range |
| `sequence_slice` | A sequence cell for consecutive items of a vector, its `slice_base`, that shares the base's items instead of copying them (`src/xdm_sequence.c`); reported as a vector. "Slice" alone names `fn:slice` and the QT4 test lists (`xquery_slice`) |
| `sequence_type` | An item type plus an occurrence |
| `sequence_view` | A read-only pointer to sequences plus `sequence_count` |
| `serialization` | Turning a sequence into bytes (Serialization 4.0) |
| `serialization_flag` | The value of a boolean serialization parameter: default (not given), no or yes |
| `serialization_frame` | An entry of the serializer's explicit stack: an array, map, JNode or member sequence whose items are being written (D71) |
| `serialization_method` | The Serialization 4.0 output method: adaptive in Phase 1; xml, json, text and others later |
| `serialization_parameter_names` | Fields of `sxml_serialization_parameters` spell the Serialization 4.0 parameter names with `_` for `-` (`omit_xml_declaration`, `use_character_maps`); `serialization_method` is `method`, `character_encoding` is `encoding`; a `has_` field marks a parameter that may be absent |
| `serialization_parameter_reader` | Reads serialization parameters from a parameter document or an options map and keeps what they refer to |
| `serialization_parameter_text` | A serialization parameter given as text, as an output declaration gives it: the parameter's local name and a value in the lexical form of a parameter document (D146) |
| `serialization_parameters` | The Serialization 4.0 parameters of one serialization (method, item separator, ...) |
| `shadow_attribute` | An attribute `_name` whose value, a value template evaluated at compile time, replaces the attribute `name` (XSLT 4.0, 3.11.4) |
| `shared_pattern_variable` | A pattern_variable read where 3.23 does not scope it (by another pattern, a variable outside patterns, an XSLT declaration): a global of its written name refers to the first compiled pattern's let of that name, and each such reference is a compatibility_warning (D331) |
| `side_module` | A WebAssembly module with a `dylink.0` section (WebAssembly tool conventions, "Dynamic linking") that imports the memory, function table and stack pointer of the module it is linked into, and places its data and its functions at the bases it is given (`__memory_base`, `__table_base`) (D293) |
| `side_result` | What a call of the JavaScript shim delivers besides its result, kept by the handle that made it until its next call: a tree an updating query changed, a document fn:put stores, a result document, a message, a schema report (D216) |
| `simple_content` | The string a sequence constructor or value makes for an attribute, comment, PI, namespace or value template: adjacent text nodes merged, zero-length ones dropped, then atomized and joined with a separator (XSLT 4.0, 5.7.2) |
| `simple_content_type` | The simple type of the content of a complex type whose content type variety is simple; it gives the typed value |
| `simple_pattern` | A pattern of an item pattern (`.`, `atomic(...)`, a map, record, array or JNode pattern) and predicates, matching any item (XSLT 4.0, 6.3.3) |
| `simplified_stylesheet` | A stylesheet module whose outermost element is not `xsl:stylesheet`, `xsl:transform` or `xsl:package`; read as one template rule matching `.` (XSLT 4.0, 3.7) |
| `single_item_shortcut` | Matching or coercing no item or one item without the frames of the type matcher or the coercion engine: the count against the occurrence, one item by the item rules alone, one item (or a node's one-item typed value) to an atomic type by the atomic coercion rules, one item to a choice by the first alternative it matches when that one keeps it; whatever they do not decide at once goes to the frames (internal, `src/evaluator_function.c`, D316, D319) |
| `size_budget` | The most bytes a WebAssembly module may have, stripped and gzipped, checked by a ctest (D93) |
| `slice_base` | The vector a sequence slice holds one reference to and reads its items from; a slice of a slice has the same base |
| `slice_baseline` | The number of torture-slice test cases that must pass (`tests/qt4/slice-baseline.txt`); it only goes up |
| `slot` | A numbered place, from 1, in the frame of an IR function, holding one variable's value |
| `snippet` | A fixed XPath text that lowering parses and lowers itself: the parts of built-in templates, the default select of `xsl:apply-templates` |
| `snowball_program` | A Snowball stemming algorithm compiled for sxml's stemming machine: instructions with jumps over the word's code points, its strings, groupings and among tables (snowballstem.org; Full Text 3.0, 3.4.4; D214) |
| `sort_key` | An expression whose value orders the items of a sort, with direction and empty order |
| `sort_key_definition` | One key of `fn:sort-by`: a key function (or `fn:data#1`), a collation and an order direction (F&O 4.0, `fn:sort-key-record`) |
| `sort_key_rule` | A SORT key's collation and direction, resolved once when the SORT is evaluated (D152) |
| `sort_key_value` | The atomized key of one item in `fn:sort`: a sequence of atomic items compared item by item |
| `source_code_language` | A programming or data language that tree-sitter:parse reads (namespace `urn:sxml:tree-sitter`, D307): a vendored tree-sitter grammar (third_party/tree-sitter-grammars) with its highlights query, named as `javascript` or `python` (D285) |
| `source_location` | Source name, line number, column number and byte offset of a position in a source text |
| `source_location_policy` | Which documents sxml parses keep the `source_location` of each node: AUTOMATIC (the zero value: the programs decide), ALWAYS or NEVER; `sxml_processor_configuration.source_location_policy` and the CLI's `--locations` (D313, D318) |
| `source_name` | The name a caller gives a source text: module URI or file name |
| `source_node` | The node of a stylesheet module's tree that a stylesheet AST node was read from |
| `source_span` | Where a syntax node is: byte offset and byte count, with line and column of its first byte; a `source_location` without the source name |
| `space_rule` | One name test of an `xsl:strip-space` or `xsl:preserve-space`, with its import precedence, default priority and declaration order |
| `special_type` | xs:anyType, xs:anySimpleType and xs:anyAtomicType: no list item type, union member or restriction base of a schema's simple type (XSD 1.1 Part 1, 3.16.6.2) |
| `spell_out_rule_set` | A named list of CLDR's rule-based number formatting rules that writes numbers in words or ordinals, such as `spellout-ordinal` (UTS #35, Part 3, section 6) |
| `staged_bytes` | Encoded bytes the serializer collects before one write to the output stream |
| `standard_attribute` | One of the attributes allowed on every XSLT element (`version`, `expand-text`, `use-when`, ...; XSLT 4.0, 3.3) |
| `start_filter` | What a compiled regex knows of where a match can start: the first characters it can read (an ASCII set and whether a non-ASCII one can follow) and whether it can start only at the start of the input; a search skips the start positions it excludes (internal, `src/regex.c`, D310) |
| `start_node` | A node an active pattern is applied to: the instance document root, or under a phase with from each node from selects (5.5.7); an expression rule context is evaluated from each (D315) |
| `starter` | A codepoint of combining class 0 (UAX #15, D107) |
| `static_analysis` | Resolving the names of an AST against the static context and reporting static errors (XPath 4.0, section 2.3.3.1) |
| `static_base_uri` | The base URI of the static context (XPath 4.0, section 2.2.1), against which fn:doc and fn:resolve-uri resolve relative URIs |
| `static_context` | XPath static context |
| `static_error` | A specification error whose code and description are static text, reported without the heap (`sxml_static_error`) |
| `static_expression` | An XPath expression evaluated while the stylesheet is read: `[xsl:]use-when`, the select of a static variable, the value templates of shadow attributes (XSLT 4.0, 9.7) |
| `static_form` | The abstract syntax a function item was made from, compared up to the renaming of bound variables; two function items with the same static form and captured context are equivalent (F&O 4.0, section 1.9.5; D143) |
| `static_namespaces` | The statically known namespaces of a compiled unit beyond the predeclared prefixes, kept for what resolves prefixes at run time (casts to xs:QName) |
| `static_parameter` | A global `xsl:param` with `static="yes"`, given its value before compilation (XSLT 4.0, 9.6) |
| `static_scope` | The static variables and function definitions every static expression of one stylesheet sees (`sxml_xslt_static_scope`) |
| `static_variable` | A global `xsl:variable` or `xsl:param` with `static="yes"`, whose value is known while the stylesheet is read (XSLT 4.0, 9.6) |
| `status` | The result code of a fallible function (`sxml_status`) |
| `step_count` | Instructions a regex matcher has run in one search; `maximum_step_count` bounds it for untrusted regexes |
| `step_grant` | The evaluation steps each element a streamed document reads gives back to the evaluation, so the step bound of a streamed evaluation grows with its input (D295) |
| `stored_namespace` | A namespace node held as a stored node of a tree: only a parentless one, at node index 0 (D98) |
| `stream_availability` | What fn:stream-available finds by a streamed open of a document read to the end of its document element's start tag: available, unavailable, or unknown without a processor's loader (`sxml_stream_availability`, XSLT 4.0, 18.2; D273) |
| `stream_point` | An instruction of a streamed body that selects streamed nodes as their events pass: an xsl:for-each whose select is a downward path, lowered to a chain of STREAM expressions (D219), or an instruction with a streamed path (D223) |
| `stream_tee` | An element whose events the stream builder also sends to a full stream window of its own as they pass, for end rules with capture="yes" (XSLT 4.0, 19.9; D236) |
| `stream_window` | The small tree a node of a streamed document lives in: the document node, the node's ancestors with their attributes and namespaces, the node, and its subtree (a full window) or not (a shallow window) (D219) |
| `streamability` | Whether a construct is guaranteed-streamable by the rules of XSLT 4.0, chapter 19, computed from postures and sweeps |
| `streamed_accumulator_values` | The values of the xslt accumulators a streamed document computes as its events pass, kept by depth for the nodes its stream windows hold now and read by fn:accumulator-before and fn:accumulator-after for those nodes (D231) |
| `streamed_document` | A document read by streamed evaluation: its parser's events are consumed as they arrive, and it is never built as one tree (XSLT 4.0, 18.1; D219) |
| `streamed_iteration` | An xsl:iterate, or an xsl:for-each-group whose groups form as the nodes pass, whose select is a downward path of a streamed body, lowered to a chain of STREAM expressions whose last folds the iteration states: each selected node, in its full window, is given to the iteration's closure as it passes, and a break ends the reading (internal, `src/xslt_lower_stream.c`, D302) |
| `streamed_path` | A downward path in the select expression of an instruction of a streamed body, the rest of the expression motionless around it, lowered in place to a chain of STREAM expressions whose body grounds each selected node: its full window, its atomized value, or only a count (D223) |
| `streamed_principal_source` | The principal source of a transformation given as a URI and read as a streamed document, its document node the initial match selection, when the initial mode is streamed (`streamed_source_uri`, XSLT 4.0, 2.4.3; D228) |
| `streamed_prong` | A prong of an xsl:fork in the body of a streamed xsl:source-document, streamed in a pass of its own over the document: a STREAM with OPENS_DOCUMENT over the same href (D231) |
| `streamed_serialization` | An XML or text serialization fed by tree builder events, writing each start tag, text and end tag as it arrives and holding only the open elements (D231, D295) |
| `streamed_template_rule` | A template rule of a streamed mode whose motionless parameters and body read its node's content only through one stream point, so it runs on the node's shallow window (D228) |
| `streaming_parameter` | The first parameter of a declared-streamable stylesheet function (XSLT 4.0, 19.8.5) |
| `string` | `sxml_string`: well-formed UTF-8 bytes with codepoint count and ASCII flag |
| `string_constructor` | XQuery ``` ``[text `{expression}`]`` ``` |
| `string_template` | XPath 4.0 `` `text {expression}` `` |
| `string_value` | `dm:string-value` of a node |
| `structural_record_type` | A record type matched by its fields (`record(a as xs:int)`) |
| `stylesheet` | An XSLT 4.0 stylesheet: a principal module with every module it includes and imports, compiled together |
| `stylesheet_ast` | What the stylesheet reader builds (`sxml_xslt_ast`): one node per stylesheet element and text node, attributes read into typed values, each expression, pattern, value template and type its own `abstract_syntax_tree`; `xslt_ast` allowed as a module name only |
| `stylesheet_function` | A function an `xsl:function` declares; one IR function per arity of its arity range (XSLT 4.0, 10.3; D150) |
| `stylesheet_level` | A stylesheet module with the modules it includes (`xsl:include`), directly or not; they share one import precedence (XSLT 4.0, 3.10.2) |
| `stylesheet_lowering` | One lowering of a sealed stylesheet AST into an IR unit (`sxml_xslt_lowering`): the XPath static context of each AST static context, the namespace aliases and the instruction rules, around one `lowerer` |
| `stylesheet_module` | One XML document of a stylesheet as reached by one `xsl:include` or `xsl:import`, or the principal module; `module` in `sxml_xslt_ast_module` |
| `stylesheet_outputs` | The compiled output part of a stylesheet: its output definitions by name, its named character maps, and what each `xsl:result-document` instruction reads when it runs (packet 4.13) |
| `stylesheet_parameter` | A global `xsl:param` that is not static, given its value when a transformation is primed (XSLT 4.0, 9.5) |
| `stylesheet_reader` | What reads stylesheet modules into the stylesheet AST (`sxml_xslt_read_stylesheet`, packet 4.5) |
| `sub_picture` | One of the two halves of a `fn:format-number` picture string, for positive or for negative numbers (F&O 4.0, "Syntax of the picture string") |
| `sub_serialization` | Serializing a node into a string inside another serialization (the JSON method's nodes, json-node-output-method) |
| `subject_node` | The node a finding is about: what the assertion's subject attribute, else its rule's, selects for the rule's context node; else the context item when it is a node, else the rule's context node (5.5.19, D315) |
| `subordinate_document` | A document a schematron pattern's documents attribute names (5.4.12): the pattern's rules apply to it instead of the instance, and each of its findings names it, with its own lines and columns (`sxml_schematron_report.document_uri`; D331) |
| `substitution_group` | The element declarations that may appear where a head declaration is expected: those whose substitution group affiliations reach it (XSD 1.1 Part 1, 3.3.6.2) |
| `subtype` | The item type or sequence type relation of XPath 4.0, section 3.3 |
| `subtype_frame` | A conjunction or disjunction of subtype goals on the explicit stack of a subtype search, waiting for the answer of its current sub-goal |
| `subtype_goal` | One question in the and/or tree a subtype question is reduced to: an answer, an item or sequence type pair, or a conjunction or disjunction of sub-goals (D58) |
| `suffix_state` | The content model state that suffix open content moves to once the content may end: it accepts only the open content wildcard |
| `suggestion` | The closest valid name or value to a mistyped one ("did you mean"), found by `edit_distance` |
| `suite_deviation` | A case of a conformance suite whose expected result sxml deliberately does not give, named in the driver's deviation list with the decision record that chose it; counted apart from failures, never reclassified (D178, D323) |
| `suite_path` | The path of a QT4 suite file relative to the directory of `catalog.xml`, with `.` and `..` segments removed |
| `svrl` | The Schematron Validation Report Language (ISO/IEC 19757-3, Annex D, informative): an XML form of one validation's reports; sxml writes it from schematron reports and the compiled schema's static table, never from its own data (D313, D315) |
| `sweep` | How much of a streamed input a construct reads: motionless, consuming or free-ranging (XSLT 4.0, 19.1) |
| `swift_package` | The Swift binding in `bindings/swift/` (module `Sxml`, manifest `Package.swift` at the repository root), importing the host shim through the module map `CSxmlHost` |
| `symbol_sequence` | A numbering sequence with one Unicode symbol per number over a fixed range, named by its symbol for 1 (CIRCLED DIGIT ONE `①`, 0 to 50); numbers outside the range are written in decimal |
| `symbol_space` | One of XSD's seven partitions of global component names: type definitions, element declarations, attribute declarations, model group definitions, attribute group definitions, identity-constraint definitions, notation declarations (XSD 1.1 Part 1, 2.6) |
| `syntax_node` | A node of a tree-sitter syntax tree: named (a rule of the grammar, which becomes an element named by its kind) or anonymous (a token such as `return` or `;`, which becomes text) |
| `syntax_node_kind` | The grammar's name for the type of a syntax node (tree-sitter's "type"), such as `function_declaration`; the local name of its element |
| `synthesized` | Made by the stylesheet reader rather than read from an element (the root and template of a simplified stylesheet) |
| `system_property` | A processor capability or fact, under its F&O 4.0 name (fn:system-properties, 17.1.9) and its XSLT 4.0 name (fn:system-property, 24.2.1); one table holds both |
| `tail_call` | A call whose value is the value of the function making it; the evaluator reuses that function's frame |
| `tail_position` | Where `xsl:next-iteration` and `xsl:break` may stand: last in the body of `xsl:iterate`, or in the tail position of an `xsl:if`, `xsl:when`, `xsl:otherwise`, `xsl:try` or `xsl:catch` that is (XSLT 4.0, 7.2) |
| `tailoring` | Changes to a collation element table for a language or a reordering, compiled from CLDR rules (UTS #35, Part 5) |
| `target_namespace_uri` | The namespace of an XQuery library module that `import module` asks for |
| `target_type` | The type a cast converts to: atomic, list, union, enumeration or choice of atomic types |
| `task_stack` | The evaluator's explicit stack of evaluator tasks, in allocator memory, which replaces recursion on the C stack |
| `tee` | A tree builder that passes each event to two builders in turn |
| `template_function` | The IR function an `xsl:template` (or a built-in template) lowers to: the focus (item, position, size) and the two parameter maps are its five parameters (D144) |
| `template_parameter` | An `xsl:param` of a template; the host passes values to those of the initial template or of the rules of the initial mode |
| `template_rule` | An XSLT rule: a match function, a template function, priority and import precedence |
| `temporary_output_state` | The output state in which `xsl:result-document` is XTDE1480: inside variables, parameters, functions, keys and sort keys, and during a function call invocation (XSLT 4.0, 26.2.6) |
| `tentative_encoding` | An encoding the HTML parser sniffed from octets rather than got from a byte order mark or the encoding option; a meta element that declares another one makes the parser read the document again in that one (HTML Living Standard, 13.2.3.2 and 13.2.3.4; `is_encoding_tentative`) |
| `term` | What a particle contains: an element declaration, a model group or a schema wildcard |
| `terminal` | A leaf of a concrete syntax tree: one token of the source text (XPath 4.0, A.3) |
| `test_budget` | The allocator of one conformance test: passes allocations on, counts live allocations and bytes, refuses any past the byte limit, and turns a leak into an error outcome (`tests/qt4/driver/budget.h`) |
| `test_case` | One named test function within a test suite |
| `test_catalog` | Any conformance driver's catalog as the test selection walks it: named test sets, one open at a time, each with named test cases (an interface over the QT4, XSLT 4.0 and XSD 1.1 catalogs; `tests/qt4/driver/selection.h`) |
| `test_check` | One checked condition within a test case |
| `test_group` | A `testGroup` of an XSD test set: at most one schema test and the instance tests validated against that test's schema (or the built-in components alone when there is none) |
| `test_list` | A file naming QT4 test cases, one `<test-set> <test-case>` per line (`tests/qt4/torture-slice.txt`) |
| `test_outcome` | What running one QT4 test case gave: pass, wrong error (an expected error raised with another code, which passes), fail, error (sxml itself failed: a limit, a leak, a non-specification status) or not run (not applicable, or an assertion the driver cannot check) |
| `test_selection` | The QT4 test cases one driver run covers: a test list, one test set, or every test set |
| `test_set_group` | The directory under `tests/` of the XSLT 4.0 suite that holds a test set (`attr`, `decl`, `expr`, `fn`, `insn`, `misc`, `strm`, `type`, `sandp`); D95 leaves `strm` and `sandp` out of scope |
| `test_suite` | The unit tests of one module: one test executable |
| `text_declaration` | The `<?xml version? encoding?>` at the start of an external entity, with a required encoding (XML 1.0, section 4.3.1) |
| `text_decoding` | Turning the octets of a text resource or a binary value into a string: the effective encoding and start offset chosen from a byte order mark and an encoding candidate, then the characters (`sxml_text_decode`; F&O 4.0, 17.1.5; EXPath Binary 4.0, bin:infer-encoding) |
| `text_form` | The printed form of a unit, for golden tests and debugging |
| `text_form_writer` | The bounded writer every text form writes through: it writes into caller memory what fits and counts every byte, so a caller can ask for the size first (`sxml_text_form_writer`; D249) |
| `text_run` | A terminal node of an ixml parse tree for consecutive characters of the input that the XML keeps: one node for the text of a regular token, and for neighbouring kept terminals (D290) |
| `text_store` | The blocks of an arena tree that hold node content (text, attribute values, comments, ...) back to back |
| `text_window` | The part of a document's decoded UTF-8 text the XML parser holds: decoded from the input a part at a time as document segments need it, the text before the segment being parsed given up (`sxml_xml_text_window`, D233) |
| `thesaurus_expansion` | The query strings a thesaurus gives one query string: the string itself and the terms related to it, within the relationship and levels asked for, each matched as an alternative (Full Text 3.0, 3.4.3; D214) |
| `timezone_database` | Zones of the IANA Time Zone Database: for a zone name and an instant, the offset and abbreviation in effect |
| `timezone_offset_minutes` | A timezone as minutes east of UTC, from -840 to 840 |
| `timezone_rule` | A TZ string read (POSIX XBD 8.3, the footer of a TZif file): a zone's standard time and its daylight saving time with when it starts and ends each year, for instants after the zone's last transition (D209) |
| `token_automaton` | The deterministic automaton of a regular token: a state is a set of states of the token's nondeterministic automaton, each with the number of ways it is reached, one or more; an end reached more than one way is an ambiguous completion (D290) |
| `token_element` | An `sxml:token` element: an anonymous syntax node that has a highlight, a field name or is missing, so it can carry attributes (D285) |
| `trace_event` | One piece of trace output: its kind, label, value and, for `xsl:message`, whether it terminates |
| `trace_sink` | Host interface receiving the output of `fn:trace`, `fn:message` and `xsl:message` |
| `transcoding` | Converting a document from its character encoding into UTF-8 before parsing |
| `transformation` | One run of a compiled stylesheet: priming it (stylesheet parameters, global context item) and invoking it once (XSLT 4.0, 2.4) |
| `traversal` | A higher-order function's walk over the items of a sequence, members of an array or entries of a map, asking for one call of a function item per position (`fn:for-each`, `fn:filter`, `map:for-each`, ...) |
| `traversal_event` | A start or an end visit of a node in the traversal of a tree for an xslt accumulator, the start before the node's descendants and the end after them (XSLT 4.0, 19.4) |
| `tree` | One XML document or fragment, stored in an arena |
| `tree_builder` | Interface receiving construction events in document order (parser, constructors, serializers) |
| `tree_identifier` | Identifies a tree; with `node_index`, identifies a node |
| `tree_operations` | The operations table a tree implementation supplies |
| `tree_owner` | Interface told when a tree is no longer referenced, so its owner can free it |
| `trial` | In an unordered deep-equal comparison, the comparison of one unit with one candidate; a difference inside it moves on to the next candidate |
| `tunnel_parameter` | A template parameter with `tunnel="yes"`, passed on through every template to the ones that declare it (XSLT 4.0, 10.1.6) |
| `tuple_array` | One tuple of a FLWOR as an array: one member per live variable bound to a slot (D55) |
| `tuple_layout` | Which variable each member of the tuple arrays holds, and the variables (constants, globals) that are not packed |
| `tuple_stream_clause` | A FLWOR clause that works on the whole tuple stream as a value: order by, count, while and group by (XQuery 4.0, section 4.13; D55); a window clause is a clause of a segment, like for (D137) |
| `type_alternative` | One entry of an element declaration's type table: an XPath test and the type it selects (XSD 1.1 Part 1, 3.12) |
| `type_annotation` | The type identifier an atomic item, node or record is annotated with |
| `type_description` | What the type registry reports about one type: variety, name, base type and variety-specific parts |
| `type_identifier` | Index of a type in the type registry |
| `type_match_frame` | An entry on the explicit stack of matching a value against a sequence type (instance of, treat as, coercion) |
| `type_registry` | The table of built-in and schema-derived types |
| `type_table` | An element declaration's type alternatives with a test and its default type definition (the last alternative without a test, else the declared type); two are compared by equivalence (XSD 1.1 Part 1, 3.3.2.2, 3.8.6.3) |
| `type_variety` | What kind of type a type identifier names: complex, atomic, list, union, or a kind of item type |
| `typed_value` | `dm:typed-value` of a node |
| `u_type` | A set of fundamental item types (node kinds, primitive atomic types, function items), the static type of streamability analysis (XSLT 4.0, 19.1) |
| `uca_fallback` | sxml's stand-in for a UCA collation URI: the levels of UTS #10 over weights derived from Unicode properties, without DUCET tables (D97, D116) |
| `unexpanded_declaration` | A markup declaration of external DTD text with parameter-entity references outside its literals, which matches its production only after the references are replaced (XML 1.0, section 4.4.8) |
| `unit` | Everything one compilation produces in the IR: functions, expressions, constants, global variables, modes; reference counted (`sxml_ir_unit`) |
| `unit_evaluation` | One evaluation of a sealed IR unit with the host's dynamic context: what the XPath and XQuery APIs share (`sxml_evaluate_unit`, D146); a native step can ask the machine for one of another unit's main function, its global table first, and hand the value to the next step (D154) |
| `unit_evaluation_extension` | What a language adds to the shared evaluation of a unit (`src/xpath_api_internal.h`): XSLT's function table, document preparation and result writer (D157); a unit evaluation a native step asks for carries one in its global table, which the unit's code reads (fn:transform, D203) |
| `unix_time` | Milliseconds since 1970-01-01T00:00:00Z, leap seconds not counted (fn:unix-dateTime) |
| `unnamed_child` | A named concrete syntax tree child that its grammar gives no field (XQuery's module); a parser part names its AST field |
| `unnamed_decimal_format` | The decimal format without a name, used when `fn:format-number` names none |
| `unparsed_entity` | An unparsed entity of a document: system and public identifier |
| `unpermitted_escape` | A JSON escape sequence for a codepoint that is not a `permitted_character` (U+0000, an unpaired surrogate, U+FFFE, U+FFFF); what the `fallback` option of `fn:parse-json` is called with (D143) |
| `update` | Describes one change to a persistent map or array (`map_update`, `array_update`) |
| `update_primitive` | One change of a pending update list: insert, delete, replace node, replace value, rename, put (XQuery Update Facility 3.0, "update primitives") |
| `update_sink` | The host interface that receives each tree an updating query changed, as its old and new roots (`sxml_xquery_update_sink`, D187) |
| `updated_tree` | The new tree that applying a pending update list rebuilt from an old one, with every change made (D187) |
| `updating_expression` | An expression whose value is a pending update list: insert, delete, replace, rename, an updating function call, and the expressions that pass one on (XQuery Update Facility 3.0, 2.2); its opposite is a `simple_expression` |
| `uri_collection` | The sequence of `xs:anyURI` items `fn:uri-collection` returns for a URI |
| `uri_escaping` | NFC and %-escaping of non-ASCII characters in a URI attribute value of an HTML element (Serialization 4.0, section 4, rule a; appendix D) |
| `uri_reference` | A URI or relative reference as written, split into scheme, authority, path, query and fragment (RFC 3986, section 4.1) |
| `uri_resolution` | Resolving a URI reference against a base URI (RFC 3986, section 5.2) |
| `uri_structure` | The parts of a URI as `fn:parse-uri` finds them and `fn:build-uri` joins them: the fields of `fn:uri-structure-record` (F&O 4.0, section 7.6.1) |
| `usage_error` | A command line that does not match the table: unknown, missing, repeated or extra arguments |
| `used_package` | The package an `xsl:use-package` names, read again for every use, so each use has components of its own (XSLT 4.0, 3.4.2) |
| `utf8` | UTF-8 encoded text, as a byte view; allowed short form |
| `vacuous` | Of an item: a zero-length text node, a document node without children, a zero-length atomic item, or an array whose flattened members all are (XSLT 4.0, 8.5.2) |
| `vacuous_expression` | `()`, a call of fn:error, or an expression made only of those: allowed where an updating or a simple expression is required (XQuery Update Facility 3.0, 2.2) |
| `validation_frame` | The schema validator's entry for one open element: its assessment, governing declaration and type, content type, content model cursor, nilled and error state, and where its collected text and namespace bindings start |
| `validation_mode` | How the validation root is assessed when no governing type is given: strict (a declaration must exist) or lax (XSD 1.1 Part 1, 5.2) |
| `validation_outcome` | What a schema validator reports once its tree has ended: the validity of the validation root and the count of errors |
| `validation_root` | The element (or attribute) where one validation starts: its subtree is assessed, and IDs and IDREFs are checked within it (XSD 1.1 Part 1, "validation root", cvc-id) |
| `validity` | The outcome XSD gives an assessed item (XSD 1.1 Part 1, the [validity] property): valid, invalid or not known |
| `validity_constraint` | A rule of XML 1.0 marked "VC" that only a validating processor checks; sxml checks those no schema component expresses in the parser and the rest with the schema validator (D199) |
| `value_comparison` | `eq`, `ne`, `lt`, `le`, `gt`, `ge` on two single atomic items, through fn:compare (XPath 4.0, section 4.11.1) |
| `value_constraint` | The default or fixed value of an element or attribute declaration or an attribute use, as its lexical form (XSD 1.1 Part 1, 3.3.1) |
| `value_order` | XSD's order of two values of one primitive type: less, equal, greater or incomparable (partial for dates, times and durations, NaN incomparable; XSD 1.1 Part 2, 2.2.3) |
| `value_template` | An attribute value template or text value template: fixed text with enclosed XPath expressions in `{ }` (XSLT 4.0, 5.6) |
| `variable_binding` | A variable name with its value, in the dynamic context |
| `variable_binding_element` | An `xsl:variable`, `xsl:param` or `xsl:with-param` element, whose value comes from its select, its content or neither (XSLT 4.0, 9.3, "variable-binding elements"); its binding role says which type error its declared type raises (XTTE0570 or XTTE0590) |
| `variable_declaration` | A variable name with its declared type, in the static context (an external variable) |
| `variable_marker` | A part of a date picture string in square brackets, replaced by a date or time component (F&O 4.0, section 9.9.4.1) |
| `variable_part` | `{ Expr? }` in a value template; empty, whitespace or comments only give a zero-length string (XSLT 4.0, 5.6) |
| `variable_scope` | The variables visible at one point of an expression, innermost binding first |
| `verifier` | The pass that checks the invariants of a unit and changes nothing |
| `version_token` | A token of an XSD test suite `version` attribute (`1.1`, `XML-1.0-5e`, `full-xpath-in-CTA`): a test set, group or test applies when, for each token class it names, it names a token the driver supports; an `expected` applies when the driver supports all its tokens |
| `visibility` | Public, private, final, abstract or hidden: who may refer to or override a component instance (XSLT 4.0, 3.4.5.1) |
| `visit_each_item` | An item a rule's visit-each expression gives for one context node (5.5.21): the context item of the rule's assertions in place of the context node (the rule's variables are evaluated once per context node, before); with none, the rule has fired and no assertion is evaluated (D315) |
| `void_element` | An HTML element with an empty content model, written as a start tag alone (`<br>`; Serialization 4.0, 7.1) |
| `well_known_namespace` | A namespace with a fixed `namespace_uri_identifier` in every name pool (`SXML_NAMESPACE_XML_SCHEMA`) |
| `whitespace_facet` | How a type treats whitespace before parsing a lexical form: preserve, replace or collapse (XSD 1.1 Part 2, section 4.3.6) |
| `whitespace_stripping` | Removing the whitespace text nodes that `xsl:strip-space` names from a copy of a source document (XSLT 4.0, 4.2.2; D150) |
| `width_modifier` | The `,min-max` part of a variable marker: the minimum and maximum number of characters |
| `wildcard` | A name test with `*` for the namespace (`*:local`) or the local name (`prefix:*`, `Q{uri}*`) |
| `window_array` | One window of a window clause as an array of nine members: the window, then the start and end items, their positions, previous and next items (D137) |
| `with_parameter` | The value one `xsl:with-param` supplies, with its name and whether it is a tunnel parameter |
| `writer` | An output stream plus the status of its first failed write |
| `written_bindings` | The namespace bindings already declared for the open elements, against which an element's declarations and undeclarations are computed |
| `xinclude` | XInclude processing: copying a document with each `xi:include` element replaced by the document or text its `href` names, or by its `xi:fallback`'s children when that cannot be read; the `xinclude` option of fn:doc and fn:parse-xml (XInclude 1.0) |
| `xml_island` | An element the HTML method writes as XML because it is not an HTML element (Serialization 4.0, 7.1) |
| `xml_open_content` | What the declaration of an open element says of its content, kept by a validating parse for the validity constraints on content |
| `xml_parser` | Reads XML 1.0 bytes and sends their tree builder events (`include/sxml/xml.h`) |
| `xml_representation_of_json` | The elements `map`, `array`, `string`, `number`, `boolean` and `null` in the fn namespace, with `key`, `escaped` and `escaped-key` attributes, that stand for a JSON text (F&O 4.0, section 17.4.2) |
| `xml_version` | The version of XML a document follows, 1.0 or 1.1, read from its XML declaration (`sxml_xml_version`; XML 1.1, section 4.3.4); it decides the characters allowed, the line ends and whether prefixes may be undeclared |
| `xml_writer` | Writes nodes by the XML or XHTML method: open elements and written bindings on stacks (D71) |
| `xnode_pattern` | A pattern made of paths, steps, unions, intersect and except, matching XNodes (XSLT 4.0, 6.3.2) |
| `xpath_1_compatibility_mode` | The static context setting under which comparisons, arithmetic and function arguments follow XPath 1.0's conversions (XPath 4.0, section 2.2.1); XSLT sets it for elements whose effective version is below 2.0 |
| `xpath_expression` | An XPath expression compiled for one processor, evaluated any number of times |
| `xpath_record` | XSD's XPath Expression property record: the expression text with its namespace context, and its compiled form once a stage compiles it (XSD 1.1 Part 1, 3.13.1) |
| `xquery_query` | An XQuery main module compiled for one processor, with the library modules it imports, evaluated any number of times |
| `xquery_slice` | The QT4 test cases of `tests/qt4/xquery-slice.txt`, run with `--spec=XQ40` by ctest `qt4/xquery-slice` against `tests/qt4/xquery-slice-baseline.txt`, which only goes up (packet 3.13) |
| `xsd_pattern` | A regular expression of XSD's own dialect (XSD 1.1 Part 2, appendix G), the value of a pattern facet: anchored to the whole value, without F&O's additions; the regex engine's `is_xsd_pattern` flag (D53, D171) |
| `xsd_test_case` | A schema test or an instance test of an XSD test set, named `GROUP/TEST` in test lists and reports |
| `xsd_test_catalog` | The W3C XSD test suite's `suite.xml` (`third_party/xsdtests/`) as the driver holds it: its test-set references and one open test set |
| `xsd_test_driver` | The XSD 1.1 test-suite driver: the developer tool `sxml-xsd-test` under `tests/xsd/driver/`, a sibling of the QT4 and XSLT drivers that links the QT4 driver's library (packet 5.3) |
| `xsd_test_set` | One `testSet` file of the XSD test suite, named in reports by its directory without `Meta` and its file name without extension (`saxon-Assert`, `ms-DataTypes_w3c`); the directory is its contributor area |
| `xslt_accumulator` | An `xsl:accumulator`: a value carried through a traversal of a tree, giving every node but attributes and namespaces a pre-descent and a post-descent value (XSLT 4.0, 19). In code always `xslt_accumulator`; a bare `accumulator` is a fold's (D155) |
| `xslt_function_table` | The function table put in front of every static context of a stylesheet: stylesheet functions lowered into the unit (fn:key, fn:copy-of), then the XSLT-only library parts (D149) |
| `xslt_test_case` | One `<test-case>` of an XSLT 4.0 test set: environment, stylesheets and packages, parameters, invocation, output, dependencies, result |
| `xslt_test_catalog` | The XSLT 4.0 suite's `catalog.xml` (`third_party/xslt40-test/`) as the driver holds it: its test-set entries and one open test set |
| `xslt_test_driver` | The XSLT 4.0 test-suite driver: the developer tool `sxml-xslt-test` under `tests/xslt/driver/`, a sibling of the QT4 driver that links its library (D117) |
| `xslt_test_environment` | The environment of an XSLT 4.0 test case: what a QT4 environment holds, plus inline source documents, stylesheets, packages, static parameters and an output element |
| `xslt_test_execution` | Running one planned XSLT 4.0 test case through `include/sxml/xslt.h` in a processor of its own and checking its assertions (`tests/xslt/driver/xslt_execution.h`, D157) |
| `xslt_test_plan` | What the transformation of one XSLT 4.0 test case needs, read from the catalog alone: principal module, secondary modules, static and stylesheet parameters, global context item, invocation kind, result delivery |
| `zone_name` | An IANA timezone name such as `Europe/Berlin` (an "Olson name"), a place of fn:civil-timezone and the formatting functions |
