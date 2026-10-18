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
| `cast` | Converting an atomic item to a target type (F&O 4.0, section 19 "Casting"): `cast as`, constructor functions, and the untypedAtomic casting and promotion of coercion |
| `castability` | Whether a value can be cast to a target type, as `castable as` answers |
| `target_type` | The type a cast converts to: atomic, list, union, enumeration or choice of atomic types |
| `lexical_form` | A string in the lexical space of a type (XSD 1.1 Part 2): what a cast from `xs:string` parses |
| `whitespace_facet` | How a type treats whitespace before parsing a lexical form: preserve, replace or collapse (XSD 1.1 Part 2, section 4.3.6) |
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
| `unit` | Everything one compilation produces in the IR: functions, expressions, constants, global variables, modes; reference counted (`sxml_ir_unit`) |
| `atom` | An IR operand that needs no evaluation: a constant, a slot or a global variable (D46) |
| `slot` | A numbered place, from 1, in the frame of an IR function, holding one variable's value |
| `frame` | The slots of one active call of an IR function |
| `main_function` | Function 1 of a unit: the body of the expression or query |
| `context_position` | The position part of the focus (`fn:position`) |
| `context_size` | The size part of the focus (`fn:last`) |
| `captured_variable` | An atom of the defining function whose value a function item made from an IR function keeps (its `captured_value`) |
| `global_variable` | A variable of a unit outside every function: external, or computed by an initializer function |
| `constant` | A value made at compile time and kept by a unit |
| `function_target` | What a static call or function reference calls: a native function, an IR function or a constructor |
| `native_function` | A function written in C, described by one row of static data (name, parameters, result type, properties, body) |
| `function_parameter` | Name, declared type as text and default of one parameter of a function |
| `parameter_default` | How an omitted parameter gets its value: required, the context value, or supplied by the body |
| `default_function` | The IR function that computes the default expression of one optional parameter of a declared XQuery function; its parameters are the caller's context value, position and size, of which it reads those its IR function's focus names (`SXML_PARAMETER_DEFAULT_EXPRESSION`, XQuery 4.0, 5.18.3) |
| `ir_module` | A static context a unit keeps for code that reads it when it runs: an XQuery library module whose functions the unit holds (its source name and static base URI, D148), or an XSLT element's (base URI, namespaces, default collation, its package's decimal formats, D163), named by a function's module_number or by a STATIC_CALL or node constructor (`sxml_ir_module`) |
| `function_library` | The per-processor index over the native function parts, resolving their types |
| `function_table` | Interface through which static analysis finds a function by name and arity |
| `function_entry` | What a function table finds: the target and its signature for one arity |
| `coercion` | The XPath 4.0 coercion rules (section 3.4) applied to a value and a sequence type; `coercion_role` decides the error code |
| `effective_boolean_value` | The boolean value of a sequence (XPath 4.0, section 2.5.4) |
| `pass` | An IR-to-IR function over a unit, run in a fixed list before the unit is sealed |
| `verifier` | The pass that checks the invariants of a unit and changes nothing |
| `static_analysis` | Resolving the names of an AST against the static context and reporting static errors (XPath 4.0, section 2.3.3.1) |
| `lowering` | Translating an AST into IR expressions of a unit |
| `lowerer` | The state of one lowering into one unit: the AST and static context being lowered, the variable scope, the current function and its focus (D103) |
| `lowering_procedure` | A construct lowered by frames: a step function and the size of its locals, static data (D103) |
| `lowering_frame` | One running lowering procedure on the lowering machine's stack, in allocator memory, with its step, locals and destination (D71, D103) |
| `lowering_machine` | The explicit stack of lowering frames that lowers without C recursion (D71) |
| `lowering_block` | The LETs bound while one expression is lowered, closed by its result (administrative normal form) |
| `lowering_part` | A file defining `sxml_xpath_lowering_part_<part>`: AST kinds and the procedures or leaf functions lowering them for some languages, found by CMake's glob (D103) |
| `embedding` | One XPath AST lowered inside a caller's scope: its variables, a focus of the caller's choice and the AST's own static context (D103) |
| `binding_collection` | The value a for binding iterates (XPath 4.0, section 4.13.1): its items, the members of its arrays (`for member`) or the entries of its maps (`for key/value`) |
| `range_variable` | A variable a for, let or quantifier binding binds to an item, member, key, value or part; the position variable is not one |
| `destructuring_let` | A let binding of several variables to the parts of one value: `$( ... )` items of a sequence, `$[ ... ]` members of an array, `${ ... }` entries of a map by name (XPath 4.0, section 4.13.2) |
| `tuple_stream_clause` | A FLWOR clause that works on the whole tuple stream as a value: order by, count, while and group by (XQuery 4.0, section 4.13; D55); a window clause is a clause of a segment, like for (D137) |
| `clause_segment` | The FLWOR clauses between two tuple stream clauses, lowered as nested MAP, LET and CONDITIONAL without the tuple stream as a value |
| `tuple_array` | One tuple of a FLWOR as an array: one member per live variable bound to a slot (D55) |
| `tuple_layout` | Which variable each member of the tuple arrays holds, and the variables (constants, globals) that are not packed |
| `grouping_key` | The atomized value of a grouping variable of a group by clause, empty or one atomic item; tuples with equivalent grouping keys form one group (XQuery 4.0, section 4.13.8) |
| `window_array` | One window of a window clause as an array of nine members: the window, then the start and end items, their positions, previous and next items (D137) |
| `condition_closure` | The function item a window's start or end condition is lowered to: its parameters are the condition's window variables, its body the condition (D137) |
| `dynamic_node_test` | A node test `{E}`: a node passes when its name, local name or ·jkey· is atomic-equal to an item of E's value (XPath 4.0, section 4.7.5) |
| `item_axis` | The axis `item::`, defined for JNodes: the items of the origin's ·jvalue· as JNodes (XPath 4.0, section 4.7.4) |
| `focus_constructor` | A constructor function call or reference that leaves its argument out and takes the context value: `xs:int()`, `xs:int#0` (XPath 4.0, section 4.5.1.3) |
| `method_call` | `M =?> N(...)`: the function in entry N of each map of M, called with that map first (XPath 4.0, section 4.15.5) |
| `but_with` | `A but with B`: record A with the entries of map B put in, use-last (XPath 4.0, section 4.15.4) |
| `predeclared_namespace` | A prefix bound in every static context (xml, xs, xsi, fn, map, array, math, err) |
| `default_element_namespace` | The namespace of unprefixed element and type names in the static context |
| `variable_scope` | The variables visible at one point of an expression, innermost binding first |
| `free_variable` | A variable an inline function body reads but does not bind; the function captures it |
| `argument_placeholder` | A `?` in an argument list, making the call a partial application |
| `partial_application` | A function call with argument placeholders, giving a function item (XPath 4.0, section 4.5.2.5) |
| `keyword_argument` | An argument given as `name := value` (XPath 4.0, section 4.5.1.1) |
| `text_form` | The printed form of a unit, for golden tests and debugging |
| `seal` | End the building of a unit: nothing in it moves afterwards |
| `evaluator` | Walks the IR of a sealed unit and computes values |
| `document_loader` | Internal interface through which fn:doc and fn:parse-xml obtain document nodes; the processor implements it |
| `evaluation_environment` | What the library functions of one evaluation read from the host beyond fn:doc: the resolver for text, binary and collection resources, the trace sink, the default language, environment variables, and the static context's decimal formats and collations; carried by the processor's document loader |
| `evaluation_memo` | Values a native body computes once per evaluation and subject node (an xsl:key's index for one tree), kept in the dynamic context until the evaluation ends (D149) |
| `current_item` | XSLT's current item (fn:current, XSLT 4.0, 20.3.1): the context value the outermost expression started with, the focus of its embedding; in a pattern, the item being matched (D149) |
| `xslt_function_table` | The function table put in front of every static context of a stylesheet: stylesheet functions lowered into the unit (fn:key, fn:copy-of), then the XSLT-only library parts (D149) |
| `context_function` | An XSLT function whose value an enclosing instruction sets lexically: fn:current-group, fn:current-grouping-key, fn:current-merge-group, fn:current-merge-key; lowering reads a variable of the same local name in the XSLT namespace, absent when none is in scope (D152) |
| `context_variable` | The variable Q{XSLT}N an XSLT instruction puts in scope for a context function, read by a parameter default (`SXML_PARAMETER_DEFAULT_CONTEXT_VARIABLE`) named N; the empty sequence where none is in scope (D153) |
| `captured_groups` | XSLT's current captured groups (XSLT 4.0, 17.2): the map from group numbers and names to fn:captured-group-record values, group 0 the match, set in xsl:matching-substring as the context variable regex-groups (D153) |
| `place_marker` | The sequence of integers xsl:number formats: its value attribute's, or computed from the selected node's position by level, count and from (XSLT 4.0, 12) |
| `population` | The sequence xsl:for-each-group groups, its select value, in population order (XSLT 4.0, 14.1) |
| `grouping_pair` | The array [item, value] lowering makes for each item of a population: its grouping keys, or whether it matches the pattern (D152) |
| `group_array` | One group of xsl:for-each-group as the array [current group, current grouping key] (D152) |
| `grouping_method` | How xsl:for-each-group forms groups: by key, by adjacent keys (each composite or not), starting or ending with a pattern (D152) |
| `kept_ast` | An XPath AST a stylesheet lowering makes (a short text, a pattern's match expression) and keeps until it ends (D152) |
| `key_index` | The map from key values to nodes of one xsl:key name for one tree, built once per evaluation (XSLT 4.0, 20.2; D149) |
| `xslt_accumulator` | An `xsl:accumulator`: a value carried through a traversal of a tree, giving every node but attributes and namespaces a pre-descent and a post-descent value (XSLT 4.0, 19). In code always `xslt_accumulator`; a bare `accumulator` is a fold's (D155) |
| `traversal_event` | A start or an end visit of a node in the traversal of a tree for an xslt accumulator, the start before the node's descendants and the end after them (XSLT 4.0, 19.4) |
| `pre_descent_value` | The value of an xslt accumulator just after the start event of a node (fn:accumulator-before) |
| `post_descent_value` | The value of an xslt accumulator just after the end event of a node (fn:accumulator-after) |
| `accumulator_values` | The map of one xslt accumulator for one tree, from each node's generate-id to its pre- and post-descent values, built once per evaluation and kept in the evaluation memo (D155) |
| `accumulator_scope` | The xslt accumulators applicable to one tree when they are restricted: for the initial match selection, those use-accumulators of the initial mode names; kept in the evaluation memo (XSLT 4.0, 19.2; D155) |
| `sort_key` | An expression whose value orders the items of a sort, with direction and empty order |
| `template_rule` | An XSLT rule: a match function, a template function, priority and import precedence |
| `mode` | An XSLT mode: its template rules and its built-in rule |
| `parameter_binding` | A parameter name with the value passed for it (`xsl:with-param`) |
| `template_function` | The IR function an `xsl:template` (or a built-in template) lowers to: the focus (item, position, size) and the two parameter maps are its five parameters (D144) |
| `parameter_map` | A map(xs:QName, item()*) of the parameters supplied to a template: one for the non-tunnel and one for the tunnel parameters (D144) |
| `with_parameter` | The value one `xsl:with-param` supplies, with its name and whether it is a tunnel parameter |
| `built_in_template` | The template function a mode calls for an item no template rule matches: the built-in template rule of its on-no-match (XSLT 4.0, 6.10) |
| `rule_search` | Which template rules of a mode an apply-templates tries: all of them, those after the current template rule (`xsl:next-match`), or those of the levels the current rule's level imports (`xsl:apply-imports`) |
| `current_template_rule` | The template rule whose template is running, absent in a changed focus, functions and globals (XSLT 4.0, 6.11) |
| `current_mode` | The mode the running template was chosen in; `mode="#current"` (XSLT 4.0, 6.9.2) |
| `enclosing_mode` | An `xsl:mode` with `xsl:template` children, the only rules of that mode (XSLT 4.0, 6.9.6) |
| `stylesheet_level` | A stylesheet module with the modules it includes, directly or not: one import precedence (XSLT 4.0, 3.10.2) |
| `invocation` | How the main function starts a transformation: a template to call, or templates applied to the global context item in an initial mode (XSLT 4.0, 2.4) |
| `stylesheet_function` | A function an `xsl:function` declares; one IR function per arity of its arity range (XSLT 4.0, 10.3; D150) |
| `arity_range` | The arities a function accepts: its required parameters up to all of them (XSLT 4.0, 10.3.1) |
| `eclipsed` | Said of an `xsl:function` overridden by one of the same name, higher import precedence and an arity range holding all of its own (XSLT 4.0, 10.3.4.1) |
| `attribute_set` | All `xsl:attribute-set` declarations of one name: one IR function of the focus that returns their attributes (XSLT 4.0, 10.2; D150) |
| `attribute_set_invocation` | One name in an `[xsl:]use-attribute-sets` attribute, a call of that attribute set with the current focus (XSLT 4.0, 10.2.1) |
| `component_lowering` | The stylesheet functions and attribute sets of one stylesheet lowering, with the function table each static context gets (D150) |
| `package` | An XSLT package: the principal module or the module an `xsl:use-package` read, with the modules it includes and imports; an `xsl:stylesheet` is an implicit package (XSLT 4.0, 3.4; D160) |
| `used_package` | The package an `xsl:use-package` names, read again for every use, so each use has components of its own (XSLT 4.0, 3.4.2) |
| `component` | A stylesheet function, named template, attribute set, global variable or parameter, or mode, as packages bind references to it (XSLT 4.0, 3.4.5) |
| `component_instance` | One component as one package contains it, with its visibility there: declared in the package, accepted from a used package, or overriding one (XSLT 4.0, 3.4.5, the "containing package") |
| `visibility` | Public, private, final, abstract or hidden: who may refer to or override a component instance (XSLT 4.0, 3.4.5.1) |
| `overriding_declaration` | A child of `xsl:override`: the using package's replacement of a component of the used package (XSLT 4.0, 3.4.5.3) |
| `package_table` | The packages and component instances of one stylesheet lowering, which binds every reference to a component (D160) |
| `whitespace_stripping` | Removing the whitespace text nodes that `xsl:strip-space` names from a copy of a source document (XSLT 4.0, 4.2.2; D150) |
| `space_rule` | One name test of an `xsl:strip-space` or `xsl:preserve-space`, with its import precedence, default priority and declaration order |
| `document_preparation` | A step the document loader applies to each document it gets from the resolver before keeping it: XSLT's whitespace stripping (D150) |
| `snippet` | A fixed XPath text that lowering parses and lowers itself: the parts of built-in templates, the default select of `xsl:apply-templates` |
| `grammar` | A tree-sitter grammar under `grammars/<language>/`, and the parser generated from it |
| `concrete_syntax_tree` | The tree-sitter parse tree of a query or document, before sxml builds its own structures |
| `node_type` | The name a grammar gives one kind of concrete syntax tree node (`additive_expression`), as listed in `src/node-types.json` |
| `terminal` | A leaf of a concrete syntax tree: one token of the source text (XPath 4.0, A.3) |
| `non_delimiting_terminal` | A name, keyword or numeric literal terminal; two of them must not touch (XPath 4.0, A.3.2) |
| `query_scanner` | The external tree-sitter scanner shared by the XPath, XQuery and XSLT pattern grammars |
| `parser_part` | What a grammar derived from grammars/xpath adds to the XPath front end's walker: its language, node types, fields and the hooks `apply_token`, `describe_payload` and `finish_node` (`sxml_ast_parser_part`, D111) |
| `parser_token` | A concrete syntax tree child that makes no AST node and is absorbed into its parent's detail or flags as the parent is described |
| `unnamed_child` | A named concrete syntax tree child that its grammar gives no field (XQuery's module); a parser part names its AST field |
| `boundary_whitespace` | Whitespace in direct element content delimited only by tags, direct constructors and enclosed expressions (XQuery 4.0, 4.12.1.4) |
| `namespace_declaration_attribute` | An `xmlns` or `xmlns:p` attribute of a direct element constructor; it makes a namespace binding, not an attribute (XQuery 4.0, 4.12.1.2) |
| `character_reference` | `&#N;` or `&#xH;` in an XQuery string literal, BracedURILiteral or direct constructor text, replaced by its character, which must be an XML 1.0 character (XQuery 4.0, 4.3.1.2; XQST0090) |
| `predefined_entity_reference` | `&lt;`, `&gt;`, `&amp;`, `&quot;` or `&apos;` in the same places (XQuery 4.0, 4.3.1.2) |
| `keyword_name` | A keyword used as a name; XPath 4.0 keywords are not reserved |
| `direct_constructor` | XQuery XML-like node constructor syntax: `<a b="{1}">text</a>`, `<!-- -->`, `<?pi?>` |
| `computed_constructor` | Node constructor written with a keyword: `element #a { ... }` |
| `content_sequence` | The nodes an element or document constructor adds to its new node: the value of its content with arrays flattened, adjacent atomic items made into one text node and document nodes replaced by their children (XQuery 4.0, "Content"; XSLT 4.0, 5.7.1) |
| `content_frame` | An entry of a node constructor's explicit stack of arrays whose members are being added to a content sequence (D71) |
| `construction_mode` | `strip` or `preserve` (XQuery 4.0 construction declaration): whether constructed and copied nodes become untyped or keep their types |
| `copy_namespaces_mode` | `preserve` or `no-preserve`, `inherit` or `no-inherit` (XQuery 4.0 copy-namespaces declaration): which in-scope namespaces a copied element keeps and whether it takes those of its new parent |
| `construction_flags` | The bits of an IR constructor that carry the construction and copy-namespaces modes and XSLT's content rules; 0 is XQuery's defaults (`sxml_ir_construction_flag`, D110) |
| `name_namespaces` | The namespace bindings an IR constructor resolves a name given as a string with: the statically known namespaces extended by enclosing direct constructors' declarations (D110) |
| `string_template` | XPath 4.0 `` `text {expression}` `` |
| `string_constructor` | XQuery ```` ``[text `{expression}`]`` ```` |
| `pragma` | XQuery `(# name contents #)` before an extension expression |
| `pattern` | An XSLT 4.0 match pattern (XSLT 4.0, section 6.3) |
| `xnode_pattern` | A pattern made of paths, steps, unions, intersect and except, matching XNodes (XSLT 4.0, 6.3.2) |
| `simple_pattern` | A pattern of an item pattern (`.`, `atomic(...)`, a map, record, array or JNode pattern) and predicates, matching any item (XSLT 4.0, 6.3.3) |
| `sequence_pattern` | A pattern with an occurrence indicator, or `empty-sequence()`, matching a value inside a map, array or JNode pattern (XSLT 4.0, 6.3.5) |
| `leading_step` | A step of an XNode pattern that is not on the right of `/` or `//`; it is matched on the axes child-or-top, attribute-or-top or namespace-or-top (XSLT 4.0, 6.3.2.2) |
| `equivalent_expression` | The XPath expression of the same form as an XNode pattern, with its leading steps' axes adjusted, that defines what it matches (XSLT 4.0, 6.3.2.2) |
| `match_expression` | The XPath AST of xs:boolean, over the item as context value, that is true exactly when the item matches a pattern (D118) |
| `match_function` | The IR function of one parameter, returning xs:boolean, that a template rule's pattern is lowered to |
| `default_priority` | The priority of a template rule without a priority attribute, computed from the form of its pattern (XSLT 4.0, 6.4) |
| `stylesheet` | An XSLT 4.0 stylesheet: a principal module with every module it includes and imports, compiled together |
| `stylesheet_module` | One XML document of a stylesheet as reached by one `xsl:include` or `xsl:import`, or the principal module; `module` in `sxml_xslt_ast_module` |
| `principal_module` | The stylesheet module the processor was asked to compile (module 1) |
| `stylesheet_ast` | What the stylesheet reader builds (`sxml_xslt_ast`): one node per stylesheet element and text node, attributes read into typed values, each expression, pattern, value template and type its own `abstract_syntax_tree`; `xslt_ast` allowed as a module name only |
| `stylesheet_level` | Stylesheet modules connected by `xsl:include`; they share one import precedence (XSLT 4.0, 3.10.2) |
| `import_precedence` | The rank of a stylesheet level: an importing level above what it imports, a later import above an earlier one (post-order of the import tree, XSLT 4.0, 3.10.3) |
| `declaration` | A top-level element of a stylesheet module (`xsl:template`, `xsl:function`, ...) |
| `declaration_order` | The order of the declarations of a stylesheet level, with included modules in place of their `xsl:include` |
| `instruction` | An XSLT element that may appear in a sequence constructor (`xsl:if`, `xsl:copy`, ...) |
| `sequence_constructor` | The children of a stylesheet element that are evaluated to make a sequence (XSLT 4.0, 5.7) |
| `literal_result_element` | An element of a sequence constructor outside the XSLT namespace, copied to the result (XSLT 4.0, 11.1) |
| `value_template` | An attribute value template or text value template: fixed text with enclosed XPath expressions in `{ }` (XSLT 4.0, 5.6) |
| `fixed_part` | Text of a value template outside its variable parts, with `{{` and `}}` for single curly brackets (XSLT 4.0, 5.6); a string template's are `string_template_fixed_part` |
| `variable_part` | `{ Expr? }` in a value template; empty, whitespace or comments only give a zero-length string (XSLT 4.0, 5.6) |
| `curly_bracket` | `{` or `}`, the spec's name; not `brace` |
| `standard_attribute` | One of the attributes allowed on every XSLT element (`version`, `expand-text`, `use-when`, ...; XSLT 4.0, 3.3) |
| `shadow_attribute` | An attribute `_name` whose value, a value template evaluated at compile time, replaces the attribute `name` (XSLT 4.0, 3.11.4) |
| `simplified_stylesheet` | A stylesheet module whose outermost element is not `xsl:stylesheet`, `xsl:transform` or `xsl:package`; read as one template rule matching `.` (XSLT 4.0, 3.7) |
| `synthesized` | Made by the stylesheet reader rather than read from an element (the root and template of a simplified stylesheet) |
| `source_node` | The node of a stylesheet module's tree that a stylesheet AST node was read from |
| `name_token` | One token of a name-list attribute of an XSLT element: a name, a wildcard, or `#default`, `#unnamed`, `#current`, `#all` |
| `keyword` | A fixed word of a language: an XPath keyword (see `keyword_name`), or one of the words an XSLT attribute takes from a fixed list (`shallow-copy`, `strict`) |
| `simple_content` | The string a sequence constructor or value makes for an attribute, comment, PI, namespace or value template: adjacent text nodes merged, zero-length ones dropped, then atomized and joined with a separator (XSLT 4.0, 5.7.2) |
| `namespace_alias` | An `xsl:namespace-alias` pair: literal result elements and attributes in its stylesheet URI are output in its result URI (XSLT 4.0, 11.1.4) |
| `stylesheet_lowering` | One lowering of a sealed stylesheet AST into an IR unit (`sxml_xslt_lowering`): the XPath static context of each AST static context, the namespace aliases and the instruction rules, around one `lowerer` |
| `instruction_rule` | Which lowering procedure lowers one kind of XSLT instruction (`sxml_xslt_instruction_rule`); each XSLT lowering file lists its own |
| `variable_binding` | An `xsl:variable`, `xsl:param` or `xsl:with-param` element, whose value comes from its select, its content or neither (XSLT 4.0, 9.3); its `binding_role` says which type error its declared type raises (XTTE0570 or XTTE0590) |
| `conditional_content` | A sequence constructor with `xsl:on-empty` or `xsl:on-non-empty` children, whose value depends on whether the other children's value is vacuous (XSLT 4.0, 8.5) |
| `vacuous` | Of an item: a zero-length text node, a document node without children, a zero-length atomic item, or an array whose flattened members all are (XSLT 4.0, 8.5.2) |
| `deemed_empty` | Of an item, for `xsl:where-populated`: a document or element node without children, another node or atomic item of zero-length string value, an empty map, or an array whose flattened members all are (XSLT 4.0, 8.5.1) |
| `tail_position` | Where `xsl:next-iteration` and `xsl:break` may stand: last in the body of `xsl:iterate`, or in the tail position of an `xsl:if`, `xsl:when`, `xsl:otherwise`, `xsl:try` or `xsl:catch` that is (XSLT 4.0, 7.2) |
| `iteration_state` | The array `xsl:iterate` lowering passes from one item to the next: control (0 go on, 1 broken), the item's output, the parameter values (D145) |
| `iteration_marker` | The map `{Q{XSLT}iterate: state}` that `xsl:next-iteration` or `xsl:break` gives as the last item of the body's value (D145) |
| `record_field` | An attribute of `xsl:record`: the field name is its local name, the value its expression |
| `excluded_namespace` | A namespace whose namespace nodes literal result elements do not copy (XSLT 4.0, 11.1.3) |
| `extension_namespace` | A namespace whose elements are extension instructions (XSLT 4.0, 24.3) |
| `extension_instruction` | An element in an extension namespace inside a sequence constructor; sxml knows none and runs its fallback |
| `data_element` | A top-level element outside the XSLT namespace, ignored by the processor (XSLT 4.0, 3.6.4) |
| `fixed_namespace_bindings` | The namespace bindings that the `fixed-namespaces` attribute sets for every expression of a stylesheet module (XSLT 4.0, 3.6.1) |
| `module_uri` | The absolute URI a stylesheet module was read from; the source name of its expressions |
| `nesting_depth` | How many modules lie between a stylesheet module and the principal module in the module tree |
| `transformation` | One run of a compiled stylesheet: priming it (stylesheet parameters, global context item) and invoking it once (XSLT 4.0, 2.4) |
| `invocation_kind` | How a transformation starts: apply-templates, call-template or a function call (XSLT 4.0, 2.4.3 to 2.4.5) |
| `initial_match_selection` | The value an apply-templates invocation processes (XSLT 4.0, 2.4.3) |
| `initial_mode` | The mode an apply-templates invocation selects template rules in (XSLT 4.0, 2.4.3) |
| `initial_template` | The named template a call-template invocation evaluates, `xsl:initial-template` by default (the spec's "initial named template", XSLT 4.0, 2.4.4) |
| `initial_function` | The stylesheet function a function call invocation calls (XSLT 4.0, 2.4.5) |
| `global_context_item` | The context item of global variables and parameters, one item or absent (XSLT 4.0, 2.4.2) |
| `stylesheet_parameter` | A global `xsl:param` that is not static, given its value when a transformation is primed (XSLT 4.0, 9.5) |
| `static_parameter` | A global `xsl:param` with `static="yes"`, given its value before compilation (XSLT 4.0, 9.6) |
| `static_variable` | A global `xsl:variable` or `xsl:param` with `static="yes"`, whose value is known while the stylesheet is read (XSLT 4.0, 9.6) |
| `static_expression` | An XPath expression evaluated while the stylesheet is read: `[xsl:]use-when`, the select of a static variable, the value templates of shadow attributes (XSLT 4.0, 9.7) |
| `static_scope` | The static variables and function definitions every static expression of one stylesheet sees (`sxml_xslt_static_scope`) |
| `stylesheet_reader` | What reads stylesheet modules into the stylesheet AST (`sxml_xslt_read_stylesheet`, packet 4.5) |
| `forwards_compatible_mode` | Reading an element whose effective version is above 4.0: unknown XSLT elements and attributes are ignored or kept for fallback (XSLT 4.0, 3.9) |
| `template_parameter` | An `xsl:param` of a template; the host passes values to those of the initial template or of the rules of the initial mode |
| `tunnel_parameter` | A template parameter with `tunnel="yes"`, passed on through every template to the ones that declare it (XSLT 4.0, 10.1.6) |
| `raw_result` | The value an invoked template or function returns, coerced to its declared type, before any tree is built or serialization (XSLT 4.0, 2.4) |
| `principal_result` | The result of the invocation itself, as opposed to the result documents (XSLT 4.0, 2.4.6) |
| `invocation_global` | A global of a stylesheet's IR unit through which the host supplies an invocation's run-time values (the template and tunnel parameter maps, the initial match selection, the initial function's arguments), bound by name as stylesheet parameters are (D157) |
| `invocation_value` | The value src/xslt.c binds to an invocation global for one transformation (D157) |
| `unit_evaluation_extension` | What a language adds to the shared evaluation of a unit (`src/xpath_api_internal.h`): XSLT's function table, document preparation and result writer (D157) |
| `xslt_test_execution` | Running one planned XSLT 4.0 test case through `include/sxml/xslt.h` in a processor of its own and checking its assertions (`tests/xslt/driver/xslt_execution.h`, D157) |
| `file_catalog` | The QT4 catalog the XSLT driver never loads, over the same suite files: what the QT4 resource table and assertions read files through (D157) |
| `result_document` | A secondary result: what one `xsl:result-document` instruction delivers, identified by its absolute URI (XSLT 4.0, 26.2) |
| `base_output_uri` | The URI relative `href` values of `xsl:result-document` are resolved against; the URI of the principal result (XSLT 4.0, 2.4.6) |
| `output_definition` | The serialization parameters and `build-tree` of one `xsl:output` declaration, named or unnamed (XSLT 4.0, 26.1) |
| `build_tree` | The `build-tree` setting of an output definition: whether an unserialized result is turned into a document node (XSLT 4.0, 26.1) |
| `result_delivery` | How a principal result or result document reaches the host: raw, as a tree, or serialized to an output stream |
| `result_document_handler` | Host interface that receives result documents: it is opened with a document's URI and output definition and closed with its value |
| `result_document_collector` | The built-in result document handler: keeps every result document in memory, by URI |
| `stylesheet_outputs` | The compiled output part of a stylesheet: its output definitions by name, its named character maps, and what each `xsl:result-document` instruction reads when it runs (packet 4.13) |
| `named_character_map` | The character map an `xsl:character-map` declares: its `xsl:output-character` mappings after those of the maps it uses, expanded, a later mapping of a character replacing an earlier one (XSLT 4.0, 26.1.6) |
| `result_document_instruction` | What one `xsl:result-document` reads when it runs: its attribute value templates in a fixed order, its namespaces, base URI and character maps |
| `result_writer` | The object of one transformation that delivers its result documents and principal result: the stylesheet outputs, the result document handler, the base output URI, the URIs written (XTDE1490) and the current output URI |
| `open_result_document` | A result document between the handler's open and close: its URI, effective output definition and target |
| `current_output_uri` | The URI of the result being written: the base output URI, or the absolute `href` of the `xsl:result-document` being evaluated (XSLT 4.0, 26.3) |
| `temporary_output_state` | The output state in which `xsl:result-document` is XTDE1480: inside variables, parameters, functions, keys and sort keys, and during a function call invocation (XSLT 4.0, 26.2.6) |
| `front_end` | The code that turns the text of one language (XPath, XQuery, an XSLT pattern) into an abstract syntax tree |
| `abstract_syntax_tree` | What a front end builds from a concrete syntax tree: one node per visible grammar production, children by grammar field, names as written, literals decoded; `ast` allowed as a module name only (`sxml_ast_node`) |
| `ast_field` | The role of a child in its AST parent: a field name of the grammar (`left`, `predicate`, `argument`) |
| `source_span` | Where a syntax node is: byte offset and byte count, with line and column of its first byte; a `source_location` without the source name |
| `operator` | The token that selects what an operator expression computes (`SXML_AST_OPERATOR_PLUS`); in the IR, what a binary or unary expression computes |
| `arithmetic_operator` | What an arithmetic expression computes: add, subtract, multiply, divide, integer divide or modulo (XPath 4.0, section 4.9) |
| `comparison_operator` | What a value or general comparison tests: equal, not equal, less, less or equal, greater, greater or equal |
| `node_comparison_operator` | What a GNode comparison tests: `is`, `is-not`, `<<`, `>>`, `precedes-or-is`, `follows-or-is` (XPath 4.0, section 4.11.3) |
| `value_comparison` | `eq`, `ne`, `lt`, `le`, `gt`, `ge` on two single atomic items, through fn:compare (XPath 4.0, section 4.11.1) |
| `general_comparison` | `=`, `!=`, `<`, `<=`, `>`, `>=`: true when some pair of atomized items has the relationship (XPath 4.0, section 4.11.2) |
| `test_suite` | The unit tests of one module: one test executable |
| `test_case` | One named test function within a test suite |
| `test_check` | One checked condition within a test case |
| `qt4_driver` | The QT4 test-suite driver: the developer tool under `tests/qt4/driver/` that reads the QT4 catalog and runs its test cases (D44) |
| `qt4_catalog` | The QT4 suite's `catalog.xml` as the driver holds it: its test-set entries and global environments |
| `qt4_test_set` | One test-set file of the QT4 suite (`fn/abs.xml`): local environments, set dependencies and test cases |
| `qt4_test_case` | One `<test-case>` of a QT4 test set: query, environment, dependencies, result (never `test_case`, which is a unit test) |
| `qt4_environment` | The static and dynamic context a QT4 test case runs in: sources, parameters, namespaces, static base URI, resources, collections |
| `qt4_source` | A document of a QT4 environment: the context item (`role="."`), an external variable (`role="$name"`) or a document available by URI |
| `qt4_parameter` | A `<param>` of a QT4 environment: an external variable with a `select` expression and an optional declared type |
| `qt4_resource` | A text resource of a QT4 environment, reached by URI (`fn:unparsed-text`, `fn:json-doc`) |
| `qt4_collection` | A collection of a QT4 environment for `fn:collection`: sources, resources and queries under one URI |
| `qt4_resource_table` | What one QT4 test case's environment serves through the resource resolver: its resources by URI, its collections as sequences, and suite files by `file:///` URI |
| `qt4_scope` | Whether a QT4 test case counts in the denominator of the pass-rate targets (D95): its spec matches and it needs no non-goal; said `in_scope` in reports |
| `qt4_dependency` | A `<dependency type value satisfied>` of a QT4 test set or test case |
| `qt4_target` | The language and version a driver run targets: XP40 or XQ40 |
| `applicability` | Whether a QT4 test case can run for a target: its spec, its other dependencies and its environment are supported; otherwise the reason it is skipped |
| `test_list` | A file naming QT4 test cases, one `<test-set> <test-case>` per line (`tests/qt4/torture-slice.txt`) |
| `test_selection` | The QT4 test cases one driver run covers: a test list, one test set, or every test set |
| `test_outcome` | What running one QT4 test case gave: pass, wrong error (an expected error raised with another code, which passes), fail, error (sxml itself failed: a limit, a leak, a non-specification status) or not run (not applicable, or an assertion the driver cannot check) |
| `assertion_verdict` | What one QT4 result assertion, or a combination of them (`all-of`, `any-of`, `not`), says about a test's result: pass, wrong error, fail or unsupported |
| `run_report` | The output of `sxml-qt4 run`: one entry per test case that did not pass, counts per test set and overall, as text or JSON |
| `slice_baseline` | The number of torture-slice test cases that must pass (`tests/qt4/slice-baseline.txt`); it only goes up |
| `xquery_slice` | The QT4 test cases of `tests/qt4/xquery-slice.txt`, run with `--spec=XQ40` by ctest `qt4/xquery-slice` against `tests/qt4/xquery-slice-baseline.txt`, which only goes up (packet 3.13) |
| `suite_path` | The path of a QT4 suite file relative to the directory of `catalog.xml`, with `.` and `..` segments removed |
| `xslt_test_driver` | The XSLT 4.0 test-suite driver: the developer tool `sxml-xslt-test` under `tests/xslt/driver/`, a sibling of the QT4 driver that links its library (D117) |
| `xslt_test_catalog` | The XSLT 4.0 suite's `catalog.xml` (`third_party/xslt40-test/`) as the driver holds it: its test-set entries and one open test set |
| `xslt_test_case` | One `<test-case>` of an XSLT 4.0 test set: environment, stylesheets and packages, parameters, invocation, output, dependencies, result |
| `xslt_test_environment` | The environment of an XSLT 4.0 test case: what a QT4 environment holds, plus inline source documents, stylesheets, packages, static parameters and an output element |
| `xslt_test_plan` | What the transformation of one XSLT 4.0 test case needs, read from the catalog alone: principal module, secondary modules, static and stylesheet parameters, global context item, invocation kind, result delivery |
| `test_set_group` | The directory under `tests/` of the XSLT 4.0 suite that holds a test set (`attr`, `decl`, `expr`, `fn`, `insn`, `misc`, `strm`, `type`, `sandp`); D95 leaves `strm` and `sandp` out of scope |
| `test_catalog` | Any conformance driver's catalog as the test selection walks it: named test sets, one open at a time, each with named test cases (an interface over the QT4, XSLT 4.0 and XSD 1.1 catalogs; `tests/qt4/driver/selection.h`) |
| `test_budget` | The allocator of one conformance test: passes allocations on, counts live allocations and bytes, refuses any past the byte limit, and turns a leak into an error outcome (`tests/qt4/driver/budget.h`) |
| `xsd_test_driver` | The XSD 1.1 test-suite driver: the developer tool `sxml-xsd-test` under `tests/xsd/driver/`, a sibling of the QT4 and XSLT drivers that links the QT4 driver's library (packet 5.3) |
| `xsd_test_catalog` | The W3C XSD test suite's `suite.xml` (`third_party/xsdtests/`) as the driver holds it: its test-set references and one open test set |
| `xsd_test_set` | One `testSet` file of the XSD test suite, named in reports by its directory without `Meta` and its file name without extension (`saxon-Assert`, `ms-DataTypes_w3c`); the directory is its contributor area |
| `test_group` | A `testGroup` of an XSD test set: at most one schema test and the instance tests validated against that test's schema (or the built-in components alone when there is none) |
| `schema_test` | An XSD test that compiles the schema documents it names into one schema: valid means it compiles |
| `instance_test` | An XSD test that validates an instance document against its test group's schema: valid means the validation root is valid |
| `xsd_test_case` | A schema test or an instance test of an XSD test set, named `GROUP/TEST` in test lists and reports |
| `version_token` | A token of an XSD test suite `version` attribute (`1.1`, `XML-1.0-5e`, `full-xpath-in-CTA`): a test set, group or test applies when, for each token class it names, it names a token the driver supports; an `expected` applies when the driver supports all its tokens |
| `expected_validity` | The outcome an XSD test prescribes: `valid` or `invalid` are in scope; `notKnown`, `indeterminate`, `runtime-schema-error` and the implementation-defined values are not |
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
| `subtype_goal` | One question in the and/or tree a subtype question is reduced to: an answer, an item or sequence type pair, or a conjunction or disjunction of sub-goals (D58) |
| `subtype_frame` | A conjunction or disjunction of subtype goals on the explicit stack of a subtype search, waiting for the answer of its current sub-goal |
| `derivation_frame` | A union base type on the explicit stack of a derivation question, whose members are tried in turn |
| `cast_frame` | A cast to a union, choice or list target on the explicit stack of a cast, waiting for the cast of a member or token (D66) |
| `item_type` | A class of items, such as `xs:integer`, `element(a)` or `map(xs:string, item())` |
| `sequence_type` | An item type plus an occurrence |
| `occurrence` | How many items a sequence type allows: zero, one, zero or one, zero or more, one or more |
| `record_type` | An item type whose instances are records; a list of fields |
| `record_field` | One field of a record type: a name and a sequence type |
| `structural_record_type` | A record type matched by its fields (`record(a as xs:int)`) |
| `nominative_record_type` | A named record type matched only by records annotated with it |
| `builtin_record_type` | A nominative record type of F&O 4.0 (appendix C, `fn:dateTime-record`) that the function library declares in its processor's type registry, so every static context knows it |
| `enumeration_type` | An item type matching a fixed set of strings (`enum("a", "b")`) |
| `choice_item_type` | An item type matching items of any of its alternatives (`(A \| B)`) |
| `function_type` | The item type of function items with given parameter and result types; a function's signature |
| `node_test` | An item type over XNodes: a node kind, an optional name and an optional type annotation |
| `json_node_test` | An item type over JNodes: `jnode(key, value type)` |
| `type_annotation` | The type identifier an atomic item, node or record is annotated with |
| `item_heap` | Per-processor owner of the reference-counted cells that items and sequences point to |
| `cell` | A reference-counted, immutable heap object holding the data of an item or sequence that does not fit inline |
| `cell_class` | The static const operations of one kind of cell: its name, `release_contents` and, for sequence cells, the read interface (`src/xdm_internal.h`) |
| `release_contents` | The cell class operation that releases what a cell holds before the heap frees it |
| `pending_cell` | A cell whose last reference is gone, waiting on the item heap's list to be freed without recursion |
| `item_representation` | How an `sxml_item` is stored: which inline value or which kind of cell its payload holds (`SXML_ITEM_REPRESENTATION_*`) |
| `sequence_cell` | A cell that holds a sequence of two or more items: vector, integer range, concatenation or lazy |
| `atomic_family` | The atomic items that compare with one another: numbers, strings (with `xs:anyURI`, `xs:untypedAtomic`), binary, or one other primitive type |
| `exact_text` | The canonical decimal text of the exact value of a double, as compared with decimals |
| `static_error` | A specification error whose code and description are static text, reported without the heap (`sxml_static_error`) |
| `retain` / `release` | Add / drop one reference to an item, sequence or tree |
| `immortal` | A cell or tree that is never released (static data); retain and release do nothing to it |
| `item_kind` | Atomic item, node (XNode), JNode, function item, map or array |
| `generic_node` | A GNode: a node (XNode) or a JNode (XDM 4.0, section 6) |
| `datum` | The value of an atomic item, without its type annotation (XDM 4.0, section 5) |
| `datum_kind` | The C representation a datum is read and written in (boolean, integer, string, ...) |
| `date_time` | The seven components of a date/time datum (XDM 4.0, section 5.4) |
| `duration` | A duration datum: months plus seconds and nanoseconds (XDM 4.0, section 5.5) |
| `timezone_offset_minutes` | A timezone as minutes east of UTC, from -840 to 840 |
| `gregorian_value` | A value of one of the eight Gregorian types: xs:dateTime, xs:date, xs:time, xs:gYear, xs:gYearMonth, xs:gMonth, xs:gMonthDay, xs:gDay (F&O 4.0, section 9.3) |
| `local_value` | A date/time value as written, without adjusting it by its timezone (F&O 4.0, section 9.6) |
| `relabel` | Coercion that changes an atomic item's type annotation to a derived type whose value space holds the datum, keeping the datum (XPath 4.0, section 3.4.3) |
| `civil_timezone` | The timezone offset in conventional use at a place and time (F&O 4.0, fn:civil-timezone) |
| `ietf_date` | A date and time in the formats of RFC 822, RFC 850 and asctime that fn:parse-ietf-date reads |
| `unix_time` | Milliseconds since 1970-01-01T00:00:00Z, leap seconds not counted (fn:unix-dateTime) |
| `atomic_equal` | The context-free equality of map keys (F&O 4.0, section 2.2.1 `fn:atomic-equal`) |
| `atomic_hash` | A hash consistent with `atomic_equal` |
| `sequence_representation` | How a sequence is stored: empty, single item, vector, integer range, concatenation, lazy, descending integer range |
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
| `entry_block` | A map made from an entry list: its entries in entry order in one cell, with a hash index (D61) |
| `hash_index` | An open-addressing table from key hash to entry index, built for an entry block |
| `persistent_map` | A map made by put or remove: an entry tree and a key tree that share structure with the map they came from (D61) |
| `map_node` | One node of an entry tree or a key tree: an entry, its key hash and entry stamp, two subtrees (D61) |
| `entry_tree` | The balanced tree of a persistent map's entries ordered by entry stamp, counting entries per subtree so entries are found by entry index |
| `key_tree` | The balanced tree of a persistent map's keys ordered by key hash, then entry stamp, for lookup by key |
| `entry_stamp` | The number a persistent map gives a key when it enters; entry order is stamp order |
| `field_key` | The xs:string key item of one record field, made by a record on first need |
| `member_index` | Position of a member in an array, from 0 |
| `field_index` | Position of a field in a record type, from 0 |
| `arity` | Number of parameters of a function item |
| `array_node` | A cell of an array's persistent balanced tree (`src/xdm_array.c`): a flat node, a run node or a branch node; an array item's cell is the root array node |
| `flat_node` | An array node that holds its members inline, as `sxml_array_make` builds them |
| `run_node` | An array leaf that names a contiguous part of a flat node's members, so splitting a leaf copies nothing |
| `branch_node` | An array node with a left and a right subtree; its members are the left's followed by the right's |
| `node_height` | Of an array node: 0 for leaves, one more than the taller subtree for a branch node (AVL balance) |
| `join` / `split` | Concatenate two array trees, keeping balance / cut an array tree before a member index into two |
| `function_annotation` | An annotation name with a sequence of atomic items (`%private`) |
| `function_body` | Interface that runs a function item: native C code or an evaluator for the IR |
| `function_call` | What a function body receives: the function item, the arguments, the dynamic context |
| `captured_value` | A nonlocal variable value a function item keeps (its closure) |
| `json_key` | The ·jkey· of a JNode: the map key or 1-based array or sequence position under its parent |
| `json_value` | The ·jvalue· of a JNode: the value it wraps |
| `json_tree` | A JTree: the JNodes reachable from one root JNode |
| `navigational` | Of a step E2 in E1/E2 (XPath 4.0, section 4.7.3): ".", an axis step, a path, or a union, sequence, filter, conditional or let made only of navigational expressions |
| `selector` | A non-navigational step from a JNode, or a dynamic node test: its atomized value selects the children whose ·jkey· is atomic-equal to one of its items (XPath 4.0, section 4.7.5.4) |
| `fixpoint` | Of a fold: an accumulator that its function is certain to return unchanged for every further item, which ends the fold early |
| `option_definition` | One option of a library function that follows the option parameter conventions (F&O 4.0, section 1.7): its name and the type its value is coerced to |
| `child_index` | Position of a child among the children of its parent, from 0 |
| `node_kind` | One of the seven XNode kinds: document, element, attribute, text, comment, processing instruction, namespace |
| `node_reference` | A tree plus a node index (plus a namespace number for namespace nodes); identifies a node |
| `namespace_number` | Which in-scope namespace of an element a namespace node is, from 1; 0 for other nodes and for a stored namespace node |
| `parentless_namespace` | A namespace node without a parent element (XQuery computed namespace constructor, xsl:namespace outside an element); stored as the root of a tree of its own (D98) |
| `stored_namespace` | A namespace node held as a stored node of a tree: only a parentless one, at node index 0 (D98) |
| `node_description` | What a tree reports about one node: kind, name, parent, siblings, children, type annotation, flags |
| `last_descendant` | The node index of the last node in a node's subtree (the node itself when it has none) |
| `tree_operations` | The operations table a tree implementation supplies |
| `tree_owner` | Interface told when a tree is no longer referenced, so its owner can free it |
| `arena_tree` | The built-in tree: nodes in arrays indexed by node index, built by a tree builder |
| `tree_builder` | Interface receiving construction events in document order (parser, constructors, serializers) |
| `event` | One step given to a tree builder: start element, attribute, text, ... |
| `namespace_fixup` | What a tree builder does so every element and attribute name has a binding of its prefix to its namespace URI: adds bindings, undeclares the default namespace, or picks another prefix (XSLT 4.0, section 5.7.3) |
| `declared_namespace_bindings` | The namespace bindings an element has itself, sent or added by namespace fixup, as opposed to those it inherits from its parent |
| `generated_prefix` | A prefix namespace fixup makes up (`ns0`, `ns1`, ...) when a name's own prefix is taken by another namespace URI |
| `text_store` | The blocks of an arena tree that hold node content (text, attribute values, comments, ...) back to back |
| `node_segment` | One of the blocks holding an arena tree's nodes; segment k holds 16·2^k nodes, so nodes never move as the tree grows |
| `reference_node` | For the preceding and following axes from an attribute or namespace node: its parent element, whose position decides which nodes come before and after |
| `namespace_binding` | A prefix bound to a namespace URI |
| `in_scope_namespaces` | The namespace bindings in effect on an element (XDM 4.0, `dm:namespace-nodes`) |
| `string_value` | `dm:string-value` of a node |
| `typed_value` | `dm:typed-value` of a node |
| `base_uri` | `dm:base-uri` of a node |
| `uri_reference` | A URI or relative reference split into scheme, authority, path, query and fragment (RFC 3986, section 4.1) |
| `reference_resolution` | Turning a relative reference into a URI against a base URI (RFC 3986, section 5.2) |
| `dot_segment` | A `.` or `..` path segment, removed by reference resolution (RFC 3986, section 5.2.4) |
| `document_uri` | `dm:document-uri` of a document node |
| `unparsed_entity` | An unparsed entity of a document: system and public identifier |
| `byte_view_list` | A read-only pointer to byte views plus `byte_view_count` |
| `processor` | The object (Phase 1) that owns one name pool, type registry and item heap and compiles and runs queries |
| `limits` | A struct of bounds on work and memory for untrusted input (`sxml_processor_limits`, `sxml_xml_limits`) |
| `xml_parser` | Reads XML 1.0 bytes and sends their tree builder events (`include/sxml/xml.h`) |
| `encoding_name` | The name of a character encoding (`UTF-8`, `ISO-8859-1`), from outside a document or from its encoding declaration |
| `entity_expansion` | The text that replacing entity references produces (XML 1.0, section 4.4); bounded for untrusted input |
| `character_encoding` | How a document's characters are stored as bytes: UTF-8, UTF-16 (little or big endian), ISO-8859-1 or US-ASCII (`sxml_xml_encoding`, XML 1.0, section 4.3.3) |
| `byte_order_mark` | U+FEFF at the start of a document, which identifies UTF-8 or the byte order of UTF-16 (XML 1.0, Appendix F) |
| `transcoding` | Converting a document from its character encoding into UTF-8 before parsing |
| `line_end_normalization` | Turning CR LF and lone CR into LF in all text of a document (XML 1.0, section 2.11) |
| `general_entity` | An entity referenced as `&name;` (XML 1.0, section 4): internal (with replacement text), external parsed, or unparsed |
| `parameter_entity` | An entity referenced as `%name;` inside the DTD (XML 1.0, section 4) |
| `replacement_text` | The text an internal entity stands for: its literal value with character references expanded and general entity references kept (XML 1.0, section 4.5) |
| `entity_frame` | One level of nested entity expansion in the XML parser: the replacement text being walked and the entity it belongs to |
| `internal_subset` | The declarations between `[` and `]` of a document type declaration (XML 1.0, section 2.8) |
| `attribute_definition` | One attribute of an attribute-list declaration: its name, type and default (XML 1.0, section 3.3) |
| `attribute_default` | The value an attribute definition supplies when an element does not specify the attribute (XML 1.0, section 3.3.2) |
| `file_reader` | Interface through which a command of the sxml program reads a named file or standard input (`sxml_cli_file_reader`) |
| `resource` | What a resource resolver delivers for a URI: bytes, a document node, or nothing |
| `resource_resolver` | Host interface that retrieves resources by URI: documents, text, binary resources, collections, XQuery modules, XSLT modules and packages |
| `resource_kind` | What a resource is wanted for: an XML document for `fn:doc`, ... |
| `resource_form` | How a resolver delivers a resource: none, bytes, a document node, or a sequence (collections) |
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
| `decimal_format` | A named or unnamed set of properties that `fn:format-number` formats with (XPath 4.0, section 2.2.1, "statically known decimal formats") |
| `unnamed_decimal_format` | The decimal format without a name, used when `fn:format-number` names none |
| `decimal_format_property` | One property of a decimal format (decimal-separator, percent, NaN, ...), given as text |
| `marker` | The single character that stands for a decimal format property in a picture string |
| `rendition` | The string a decimal format property becomes in the formatted result; `%:pc` is marker `%`, rendition `pc` |
| `decimal_format_settings` | A decimal format with every property decoded: markers as codepoints, renditions as UTF-8, the zero digit and the three plain renditions |
| `picture_string` | The second argument of `fn:format-integer` and `fn:format-number`: the pattern a number is written by |
| `sub_picture` | One of the two halves of a `fn:format-number` picture string, for positive or for negative numbers (F&O 4.0, "Syntax of the picture string") |
| `primary_format_token` | The part of a `fn:format-integer` picture before its last semicolon: a digit pattern or a numbering token such as `a`, `I`, `Ww` |
| `format_modifier` | The part of a `fn:format-integer` picture after its last semicolon: `c` or `o` with an optional variant in parentheses, then `a` or `t` |
| `digit_pattern` | A primary format token of mandatory digit signs, optional digit signs (`#`) and grouping separators |
| `digit_family` | Ten consecutive Unicode characters of category Nd with digit values 0 to 9; named by its zero digit |
| `grouping_position` | The number of digit signs between a grouping separator and the end of the integer part (or the decimal separator) |
| `numbering_kind` | How a primary format token numbers: digits, letters, Roman numerals or words, or an unsupported token formatted as `1` |
| `alphabet` | The letters a numbering by letters counts with (`a` to `z`, Greek `α` to `ω`), named by its first letter: 1 is the first letter, after the last come two letters (bijective numeration) |
| `symbol_sequence` | A numbering sequence with one Unicode symbol per number over a fixed range, named by its symbol for 1 (CIRCLED DIGIT ONE `①`, 0 to 50); numbers outside the range are written in decimal |
| `ordinal` | Ordinal numbering (`1st`, `second`), asked for by the `o` format modifier |
| `variable_marker` | A part of a date picture string in square brackets, replaced by a date or time component (F&O 4.0, section 9.9.4.1) |
| `component_specifier` | The letter that starts a variable marker and names the component: `Y`, `M`, `D`, `d`, `F`, `W`, `w`, `H`, `h`, `P`, `m`, `s`, `f`, `Z`, `z`, `C`, `E` |
| `presentation_modifier` | The first (a primary format token or `n`, `N`, `Nn`) or second (`a`, `t`, `c`, `o`) modifier of a variable marker |
| `width_modifier` | The `,min-max` part of a variable marker: the minimum and maximum number of characters |
| `calendar` | The calendar a date is formatted in, named by a designator such as `AD` or `ISO` (F&O 4.0, section 9.9.4.8) |
| `fallback_marker` | Text such as `[Calendar: AD]` or `[Language: en]` put before a formatted date when sxml uses another calendar or language than the one asked for |
| `default_collation` | The collation used when a function or operator is given none (XPath 4.0, section 2.2.2) |
| `collation_binding` | A host's collation URI standing for a collation sxml implements, added to the statically known collations |
| `default_language` | The language of the dynamic context, for `fn:default-language` and the formatting functions |
| `environment_variable` | A name and value the host exposes to `fn:environment-variable`; sxml never reads the platform's environment |
| `collection` | The sequence of items `fn:collection` returns for a URI |
| `uri_collection` | The sequence of `xs:anyURI` items `fn:uri-collection` returns for a URI |
| `default_collection` | The collection (or URI collection) for a call without a URI (XPath 4.0, section 2.2.2) |
| `target_namespace_uri` | The namespace of an XQuery library module that `import module` asks for |
| `package_version` | The versions of an XSLT package that `xsl:use-package` accepts (XSLT 4.0, "Versions of a Package") |
| `trace_sink` | Host interface receiving the output of `fn:trace`, `fn:message` and `xsl:message` |
| `trace_event` | One piece of trace output: its kind, label, value and, for `xsl:message`, whether it terminates |
| `api_version` | The release whose interface the headers declare, as one integer (`SXML_API_VERSION`) |
| `xpath_expression` | An XPath expression compiled for one processor, evaluated any number of times |
| `expression_text` | The source text of an expression or query |
| `evaluation` | One run of a compiled expression or query against a dynamic context |
| `evaluation_depth` | How deeply evaluation nests (function calls, nested expressions); bounded so deep recursion is a limit, not a stack overflow |
| `evaluation_step` | One unit of evaluator work counted against `maximum_evaluation_step_count`: a task the machine runs, or an item pair a general comparison reads |
| `static_base_uri` | The base URI of the static context (XPath 4.0, section 2.2.1), against which fn:doc and fn:resolve-uri resolve relative URIs |
| `uri_resolution` | Resolving a URI reference against a base URI (RFC 3986, section 5.2) |
| `loaded_document` | A document fn:doc loaded during one evaluation, kept so the same URI gives the same node |
| `evaluator_task` | One entry of the evaluator's task stack: an expression, call or native step being evaluated, with the state that resumes it when a nested value arrives (D71, D75) |
| `task_stack` | The evaluator's explicit stack of evaluator tasks, in allocator memory, which replaces recursion on the C stack |
| `evaluator_part` | One `src/evaluator_<part>.c` file defining `sxml_evaluator_part_<part>`: the IR expression kinds it evaluates and their step functions (D101) |
| `part_task` | An evaluator task whose step function an evaluator part supplies, with state the part owns (D101) |
| `catching_task` | An evaluator task marked to receive the dynamic or type errors raised above it: the machine unwinds to it instead of ending the evaluation (`try`/`catch`, D101) |
| `receiving_task` | A node constructor's part task while its content is evaluated: the values of the delivering tasks above it go into its construction as they are made (D166) |
| `delivering_task` | An evaluator task in content position: when it finishes, its value is added to the construction of the receiving task below instead of being handed to the task below, and a loop's body or a call's body in it delivers each value at once (D166) |
| `content_let` | A LET whose variable only the content of its block reads, in order: in a delivering task its bound expression delivers too, and the variable holds the empty sequence (D166) |
| `error_details` | What a caught error binds in a catch clause, copied out of the error record: code, description, value, module, line and column number; bound as the map `$err:map` (XQuery 4.0, "Try/Catch Expressions"; D109) |
| `pending_error` | An error of a try or catch clause that a finally clause raises again once it has run without error (D109) |
| `tail_call` | A call whose value is the value of the function making it; the evaluator reuses that function's frame |
| `native_step` | One run of a resumable native body: it finishes with a value, or asks the evaluator to call a function item and to run the next step with the result |
| `resumable_native_body` | A native function body written as native steps, so a higher-order function never re-enters the evaluator on the C stack |
| `function_coercion` | Wrapping a function item so that its arguments and result are coerced to a required function type (XPath 4.0, section 3.4.4) |
| `atomization` | Replacing nodes by their typed values and arrays by their members, until only atomic items remain (XPath 4.0, section 2.5.3) |
| `type_match_frame` | An entry on the explicit stack of matching a value against a sequence type (instance of, treat as, coercion) |
| `coercion_frame` | An entry on the explicit stack of applying the coercion rules (XPath 4.0, section 3.4) to a value: a sequence, an item, a choice being tried, or the parts of an array, map or record |
| `static_namespaces` | The statically known namespaces of a compiled unit beyond the predeclared prefixes, kept for what resolves prefixes at run time (casts to xs:QName) |
| `call_each` | The evaluator's own resumable body that calls a sequence of function items in turn and concatenates the results (a dynamic call on several functions, XPath 4.0, section 4.6.3.1) |
| `expression_depth` | How deeply the syntax of an expression nests; bounded for untrusted expressions |
| `serialization` | Turning a sequence into bytes (Serialization 4.0) |
| `serialization_method` | The Serialization 4.0 output method: adaptive in Phase 1; xml, json, text and others later |
| `serialization_parameters` | The Serialization 4.0 parameters of one serialization (method, item separator, ...) |
| `item_separator` | The text written between the items of a serialized sequence |
| `serialization_parameter_names` | Fields of `sxml_serialization_parameters` spell the Serialization 4.0 parameter names with `_` for `-` (`omit_xml_declaration`, `use_character_maps`); `serialization_method` is `method`, `character_encoding` is `encoding`; a `has_` field marks a parameter that may be absent |
| `serialization_flag` | The value of a boolean serialization parameter: default (not given), no or yes |
| `character_map` | The pairs of the `use-character-maps` parameter: characters and the text written in their place (Serialization 4.0, section 12) |
| `character_mapping` | One pair of a character map: a codepoint and its `replacement_text` |
| `line_ending` | What the serializer writes for each newline (parameter `line-ending`): LF, CR LF or CR |
| `parameter_document` | An `output:serialization-parameters` element giving serialization parameters (Serialization 4.0, section 3.1) |
| `serialization_parameter_reader` | Reads serialization parameters from a parameter document or an options map and keeps what they refer to |
| `output_declaration` | An XQuery option declaration in the serialization namespace (`declare option output:indent "yes"`, XQuery 4.0, section 5.22) |
| `serialization_parameter_text` | A serialization parameter given as text, as an output declaration gives it: the parameter's local name and a value in the lexical form of a parameter document (D146) |
| `unit_evaluation` | One evaluation of a sealed IR unit with the host's dynamic context: what the XPath and XQuery APIs share (`sxml_evaluate_unit`, D146) |
| `query_run` | In the `sxml` program: one run of a query command (`xpath`, `xquery`, `serialize`): its processor, `--param` bindings and context item (`apps/sxml/query_command.h`) |
| `sequence_normalization` | Turning the sequence to serialize into one result tree: JNodes to their values, arrays flattened, atomic items to text, documents replaced by their children, adjacent text merged (Serialization 4.0, section 2) |
| `result_tree` | The document node that sequence normalization makes; what the XML, XHTML, HTML and text methods write |
| `sequence_normalizer` | Gives the children of the result tree one at a time (`normalized_event`: text or a node to copy), without building the tree |
| `character_pipeline` | What text goes through on its way out: character maps, Unicode normalization, escaping, line endings, encoding (Serialization 4.0, section 4) |
| `system_property` | A processor capability or fact, under its F&O 4.0 name (fn:system-properties, 17.1.9) and its XSLT 4.0 name (fn:system-property, 24.2.1); one table holds both |
| `cdata_section` | A text node's request to be serialized as CDATA sections (XSLT 4.0, 11.4.3: the cdata attribute of xsl:text and xsl:value-of); a builder and serializer property, not part of XDM (`is_cdata_section`) |
| `character_context` | Where characters are written: markup, text, an attribute value, a URI attribute value or CDATA sections; decides escaping and mapping |
| `escape_style` | How the character pipeline escapes text for a method: XML, Canonical XML, HTML, none (text), JSON strings or CSV fields |
| `staged_bytes` | Encoded bytes the serializer collects before one write to the output stream |
| `xml_writer` | Writes nodes by the XML or XHTML method: open elements and written bindings on stacks (D71) |
| `written_bindings` | The namespace bindings already declared for the open elements, against which an element's declarations and undeclarations are computed |
| `prefix_normalization` | XHTML with HTML5: elements in the XHTML, SVG or MathML namespace written unprefixed with a default namespace, and prefixes for those namespaces dropped (Serialization 4.0, section 6) |
| `raw_text` | Text the serializer writes unescaped whatever the escape style: the content of HTML `script` and `style` elements (character context `RAW_TEXT`) |
| `void_element` | An HTML element with an empty content model, written as a start tag alone (`<br>`; Serialization 4.0, 7.1) |
| `boolean_attribute` | An HTML attribute whose only value is its name, written in minimized form (`<option selected>`; Serialization 4.0, 7.2) |
| `xml_island` | An element the HTML method writes as XML because it is not an HTML element (Serialization 4.0, 7.1) |
| `sub_serialization` | Serializing a node into a string inside another serialization (the JSON method's nodes, json-node-output-method) |
| `atomic_text` | The lexical form of an atomic item (its cast to xs:string) as the serializer writes it, inline or in heap memory |
| `html_element` | An element the XHTML method recognizes as HTML: in the XHTML namespace, or with HTML5 in no namespace with an HTML5 element name |
| `uri_escaping` | NFC and %-escaping of non-ASCII characters in a URI attribute value of an HTML element (Serialization 4.0, section 4, rule a; appendix D) |
| `content_type_meta` | The meta element the XHTML and HTML methods add as the first child of head (parameter `include-content-type`) |
| `xquery_query` | An XQuery main module compiled for one processor, with the library modules it imports, evaluated any number of times |
| `main_module` | An XQuery module with a query body, the one a host compiles (XQuery 4.0, section 5) |
| `library_module` | An XQuery module that starts with a module declaration and is reached by `import module` |
| `module_import` | An `import module` declaration: a target namespace and its location hints (XQuery 4.0, 5.12) |
| `location_hint` | A URI after `at` in a module import, resolved against the importing module's static base URI; one resolver request each |
| `module_uri` | Also the URI an XQuery library module was found at: its source name and static base URI |
| `module_set` | The main module and every library module one compilation loads, each once; imports name namespaces, so a module sees every loaded module of a namespace it imports (XQuery 4.0, 5.12.2) |
| `reading_order` | The order the module set's static contexts are read in: imported modules before importers, a cycle broken where the walk meets it |
| `public_declaration` | A function, variable or item type a library module declares without %private: what an import makes visible (XQuery 4.0, 5.16, 5.18) |
| `global_number` | The number of a global variable of the unit (from 1); a static context maps each variable declaration it sees to one |
| `prolog` | The declarations of an XQuery module before its query body: setters, namespace declarations, imports, variables, functions, types, options (XQuery 4.0, section 5) |
| `declared_function` | A function declared in a prolog; one IR function of the unit per arity of its arity range |
| `arity_range` | The arities a function definition accepts: from its required parameters up to all its parameters (XQuery 4.0, 5.18) |
| `initial_context_value` | The context value of the query body and of every variable initializer: a context value declaration's value, else the host's (XQuery 4.0, 5.17) |
| `context_global` | The global variable of a unit with name 0 that holds the initial context value |
| `global_table` | The global variables of one unit in one evaluation, a slot per global number; the evaluation's own unit has the first, a unit evaluated on the same machine (fn:load-xquery-module) one more, kept until the evaluation ends |
| `unit_evaluation` | A native step that asks the machine to evaluate another unit's main function, its global table first, and to hand the value to the next step (D154) |
| `module_record` | The result of fn:load-xquery-module: the public variables and functions of the loaded module (F&O 4.0, 18.1) |
| `computation_order` | The order in which the global part computes declared variables: by the index of their initializer functions, which lowering adds dependencies first |
| `output_stream` | Interface that receives bytes, written in order |
| `output_buffer` | The built-in output stream: collects bytes in memory |
| `well_known_namespace` | A namespace with a fixed `namespace_uri_identifier` in every name pool (`SXML_NAMESPACE_XML_SCHEMA`) |
| `axis` | An XPath axis (`SXML_AXIS_CHILD`, ...), walked by an `axis_iterator` |
| `axis_iterator` | Walks the nodes of one axis from an origin node |
| `integer_range` | The bounds of a range sequence (`first_integer` to `last_integer`) |
| `descending_integer_range` | A sequence representation holding an `integer_range` whose items run from `last_integer` down to `first_integer` (`fn:reverse` of a range, D143) |
| `static_form` | The abstract syntax a function item was made from, compared up to the renaming of bound variables; two function items with the same static form and captured context are equivalent (F&O 4.0, section 1.9.5; D143) |
| `pair` | Two values a function relates or compares (`type_pair`, `item_pair`, `node_pair`) |
| `update` | Describes one change to a persistent map or array (`map_update`, `array_update`) |
| `name_test` | Which names a node test or path step matches: any name, one expanded name, or a wildcard |
| `wildcard` | A name test with `*` for the namespace (`*:local`) or the local name (`prefix:*`, `Q{uri}*`) |
| `duplicate_attribute_policy` | What a tree builder does with a second attribute of the same name: reject (XQuery) or replace (XSLT) |
| `raised_error` | The error an item heap keeps for an error record whose description or value was computed (`sxml_item_heap_raise_error`) |
| `error_status` | The status an error is raised with: static, dynamic or type error |
| `emptiness` | Whether a sequence has no items, as tested by `sxml_sequence_test_emptiness` (`fn:empty`) |
| `part` | One sequence of a concatenation, kept by reference; a concatenation's parts are never empty and never concatenations (`part_index`) |
| `part_end` | The number of items in a concatenation's parts up to and including one part; cached as parts are counted |
| `produced_item` | An item a sequence generator has produced, kept by its lazy sequence so every reading sees it (`produced_item_count`) |
| `kept_error` | The copy of a generator's error that a lazy sequence keeps and raises again on every later read (D41) |
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
| `regex` | A compiled regular expression of the F&O 4.0 dialect (F&O 4.0, section 6.1), immutable once compiled (`sxml_regex`); never `regexp` or `pattern` (`pattern` is the XSLT match pattern) |
| `regex_text` | The source text of a regular expression (`$pattern` of `fn:matches`, `fn:tokenize`, ...) |
| `regex_flags` | The flags of a regular expression, from the `$flags` string: `s` dot-all, `m` multi-line, `i` case-insensitive, `x` whitespace removed, `q` literal, `c` comments (F&O 4.0, section 6.2) |
| `regex_matcher` | The working state of matching one compiled regex: backtrack stack, captured groups, step count; one per thread (`sxml_regex_matcher`) |
| `regex_input` | The string a regex matcher searches (`$value` of `fn:tokenize`, the "input string" of F&O 4.0, section 6.1.1) |
| `regex_instruction` | One operation of a compiled regex, run by the backtracking matcher |
| `matching_segment` | A segment of the regex input that a regex matches: start and length, held as a byte range (F&O 4.0, section 6.1.1) |
| `disjoint_matching_segments` | The matching segments left after removing those that overlap an earlier one; what `fn:tokenize` and `fn:replace` work on (F&O 4.0, section 6.1.1) |
| `captured_group` | The segment a capturing subexpression matched; group 0 is the whole matching segment (F&O 4.0, section 6.1.5) |
| `group_number` | The number of a capturing subexpression: its capturing left parenthesis counted from 1, left to right |
| `case_variant` | A character that `i` matches in place of another: same `fn:lower-case` or same `fn:upper-case` (F&O 4.0, section 6.2) |
| `backtrack_stack` | The stack of choices and undo entries of a backtracking matcher; it grows through an allocator within a limit |
| `step_count` | Instructions a regex matcher has run in one search; `maximum_step_count` bounds it for untrusted regexes |
| `group_frame` | The regex compiler's record of a group whose ")" has not been read (and of the whole regex), kept on an explicit stack instead of recursing (D71) |
| `group_name` | The name of a named capturing subexpression, `(?<name>...)`; `\k<name>` refers back to it and result maps key its group by it (F&O 4.0, section 6.1.5) |
| `lookahead_group` | A captured group of a capturing subexpression within a lookahead assertion; it may lie outside the matching segment (F&O 4.0, `fn:analyze-string`) |
| `assertion_entry` | A backtrack-stack entry marking where a lookahead or lookbehind assertion started; the assertion's end and backtracking find it (D105) |
| `counted_loop` | A counted repetition `x{n,m}` compiled as `x` once plus a counter slot that counts its iterations, instead of copies of `x`; used when the copies would pass 256 instructions (D147) |
| `general_category` | The Unicode general category of a codepoint (Lu, Nd, ...), what a category escape `\p{..}` tests (XSD 1.1 Part 2, appendix G) |
| `block_escape` | `\p{IsX}`: the codepoints of Unicode block X, named as in Blocks.txt without spaces |
| `name_table` | The regex compiler's list of the group names read so far, for duplicates and `\k<name>` (D105) |
| `normalization_form` | One of NFC, NFD, NFKC, NFKD (UAX #15) or FULLY-NORMALIZED (F&O 4.0, `fn:normalize-unicode`) |
| `combining_class` | The Canonical_Combining_Class of a codepoint, 0 to 254; 0 is a starter (UAX #15) |
| `starter` | A codepoint of combining class 0 (UAX #15, D107) |
| `decomposition` | The full canonical or full compatibility decomposition of a codepoint: its UnicodeData.txt mapping applied until nothing changes (UAX #15) |
| `primary_composite` | A codepoint with a canonical decomposition of two codepoints that is not Full_Composition_Exclusion; canonical composition joins such pairs (UAX #15) |
| `composing_character` | A codepoint of combining class above 0, or the second codepoint of the canonical decomposition of a character not in CompositionExclusions.txt (F&O 4.0, `fn:normalize-unicode`) |
| `grapheme_cluster` | An extended grapheme cluster of UAX #29: one user-perceived character, one item of `fn:graphemes` |
| `grapheme_break` | The Grapheme_Cluster_Break property value of a codepoint (CR, LF, Extend, ZWJ, L, V, ...; UAX #29, section 3.1) |
| `conjunct_break` | The Indic_Conjunct_Break property value of a codepoint (None, Linker, Consonant, Extend), for rule GB9c of UAX #29 |
| `character_reference_name` | An HTML named character reference without "&" and ";" (`nbsp`, `NotEqualTilde`), an argument of `fn:char` (HTML, section 13.5) |
| `library_part` | The native functions of one `src/function_library_<part>.c` file: one `sxml_native_function_view`, named in a function library's configuration |
| `part_registry` | The list of library parts or evaluator parts that CMake generates from the glob of their files, so a new part needs no edit elsewhere (D101) |
| `language_set` | The languages (XPath, XQuery, XSLT) a library part serves; a part without one serves all three (D101) |
| `collation` | Rules for comparing strings, named by a URI; Phase 1 knows only the Unicode codepoint collation (F&O 4.0, section 5.3) |
| `case_mapping` | The Unicode full case mapping of `fn:upper-case` and `fn:lower-case`: one codepoint to one or more (F&O 4.0, section 5.4) |
| `deep_equality` | `fn:deep-equal` of two sequences: items compared pairwise, into nodes, maps and arrays (F&O 4.0, section 14.2.4) |
| `comparison_pair` | Two items, or two runs of children, waiting to be compared on the explicit stack of deep equality |
| `collation_key` | Bytes whose octet order is the order of strings under a collation (F&O 4.0, `fn:collation-key`) |
| `collation_element` | The weights one codepoint gives a collation: primary, secondary, tertiary and quaternary, zero where it is ignorable (UTS #10, section 3.2) |
| `collation_unit` | A collation element that is not ignorable on every compared level; substring matching under a collation compares these (F&O 4.0, section 5.4) |
| `collation_strength` | How many levels of collation elements a UCA collation compares, primary to identical (UTS #10; the `strength` parameter) |
| `uca_fallback` | sxml's stand-in for a UCA collation URI: the levels of UTS #10 over weights derived from Unicode properties, without DUCET tables (D97, D116) |
| `collation_match` | The bytes of a string that equal a search string under a collation, starting and ending at collation unit boundaries |
| `comparison_unit` | One significant child (a node or a merged text run), or one top-level item, as deep equality compares it |
| `trial` | In an unordered deep-equal comparison, the comparison of one unit with one candidate; a difference inside it moves on to the next candidate |
| `sort_key_value` | The atomized key of one item in `fn:sort`: a sequence of atomic items compared item by item |
| `random_seed` | The value that fixes the numbers of `fn:random-number-generator` |
| `traversal` | A higher-order function's walk over the items of a sequence, members of an array or entries of a map, asking for one call of a function item per position (`fn:for-each`, `fn:filter`, `map:for-each`, ...) |
| `accumulator` | The value a fold carries from one call of its function item to the next (`fn:fold-left`, `fn:scan`) |
| `comparator` | A function item of `fn:sort-with` that orders two items by returning a negative, zero or positive integer |
| `sort_key_definition` | One key of `fn:sort-by`: a key function (or `fn:data#1`), a collation and an order direction (F&O 4.0, `fn:sort-key-record`) |
| `sort_key_rule` | A SORT key's collation and direction, resolved once when the SORT is evaluated (D152) |
| `composite_sort_key` | A sort key whose value is a sequence of atomic items compared item by item, the shorter first when one starts the other (XSLT 4.0, 13.1.2) |
| `partially_applied_function` | The function item `fn:partial-apply` or a placeholder call makes: the wrapped function with some arguments bound, captured with it |
| `operator_function` | A function item made by `fn:op`: `fn($x, $y) { $x op $y }` for one binary operator |
| `assembly` | How `map:build` and `map:merge` add entries one by one to a map, combining duplicate keys by the `duplicates` option |
| `duplicates_policy` | The `duplicates` option of `map:build` and `map:merge`: reject, use-first, use-last, use-any, combine, or a function |
| `json_text` | A text in the JSON syntax of RFC 8259: the `$value` of `fn:parse-json` and `fn:json-to-xml` |
| `json_event` | One step of walking a JSON text: the start or end of an object or array, a key, or a scalar; what a JSON sink receives |
| `json_sink` | Interface receiving the JSON events of one walk: it makes XDM values (`fn:parse-json`) or tree builder events (`fn:json-to-xml`) |
| `xml_representation_of_json` | The elements `map`, `array`, `string`, `number`, `boolean` and `null` in the fn namespace, with `key`, `escaped` and `escaped-key` attributes, that stand for a JSON text (F&O 4.0, section 17.4.2) |
| `escaped_form` | Text whose special characters are kept as JSON escape sequences (option `escape`; attributes `escaped`, `escaped-key`) |
| `permitted_character` | A codepoint an xs:string can hold: U+0001 to U+D7FF, U+E000 to U+FFFD, U+10000 to U+10FFFF (F&O 4.0, section 17.4.3) |
| `unpermitted_escape` | A JSON escape sequence for a codepoint that is not a `permitted_character` (U+0000, an unpaired surrogate, U+FFFE, U+FFFF); what the `fallback` option of `fn:parse-json` is called with (D143) |
| `json_replacement` | The string value of the fallback function's result for one codepoint, inserted for each of its `unpermitted_escape`s (D143) |
| `number_format` | The `number-format` option of `fn:parse-json`: `double`, `decimal` or `adaptive` |
| `null_value` | The value that stands for JSON null in `fn:parse-json` (option `null`, the empty sequence by default) |
| `key_index` | The hash index of the keys of the JSON objects (or map elements) open at once, to find duplicate keys in O(1) |
| `serialization_frame` | An entry of the serializer's explicit stack: an array, map, JNode or member sequence whose items are being written (D71) |
| `fuzz_target` | A libFuzzer entry point (`tests/fuzz/<name>_fuzz.c`) that feeds generated bytes to one sxml entry point and stops on a crash, a leak or a broken invariant |
| `fuzz_fixture` | The objects every fuzz target makes fresh for each input (counting allocator, name pool, type registry, item heap, function library) and checks for leaks after it |
| `seed` | An input a fuzz target starts from, taken from a vendored test suite by `tools/fuzz-seeds.py`; the seeds of one target are its seed corpus |
| `element_layout` | How `fn:element-to-map` represents one element (F&O 4.0, 14.6.1): `empty`, `empty-plus`, `simple`, `simple-plus`, `list`, `list-plus`, `record`, `sequence`, `mixed`, `xml`, plus the plan-only `deep-skip` and `error` |
| `conversion_plan` | The map that chooses element layouts and content types by element and attribute name (F&O 4.0, 14.6.5); one `plan_entry` per name, `*` the `fallback_layout` |
| `attribute_marker` | The string put before the key of an attribute in an element map (`@` by default) |
| `content_key` | The key of an element's simple content or nilled marker in an element map (`#content` by default, `#` prepended against conflicts) |
| `nilled_marker` | The xs:QName `fn:null` that stands for a nilled element in an element map |
| `name_format` | The `name-format` option of `fn:element-to-map`: `default`, `eqname`, `local` or `lexical` |
| `element_slot` | The work record `fn:element-to-map` keeps per node of the converted tree: chosen layout and the value built, filled in reverse document order (D71) |
| `hash_algorithm` | One of the algorithms `fn:hash` names: MD5, SHA-1, SHA-256, BLAKE3, CRC-32 (F&O 4.0, fn:hash) |
| `digest` | The octets a hash algorithm returns for a message: 16 (MD5), 20 (SHA-1), 32 (SHA-256, BLAKE3) or 4 (CRC-32, big-endian) |
| `uri_structure` | The parts of a URI as `fn:parse-uri` finds them and `fn:build-uri` joins them: the fields of `fn:uri-structure-record` (F&O 4.0, section 7.6.1) |
| `percent_decoding` | Replacing `%HH` by its octet and reading the octets as UTF-8, with U+FFFD for what is not (F&O 4.0, fn:decode-from-uri); `form_decoding` turns `+` into a space first |
| `javascript_package` | The hand-written TypeScript package in `bindings/javascript/` (npm name `sxml-js` for now) that loads `sxml.wasm` in Node.js and browsers (D10, D138) |
| `javascript_shim` | The C entry points `sxml.wasm` exports for the JavaScript package (`bindings/javascript/sxml_javascript.c`), written against the public API only |
| `handle` | The address of a C object in WebAssembly memory that a JavaScript object owns; released by `dispose()` or, failing that, by a `FinalizationRegistry` |
| `size_budget` | The most bytes a WebAssembly module may have, stripped and gzipped, checked by a ctest (D93) |
| `csv_dialect` | The characters that structure a CSV text, each one character: separator, quote character and optional comment marker, and whether unquoted fields are trimmed of whitespace (F&O 4.0, section 17.5) |
| `csv_token` | What reading a CSV text gives next: a field (its text, quoted or not), the end of a row (a row with no field before it is a blank row) or the end of the input |
| `csv_cursor` | Where reading a CSV text continues: a byte offset and whether it is at the start of a row, after a field that ended its row, or at the end |
| `schema` | A compiled XSD 1.1 schema: the schema components assembled from one or more schema documents, with its types registered in the processor's type registry (`sxml_schema`, D165) |
| `schema_document` | An XML document whose document element is xs:schema (XSD 1.1 Part 1, 3.17.2); read by the XML parser, found through the resource resolver (kind `SCHEMA_DOCUMENT`) |
| `schema_document_source` | Where the configuration's schema document comes from: its text, or the resource resolver |
| `schema_component` | One of the building blocks of a schema (XSD 1.1 Part 1, 2.2): a type definition, element or attribute declaration, attribute use, model group, particle, wildcard, identity-constraint definition, type alternative, assertion or notation declaration |
| `element_declaration` | The schema component that governs elements of one name: type, nillable, abstract, value constraint, substitution group, identity constraints, type alternatives (XSD 1.1 Part 1, 3.3) |
| `attribute_declaration` | The schema component that governs attributes of one name: simple type and value constraint (XSD 1.1 Part 1, 3.2) |
| `content_type_variety` | What a complex type allows as content: empty, simple, element-only or mixed (XSD 1.1 Part 1, 3.4.1, {content type} {variety}) |
| `simple_content_type` | The simple type of the content of a complex type whose content type variety is simple; it gives the typed value |
| `facet` | One constraining facet of a simple type's restriction step (XSD 1.1 Part 2, 4.3): its `facet_kind` (length, pattern, enumeration, minInclusive, ...) and its value as text |
| `constraint_name` | The name XSD 1.1 gives a rule an error breaks (`cvc-complex-type.2.4`, `src-resolve`; Part 1, appendix C "Outcome Tabulations"): the local name, in no namespace, of the error code schema errors carry |
| `schema_report` | One schema or validation error as a report sink receives it: constraint name, description, location |
| `schema_report_sink` | The interface that receives every schema report of a compilation or validation, so that validation can go on after the first error |
| `schema_validator` | The tree builder that assesses the events it receives against a schema and passes them on, annotated (type annotation, is-id, is-idrefs, nilled, defaults), to its result builder (D165) |
| `result_builder` | The tree builder a schema validator sends its annotated events to; the null builder discards them, for validity checks that build nothing |
| `validation_mode` | How the validation root is assessed when no governing type is given: strict (a declaration must exist) or lax (XSD 1.1 Part 1, 5.2) |
| `governing_type` | The type a validation root is assessed against when the caller names one (XQuery `validate type`, XSLT `type`); otherwise the declaration of its name decides (XSD 1.1 Part 1, "governing type definition", 3.3.4) |
| `validation_root` | The element (or attribute) where one validation starts: its subtree is assessed, and IDs and IDREFs are checked within it (XSD 1.1 Part 1, "validation root", cvc-id) |
| `validity` | The outcome XSD gives an assessed item (XSD 1.1 Part 1, the [validity] property): valid, invalid or not known |
| `validation_outcome` | What a schema validator reports once its tree has ended: the validity of the validation root and the count of errors |
| `in_scope_schemas` | The compiled schemas a static context imports: their types, element and attribute declarations are XPath 4.0's in-scope schema definitions (section 2.2.1) |
| `schema_components` | The store of one schema's components (`sxml_schema_components`, `src/schema_internal.h`, D167): one array per schema table, addressed by index with entry 0 "none", plus the name index and the blocks compile stages allocate |
| `schema_table` | One array of the schema components: a component kind (particles, element declarations, ...) or what links them (references, lists, texts, namespace bindings) |
| `schema_position` | Where a component's XML representation starts: its schema document's index, line and column |
| `symbol_space` | One of XSD's seven partitions of global component names: type definitions, element declarations, attribute declarations, model group definitions, attribute group definitions, identity-constraint definitions, notation declarations (XSD 1.1 Part 1, 2.6) |
| `schema_name_index` | The hash table from symbol space and expanded name to the global component of that name |
| `schema_reference` | A QName attribute of a schema document (`type`, `base`, `ref`, `substitutionGroup`, `refer`, ...) as read: a symbol space and a name, which the resolve stage turns into a component index |
| `builtin_component` | A component every schema has (XSD 1.1 Part 1, 3.2.7, 3.4.7, 3.16.7): the built-in type definitions, the content of xs:anyType and the four xsi: attribute declarations, added first so their indexes are fixed |
| `compile_stage` | One step of schema compilation, a function of its own file: read, resolve, simple types, complex types, identity constraints, assertions (D167) |
| `schema_compilation` | The state one compilation passes from stage to stage: the configuration, the schema components and the count of errors reported |
| `derivation_set` | A set of derivation methods (extension, restriction, list, union, substitution) as a bit set: XSD's {final}, {prohibited substitutions}, {disallowed substitutions}, {substitution group exclusions} |
| `explicit_content` | The particle a complex type definition's XML representation gives itself, before the complex types stage combines it with the base type's (XSD 1.1 Part 1, 3.4.2.3.3) |
| `particle` | The component that says how often a term may occur: minimum and maximum occurrences and the term (XSD 1.1 Part 1, 3.9) |
| `term` | What a particle contains: an element declaration, a model group or a schema wildcard |
| `model_group` | A term that combines particles with a compositor (XSD 1.1 Part 1, 3.8) |
| `compositor` | How a model group combines its particles: sequence, choice or all |
| `model_group_definition` | A named model group, the target of `<xs:group ref>` (XSD 1.1 Part 1, 3.7) |
| `attribute_use` | The component that says whether an attribute is required, with which declaration and value constraint, in a complex type or attribute group (XSD 1.1 Part 1, 3.5) |
| `attribute_group` | A named set of attribute uses and an attribute wildcard (XSD 1.1 Part 1, 3.6, "attribute group definition") |
| `schema_wildcard` | XSD's wildcard component: a namespace constraint, disallowed names and process contents, matching elements or attributes by name (XSD 1.1 Part 1, 3.10); not the XPath `wildcard` name test |
| `namespace_constraint` | Which namespaces a schema wildcard allows: any, an enumeration, or all but an enumeration (`not`) |
| `process_contents` | How an item a schema wildcard matched is assessed: strict, lax or skip |
| `open_content` | Elements a complex type allows besides its particle, matched by a schema wildcard, anywhere (interleave) or after the content (suffix) (XSD 1.1 Part 1, 3.4.1) |
| `value_constraint` | The default or fixed value of an element or attribute declaration or an attribute use, as its lexical form (XSD 1.1 Part 1, 3.3.1) |
| `identity_constraint` | A unique, key or keyref definition: a name, a selector and fields as XPath records, and the referenced key of a keyref (XSD 1.1 Part 1, 3.11) |
| `type_alternative` | One entry of an element declaration's type table: an XPath test and the type it selects (XSD 1.1 Part 1, 3.12) |
| `schema_assertion` | An xs:assert of a complex type or an xs:assertion facet of a simple type: an XPath test that must be true (XSD 1.1 Part 1, 3.13) |
| `notation_declaration` | A named notation with its system and public identifiers (XSD 1.1 Part 1, 3.14) |
| `namespace_context` | The namespace bindings, XPath default namespace and base URI that QName values and XPath records of a schema document are read with |
| `xpath_record` | XSD's XPath Expression property record: the expression text with its namespace context, and its compiled form once a stage compiles it (XSD 1.1 Part 1, 3.13.1) |
| `substitution_group` | The element declarations that may appear where a head declaration is expected: those whose substitution group affiliations reach it (XSD 1.1 Part 1, 3.3.6.2) |
| `content_model` | The compiled form of a complex type's particle: a deterministic automaton over element names whose transitions are element declarations and schema wildcards, with counters for large maxOccurs (D167) |
| `content_model_counter` | A counter of a content model that counts the repetitions of one particle between its minimum and maximum occurrences, instead of a copy of states per repetition |
| `content_model_cursor` | Where matching an element's children has reached in its content model: a state and the counter values |
| `expected_term` | A term a content model cursor would accept next, for error messages ("expected 'shipTo'") |
| `content_model_position` | One occurrence of an element or wildcard particle in the particle tree as a content model compiles it (small repetitions expanded into copies): a state of the Glushkov automaton before determinization |
| `content_model_transition` | A move of a content model state on an element declaration (with its substitutable members) or a schema wildcard, with counter tests and counter operations |
| `counter_test` | A condition of a content model transition or final state: a content model counter's value lies in a range |
| `counter_operation` | What a content model transition does to a counter: set it to one (entering the counted particle) or add one (repeating it) |
| `suffix_state` | The content model state that suffix open content moves to once the content may end: it accepts only the open content wildcard |
| `default_binding` | What an element or attribute is bound to when it is valid: a declaration, an attribute use, or strict, lax or skip; restriction compares them (XSD 1.1 Part 1, 3.4.6.4) |
| `scratch_array` | A growable array of fixed-size entries in allocator memory for the work lists of a compile stage |
| `recorded_subtree` | The validated copy of an element and its descendants that the schema validator keeps while the element is open, for the XPath of assertions and identity constraints |
| `id_table` | The IDs and IDREFs a schema validator has met under one validation root, checked at its end (cvc-id) |
| `assessment` | How the schema validator assesses one element or attribute: strictly, against a governing type; laxly, where nothing governs it (global declarations are used where they exist, validity not known); or skipped with its subtree (processContents skip) (XSD 1.1 Part 1, 5.2 and 3.10.1) |
| `validation_frame` | The schema validator's entry for one open element: its assessment, governing declaration and type, content type, content model cursor, nilled and error state, and where its collected text and namespace bindings start |
| `pending_element` | The start of an element that the schema validator holds back, with its namespace bindings and attributes, until its first content or its end: xsi:type, xsi:nil and the attributes decide its governing type and annotation first |
| `schema_normalized_value` | A simple value after the whiteSpace facet of its type (XSD 1.1 Part 2, 4.3.6): what a validated attribute's or simple element's text becomes (XDM 4.0, 7.5.7.4) |
| `deferred_check` | An assertion or identity-constraint check of an element that reads its recorded subtree, queued by the schema validator and run when the outermost recorded element ends |
| `type_table` | An element declaration's type alternatives with a test and its default type definition (the last alternative without a test, else the declared type); two are compared by equivalence (XSD 1.1 Part 1, 3.3.2.2, 3.8.6.3) |
| `inherited_attribute` | An attribute of an ancestor attributed to an attribute use, or governed by a declaration, whose {inheritable} is true: type alternatives see it as the element's own when the element has none of that name (XSD 1.1 Part 1, 3.3.5.6) |
| `basic_member` | A member type of a union with every union member replaced by its own members, in order: an atomic or list type; the active basic member is the first the value is valid for (XSD 1.1 Part 2, 2.4.1.3) |
| `assertion_copy` | The copy of an element's recorded subtree that the assertions of its complex type evaluate on: nothing outside the element, the element itself annotated xs:anyType (XSD 1.1 Part 1, 3.13.4.1) |
| `schema_element_role` | The part an element of a schema document plays where it appears (a global or a local element declaration, a group reference, a facet, ...): it decides the element's attributes, its children and the component it is read into (`src/schema_read.c`, D170) |
| `child_slot` | One place in the content of a schema element role: the roles allowed there, once or any number of times, required or not; the children of an element fill its role's slots in order, as the schema for schema documents allows |
| `conditional_inclusion` | Ignoring an element of a schema document with its attributes and descendants, as its vc: attributes say (vc:minVersion, vc:maxVersion, vc:typeAvailable, ...), before it is read (XSD 1.1 Part 1, 4.2.2) |
| `chameleon_inclusion` | Including (or redefining, overriding) a schema document without a targetNamespace from one with a target namespace: its components take the includer's namespace (XSD 1.1 Part 1, 4.2.3) |
| `schema_assembly` | The read stage's work list of schema documents: each one, known by its absolute URI and target namespace, is read once; xs:include, xs:import, xs:redefine and xs:override add to it (XSD 1.1 Part 1, 4.2; D170) |
| `redefinition` | A component that an xs:redefine or xs:override child gives in place of the component of its name in the document it refers to (XSD 1.1 Part 1, 4.2.4 and 4.2.5) |
| `xsd_pattern` | A regular expression of XSD's own dialect (XSD 1.1 Part 2, appendix G), the value of a pattern facet: anchored to the whole value, without F&O's additions; the regex engine's `is_xsd_pattern` flag (D53, D171) |
| `effective_facets` | The facets of a simple type with those of its bases it does not overlay, and those built-in types have implicitly, one per kind; what a restriction's own facets are checked against (XSD 1.1 Part 1, 3.16.6.4; D171) |
| `value_order` | XSD's order of two values of one primitive type: less, equal, greater or incomparable (partial for dates, times and durations, NaN incomparable; XSD 1.1 Part 2, 2.2.3) |
| `special_type` | xs:anyType, xs:anySimpleType and xs:anyAtomicType: no list item type, union member or restriction base of a schema's simple type (XSD 1.1 Part 1, 3.16.6.2) |
