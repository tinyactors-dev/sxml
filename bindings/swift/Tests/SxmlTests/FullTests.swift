// Tests of XQuery, XSLT and XSD validation in the Swift package (ctest
// swift/linux; D238): the tests of the JavaScript package's
// test/full.test.ts by the same names, inputs and expected text, plus the
// package's own tests of ARC and of the resource data. Every test ends with
// its processor destroyed and 0 allocations left (D23). The native library
// has every language, so full.test.ts's test of the core module, which
// refuses them, has no counterpart. Golden:
// bindings/javascript/test/full.test.ts.
import Foundation
import Testing

@testable import Sxml

/// The text of an error, for the tests that match it as full.test.ts's
/// regular expressions do.
func errorText(_ error: (any Error)?) -> String {
    switch error {
    case let error as SxmlError:
        return error.message
    case let error as SchemaError:
        return error.description
    case let error?:
        return "\(error)"
    case nil:
        return ""
    }
}

/// Collects what callbacks receive, outside the callbacks.
final class Collected<Element> {
    var elements: [Element] = []

    func append(_ element: Element) {
        elements.append(element)
    }
}

@Suite("XQuery")
struct XQueryTests {
    @Test("compiles once and evaluates many times with external variables")
    func compilesOnce() throws {
        try withProcessor { processor throws in
            let query = try processor.compileQuery(
                "declare variable $n external := 10; declare variable $who external; $who || \": \" || $n * 2")
            #expect(query.isUpdating == false)
            #expect(try query.evaluate(variables: ["who": "a", "n": 1]).description == "\"a: 2\"")
            #expect(try query.evaluate(variables: ["who": "b"]).description == "\"b: 20\"")
            // A name the query does not declare is ignored (XPath 4.0's
            // dynamic context rule); a required one without a value raises
            // XPDY0002.
            #expect(try query.evaluate(variables: ["who": "c", "other": 3]).description == "\"c: 20\"")
            let error = #expect(throws: SxmlError.self) { try query.evaluate() }
            #expect(error?.code == "err:XPDY0002")
        }
    }

    @Test("evaluates against a context value, with namespaces and Q{uri}local names")
    func contextAndNames() throws {
        try withProcessor { processor throws in
            let document = try processor.parse("<p:list xmlns:p=\"urn:p\"><p:i>1</p:i><p:i>2</p:i></p:list>")
            let query = try processor.compileQuery(
                "declare variable $Q{urn:v}factor external; sum(//p:i) * $Q{urn:v}factor",
                namespaces: ["p": "urn:p"])
            let result = try query.evaluate(context: document, variables: ["Q{urn:v}factor": 10])
            #expect(result.description == "3.0e1")
        }
    }

    @Test("binds bin for EXPath Binary, as the sxml command does")
    func bindsBin() throws {
        try withProcessor { processor throws in
            let query = try processor.compileQuery("bin:length(bin:hex(\"CAFE\"))")
            #expect(try query.run() == "2")
        }
    }

