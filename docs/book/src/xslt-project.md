# Project: Publishing the Specification

<!-- examples: spec -->

An Internet-Draft is written once and read in several forms: as a web
page with a table of contents, as plain text in the style of an RFC, and
by tools that want the requirements as data. Each new version also
needs a page that says what changed since the last one. This project
makes all of them from `latch.xml` with one script, and stops before it
writes anything when the draft has a mistake that would make the
published text wrong.

The pieces come from the earlier sections of this chapter. The HTML
page is `html.xsl` and the plain text is `text.xsl`, both with the
package `spec.xsl` and the module `common.xsl` from [Many Outputs,
Functions and Packages](xslt-packages.md). New are a check, a JSON
output and the page of changes.

## The script

`publish.sh`:

```sh
{{#include ../examples/spec/publish.sh}}
```

The steps run in order, and `set -e` stops the script at the first one
that fails: every `sxml` command exits with a status other than 0 when
it fails. The validation against `latch.xsd` comes first, because a
stylesheet that meets an element it doesn't expect makes wrong output,
not an error. Every transformation writes its own file with `--output`,
and `--force` replaces the file of the last run.

## The check

The schema says what the draft may contain. It cannot say that every
reference is cited, or that a key word is marked as one. A query can:

`publish-check.xq`:

```xquery
{{#include ../examples/spec/publish-check.xq}}
```

The query makes two lists. An *error* is a problem that makes the
published text wrong: an `xref` to an anchor that doesn't exist, a
reference that no `xref` cites, a key word written in capitals but
without `bcp14`, which a reader cannot tell from a requirement. (The
key words of the Terminology section are quoted on purpose, so the
check skips that section.) A *warning* is open editorial work: a
paragraph with a key word but no anchor, which other documents cannot
refer to, and a note. When there are errors, `fn:error` stops the query
with all of them in its description, and `sxml xquery` exits with
status 1; otherwise the query writes the warnings, one on each line.

[Project: Editing a Specification Without Rewriting
It](xquery-project.md) builds a stricter check, which counts the
unanchored `SHOULD` as an error, and the edits that fix what it finds.

## The requirements as JSON

Tools that test locks, or a tracker of open requirements, want the
requirements as data. With the `json` output method, a stylesheet's
result can be a map, written as a JSON object:

`requirements.xsl`:

```xml
{{#include ../examples/spec/requirements.xsl}}
```

The text of each requirement is the text a reader sees, with the
`xref`s made by the rules of `common.xsl`: `spec:text` applies the
templates of the stylesheet that imports it.

## The changes

The page of changes compares the draft with the earlier one, which the
parameter `old` names. Requirements are matched by anchor: an anchor is
given once and never changes, so the same anchor in both drafts is the
same requirement, however its text and its number changed. A key finds
a requirement by anchor in either document:

`changes.xsl`:

```xml
{{#include ../examples/spec/changes.xsl}}
```

Each requirement of the new draft is added, changed or the same; a
requirement whose anchor only the old draft has was removed, and gets
a row of its own in the mode `removed`. The texts are compared as
`spec:text` makes them, so a line broken in another place is not a
change. The links point into the HTML page, `latch.html`, which the
script writes next to this one.

## Publishing

```console
$ sh publish.sh
warning: security: a requirement without an anchor
warning: lockout: open note issue-1 (AM)
warning: security: open note issue-2 (AB)
published latch.xml into site/
$ ls site
changes.html
latch.html
latch.txt
requirements.json
```

The draft has no errors, and three warnings: the open notes, and the
`SHOULD` in the Security Considerations, which has no anchor yet. The
four files are in `site/`. The HTML page starts with the table of
contents and ends with the index of requirements and the references:

```console
$ sed -n '/<nav>/,/<\/nav>/p' site/latch.html
    <nav>
      <ol class="toc">
        <li><a href="#intro">1. Introduction</a><ol><li><a href="#terminology">1.1. Terminology</a></li></ol></li>
        <li><a href="#states">2. Lock States</a></li>
        <li><a href="#messages">3. Messages</a><ol><li><a href="#unlock">3.1. Unlock</a></li><li><a href="#status">3.2. Status</a></li></ol></li>
        <li><a href="#lockout">4. Lockout</a></li>
        <li><a href="#security">5. Security Considerations</a></li>
        <li><a href="#iana">6. IANA Considerations</a></li>
      </ol>
    </nav>
$ grep -A4 '<a href="#req-9">' site/latch.html
          <td><a href="#req-9">R6</a></td>
          <td>MAY</td>
          <td>3.1. Unlock</td>
        </tr>
        <tr>
```

