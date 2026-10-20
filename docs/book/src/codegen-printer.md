# Printing the Tree with XSLT

<!-- examples: codegen -->

The tree says what the program is. The printer says how it is written:
which keywords and punctuation each kind of element becomes, where the
lines break and how far they are indented. This is the kind of job
XSLT's template rules were made for, as in [Transforming with
XSLT](xslt.md): the output follows the input, one rule for each kind of
element, and the processor finds the rule for each one.

## The printer

`print.xsl`:

```xml
{{#include ../examples/codegen/print.xsl}}
```

The stylesheet writes text, so `xsl:output` says `method="text"`, and
the text in a template body is the TypeScript itself: the rule for
`return` reads `return <xsl:apply-templates/>;`. `xsl:strip-space`
drops the spaces and line breaks that indent the tree, except inside
the elements whose text is part of the program, such as `string`.

Let's read the rules in groups.

- *Layout.* Only the first group knows about lines. `print:indent`
  counts the blocks around a node (a function's `body`, a `then`, an
  interface, an object) and indents by `$indent` for each.
  `print:braces` says which blocks get braces: all but the `then` of an
  `if` with one statement and no `else`, which stays on the line of the
  `if`. The rule with priority 1 matches everything that is a line of
  its own: a declaration in a module, a statement in a block, a member
  of an interface, an entry of an object. It writes the indentation,
  calls `xsl:next-match` to let the rule for the element write the
  element, and ends the line. The rule with priority 2 puts an empty
  line before each declaration, and then lets the line rule do the
  rest. So every other rule writes one construct without a thought for
  where it is.
- *Files.* Each `module` of a `program` becomes a file through
  `xsl:result-document`, named by its `file` attribute. A `module` on
  its own is written to standard output, which is what the pieces of
  the last section used.
- *Declarations, types, statements.* One rule each, in the words of the
  vocabulary. Lists are joined with the `separator` attribute of
  `xsl:for-each`, new in XSLT 4.0: parameters, arguments and type
  arguments with `", "`, a union with `" | "`.
- *Expressions.* A `string` becomes a JSON string, which is a valid
  JavaScript string, with every escape it needs; the same function
  writes the `from` of an import. A `regex` escapes its slashes. The
  operators are a map from their names to their symbols, used as the
  separator of a `binary` expression's operands. The generator never
  puts one binary expression into another, so the printer needs no
  rules for parentheses; a printer for any tree would add them where
  the precedence of the operators needs them.
- *Anything else* is an error: the last rule matches every element
  that no other rule does, with the lowest priority, and stops the
  transformation, so an element the printer doesn't know can't be
  dropped without a word:

```console
$ echo '<module><while/></module>' | sxml transform print.xsl -
print.xsl:302:20: print:unknown: The printer has no rule for while
```

## Writing the files

`generate.sh`, in the last section, keeps the tree of the program in
`generated/program.xml`, next to the files printed from it. Print it
into a new directory: the `--result-directory` option lets the
transformation write its result documents there, as in [Many Outputs,
Functions and Packages](xslt-packages.md):

```console
$ mkdir out
$ sxml transform --result-directory=out print.xsl generated/program.xml
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
// Generated from the API description by program.xq and print.xsl. Do not edit.
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
// Generated from the API description by program.xq and print.xsl. Do not edit.
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

The objects inside objects of `createHandler` are indented by the same
rule as the statements: an `entry` is a line, inside one block more for
each `object` around it.

`types.ts` has every type of the schema, as in the pieces of the last
section. The check for a loan request calls the checks of its three
properties:

```console
$ sed -n '/^export function checkLoanRequest/,/^}/p' out/types.ts
export function checkLoanRequest(value: unknown, path: string, problems: string[]): value is LoanRequest {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkBookId(value.book, path + ".book", problems);
  checkMemberId(value.member, path + ".member", problems);
  checkLoanDays(value.days, path + ".days", problems);
  for (const key of Object.keys(value)) {
    if (!["book", "member", "days"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}
```

The printer reads nothing but the tree: no date, no file name, no
order that could change from one run to the next. So the same tree
always gives the same text, and the same sources the same tree.

## The same tree in another style

A team that wants four spaces, and braces around every block, doesn't
need another generator, or even another printer. A stylesheet imports
`print.xsl` and replaces the two things that decide the layout: the
parameter `$indent` and the function `print:braces`. A declaration in
the importing stylesheet wins over the one it imports, as a template
rule does:

`braces.xsl`:

```xml
{{#include ../examples/codegen/braces.xsl}}
```

The check of a book list, printed both ways:

```console
$ sxml xquery --method=xml --expression='<module>{ doc("generated/program.xml")//function[@name = "checkBookList"] }</module>' > book-list.xml
$ sxml transform print.xsl book-list.xml
export function checkBookList(value: unknown, path: string, problems: string[]): value is BookList {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  if (Array.isArray(value.books)) {
    value.books.forEach((item, index) => checkBook(item, path + ".books[" + index + "]", problems));
  } else {
    problems.push(path + ".books: expected an array");
  }
  for (const key of Object.keys(value)) {
    if (!["books"].includes(key)) problems.push(path + "." + key + ": is not allowed");
  }
  return problems.length === count;
}
$ sxml transform braces.xsl book-list.xml
export function checkBookList(value: unknown, path: string, problems: string[]): value is BookList {
    if (!checkObject(value, path, problems)) {
        return false;
    }
    const count = problems.length;
    if (Array.isArray(value.books)) {
        value.books.forEach((item, index) => checkBook(item, path + ".books[" + index + "]", problems));
    } else {
        problems.push(path + ".books: expected an array");
    }
    for (const key of Object.keys(value)) {
        if (!["books"].includes(key)) {
            problems.push(path + "." + key + ": is not allowed");
        }
    }
    return problems.length === count;
}
```

Only the layout changed: the tree is the same, so what the code does is
the same. A style is a few declarations, not a second copy of the
generator.
