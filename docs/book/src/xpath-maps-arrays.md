# Maps, Arrays and Records

<!-- examples: spec -->

Documents are trees, but a lot of data is not: a lookup table, a list of
rows, the response of a web service in JSON. XPath 4.0 has two
structures for such data. A **map** associates keys with values, and an
**array** is a list of values by position. Both are items, so they can
be in sequences, be passed to functions and be put inside each other.

## Maps

A map is written with curly braces, with a key and a value for each
entry:

```console
$ sxml xpath '{ "anchor": "req-7", "keyword": "MUST" }'
{"anchor":"req-7","keyword":"MUST"}
```

(You can also write `map { ... }`, as XPath 3.1 required.) The `?`
operator looks up a key:

```console
$ sxml xpath 'let $requirement := { "anchor": "req-7", "keyword": "MUST" } return $requirement?keyword'
"MUST"
```

A key that is not in the map gives the empty sequence, and `?*` gives
all the values:

```console
$ sxml xpath 'let $requirement := { "anchor": "req-7", "keyword": "MUST" } return ($requirement?anchor, $requirement?section, "and", $requirement?*)'
"req-7"
"and"
"req-7"
"MUST"
```

The keys are atomic values: strings, numbers, dates and so on. A map is
also a function from its keys to its values, so `$requirement("keyword")`
is the same lookup; that is how the month map of
[Values and Types](xpath-values.md) was used. Maps are values, so they
never change. Functions in the `map` namespace make new maps from old
ones:

```console
$ sxml xpath '{ "anchor": "req-7" } => map:put("keyword", "MUST") => map:remove("anchor")'
{"keyword":"MUST"}
$ sxml xpath 'map:keys({ "MUST": 5, "MAY": 2 }), map:size({ "MUST": 5, "MAY": 2 })'
"MUST"
"MAY"
2
```

## Building maps from documents

Maps are a good way to index a document. An `xref` without text, such as
`<xref target="RFC2119"/>`, gets its text from what it points to: a
renderer writes "[RFC2119]" for a reference and "Section 4" for a
section. A map from anchor to that text is the lookup table such a
renderer needs. `map:merge` combines many maps into one:

```console
$ sxml xpath --context-item=vane.xml 'map:merge(//section ! { @anchor: string(name) })'
{"intro":"Introduction","terminology":"Terminology","states":"Flag States","messages":"Messages","rollout":"Rollout","status":"Status","pause":"Pause","security":"Security Considerations","iana":"IANA Considerations"}
```

The key `@anchor` is an attribute, which becomes its value when it is
used as a key. Now the whole table: sections numbered as in
[Paths, Steps and Predicates](xpath-paths.md), references in brackets,
and terms by their text:

```console
$ sxml xpath --context-item=vane.xml 'let $text := map:merge((//section ! { @anchor: "Section " || string-join(ancestor-or-self::section ! (count(preceding-sibling::section) + 1), ".") }, //reference ! { @anchor: `[{@anchor}]` }, //dt ! { @anchor: string() })) return ($text?pause, $text?RFC2119, $text?term-percentage, map:size($text))'
"Section 4"
"[RFC2119]"
"Percentage"
17
```

Every element with an anchor can go into one map, here from anchor to
the element's name. Its keys are the targets an `xref` may name, so
an `xref` whose target is not a key points at nothing:

```console
$ sxml xpath --context-item=vane.xml 'map:merge(//*[@anchor] ! { @anchor: name() }) => map:size()'
33
$ sxml xpath --context-item=vane.xml 'let $kinds := map:merge(//*[@anchor] ! { @anchor: name() }) return count(//xref[empty($kinds(@target))])'
0
```

Thirty-three anchors, and no dangling reference. `map:merge` keeps the
first value when two maps have the same key; with the option
`{ "duplicates": "reject" }` it raises an error instead, which makes it
a check that the anchors are unique. Key words are not unique, so this
fails:

```console
$ sxml xpath --context-item=vane.xml 'map:merge(//t[@anchor] ! { string(bcp14): string(@anchor) }, { "duplicates": "reject" })'
expression:1:1: err:FOJS0003: Duplicate keys are rejected by the duplicates option
```

When several items share a key, `map:build` groups them. Here, the
requirements by key word:

```console
$ sxml xpath --context-item=vane.xml 'map:build(//t[@anchor], fn { string(bcp14) }, fn { string(@anchor) })'
{"MUST":("req-1","req-2","req-4","req-6","req-7"),"SHOULD":("req-3","req-8"),"MUST NOT":"req-5","MAY":("req-9","req-11"),"SHOULD NOT":"req-10"}
```

The value of an entry can be any sequence, as here. `fn { ... }` is a
small function; the next section explains them.

## Arrays

An array is written with square brackets. Unlike a sequence, an array
keeps its members apart, even when a member is itself a sequence:

```console
$ sxml xpath '[1, 2, (3, 4)]'
[1,2,(3,4)]
$ sxml xpath '[1, 2, (3, 4)]?3'
3
4
```

