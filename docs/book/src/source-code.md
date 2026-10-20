# Source Code as XML

<!-- examples: source-code -->
<!-- status: draft -->

Program source code has a grammar too, and for many languages the
grammar is already written: [tree-sitter](https://tree-sitter.github.io)
grammars power the syntax highlighting and code navigation of many
editors. sxml includes the tree-sitter grammars of JSON, JavaScript (with
JSX), Python, C, CSS, HTML and Go, and `sxml:parse-code` turns code in any
of them into an XML document. Then XPath, XQuery and XSLT can answer
questions about the code: where a function is called, which functions a
module exports, which comments say TODO. The examples are in
`docs/book/examples/source-code`.

## Parsing code

`greet.js`:

```javascript
{{#include ../examples/source-code/greet.js}}
```

`sxml:parse-code` takes the code as a string and the name of its
language, and returns a document:

```console
$ sxml xpath --method=xml 'sxml:parse-code(unparsed-text("greet.js"), "javascript")'
<program xmlns:sxml="urn:sxml"><comment highlight="comment">// Says hello.</comment>
<function_declaration><sxml:token highlight="keyword">function</sxml:token> <identifier field="name" highlight="function">greet</identifier><formal_parameters field="parameters">(<identifier highlight="variable.parameter">name</identifier>)</formal_parameters> <statement_block field="body">{
  <return_statement><sxml:token highlight="keyword">return</sxml:token> <binary_expression><string field="left" highlight="string">"<string_fragment>Hello, </string_fragment>"</string> <sxml:token field="operator" highlight="operator">+</sxml:token> <identifier field="right" highlight="variable">name</identifier></binary_expression>;</return_statement>
}</statement_block></function_declaration>
</program>
```

The document follows the grammar's syntax tree:

- Each named node of the tree is an element whose name is the node's
  kind: `function_declaration`, `identifier`, `return_statement`. The
  names are the grammar's own; tree-sitter's playground and each
  grammar's `node-types.json` list them.
- When the grammar gives a node a field name, as in `name`, `parameters`
  and `body`, the element has a `field` attribute.
- Punctuation and keywords are text, and so are the spaces and line
  breaks between nodes, so the string value of the document is the
  source code, exactly:

```console
$ sxml xpath 'string(sxml:parse-code(unparsed-text("greet.js"), "javascript")) = unparsed-text("greet.js")'
true()
```

- The `highlight` attribute is what the language's highlighting query
  (the `queries/highlights.scm` of its grammar) says about the node:
  `keyword`, `function`, `string`, `comment`, `variable.parameter` and so
  on. A token that has a highlight or a field becomes an `sxml:token`
  element, so that it can carry the attribute.

The prefix `sxml` is bound to the namespace `urn:sxml` in the `sxml`
command's XPath and XQuery; a stylesheet declares it. The elements of
the tree itself are in no namespace.

The third argument takes options:

| Option | Default | Meaning |
| --- | --- | --- |
| `highlights` | `true()` | Run the language's highlighting query and write `highlight` attributes |
| `positions` | `false()` | Write `line` and `column` attributes (where each element starts, from 1, columns counted in characters) and `end-line` and `end-column` (where it ends) |

`sxml:code-languages()` lists the languages:

```console
$ sxml xpath 'sxml:code-languages()'
"c"
"css"
"go"
"html"
"javascript"
"json"
"python"
```

## Which functions does a module export?

`cart.js`:

```javascript
{{#include ../examples/source-code/cart.js}}
```

An exported function is a `function_declaration`, or a `const` with an
arrow function, inside an `export_statement`. The name is the child
whose field is `name`:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("cart.js"), "javascript")//export_statement//*[@field = "name"][1] ! string()'
"total"
"totalWithTax"
"summary"
```

The same with the functions that are not exported, and how many
parameters each has:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("cart.js"), "javascript")/program/function_declaration ! `{identifier} takes {count(formal_parameters/*)}`'
"describe takes 1"
```

## TODO comments and where they are

With the option `positions`, each element knows its line. A comment
knows the function it is in through the `ancestor` axis:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("cart.js"), "javascript", { "positions": true() })//comment[contains(., "TODO")] ! `line {@line}, in {ancestor::function_declaration/identifier}: {substring-after(., "TODO: ")}`'
"line 6, in total: apply discounts"
"line 15, in describe: localize the currency"
```

Python works the same way. `report.py`:

```python
{{#include ../examples/source-code/report.py}}
```

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("report.py"), "python", { "positions": true() })//comment ! `{@line}: {ancestor::function_definition/identifier[@field = "name"]}: {.}`'
"11: total: # TODO: skip rows without an amount"
```

A function whose body does not start with a string has no docstring:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("report.py"), "python")//function_definition[not(block/*[1]/string)]/identifier[@field = "name"] ! string()'
"total"
"__init__"
"render"
```

## Checking a style rule

sxml's own C code has two rules that a query can check: a function has
at most three parameters, and names are spelled out. `buffer.c`:

```c
{{#include ../examples/source-code/buffer.c}}
```

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("buffer.c"), "c", { "positions": true() })//function_declarator[count(parameter_list/parameter_declaration) > 3] ! `{@line}: {*[@field = "declarator"]} has {count(parameter_list/parameter_declaration)} parameters`'
"5: buffer_append has 4 parameters"
$ sxml xpath 'sxml:parse-code(unparsed-text("buffer.c"), "c", { "positions": true() })//parameter_declaration//identifier[. = ("buf", "len", "ptr", "tmp")] ! `{@line}:{@column}: "{.}" is not spelled out`'
"14:58: "len" is not spelled out"
"19:25: "buf" is not spelled out"
```

## Counting JSX elements

`Gallery.jsx`:

```javascript
{{#include ../examples/source-code/Gallery.jsx}}
```

JSX is part of the JavaScript grammar. An element with content is a
`jsx_element`, one that closes itself a `jsx_self_closing_element`:

```console
$ sxml xpath 'count(sxml:parse-code(unparsed-text("Gallery.jsx"), "javascript")//(jsx_element | jsx_self_closing_element))'
4
```

The highlighting query tells HTML tags from components: it marks a
lowercase name as `tag` and a capitalized one as `constructor`. So the
components the file uses are:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("Gallery.jsx"), "javascript")//(jsx_opening_element | jsx_self_closing_element)/identifier[@highlight = "constructor"] => distinct-values()'
"Photo"
"Footer"
```

## A refactoring report

Before renaming a function, find every call of it. `money.js`:

```javascript
{{#include ../examples/source-code/money.js}}
```

`calls.xq`:

```xquery
{{#include ../examples/source-code/calls.xq}}
```

```console
$ sxml xquery calls.xq
cart.js:16: formatPrice(item.price)
money.js:6: formatPrice(amounts.reduce((sum, amount) => sum + amount, 0))
```

## Highlighted HTML with XSLT

Because the highlighting query has already named each part of the code,
turning code into highlighted HTML takes one template rule: every
element with a `highlight` attribute becomes a `span` whose class is
the highlight, and the built-in rules copy the text of everything else.

`highlight.xsl`:

```xml
{{#include ../examples/source-code/highlight.xsl}}
```

```console
$ sxml transform --param=file=greet.js highlight.xsl
<!DOCTYPE html><pre class="code"><span class="comment">// Says hello.</span>
<span class="keyword">function</span> <span class="function">greet</span>(<span class="variable parameter">name</span>) {
  <span class="keyword">return</span> <span class="string">"Hello, "</span> <span class="operator">+</span> <span class="variable">name</span>;
}
</pre>
```

A style sheet with rules such as `.keyword { color: purple }` and
`.string { color: green }` does the rest. The `language` parameter
selects another grammar: `--param=language=python`.

## Code with syntax errors

tree-sitter does not stop at a syntax error. It parses what it can and
marks the rest: an `ERROR` element holds text it could not place, and an
element with `missing="true"` stands for a token it expected but did
not find (an `sxml:token` with the token's `kind` when the token is
punctuation). The document is still the source text:

```console
$ sxml xpath --method=xml 'sxml:parse-code("[1, 2", "json")'
<document xmlns:sxml="urn:sxml"><array>[<number highlight="number">1</number>, <number highlight="number">2</number><sxml:token missing="true" kind="]"/></array></document>
```

So a query can report problems instead of failing:

```console
$ sxml xpath 'sxml:parse-code("{ \"a\": 1 \"b\": 2 }", "json")//(ERROR | *[@missing]) => count()'
1
```

A language sxml does not have is an error:

```console
$ sxml xpath 'sxml:parse-code("IDENTIFICATION DIVISION.", "cobol")'
expression:1:1: sxml:unknown-language: the language is not one that sxml:parse-code knows; sxml:code-languages() lists them
```
