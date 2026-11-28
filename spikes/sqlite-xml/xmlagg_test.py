"""xmlagg (SQL/XML XMLAGG), as an aggregate and as a window function, and
xmlconcat (XMLCONCAT).

    python3 xmlagg_test.py

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


db.execute("create table orders(id integer primary key, customer text, doc blob)")
db.executemany("insert into orders(customer, doc) values (?, ?)", [
    ("ann", '<order id="1"><line sku="KB" qty="2" price="10"/></order>'),
    ("bob", '<order id="2"><line sku="MS" qty="1" price="5"/></order>'),
    ("ann", None),
    ("ann", '<order id="4"><line sku="KB" qty="1" price="10"/><line sku="PD" qty="4" price="3"/></order>'),
])
db.execute("update orders set doc = xmlparse(doc) where id = 2")  # XDMB and text side by side

check("the result is an XDMB document", run("select substr(xmlagg(doc), 1, 4) from orders"), [(b"XDMB",)])
check("ORDER BY inside the aggregate; NULLs skipped",
      run("select xmlquery('string-join(*/@id, \",\")', xmlagg(doc order by id desc)) from orders"), [("4,2,1",)])
check("GROUP BY, wrapped in an element by xmlquery",
      run("select customer, xmlserialize(xmlquery('<orders n=\"{count(*)}\">{*/@id ! string()}</orders>', "
          "xmlagg(doc order by id))) from orders group by customer order by customer"),
      [("ann", '<orders n="2">1 4</orders>'), ("bob", '<orders n="1">2</orders>')])
check("a query over the group", run("select xmlquery('sum(//line/(@qty * @price))', xmlagg(doc)) from orders"),
      [(47.0,)])
check("no rows, or only NULLs, is NULL",
      run("select xmlagg(doc), (select xmlagg(doc) from orders where doc is null) from orders where 0"),
      [(None, None)])
check("xmlquery's nodes aggregate without separators",
      run("select xmlquery('string-join(line/@sku)', xmlagg(xmlquery('//line', doc) order by id)) from orders "
          "where doc is not null"),
      [("KBMSKBPD",)])
check("several nodes from xmlquery are content",
      run("select xmlquery('count(*)', xmlagg(xmlquery('//line', doc))) from orders "
          "where doc is not null"), [(4,)])
check("text is content: text nodes, fragments",
      run("select xmlserialize(xmlagg(x)) from (select 'a' x union all select '<b/>c' union all select 7)"),
      [("a<b/>c7",)])
check("adjacent text nodes merge, as in a document constructor",
      run("select xmlquery('count(node())', xmlagg(x)) from (select 'a' x union all select 'b')"), [(1,)])
check("a document with a prolog", run("select xmlserialize(xmlagg(x)) from (select '<?xml version=\"1.0\"?>"
                                      "<!-- c --><a/>' x)"), [("<!-- c --><a/>",)])
check("EXI input", run("select xmlserialize(xmlagg(xmlexi(xmlserialize(doc), 0) order by id)) from orders "
                       "where id < 3"),
      [('<order id="1"><line sku="KB" qty="2" price="10"/></order><order id="2"><line sku="MS" qty="1" price="5"/>'
        '</order>',)])
check("what is not XML is an error", run("select xmlagg(x) from (select '<a>' x)"),
      lambda r: isinstance(r, str) and "not well-formed" in r)

IDS = 'string-join(*/@id, ",")'
check("window: a running aggregate",
      run(f"select id, xmlquery('{IDS}', xmlagg(doc) over (order by id)) from orders"),
      [(1, "1"), (2, "1,2"), (3, "1,2"), (4, "1,2,4")])
check("window: a sliding frame (xInverse), NULL rows in it",
      run(f"select id, xmlquery('{IDS}', xmlagg(doc) over (order by id rows 1 preceding)) from orders"),
      [(1, "1"), (2, "1,2"), (3, "2"), (4, "4")])
check("window: a frame that empties is NULL",
      run("select id, xmlagg(doc) over (order by id rows between current row and current row) is null "
          "from orders"), [(1, 0), (2, 0), (3, 1), (4, 0)])
check("window: PARTITION BY",
      run(f"select id, xmlquery('{IDS}', xmlagg(doc) over (partition by customer order by id)) from orders "
          "order by id"), [(1, "1"), (2, "2"), (3, "1"), (4, "1,4")])
check("window: a frame that starts later than the first row",
      run("select id, xmlquery(?, x) from (select id, xmlagg(doc) over (order by id rows between 1 following "
          "and 2 following) x from orders) where x is not null", IDS), [(1, "2"), (2, "4"), (3, "4")])

check("xmlconcat: NULLs skipped", run("select xmlserialize(xmlconcat('<a/>', null, 'x', ?))",
                                      db.execute("select xmlparse('<b/>')").fetchone()[0]), [("<a/>x<b/>",)])
check("xmlconcat: only NULLs is NULL", run("select xmlconcat(null, null)"), [(None,)])

check("xmlquery: several elements are written with no separator",
      run("select xmlquery('//line', doc) from orders where id = 4"),
      [('<line sku="KB" qty="1" price="10"/><line sku="PD" qty="4" price="3"/>',)])
check("xmlquery: attributes and atomic items keep the adaptive method",
      run("select xmlquery('//line/@sku', doc), xmlquery('(1, 2)', doc) from orders where id = 4"),
      [('sku="KB"\nsku="PD"', "1\n2")])
print("xmlagg:", "ok" if failures == 0 else f"{failures} failures")
sys.exit(1 if failures else 0)
