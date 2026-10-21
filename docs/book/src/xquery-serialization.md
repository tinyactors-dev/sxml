# Writing Results

<!-- examples: spec -->

A query computes a value: a sequence of nodes, atomic values, maps and
arrays. To see it, or to save it, the value must be turned into text.
That step is called *serialization*, and *XSLT and XQuery Serialization
4.0* defines how it works. This section shows the choices you have, and
writes the requirements index of the draft in each of the forms people
and programs ask for.

## Output methods

The *output method* decides the overall form of the text. `sxml xquery`
uses the `xml` method unless the query or the command line says
otherwise. The same sequence, written by four methods:

```console
$ sxml xquery --context-item=vane.xml --expression='//t[@anchor = "req-7"]/bcp14, 7, 11, "req-7", <ok/>'
<bcp14>MUST</bcp14>7 11 req-7<ok/>
$ sxml xquery --context-item=vane.xml --method=text --expression='//t[@anchor = "req-7"]/bcp14, 7, 11, "req-7", <ok/>'
MUST7 11 req-7
$ sxml xquery --context-item=vane.xml --method=adaptive --expression='//t[@anchor = "req-7"]/bcp14, 7, 11, "req-7", <ok/>'
<bcp14>MUST</bcp14>
7
11
"req-7"
<ok/>
$ sxml xquery --context-item=vane.xml --method=json --expression='[//t[@anchor = "req-7"]/bcp14, 7, 11, "req-7"]'
["<bcp14>MUST<\/bcp14>",7,11,"req-7"]
```

- **xml** writes nodes as XML, and atomic values as text, with a space
  between neighbouring atomic values.
- **text** writes only the text: the string values of nodes, and the
  atomic values. Tags are left out.
- **adaptive** writes each item on its own line, in a form that shows
  its type: strings in quotes, other atomic values as constructor calls.
  It's the method of `sxml xpath`, and the best for looking at results.
- **json** writes maps, arrays, strings, numbers and booleans as JSON.
- **html** and **xhtml** write HTML pages; **csv** writes rows of
  values.

## Output declarations

A query states how its result is written with *output declarations* in
the prolog. Each one sets one *serialization parameter*. The prefix
`output` is predeclared for them; every query file of this chapter so
far started with two:

```console
$ sxml xquery --context-item=vane.xml --expression='
    declare option output:method "text";
    declare option output:item-separator ", ";
    //t[@anchor][bcp14 = "MUST"]/@anchor ! string()'
req-1, req-2, req-4, req-6, req-7
```

`--method` on the command line replaces the method that the query
declares, but keeps its other parameters. That's handy for looking at
the result of a query that writes text or JSON:

```console
$ sxml xquery --method=xml --expression='declare option output:method "text"; <a>x</a>'
<a>x</a>
```

A parameter that doesn't exist, or a value it doesn't allow, is an
error:

```console
$ sxml xquery --expression='declare option output:indent "maybe"; 1'
expression:1:1: err:SEPM0016: a serialization parameter has a value it does not permit
$ sxml xquery --expression='declare option output:colour "blue"; 1'
expression:1:1: err:XQST0109: an output declaration names no serialization parameter
```

Flags such as `indent` take `yes` or `no` (also `true` and `false`, and
`1` and `0`).

## XML

The parameters you'll use most with the `xml` method are `indent`,
`omit-xml-declaration` and the doctype. Many xml2rfc drafts start with
a document type declaration that names a file of character entities,
and a query that writes a new draft can write it too:

```console
$ sxml xquery --expression='
    declare option output:indent "yes";
    declare option output:omit-xml-declaration "no";
    declare option output:doctype-system "rfc2629-xhtml.ent";
    <rfc version="3" docName="draft-example-vane-protocol-02"><front><title>The Vane Protocol</title></front></rfc>'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE rfc SYSTEM "rfc2629-xhtml.ent">
<rfc version="3" docName="draft-example-vane-protocol-02">
  <front>
    <title>The Vane Protocol</title>
  </front>
</rfc>
```

Indenting adds whitespace between elements. sxml leaves an element alone
when it has text of its own, but a paragraph that happens to consist of
elements only, here a keyword followed by a cross reference, would get
line breaks inside it, which change its text. `suppress-indentation`
names elements whose content stays as it is:

```console
$ sxml xquery --expression='
    declare option output:indent "yes";
    <section><t><bcp14>MUST</bcp14><xref target="term-flag">flags</xref></t></section>'
<section>
  <t>
    <bcp14>MUST</bcp14>
    <xref target="term-flag">flags</xref>
  </t>
</section>
$ sxml xquery --expression='
    declare option output:indent "yes";
    declare option output:suppress-indentation "t";
    <section><t><bcp14>MUST</bcp14><xref target="term-flag">flags</xref></t></section>'
<section>
  <t><bcp14>MUST</bcp14><xref target="term-flag">flags</xref></t>
</section>
```

