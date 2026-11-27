"""What it takes to query EXI in SQLite, next to text and XDMB.

    python3 bench_exi.py [COUNT]

The orders of orders.py stored as XML text, as XDMB (queried in place) and
as EXI in several alignments, each blob carrying its options in its header
(xml_exi), and as schema-informed EXI against orders.xsd, whose blobs carry
no options (the connection names them, xml_exi_schema). "full" keeps
comments, PIs and prefixes, as XDMB does; the other EXI forms keep EXI's
default fidelity. Every query runs over every form; the results must be equal.
Times are per row, the best of three runs.
"""
import os
import platform
import sqlite3
import sys
import time

from orders import orders

count = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
db = sqlite3.connect(":memory:")
db.enable_load_extension(True)
db.load_extension("./xml.so", entrypoint="sqlite3_xml_init")
texts = orders(count)

forms = {
    "text": None,
    "XDMB": "xmlb(doc)",
    "EXI bit-packed": "xml_exi(doc, 0)",
    "EXI byte-aligned": "xml_exi(doc, 1)",
    "EXI pre-compression": "xml_exi(doc, 2)",
    "EXI compression": "xml_exi(doc, 3)",
    "EXI bit-packed, full": "xml_exi(doc, 0, 1)",
    "EXI compression, full": "xml_exi(doc, 3, 1)",
    "EXI schema bit-packed": "xml_exi(doc, 0, 0, 1)",
    "EXI schema compression": "xml_exi(doc, 3, 0, 1)",
}
# Schema-informed streams carry no options: the connection names the schema
# and the alignment they are read with (xml_exi_schema).
schema_text = open("orders.xsd").read()
schema_alignment = {"EXI schema bit-packed": 0, "EXI schema compression": 3}


def use(name):
    if name in schema_alignment:
        db.execute("select xml_exi_schema(?, ?)", (schema_text, schema_alignment[name]))
tables = {}
db.execute("create table t0(id integer primary key, doc text)")
db.executemany("insert into t0(doc) values (?)", ((text,) for text in texts))
tables["text"] = "t0"
encode_times = {}
for number, (name, expression) in enumerate(forms.items()):
    if expression is None:
        continue
    table = f"t{number}"
    use(name)
    db.execute(f"create table {table}(id integer primary key, doc blob)")
    start = time.perf_counter()
    db.execute(f"insert into {table}(doc) select {expression} from t0")
    encode_times[name] = time.perf_counter() - start
    tables[name] = table

print(f"{count} orders; {platform.machine()}, {os.cpu_count()} CPUs; SQLite {sqlite3.sqlite_version}")
text_bytes = db.execute("select sum(length(cast(doc as blob))) from t0").fetchone()[0]
print(f"\n{'form':24} {'bytes':>12} {'vs text':>8} {'encode us/row':>14}")
for name, table in tables.items():
    size = db.execute(f"select sum(length(cast(doc as blob))) from {table}").fetchone()[0]
    encode = f"{encode_times[name] * 1e6 / count:14.1f}" if name in encode_times else f"{'':>14}"
    print(f"{name:24} {size:12,} {size / text_bytes:8.2f} {encode}")

queries = {
    "open only": "select count(*) from {t} where xml_exists(doc, '/')",
    "revenue": "select sum(xml_xpath(doc, 'sum(//*:line/(@qty * @price))')) from {t}",
    "filter": "select count(*) from {t} where xml_exists(doc, '/*:order[@status = \"refunded\"][*:shipping/@country = \"EE\"]')",
    "extract": "select count(xml_xpath(doc, 'string(/*:order/*:customer/*:name)')) from {t}",
    "xquery": "select count(xml_query(doc, 'declare default element namespace \"urn:example:orders\"; <summary id=\"{{/order/@id}}\">{{count(//line)}}</summary>')) from {t}",
    "last line": "select count(xml_xpath(doc, '(//*:line)[last()]/@sku/string()')) from {t}",
}
names = list(tables)
print(f"\nus per row, best of 3")
print(f"{'query':10} " + " ".join(f"{name:>12.12}" for name in names))
for query, sql in queries.items():
    results, times = [], []
    for name in names:
        use(name)
        best = None
        for _ in range(3):
            start = time.perf_counter()
            value = db.execute(sql.format(t=tables[name])).fetchone()[0]
            elapsed = time.perf_counter() - start
            best = elapsed if best is None else min(best, elapsed)
        results.append(value)
        times.append(best)
    assert len(set(results)) == 1, (query, dict(zip(names, results)))
    print(f"{query:10} " + " ".join(f"{elapsed * 1e6 / count:12.1f}" for elapsed in times))
