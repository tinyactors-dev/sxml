"""XDMB version 2, with the gaps of version 1 closed: does a blob give back
everything the tree had?

    python3 fidelity_test.py [SAMPLE]

1. Typed documents: the schema/instance pairs of tests/exi (60 pairs from
   the W3C XSD test suites) and the orders of orders.py against
   orders.xsd (inline types) and orders-named.xsd (named types). Each
   valid instance is queried twice, validated in memory (xmlquery_validated(q, doc)) and as typed XDMB (xmlparse(doc, 'validate')): every element's and
   attribute's path, type annotation, and typed value with the type of
   each item. Counted apart: documents whose blob holds an approximated
   annotation (an anonymous type of a local declaration, written as its
   named base type).
2. Locations and unparsed entities: SAMPLE documents of third_party (the
   differential test's corpus): fn:location of every node, text against
   xmlparse(doc, 'locations'), and each blob re-encoded must be the same bytes,
   which it can only be when the unparsed entities and locations come back
   out of the tree it was opened as.
"""
import glob
import os
import random
import sqlite3
import sys

from orders import orders

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
db = sqlite3.connect(":memory:")
db.enable_load_extension(True)
db.load_extension("./xml.so", entrypoint="sqlite3_xml_init")

TYPED = (
    'string-join(for $n in (//* | //@*) return concat(path($n), " ", node-type-annotation($n)?name, " ",'
    ' try { string-join(data($n) ! (string(.) || "/" || atomic-type-annotation(.)?name), ",") }'
    ' catch * { "no typed value" }), "&#10;")'
)


def typed(cases, label):
    valid = equal = approximate_documents = approximate_equal = not_compiled = 0
    for schema, text in cases:
        try:
            db.execute("select xmlschema(?)", (schema,))
        except sqlite3.Error:
            not_compiled += 1  # includes or redefines a file: the spike compiles from text alone
            continue
        try:
            reference = db.execute("select xmlquery_validated(?2, ?1)", (text, TYPED)).fetchone()[0]
        except sqlite3.Error:
            continue  # not valid against its schema
        valid += 1
        blob = db.execute("select xmlparse(?, 'validate')", (text,)).fetchone()[0]
        approximated = db.execute("select xmlparse_approximate()").fetchone()[0]
        got = db.execute("select xmlquery(?2, ?1)", (blob, TYPED)).fetchone()[0]
        same = got == reference
        if approximated:
            approximate_documents += 1
            approximate_equal += same
        else:
            equal += same
            if not same:
                print(f"  DIFFERS without approximation: {label}")
    exact = valid - approximate_documents
    print(f"{label}: {not_compiled} schemas not compiled here; {valid} valid documents; exact annotations: {equal} of {exact} equal; "
          f"with approximated annotations: {approximate_documents}, of which {approximate_equal} still equal")
    return exact - equal


failures = 0
rows = [line.rstrip("\n").split("\t") for line in open(os.path.join(ROOT, "tests/exi/manifest.tsv"))
        if not line.startswith("#")]
pairs = [(row[3].split("=", 1)[1], row[2]) for row in rows if row[0] == "schema"]
failures += typed([(open(os.path.join(ROOT, schema)).read(), open(os.path.join(ROOT, instance)).read())
                   for schema, instance in pairs], "XSD test suite pairs")
order_texts = orders(200)
failures += typed([(open("orders.xsd").read(), text) for text in order_texts], "orders, inline types")
failures += typed([(open("orders-named.xsd").read(), text) for text in order_texts], "orders, named types")

LOCATIONS = ("string-join(for $n in (//node(), //@*) return "
             "serialize(location($n), {'method': 'adaptive'}), ' ')")
files = sorted(glob.glob(os.path.join(ROOT, "third_party") + "/**/*.xml", recursive=True))
random.seed(1)
random.shuffle(files)
documents = with_entities = location_failures = unstable = 0
for path in files[: int(sys.argv[1]) if len(sys.argv) > 1 else 3000]:
    raw = open(path, "rb").read()
    if len(raw) > 200_000:
        continue
    try:
        text = raw.decode("utf-8")
        blob = db.execute("select xmlparse(?, 'locations')", (text,)).fetchone()[0]
    except Exception:
        continue
    documents += 1
    with_entities += b"NDATA" in raw
    if db.execute("select xmlparse(?, 'locations') = ?", (blob, blob)).fetchone()[0] != 1:
        unstable += 1
        print("  NOT STABLE", path)
    if db.execute("select xmlquery(?2, ?1)", (text, LOCATIONS)).fetchone()[0] != \
            db.execute("select xmlquery(?2, ?1)", (blob, LOCATIONS)).fetchone()[0]:
        location_failures += 1
        if location_failures < 5:
            print("  LOCATIONS DIFFER", path)
print(f"third_party: {documents} documents ({with_entities} declaring unparsed entities); "
      f"locations differ in {location_failures}; blobs not re-encoded identically: {unstable}")
failures += location_failures + unstable
print("fidelity:", "ok" if failures == 0 else f"{failures} failures")
sys.exit(1 if failures else 0)
