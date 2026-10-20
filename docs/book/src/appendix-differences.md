# Appendix D: Differences from Other Processors

If you know another XPath, XQuery or XSLT processor, most of what you
know applies to sxml unchanged: the languages are standards, and sxml
follows them closely. These are the differences you are most likely to
notice.

## The 4.0 languages

sxml implements the 4.0 versions of XPath, XQuery and XSLT, so you can
use their new syntax everywhere: `{ }` map constructors without the
`map` keyword, `fn { }` and `fn($x) { }` inline functions, string
templates in backticks, `otherwise`, `=!>`, `for member`, records,
`while` clauses in FLWOR expressions, default parameter values and
keyword arguments, `transform with`, and the new functions such as
`sort-by`, `highest`, `partition`, `parse-csv` and `parse-html`.

Older code keeps working. XPath 3.1 and XQuery 3.1 expressions are 4.0
expressions, and XSLT 1.0, 2.0 and 3.0 stylesheets run with the behaviour
their `version` attribute asks for, XPath 1.0 compatibility mode
included. The 4.0 specifications are still drafts, and where a draft
changes, sxml follows the newer text.

## No network, few files

The sxml library reads only what its resolver gives it, and the `sxml`
command's resolver reads only local files. `doc("https://...")` is not
fetched. External DTDs and entities are not read unless you ask for
them, EXPath File exists only with `--allow-file-access`, and `fn:put`
writes only with `--allow-put`. Processors that read URLs and files by
default will behave differently here on purpose.

XInclude is processed only when a call asks for it: `fn:doc` and
`fn:parse-xml` with the options `xinclude` and `trust-external` both
true. `xpointer` attributes are not supported; an include that has one
takes its `xi:fallback`.

## Output

The `sxml xpath` command writes results by the *adaptive* output method,
which shows types: strings in quotes, doubles in exponent notation, and
other atomic values as constructor calls, such as
`xs:date("2026-10-20")`. Choose `--method=text` or another method when
you want plain output.

## Error messages

Error codes are those of the specifications, so `try`/`catch` and
`xsl:catch` code that tests them carries over. The descriptions are
sxml's own, and name the section of the specification that decides the
error.

## Extensions

sxml has no extension functions of its own beyond EXPath Binary and
EXPath File and EXSLT Common in XSLT 1.0 stylesheets, and it runs no
code written in other languages from a query. A C program that embeds
sxml can pass its own functions to a query as function items.

## Limits

Every evaluation runs under limits on the work it does, the depth of its
calls and the memory it uses, which a program sets in the processor's
configuration. The defaults are generous (a hundred million evaluation steps,
a million nested calls), and they keep an accidental infinite loop
from running forever. sxml never uses the C stack for deep nesting, so
deeply nested documents and deeply recursive functions are bounded only
by these limits and by memory.
