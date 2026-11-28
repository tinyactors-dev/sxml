"""xmltable: SQL/XML's XMLTABLE as an eponymous table-valued function, one
row per item of the row query, columns from up to eight paths.

    python3 xmltable_test.py

Each check prints and the script exits 1 when one fails.
"""
import sqlite3
import sys

db = sqlite3.connect(":memory:")
db.enable_load_extension(True)
db.load_extension("./xml.so", entrypoint="sqlite3_xml_init")
failures = 0


def run(sql, *arguments):
    try:
        return db.execute(sql, arguments).fetchall()
    except sqlite3.Error as error:
        return f"error: {error}"


def check(name, actual, expected):
    global failures
    ok = expected(actual) if callable(expected) else actual == expected
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {actual!r}")


order = ('<order id="7"><!--n--><line sku="KB" qty="2" price="10">Keyboard</line>'
         '<line sku="MS" qty="1" price="5"><note>gift</note></line></order>')

check("one row per item, positions from 1",
      run("select position, name, kind, path from xmltable('//line', ?)", order),
      [(1, "line", "element", "/Q{}order[1]/Q{}line[1]"), (2, "line", "element", "/Q{}order[1]/Q{}line[2]")])
check("value is the string value of a node, xml its serialization",
      run("select value, xml from xmltable('//line[1]', ?)", order),
      [("Keyboard", '<line sku="KB" qty="2" price="10">Keyboard</line>')])
check("columns: attributes as strings, numbers as numbers, elements as XML",
      run("select c1, c2, c3 from xmltable('//line', ?, '@sku', '@qty * @price', 'note')", order),
      [("KB", 20.0, None), ("MS", 5.0, "<note>gift</note>")])
check("a path is evaluated with the row's item as context",
      run("select c1 from xmltable('//line', ?, 'count(preceding-sibling::line)')", order), [(0,), (1,)])
check("atomic items are rows too", run("select position, value, kind from xmltable('1 to 3', ?)", order),
      [(1, 1, "atomic"), (2, 2, "atomic"), (3, 3, "atomic")])
check("attributes, comments and text as rows",
      run("select kind, value from xmltable('(/order/@id, /order/comment(), //line[1]/text())', ?)", order),
      [("attribute", "7"), ("comment", "n"), ("text", "Keyboard")])
check("an empty result is no rows", run("select count(*) from xmltable('//nothing', ?)", order), [(0,)])
check("a NULL doc is no rows", run("select count(*) from xmltable('//line', NULL)"), [(0,)])
check("XDMB input", run("select c1 from xmltable('//line', xmlparse(?), '@sku')", order), [("KB",), ("MS",)])
check("EXI input", run("select c1 from xmltable('//line', xmlexi(?), '@sku')", order), [("KB",), ("MS",)])
check("a row query that does not compile is an error",
      run("select * from xmltable('//line[', ?)", order), lambda r: isinstance(r, str) and "XPST0003" in r)
check("a path that does not compile is an error",
      run("select * from xmltable('//line', ?, '@')", order), lambda r: isinstance(r, str) and "XPST0003" in r)
check("a doc that is not XML is an error", run("select * from xmltable('//line', 'plain')"),
      "error: sxml: doc is not XML, XDMB or EXI")
check("the row query and doc are required", run("select * from xmltable"), lambda r: isinstance(r, str))

db.execute("create table orders(id integer primary key, doc blob)")
db.executemany("insert into orders(doc) values (xmlparse(?))", [
    (order,),
    ('<order id="8"><line sku="KB" qty="3" price="10"/></order>',),
    ('<order id="9"/>',),
])
check("a join: each order's lines, with the order's own query reused",
      run("select o.id, l.c1, l.c2 from orders o, xmltable('//line', o.doc, '@sku', '@qty * @price') l "
          "order by o.id, l.position"),
      [(1, "KB", 20.0), (1, "MS", 5.0), (2, "KB", 30.0)])
check("an aggregate over the join",
      run("select l.c1, sum(l.c2) from orders o, xmltable('//line', o.doc, '@sku', '@qty * @price') l "
          "group by l.c1 order by l.c1"),
      [("KB", 50.0), ("MS", 5.0)])
check("left join keeps an order without lines",
      run("select o.id, count(l.position) from orders o left join xmltable('//line', o.doc) l "
          "group by o.id order by o.id"),
      [(1, 2), (2, 1), (3, 0)])
check("the row query may change per row (the cache follows it)",
      run("select o.id, l.value from orders o, xmltable(case o.id when 1 then '//@sku' else '/order/@id' end, o.doc) l "
          "order by o.id, l.position"),
      [(1, "KB"), (1, "MS"), (2, "8"), (3, "9")])
check("it can be used in a view (innocuous)",
      run("create view lines as select o.id, l.c1 sku from orders o, xmltable('//line', o.doc, '@sku') l")
      or run("select count(*) from lines"), [(3,)])
print("xmltable:", "ok" if failures == 0 else f"{failures} failures")
sys.exit(1 if failures else 0)
