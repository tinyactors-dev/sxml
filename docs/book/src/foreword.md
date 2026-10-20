# Foreword

XML has been around long enough that most programmers have an opinion
about it, and the opinion is often formed by the tools they met first: a
parser that handed them a tree of nodes and left the rest to them, a
library that needed a dozen other libraries, a processor that ran only on
one platform.

The languages built on top of XML tell a different story. XPath is a
small, precise language for finding things. XQuery is a functional
programming language whose values are documents, maps, arrays and
numbers. XSLT turns one document into another by rules, and can do it
while the input streams past. XSD gives documents types. Their latest
versions, XPath 4.0, XQuery 4.0 and XSLT 4.0, add maps and arrays, JSON
and CSV, records, better functions and a friendlier syntax.

sxml is an implementation of all of them in one small C library. It
makes no system calls you did not ask for, uses only the memory you give
it, and runs the same code on a server, in a phone app, in a browser and
on the command line. This book is about using it.

You do not need to know XML's languages to read this book. You need a
terminal, a little curiosity and a few minutes for each chapter. Every
example in the book is real: it was run with sxml, and the output you see
is the output sxml gave.
