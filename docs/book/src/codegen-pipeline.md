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
browser.ts
console.ts
node.ts
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

The clients and the server are printed from the same tree, so they
should fit together: every client module has the functions that the
tree gives it, every function of a client has a method in the server's
`Implementation`, every method is in some client and has a route in
`createHandler`, and every name the modules import from `runtime.ts` is
one that `runtime.ts` exports.
The [Source Code as XML](source-code.md) chapter's `sxml:parse-code`
can check that on the code itself, after it was printed, with
tree-sitter's TypeScript grammar. `check.xq` reads the names of the
modules from the tree, parses each printed file once into a map from
file name to syntax tree, and compares their parts, and the clients
with the tree they were printed from:

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
5 functions in 3 clients: clients, server and runtime agree
```

Someone who deletes a method from the server's interface by hand, in
a copy, finds out at once:

```console
$ cp -r generated edited
$ sed '/killFlag(input: KillRequest)/d' generated/server.ts > edited/server.ts
$ sxml xquery --method=text --param=directory=edited check.xq
check.xq:42:8: local:mismatch: console.ts: client function killFlag has no method in Implementation
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
generator does. The `clients` of a function become its `tags`, which
OpenAPI tools use to group the operations, so a reader of the document
sees the evaluation API of each SDK and the console's admin API.

`generate.sh` writes the document to `generated/openapi.json`. The
whole file is long; here are the paths, with their HTTP methods and
tags, and the type of a flag key:

```console
$ sxml xquery --method=adaptive --expression='
    for key $path value $operation in json-doc("generated/openapi.json")?paths
    for key $method value $details in $operation
    return `{ $path }: { $method } ({ string-join($details?tags?*, ", ") })`'
"/api/getFlags: get (node)"
"/api/evaluateFlags: get (browser)"
"/api/getStatus: get (browser, node)"
"/api/rollOutFlag: post (console)"
"/api/killFlag: post (console)"
$ sxml xpath --method=json 'json-doc("generated/openapi.json")?components?schemas?FlagKey'
{"type":"string","pattern":"^(?:[a-z0-9][a-z0-9-]*)$","maxLength":64}
```

Which language to use for which step is a matter of taste. XQuery
reads like a program, and suits the generator, which looks things up
in two documents and builds a new one; XSLT's template rules suit the
printer and this stylesheet, whose sources have many kinds of nodes and
whose output follows the input, and its `xsl:result-document` writes
many files from one transformation.

## When the schema changes

The Vane protocol's draft says that a flag key has at most 32
characters, and the schema allowed 64: an SDK would accept a key that
the update stream can't carry. And the frontend SDK wants to know why a
flag is on or off for a user: through a rule, the percentage, the
default, or because the flag is off. `vane-api-2.xsd` is the next
version of the schema:

```console
$ diff -U2 -L vane-api.xsd -L vane-api-2.xsd vane-api.xsd vane-api-2.xsd
--- vane-api.xsd
+++ vane-api-2.xsd
@@ -8,5 +8,5 @@
     <xs:restriction base="xs:string">
       <xs:pattern value="[a-z0-9][a-z0-9-]*"/>
-      <xs:maxLength value="64"/>
+      <xs:maxLength value="32"/>
     </xs:restriction>
   </xs:simpleType>
@@ -33,4 +33,13 @@
   </xs:simpleType>

+  <xs:simpleType name="Reason">
+    <xs:restriction base="xs:string">
+      <xs:enumeration value="rule"/>
+      <xs:enumeration value="percentage"/>
+      <xs:enumeration value="default"/>
+      <xs:enumeration value="off"/>
+    </xs:restriction>
+  </xs:simpleType>
+
   <xs:simpleType name="Percentage">
     <xs:restriction base="xs:integer">
@@ -102,4 +111,5 @@
       <xs:element name="flag" type="FlagKey"/>
       <xs:element name="on" type="xs:boolean"/>
+      <xs:element name="reason" type="Reason" minOccurs="0"/>
     </xs:sequence>
   </xs:complexType>
```

