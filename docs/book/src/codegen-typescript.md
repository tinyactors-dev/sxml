# Building a Tree of the Program with XQuery

<!-- examples: codegen -->

A code generator is a program that writes text, and the most direct way
to write text in XQuery is a *string constructor*. It starts with
``` ``[ ``` and ends with ``` ]`` ```. Everything between is text, kept
as it is, line breaks included, except for expressions written between
`` `{ `` and `` }` ``, whose values are put in:

```console
$ sxml xquery --method=text --expression='
    let $name := "FlagKey"
    return ``[export function check`{ $name }`(value: unknown): boolean { return true; }]``'
export function checkFlagKey(value: unknown): boolean { return true; }
```

Braces, quotes and backticks are ordinary text in a string constructor,
so TypeScript can be written in it as it looks. For a line or two of
text with a fixed shape, that is the simplest tool there is. For whole
files it gets hard to follow. Each piece of the generator then does three
jobs at once: it decides what the program says (a check for each facet),
how TypeScript writes it (keywords, punctuation, quotes and escapes),
and how it is laid out (indentation, line breaks, empty lines). Each
piece has to get all three right, and the indentation of a line depends
on pieces far away. What comes out is text, which can be read but not
asked anything.

So we'll split the work in two. The generator, in XQuery, builds a
*tree of the TypeScript program*: an XML document with an element for
each declaration, type, statement and expression. It decides what the
program says, and nothing else. A *printer*, an XSLT stylesheet that the
[next section](codegen-printer.md) shows, turns the tree into text, with
one template rule for each kind of element. Between the two steps, the
tree is a document like any other: we can look at it, query it and test
it.

## A vocabulary for TypeScript

The tree needs element names. The [Source Code as XML](source-code.md)
chapter has some already: `tree-sitter:parse` turns TypeScript into a
tree, with an element for each node of tree-sitter's grammar. Here is a
line of code from the generated checks:

```console
$ sxml xpath --method=xml 'tree-sitter:parse("const count = problems.length;", "typescript", { "highlights": false() })'
<program xmlns:sxml="urn:sxml"><lexical_declaration><sxml:token field="kind">const</sxml:token> <variable_declarator><identifier field="name">count</identifier> = <member_expression field="value"><identifier field="object">problems</identifier>.<property_identifier field="property">length</property_identifier></member_expression></variable_declarator>;</lexical_declaration></program>
```

That tree is *concrete*: every token and every space is in it, so its
string value is the code. That is what makes it good for reading code,
and it is why it is a poor thing to write: to write such a tree, you
write the text anyway. A generator needs an *abstract* tree, which has
only the parts that vary, and leaves the keywords, punctuation and
layout to the printer. So we'll use a small vocabulary of our own, in
no namespace, named after TypeScript's own words. The same line is

```xml
<const name="count"><member name="length"><name>problems</name></member></const>
```

and the whole vocabulary fits in a table:

| Elements | TypeScript |
| --- | --- |
| `module` (with `file`) | a file; its children are its declarations |
| `comment`, `doc` | `// text` and `/** text */` |
| `import` (with `from`, `type-only`) | `import type { A, B } from "./types.ts";` |
| `type-alias` | `export type State = "stable" \| "paused";` |
| `interface`, `property` (with `optional`), `method` | `export interface FlagStatus { flag: FlagKey; retryAfter?: Seconds; }` |
| `function` (with `export`, `async`), `parameter`, `returns`, `body` | `export async function getFlags(input: FlagsRequest): Promise<FlagSet> { ... }` |
| `type`, `array-type`, `union`, `predicate` | `Promise<FlagSet>`, `string[]`, `"a" \| "b"`, `value is FlagSet` |
| `statement`, `const`, `return`, `if` with `then` and `else`, `for-of` | statements |
| `name`, `string`, `number`, `boolean`, `regex` | `value`, `"text"`, `100`, `false`, `/^[a-z0-9-]*$/u` |
| `call` (with `function`, or `method` of its first child), `member`, `not`, `binary` (with `operator`), `await`, `array`, `object` with `entry`, `arrow` | expressions |

A name of something, such as a function, is an attribute; the parts of
it are children. A `string` holds the text of the string, not the
TypeScript literal: how quotes and escapes are written is the printer's
business. An operator has a name, such as `less`, not a symbol, so no
`&lt;` is needed in the generator.

## The hand-written part

Not everything should be generated. The code that is the same for every
API is written once, by hand, and the generated code calls it:

`runtime.ts`:

```ts
{{#include ../examples/codegen/runtime.ts}}
```

It has three groups of functions:

- *Checks.* A check looks at a value that came from JSON, so its type
  is `unknown`, and adds a message to `problems` for each way it is
  wrong. `path` says where the value is, such as `input.percentage`, so the
  messages can say it too. The checks of the built-in XSD types are
  here: an `xs:integer` must be a whole number, an `xs:date` a string
  such as `2026-10-20`. `characterCount` counts characters as XSD does:
  JavaScript's `length` counts UTF-16 code units, which differ for
  characters outside the Basic Multilingual Plane.
