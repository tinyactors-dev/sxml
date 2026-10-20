# JSON, CSV, HTML and Invisible XML

<!-- examples: formats -->

Not all data comes as XML. The XDM 4.0 data model that sxml's languages
share has room for other formats: JSON becomes maps and arrays, CSV
becomes rows of strings, HTML becomes a tree of elements, and any text
with a grammar can become XML. Once data is in the model, everything in
the previous chapters works on it. The examples are in
`docs/book/examples/formats`.

## JSON

`weather.json`:

```json
{{#include ../examples/formats/weather.json}}
```

`json-doc` reads a JSON file into maps and arrays (`parse-json` does the
same with a string). Then the lookup operator `?` navigates it, as
Chapter 2 showed:

```console
$ sxml xpath 'json-doc("weather.json")?city'
"Lisbon"
$ sxml xpath 'json-doc("weather.json")?days?*[?rain]?date'
"2026-10-21"
"2026-10-22"
$ sxml xpath 'json-doc("weather.json")?days?1'
{"date":"2026-10-20","high":2.35e1,"low":1.6e1,"rain":false()}
```

`?days?*` is every member of the `days` array, `[?rain]` keeps the days
whose `rain` is true, and `?date` gets their dates. JSON numbers become
doubles, as the third result shows, and JSON's `null` becomes the empty
sequence.

To write JSON, build maps and arrays and use the `json` output method,
as in Chapters 2 and 3.

### JSON as XML

Sometimes it's more convenient to treat JSON as XML, for example to
transform it with XSLT templates. `json-to-xml` turns JSON into an XML
vocabulary of `map`, `array`, `string`, `number`, `boolean` and `null`
elements, and `xml-to-json` turns that vocabulary back into JSON text:

```console
$ sxml xpath --method=xml 'json-to-xml(unparsed-text("weather.json"))//*[@key = "days"]/*[1]'
<map xmlns="http://www.w3.org/2005/xpath-functions"><string key="date">2026-10-20</string><number key="high">23.5</number><number key="low">16</number><boolean key="rain">false</boolean></map>
$ sxml xpath --method=text 'xml-to-json(json-to-xml(unparsed-text("weather.json")))'
{"city":"Lisbon","days":[{"date":"2026-10-20","high":23.5,"low":16,"rain":false},{"date":"2026-10-21","high":21,"low":15.5,"rain":true},{"date":"2026-10-22","high":19.5,"low":14,"rain":true}]}
```

## CSV

`sales.csv`:

```
{{#include ../examples/formats/sales.csv}}
```

`parse-csv` reads CSV text. With `"header": true()`, the first row names
the columns. The result is a record with the column names, a map from
each name to its position, and the rows, each an array of strings:

```console
$ sxml xpath 'parse-csv(unparsed-text("sales.csv"), { "header": true() })?rows'
["North","Tea","120","3.50"]
["South","Tea","80","3.50"]
["North","Coffee","95","4.20"]
["South","Coffee","130","4.20"]
```

The `csv` output method writes arrays as rows, so a query can read CSV,
compute, and write CSV again:

`revenue.xq`:

```xquery
{{#include ../examples/formats/revenue.xq}}
```

```console
$ sxml xquery revenue.xq
product,units,revenue
Coffee,225,945
Tea,200,700
```

`$row($column?units)` looks up the position of the `units` column and
then that member of the row, so the query keeps working if the columns
are reordered. `csv-to-xml` turns CSV into XML instead, with `row` and
`field` elements.

## HTML

Real HTML is rarely well-formed XML: end tags are left out, attributes
have no quotes, and browsers fix it all up by rules that the HTML
standard spells out. `html-doc` and `parse-html` parse HTML by exactly
those rules, into elements in the XHTML namespace:

`shop.html`:

```html
{{#include ../examples/formats/shop.html}}
```

```console
$ sxml xpath 'html-doc("shop.html")//*:tr[*:td] ! { string(*:td[1]): xs:decimal(*:td[2]) } => map:merge()'
{"Tea":3.5,"Coffee":4.2,"Cake":2.8}
$ sxml xpath 'html-doc("shop.html")/*:html/*:head/*:title/string()'
"Corner Shop"
```

Because the elements are in the XHTML namespace, a plain `td` would not
match them; `*:td` matches `td` in any namespace. In XQuery, you can
write `declare default element namespace "http://www.w3.org/1999/xhtml";`
instead and use plain names. The parser found the `head`, the `tbody`
and every missing end tag, as a browser would:

```console
$ sxml xpath 'parse-html("<p>One<p>Two")//*:p => count()'
2
```

To write HTML, use the `html` output method, which knows which elements
have no end tag and how to write `<!DOCTYPE html>`.

## Invisible XML

Invisible XML (ixml) turns any text that has a grammar into XML. You
write the grammar, and `invisible-xml` compiles it into a parser
function. Here is a grammar for a simple log format:

`log.ixml`:

```
{{#include ../examples/formats/log.ixml}}
```

Each rule says what a part of the text looks like: a `log` is one or
more `line`s separated by line feeds (`#a`), and so on. Marks control the
XML: `@` makes an attribute, and `-` hides a part, so the spaces between
the parts and the `digit` wrapper do not appear. `~[#a]*` is any
characters except a line feed.

`server.log`:

```
{{#include ../examples/formats/server.log}}
```

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("log.ixml"))(unparsed-text("server.log"))'
<log><line date="2026-10-20" time="09:15:02" level="INFO"><message>server started</message></line><line date="2026-10-20" time="09:17:45" level="WARN"><message>disk 91% full</message></line><line date="2026-10-20" time="09:21:13" level="ERROR"><message>disk full</message></line><line date="2026-10-20" time="09:21:14" level="INFO"><message>cleanup started</message></line></log>
```

Now the log is a document, and XPath can query it:

```console
$ sxml xpath 'let $parse := invisible-xml(unparsed-text("log.ixml")) return $parse(unparsed-text("server.log"))//line[@level = "ERROR"] ! `{@time} {message}`'
"09:21:13 disk full"
```

Text that does not match the grammar gives a document that says where
the parse failed and what was expected:

```console
$ sxml xpath --method=xml 'invisible-xml(unparsed-text("log.ixml"))("2026-10-20 9:15:02 INFO oops")'
<failed xmlns:ixml="http://invisiblexml.org/NS" ixml:state="failed"><line>1</line><column>13</column><position>13</position><unexpected>":"</unexpected><expected>["0"-"9"]</expected></failed>
```

The hour has one digit where the grammar wants two. With the option
`{ "fail-on-error": true() }`, the parser raises an error instead.