    @Test("serializes with the output declarations of the prolog")
    func outputDeclarations() throws {
        try withProcessor { processor throws in
            let query = try processor.compileQuery(
                "declare option output:method \"json\"; declare option output:indent \"no\"; map { \"a\": [1, 2] }")
            let json = try JSONSerialization.jsonObject(with: Data(try query.run().utf8)) as? [String: [Int]]
            #expect(json == ["a": [1, 2]])
            let xml = try processor.compileQuery("<r>{1 + 1}</r>")
            // XQuery 4.0, 5.22.1: method xml, omit-xml-declaration yes.
            #expect(try xml.run() == "<r>2</r>")
            let value = try xml.evaluate()
            #expect(
                try value.serialize(["method": "xml", "omitXmlDeclaration": false])
                    == "<?xml version=\"1.0\" encoding=\"UTF-8\"?><r>2</r>")
        }
    }

    @Test("reports static errors with code, location and source")
    func staticErrors() throws {
        try withProcessor { processor throws in
            let error = #expect(throws: SxmlError.self) {
                try processor.compileQuery("declare variable $x := 1;\n$x +")
            }
            #expect(error?.status == .staticError)
            #expect(error?.code == "err:XPST0003")
            #expect(error?.sourceName == "query")
            #expect(error?.lineNumber == 2)
            let undeclared = #expect(throws: SxmlError.self) { try processor.compileQuery("$undeclared") }
            #expect(errorText(undeclared).contains("XPST0008"))
        }
    }

    @Test("reports dynamic errors")
    func dynamicErrors() throws {
        try withProcessor { processor throws in
            let query = try processor.compileQuery("error(xs:QName(\"err:FOER0000\"), \"stop\")")
            let error = #expect(throws: SxmlError.self) { try query.evaluate() }
            #expect(error?.status == .dynamicError)
            #expect(error?.code == "err:FOER0000")
            #expect(error?.description == "stop")
        }
    }

    @Test("imports library modules through the resolver")
    func importsModules() throws {
        let log = RequestLog()
        try withProcessor(resolver: { request in
            log.entries.append("\(request.kind.rawValue) \(request.absoluteURI)")
            return .text("module namespace m = \"urn:m\"; declare function m:twice($x) { $x * 2 };")
        }) { processor throws in
            let query = try processor.compileQuery(
                "import module namespace m = \"urn:m\" at \"m.xq\"; m:twice(21)",
                baseURI: "https://example.com/q/")
            #expect(try query.evaluate().description == "42")
            #expect(log.entries == ["xquery-module https://example.com/q/m.xq"])
        }
    }

    @Test("hands the trees an updating query changed to the update sink")
    func updatedTrees() throws {
        try withProcessor { processor throws in
            let document = try processor.parse("<a><b>1</b></a>", uri: "urn:doc")
            let query = try processor.compileQuery(
                "replace value of node //b with \"2\", insert node <c/> into /a")
            #expect(query.isUpdating == true)
            let trees = Collected<UpdatedTree>()
            let result = try query.evaluate(
                context: document, updates: UpdateSink(updatedTree: { trees.append($0) }))
            // XQUF 3.0, 2.2.3: an updating query's value is the empty
            // sequence.
            #expect(result.description == "")
            #expect(trees.elements.count == 1)
            #expect(trees.elements.first?.original.description == "<a><b>1</b></a>")
            #expect(trees.elements.first?.updated.description == "<a><b>2</b><c/></a>")
            // Trees never change: the document is as it was.
            #expect(document.description == "<a><b>1</b></a>")
            // Without a sink the trees are dropped, and nothing leaks.
            _ = try query.evaluate(context: document)
        }
    }

    @Test("hands fn:put documents to the update sink, or raises FOUP0002 without")
    func putDocuments() throws {
        try withProcessor { processor throws in
            let query = try processor.compileQuery(
                "put(<doc>{1 + 1}</doc>, \"out.xml\", map { \"indent\": true() })", baseURI: "file:///data/")
            let documents = Collected<PutDocument>()
            _ = try query.evaluate(updates: UpdateSink(putDocument: { documents.append($0) }))
            #expect(documents.elements.count == 1)
            #expect(documents.elements.first?.uri == "file:///data/out.xml")
            #expect(documents.elements.first?.node.description == "<doc>2</doc>")
            #expect(documents.elements.first?.parameters != nil)
            #expect(try documents.elements.first?.serialize() == "<doc>2</doc>")
            let error = #expect(throws: SxmlError.self) { try query.evaluate() }
            #expect(errorText(error).contains("FOUP0002"))
        }
    }

    @Test("delivers fn:trace and fn:message output")
    func messages() throws {
        try withProcessor { processor throws in
            let messages = Collected<Message>()
            let query = try processor.compileQuery("trace(1 + 1, \"sum\"), message(\"note\")")
            _ = try query.evaluate(messages: { messages.append($0) })
            #expect(messages.elements.map { [$0.text, $0.code] } == [["2", "sum"], ["\"note\"", ""]])
        }
    }
}