`cdata-section-elements` names elements whose text is written in CDATA
sections, as xml2rfc authors write code, so that `<` and `&` need no
escaping:

```console
$ sxml xquery --context-item=vane.xml --expression='
    declare option output:cdata-section-elements "sourcecode";
    //sourcecode'
<sourcecode type="abnf" name="vane.abnf"><![CDATA[
message        = rollout-update / kill-update / status-request
rollout-update = "ROLLOUT" SP flag-key SP percentage CRLF
kill-update    = "KILL" SP flag-key CRLF
status-request = "STATUS" SP flag-key CRLF
flag-key       = 1*32(ALPHA / DIGIT / "-")
percentage     = 1*3DIGIT
]]></sourcecode>
```

The parser doesn't keep CDATA sections (they are only another way to
write text), so without the parameter the same element comes out as
plain text. For this ABNF that is harmless, as it has no `<` or `&`;
for code with comparisons in it, the CDATA section keeps it readable.

`encoding` chooses the character encoding, UTF-8 by default. A
character that the encoding can't hold is written as a character
reference, such as `&#x2713;`.

## XHTML and HTML

The XHTML page of the index, `index-page.xq` from [Building
XML](xquery-constructors.md), is written by the `xml` method unless it
says otherwise. The `xhtml` method writes XML too, by the rules HTML
browsers need: a `<!DOCTYPE html>` at the start and a `meta` element for
the encoding:

```console
$ sxml xquery --context-item=vane.xml --method=xhtml index-page.xq | head -5
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head>
    <meta charset="UTF-8"/>
    <title>Requirements of draft-example-vane-protocol-01</title>
```

The `html` method writes HTML5 by the rules of HTML: empty elements such
as `<br>` and `<meta>` without a slash, the doctype, and the `meta`
element:

```console
$ sxml xquery --context-item=vane.xml --expression='
    declare option output:method "html";
    declare option output:indent "yes";
    <html><head><title>Vane</title></head><body><p>{ count(//t[@anchor]) } requirements.<br/>{ count(//cref) } editorial notes.</p></body></html>'
<!DOCTYPE html>
<html>
  <head>
    <meta charset="UTF-8">
    <title>Vane</title>
  </head>
  <body>
    <p>11 requirements.<br>2 editorial notes.</p>
  </body>
</html>
```

## JSON

The `json` method writes one item: a map or an array, usually. Maps
become objects, arrays become arrays, the empty sequence becomes `null`,
and numbers and booleans are what you'd expect. The entries of
`spec.xqm` are records, which are maps, so the index as JSON is one
array of them (here the first two, to keep it short):

```console
$ sxml xquery --context-item=vane.xml --expression='
    import module namespace spec = "urn:example:spec" at "spec.xqm";
    declare option output:method "json";
    declare option output:indent "yes";
    array { spec:requirements(/)[position() le 2] ! spec:entry(.) }'
[
  {
    "anchor": "req-1",
    "number": 1,
    "section": "2",
    "keyword": "MUST",
    "text": "A flag MUST be in exactly one of the states \"stable\", \"rolling-out\" and \"paused\"."
  },
  {
    "anchor": "req-2",
    "number": 2,
    "section": "2",
    "keyword": "MUST",
    "text": "A flag MUST start in the state \"stable\" when it is created."
  }
]
```

Note `array { ... }`: a sequence of records in a map or at the top would
be a sequence, not one value, and JSON has no sequences. A sequence of
more than one item at the top is an error, unless you ask for *JSON
lines*, one JSON value on each line, which many tools read one line at
a time:

```console
$ sxml xquery --method=json --expression='{ "a": 1 }, { "b": 2 }'
expression: err:SERE0023: the JSON method cannot write a sequence of more than one item
$ sxml xquery --context-item=vane.xml --expression='
    import module namespace spec = "urn:example:spec" at "spec.xqm";
    declare option output:method "json";
    declare option output:json-lines "yes";
    spec:requirements(/) ! spec:entry(.) ! map:remove(., "text")'
{"anchor":"req-1","number":1,"section":"2","keyword":"MUST"}
{"anchor":"req-2","number":2,"section":"2","keyword":"MUST"}
{"anchor":"req-3","number":3,"section":"2","keyword":"SHOULD"}
{"anchor":"req-4","number":4,"section":"3.1","keyword":"MUST"}
{"anchor":"req-5","number":5,"section":"3.1","keyword":"MUST NOT"}
{"anchor":"req-9","number":6,"section":"3.1","keyword":"MAY"}
{"anchor":"req-6","number":7,"section":"3.2","keyword":"MUST"}
{"anchor":"req-10","number":8,"section":"3.2","keyword":"SHOULD NOT"}
{"anchor":"req-7","number":9,"section":"4","keyword":"MUST"}
{"anchor":"req-8","number":10,"section":"4","keyword":"SHOULD"}
{"anchor":"req-11","number":11,"section":"4","keyword":"MAY"}
```

