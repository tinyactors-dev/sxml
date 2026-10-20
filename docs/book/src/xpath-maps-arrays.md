# Maps, Arrays and Records

<!-- examples: xpath -->

Documents are trees, but a lot of data is not: a lookup table, a list of
rows, the response of a web service in JSON. XPath 4.0 has two
structures for such data. A **map** associates keys with values, and an
**array** is a list of values by position. Both are items, so they can
be in sequences, be passed to functions and be put inside each other.

## Maps

A map is written with curly braces, with a key and a value for each
entry:

```console
$ sxml xpath '{ "title": "Dune", "year": 1965 }'
{"title":"Dune","year":1965}
```

(You can also write `map { ... }`, as XPath 3.1 required.) The `?`
operator looks up a key:

```console
$ sxml xpath 'let $book := { "title": "Dune", "year": 1965 } return $book?title'
"Dune"
```

A key that is not in the map gives the empty sequence, and `?*` gives
all the values:

```console
$ sxml xpath 'let $map := { "a": 1, "b": 2 } return ($map?a, $map?z, "and", $map?*)'
1
"and"
1
2
```

The keys are atomic values: strings, numbers, dates and so on. Maps are
values, so they never change. Functions in the `map` namespace make
new maps from old ones:

```console
$ sxml xpath '{ "a": 1 } => map:put("b", 2) => map:remove("a")'
{"b":2}
$ sxml xpath 'map:keys({ "a": 1, "b": 2 }), map:size({ "a": 1, "b": 2 })'
"a"
"b"
2
```

## Building maps from documents

Maps are a good way to index a document. `map:merge` combines many maps
into one:

```console
$ sxml xpath --context-item=library.xml 'map:merge(//book ! { string(@id): xs:integer(@year) })'
{"geb":1979,"taocp":1968,"sicp":1986,"dune":1965,"lhod":1969}
```

`map:build` groups items by a key you compute. Here, the books by shelf:

```console
$ sxml xpath --context-item=library.xml 'map:build(//book, fn { string(../@name) }, fn { string(@id) })'
{"Computing":("geb","taocp","sicp"),"Fiction":("dune","lhod")}
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
sequence:

```console
$ sxml xpath 'array { 1 to 3 }, [10, 20, 30]?*'
[1,2,3]
10
20
30
```

`for member` goes through the members of an array:

```console
$ sxml xpath 'for member $pair in [[1, "one"], [2, "two"]] return $pair?2'
"one"
"two"
```

## JSON

Maps and arrays are exactly what JSON is made of, which makes JSON easy
to read and write. `parse-json` reads JSON text, and the `json` output
method writes maps and arrays as JSON:

```console
$ sxml xpath 'parse-json('"'"'{"title": "Dune", "tags": ["desert", "politics"]}'"'"')?tags?2'
"politics"
$ sxml xpath --context-item=library.xml --method=json 'array { //shelf ! { "shelf": string(@name), "books": array { book/title/string() } } }'
[{"shelf":"Computing","books":["Godel, Escher, Bach","The Art of Computer Programming","Structure and Interpretation of Computer Programs"]},{"shelf":"Fiction","books":["Dune","The Left Hand of Darkness"]}]
```

(The `'"'"'` is how a shell puts a single quote inside single quotes.
In a query file, you don't need it.) That second command turned an XML
document into JSON in one line. Chapter 6 has much more on JSON.

## Records

A *record type* describes maps with known keys, like a struct in other
languages. Use it where a sequence type is allowed, to check that a map
has the shape you expect:

```console
$ sxml xpath 'let $book as record(title as xs:string, year as xs:integer) := { "title": "Dune", "year": 1965 } return $book?year'
1965
$ sxml xpath 'let $book as record(title as xs:string, year as xs:integer) := { "title": "Dune" } return $book?year'
expression:1:5: err:XPTY0004: The value does not match the required type (XPath 4.0, section 3.4, coercion rules)
```

The second map has no `year`, so it is not a value of the record type.
Records shine in XQuery and XSLT, where function signatures can use
them.
