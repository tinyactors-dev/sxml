# Project: Publishing the Specification

<!-- examples: spec -->

An Internet-Draft is written once and read in several forms: as a web
page with a table of contents, as plain text in the style of an RFC, and
by tools that want the requirements as data. Each new version also
needs a page that says what changed since the last one. This project
makes all of them from `vane.xml` with one script, and stops before it
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
it fails. The validation against `vane.xsd` comes first, because a
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

Tools that test an SDK, or a tracker of open requirements, want the
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
change. The links point into the HTML page, `vane.html`, which the
script writes next to this one.

## Publishing

```console
$ sh publish.sh
warning: security: a requirement without an anchor
warning: pause: open note issue-1 (AM)
warning: security: open note issue-2 (AB)
published vane.xml into site/
$ ls site
changes.html
requirements.json
vane.html
vane.txt
```

The draft has no errors, and three warnings: the open notes, and the
`SHOULD` in the Security Considerations, which has no anchor yet. The
four files are in `site/`. The HTML page starts with the table of
contents and ends with the index of requirements and the references:

```console
$ sed -n '/<nav>/,/<\/nav>/p' site/vane.html
    <nav>
      <ol class="toc">
        <li><a href="#intro">1. Introduction</a><ol><li><a href="#terminology">1.1. Terminology</a></li></ol></li>
        <li><a href="#states">2. Flag States</a></li>
        <li><a href="#messages">3. Messages</a><ol><li><a href="#rollout">3.1. Rollout</a></li><li><a href="#status">3.2. Status</a></li></ol></li>
        <li><a href="#pause">4. Pause</a></li>
        <li><a href="#security">5. Security Considerations</a></li>
        <li><a href="#iana">6. IANA Considerations</a></li>
      </ol>
    </nav>
$ grep -A4 '<a href="#req-9">' site/vane.html
          <td><a href="#req-9">R6</a></td>
          <td>MAY</td>
          <td>3.1. Rollout</td>
        </tr>
        <tr>
```

The plain text is the draft as `text.xsl` writes it:

```console
$ sed -n '/^4\./,/^5\./p' site/vane.txt
4.  Pause

   After three failed checks in a row, a service MUST put the flag in a
   pause.

   A pause SHOULD last five minutes.

   A service MAY report a pause to the owners of the flag as soon as it
   begins.

5.  Security Considerations
```

The JSON file is for programs, and a query reads it back as one:

```console
$ head -10 site/requirements.json
{
  "draft": "draft-example-vane-protocol-01",
  "requirements": [
    {
      "anchor": "req-1",
      "number": "R1",
      "keyword": "MUST",
      "section": "2",
      "text": "A flag MUST be in exactly one of the states \"stable\", \"rolling-out\" and \"paused\"."
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
        <td><a href="vane.html#req-9">R6</a></td>
        <td>added</td>
        <td><ins>A service MAY ask an owner for a time-based one-time password [RFC6238] before a rollout.</ins></td>
--
      <tr class="added">
        <td><a href="vane.html#req-10">R8</a></td>
        <td>added</td>
        <td><ins>An SDK SHOULD NOT send more than one status request per second for a flag; it uses its cached answer in between.</ins></td>
--
      <tr class="changed">
        <td><a href="vane.html#req-7">R9</a></td>
        <td>changed</td>
        <td><del>After five failed checks in a row, a service MUST put the flag in a pause.</del><br><ins>After three failed checks in a row, a service MUST put the flag in a pause.</ins></td>
--
      <tr class="changed">
        <td><a href="vane.html#req-8">R10</a></td>
        <td>changed</td>
        <td><del>A pause SHOULD last one minute.</del><br><ins>A pause SHOULD last five minutes.</ins></td>
--
      <tr class="added">
        <td><a href="vane.html#req-11">R11</a></td>
        <td>added</td>
        <td><ins>A service MAY report a pause to the owners of the flag as soon as it begins.</ins></td>
```

Swap the drafts, and the same stylesheet shows the way back: the three
new requirements are removed rows.

```console
$ sxml transform --param=old=vane.xml changes.xsl vane-00.xml | grep -c 'class="removed"'
3
```

## When the draft is wrong

Two broken copies show the two guards. In the first, an `xref` points
to `RFC6239`, which the draft does not have. The schema's key reference
from `xref/@target` to the anchors finds it, and nothing is written:

```console
$ sed 's/target="RFC6238"/target="RFC6239"/' vane.xml > broken.xml
$ sh publish.sh broken.xml; echo $?
broken.xml:98:38: cvc-identity-constraint.4.3: element 'xref': its key sequence is no key sequence of the referenced key (keyref 'xref-target')
1
```

In the second, the citation of RFC 6238 is gone and a `SHOULD` has lost
its markup. The copy is valid, and the check stops it:

```console
$ sed -e 's| <xref target="RFC6238"/>||' -e 's|<bcp14>SHOULD</bcp14> last|SHOULD last|' vane.xml > unmarked.xml
$ sh publish.sh unmarked.xml; echo $?
publish-check.xq:33:8: spec:check: references: [RFC6238] is never cited; pause: a key word without bcp14: "A pause SHOULD last five minutes."
1
```

The files in `site/` are still those of the last good run. Each step
adds one guarantee, and the stylesheets can assume what the steps before
them checked: that every target exists, that every key word is marked,
that the draft has the structure the schema describes.

## Summary

XSLT transforms documents by rules:

- template rules match nodes with patterns, and `xsl:apply-templates`
  moves the processing on; the best rule wins by import precedence and
  priority;
- modes process the same nodes in different ways, and a mode with
  `on-no-match="shallow-copy"` copies everything you don't change;
- keys index documents, `xsl:number` numbers sections and requirements
  by their position, and `xsl:for-each-group` groups by value or by
  position;
- result documents write many files, and imports, functions and
  packages organize large stylesheets that share their rules;
- a script of small checked steps turns one source into every form a
  document is read in.

XSLT can also transform files too large for memory, as they stream
past; [Streaming Large Trace Files](traces-streaming.md) teaches it on
the trace archives of the last project. Next, the diagram tool of
[Project: Drawing Diagrams with SVG](svg.md) puts XQuery and XSLT
together on a different kind of XML: pictures.
