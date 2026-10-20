# Source Code as XML

<!-- examples: source-code -->

Program source code has a grammar too, and for many languages the
grammar is already written: [tree-sitter](https://tree-sitter.github.io)
grammars power the syntax highlighting and code navigation of many
editors. sxml includes the tree-sitter grammars of JSON, JavaScript (with
JSX), TypeScript, TSX, Python, C, CSS, HTML, Go and Markdown, and
`sxml:parse-code` turns code in any of them into an XML document. Other
tree-sitter grammars can be added when sxml runs (see
[Supplying your own language](#supplying-your-own-language)). Then XPath, XQuery and XSLT can answer
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
<function_declaration><sxml:token highlight="keyword">function</sxml:token> <identifier field="name" highlight="function">greet</identifier><formal_parameters field="parameters"><sxml:token highlight="punctuation.bracket">(</sxml:token><identifier highlight="variable.parameter">name</identifier><sxml:token highlight="punctuation.bracket">)</sxml:token></formal_parameters> <statement_block field="body"><sxml:token highlight="punctuation.bracket">{</sxml:token>
  <return_statement><sxml:token highlight="keyword">return</sxml:token> <binary_expression><string field="left" highlight="string">"<string_fragment>Hello, </string_fragment>"</string> <sxml:token field="operator" highlight="operator">+</sxml:token> <identifier field="right" highlight="variable.parameter">name</identifier></binary_expression><sxml:token highlight="punctuation.delimiter">;</sxml:token></return_statement>
<sxml:token highlight="punctuation.bracket">}</sxml:token></statement_block></function_declaration>
</program>
```

The document follows the grammar's syntax tree:

- Each named node of the tree is an element whose name is the node's
  kind: `function_declaration`, `identifier`, `return_statement`. The
  names are the grammar's own; tree-sitter's playground and each
  grammar's `node-types.json` list them.
- When the grammar gives a node a field name, as in `name`, `parameters`
  and `body`, the element has a `field` attribute.
- Tokens, the keywords and punctuation, are text, and so are the spaces
  and line breaks between nodes, so the string value of the document is
  the source code, exactly:

```console
$ sxml xpath 'string(sxml:parse-code(unparsed-text("greet.js"), "javascript")) = unparsed-text("greet.js")'
true()
```

- The `highlight` attribute is what the language's highlighting query
  (the `queries/highlights.scm` of its grammar) says about the node:
  `keyword`, `function`, `string`, `comment`, `variable.parameter` and so
  on. Where the grammar has a locals query (`queries/locals.scm`), a name
  that refers to a parameter or variable gets the highlight of its
  definition: the `name` in `return "Hello, " + name` is a
  `variable.parameter`, like the parameter it names. A token that has a
  highlight or a field becomes an `sxml:token` element, so that it can
  carry the attribute; with the option `{ "highlights": false() }` most
  tokens stay plain text.

The prefix `sxml` is bound to the namespace `urn:sxml` in the `sxml`
command's XPath and XQuery; a stylesheet declares it. The elements of
the tree itself are in no namespace.

The third argument takes options:

| Option | Default | Meaning |
| --- | --- | --- |
| `highlights` | `true()` | Run the language's highlighting query and write `highlight` attributes |
| `positions` | `false()` | Write `line` and `column` attributes (where each element starts, from 1, columns counted in characters) and `end-line` and `end-column` (where it ends) |
| `injections` | `true()` | Parse code inside code (a Markdown code block, an HTML `script` element) with its own language; see [Code inside code](#code-inside-code) |

`sxml:code-languages()` lists the languages:

```console
$ sxml xpath 'sxml:code-languages()'
"c"
"css"
"go"
"html"
"javascript"
"json"
"markdown"
"markdown_inline"
"python"
"tsx"
"typescript"
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
$ sxml xpath 'sxml:parse-code(unparsed-text("cart.js"), "javascript")/program/function_declaration ! `{identifier} takes {count(formal_parameters/identifier)}`'
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
$ sxml xpath 'sxml:parse-code(unparsed-text("report.py"), "python")//function_definition[not(block/*[1][self::string])]/identifier[@field = "name"] ! string()'
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
$ sxml xpath 'sxml:parse-code(unparsed-text("buffer.c"), "c", { "positions": true() })//parameter_declaration//identifier[. = ("buf", "len", "ptr", "tmp")] ! `{@line}:{@column}: {.} is not spelled out`'
"14:61: len is not spelled out"
"19:25: buf is not spelled out"
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

## TypeScript and TSX

`shapes.ts`:

```typescript
{{#include ../examples/source-code/shapes.ts}}
```

The TypeScript grammar has the nodes of JavaScript and the types on top
of them. The properties of each interface:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("shapes.ts"), "typescript")//interface_declaration ! `{type_identifier}: {string-join(.//property_signature/property_identifier, ", ")}`'
"Circle: kind, radius"
"Rectangle: kind, width, height"
```

The signature of each exported function, with the types written as in
the source:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("shapes.ts"), "typescript")//export_statement/function_declaration ! `{identifier}({string-join(formal_parameters/(required_parameter | optional_parameter), ", ")}){*[@field = "return_type"]}`'
"area(shape: Shape): number"
"describe(shape: Shape, digits?: number): string"
```

TSX is TypeScript with JSX, and has a grammar of its own, `tsx`.
`Profile.tsx`:

```tsx
{{#include ../examples/source-code/Profile.tsx}}
```

The components the file uses, and the properties that are optional:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("Profile.tsx"), "tsx")//(jsx_opening_element | jsx_self_closing_element)/identifier[@highlight = "constructor"] ! string()'
"Avatar"
"Markdown"
$ sxml xpath 'sxml:parse-code(unparsed-text("Profile.tsx"), "tsx")//property_signature[contains(., "?")]/property_identifier ! string()'
"avatar"
"bio"
```

## Markdown

Markdown has two grammars: `markdown` for the blocks (headings,
paragraphs, lists, code blocks) and `markdown_inline` for what is inside
a paragraph or a heading (emphasis, links, code spans). `notes.md`:

````markdown
{{#include ../examples/source-code/notes.md}}
````

The headings and their levels:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("notes.md"), "markdown")//atx_heading ! `{string-length(*[1])}: {normalize-space(inline)}`'
"1: Release notes"
"2: Fixed"
"2: Example"
```

`sxml:parse-code` parses the text of each block with the inline grammar
(see the next section), so the links are there too:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("notes.md"), "markdown")//inline_link ! `{link_text}: {link_destination}`'
"issue 12: https://example.com/issues/12"
"the guide: guide.md"
```

## Code inside code

A Markdown code block holds code in another language, and so does an
HTML `script` element. The grammar's injections query
(`queries/injections.scm`) says which nodes hold code and in which
language: the info string after the fence, `javascript` for a `script`
element, the inline grammar for Markdown's paragraphs. `sxml:parse-code`
parses each such node with that language, when it knows the language,
and puts the tree inside the node's element. The tree's document element
names its language in a `language` attribute:

```console
$ sxml xpath --method=xml 'sxml:parse-code("Some *text*.", "markdown")'
<document xmlns:sxml="urn:sxml"><section><paragraph><inline><inline language="markdown_inline">Some <emphasis highlight="text.emphasis"><emphasis_delimiter highlight="punctuation.delimiter">*</emphasis_delimiter>text<emphasis_delimiter highlight="punctuation.delimiter">*</emphasis_delimiter></emphasis>.</inline></inline></paragraph></section></document>
```

So the Python function in the release notes is a `function_definition`
like any other:

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("notes.md"), "markdown")//*[@language = "python"]//function_definition/identifier ! string()'
"greet"
$ sxml xpath 'sxml:parse-code(unparsed-text("notes.md"), "markdown")//fenced_code_block ! `{info_string}: {code_fence_content/*[@language]/name()}`'
"python: module"
"javascript: program"
```

and the script and style sheet of an HTML page are JavaScript and CSS,
highlighted as such. `page.html`:

```html
{{#include ../examples/source-code/page.html}}
```

```console
$ sxml xpath 'sxml:parse-code(unparsed-text("page.html"), "html")//*[@language] ! `{@language}: {count(.//*[@highlight])} highlighted`'
"css: 7 highlighted"
"javascript: 33 highlighted"
$ sxml xpath 'sxml:parse-code(unparsed-text("page.html"), "html")//*[@language = "javascript"]//*[@highlight = "function.method"] ! string()'
"getElementById"
"reduce"
```

The text is still the source text: the string value of the document is
the page, and the option `{ "injections": false() }` leaves the code
inside code as text.

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
<pre class="code"><span class="comment">// Says hello.</span>
<span class="keyword">function</span> <span class="function">greet</span><span class="punctuation bracket">(</span><span class="variable parameter">name</span><span class="punctuation bracket">)</span> <span class="punctuation bracket">{</span>
  <span class="keyword">return</span> <span class="string">"Hello, "</span> <span class="operator">+</span> <span class="variable parameter">name</span><span class="punctuation delimiter">;</span>
<span class="punctuation bracket">}</span>
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
$ cat broken.json
{ "a": 1 "b": 2 }
$ sxml xpath 'sxml:parse-code(unparsed-text("broken.json"), "json")//(ERROR | *[@missing]) => count()'
1
```

A language sxml does not have is an error:

```console
$ sxml xpath 'sxml:parse-code("IDENTIFICATION DIVISION.", "cobol")'
expression:1:1: sxml:unknown-language: the language is not one that sxml:parse-code knows; sxml:code-languages() lists them
```

## Supplying your own language

sxml includes a dozen grammars, and tree-sitter has hundreds more. A
grammar that sxml does not include can be added when sxml runs: tree-sitter
compiles each grammar to C, and the C compiles to a shared library
(`libtree-sitter-rust.so` on Linux, `.dylib` on macOS, `.dll` on Windows)
that exports one function, `tree_sitter_rust()`. The grammar's
repository also has its queries, `queries/highlights.scm` and often
`locals.scm` and `injections.scm`. With the
[tree-sitter command](https://tree-sitter.github.io/tree-sitter/cli/build.html):

```sh
git clone https://github.com/tree-sitter/tree-sitter-rust
cd tree-sitter-rust
tree-sitter build        # writes rust.so (rust.dylib, rust.dll)
```

The `sxml` command's option `--code-language=LIBRARY` loads such a
library for `sxml xpath`, `sxml xquery` and `sxml transform`. It may be
given more than once:

```sh
sxml xpath --code-language=tree-sitter-rust/rust.so \
  'sxml:parse-code(unparsed-text("main.rs"), "rust")//function_item/identifier ! string()'
```

- The language's name comes from the file name: the extension and the
  prefixes `lib` and `tree-sitter-` are dropped, and `-` becomes `_`. So
  `libtree-sitter-c-sharp.so` is `c_sharp`, and the library's function
  must be `tree_sitter_c_sharp()`.
- The queries come from the directory `queries` beside the library, as
  in a grammar's repository after `tree-sitter build`, or else from
  `../queries/NAME` (the layout of Neovim's parsers and queries).
  Without queries, the language parses but has no highlights.
- A language with the name of one that sxml includes replaces it, so a
  newer version of a grammar can be used.
- The grammar must be generated for tree-sitter's language ABI 13 to 15
  (tree-sitter 0.20 and later); sxml says so when it is not, and when a
  query does not fit the grammar.

To try it without a network, build the JSON grammar from sxml's own
sources, and give it queries of your own. `queries/highlights.scm`:

```scheme
{{#include ../examples/source-code/queries/highlights.scm}}
```

```console
$ cc -shared -fPIC -O1 -o json.so "$SXML/third_party/tree-sitter-grammars/json/src/parser.c"
$ sxml xpath --code-language=json.so --method=xml 'sxml:parse-code("[1, true]", "json")'
<document xmlns:sxml="urn:sxml"><array>[<number highlight="number.json">1</number>, <true highlight="boolean">true</true>]</array></document>
```

A library that cannot be loaded is an error, and so are a grammar of
another ABI and a query that does not fit the grammar:

```console
$ sxml xpath --code-language=missing.so 'sxml:code-languages()'
sxml xpath: --code-language=missing.so: cannot load the library: ./missing.so: cannot open shared object file: No such file or directory
```

A program that uses sxml's C API registers a language the same way: it
has the grammar's `TSLanguage` (from a library it loaded, or one it links)
and the text of its queries, and lists them in the processor's
configuration, in `source_code_languages`.
`sxml_source_code_check_language` says what is wrong with a language
before the processor is made:

```c
#include "sxml/processor.h"

const struct TSLanguage *tree_sitter_rust(void);

/* rust_highlights: the text of queries/highlights.scm, read by the program. */
sxml_status make_processor(sxml_allocator allocator, sxml_byte_view rust_highlights,
                           sxml_processor **processor) {
    static sxml_source_code_language rust;
    rust = (sxml_source_code_language){
        .language_name = SXML_BYTE_VIEW_INITIALIZER("rust"),
        .grammar = tree_sitter_rust(),
        .highlights_query = rust_highlights,
        .locals_query = {.bytes = NULL, .byte_count = 0},
        .injections_query = {.bytes = NULL, .byte_count = 0},
        .injection_regex = SXML_BYTE_VIEW_INITIALIZER("^(rs|rust)$")};
    sxml_source_code_language_report report;
    if (sxml_source_code_check_language(&rust, &report) != SXML_STATUS_OK) {
        return SXML_STATUS_INVALID_ARGUMENT; /* report.problem says why */
    }
    const sxml_optional_feature_view features = sxml_all_optional_features();
    return sxml_processor_create(
        allocator,
        &(sxml_processor_configuration){
            .allocator = allocator,
            .optional_features = features,
            .source_code_languages = {.languages = &rust, .language_count = 1}},
        processor);
}
```

The languages, their grammars and the query texts must outlive the
processor. The JavaScript package cannot load languages yet: tree-sitter's
WebAssembly language files (`tree-sitter-rust.wasm`) need a module that
links them at run time, which `sxml.wasm` is not.
