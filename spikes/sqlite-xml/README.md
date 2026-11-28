# Spike: binary XML in SQLite, queried with sxml

A spike, not a design: nothing here has been through `tools/spec/spec`, it
uses the host shim's internal header, and it calls `malloc`. It answers
two questions:

1. Is there a standard, stable binary encoding of XML we should use?
2. What would a SQLite extension look like that runs XPath, XQuery (and
   later XSLT, validation) over XML kept as small blobs, the way SQLite's
   `json_*` functions work over JSONB?

## Answer in short

- **The standard is EXI** (W3C Efficient XML Interchange, Recommendation
  2011, second edition 2014), and **Canonical EXI** (W3C Recommendation,
  7 June 2018) makes its bytes stable. But EXI is an interchange format: a
  bit-packed event stream decoded through grammars. To answer an XPath
  over it you decode it into a tree first, which is a parse. No standard
  binary XML format is built to be queried in place. Fast Infoset (ITU-T
  X.891) has the same problem.
- **SQLite's JSONB is not a standard either.** It is SQLite's own
  documented format, and it is not even canonical: size fields need not
  be minimal. "Like JSONB" means a format we own and specify, versioned,
  with a stable layout.
- **What databases actually do** is a node table in document order plus a
  vocabulary: the XPath accelerator's pre/size/level numbering (Grust
  2002; MonetDB/XQuery, BaseX), Saxon's TinyTree, SQL Server's xml type
  (the MS-BINXML token stream plus a "primary XML index", which is a node
  table), and Oracle's binary XML with a shared token repository.
- **sxml already numbers nodes this way** (`include/sxml/tree.h`: preorder,
  attributes first, `last_descendant`, which is pre/size). So the natural
  format is sxml's arena tree laid flat. That is what this spike's
  **XDMB** is, and `xdmb_open` implements `sxml_tree_operations` directly
  over the blob's bytes. sxml's whole evaluator (XPath, XQuery, and XSLT
  through the same tree interface) then runs over a blob with no parse.
- **Recommendation:** XDMB (or its successor) as the storage format,
  specified like SQLite's jsonb.md; EXI as an import/export format
  (`xml_to_exi`, `xml_from_exi`) if interchange is wanted. Different jobs.

## What was built

| File | What |
|---|---|
| `xdmb.h`, `xdmb.c` | the format: an encoder over any `sxml_tree`, and an `sxml_tree` that reads a blob in place |
| `sqlite_xml.c` | the loadable extension: `xmlb`, `xml`, `xml_xpath`, `xml_exists`, `xml_query` |
| `build.sh` | builds `xml.so` against an sxml build directory |
| `smoke.py`, `differential.py`, `bench.py`, `breakdown.py`, `orders.py` | checks and measurements |

```sql
select load_extension('./xml.so', 'sqlite3_xml_init');
create table orders(id integer primary key, doc blob);
insert into orders(doc) values (xmlb('<order status="paid">...</order>'));

select sum(xml_xpath(doc, 'sum(//line/(@qty * @price))')) from orders;
select id from orders where xml_exists(doc, '/order[@status = "refunded"]');
select xml_query(doc, '<summary>{count(//line)}</summary>') from orders;
select xml(doc) from orders where id = 17;     -- back to text
create index orders_customer on orders(xml_xpath(doc, 'string(/order/customer/@id)'));
```

- `doc` is XML text or an XDMB blob; a blob is queried where SQLite holds
  it, for the duration of the call.
- One atomic result comes back as an SQL integer, real or text; anything
  else is serialized (adaptive method); the empty sequence is `NULL`.
- Compiled expressions are cached per statement (`sqlite3_set_auxdata`),
  so a constant expression compiles once per statement, not per row.
- The functions are `DETERMINISTIC`, so expression indexes work: the
  query above goes from a 110 ms scan to a 0.1 ms index search over
  20,000 orders (`SEARCH orders USING COVERING INDEX`).

## The format (XDMB version 1)

