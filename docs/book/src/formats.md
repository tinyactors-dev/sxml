# JSON, CSV and HTML

<!-- examples: traces -->

Not all data comes as XML. The XDM 4.0 data model that sxml's languages
share has room for other formats: JSON becomes maps and arrays, CSV
becomes rows of strings, and HTML becomes a tree of elements. Once data
is in the model, everything in the previous chapters works on it. The
examples use the trace files of [Project: Comparing Traces Before and
After a Change](traces.md), in `docs/book/examples/traces`.

## JSON

`json-doc` reads a JSON file into maps and arrays (`parse-json` does the
same with a string). Then the lookup operator `?` navigates it. An OTLP
file is an object whose `resourceSpans` is an array; each member has
`scopeSpans`, an array again, whose members have `spans`:

```console
$ sxml xpath 'json-doc("before.json")?resourceSpans?1?resource?attributes?1?value?stringValue'
"storefront"
$ sxml xpath 'json-doc("before.json")?resourceSpans?*?scopeSpans?*?spans?* => count()'
14
$ sxml xpath 'json-doc("before.json")?resourceSpans?*?scopeSpans?*?spans?*[?kind = 3]?name'
"GET /api/getFlags"
"SELECT flags"
"SELECT flags"
"UPDATE flags"
"GET"
"POST /api/killFlag"
```

`?1` is the first member of an array, and `?*` is every member, so
`?resourceSpans?*?scopeSpans?*?spans?*` is every span of every scope of
every resource. `[?kind = 3]` keeps the client spans, and `?name` gets
their names: the backend SDK's call of the service, the service's
queries to PostgreSQL and Redis (the Redis span is named after its
command, `GET`), and the console's call. A key that is not a name,
such as `Duration (ms)`, is looked up with a string in parentheses:
`?("Duration (ms)")`.

JSON numbers become doubles, and JSON's `null` becomes the empty
sequence:

```console
$ sxml xpath 'json-doc("before.json")?resourceSpans?1?scopeSpans?1?spans?1?kind'
3.0e0
```

A double has 53 bits for its digits, about sixteen decimal digits.
That is why OTLP writes the 64-bit integers, times in nanoseconds and
`intValue` attributes, as strings: a JSON reader that turns numbers
into doubles would lose their last digits. Two times one nanosecond
apart are the same double, but `xs:integer` reads the strings exactly:

```console
$ sxml xpath 'parse-json("1792573200000000001") = parse-json("1792573200000000000")'
true()
$ sxml xpath 'xs:integer("1792573200000000001") - xs:integer("1792573200000000000")'
1
```

When you read JSON that writes such numbers as numbers anyway, ask
`parse-json` or `json-doc` for decimals with the option `number-format`.
A decimal keeps every digit; `"adaptive"` makes a decimal of a number
written without an exponent and a double of one written with it:

```console
$ sxml xpath 'parse-json("1792573200000000001", { "number-format": "decimal" }) - parse-json("1792573200000000000", { "number-format": "decimal" })'
1
$ sxml xpath 'parse-json("[1792573200000000001, 2.5e0]", { "number-format": "adaptive" })?* ! (. instance of xs:decimal)'
true()
false()
```

To write JSON, build maps and arrays and use the `json` output method.
[The Trace Diff](traces-diff.md) writes its result that way, and
`select.xq` there writes a whole OTLP file.

### JSON as XML

Sometimes it's more convenient to treat JSON as XML, for example to
search it with paths or transform it with XSLT templates. `json-to-xml`
turns JSON into an XML vocabulary of `map`, `array`, `string`, `number`,
`boolean` and `null` elements, with the keys in `key` attributes, and
`xml-to-json` turns that vocabulary back into JSON text. Here is the
span with a given id, wherever it is in the file:

