# Writing Results

<!-- examples: xquery -->

A query computes a value: a sequence of nodes, atomic values, maps and
arrays. To see it, or to save it, the value must be turned into text.
That step is called *serialization*, and *XSLT and XQuery Serialization
4.0* defines how it works. This section shows the choices you have.

## Output methods

The *output method* decides the overall form of the text. `sxml xquery`
uses the `xml` method unless the query or the command line says
otherwise. The same sequence, written by four methods:

```console
$ sxml xquery --expression='<p>Gödel</p>, 1, 2, "a", <q/>'
<p>Gödel</p>1 2 a<q/>
$ sxml xquery --method=text --expression='<p>Gödel</p>, 1, 2, "a", <q/>'
Gödel1 2 a
$ sxml xquery --method=adaptive --expression='<p>Gödel</p>, 1, 2, "a", <q/>'
<p>Gödel</p>
1
2
"a"
<q/>
$ sxml xquery --method=json --expression='[<p>Gödel</p>, 1, 2, "a"]'
["<p>Gödel<\/p>",1,2,"a"]
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
`output` is predeclared for them:

```console
$ sxml xquery --method=text --expression='
    declare option output:method "text";
    declare option output:item-separator ", ";
    1 to 3'
1, 2, 3
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
`omit-xml-declaration` and the doctype:

```console
$ sxml xquery --expression='
    declare option output:indent "yes";
    declare option output:omit-xml-declaration "no";
    declare option output:doctype-system "notes.dtd";
    <notes><note>Bring the books back.</note></notes>'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE notes SYSTEM "notes.dtd">
<notes>
  <note>Bring the books back.</note>
</notes>
```

Indenting adds whitespace between elements, which is fine for data but
can change mixed content, such as a paragraph with some bold words in
it. `suppress-indentation` names elements whose content stays as it
is, and `cdata-section-elements` names elements whose text is written
in CDATA sections:

```console
$ sxml xquery --expression='
    declare option output:indent "yes";
    declare option output:suppress-indentation "p";
    declare option output:cdata-section-elements "code";
    <div><p>Use <code>a &lt; b</code> to compare.</p><ul><li>One</li></ul></div>'
<div>
  <p>Use <code><![CDATA[a < b]]></code> to compare.</p>
  <ul>
    <li>One</li>
  </ul>
</div>
```

`encoding` chooses the character encoding, UTF-8 by default. A
character that the encoding can't hold is written as a character
reference, such as `&#x2713;`.

## HTML

The `html` method writes HTML5 by the rules of HTML: empty elements such
as `<br>` without a slash, a `<!DOCTYPE html>` at the start, and a
`meta` element for the encoding in the `head`:

```console
$ sxml xquery --expression='
    declare option output:method "html";
    declare option output:indent "yes";
    <html><head><title>Loans</title></head><body><p>Six loans.<br/>Two overdue.</p></body></html>'
<!DOCTYPE html>
<html>
  <head>
    <meta charset="UTF-8">
    <title>Loans</title>
  </head>
  <body>
    <p>Six loans.<br>Two overdue.</p>
  </body>
</html>
```

The `xhtml` method writes the same as XML, for elements in the XHTML
namespace.

## JSON

The `json` method writes one item: a map or an array, usually. Maps
become objects, arrays become arrays, the empty sequence becomes `null`,
and numbers and booleans are what you'd expect:

```console
$ sxml xquery --context-item=library.xml --expression='
    declare option output:method "json";
    declare option output:indent "yes";
    {
      "books": array { //book ! string(@id) },
      "count": count(//book),
      "cheapest": (),
      "complete": true()
    }'
{
  "books": [
    "geb",
    "taocp",
    "sicp",
    "dune",
    "lhod"
  ],
  "count": 5,
  "cheapest": null,
  "complete": true
}
```

Note `array { ... }`: a sequence of strings in a map would be a
sequence, not one value, and JSON has no sequences. A sequence of more
than one item at the top is an error, unless you ask for *JSON lines*,
one JSON value on each line:

```console
$ sxml xquery --method=json --expression='{ "a": 1 }, { "b": 2 }'
expression: err:SERE0023: the JSON method cannot write a sequence of more than one item
$ sxml xquery --expression='
    declare option output:method "json";
    declare option output:json-lines "yes";
    { "a": 1 }, { "b": 2 }'
{"a":1}
{"b":2}
```

A node in a JSON result is written as a string of its XML. By default,
the `/` of end tags is escaped as `\/`, which JSON allows; set
`escape-solidus` to `no` to keep it plain, or `json-node-output-method`
to `text` to write only the text of the node:

```console
$ sxml xquery --context-item=library.xml --expression='
    declare option output:method "json";
    declare option output:escape-solidus "no";
    { "title": (//book)[1]/title }'
{"title":"<title>Godel, Escher, Bach</title>"}
$ sxml xquery --context-item=library.xml --expression='
    declare option output:method "json";
    declare option output:json-node-output-method "text";
    { "title": (//book)[1]/title }'
{"title":"Godel, Escher, Bach"}
```

## CSV

The `csv` method writes each array of a sequence as a row. Values with a
comma or a quote in them are quoted. `csv-separator` chooses another
separator:

```console
$ sxml xquery --context-item=library.xml --expression='
    declare option output:method "csv";
    declare option output:csv-separator ";";
    ["id", "title"], //book ! [string(@id), string(title)]'
id;title
geb;Godel, Escher, Bach
taocp;The Art of Computer Programming
sicp;Structure and Interpretation of Computer Programs
dune;Dune
lhod;The Left Hand of Darkness
```

## Serializing inside a query

`serialize` turns a value into a string inside the query, with the
parameters in a map. That's how a query writes JSON into an XML
attribute, or XML into a JSON string:

```console
$ sxml xquery --context-item=library.xml --method=text --expression='
    serialize((//book)[1]/title),
    serialize({ "id": "geb", "tags": array { (//book)[1]//tag/string() } }, { "method": "json" })'
<title>Godel, Escher, Bach</title> {"id":"geb","tags":["logic","music","art"]}
```

`parse-xml` and `parse-json` go the other way.

## Saving the result

`--output=FILE` writes the result to a file instead of standard output.
The file is written only when the query succeeds, and a file that exists
is kept unless you add `--force`:

```console
$ sxml xquery --context-item=library.xml --output=count.xml --expression='<count>{ count(//book) }</count>'
$ cat count.xml
<count>5</count>
$ sxml xquery --context-item=library.xml --output=count.xml --expression='<count>0</count>'
sxml xquery: 'count.xml' exists; give --force to replace it
```

A query that writes several files uses `fn:put`, in the section on
updates, or XSLT's `xsl:result-document`, in Chapter 4.
