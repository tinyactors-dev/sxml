# Generating TypeScript with XQuery

<!-- examples: codegen -->

A code generator is a program that writes text, and most of that text
is the same every time: the shape of a function, its braces, its
keywords. Only a few names and numbers change. XQuery has a kind of
string made for this.

## String constructors

A *string constructor* starts with ``` ``[ ``` and ends with ``` ]`` ```.
Everything between is text, kept as it is, line breaks included, except
for expressions written between `` `{ `` and `` }` ``, whose values are
put in:

```console
$ sxml xquery --method=text --expression='
    let $name := "BookId"
    return ``[export function check`{ $name }`(value: unknown): boolean { return true; }]``'
export function checkBookId(value: unknown): boolean { return true; }
```

Braces, quotes and backticks are ordinary text in a string constructor,
so TypeScript, C or JSON can be written in it as it looks, without
escapes. The string templates of [What's New in XQuery
4.0](xquery-4.md) are shorter, but there every brace starts an
expression, which is the wrong default for code with many braces.

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
  wrong. `path` says where the value is, such as `input.days`, so the
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
thing it writes:

`typescript.xqm`:

```xquery
{{#include ../examples/codegen/typescript.xqm}}
```

Let's read it from the top.

- `$ts:built-in` maps the built-in XSD types that the API uses to their
  TypeScript type and their check. `ts:type` and `ts:check` look a type
  name up there, and a name of the schema keeps its name. A built-in
  type that isn't in the map is an error, not a guess.
- `ts:literal` writes a string as a JavaScript string literal. JSON
  strings are JavaScript strings, so `serialize` with the JSON method
  does it, with every escape that is needed.
- `ts:facet` turns one facet into one line of TypeScript. A `pattern`
  becomes a regular expression with `^(?:` and `)$` around it, because
  an XSD pattern must match the whole value (XSD 1.1 Part 2, appendix
  G). Length facets count characters. A facet the generator doesn't
  know is an error: a generator that left a rule out without saying so
  would make code that accepts what the schema refuses.
- `ts:simple-type` writes a type and a check for an `xs:simpleType`.
  With enumeration facets, the type is a union of string literals, so
  the TypeScript compiler knows the values too.
- `ts:property` and `ts:property-check` handle one element of a
  sequence: required, optional with `?`, or an array. `ts:complex-type`
  writes an interface and a check, which also refuses properties that
  the schema doesn't have.
- `ts:types` writes `types.ts`: every type of the schema, in the order
  of the schema.
- `ts:functions` returns the functions of the description, after it has
  checked that their types are complex types of the schema.
- `ts:client` and `ts:server` write `client.ts` and `server.ts`.

The XSD types are read as XML, with paths such as
`xs:restriction/@base`. This is the schema as a document, not the
compiled schema that `sxml validate` uses: a generator needs to know
how the types are written, and paths are the simplest way to ask.

## Trying the pieces

As with any library module, try the functions on their own first. One
simple type becomes a type and a check:

```console
$ sxml xquery --method=text --expression='
    import module namespace ts = "https://example.org/typescript" at "typescript.xqm";
    declare namespace xs = "http://www.w3.org/2001/XMLSchema";
    ts:simple-type(doc("library.xsd")//xs:simpleType[@name = "LoanDays"])'

export type LoanDays = number;

export function checkLoanDays(value: unknown, path: string, problems: string[]): value is LoanDays {
  if (!checkInteger(value, path, problems)) return false;
  const count = problems.length;
  if (value < 1) problems.push(path + ": must be at least 1");
  if (value > 28) problems.push(path + ": must be at most 28");
  return problems.length === count;
}
```

The facets `minInclusive` and `maxInclusive` became the two `if`
statements. The checks are *type predicates*: when `checkInteger`
returns true, TypeScript knows that `value` is a number in the lines
after it. A complex type becomes an interface:

```console
$ sxml xquery --method=text --expression='
    import module namespace ts = "https://example.org/typescript" at "typescript.xqm";
    declare namespace xs = "http://www.w3.org/2001/XMLSchema";
    ts:complex-type(doc("library.xsd")//xs:complexType[@name = "BookSearch"])'

export interface BookSearch {
  genre?: Genre;
  availableOnly?: boolean;
}

export function checkBookSearch(value: unknown, path: string, problems: string[]): value is BookSearch {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  if (value.genre !== undefined) checkGenre(value.genre, path + ".genre", problems);
  if (value.availableOnly !== undefined) checkBoolean(value.availableOnly, path + ".availableOnly", problems);
  for (const key of Object.keys(value)) {
    if (!["genre", "availableOnly"].includes(key)) {
      problems.push(path + "." + key + ": is not allowed");
    }
  }
  return problems.length === count;
}
```

And a facet without a translation stops the generator:

