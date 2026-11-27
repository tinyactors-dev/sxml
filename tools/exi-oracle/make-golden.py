#!/usr/bin/env python3
"""Makes the EXI golden corpus (tests/exi/README.md) with the EXI oracle.

    tools/exi-oracle/make-golden.py [--sxml=PATH]

Deterministic: the same third_party commits and handmade files give the
same corpus. Steps:

1. Select documents: tests/exi/handmade, then a fixed sample of the
   vendored suites (sorted, every Nth file, size and content limits).
2. Normalize each into tests/exi/inputs/<id>.xml with the sxml command:
   UTF-8, no DTD, and no whitespace-only text node outside xml:space
   "preserve", because EXIficient's SAX encoder drops those and the
   encoder comparison needs both encoders to see the same XDM.
3. Cross the documents with the option sets below into
   tests/exi/manifest.tsv.
4. Run the oracle; copy each stream and its decoding to
   tests/exi/golden/<set>/<id>.exi and .xml, and record EXIficient's
   refusals in the manifest's status column.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TESTS = os.path.join(ROOT, "tests", "exi")

FULL = "preserve=comments,pis,prefixes"

# Option sets for schema-less documents: (name, options).
SCHEMA_LESS_SETS = [
    ("bit-packed", ""),
    ("bit-packed-full", FULL),
    ("byte-alignment-full", "alignment=byte-alignment;" + FULL),
    ("pre-compression", "alignment=pre-compression"),
    ("pre-compression-full", "alignment=pre-compression;" + FULL),
    ("pre-compression-blocks", "alignment=pre-compression;block-size=64"),
    ("compression", "alignment=compression"),
    ("compression-full", "alignment=compression;" + FULL),
    ("compression-blocks", "alignment=compression;block-size=64;" + FULL),
    ("header-options", "header=cookie,options;" + FULL),
    ("string-limits", "value-max-length=6;value-partition-capacity=10"),
    ("string-limits-zero", "value-partition-capacity=0;" + FULL),
    ("fragment", "fragment;" + FULL),
    # ELEMENTS: the document's most frequent element name (self_contained_names).
    ("self-contained", "self-contained=ELEMENTS;" + FULL),
    ("self-contained-byte-alignment", "self-contained=ELEMENTS;alignment=byte-alignment"),
    ("xsd-types-only", "xsd-types-only;" + FULL),
    ("canonical", "canonical;" + FULL),
    ("canonical-byte-alignment", "canonical;alignment=byte-alignment;" + FULL),
    ("canonical-compression", "canonical;alignment=compression;" + FULL),
]

# Option sets for documents with a schema; SCHEMA is replaced.
SCHEMA_SETS = [
    ("schema", "schema=SCHEMA"),
    ("schema-full", "schema=SCHEMA;" + FULL),
    ("schema-strict", "schema=SCHEMA;strict"),
    ("schema-lexical", "schema=SCHEMA;preserve=lexical-values"),
    ("schema-byte-alignment", "schema=SCHEMA;alignment=byte-alignment"),
    ("schema-compression", "schema=SCHEMA;alignment=compression;" + FULL),
    ("schema-header", "schema=SCHEMA;header=options,schema-id"),
    ("schema-canonical", "schema=SCHEMA;canonical"),
    ("schema-canonical-utc", "schema=SCHEMA;canonical;utc-time"),
]

# Documents of the vendored suites: (directory, take every Nth, limit).
SAMPLES = [
    ("third_party/qt4tests/docs", 1, 40),
    ("third_party/qt4tests/fn", 7, 30),
    ("third_party/xslt40-test/tests/misc", 23, 30),
    ("third_party/xmlconf/eduni/namespaces/1.0", 3, 20),
    ("third_party/xmlconf/xmltest/valid/sa/out", 4, 20),
    ("third_party/html5lib-tests", 1, 0),
]

MAXIMUM_BYTES = 40_000
NORMALIZE = (
    "copy $document := . modify delete node $document//text()[normalize-space() = '']"
    "[not((ancestor::*[@xml:space][1]/@xml:space) = 'preserve')] return $document"
)


def run(command, **keywords):
    return subprocess.run(command, check=False, capture_output=True, **keywords)


def normalize(sxml, source, target):
    """Writes source, normalized, to target; False when sxml refuses it."""
    with open(source, "rb") as handle:
        head = handle.read(4096)
    if b"<!DOCTYPE" in head or b"<!ENTITY" in head:
        return False
    result = run([sxml, "xquery", "--context-item=" + source, "--method=xml", "--expression=" + NORMALIZE])
    if result.returncode != 0 or not result.stdout.strip():
        return False
    with open(target, "wb") as handle:
        handle.write(result.stdout)
    return True


def sample(directory, step, limit):
    paths = []
    for base, _, files in os.walk(os.path.join(ROOT, directory)):
        for name in files:
            if name.endswith(".xml"):
                path = os.path.join(base, name)
                if os.path.getsize(path) <= MAXIMUM_BYTES:
                    paths.append(path)
    paths.sort()
    paths = paths[::step]
    return paths[:limit] if limit else []


def identifier(path):
    relative = os.path.relpath(path, ROOT)
    stem = re.sub(r"[^A-Za-z0-9]+", "-", os.path.splitext(relative)[0]).strip("-").lower()
    return stem[-48:].strip("-") + "-" + hashlib.sha256(relative.encode()).hexdigest()[:6]


def schema_pairs():
    """Schema and valid instance pairs of the XSD 1.0 suite: one schema
    document, both expected valid, every 9th group, at most 60."""
    pairs = []
    meta = os.path.join(ROOT, "third_party", "xsdtests", "msMeta")
    xlink = "{http://www.w3.org/1999/xlink}href"
    namespace = "{http://www.w3.org/XML/2004/xml-schema-test-suite/}"
    for name in sorted(os.listdir(meta)):
        if not name.endswith("_w3c.xml"):
            continue
        tree = ElementTree.parse(os.path.join(meta, name))
        for group in tree.iter(namespace + "testGroup"):
            schema = group.find(namespace + "schemaTest")
            instance = group.find(namespace + "instanceTest")
            if schema is None or instance is None:
                continue
            documents = schema.findall(namespace + "schemaDocument")
            if len(documents) != 1:
                continue
            if schema.find(namespace + "expected").get("validity") != "valid":
                continue
            if instance.find(namespace + "expected").get("validity") != "valid":
                continue
            schema_path = os.path.normpath(os.path.join(meta, documents[0].get(xlink)))
            instance_path = os.path.normpath(os.path.join(meta, instance.find(namespace + "instanceDocument").get(xlink)))
            if os.path.exists(schema_path) and os.path.exists(instance_path):
                pairs.append((schema_path, instance_path))
    return pairs[::9][:60]


def self_contained_names(path):
    """The element name a self-contained set makes self-contained in the
    document at path, as {uri}local: its most frequent element name, the
    first in document order among equals (the root's when it has no other)."""
    counts = {}
    for element in ElementTree.parse(os.path.join(ROOT, path)).iter():
        if isinstance(element.tag, str):
            counts[element.tag] = counts.get(element.tag, 0) + 1
    tags = list(counts)
    if len(tags) > 1:
        tags = tags[1:]
    return max(tags, key=lambda tag: (counts[tag], -tags.index(tag)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sxml", default=os.environ.get("SXML", os.path.join(ROOT, "build", "release", "apps", "sxml")))
    arguments = parser.parse_args()
    inputs = os.path.join(TESTS, "inputs")
    golden = os.path.join(TESTS, "golden")
    for directory in (inputs, golden):
        shutil.rmtree(directory, ignore_errors=True)
        os.makedirs(directory)

    documents = []  # (id, normalized input relative to ROOT, schema or None)
    handmade = os.path.join(TESTS, "handmade")
    sources = [os.path.join(handmade, name) for name in sorted(os.listdir(handmade)) if name.endswith(".xml")]
    for directory, step, limit in SAMPLES:
        sources += sample(directory, step, limit)
    seen = set()
    for source in sources:
        case = identifier(source)
        target = os.path.join(inputs, case + ".xml")
        if case not in seen and normalize(arguments.sxml, source, target):
            seen.add(case)
            documents.append((case, os.path.relpath(target, ROOT), None))
    for schema, instance in schema_pairs():
        case = identifier(instance)
        target = os.path.join(inputs, case + ".xml")
        if case not in seen and normalize(arguments.sxml, instance, target):
            seen.add(case)
            documents.append((case, os.path.relpath(target, ROOT), os.path.relpath(schema, ROOT)))

    rows = []  # (set, id, input, options)
    for case, path, schema in documents:
        if schema is None:
            for name, options in SCHEMA_LESS_SETS:
                if "ELEMENTS" in options:
                    options = options.replace("ELEMENTS", self_contained_names(path))
                rows.append((name, case, path, options))
        else:
            for name, options in SCHEMA_SETS:
                rows.append((name, case, path, options.replace("SCHEMA", schema)))

    with tempfile.TemporaryDirectory() as scratch:
        manifest = os.path.join(scratch, "manifest.tsv")
        with open(manifest, "w") as handle:
            for name, case, path, options in rows:
                handle.write(f"{name}/{case}\t{os.path.join(ROOT, path)}\t{options}\n")
        output = os.path.join(scratch, "out")
        oracle = run([os.path.join(ROOT, "tools", "exi-oracle", "oracle.sh"), "batch", manifest, output], cwd=ROOT)
        if oracle.returncode != 0:
            sys.stderr.write(oracle.stderr.decode())
            sys.exit(1)
        status = {}
        with open(os.path.join(output, "results.txt")) as handle:
            for line in handle:
                fields = line.rstrip("\n").split("\t")
                status[fields[0]] = fields[1] if fields[1] == "ok" else "refused: " + fields[2][:200]
        written = 0
        decoded = {}
        os.makedirs(os.path.join(golden, "decoded"), exist_ok=True)
        for name, case, _, _ in rows:
            key = f"{name}/{case}"
            if status.get(key) == "ok":
                os.makedirs(os.path.join(golden, name), exist_ok=True)
                shutil.copyfile(os.path.join(output, key + ".exi"), os.path.join(golden, key + ".exi"))
                # Decodings repeat across alignments: stored once, by content.
                with open(os.path.join(output, key + ".xml"), "rb") as handle:
                    content = handle.read()
                digest = hashlib.sha256(content).hexdigest()[:16]
                with open(os.path.join(golden, "decoded", digest + ".xml"), "wb") as handle:
                    handle.write(content)
                decoded[key] = f"tests/exi/golden/decoded/{digest}.xml"
                written += 1

    with open(os.path.join(TESTS, "manifest.tsv"), "w") as handle:
        handle.write("# Generated by tools/exi-oracle/make-golden.py; do not edit.\n")
        handle.write("# set\tid\tinput\toptions\toracle\tdecoded\n")
        for name, case, path, options in rows:
            key = name + "/" + case
            handle.write(f"{name}\t{case}\t{path}\t{options}\t{status.get(key, 'missing')}\t{decoded.get(key, '')}\n")
    print(f"{len(documents)} documents, {len(rows)} cases, {written} golden streams")


if __name__ == "__main__":
    main()
