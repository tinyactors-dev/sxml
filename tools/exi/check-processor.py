#!/usr/bin/env python3
"""The acceptance check of packet 8.13 (docs/tasks/phase-8.md; D333): EXI
through the processor, checked with the sxml command only, against the
golden streams of tests/exi (which EXIficient wrote).

    tools/exi/check-processor.py SXML [--sample=N]

SXML is a built sxml command. For a sample of each schema-less set it
checks, with XQuery run by `sxml xquery`:

- exi:serialize($document, {'options': <header .../>, ...}) is the golden
  stream byte for byte (compression: exi:parse of it gives the golden
  decoding instead);
- exi:parse(file:read-binary($golden), {'options': <header .../>}) is
  deep-equal to EXIficient's decoding;
- the output method exi:exi writes the golden default stream, from an
  XQuery output declaration (sxml xquery -o) and from xsl:output (sxml
  transform -o);
- fn:serialize with the exi method raises SEPM0016.

The options are the W3C EXI options document (EXI 1.0, Appendix C) in
namespace http://www.w3.org/2009/exi, built from each case's option
specification (tests/exi/README.md, "Options").
"""
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SETS = ["bit-packed", "bit-packed-full", "byte-alignment-full", "pre-compression", "pre-compression-full",
        "pre-compression-blocks", "compression", "compression-full", "compression-blocks",
        "string-limits", "string-limits-zero", "fragment"]


def header_element(specification):
    """The EXI options document of an option specification, and the
    other entries of the options map."""
    options = {}
    for part in specification.split(";"):
        if part:
            key, _, value = part.partition("=")
            options[key] = value
    uncommon = ""
    alignment = options.get("alignment", "bit-packed")
    if alignment == "byte-alignment":
        uncommon += "<alignment><byte/></alignment>"
    elif alignment == "pre-compression":
        uncommon += "<alignment><pre-compress/></alignment>"
    if "value-max-length" in options:
        uncommon += f"<valueMaxLength>{options['value-max-length']}</valueMaxLength>"
    if "value-partition-capacity" in options:
        uncommon += f"<valuePartitionCapacity>{options['value-partition-capacity']}</valuePartitionCapacity>"
    preserve = ""
    preserved = options.get("preserve", "").split(",")
    for item, element in (("dtd", "dtd"), ("prefixes", "prefixes"), ("lexical-values", "lexicalValues"),
                          ("comments", "comments"), ("pis", "pis")):
        if item in preserved:
            preserve += f"<{element}/>"
    lesscommon = ""
    if uncommon:
        lesscommon += f"<uncommon>{uncommon}</uncommon>"
    if preserve:
        lesscommon += f"<preserve>{preserve}</preserve>"
    if "block-size" in options:
        lesscommon += f"<blockSize>{options['block-size']}</blockSize>"
    common = ""
    if alignment == "compression":
        common += "<compression/>"
    if "fragment" in options:
        common += "<fragment/>"
    body = ""
    if lesscommon:
        body += f"<lesscommon>{lesscommon}</lesscommon>"
    if common:
        body += f"<common>{common}</common>"
    if "strict" in options:
        body += "<strict/>"
    header = f'<header xmlns="http://www.w3.org/2009/exi">{body}</header>'
    extra = []
    header_items = options.get("header", "").split(",")
    if "cookie" in header_items:
        extra.append("'include-cookie': true()")
    if "options" in header_items:
        extra.append("'include-options': true()")
    return header, extra, options