let identity = """
    <xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
      <xsl:output method="xml" omit-xml-declaration="yes"/>
      <xsl:param name="greeting" select="'Hello'"/>
      <xsl:template match="/">
        <out><xsl:value-of select="$greeting, //name"/></out>
      </xsl:template>
      <xsl:template match="name" mode="upper">
        <name><xsl:value-of select="upper-case(.)"/></name>
      </xsl:template>
      <xsl:template name="xsl:initial-template">
        <initial/>
      </xsl:template>
      <xsl:template name="named">
        <xsl:param name="n" as="xs:integer"/>
        <named n="{$n}"/>
      </xsl:template>
      <xsl:function name="f:double" xmlns:f="urn:f" as="xs:integer" visibility="public">
        <xsl:param name="x" as="xs:integer"/>
        <xsl:sequence select="$x * 2"/>
      </xsl:function>
    </xsl:stylesheet>
    """

@Suite("XSLT")
struct XSLTTests {
    @Test("compiles once and transforms many times with stylesheet parameters")
    func compilesOnce() throws {
        try withProcessor { processor throws in
            let stylesheet = try processor.compileStylesheet(identity)
            let source = try processor.parse("<person><name>Ada</name></person>")
            let result = try stylesheet.transform(source: source)
            #expect(result.output == "<out>Hello Ada</out>")
            #expect(result.value == nil)
            #expect(result.resultDocuments.isEmpty)
            #expect(try stylesheet.transform(source: source, parameters: ["greeting": "Hi"]).output == "<out>Hi Ada</out>")
        }
    }

