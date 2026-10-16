# Glossary

One spelling per concept. Identifiers use these words exactly. Add a term here
before using it in code. Terms follow the XDM 4.0 and XPath 4.0 specifications.

| Term in code | Meaning |
| --- | --- |
| `item` | One XDM item: a node, an atomic item, or a function item (maps and arrays are function items) |
| `sequence` | An ordered collection of zero or more items |
| `atomic_item` | A value of an atomic type together with its type annotation (XPath 4.0 says "atomic item", not "atomic value") |
| `type_identifier` | Index of a type in the type registry |
| `type_registry` | The table of built-in and schema-derived types |
| `function_item` | A callable item, including maps, arrays and closures |
| `map` | An XDM 4.0 map; entries keep insertion order |
| `map_entry` | One key/value pair of a map |
| `array` | An XDM array; members are sequences |
| `array_member` | One member of an array |
| `record` | A map whose keys are fixed by a record type |
| `node` | An XML tree node (an XNode in XPath 4.0 terms) |
| `json_node` | A JNode: a wrapper that lets maps and arrays be navigated with axes |
| `node_index` | Index of a node within its tree's arena |
| `tree` | One XML document or fragment, stored in an arena |
| `tree_identifier` | Identifies a tree; with `node_index`, identifies a node |
| `document_order` | The total order of nodes defined by XDM |
| `expanded_name` | Namespace URI plus local name |
| `qualified_name` | Prefix plus local name, as written (the QName); `qname` allowed |
| `namespace_uri` | The namespace part of an expanded name |
| `local_name` | The local part of a name |
| `namespace_prefix` | The prefix part of a qualified name |
| `byte_count` | A length measured in bytes |
| `codepoint_count` | A length measured in Unicode codepoints |
| `byte_view` | A read-only pointer plus `byte_count` |
| `memory_view` | A writable pointer plus `byte_count` |
| `allocator` | The memory interface: state plus operations table |
| `null_allocator` | An `allocator` whose operations is NULL: it never allocates; in a configuration it means "must not grow" |
| `default_allocator` | The allocator over the C library heap |
| `backing_allocator` | The allocator another allocator passes its requests on to |
| `counting_allocator` | The allocator test double: counts allocations and fails a chosen one |
| `allocation_request` | Byte count and alignment of one allocation |
| `allocation_number` | Position of an allocate call among all calls to one allocator, from 1 |
| `alignment` | A power of two that an address must be a multiple of |
| `arena` | Bump allocator whose allocations are released together |
| `block` | One piece of memory an arena takes from its block allocator |
| `byte_range` | A byte offset plus a byte count within a view |
| `byte_offset` | A position measured in bytes from the start of something |
| `operand` | One input of a checked arithmetic function (`left_operand`, `right_operand`) |
| `decimal` | An `xs:decimal` value, or an `xs:integer` beyond `int64_t`, as a libmpdec number (`sxml_decimal`, `src/decimal_internal.h`; D36, D42) |
| `decimal_scope` | The thread-local allocator pointer libmpdec allocates through during one decimal operation (D42) |
| `digit_count` | A number of decimal digits (`maximum_digit_count`, `division_digit_count`) |
| `canonical_text` | The canonical lexical form of a value as UTF-8 bytes (F&O 4.0, section 19.1.2, casting to `xs:string`) |
| `rounding_mode` | How a value is rounded to a precision: the `$rounding-mode` of `fn:round` (F&O 4.0) |
| `precision` | In rounding, the number of digits kept after the decimal point; negative rounds to a power of ten (`fn:round`) |
| `memory_requirements` | Size and alignment an object needs |
| `configuration` | Struct describing how an object is set up |
| `request` | Struct describing one operation |
| `status` | The result code of a fallible function (`sxml_status`) |
| `input_output` | Reading or writing bytes through the platform; spelled `input_output`, never `io` (`SXML_STATUS_INPUT_OUTPUT_FAILED`) |
| `error_record` | A specification error code (such as `FOAR0001`) with details |
| `diagnostic` | Caller-supplied record receiving position and context of a failure |
| `error_code` | The expanded QName of a specification error, such as `err:FOAR0001` |
| `error_value` | The optional sequence that an error carries (`$err:value`) |
| `source_location` | Source name, line number, column number and byte offset of a position in a source text |
| `source_name` | The name a caller gives a source text: module URI or file name |
| `line_number` | Line of a source position, from 1; 0 means unknown |
| `column_number` | Column of a source position in codepoints, from 1; 0 means unknown |
| `codepoint` | A Unicode scalar value, held in `uint32_t` |
| `string` | `sxml_string`: well-formed UTF-8 bytes with codepoint count and ASCII flag |
| `utf8` | UTF-8 encoded text, as a byte view; allowed short form |
| `is_ascii` | The flag of a string whose bytes are all below 0x80 (D6) |
| `codepoint_iterator` | Walks the codepoints of a string in order |
| `lead_byte` | The first byte of the UTF-8 encoding of one codepoint |
| `continuation_byte` | A later byte of the UTF-8 encoding of one codepoint, 0x80 to 0xBF |
| `encoded_byte_count` | The number of bytes in the UTF-8 encoding of one codepoint, 1 to 4 |
| `ordering` | Result of a three-way comparison: less, equal or greater |
| `placement` | The memory an object is placed in by `initialize` |
| `static_context` | XPath static context |
| `dynamic_context` | XPath dynamic context |
| `focus` | Context item, context position and context size together |
| `context_item` | The item the focus is on |
| `intermediate_representation` | The shared IR; `ir` allowed as a module name only |
| `grammar` | A tree-sitter grammar under `grammars/<language>/`, and the parser generated from it |
| `concrete_syntax_tree` | The tree-sitter parse tree of a query or document, before sxml builds its own structures |
| `query_scanner` | The external tree-sitter scanner shared by the XPath, XQuery and XSLT pattern grammars |
| `keyword_name` | A keyword used as a name; XPath 4.0 keywords are not reserved |
| `direct_constructor` | XQuery XML-like node constructor syntax: `<a b="{1}">text</a>`, `<!-- -->`, `<?pi?>` |
| `computed_constructor` | Node constructor written with a keyword: `element #a { ... }` |
| `string_template` | XPath 4.0 `` `text {expression}` `` |
| `string_constructor` | XQuery ```` ``[text `{expression}`]`` ```` |
| `pragma` | XQuery `(# name contents #)` before an extension expression |
| `pattern` | An XSLT 4.0 match pattern (XSLT 4.0, section 6.3) |
| `front_end` | The code that turns the text of one language (XPath, XQuery, an XSLT pattern) into an abstract syntax tree |
| `abstract_syntax_tree` | What a front end builds from a concrete syntax tree: one node per visible grammar production, children by grammar field, names as written, literals decoded; `ast` allowed as a module name only (`sxml_ast_node`) |
| `ast_field` | The role of a child in its AST parent: a field name of the grammar (`left`, `predicate`, `argument`) |
| `source_span` | Where a syntax node is: byte offset and byte count, with line and column of its first byte; a `source_location` without the source name |
| `operator` | The token that selects what an operator expression computes (`SXML_AST_OPERATOR_PLUS`); in the IR, what a binary or unary expression computes |
| `test_suite` | The unit tests of one module: one test executable |
| `test_case` | One named test function within a test suite |
| `test_check` | One checked condition within a test case |
| `name_pool` | Per-processor table that interns namespace URIs, prefixes and expanded names into small identifiers |
| `name_identifier` | Identifier of an interned expanded name (namespace URI plus local name) in a name pool |
| `namespace_uri_identifier` | Identifier of an interned namespace URI; 0 is "no namespace" |
| `namespace_prefix_identifier` | Identifier of an interned prefix; 0 is "no prefix" |
| `name_parts` | The namespace URI identifier and local name of a name identifier, as returned by the name pool |
| `builtin_type` | A type predefined by XSD, XDM or XPath, with a constant type identifier (`SXML_TYPE_XS_INTEGER`) |
| `schema_type` | A type that can annotate a node: complex, atomic, list or union (XDM 4.0, section 4.1.2) |
| `type_variety` | What kind of type a type identifier names: complex, atomic, list, union, or a kind of item type |
| `type_description` | What the type registry reports about one type: variety, name, base type and variety-specific parts |
| `base_type` | The type a schema type is derived from by restriction, list or union |
| `primitive_type` | The primitive atomic type an atomic type is derived from (`xs:integer` gives `xs:decimal`) |
| `is_derived_from` | Schema type derivation: the base type chain reaches the other type (XSD 1.1 Part 1, section 3.16.6) |
| `subtype` | The item type or sequence type relation of XPath 4.0, section 3.3 |
| `item_type` | A class of items, such as `xs:integer`, `element(a)` or `map(xs:string, item())` |
| `sequence_type` | An item type plus an occurrence |
| `occurrence` | How many items a sequence type allows: zero, one, zero or one, zero or more, one or more |
| `record_type` | An item type whose instances are records; a list of fields |
| `record_field` | One field of a record type: a name and a sequence type |
| `structural_record_type` | A record type matched by its fields (`record(a as xs:int)`) |
| `nominative_record_type` | A named record type matched only by records annotated with it |
| `enumeration_type` | An item type matching a fixed set of strings (`enum("a", "b")`) |
| `choice_item_type` | An item type matching items of any of its alternatives (`(A \| B)`) |
| `function_type` | The item type of function items with given parameter and result types; a function's signature |
| `node_test` | An item type over XNodes: a node kind, an optional name and an optional type annotation |
| `json_node_test` | An item type over JNodes: `jnode(key, value type)` |
| `type_annotation` | The type identifier an atomic item, node or record is annotated with |
| `item_heap` | Per-processor owner of the reference-counted cells that items and sequences point to |
| `cell` | A reference-counted, immutable heap object holding the data of an item or sequence that does not fit inline |
| `retain` / `release` | Add / drop one reference to an item, sequence or tree |
| `immortal` | A cell or tree that is never released (static data); retain and release do nothing to it |
| `item_kind` | Atomic item, node (XNode), JNode, function item, map or array |
| `generic_node` | A GNode: a node (XNode) or a JNode (XDM 4.0, section 6) |
| `datum` | The value of an atomic item, without its type annotation (XDM 4.0, section 5) |
| `datum_kind` | The C representation a datum is read and written in (boolean, integer, string, ...) |
| `date_time` | The seven components of a date/time datum (XDM 4.0, section 5.4) |
| `duration` | A duration datum: months plus seconds and nanoseconds (XDM 4.0, section 5.5) |
| `timezone_offset_minutes` | A timezone as minutes east of UTC, from -840 to 840 |
| `atomic_equal` | The context-free equality of map keys (F&O 4.0, section 2.2.1 `fn:atomic-equal`) |
| `atomic_hash` | A hash consistent with `atomic_equal` |
| `sequence_representation` | How a sequence is stored: empty, single item, vector, integer range, concatenation, lazy |
| `item_count` | Number of items: `uint64_t` for a sequence (a range may hold more than fits in memory), `size_t` in an `item_view` |
| `item_index` | Position of an item in a sequence, from 0 |
| `item_view` | A read-only pointer to items plus `item_count` |
| `sequence_view` | A read-only pointer to sequences plus `sequence_count` |
| `sequence_iterator` | Walks the items of a sequence in order |
| `iteration_step` | What one call to an iterator yields: whether there was an item, and the item |
| `sequence_builder` | Collects items one by one into a new sequence |
| `sequence_generator` | Interface that produces the items of a lazy sequence on demand |
| `entry_index` | Position of an entry in a map's entry order, from 0 |
| `duplicate_key_policy` | What building a map does with keys that are atomic-equal: reject, keep first, keep last |
| `member_index` | Position of a member in an array, from 0 |
| `field_index` | Position of a field in a record type, from 0 |
| `arity` | Number of parameters of a function item |
| `function_annotation` | An annotation name with a sequence of atomic items (`%private`) |
| `function_body` | Interface that runs a function item: native C code or an evaluator for the IR |
| `function_call` | What a function body receives: the function item, the arguments, the dynamic context |
| `captured_value` | A nonlocal variable value a function item keeps (its closure) |
| `json_key` | The ·jkey· of a JNode: the map key or 1-based array or sequence position under its parent |
| `json_value` | The ·jvalue· of a JNode: the value it wraps |
| `json_tree` | A JTree: the JNodes reachable from one root JNode |
| `child_index` | Position of a child among the children of its parent, from 0 |
| `node_kind` | One of the seven XNode kinds: document, element, attribute, text, comment, processing instruction, namespace |
| `node_reference` | A tree plus a node index (plus a namespace number for namespace nodes); identifies a node |
| `namespace_number` | Which in-scope namespace of an element a namespace node is, from 1; 0 for other nodes |
| `node_description` | What a tree reports about one node: kind, name, parent, siblings, children, type annotation, flags |
| `last_descendant` | The node index of the last node in a node's subtree (the node itself when it has none) |
| `tree_operations` | The operations table a tree implementation supplies |
| `tree_owner` | Interface told when a tree is no longer referenced, so its owner can free it |
| `arena_tree` | The built-in tree: nodes in arrays indexed by node index, built by a tree builder |
| `tree_builder` | Interface receiving construction events in document order (parser, constructors, serializers) |
| `event` | One step given to a tree builder: start element, attribute, text, ... |
| `namespace_binding` | A prefix bound to a namespace URI |
| `in_scope_namespaces` | The namespace bindings in effect on an element (XDM 4.0, `dm:namespace-nodes`) |
| `string_value` | `dm:string-value` of a node |
| `typed_value` | `dm:typed-value` of a node |
| `base_uri` | `dm:base-uri` of a node |
| `document_uri` | `dm:document-uri` of a document node |
| `unparsed_entity` | An unparsed entity of a document: system and public identifier |
| `byte_view_list` | A read-only pointer to byte views plus `byte_view_count` |
| `processor` | The object (Phase 1) that owns one name pool, type registry and item heap and compiles and runs queries |
| `limits` | A struct of bounds on work and memory for untrusted input (`sxml_processor_limits`, `sxml_xml_limits`) |
| `xml_parser` | Reads XML 1.0 bytes and sends their tree builder events (`include/sxml/xml.h`) |
| `encoding_name` | The name of a character encoding (`UTF-8`, `ISO-8859-1`), from outside a document or from its encoding declaration |
| `entity_expansion` | The text that replacing entity references produces (XML 1.0, section 4.4); bounded for untrusted input |
| `resource` | What a resource resolver delivers for a URI: bytes, a document node, or nothing |
| `resource_resolver` | Host interface that retrieves resources by URI, for `fn:doc` and, later, text, modules and stylesheets |
| `resource_kind` | What a resource is wanted for: an XML document for `fn:doc`, ... |
| `resource_form` | How a resolver delivers a resource: none, bytes, or a document node |
| `resource_entry` | One URI and its bytes in a list served by the table resolver |
| `uri_reference` | A URI as written, possibly relative (RFC 3986, section 4.1) |
| `absolute_uri` | A URI with a scheme, after resolving a reference against a base URI (RFC 3986, section 5) |
| `clock` | Host interface that reads the current date and time and the implicit timezone |
| `clock_reading` | What one reading of a clock gives: the current dateTime and the implicit timezone |
| `current_date_time` | The instant `fn:current-dateTime` returns, read once per evaluation |
| `implicit_timezone` | The timezone of the dynamic context, used for date/time values without one |
| `context_value` | The XPath 4.0 context value: the sequence the focus is on; when it is one item, the context item |
| `variable_declaration` | A variable name with its declared type, in the static context (an external variable) |
| `variable_binding` | A variable name with its value, in the dynamic context |
| `declared_type` | The sequence type a variable or parameter is declared with |
| `available_document` | A URI and the document node `fn:doc` returns for it (XPath 4.0, "available documents") |
| `xpath_expression` | An XPath expression compiled for one processor, evaluated any number of times |
| `expression_text` | The source text of an expression or query |
| `evaluation` | One run of a compiled expression or query against a dynamic context |
| `evaluation_depth` | How deeply evaluation nests (function calls, nested expressions); bounded so deep recursion is a limit, not a stack overflow |
| `expression_depth` | How deeply the syntax of an expression nests; bounded for untrusted expressions |
| `serialization` | Turning a sequence into bytes (Serialization 4.0) |
| `serialization_method` | The Serialization 4.0 output method: adaptive in Phase 1; xml, json, text and others later |
| `serialization_parameters` | The Serialization 4.0 parameters of one serialization (method, item separator, ...) |
| `item_separator` | The text written between the items of a serialized sequence |
| `output_stream` | Interface that receives bytes, written in order |
| `output_buffer` | The built-in output stream: collects bytes in memory |
| `well_known_namespace` | A namespace with a fixed `namespace_uri_identifier` in every name pool (`SXML_NAMESPACE_XML_SCHEMA`) |
| `axis` | An XPath axis (`SXML_AXIS_CHILD`, ...), walked by an `axis_iterator` |
| `axis_iterator` | Walks the nodes of one axis from an origin node |
| `integer_range` | The bounds of a range sequence (`first_integer` to `last_integer`) |
| `pair` | Two values a function relates or compares (`type_pair`, `item_pair`, `node_pair`) |
| `update` | Describes one change to a persistent map or array (`map_update`, `array_update`) |
| `name_test` | Which names a node test or path step matches: any name, one expanded name, or a wildcard |
| `wildcard` | A name test with `*` for the namespace (`*:local`) or the local name (`prefix:*`, `Q{uri}*`) |
| `duplicate_attribute_policy` | What a tree builder does with a second attribute of the same name: reject (XQuery) or replace (XSLT) |
| `raised_error` | The error an item heap keeps for an error record whose description or value was computed (`sxml_item_heap_raise_error`) |
| `error_status` | The status an error is raised with: static, dynamic or type error |
| `emptiness` | Whether a sequence has no items, as tested by `sxml_sequence_test_emptiness` (`fn:empty`) |
| `cli` | The command-line interface (`apps/`, docs/CLI.md); allowed short form, as the `sxml_cli_` prefix of application code only |
| `program` | A command-line program as a static table: name, summary, commands (`sxml_cli_program`) |
| `command` | One verb of a program (`sxml version`) with its options, positionals and function |
| `option` | A named command-line argument: `long_name` (`--format`), optional `short_name` (`-f`), `value_kind`, `value_name` (`FORMAT`), `choices`, `default_value` |
| `positional` | A command-line argument identified by its place, not a name (POSIX "operand"; `operand` is taken) |
| `multiplicity` | How often an option or positional may appear: at most once, repeatable, exactly one, optional, one or more, zero or more |
| `argument_list` | The command-line arguments after the program or command name, as byte views |
| `invocation` | What a command function receives: program, command, parsed arguments, writers |
| `exit_code` | The process exit status: success 0, operation failed 1, usage error 2, input/output error 3, internal error 4 |
| `usage_error` | A command line that does not match the table: unknown, missing, repeated or extra arguments |
| `suggestion` | The closest valid name or value to a mistyped one ("did you mean"), found by `edit_distance` |
| `edit_distance` | Optimal string alignment distance between two names, ASCII case ignored |
| `output_stream` | Interface receiving bytes a command-line program writes (`sxml_cli_output_stream`) |
| `writer` | An output stream plus the status of its first failed write |
| `memory_stream` | The output stream test double: keeps what is written, up to a capacity |
| `platform_layer` | The only code of the program that touches stdio (`apps/sxml/platform_stdio.c`) |