```console
$ sxml xquery --expression='
    import module namespace ts = "https://example.org/typescript" at "typescript.xqm";
    ts:facet(<xs:totalDigits value="5"/>)'
typescript.xqm:62:20: ts:unsupported: The generator has no check for the facet totalDigits
```

## Writing the files

A main module puts it together. It reads the description, finds its
schema, and stores each file with `fn:put`, as text:

`generate.xq`:

```xquery
{{#include ../examples/codegen/generate.xq}}
```

The third argument of `put` holds serialization parameters, as in
[Writing Results](xquery-serialization.md); the text method writes the
string as it is. The files are put in sorted order, so a run always does
the same thing in the same order. `put` doesn't make directories, so we
make one first:

```console
$ mkdir out
$ sxml xquery --allow-put --param=directory=out generate.xq
$ ls out
client.ts
server.ts
types.ts
```

The client has one async function for each remote function. It checks
the input before it sends it, so a wrong value fails at once, in the
browser, and checks the output, so a server that answers with something
else can't go unnoticed:

```console
$ cat out/client.ts
// Generated from the API description by generate.xq. Do not edit.
import { call, expect } from "../runtime.ts";
import type { BookRequest, Book, BookSearch, BookList, LoanRequest, Loan } from "./types.ts";
import { checkBookRequest, checkBook, checkBookSearch, checkBookList, checkLoanRequest, checkLoan } from "./types.ts";

/** Finds one book by its id. */
export async function getBook(input: BookRequest): Promise<Book> {
  expect<BookRequest>(input, checkBookRequest);
  const output = await call("getBook", "query", input);
  return expect<Book>(output, checkBook);
}

/** Lists the books of a genre, or all books. */
export async function searchBooks(input: BookSearch): Promise<BookList> {
  expect<BookSearch>(input, checkBookSearch);
  const output = await call("searchBooks", "query", input);
  return expect<BookList>(output, checkBookList);
}

/** Lends a book to a member for some days. */
export async function borrowBook(input: LoanRequest): Promise<Loan> {
  expect<LoanRequest>(input, checkLoanRequest);
  const output = await call("borrowBook", "command", input);
  return expect<Loan>(output, checkLoan);
}
```

A page imports `getBook` and calls it like any other function; that it
runs on the server is a detail of how it is made. The server side is an
interface, which the server's own code implements, and a handler that
routes each request to it, with the checks around it:

```console
$ cat out/server.ts
// Generated from the API description by generate.xq. Do not edit.
import { serve } from "../runtime.ts";
import type { BookRequest, Book, BookSearch, BookList, LoanRequest, Loan } from "./types.ts";
import { checkBookRequest, checkBook, checkBookSearch, checkBookList, checkLoanRequest, checkLoan } from "./types.ts";

/** What the server does: one method for each remote function. */
export interface Implementation {
  /** Finds one book by its id. */
  getBook(input: BookRequest): Promise<Book>;
  /** Lists the books of a genre, or all books. */
  searchBooks(input: BookSearch): Promise<BookList>;
  /** Lends a book to a member for some days. */
  borrowBook(input: LoanRequest): Promise<Loan>;
}

/** A Node.js request listener that checks every input and output. */
export function createHandler(implementation: Implementation) {
  return serve({
    getBook: {
      kind: "query",
      checkInput: checkBookRequest,
      checkOutput: checkBook,
      run: (input) => implementation.getBook(input),
    },
    searchBooks: {
      kind: "query",
      checkInput: checkBookSearch,
      checkOutput: checkBookList,
      run: (input) => implementation.searchBooks(input),
    },
    borrowBook: {
      kind: "command",
      checkInput: checkLoanRequest,
      checkOutput: checkLoan,
      run: (input) => implementation.borrowBook(input),
    },
  });
}
```

`types.ts` has every type of the schema, as in the pieces above. The
check for a loan request calls the checks of its three properties:

```console
$ sed -n '/^export function checkLoanRequest/,/^}/p' out/types.ts
export function checkLoanRequest(value: unknown, path: string, problems: string[]): value is LoanRequest {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkBookId(value.book, path + ".book", problems);
  checkMemberId(value.member, path + ".member", problems);
  checkLoanDays(value.days, path + ".days", problems);
  for (const key of Object.keys(value)) {
    if (!["book", "member", "days"].includes(key)) {
      problems.push(path + "." + key + ": is not allowed");
    }
  }
  return problems.length === count;
}
```

## The rule the schema couldn't check

`ts:functions` checks that each function's types exist. With a typo in
a type name, the generator stops with its own error, and writes no file,
because `put` stores its documents only when the whole query succeeds:

```console
$ sed 's/output="Loan"/output="Receipt"/' api.xml > typo.xml
$ mkdir typo
$ sxml xquery --allow-put --param=api-file=typo.xml --param=directory=typo generate.xq
typescript.xqm:158:10: ts:unknown-type: The function borrowBook uses the type Receipt, which the schema doesn't declare
$ ls typo
```

Next, let's run the code.