```
"XDMB" version widths                 node, name, payload widths: 1, 2 or 4 bytes
varint node_count string_count name_count set_count text_heap_size
varint document_uri base_uri          string numbers
strings        varint length, bytes   string 0 is ""
names          varint uri, local, prefix (string numbers)
namespace sets varint count, (prefix, uri) pairs sorted by prefix: the in-scope set
node records   kind_flags(1) name parent last_descendant payload, fixed size per document
text heap      varint length, bytes; equal texts stored once
```

- Every node operation is O(1) or O(depth): `describe_node` reads one
  fixed-size record; first child is `index + 1 + attributes`, next
  sibling `last_descendant + 1`, last child and previous sibling climb
  parents.
- Opening a blob interns its names into the processor's name pool and
  checks every record once (names in range, parents before children,
  descendant ranges nested), so a corrupt blob is refused, not misread.
- **Stable:** the same XDM gives the same bytes. Tables are numbered in
  order of first occurrence, namespace bindings are sorted, widths are
  minimal, and re-encoding a blob gives the same blob.
- Consequence of sorting: `serialize()` writes namespace declarations in
  prefix order, which Canonical XML does too. The XDM is unchanged.
- Kept: comments, processing instructions, prefixes, CDATA and
  disable-output-escaping marks, ID/IDREFS flags (from a DTD), nilled.
  Not kept yet: type annotations of validated documents (it reports
  `xs:untyped`), source locations, unparsed entities.

## Results

Machine: x86_64, Intel Xeon @ 2.30 GHz, 4 CPUs; SQLite 3.53.1 through
Python's `sqlite3`; sxml at `c98f2439a`, release build, clang. 2026-11-27.

**Correctness** (`differential.py`): 2,877 documents from `third_party`
(the XML, XSD, XSLT and QT4 test suites), each queried as text and as
XDMB with 17 expressions covering every axis, namespaces, attributes,
string values and `deep-equal(/, parse-xml(serialize(/)))`: 48,909
comparisons, 0 differences. Every blob re-encodes to the same bytes.

**Speed** (`bench.py`, 20,000 order documents of about 950 B, best of 3):

| Query | text, per row | XDMB, per row | Speedup |
|---|---|---|---|
| `sum(//line/(@qty * @price))` | 93.0 us | 10.1 us | 9.2x |
| `xml_exists(... [@status][shipping/@country])` | 85.7 us | 3.5 us | 24.3x |
| `string(/order/customer/name)` | 92.4 us | 4.1 us | 22.5x |
| XQuery element constructor | 99.9 us | 6.4 us | 15.6x |
| `(//line)[last()]/@sku` | 92.3 us | 5.4 us | 17.2x |

`xmlb()` itself costs about 97 us per document (a parse plus the encode).

**Size** is the weak point. The first 2,000 of those documents:

| Encoding | Bytes | vs text |
|---|---|---|
| text | 1,892,690 | 1.00 |
| XDMB v1 | 1,807,409 | 0.95 |
| EXI byte-aligned (EXIficient 1.0.4, schema-less, comments, PIs and prefixes kept) | 1,090,918 | 0.58 |
| EXI bit-packed | 916,450 | 0.48 |
| deflate -9, per document | 839,034 | 0.44 |
| EXI compression mode | 751,119 | 0.40 |

Where XDMB's bytes go (`breakdown.py`): node records 43% (5.75 B per
node, 134,315 nodes, 24,045 of them whitespace-only text), the text heap
31%, and the per-document tables 26%: every row carries its own
vocabulary, about 240 B for these documents.

## What would make it small

Each of these is unmeasured:

1. **A shared vocabulary.** Most rows of a table use the same names and
   namespaces. A vocabulary table the blobs refer to, as Oracle's token
   repository and EXI's schema-informed mode do, removes most of the 26%.
   The cost is that a blob is no longer self-contained: `xml()` needs the
   vocabulary, and so does moving a row to another database.