A node in a JSON result is written as a string of its XML. By default,
the `/` of end tags is escaped as `\/`, which JSON allows; set
`escape-solidus` to `no` to keep it plain, or `json-node-output-method`
to `text` to write only the text of the node:

```console
$ sxml xquery --context-item=vane.xml --expression='
    declare option output:method "json";
    declare option output:escape-solidus "no";
    { "keyword": //t[@anchor = "req-7"]/bcp14 }'
{"keyword":"<bcp14>MUST</bcp14>"}
$ sxml xquery --context-item=vane.xml --expression='
    declare option output:method "json";
    declare option output:json-node-output-method "text";
    { "keyword": //t[@anchor = "req-7"]/bcp14 }'
{"keyword":"MUST"}
```

## CSV

The `csv` method writes each array of a sequence as a row. Values with a
comma or a quote in them are quoted, and a quote inside is doubled. The
index for a spreadsheet, with a header row:

```console
$ sxml xquery --context-item=vane.xml --expression='
    import module namespace spec = "urn:example:spec" at "spec.xqm";
    declare option output:method "csv";
    ["anchor", "section", "keyword", "text"],
    spec:requirements(/) ! spec:entry(.) ! [?anchor, ?section, ?keyword, ?text]'
anchor,section,keyword,text
req-1,2,MUST,"A flag MUST be in exactly one of the states ""stable"", ""rolling-out"" and ""paused""."
req-2,2,MUST,"A flag MUST start in the state ""stable"" when it is created."
req-3,2,SHOULD,"A rolling-out flag SHOULD return to the state ""stable"" ten minutes after its rollout step began."
req-4,3.1,MUST,A service MUST check the guardrail metrics of a flag before it applies the percentage of a rollout request.
req-5,3.1,MUST NOT,A service MUST NOT roll out a flag while it is in a pause (Section 4).
req-9,3.1,MAY,A service MAY ask an owner for a time-based one-time password [RFC6238] before a rollout.
req-6,3.2,MUST,A service MUST answer a status request with the state of the flag and the number of failed checks since its last rollout step.
req-10,3.2,SHOULD NOT,An SDK SHOULD NOT send more than one status request per second for a flag; it uses its cached answer in between.
req-7,4,MUST,"After three failed checks in a row, a service MUST put the flag in a pause."
req-8,4,SHOULD,A pause SHOULD last five minutes.
req-11,4,MAY,A service MAY report a pause to the owners of the flag as soon as it begins.
```

`?anchor` and the others are lookups on the context item, the entry, so
each row takes four fields of one record. `csv-separator` chooses
another separator, such as `";"`.

## Serializing inside a query

`serialize` turns a value into a string inside the query, with the
parameters in a map. That's how a query writes XML into a string, or
JSON into an XML attribute:

```console
$ sxml xquery --context-item=vane.xml --method=text --expression='
    serialize(//t[@anchor = "req-7"]/bcp14),
    serialize({ "anchor": "req-7", "keywords": array { //t[@anchor = "req-7"]/bcp14/string() } }, { "method": "json" })'
<bcp14>MUST</bcp14> {"anchor":"req-7","keywords":["MUST"]}
$ sxml xquery --context-item=vane.xml --expression='<requirement keywords="{ serialize(array { //t[@anchor = "req-7"]/bcp14/string() }, { "method": "json" }) }"/>'
<requirement keywords="[&quot;MUST&quot;]"/>
```

`parse-xml` and `parse-json` go the other way.

## Saving the result

`--output=FILE` writes the result to a file instead of standard output.
The file is written only when the query succeeds, and a file that exists
is kept unless you add `--force`:

```console
$ sxml xquery --context-item=vane.xml --output=requirements.xml index.xq
$ grep -c '<requirement ' requirements.xml
11
$ sxml xquery --context-item=vane-00.xml --output=requirements.xml index.xq
sxml xquery: 'requirements.xml' exists; give --force to replace it
$ sxml xquery --context-item=vane-00.xml --output=requirements.xml --force index.xq
$ grep -c '<requirement ' requirements.xml
8
```

A query that writes several files uses `fn:put`, in the next section,
or XSLT's `xsl:result-document`, in [Many Outputs, Functions and
Packages](xslt-packages.md).