class Checker:
    def __init__(self, sxml):
        self.sxml = sxml
        self.failures = 0
        self.count = 0

    def xquery(self, query, *arguments):
        return subprocess.run([self.sxml, "xquery", "--allow-file-access", "--method=text", *arguments,
                               f"--expression={query}"], capture_output=True, text=True, cwd=ROOT)

    def record(self, name, passed, detail=""):
        self.count += 1
        if not passed:
            self.failures += 1
            print(f"FAIL {name}: {detail.strip()[:300]}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sxml")
    parser.add_argument("--sample", type=int, default=6)
    arguments = parser.parse_args()
    checker = Checker(os.path.abspath(arguments.sxml))
    rows = [line.rstrip("\n").split("\t") for line in open(os.path.join(ROOT, "tests", "exi", "manifest.tsv"))
            if not line.startswith("#")]
    for name in SETS:
        cases = [row for row in rows if row[0] == name and row[4] == "ok"][:arguments.sample]
        for row in cases:
            header, extra, options = header_element(row[3])
            entries = ", ".join([f"'options': {header}"] + extra)
            golden = f"tests/exi/golden/{row[0]}/{row[1]}.exi"
            with open(os.path.join(ROOT, golden), "rb") as handle:
                expected = handle.read().hex().upper()
            fidelity = options.get("preserve", "").split(",")
            comparison = ("{'comments': %s, 'processing-instructions': %s, 'namespace-prefixes': %s}" %
                          tuple("true()" if item in fidelity else "false()" for item in ("comments", "pis", "prefixes")))
            if options.get("alignment") == "compression":
                query = (f"deep-equal(exi:parse(exi:serialize(doc('{row[2]}'), {{{entries}}}), {{'options': {header}}}), "
                         f"doc('{row[5]}'), {comparison})")
                result = checker.xquery(query)
                checker.record(f"{name}/{row[1]}/serialize", result.stdout.strip() == "true", result.stderr or result.stdout)
            else:
                query = f"string(xs:hexBinary(exi:serialize(doc('{row[2]}'), {{{entries}}})))"
                result = checker.xquery(query)
                checker.record(f"{name}/{row[1]}/serialize", result.stdout.strip() == expected,
                               result.stderr or f"got {result.stdout.strip()[:80]}, expected {expected[:80]}")
            query = (f"deep-equal(exi:parse(file:read-binary('{golden}'), {{'options': {header}}}), "
                     f"doc('{row[5]}'), {comparison})")
            result = checker.xquery(query)
            checker.record(f"{name}/{row[1]}/parse", result.stdout.strip() == "true", result.stderr or result.stdout)

    # The output method, from XQuery and XSLT, with the default options.
    default = [row for row in rows if row[0] == "bit-packed" and row[4] == "ok"][:3]
    with tempfile.TemporaryDirectory() as scratch:
        for row in default:
            with open(os.path.join(ROOT, f"tests/exi/golden/bit-packed/{row[1]}.exi"), "rb") as handle:
                expected = handle.read()
            output = os.path.join(scratch, row[1] + ".exi")
            result = subprocess.run([checker.sxml, "xquery", f"--context-item={row[2]}", f"--output={output}",
                                     "--expression=declare option output:method 'exi:exi'; ."],
                                    capture_output=True, text=True, cwd=ROOT)
            written = open(output, "rb").read() if os.path.exists(output) else b""
            checker.record(f"xquery-method/{row[1]}", written == expected, result.stderr or "bytes differ")
            stylesheet = os.path.join(scratch, "copy.xsl")
            with open(stylesheet, "w") as handle:
                handle.write('<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" '
                             'xmlns:exi="urn:sxml:exi"><xsl:output method="exi:exi"/>'
                             '<xsl:template match="/"><xsl:copy-of select="."/></xsl:template></xsl:stylesheet>')
            output = os.path.join(scratch, row[1] + ".xslt.exi")
            result = subprocess.run([checker.sxml, "transform", f"--output={output}", stylesheet, row[2]],
                                    capture_output=True, text=True, cwd=ROOT)
            written = open(output, "rb").read() if os.path.exists(output) else b""
            checker.record(f"xslt-method/{row[1]}", written == expected, result.stderr or "bytes differ")
    result = checker.xquery("serialize(<a/>, {'method': QName('urn:sxml:exi', 'exi')})")
    checker.record("fn-serialize-refuses", result.returncode != 0 and "SEPM0016" in result.stderr, result.stderr or result.stdout)
    print(f"check-processor: {checker.count} checks, {checker.failures} failed")
    sys.exit(1 if checker.failures else 0)


if __name__ == "__main__":
    main()