The plain text is the draft as `text.xsl` writes it:

```console
$ sed -n '/^4\./,/^5\./p' site/latch.txt
4.  Lockout

   After three rejected codes in a row, a lock MUST enter lockout.

   A lockout SHOULD last five minutes.

   A lock MAY report a lockout to the controller as soon as it begins.

5.  Security Considerations
```

The JSON file is for programs, and a query reads it back as one:

```console
$ head -10 site/requirements.json
{
  "draft": "draft-example-latch-protocol-01",
  "requirements": [
    {
      "anchor": "req-1",
      "number": "R1",
      "keyword": "MUST",
      "section": "2",
      "text": "A lock MUST be in exactly one of the states \"locked\", \"unlocked\" and \"lockout\"."
    },
$ sxml xpath 'json-doc("site/requirements.json")?requirements?*[?keyword = "MUST"] ! `{?number} ({?anchor}) in Section {?section}`'
"R1 (req-1) in Section 2"
"R2 (req-2) in Section 2"
"R4 (req-4) in Section 3.1"
"R7 (req-6) in Section 3.2"
"R9 (req-7) in Section 4"
```

And the page of changes lists the three requirements that are new in
the second draft and the two whose text changed:

```console
$ grep -A3 '<tr' site/changes.html
      <tr class="added">
        <td><a href="latch.html#req-9">R6</a></td>
        <td>added</td>
        <td><ins>A controller MAY send codes that are time-based one-time passwords [RFC6238].</ins></td>
--
      <tr class="added">
        <td><a href="latch.html#req-10">R8</a></td>
        <td>added</td>
        <td><ins>A controller SHOULD NOT send more than one status request per second to a lock.</ins></td>
--
      <tr class="changed">
        <td><a href="latch.html#req-7">R9</a></td>
        <td>changed</td>
        <td><del>After five rejected codes in a row, a lock MUST enter lockout.</del><br><ins>After three rejected codes in a row, a lock MUST enter lockout.</ins></td>
--
      <tr class="changed">
        <td><a href="latch.html#req-8">R10</a></td>
        <td>changed</td>
        <td><del>A lockout SHOULD last one minute.</del><br><ins>A lockout SHOULD last five minutes.</ins></td>
--
      <tr class="added">
        <td><a href="latch.html#req-11">R11</a></td>
        <td>added</td>
        <td><ins>A lock MAY report a lockout to the controller as soon as it begins.</ins></td>
```

Swap the drafts, and the same stylesheet shows the way back: the three
new requirements are removed rows.

```console
$ sxml transform --param=old=latch.xml changes.xsl latch-00.xml | grep -c 'class="removed"'
3
```

## When the draft is wrong

Two broken copies show the two guards. In the first, an `xref` points
to `RFC6239`, which the draft does not have. The schema's key reference
from `xref/@target` to the anchors finds it, and nothing is written:

```console
$ sed 's/target="RFC6238"/target="RFC6239"/' latch.xml > broken.xml
$ sh publish.sh broken.xml
broken.xml:89:39: cvc-identity-constraint.4.3: element 'xref': its key sequence is no key sequence of the referenced key (keyref 'xref-target')
$ echo $?
0
```

In the second, the citation of RFC 6238 is gone and a `SHOULD` has lost
its markup. The copy is valid, and the check stops it:

```console
$ sed -e 's| <xref target="RFC6238"/>||' -e 's|<bcp14>SHOULD</bcp14> last|SHOULD last|' latch.xml > unmarked.xml
$ sh publish.sh unmarked.xml
publish-check.xq:33:8: spec:check: references: [RFC6238] is never cited; lockout: a key word without bcp14: "A lockout SHOULD last five minutes."
$ echo $?
0
```

The files in `site/` are still those of the last good run. Each step
adds one guarantee, and the stylesheets can assume what the steps before
them checked: that every target exists, that every key word is marked,
that the draft has the structure the schema describes.