```console
$ sxml xpath --method=xml 'json-to-xml(unparsed-text("before.json"))//*:map[*:string[@key = "spanId"] = "fb73f92d181aced2"]'
<map xmlns="http://www.w3.org/2005/xpath-functions"><string key="traceId">48114dcf6840d1bd46460c4b1ce0d09d</string><string key="spanId">fb73f92d181aced2</string><string key="parentSpanId">448683a55e6803a1</string><string key="name">SELECT flags</string><number key="kind">3</number><string key="startTimeUnixNano">1792573200005000000</string><string key="endTimeUnixNano">1792573200044000000</string><array key="attributes"><map><string key="key">db.system.name</string><map key="value"><string key="stringValue">postgresql</string></map></map><map><string key="key">db.operation.name</string><map key="value"><string key="stringValue">SELECT</string></map></map><map><string key="key">db.collection.name</string><map key="value"><string key="stringValue">flags</string></map></map><map><string key="key">db.query.text</string><map key="value"><string key="stringValue">SELECT key, name, state, percentage, rules, owners FROM flags</string></map></map><map><string key="key">server.address</string><map key="value"><string key="stringValue">postgresql</string></map></map><map><string key="key">server.port</string><map key="value"><string key="intValue">5432</string></map></map></array></map>
$ sxml xpath --method=text 'json-to-xml(unparsed-text("before.json"))//*:map[*:string[@key = "spanId"] = "fb73f92d181aced2"]/*:array => xml-to-json()'
[{"key":"db.system.name","value":{"stringValue":"postgresql"}},{"key":"db.operation.name","value":{"stringValue":"SELECT"}},{"key":"db.collection.name","value":{"stringValue":"flags"}},{"key":"db.query.text","value":{"stringValue":"SELECT key, name, state, percentage, rules, owners FROM flags"}},{"key":"server.address","value":{"stringValue":"postgresql"}},{"key":"server.port","value":{"intValue":"5432"}}]
```

## CSV

The `csv` output method writes each array as a row, quoting the fields
that need it. A query can so write the spans for a spreadsheet:

`spans-csv.xq`:

```xquery
{{#include ../examples/traces/spans-csv.xq}}
```

```console
$ sxml xquery spans-csv.xq
route,service,name,kind,start,duration
GET /api/getFlags,storefront,GET /api/getFlags,client,0,50
GET /api/getFlags,vane,GET /api/getFlags,server,1,48
GET /api/getFlags,vane,flags.listFlags,internal,2,46
GET /api/getFlags,vane,registry.getFlags,internal,3,43
GET /api/getFlags,vane,SELECT flags,client,5,39
POST /api/killFlag,vane-console,POST /api/killFlag,client,0,34
POST /api/killFlag,vane,POST /api/killFlag,server,1,32
POST /api/killFlag,vane,flags.killFlag,internal,2,30
POST /api/killFlag,vane,cache.getFlag,internal,3,3
POST /api/killFlag,vane,GET,client,4,1
POST /api/killFlag,vane,registry.getFlag,internal,7,9
POST /api/killFlag,vane,SELECT flags,client,8,7
POST /api/killFlag,vane,registry.setPercentage,internal,17,13
POST /api/killFlag,vane,UPDATE flags,client,18,11
```

The other way round, a tracing tool's user interface often exports a
trace as CSV. `spans.csv` is such an export of the new version's
`GET /api/getFlags`, the backend SDK downloading the flags:

`spans.csv` (the start):

```
{{#include ../examples/traces/spans.csv:1:8}}
```

`parse-csv` reads CSV text. With `"header": true()`, the first row names
the columns. The result is a record with the column names, a map from
each name to its position, the rows, each an array of strings, and a
function `get` that takes a row number and a column name:

```console
$ sxml xpath 'parse-csv(unparsed-text("spans.csv"), { "header": true() })?columns'
"Trace ID"
"Span ID"
"Parent ID"
"Service"
"Operation"
"Start Time"
"Duration (ms)"
"Statement"
$ sxml xpath 'parse-csv(unparsed-text("spans.csv"), { "header": true() })?rows[7]'
["95cfc9416dff30ee3edeae4b2c53e1c4","68edbeb8d88faf5c","3916e2d3c40454dd","vane","SELECT flags","2026-10-21 09:00:00.018","16","SELECT key, name, state, percentage, rules, owners FROM flags WHERE key = $1"]
$ sxml xpath 'parse-csv(unparsed-text("spans.csv"), { "header": true() })?get(7, "Statement")'
"SELECT key, name, state, percentage, rules, owners FROM flags WHERE key = $1"
```

