# Project: A Report on the Loans

<!-- examples: xquery-project -->

Let's put the whole chapter to work. In this project we'll write a
small application in XQuery: a report on the library's loans, as an
HTML page for people, as JSON for other programs, and as one reminder
file for each member who should bring a book back. The three outputs
share one library module, so each fact is computed in one place.

The files are in `docs/book/examples/xquery-project`. The library is
the one from before, and the members are in the JSON file from
[Documents, Parameters and the Context](xquery-inputs.md). The loans now
say when a book came back, and one loan names a book that the library
doesn't have, as real data sometimes does:

`loans.xml`:

```xml
{{#include ../examples/xquery-project/loans.xml}}
```

## The plan

The report answers three questions on a given day:

- Which loans are overdue? A loan lasts 21 days, and is overdue when the
  book hasn't come back by its due date.
- How many books has each member borrowed, and how many are still out?
- Which loans can't be described, because the book is unknown?

The day is a parameter, so we can run the report for any day, and test
it with a fixed one.

## The module

Everything the three queries need to know about loans goes into one
library module:

`library.xqm`:

```xquery
{{#include ../examples/xquery-project/library.xqm}}
```

Let's read it from the top.

- `$lib:loan-days` is the length of a loan, in one place.
- `lib:loan` is a record type: a map with the facts about one loan
  that the outputs need, with their types. `declare record` also
  declares the function `lib:loan`, which makes such a record from
  keyword arguments.
- `lib:title` finds a title, or raises the error `lib:unknown-book`,
  with the id as the error's value.
- `lib:loan-info` turns a `loan` element into a record. The member's
  name comes from the JSON map, or is the member's id when the map
  doesn't have it.
- `lib:is-overdue` decides about one loan.
- `lib:loan-infos` makes the records for many loans. It catches
  `lib:unknown-book`, and only that error, and leaves the loan out.

## Trying the pieces

Before writing a report, try the functions on their own. `--expression`
is good for that. One loan becomes one record:

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace lib = "https://example.org/library" at "library.xqm";
    lib:loan-info(<loan book="dune" member="ada" date="2026-10-01"/>, doc("library.xml"), json-doc("members.json"))'
{"member":"ada","name":"Ada Lovelace","title":"Dune","date":xs:date("2026-10-01"),"due":xs:date("2026-10-22"),"returned":()}
```

An unknown book raises the module's error:

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace lib = "https://example.org/library" at "library.xqm";
    try { lib:title(doc("library.xml"), "emma") } catch lib:unknown-book { $err:description }'
"No book has the id emma"
```

The loans file has eight loans, and seven of them are of known books:

```console
$ sxml xquery --method=adaptive --expression='
    import module namespace lib = "https://example.org/library" at "library.xqm";
    count(lib:loan-infos(doc("loans.xml")//loan, doc("library.xml"), json-doc("members.json")))'
7
```

## The HTML report

The report is a main module that imports the library module, declares
the day as an external variable, and builds a page with constructors:

`report.xq`:

```xquery
{{#include ../examples/xquery-project/report.xq}}
```

The global variables read the input once. The overdue list is a FLWOR
expression with a predicate that calls `lib:is-overdue`, and the table
groups the records by member's name. The warning paragraph uses the
braced `if`, so there is no paragraph when every book is known. Run it
for the 20th of October:

```console
$ sxml xquery --param=today=2026-10-20 report.xq
<!DOCTYPE html>
<html>
  <head>
    <meta charset="UTF-8">
    <title>Loans on 2026-10-20</title>
  </head>
  <body>
    <h1>Loans on 2026-10-20</h1>
    <h2>Overdue</h2>
    <ul>
      <li>Alan Turing has <cite>Godel, Escher, Bach</cite>, due 2026-09-26</li>
      <li>Ada Lovelace has <cite>Structure and Interpretation of Computer Programs</cite>, due 2026-10-11</li>
    </ul>
    <h2>Members</h2>
    <table>
      <tr>
        <th>Member</th>
        <th>Loans</th>
        <th>Out now</th>
      </tr>
      <tr>
        <td>Ada Lovelace</td>
        <td>3</td>
        <td>2</td>
      </tr>
      <tr>
        <td>Alan Turing</td>
        <td>2</td>
        <td>2</td>
      </tr>
      <tr>
        <td>Grace Hopper</td>
        <td>2</td>
        <td>1</td>
      </tr>
    </table>
    <p class="warning">Loans of unknown books: emma (grace, 2026-10-14)</p>
  </body>
</html>
```