2. **Smaller records.** Store the subtree size relative to the node (it
   usually fits a byte, where the node index needs two) and derive the
   parent instead of storing it: about 3.75 B per node instead of 5.75.
   The cost: parent becomes O(siblings) rather than O(1), unless a small
   index is built when the blob is opened.
3. **Typed text.** Numbers and dates as binary values in the heap, as EXI
   does with a schema. Only worth it once validated documents keep their
   types.
4. **Whitespace-only text** as an opt-in at `xmlb()` (it changes the XDM,
   like `xml:space` stripping).
5. **Compression on top** (a per-table zstd dictionary) gives text-like
   ratios but loses in-place reads: each row would be decompressed per
   query, though that is still far cheaper than a parse.

## What a real extension needs

- `xml_each(doc, path)`, a table-valued function like `json_each`, to
  turn nodes into rows (`select ... from orders, xml_each(doc, '//line')`).
- Namespace declarations and external variables for the functions
  (`xml_xpath(doc, expr, 'p', 'urn:...')` or a pragma-like
  `xml_namespace('p', 'urn:...')`).
- Updates: XQuery Update over a blob gives a new blob.
- XSLT (`xml_transform(doc, stylesheet)`) and validation (`xml_valid(doc,
  schema)`, and keeping type annotations in the blob).
- Builds: the same C builds as a native loadable extension and, with
  SQLite's wasm build, in the browser.
- In sxml itself: the format as a header and spec article, its own
  allocator use, no `malloc`; and a decision on where the extension lives
  (`apps/`, `bindings/`, or its own repository).

## Querying EXI (2026-11-27, after sxml 0.3.0)

`bench_exi.py`: the spike's queries over the same 20,000 orders stored as
text, XDMB and eight EXI forms; every result equal across all ten. EXI
blobs are decoded into a tree on every call (`sxml_host_exi_parse`), their
options read from the header, or, for schema-informed streams, from the
connection (`xml_exi_schema(xsd, alignment)`). "full" keeps comments, PIs
and prefixes, as XDMB does; the others keep EXI's default fidelity.
Machine: x86_64, Intel Xeon, 4 CPUs; SQLite 3.53.1; sxml at `678c9a354`,
release build, clang. Times in microseconds per row, best of 3.

| form | bytes | vs text | encode | open only | revenue | filter | extract | xquery | last line |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| text | 18,891,166 | 1.00 | | 127.1 | 138.9 | 137.5 | 136.8 | 134.3 | 133.9 |
| XDMB | 18,064,333 | 0.96 | 141.4 | 3.3 | 14.8 | 5.2 | 5.6 | 9.1 | 7.9 |
| EXI bit-packed | 8,631,905 | 0.46 | 163.3 | 24.8 | 35.2 | 27.0 | 25.3 | 29.7 | 29.0 |
| EXI byte-aligned | 10,506,382 | 0.56 | 157.7 | 22.4 | 33.5 | 27.0 | 27.6 | 28.3 | 29.0 |
| EXI pre-compression | 10,506,382 | 0.56 | 172.7 | 23.7 | 36.7 | 29.2 | 26.9 | 34.9 | 30.0 |
| EXI compression | 7,280,167 | 0.39 | 213.1 | 33.2 | 44.4 | 34.0 | 35.1 | 38.6 | 38.7 |
| EXI bit-packed, full | 9,281,326 | 0.49 | 170.8 | 25.2 | 35.2 | 25.7 | 26.8 | 30.6 | 27.4 |
| EXI compression, full | 7,855,496 | 0.42 | 216.9 | 35.8 | 46.1 | 35.7 | 36.2 | 41.5 | 36.6 |
| EXI schema bit-packed | 4,396,634 | 0.23 | 206.1 | 66.2 | 78.6 | 67.5 | 64.4 | 76.8 | 66.1 |
| EXI schema compression | 4,042,055 | 0.21 | 256.5 | 69.7 | 82.6 | 76.7 | 75.0 | 76.8 | 74.2 |