The statement has commas in it, so the file quotes it, and `parse-csv`
takes the quotes away. A query can read CSV, compute, and write CSV
again:

`durations.xq`:

```xquery
{{#include ../examples/traces/durations.xq}}
```

```console
$ sxml xquery durations.xq
service,operation,calls,total ms
storefront,GET /api/getFlags,1,110
vane,GET /api/getFlags,1,108
vane,SELECT flags,5,72
vane,flags.listFlags,1,104
vane,registry.getFlag,4,80
vane,registry.getFlagKeys,1,12
```

The five queries take 72 of the 104 milliseconds of `flags.listFlags`.
`order by` compares strings by their code points, so `SELECT flags`,
in capitals, comes before the function names in lower case.

`$row($column?Service)` looks up the position of the `Service` column
and then that member of the row, so the query keeps working if the
columns are reordered; a column name with spaces needs a string,
`$column?("Duration (ms)")`. `csv-to-xml` turns CSV into XML instead,
with `row` and `field` elements:

```console
$ sxml xpath --method=xml 'csv-to-xml(unparsed-text("spans.csv"), { "header": true() })//*:row[1]'
<row xmlns="http://www.w3.org/2005/xpath-functions"><field column="Trace ID">95cfc9416dff30ee3edeae4b2c53e1c4</field><field column="Span ID">6a06eb67c8c7b1df</field><field column="Parent ID"/><field column="Service">storefront</field><field column="Operation">GET /api/getFlags</field><field column="Start Time">2026-10-21 09:00:00.000</field><field column="Duration (ms)">110</field><field column="Statement"/></row>
```

## HTML

Real HTML is rarely well-formed XML: end tags are left out, attributes
have no quotes, and browsers fix it all up by rules that the HTML
standard spells out. `html-doc` and `parse-html` parse HTML by exactly
those rules, into elements in the XHTML namespace. This is the status
page of Vane, the service, its console, and the two databases the
service uses:

`status.html`:

```html
{{#include ../examples/traces/status.html}}
```

```console
$ sxml xpath 'html-doc("status.html")/*:html/*:head/*:title/string()'
"Vane status"
$ sxml xpath 'html-doc("status.html")//*:tr[*:td] ! { string(*:td[1]): normalize-space(*:td[3]) } => map:merge()'
{"vane":"up","vane-console":"up","postgresql":"up","redis":"slow"}
$ sxml xpath 'html-doc("status.html")//*:td[@class = "slow"]/preceding-sibling::*:td[1]/string()'
"7.4.1"
$ sxml xpath 'xs:dateTime(html-doc("status.html")//*:time/@datetime)'
xs:dateTime("2026-10-21T09:05:00Z")
```

Because the elements are in the XHTML namespace, a plain `td` would not
match them; `*:td` matches `td` in any namespace. In XQuery, you can
write `declare default element namespace "http://www.w3.org/1999/xhtml";`
instead and use plain names. The parser found the `head`, the `tbody`
and every missing end tag, as a browser would. A cell without an end
tag runs to the next tag, so the last cell of each row holds the line
break after it, and `normalize-space` removes it.

```console
$ sxml xpath 'parse-html("<p>One<p>Two")//*:p => count()'
2
```

To write HTML, use the `html` output method, which knows which elements
have no end tag and how to write `<!DOCTYPE html>`; [Rendering the
Diff](traces-rendering.md) writes a report with it.

## Other text

Text that has a grammar but no standard parser, a log format or the
syntax of a configuration file, can become XML too: Invisible XML turns
a grammar into a parser. [Parsing Text with Invisible XML](ixml.md)
shows how.
