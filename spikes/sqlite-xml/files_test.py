"""xmlfiles: the files a query may read, kept in a shadow table as FTS5
keeps its own, and read by the three-argument xmlquery and xmlexists.

    python3 files_test.py

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


db.execute("create virtual table docs using xmlfiles")
db.executemany("insert into docs(uri, content) values (?, ?)", [
    ("rates.xml", '<rates><rate cur="EUR">1.0</rate><rate cur="USD">1.08</rate></rates>'),
    ("lib/tax.xqm", 'module namespace tax = "urn:example:tax"; import module namespace r = "urn:example:rate" '
                    'at "rate.xqm"; declare function tax:vat($x) { $x * r:vat() };'),
    ("lib/rate.xqm", 'module namespace r = "urn:example:rate"; declare function r:vat() { 0.2 };'),
    ("note.txt", "plain text"),
    ("schemas/order.xsd", '<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"><xs:element name="order">'
                          '<xs:complexType><xs:sequence><xs:element name="line" maxOccurs="unbounded">'
                          '<xs:complexType><xs:attribute name="qty" type="xs:integer"/>'
                          '<xs:attribute name="price" type="xs:decimal"/></xs:complexType></xs:element>'
                          '</xs:sequence></xs:complexType></xs:element></xs:schema>'),
])
db.execute("insert into docs(uri, content) values ('rates.xdmb', xmlparse('<rates><rate cur=\"GBP\">0.86</rate></rates>'))")
order = '<order><line qty="2" price="10"/><line qty="1" price="5"/></order>'

check("the shadow table exists", run("select name from sqlite_schema where name like 'docs%' order by name"),
      [("docs",), ("docs_content",)])
check("lookup by uri", run("select uri from docs where uri = 'note.txt'"), [("note.txt",)])
check("fn:doc reads a row", run("select xmlquery('sum(doc(\"rates.xml\")//rate)', ?, 'docs')", order), [(2.08,)])
check("fn:doc reads an XDMB row", run("select xmlquery('string(doc(\"rates.xdmb\")//rate/@cur)', ?, 'docs')", order),
      [("GBP",)])
check("module import, and a module's relative import (lib/rate.xqm)",
      run("select xmlquery('import module namespace tax = \"urn:example:tax\" at \"lib/tax.xqm\"; "
          "tax:vat(sum(//line/(@qty * @price)))', ?, 'docs')", order), [(5.0,)])
check("fn:unparsed-text reads a row", run("select xmlquery('unparsed-text(\"note.txt\")', ?, 'docs')", order),
      [("plain text",)])
check("schema import and validate", run(
    "select xmlquery('import schema default element namespace \"\" at \"schemas/order.xsd\"; "
    "let $v := validate { . } return sum($v//line/(@qty * @price)) instance of xs:decimal', ?, 'docs')", order),
      [(1,)])
check("a missing row is FODC0002", run("select xmlquery('doc(\"nope.xml\")', ?, 'docs')", order),
      lambda r: isinstance(r, str) and "FODC0002" in r)
check("without the third argument nothing is read", run("select xmlquery('doc(\"rates.xml\")', ?)", order),
      lambda r: isinstance(r, str) and "FODC0002" in r)
check("an unknown files table is refused", run("select xmlquery('1', ?, 'nope')", order),
      "error: sxml: no xmlfiles table named nope")
check("xmlexists is true for a non-empty result", run("select xmlexists('//line[@qty > 1]', ?), "
                                                      "xmlexists('//line[@qty > 5]', ?)", order, order), [(1, 0)])
db.execute("create table orders(doc blob)")
db.execute("insert into orders values (xmlparse(?))", (order,))
check("the two-argument form can be indexed", run("create index o1 on orders(xmlquery('count(//line)', doc))"), [])
check("the three-argument form cannot", run("create index o2 on orders(xmlquery('count(//line)', doc, 'docs'))"),
      "error: unsafe use of xmlquery()")
run("create view reading as select xmlquery('1', doc, 'docs') from orders")
check("nor be used in a view (direct-only)", run("select * from reading"), "error: unsafe use of xmlquery()")
check("writes go through the virtual table", run("update docs set content = '<rates/>' where uri = 'rates.xml'"), [])
check("and are seen", run("select content from docs where uri = 'rates.xml'"), [("<rates/>",)])
check("delete", run("delete from docs where uri = 'note.txt'") or run("select count(*) from docs"), [(5,)])
db.setconfig(sqlite3.SQLITE_DBCONFIG_DEFENSIVE, True)
check("defensive mode refuses direct writes to the shadow table",
      run("insert into docs_content(uri, content) values ('x', 'y')"), "error: table docs_content may not be modified")
check("rename renames the shadow table", run("alter table docs rename to files")
      or run("select name from sqlite_schema where name like 'files%' order by name"),
      [("files",), ("files_content",)])
check("drop drops it", run("drop table files") or run("select count(*) from sqlite_schema where name like 'files%'"),
      [(0,)])
print("files:", "ok" if failures == 0 else f"{failures} failures")
sys.exit(1 if failures else 0)
