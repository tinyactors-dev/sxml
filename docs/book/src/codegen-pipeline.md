# A Pipeline, a Second Target and a Changing Schema

<!-- examples: codegen -->

A generator is used many times: each time the API changes, and each
time someone wants to know whether the generated files are current. In
this last section, we'll make generation one command, add a second
output written in XSLT, and change the schema.

## One command

`generate.sh` makes every generated file, into the directory it is
given:

`generate.sh`:

```sh
{{#include ../examples/codegen/generate.sh}}
```

It validates the description first, so the generator never sees one
that is wrong, builds the tree of the program into `program.xml`,
prints the tree into the TypeScript files, and writes the OpenAPI
document of a later part of this section. It stops at the first
command that fails. `--force` lets `sxml` replace the files of an
earlier run. The generated files of the examples were made with it,
and are kept with the sources:

```console
$ sh generate.sh
$ ls generated
client.ts
openapi.json
program.xml
server.ts
types.ts
```

## The same output every time

Nothing in the generated files depends on when or where they were
made: there is no date in them, no absolute path, and every list is in
the order of its source or sorted. So the same sources always give the
same bytes, and a check that generated files are current is a
comparison. Generate into a new directory and compare it with the files
that are kept:

```console
$ sh generate.sh fresh
$ diff -r generated fresh && echo "up to date"
up to date
```

Put these two commands into the checks that run before a change is
merged, and nobody can change the schema and forget the code, or edit
generated code by hand: `diff` fails, and shows what differs.

## Checking the generated code

The client and the server are printed from the same tree, so they
should fit together: every function of the client has a method in the
server's `Implementation` and a route in `createHandler`, and every
name they import from `runtime.ts` is one that `runtime.ts` exports.
The [Source Code as XML](source-code.md) chapter's `sxml:parse-code`
can check that on the code itself, after it was printed, with
tree-sitter's TypeScript grammar. `check.xq` parses the three
TypeScript files and compares their parts, and also compares the
client with the tree it was printed from:

`check.xq`:

```xquery
{{#include ../examples/codegen/check.xq}}
```

The element names in it are tree-sitter's (`function_declaration`,
`method_signature`, `pair`), not the generator's: the check reads the
code as any TypeScript file would be read, and doesn't trust the
printer. On the files that are kept, everything fits:

```console
$ sxml xquery --method=text check.xq
3 functions: client, server and runtime agree
```

Someone who deletes a method from the server's interface by hand, in
a copy, finds out at once:

```console
$ cp -r generated edited
$ sed '/borrowBook(input: LoanRequest)/d' generated/server.ts > edited/server.ts
$ sxml xquery --method=text --param=directory=edited check.xq
check.xq:35:8: local:mismatch: client function borrowBook has no method in Implementation
```

The check also guards the generator and the printer: a generator that
forgot a route, or a printer that dropped a function, makes it fail in
the same way. Run it after `generate.sh`, next to the comparison above.

## A second target with XSLT

Other programs and tools understand OpenAPI, a JSON format that
describes HTTP APIs, with JSON Schema for the values. The same two
sources can give an OpenAPI document too. This time we'll write the
generator in XSLT, to show the other way of thinking about it: instead
of functions that call each other, a template rule for each kind of XSD
construct, and the processor finds the rule for each one:

`openapi.xsl`:

```xml
{{#include ../examples/codegen/openapi.xsl}}
```

The output is a map, which the JSON output method writes as JSON;
`escape-solidus="no"` keeps the slashes of the paths as they are. Each
`xsl:map-entry` makes an entry of the `xsl:map` around it, so the rules
for the facets add their keywords to the type they belong to: `pattern`
becomes a `pattern`, with the anchors that JSON Schema needs, and
`minInclusive` becomes `minimum`. The rule with the priority -1 matches
every other facet, and stops the transformation, as the XQuery
generator does.

`generate.sh` writes the document to `generated/openapi.json`. The
whole file is long; here are the paths, with their HTTP methods, and
the type of the days:

```console
$ sxml xquery --method=adaptive --expression='
    for key $path value $operation in json-doc("generated/openapi.json")?paths
    return $path || ": " || map:keys($operation)'
"/api/getBook: get"
"/api/searchBooks: get"
"/api/borrowBook: post"
$ sxml xpath --method=json 'json-doc("generated/openapi.json")?components?schemas?LoanDays'
{"type":"integer","minimum":1,"maximum":28}
```

Which language to use for which step is a matter of taste. XQuery
reads like a program, and suits the generator, which looks things up
in two documents and builds a new one; XSLT's template rules suit the
printer and this stylesheet, whose sources have many kinds of nodes and
whose output follows the input, and its `xsl:result-document` writes
many files from one transformation.

## When the schema changes