- Decoding EXI costs about 22 to 25 us an order (950 B of XML), five times
  less than parsing the text (127 us) and seven times more than opening
  XDMB in place (3.3 us). DEFLATE adds about 10 us.
- Schema-informed streams are half the size of schema-less ones but decode
  three times slower. A separate run with orders of 1 and 40 lines: about
  40 us per decode is fixed, even for a 48 B stream (the grammars are
  built from the schema for every document; the decoder keeps nothing
  between calls), and each line costs about 1.4 us against 0.7 us
  schema-less (typed values turned back into text). Building the
  grammars once per schema would remove the fixed part (unmeasured).
- What the numbers say for the extension: XDMB for columns that are
  queried; EXI where size matters more than query time (archive,
  transfer): bit-packed EXI is 0.46 of the text and still five times
  faster to query than text, schema-informed 0.23 once its grammars are
  cached.

## XDMB version 2: the gaps closed (2026-11-28)

Version 2 adds a sections byte to the header and, after the text heap,
four optional sections (layout at the top of `xdmb.c`): TYPES (a type
table and a type number per node), ENTITIES (the unparsed entities),
LOCATIONS (line, column and byte offset per node, opt-in with
`xmlb(doc, 'locations')`) and BASES (entity base URIs by element). A
document with none of them is version 1's layout plus one byte. Typed
blobs come from `xmlb(doc, 'validate')` with `xml_schema(xsd)`, and are
opened with the same schema; built-in types resolve without one.

**Fidelity** (`fidelity_test.py`):

| What | Result |
|---|---|
| Typed documents whose annotations are named types or anonymous types of global declarations | 242 of 242 identical (42 instances of the W3C XSD test suites, 200 orders against `orders-named.xsd`): every node's type annotation, and every typed value with the type of each item |
| Typed documents with an anonymous type of a local declaration (an inline `xs:complexType` on a local element) | 16 of the 58 valid XSD-suite instances, and every order against `orders.xsd`; written as the nearest named base type, all of them differ |
| Locations | 2,877 documents, `fn:location` of every node identical |
| Unparsed entities | 49 documents declare them; 47 blobs carry the section (the other 2: unverified, likely declared in an external subset the spike does not load); all 49 re-encode to the same bytes |
| Entity base URIs | written and read, untested: the spike has no resolver, so no external entity is ever loaded |

The approximation must go: an anonymous local type cannot be named
through sxml's public API (no content model access, no component
designators), and a named base type stands for it wrongly (an
element-only type becomes `xs:anyType` with a typed value). The fix is a
schema API that names every type portably: W3C Schema Component
Designators (`/schemaElement::order/type()/schemaElement::customer/type()`),
or an ordinal per type in schema document order. That is a header
change (additive, a decision record). Until then, an encoder should
refuse such a document rather than approximate.

**Cost** (20,000 orders, best of 3, us per row; same machine as above):

| form | size vs text | encode | open only | revenue | extract |
|---|--:|--:|--:|--:|--:|
| text | 1.00 | | 122.8 | 133.5 | 131.5 |
| XDMB (v2, untyped) | 0.96 | 170.0 | 3.1 | 16.7 | 5.3 |
| XDMB + locations | 1.24 | 164.2 | 3.3 | 17.5 | 5.7 |
| XDMB typed (orders-named.xsd) | 1.06 | 206.8 | 4.0 | 26.3 | 6.7 |

- Untyped documents pay one byte each (18,084,333 against 18,064,333 for
  20,000).
- Types cost 10% in size (a byte per node and the type table) and about
  1 us to open (the type names are resolved on every open; cacheable).
  The slower `revenue` is semantics, not storage: typed `@price` is
  `xs:decimal`, so the sum is decimal arithmetic instead of doubles.
- Locations cost 28% in size: three fixed-width numbers per node. Delta
  encoding would shrink them (unmeasured); they stay opt-in.
- Validation drops locations: the validated tree sxml builds does not
  keep the parser's, so "typed + locations" equals "typed" (a finding
  about sxml's validator, not XDMB).
