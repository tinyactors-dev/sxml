import sqlite3, sys
db = sqlite3.connect(":memory:")
db.enable_load_extension(True)
db.load_extension("./xml.so", entrypoint="sqlite3_xml_init")
doc = '<?xml version="1.0"?><order xmlns:x="urn:x" id="o-17" status="paid"><!-- c --><customer>Ada</customer><line sku="A1" qty="2" price="9.5"/><line sku="B2" qty="1" price="20"/><?pi data?><x:note>gift &amp; wrap</x:note></order>'
q = lambda sql, *a: db.execute(sql, a).fetchone()[0]
b = q("select xmlparse(?)", doc)
print("text", len(doc), "blob", len(b))
print(q("select xmlserialize(?)", b))
for e in ["count(//line)", "sum(//line/(@qty * @price))", "string(/order/@id)", "//line[2]", "/order/customer/text()",
          "//*:note/namespace-uri()", "//line[1]/following-sibling::*[1]/@sku/string()", "(//line)[2]/preceding-sibling::*[1]/name()",
          "/order/comment()/string()", "//processing-instruction()/name()", "/order/*[last()]/name()", "//@*/name()"]:
    print(e, "=>", q("select xmlquery(?2, ?1)", b, e), "|", q("select xmlquery(?2, ?1)", doc, e))
print(q("select xmlexists('//line[@qty > 1]', ?)", b))
print(q("select xmlquery(?2, ?1)", b, "<total>{sum(//line/(@qty * @price))}</total>"))
print("stable:", q("select xmlparse(xmlserialize(?)) = ?", b, b), q("select xmlparse(?) = ?", doc.replace('id="o-17" status', 'id="o-17"  status'), b))