The library decides that a loan lasts at most 21 days, and that books
can have a rating from 1 to 5. `library-2.xsd` is the next version of
the schema:

```console
$ diff -U2 -L library.xsd -L library-2.xsd library.xsd library-2.xsd
--- library.xsd
+++ library-2.xsd
@@ -27,5 +27,12 @@
     <xs:restriction base="xs:integer">
       <xs:minInclusive value="1"/>
-      <xs:maxInclusive value="28"/>
+      <xs:maxInclusive value="21"/>
+    </xs:restriction>
+  </xs:simpleType>
+
+  <xs:simpleType name="Rating">
+    <xs:restriction base="xs:integer">
+      <xs:minInclusive value="1"/>
+      <xs:maxInclusive value="5"/>
     </xs:restriction>
   </xs:simpleType>
@@ -37,4 +44,5 @@
       <xs:element name="genre" type="Genre"/>
       <xs:element name="available" type="xs:boolean"/>
+      <xs:element name="rating" type="Rating" minOccurs="0"/>
     </xs:sequence>
   </xs:complexType>
```

Put it in place and generate again. The check of the last section now
fails, and names the files that are out of date:

```console
$ cp library-2.xsd library.xsd
$ sh generate.sh next
$ diff -rq generated next
Files generated/openapi.json and next/openapi.json differ
Files generated/program.xml and next/program.xml differ
Files generated/types.ts and next/types.ts differ
```

The client and the server didn't change: no function's types did. The
tree did, and so the types: the difference is exactly the change to
the schema, written out in TypeScript:

```console
$ diff -u -L generated/types.ts -L next/types.ts generated/types.ts next/types.ts
--- generated/types.ts
+++ next/types.ts
@@ -35,7 +35,17 @@
   if (!checkInteger(value, path, problems)) return false;
   const count = problems.length;
   if (value < 1) problems.push(path + ": must be at least 1");
-  if (value > 28) problems.push(path + ": must be at most 28");
+  if (value > 21) problems.push(path + ": must be at most 21");
+  return problems.length === count;
+}
+
+export type Rating = number;
+
+export function checkRating(value: unknown, path: string, problems: string[]): value is Rating {
+  if (!checkInteger(value, path, problems)) return false;
+  const count = problems.length;
+  if (value < 1) problems.push(path + ": must be at least 1");
+  if (value > 5) problems.push(path + ": must be at most 5");
   return problems.length === count;
 }

@@ -44,6 +54,7 @@
   title: string;
   genre: Genre;
   available: boolean;
+  rating?: Rating;
 }

 export function checkBook(value: unknown, path: string, problems: string[]): value is Book {
@@ -53,8 +64,9 @@
   checkString(value.title, path + ".title", problems);
   checkGenre(value.genre, path + ".genre", problems);
   checkBoolean(value.available, path + ".available", problems);
+  if (value.rating !== undefined) checkRating(value.rating, path + ".rating", problems);
   for (const key of Object.keys(value)) {
-    if (!["id", "title", "genre", "available"].includes(key)) problems.push(path + "." + key + ": is not allowed");
+    if (!["id", "title", "genre", "available", "rating"].includes(key)) problems.push(path + "." + key + ": is not allowed");
   }
   return problems.length === count;
 }
```

The new limit is now in the browser's checks, in the server's, and in
the OpenAPI document, from one edit:

```console
$ sxml xpath --method=json 'json-doc("next/openapi.json")?components?schemas?LoanDays'
{"type":"integer","minimum":1,"maximum":21}
```

A review of the change shows the schema and the generated code side by
side, so a reviewer sees what the schema change means for the code. The
rating is optional, so code written against the old types still
compiles. Make it required, and the TypeScript compiler points at every
place in the server's code that makes a book without one.

## What we built

From two small XML files, one validated by the other's schema, we made
three TypeScript files and an OpenAPI document, which agree with each
other because they come from the same place:

- an XSD schema whose simple types, facets and complex types became
  TypeScript types and runtime checks;
- an API description in a language of our own, with a schema of its
  own;
- an XQuery library module that builds a tree of the TypeScript
  program, in a small vocabulary of our own, and finds the imports by
  querying it;
- an XSLT printer with one template rule for each kind of element and
  the layout in one place, and a second style that imports it and
  changes two declarations;
- a check that parses the generated code with `sxml:parse-code` and
  compares the client, the server and the runtime;
- an XSLT stylesheet that writes JSON with template rules and maps;
- a script that makes everything, and a comparison that shows whether
  the kept files are current.

Each step is small, and each can be tried on its own: the generator's
functions return trees that a test can query, and the printer prints
any piece of a tree. The same approach works for other targets: a
vocabulary and a printer for a language on the server, SQL tables,
forms for a web page, or documentation. Whatever the target, the
schema stays the one place where a rule is written.