- *The client.* `call` sends a query as a GET request, with the input
  as JSON in the URL, and a command as a POST request, with the input
  as the body.
- *The server.* `answer` finds the function that a request names,
  checks the method, the input and then the output, and `serve` makes a
  request listener for Node.js's `http` module from it.

## The generator

The generator is a library module, with one function for each kind of
thing it builds. Each returns elements of the vocabulary:

`typescript.xqm`:

```xquery
{{#include ../examples/codegen/typescript.xqm}}
```

Let's read it from the top.

- `$ts:built-in` maps the built-in XSD types that the API uses to their
  TypeScript type and their check. `ts:type` makes a `type` element for
  a type name, and `ts:check` gives the name of its check; a name of the
  schema keeps its name. A built-in type that isn't in the map is an
  error, not a guess.
- `ts:push` and `ts:problem` build the statement that every rule ends
  in: `problems.push(...)`, with the message, inside an `if`.
- `ts:facet` turns one facet into one `if`. A `pattern` becomes a
  regular expression with `^(?:` and `)$` around it, because an XSD
  pattern must match the whole value (XSD 1.1 Part 2, appendix G).
  Length facets count characters. A facet the generator doesn't know is
  an error: a generator that left a rule out without saying so would
  make code that accepts what the schema refuses.
- `ts:check-function` builds the shape that every check shares: check
  the base type, remember the number of problems, run the rules, and
  return whether none was added. `ts:simple-type` gives it the facets of
  an `xs:simpleType`, after a type alias; with enumeration facets, the
  type is a union of string literals, so the TypeScript compiler knows
  the values too.
- `ts:property` and `ts:property-check` handle one element of a
  sequence: required, optional, or an array. `ts:complex-type` builds an
  interface and a check, which also refuses properties that the schema
  doesn't have.
- `ts:module` puts declarations into a `module`, with the imports they
  need, which it finds by asking the declarations: a type or a name
  that `types.ts` declares is imported from there, and a function that
  is called but declared nowhere in the program comes from
  `runtime.ts`. Nobody keeps a list of imports by hand.
- `ts:types` builds `types.ts`: every type of the schema, in the order
  of the schema. `ts:functions` returns the functions of the
  description, after it has checked that their types are complex types
  of the schema. `ts:client-names` lists the names in the `clients`
  attributes, once each and sorted, and `ts:client` builds the module
  of one of them, such as `browser.ts`, with the functions that name it.
  `ts:server` builds `server.ts`, with every function: whichever client
  sends a request, the service checks it.

The XSD types are read as XML, with paths such as
`xs:restriction/@base`. This is the schema as a document, not the
compiled schema that `sxml validate` uses: a generator needs to know
how the types are written, and paths are the simplest way to ask.

## Trying the pieces

As with any library module, try the functions on their own first. One
facet becomes one `if`, with the comparison and the message:

```console
$ sxml xquery --method=xml --expression='
    import module namespace ts = "https://example.org/typescript" at "typescript.xqm";
    declare namespace xs = "http://www.w3.org/2001/XMLSchema";
    declare option output:indent "yes";
    ts:facet(<xs:maxInclusive value="100"/>)'
<if>
  <binary operator="greater">
    <name>value</name>
    <number>100</number>
  </binary>
  <then>
    <statement>
      <call method="push">
        <name>problems</name>
        <binary operator="plus">
          <name>path</name>
          <string>: must be at most 100</string>
        </binary>
      </call>
    </statement>
  </then>
</if>
```

The tree is longer than the line of code it stands for, but each part
has a name, so a test can ask for a part instead of comparing text. The
operator and the number of a `maxLength` facet:

```console
$ sxml xquery --expression='
    import module namespace ts = "https://example.org/typescript" at "typescript.xqm";
    declare namespace xs = "http://www.w3.org/2001/XMLSchema";
    ts:facet(<xs:maxLength value="64"/>)/binary ! (@operator, number) ! string()'
greater 64
```

Given to the printer of the next section, which reads a document from
standard input when its input is `-`, the tree of a facet is one line
of TypeScript:

```console
$ sxml xquery --method=xml --expression='
    import module namespace ts = "https://example.org/typescript" at "typescript.xqm";
    declare namespace xs = "http://www.w3.org/2001/XMLSchema";
    ts:facet(<xs:pattern value="[a-z0-9][a-z0-9-]*"/>)' | sxml transform print.xsl -
if (!/^(?:[a-z0-9][a-z0-9-]*)$/u.test(value)) problems.push(path + ": must match [a-z0-9][a-z0-9-]*");
```

A simple type is a type alias and a check function. Put into a
`module`, the printer writes them as a file would have them:

```console
$ sxml xquery --method=xml --expression='
    import module namespace ts = "https://example.org/typescript" at "typescript.xqm";
    declare namespace xs = "http://www.w3.org/2001/XMLSchema";
    <module>{ ts:simple-type(doc("vane-api.xsd")//xs:simpleType[@name = "Percentage"]) }</module>' | sxml transform print.xsl -
export type Percentage = number;

export function checkPercentage(value: unknown, path: string, problems: string[]): value is Percentage {
  if (!checkInteger(value, path, problems)) return false;
  const count = problems.length;
  if (value < 0) problems.push(path + ": must be at least 0");
  if (value > 100) problems.push(path + ": must be at most 100");
  return problems.length === count;
}
```