    @Test("invokes an initial mode, a named template or a function")
    func invocations() throws {
        try withProcessor { processor throws in
            let stylesheet = try processor.compileStylesheet(identity)
            let source = try processor.parse("<person><name>Ada</name></person>")
            #expect(try stylesheet.transform(source: source, initialMode: "upper").output == "<name>ADA</name>")
            #expect(try stylesheet.transform().output == "<initial/>")
            #expect(
                try stylesheet.transform(initialTemplate: "named", templateParameters: ["n": processor.evaluate("3")])
                    .output == "<named n=\"3\"/>")
            let result = try stylesheet.transform(
                initialFunction: "Q{urn:f}double", arguments: [processor.evaluate("21")], delivery: .raw)
            #expect(result.output == nil)
            #expect(result.value?.description == "42")
        }
    }

    @Test("delivers the principal result raw, as a tree, or with other parameters")
    func deliveries() throws {
        try withProcessor { processor throws in
            let stylesheet = try processor.compileStylesheet(identity)
            let source = try processor.parse("<person><name>Ada</name></person>")
            let tree = try stylesheet.transform(source: source, delivery: .tree).value
            #expect(try processor.evaluate("count(/out)", context: tree).description == "1")
            let json = try stylesheet.transform(
                source: source, initialFunction: "Q{urn:f}double", arguments: [processor.evaluate("4")],
                serialization: ["method": "json"])
            #expect(json.output == "8")
            let text = try stylesheet.transform(source: source, serialization: ["method": "text"])
            #expect(text.output == "Hello Ada")
        }
    }

    @Test("takes static parameters at compilation")
    func staticParameters() throws {
        try withProcessor { processor throws in
            let stylesheet = try processor.compileStylesheet(
                """
                <xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
                  <xsl:param name="mode" static="yes" select="'a'"/>
                  <xsl:template name="xsl:initial-template">
                    <xsl:value-of select="'A'" use-when="$mode = 'a'"/>
                    <xsl:value-of select="'B'" use-when="$mode = 'b'"/>
                  </xsl:template>
                </xsl:stylesheet>
                """, staticParameters: ["mode": "b"])
            #expect(try stylesheet.transform(serialization: ["method": "text"]).output == "B")
        }
    }

    @Test("collects result documents, serialized or as values")
    func resultDocuments() throws {
        try withProcessor { processor throws in
            let stylesheet = try processor.compileStylesheet(
                """
                <xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
                  <xsl:template name="xsl:initial-template">
                    <xsl:for-each select="1 to 2">
                      <xsl:result-document href="part{.}.xml" omit-xml-declaration="yes"><part n="{.}"/></xsl:result-document>
                    </xsl:for-each>
                    <main/>
                  </xsl:template>
                </xsl:stylesheet>
                """)
            let serialized = try stylesheet.transform(
                baseOutputURI: "https://example.com/out/",
                serialization: ["method": "xml", "omitXmlDeclaration": true])
            #expect(serialized.output == "<main/>")
            #expect(
                serialized.resultDocuments.map { [$0.uri, $0.output ?? "-"] } == [
                    ["https://example.com/out/part1.xml", "<part n=\"1\"/>"],
                    ["https://example.com/out/part2.xml", "<part n=\"2\"/>"],
                ])
            #expect(serialized.resultDocuments.allSatisfy { $0.value == nil })
            let trees = try stylesheet.transform(baseOutputURI: "urn:out/", resultDocumentDelivery: .tree)
            #expect(trees.resultDocuments.count == 2)
            #expect(trees.resultDocuments.last?.output == nil)
            #expect(trees.resultDocuments.last?.value?.description == "<part n=\"2\"/>")
        }
    }

    @Test("delivers xsl:message output, also when it terminates")
    func messages() throws {
        try withProcessor { processor throws in
            let stylesheet = try processor.compileStylesheet(
                """
                <xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
                  <xsl:template name="xsl:initial-template">
                    <xsl:message>starting</xsl:message>
                    <xsl:message terminate="{$stop}">stopping</xsl:message>
                    <done/>
                  </xsl:template>
                  <xsl:param name="stop" select="'no'"/>
                </xsl:stylesheet>
                """)
            let messages = Collected<Message>()
            _ = try stylesheet.transform(messages: { messages.append($0) })
            #expect(
                messages.elements.map { "\($0.text) \($0.isTerminating)" } == ["starting false", "stopping false"])
            messages.elements.removeAll()
            let error = #expect(throws: SxmlError.self) {
                try stylesheet.transform(parameters: ["stop": "yes"], messages: { messages.append($0) })
            }
            #expect(errorText(error).contains("XTMM9000"))
            #expect(
                messages.elements.map { "\($0.text) \($0.code) \($0.isTerminating)" } == [
                    "starting XTMM9000 false", "stopping XTMM9000 true",
                ])
        }
    }

    @Test("reads included modules through the resolver")
    func includes() throws {
        try withProcessor(resolver: { request in
            guard request.kind == .xsltModule, request.absoluteURI == "https://example.com/lib.xsl" else {
                return nil
            }
            return .text(
                """
                <xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
                  <xsl:template name="from-lib">included</xsl:template>
                </xsl:stylesheet>
                """)
        }) { processor throws in
            let stylesheet = try processor.compileStylesheet(
                """
                <xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
                  <xsl:include href="lib.xsl"/>
                  <xsl:template name="xsl:initial-template"><xsl:call-template name="from-lib"/></xsl:template>
                </xsl:stylesheet>
                """, baseURI: "https://example.com/main.xsl")
            #expect(try stylesheet.transform(serialization: ["method": "text"]).output == "included")
        }
    }

    @Test("reports static and dynamic errors with codes and locations")
    func errors() throws {
        try withProcessor { processor throws in
            let error = #expect(throws: SxmlError.self) {
                try processor.compileStylesheet(
                    "<xsl:stylesheet version=\"3.0\" xmlns:xsl=\"http://www.w3.org/1999/XSL/Transform\">\n<xsl:bogus/>\n</xsl:stylesheet>",
                    baseURI: "urn:bad.xsl")
            }
            #expect(error?.status == .staticError)
            #expect(error?.code.hasPrefix("err:XTSE") == true)
            #expect(error?.sourceName == "urn:bad.xsl")
            #expect(error?.lineNumber == 2)
            let stylesheet = try processor.compileStylesheet(identity)
            let missing = #expect(throws: SxmlError.self) { try stylesheet.transform(initialTemplate: "missing") }
            #expect(errorText(missing).contains("XTDE0040"))
            let unbound = #expect(throws: SxmlError.self) { try stylesheet.transform(initialTemplate: "named") }
            #expect(errorText(unbound).contains("XTDE0700"))
        }
    }
}

