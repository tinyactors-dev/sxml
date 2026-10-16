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
| `memory_requirements` | Size and alignment an object needs |
| `configuration` | Struct describing how an object is set up |
| `request` | Struct describing one operation |
| `status` | The result code of a fallible function (`sxml_status`) |
| `error_record` | A specification error code (such as `FOAR0001`) with details |
| `diagnostic` | Caller-supplied record receiving position and context of a failure |
| `static_context` | XPath static context |
| `dynamic_context` | XPath dynamic context |
| `focus` | Context item, context position and context size together |
| `context_item` | The item the focus is on |
| `intermediate_representation` | The shared IR; `ir` allowed as a module name only |
| `grammar` | A tree-sitter grammar under `grammars/<language>/`, and the parser generated from it |
| `test_suite` | The unit tests of one module: one test executable |
| `test_case` | One named test function within a test suite |
| `test_check` | One checked condition within a test case |
