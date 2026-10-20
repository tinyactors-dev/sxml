# Running the Generated Code

<!-- examples: codegen -->
<!-- requires: javascript -->

Generated code is only as good as what it does when it runs. In this
section we'll start a server with the generated handler, and call it
with the generated client, the way a browser page would. The examples
need Node.js 22.18 or later, which runs TypeScript files directly by
removing their types; the generated code uses no TypeScript feature
that needs more than that. The files that `generate.sh` makes are in
the `generated` directory of the examples (the next section shows the
script).

## The server's own code

The generated interface says what the server must do; the library's
code does it. Here it keeps its books in an array:

`library.ts`:

```ts
{{#include ../examples/codegen/library.ts}}
```

The parameters need no type annotations: the interface gives them.
Mistype a property name, or return a loan without its due date, and
the TypeScript compiler finds it, because the types come from the
schema.

## Calling it

`demo.ts` starts a server on a free port, points the client at it, and
calls each function. Then it tries two calls that should fail:

`demo.ts`:

```ts
{{#include ../examples/codegen/demo.ts}}
```

```console
$ node demo.ts
getBook {"id":"dune","title":"Dune","genre":"fiction","available":true}
searchBooks {"books":[{"id":"sicp","title":"Structure and Interpretation of Computer Programs","genre":"science","available":false}]}
borrowBook {"book":"spqr","member":"m0042","due":"2026-11-03"}
borrowBook failed: input.member: must match m[0-9]{4}; input.days: must be at most 28
fetch 400 {"problems":["input.book: must match [a-z][a-z0-9-]*","input.renew: is not allowed"]}
```

The first three calls work. The fourth has the right TypeScript types,
a string and a number, but values that the schema's facets refuse, and
the client stops it before it goes over the network. The last request
doesn't use the client at all, as a request from another program
wouldn't, and the server refuses it with the same rules, and an answer
that says why.

A browser page uses `client.ts` the same way, with `settings.endpoint`
left at `/api`, and the server is the same handler, whatever framework
serves the page.