let purchaseOrder = """
    <xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
      <xs:element name="order">
        <xs:complexType>
          <xs:sequence>
            <xs:element name="item" maxOccurs="unbounded">
              <xs:complexType>
                <xs:attribute name="quantity" type="xs:positiveInteger" use="required"/>
              </xs:complexType>
            </xs:element>
          </xs:sequence>
        </xs:complexType>
      </xs:element>
      <xs:complexType name="loose"><xs:sequence><xs:any processContents="skip" maxOccurs="unbounded"/></xs:sequence></xs:complexType>
    </xs:schema>
    """

@Suite("XSD validation")
struct SchemaTests {
    @Test("validates documents and returns the typed copy")
    func validates() throws {
        try withProcessor { processor throws in
            let schema = try processor.compileSchema(.text(purchaseOrder, uri: "urn:po.xsd"))
            let result = try schema.validate("<order><item quantity=\"2\"/><item quantity=\"3\"/></order>")
            #expect(result.validity == .valid)
            #expect(result.reports.isEmpty)
            // The copy is annotated (XDM 4.0, 3.3): quantities are integers.
            let total = try processor.evaluate(
                "sum(//@quantity) instance of xs:integer", context: result.document)
            #expect(total.description == "true()")
        }
    }

    @Test("returns every report of an invalid document, with locations")
    func reports() throws {
        try withProcessor { processor throws in
            let schema = try processor.compileSchema(purchaseOrder)
            let result = try schema.validate(
                "<order>\n  <item quantity=\"0\"/>\n  <other/>\n</order>", uri: "urn:order.xml")
            #expect(result.validity == .invalid)
            #expect(result.document == nil)
            #expect(
                result.reports.map { "\($0.code) \($0.sourceName) \($0.lineNumber)" } == [
                    "cvc-datatype-valid.1.2.1 urn:order.xml 2",
                    "cvc-complex-type.2.4 urn:order.xml 3",
                ])
        }
    }

    @Test("validates nodes, laxly, and against a named type")
    func modes() throws {
        try withProcessor { processor throws in
            let schema = try processor.compileSchema(purchaseOrder)
            let document = try processor.parse("<order><item quantity=\"1\"/></order>")
            #expect(try schema.validate(document).validity == .valid)
            #expect(try schema.validate("<unknown/>").validity == .invalid)
            #expect(try schema.validate("<unknown/>", mode: .lax).validity == .notKnown)
            #expect(try schema.validate("<unknown><x/></unknown>", type: "loose").validity == .valid)
            let missing = #expect(throws: SxmlError.self) { try schema.validate("<x/>", type: "missing") }
            #expect(errorText(missing).contains("no type of that name"))
            let malformed = #expect(throws: SxmlError.self) { try schema.validate("<x>") }
            #expect(malformed?.status == .malformedInput)
        }
    }

    @Test("reports every error of a schema that does not compile")
    func compileErrors() throws {
        try withProcessor { processor throws in
            let error = #expect(throws: SchemaError.self) {
                try processor.compileSchema(
                    .text(
                        """
                        <xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
                          <xs:element name="a" type="missing"/>
                          <xs:element name="b" type="alsoMissing"/>
                        </xs:schema>
                        """, uri: "urn:bad.xsd"))
            }
            #expect(error?.error.code == "src-resolve")
            #expect(
                error?.reports.map { "\($0.code) \($0.sourceName) \($0.lineNumber)" } == [
                    "src-resolve urn:bad.xsd 2",
                    "src-resolve urn:bad.xsd 3",
                ])
            #expect(throws: SchemaError.self) { try processor.compileSchema("<not-a-schema/>") }
        }
    }

    @Test("reads included schema documents through the resolver")
    func includes() throws {
        try withProcessor(resolver: { request in
            guard request.kind == .schemaDocument else {
                return nil
            }
            switch request.absoluteURI {
            case "https://example.com/schemas/main.xsd":
                return .text(
                    "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"><xs:include schemaLocation=\"types.xsd\"/><xs:element name=\"n\" type=\"number\"/></xs:schema>")
            case "https://example.com/schemas/types.xsd":
                return .text(
                    "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"><xs:simpleType name=\"number\"><xs:restriction base=\"xs:integer\"/></xs:simpleType></xs:schema>")
            default:
                return nil
            }
        }) { processor throws in
            let schema = try processor.compileSchema(.resolved("main.xsd"), baseURI: "https://example.com/schemas/")
            #expect(try schema.validate("<n>5</n>").validity == .valid)
            #expect(try schema.validate("<n>x</n>").validity == .invalid)
        }
    }
}

