# Where an XDMB blob's bytes go: header and tables, node records, text heap.
import sqlite3, sys
from orders import orders
db = sqlite3.connect(":memory:"); db.enable_load_extension(True); db.load_extension("./xml.so", entrypoint="sqlite3_xml_init")
def varint(b, i):
    v = s = 0
    while True:
        x = b[i]; i += 1; v |= (x & 0x7F) << s; s += 7
        if not x & 0x80: return v, i
totals = {"tables": 0, "records": 0, "text heap": 0, "nodes": 0, "whitespace-only text nodes": 0}
docs = orders(2000)
for t in docs:
    b = db.execute("select xmlparse(?)", (t,)).fetchone()[0]
    w = b[5]; nw, mw, pw = 1 << (w & 3), 1 << ((w >> 2) & 3), 1 << ((w >> 4) & 3)
    i = 6; fields = []
    for _ in range(7): v, i = varint(b, i); fields.append(v)
    nodes, heap = fields[0], fields[4]
    record = 1 + mw + 2 * nw + pw
    totals["records"] += nodes * record; totals["text heap"] += heap; totals["nodes"] += nodes
    totals["tables"] += len(b) - nodes * record - heap
    totals["whitespace-only text nodes"] += db.execute("select xmlquery('count(//text()[normalize-space() = \"\"])', ?)", (b,)).fetchone()[0]
total = totals["tables"] + totals["records"] + totals["text heap"]
for k, v in totals.items(): print(f"{k:28} {v:9}" + (f"  {v/total:.0%}" if k in ("tables", "records", "text heap") else ""))
print(f"bytes per record {totals['records']/totals['nodes']:.2f}")