Check it against the data: Alan has had *Godel, Escher, Bach* since the
5th of September, due 21 days later, and Ada's *SICP* was due on the
11th of October. Ada's *Dune* was due in September too, but it came
back. Grace borrowed three books, but one of them is the unknown
`emma`, so the table counts two.

A week earlier, fewer loans are overdue:

```console
$ sxml xquery --param=today=2026-10-10 report.xq | grep '<li>'
      <li>Alan Turing has <cite>Godel, Escher, Bach</cite>, due 2026-09-26</li>
```

To publish the page, write it to a file:

```console
$ sxml xquery --param=today=2026-10-20 --output=report.html report.xq
$ grep -c '<tr>' report.html
4
```

## The same facts as JSON

Another program, say a web page that shows the overdue loans, would
rather have JSON. A second main module uses the same module, and builds
a map instead of a page:

`summary.xq`:

```xquery
{{#include ../examples/xquery-project/summary.xq}}
```

```console
$ sxml xquery --param=today=2026-10-20 summary.xq
{
  "date": "2026-10-20",
  "loans": 7,
  "overdue": [
    {
      "member": "alan",
      "title": "Godel, Escher, Bach",
      "due": "2026-09-26"
    },
    {
      "member": "ada",
      "title": "Structure and Interpretation of Computer Programs",
      "due": "2026-10-11"
    }
  ],
  "popular": [
    "Dune",
    "Godel, Escher, Bach",
    "Structure and Interpretation of Computer Programs"
  ]
}
```

Dates are converted to strings with `string`, because JSON has no
dates. The popular books use the clauses of `popular.xq` from the start
of the chapter: group, sort by count, number with `count`, keep three.

## One file for each member

Last, the reminders. `fn:put` stores a document at a URI, and a FLWOR
expression can make one for each member:

`reminders.xq`:

```xquery
{{#include ../examples/xquery-project/reminders.xq}}
```

`put` is an updating function, so the query writes its files only when
it has run to the end without an error, and only with `--allow-put`:

```console
$ sxml xquery --param=today=2026-10-20 --allow-put reminders.xq
$ ls reminder-*
reminder-ada.xml
reminder-alan.xml
$ cat reminder-alan.xml
<reminder to="Alan Turing" date="2026-10-20"><book due="2026-09-26">Godel, Escher, Bach</book></reminder>
```

## What we built

The three queries are short because the module does the work, and they
agree with each other because the rules are written once: change
`$lib:loan-days` to 14, and the page, the JSON and the reminders all
change together. Along the way, the project used most of this chapter:

- a library module with a record type, typed functions, a module
  variable and an error code of its own;
- FLWOR expressions with `where`, `order by`, `group by` and `count`;
- `try` and `catch` for one expected error, and nothing else;
- external variables with a type and a default;
- constructors for HTML, maps and arrays for JSON, and output
  declarations for both;
- `fn:put` for many output files.

## Summary

XQuery adds to XPath what you need to write programs:

- FLWOR expressions iterate, filter, sort, group, number and window;
- constructors build XML, and output declarations write it as XML,
  HTML, text, JSON or CSV;
- `switch`, `typeswitch`, sequence types and `try`/`catch` deal with
  untidy data;
- functions, which can be recursive, and library modules organize a
  program, and external variables make it a tool;
- updates change documents, or copies of them.

Next, a different way to think about turning documents into other
documents: XSLT.