The facets `minInclusive` and `maxInclusive` became the two `if`
statements. The checks are *type predicates*: when `checkInteger`
returns true, TypeScript knows that `value` is a number in the lines
after it. A complex type becomes an interface:

```console
$ sxml xquery --method=xml --expression='
    import module namespace ts = "https://example.org/typescript" at "typescript.xqm";
    declare namespace xs = "http://www.w3.org/2001/XMLSchema";
    <module>{ ts:complex-type(doc("vane-api.xsd")//xs:complexType[@name = "FlagStatus"]) }</module>' | sxml transform print.xsl -
export interface FlagStatus {
  flag: FlagKey;
  state: State;
  failedChecks: FailureCount;
  retryAfter?: Seconds;
}

export function checkFlagStatus(value: unknown, path: string, problems: string[]): value is FlagStatus {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkFlagKey(value.flag, path + ".flag", problems);
  checkState(value.state, path + ".state", problems);
  checkFailureCount(value.failedChecks, path + ".failedChecks", problems);
  if (value.retryAfter !== undefined) checkSeconds(value.retryAfter, path + ".retryAfter", problems);
  for (const key of Object.keys(value)) {
    if (!["flag", "state", "failedChecks", "retryAfter"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}
```

And a facet without a translation stops the generator:

```console
$ sxml xquery --expression='
    import module namespace ts = "https://example.org/typescript" at "typescript.xqm";
    ts:facet(<xs:totalDigits value="5"/>)'
typescript.xqm:73:20: ts:unsupported: The generator has no check for the facet totalDigits
```

## The tree of the whole program

A main module puts it together. It reads the description and its
schema, and returns a `program` with a `module` for each file:

`program.xq`:

```xquery
{{#include ../examples/codegen/program.xq}}
```

The output options in the prolog, as in [Writing
Results](xquery-serialization.md), write the tree as indented XML, so
it is easy to read and to compare. The modules are built in the order
of the files that use each other: `types.ts` first, because
`ts:module` looks into it to find the imports of the others. The
clients come next, one for each name, and the server last.

```console
$ sxml xquery --output=program.xml program.xq
$ sxml xpath --context-item=program.xml '/program/module ! `{ @file }: { count(descendant::*) } elements`'
"types.ts: 1427 elements"
"browser.ts: 62 elements"
"console.ts: 62 elements"
"node.ts: 62 elements"
"server.ts: 138 elements"
```

The tree is a document, so everything in this book about querying
documents applies to the program. Every exported function, by file:

```console
$ sxml xpath --context-item=program.xml '/program/module ! `{ @file }: { string-join(function[@export]/@name, ", ") }`'
"types.ts: checkFlagKey, checkOwner, checkEnvironment, checkState, checkPercentage, checkFailureCount, checkSeconds, checkRule, checkFlagConfig, checkFlagsRequest, checkFlagSet, checkAttribute, checkEvaluationContext, checkEvaluation, checkEvaluations, checkStatusRequest, checkFlagStatus, checkRolloutRequest, checkKillRequest, checkKillResult"
"browser.ts: evaluateFlags, getStatus"
"console.ts: rollOutFlag, killFlag"
"node.ts: getFlags, getStatus"
"server.ts: createHandler"
```

The checks that use `checkFlagKey`, which are the ones a change to the
type of a flag key reaches:

```console
$ sxml xpath --context-item=program.xml '//function[.//(call/@function | name) = "checkFlagKey"]/@name ! string()'
"checkFlagConfig"
"checkEvaluation"
"checkStatusRequest"
"checkFlagStatus"
"checkRolloutRequest"
"checkKillRequest"
"checkKillResult"
```

And the imports of the frontend SDK's client, which `ts:module` found in the tree:

```console
$ sxml xpath --context-item=program.xml '/program/module[@file = "browser.ts"]/import ! `{ @from }: { string-join(name, ", ") }`'
"../runtime.ts: call, expect"
"./types.ts: EvaluationContext, Evaluations, StatusRequest, FlagStatus"
"./types.ts: checkEvaluationContext, checkEvaluations, checkStatusRequest, checkFlagStatus"
```

None of these needs to know how TypeScript writes a function or an
import; asked of the text, each would need a parser.

## The rule the schema couldn't check

`ts:functions` checks that each function's types exist. With a typo in
a type name, the generator stops with its own error, and writes no
file, because `--output` writes its file only when the whole query
succeeds:

```console
$ sed 's/output="KillResult"/output="KillResults"/' api.xml > typo.xml
$ sxml xquery --param=api-file=typo.xml --output=typo-program.xml program.xq
typescript.xqm:248:10: ts:unknown-type: The function killFlag uses the type KillResults, which the schema doesn't declare
$ test -e typo-program.xml || echo "no file written"
no file written
```

Next, let's print the tree.