`?3` looks up the third member. `array { ... }` makes an array with one
member for each item of a sequence, and `?*` turns an array back into a
sequence. A reference has one or more authors, and an array of arrays
keeps each reference's authors together, which a flat sequence could
not:

```console
$ sxml xpath --context-item=vane.xml 'array { //reference ! array { .//author/@surname ! string() } }'
[["Bradner"],["Crocker","Overell"],["Leiba"],["M'Raihi","Machani","Pei","Rydell"]]
$ sxml xpath --context-item=vane.xml 'let $authors := array { //reference[@anchor = "RFC6238"]//author/@surname ! string() } return ($authors?1, array:size($authors), $authors?*[last()])'
"M'Raihi"
4
"Rydell"
```

`for member` goes through the members of an array:

```console
$ sxml xpath --context-item=vane.xml 'for member $authors in array { //reference ! array { .//author/@surname ! string() } } return string-join($authors?*, " and ")'
"Bradner"
"Crocker and Overell"
"Leiba"
"M'Raihi and Machani and Pei and Rydell"
```

## JSON

Maps and arrays are exactly what JSON is made of, which makes JSON easy
to read and write. The `json` output method writes maps and arrays as
JSON. Tools that check an implementation against a specification often
want its requirements as JSON; here is the list, an array with a map
for each requirement, written to a file with `--output`:

```console
$ sxml xpath --context-item=vane.xml --method=json --output=xpath-requirements.json 'array { //t[@anchor] ! { "anchor": string(@anchor), "keyword": string(bcp14), "section": string(ancestor::section[1]/name) } }'
$ cat xpath-requirements.json
[{"anchor":"req-1","keyword":"MUST","section":"Flag States"},{"anchor":"req-2","keyword":"MUST","section":"Flag States"},{"anchor":"req-3","keyword":"SHOULD","section":"Flag States"},{"anchor":"req-4","keyword":"MUST","section":"Rollout"},{"anchor":"req-5","keyword":"MUST NOT","section":"Rollout"},{"anchor":"req-9","keyword":"MAY","section":"Rollout"},{"anchor":"req-6","keyword":"MUST","section":"Status"},{"anchor":"req-10","keyword":"SHOULD NOT","section":"Status"},{"anchor":"req-7","keyword":"MUST","section":"Pause"},{"anchor":"req-8","keyword":"SHOULD","section":"Pause"},{"anchor":"req-11","keyword":"MAY","section":"Pause"}]
```

That turned part of an XML document into JSON in one line. (`--output`
does not replace a file that exists; add `--force` to run it again.)
Reading it back gives maps and arrays again. `json-doc` reads a JSON
file, and `parse-json` reads JSON text:

```console
$ sxml xpath 'json-doc("xpath-requirements.json")?*[?keyword = "MAY"]?anchor'
"req-9"
"req-11"
$ sxml xpath 'parse-json(unparsed-text("xpath-requirements.json"))?3'
{"anchor":"req-3","keyword":"SHOULD","section":"Flag States"}
$ sxml xpath 'parse-json('"'"'{"anchor": "req-7", "states": ["checking", "paused"]}'"'"')?states?2'
"paused"
```

`?*` gives the members of the array, and the predicate `[?keyword =
"MAY"]` looks up `keyword` in each. (The `'"'"'` in the last command is
how a shell puts a single quote inside single quotes. In a query file,
you don't need it.) [JSON, CSV and HTML](formats.md) has much more on
JSON.

## Records

A *record type* describes maps with known keys, like a struct in other
languages. Use it where a sequence type is allowed, to check that a map
has the shape you expect:

```console
$ sxml xpath 'let $requirement as record(anchor as xs:string, keyword as xs:string, section as xs:string) := { "anchor": "req-7", "keyword": "MUST", "section": "Pause" } return $requirement?section'
"Pause"
$ sxml xpath 'let $requirement as record(anchor as xs:string, keyword as xs:string, section as xs:string) := { "anchor": "req-7", "keyword": "MUST" } return $requirement?section'
expression:1:5: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

The second map has no `section`, so it is not a value of the record
type. A record type also converts its fields, as a function signature
converts its arguments: a field declared `xs:string` turns a node into
its string. So the requirement can be made from the paragraph's nodes:

```console
$ sxml xpath --context-item=vane.xml 'let $requirement as record(anchor as xs:string, keyword as xs:string, section as xs:string) := //t[@anchor = "req-7"] ! { "anchor": @anchor, "keyword": bcp14, "section": ancestor::section[1]/name } return $requirement'
{"anchor":"req-7","keyword":"MUST","section":"Pause"}
```

And the JSON file we wrote has exactly that shape, so its members can be
checked as records when they are read:

```console
$ sxml xpath 'let $requirements as record(anchor as xs:string, keyword as xs:string, section as xs:string)* := json-doc("xpath-requirements.json")?* return $requirements[?section = "Rollout"]?anchor'
"req-4"
"req-5"
"req-9"
```

Records shine in XQuery and XSLT, where function signatures can use
them.