/// Serves urn:sxml:timezone-data: and urn:sxml:locale-data: from the
/// repository's copies of the IANA Time Zone Database and the CLDR, as
/// the JavaScript package's README tells a resolver to.
func resourceDataResolver(log: RequestLog) -> Resolver {
    let directories: [(ResourceKind, String, String)] = [
        (.timezoneData, "urn:sxml:timezone-data:", "third_party/tzdata/zoneinfo/"),
        (.localeData, "urn:sxml:locale-data:", "third_party/cldr/common/"),
    ]
    return { request in
        for (kind, prefix, directory) in directories
        where request.kind == kind && request.absoluteURI.hasPrefix(prefix) {
            let path = String(request.absoluteURI.dropFirst(prefix.count))
            log.entries.append("\(kind.rawValue) \(path)")
            let file = repositoryRoot.appendingPathComponent(directory + path)
            return (try? Data(contentsOf: file)).map { .bytes([UInt8]($0)) }
        }
        return nil
    }
}

@Suite("resource data")
struct ResourceDataTests {
    // D213: CollationData.bundled is the package's own copy, a SwiftPM
    // resource, which must be the vendored DUCET.
    @Test("the bundled DUCET is the vendored one, and UCA collations use it")
    func bundledCollationData() throws {
        let vendored = try Data(contentsOf: repositoryRoot.appendingPathComponent("third_party/unicode-collation/allkeys.txt"))
        #expect(CollationData.bundled.servedBytes == [UInt8](vendored))
        let uca = "http://www.w3.org/2013/collation/UCA?numeric=yes;fallback=no"
        try withProcessor { processor throws in
            #expect(try processor.evaluate("compare('Chap2', 'Chap10', '\(uca)')").description == "-1")
        }
        // The resolver is asked first: for the CLDR root collation, then
        // for the DUCET, which it leaves to CollationData.
        let log = RequestLog()
        try withProcessor(resolver: { request in
            log.entries.append("\(request.kind.rawValue) \(request.absoluteURI)")
            return nil
        }) { processor throws in
            _ = try processor.evaluate("compare('a', 'b', '\(uca)')")
        }
        #expect(log.entries.contains("collation-data http://www.unicode.org/Public/UCA/16.0.0/allkeys.txt"))
    }

    @Test("the timezone-database feature reads zoneinfo files through the resolver")
    func timezoneData() throws {
        let log = RequestLog()
        try withProcessor(resolver: resourceDataResolver(log: log)) { processor throws in
            let result = try processor.evaluate(
                "format-dateTime(xs:dateTime('2026-07-01T12:00:00Z'), '[H01]:[m01] [ZN]', (), (), 'Europe/Paris')")
            #expect(result.description == "\"14:00 CEST\"")
        }
        #expect(log.entries.contains("timezone-data Europe/Paris"))
    }

