import sqlite3, time, zlib, sys, platform, os
from orders import orders
count = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
db = sqlite3.connect(":memory:")
db.enable_load_extension(True)
db.load_extension("./xml.so", entrypoint="sqlite3_xml_init")
texts = orders(count)
db.execute("create table orders_text(id integer primary key, doc text)")
db.execute("create table orders_blob(id integer primary key, doc blob)")
db.executemany("insert into orders_text(doc) values (?)", ((t,) for t in texts))
t0 = time.perf_counter()
db.execute("insert into orders_blob(doc) select xmlparse(doc) from orders_text")
ingest = time.perf_counter() - t0
def size(table): return db.execute(f"select sum(length(cast(doc as blob))) from {table}").fetchone()[0]
first = 2000
sample_text = sum(len(t.encode()) for t in texts[:first])
sample_blob = db.execute("select sum(length(doc)) from (select doc from orders_blob order by id limit ?)", (first,)).fetchone()[0]
print(f"{count} orders; machine {platform.machine()} {os.cpu_count()} CPUs, SQLite {sqlite3.sqlite_version}")
print(f"size, all: text {size('orders_text')} B, XDMB {size('orders_blob')} B ({size('orders_blob')/size('orders_text'):.2f}x)")
print(f"size, first {first}: text {sample_text}, XDMB {sample_blob}, deflate per document {sum(len(zlib.compress(t.encode(), 9)) for t in texts[:first])}")
print(f"xmlparse() of all: {ingest*1000:.0f} ms")
queries = {
  "revenue": ("select sum(xmlquery('sum(//*:line/(@qty * @price))', doc)) from {t}", ),
  "filter":  ("select count(*) from {t} where xmlexists('/*:order[@status = \"refunded\"][*:shipping/@country = \"EE\"]', doc)", ),
  "extract": ("select count(xmlquery('string(/*:order/*:customer/*:name)', doc)) from {t}", ),
  "xquery":  ("select count(xmlquery('declare default element namespace \"urn:example:orders\"; <summary id=\"{{/order/@id}}\">{{count(//line)}}</summary>', doc)) from {t}", ),
  "last line": ("select count(xmlquery('(//*:line)[last()]/@sku/string()', doc)) from {t}", ),
}
for name, (sql,) in queries.items():
    results, times = [], []
    for table in ("orders_text", "orders_blob"):
        best = None
        for _ in range(3):
            t0 = time.perf_counter(); value = db.execute(sql.format(t=table)).fetchone()[0]; elapsed = time.perf_counter() - t0
            best = elapsed if best is None else min(best, elapsed)
        results.append(value); times.append(best)
    assert results[0] == results[1], (name, results)
    print(f"{name:10} text {times[0]*1e6/count:7.1f} us/row  XDMB {times[1]*1e6/count:7.1f} us/row  ({times[0]/times[1]:.1f}x)  = {results[1]}")