Put it in place and generate again. The check of the last section now
fails, and names the files that are out of date:

```console
$ cp vane-api-2.xsd vane-api.xsd
$ sh generate.sh next
$ diff -rq generated next
Files generated/openapi.json and next/openapi.json differ
Files generated/program.xml and next/program.xml differ
Files generated/types.ts and next/types.ts differ
```

The clients and the server didn't change: no function's types did. The
tree did, and so the types: the difference is exactly the change to
the schema, written out in TypeScript:

```console
$ diff -u -L generated/types.ts -L next/types.ts generated/types.ts next/types.ts
--- generated/types.ts
+++ next/types.ts
@@ -7,7 +7,7 @@
   if (!checkString(value, path, problems)) return false;
   const count = problems.length;
   if (!/^(?:[a-z0-9][a-z0-9-]*)$/u.test(value)) problems.push(path + ": must match [a-z0-9][a-z0-9-]*");
-  if (characterCount(value) > 64) problems.push(path + ": must have at most 64 characters");
+  if (characterCount(value) > 32) problems.push(path + ": must have at most 32 characters");
   return problems.length === count;
 }

@@ -38,6 +38,15 @@
   return problems.length === count;
 }

+export type Reason = "rule" | "percentage" | "default" | "off";
+
+export function checkReason(value: unknown, path: string, problems: string[]): value is Reason {
+  if (!checkString(value, path, problems)) return false;
+  const count = problems.length;
+  if (!["rule", "percentage", "default", "off"].includes(value)) problems.push(path + ": must be one of rule, percentage, default, off");
+  return problems.length === count;
+}
+
 export type Percentage = number;

 export function checkPercentage(value: unknown, path: string, problems: string[]): value is Percentage {
@@ -187,6 +196,7 @@
 export interface Evaluation {
   flag: FlagKey;
   on: boolean;
+  reason?: Reason;
 }

 export function checkEvaluation(value: unknown, path: string, problems: string[]): value is Evaluation {
@@ -194,8 +204,9 @@
   const count = problems.length;
   checkFlagKey(value.flag, path + ".flag", problems);
   checkBoolean(value.on, path + ".on", problems);
+  if (value.reason !== undefined) checkReason(value.reason, path + ".reason", problems);
   for (const key of Object.keys(value)) {
-    if (!["flag", "on"].includes(key)) problems.push(path + "." + key + ": is not allowed");
+    if (!["flag", "on", "reason"].includes(key)) problems.push(path + "." + key + ": is not allowed");
   }
   return problems.length === count;
 }
```

The new limit is now in the checks of both SDKs and of the console, in
the service's, and in the OpenAPI document, from one edit:

```console
$ sxml xpath --method=json 'json-doc("next/openapi.json")?components?schemas?FlagKey'
{"type":"string","pattern":"^(?:[a-z0-9][a-z0-9-]*)$","maxLength":32}
```

A review of the change shows the schema and the generated code side by
side, so a reviewer sees what the schema change means for the code. The
reason is optional, so code written against the old types still
compiles, and an SDK that doesn't know about reasons still accepts the
answers of a service that sends them. Make it required, and the
TypeScript compiler points at every place in the service's code that
makes an evaluation without one.

## What we built

From two small XML files, one validated by the other's schema, we made
five TypeScript files, the clients of Vane's two SDKs and its console,
the service's request handler and the types they share, and an OpenAPI
document, which agree with each other because they come from the same
place:

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
  compares the clients, the server and the runtime;
- an XSLT stylesheet that writes JSON with template rules and maps;
- a script that makes everything, and a comparison that shows whether
  the kept files are current.

Each step is small, and each can be tried on its own: the generator's
functions return trees that a test can query, and the printer prints
any piece of a tree. The same approach works for other targets: a
vocabulary and a printer for a language on the server, SQL tables,
forms for a web page, or documentation. Whatever the target, the
schema stays the one place where a rule is written.