    @Test("the localization feature reads CLDR files through the resolver")
    func localeData() throws {
        let log = RequestLog()
        try withProcessor(resolver: resourceDataResolver(log: log)) { processor throws in
            let result = try processor.evaluate("format-date(xs:date('2003-03-07'), '[MNn]', 'de', (), ())")
            #expect(result.description == "\"März\"")
        }
        #expect(log.entries.contains("locale-data main/de.xml"))
        // Without the files, dates are English after [Language: en].
        try withProcessor { processor throws in
            let result = try processor.evaluate("format-date(xs:date('2003-03-07'), '[MNn]', 'de', (), ())")
            #expect(result.description == "\"[Language: en]March\"")
        }
    }
}

@Suite("memory of the full module")
struct FullMemoryTests {
    // ARC (D238): queries, stylesheets and schemas retain their processor,
    // which is destroyed after the last of them.
    @Test("queries, stylesheets and schemas keep their processor alive")
    func compiledKeepProcessor() throws {
        let report = LeakReport()
        var compiled: (XQuery, Stylesheet, Schema)? = try {
            let processor = try Processor()
            report.observe(processor)
            return (
                try processor.compileQuery("1"), try processor.compileStylesheet(identity),
                try processor.compileSchema(purchaseOrder)
            )
        }()
        #expect(report.leakedAllocationCount == nil)
        #expect(try compiled?.0.run() == "1")
        #expect(try compiled?.1.transform().output == "<initial/>")
        #expect(try compiled?.2.validate("<order/>").validity == .invalid)
        compiled = nil
        #expect(report.leakedAllocationCount == 0)
    }

    @Test("every allocation failure is reported and leaks nothing")
    func allocationFailures() throws {
        // As package.test.ts does for XPath (D23): fail the first, second,
        // ... allocation until the scenario succeeds.
        var failing = 1
        while try !succeeds(failingAllocationNumber: failing) {
            failing += 1
            guard failing < 1_000_000 else {
                Issue.record("no end to the allocation failures")
                return
            }
        }
    }

    /// One round of the allocation-failure loop: whether the scenario
    /// succeeded. Checks the failure and that nothing leaked.
    private func succeeds(failingAllocationNumber: Int) throws -> Bool {
        let report = LeakReport()
        var succeeded = false
        do {
            let processor: Processor
            do {
                processor = try Processor(failingAllocationNumber: failingAllocationNumber)
            } catch let error as SxmlError {
                #expect(error.status == .outOfMemory)
                return false
            }
            report.observe(processor)
            do {
                let document = try processor.parse("<order><item quantity=\"2\"/></order>")
                let updates = Collected<UpdatedTree>()
                _ = try processor.compileQuery("replace value of node //@quantity with 3")
                    .evaluate(context: document, updates: UpdateSink(updatedTree: { updates.append($0) }))
                let result = try processor.compileStylesheet(identity).transform(
                    source: document, initialTemplate: "named", templateParameters: ["n": processor.evaluate("1")])
                let validation = try processor.compileSchema(purchaseOrder).validate(document)
                #expect(updates.elements.count == 1)
                #expect(result.output == "<named n=\"1\"/>")
                #expect(validation.validity == .valid)
                succeeded = true
            } catch let error as SxmlError {
                #expect(
                    [.outOfMemory, .limitExceeded].contains(error.status) || !error.code.isEmpty,
                    "\(error.message)")
            } catch let error as SchemaError {
                // A schema whose compilation ran out of memory.
                #expect(error.error.status == .outOfMemory || !error.error.code.isEmpty, "\(error)")
            }
        }
        #expect(report.leakedAllocationCount == 0, "leaked at allocation \(failingAllocationNumber)")
        return succeeded
    }
}
