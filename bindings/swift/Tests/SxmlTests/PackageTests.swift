// The Swift package's tests (ctest swift/linux; D238): the tests of the
// JavaScript package's test/package.test.ts by the same names, inputs and
// expected text, plus the package's own ARC tests. Every test ends with
// its processor destroyed and 0 allocations left (D23). Golden:
// bindings/javascript/test/package.test.ts.
import Foundation
import Testing

@testable import Sxml

/// The allocations a processor left when it was destroyed; nil while it
/// lives.
final class LeakReport {
    var leakedAllocationCount: Int?

    func observe(_ processor: Processor) {
        processor.leakObserver = { [self] count in leakedAllocationCount = count }
    }
}

/// Runs body with a fresh processor, then drops it and checks that it was
/// destroyed with nothing left.
func withProcessor(
    resolver: Resolver? = nil, features: Set<String>? = nil, collationData: CollationData = .bundled,
    _ body: (Processor) throws -> Void
) throws {
    let report = LeakReport()
    do {
        let processor = try Processor(resolver: resolver, features: features, collationData: collationData)
        report.observe(processor)
        try body(processor)
    }
    #expect(report.leakedAllocationCount == 0, "leaked allocations, or a value outlived the test")
}

/// The repository's root, from this file's path.
let repositoryRoot = URL(fileURLWithPath: #filePath)
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent()

/// The optional features the library under test must have (D189):
/// SXML_HOST_FEATURES lists them, comma-separated (the ctest sets it); by
/// default those of the native link.
let expectedFeatures: [String] = (ProcessInfo.processInfo.environment["SXML_HOST_FEATURES"]
    ?? "test,expath-binary,html,xslt-transform,timezone-database,dtd-validation,uca-collation,full-text,invisible-xml,localization,builtin-schemas")
    .split(separator: ",").map(String.init)

@Suite("the module")
struct ModuleTests {
    @Test("reports the release version, that of the package")
    func version() throws {
        #expect(Sxml.version == "0.2.0")
        let packageJSON = try Data(contentsOf: repositoryRoot.appendingPathComponent("bindings/javascript/package.json"))
        let package = try JSONSerialization.jsonObject(with: packageJSON) as? [String: Any]
        #expect(package?["version"] as? String == Sxml.version)
    }
}

@Suite("optional features")
struct FeatureTests {
    @Test("the module lists the features it links")
    func listsFeatures() {
        #expect(Sxml.features == expectedFeatures)
    }

    @Test("a processor takes a list of the module's features")
    func takesFeatures() throws {
        for features in [Set<String>(), Set(expectedFeatures), nil] {
            try withProcessor(features: features) { processor throws in
                #expect(try processor.evaluate("1 + 1").description == "2")
            }
        }
    }

    // The conventional prefix of EXPath Binary, as the sxml command binds
    // it (D235); the caller's namespaces may rebind it.
    @Test("bin is bound wherever the processor has expath-binary")
    func bindsBin() throws {
        let hasBinary = expectedFeatures.contains("expath-binary")
        for features in [nil, Set<String>()] {
            try withProcessor(features: features) { processor throws in
                if hasBinary && features == nil {
                    let result = try processor.evaluate("bin:hex(\"CAFE\") => bin:to-octets()")
                    #expect(result.description == "202\n254")
                } else {
                    let error = #expect(throws: SxmlError.self) {
                        try processor.evaluate("bin:hex(\"CAFE\")")
                    }
                    #expect(error?.code == "err:XPST0081")
                }
                let rebound = try processor.evaluate(
                    "string(namespace-uri-from-QName(xs:QName(\"bin:x\")))", namespaces: ["bin": "urn:b"])
                #expect(rebound.description == "\"urn:b\"")
            }
        }
    }

    @Test("a feature the module does not have is refused")
    func refusesUnknownFeature() {
        #expect(throws: SxmlUsageError.unknownFeature("no-such-feature")) {
            try Processor(features: ["no-such-feature"])
        }
    }

    // The uca-collation feature reads the DUCET the processor was given
    // (D213); the vendored copy stands in for the package's.
    @Test("UCA collations use the DUCET when the module has the feature")
    func ucaCollation() throws {
        let uca = "http://www.w3.org/2013/collation/UCA?numeric=yes;fallback=no"
        let query = "compare('Chap2', 'Chap10', '\(uca)')"
        let hasFeature = expectedFeatures.contains("uca-collation")
        let ducet = try Data(contentsOf: repositoryRoot.appendingPathComponent("third_party/unicode-collation/allkeys.txt"))
        for (collationData, isServed) in [(CollationData.bytes([UInt8](ducet)), hasFeature), (.none, false)] {
            try withProcessor(collationData: collationData) { processor throws in
                if isServed {
                    #expect(try processor.evaluate(query).description == "-1")
                } else {
                    let error = #expect(throws: SxmlError.self) { try processor.evaluate(query) }
                    #expect(error?.code == "err:FOCH0002")
                }
            }
        }
    }
}

@Suite("XPath")
struct XPathTests {
    @Test("evaluates without a context")
    func withoutContext() throws {
        try withProcessor { processor throws in
            #expect(try processor.evaluate("1 to 3").description == "1\n2\n3")
        }
    }

    @Test("evaluates against a parsed document")
    func againstDocument() throws {
        try withProcessor { processor throws in
            let book = try processor.parse("<book><title>XPath 4.0</title><title>XQuery</title></book>")
            let titles = try processor.evaluate("//title/string()", context: book)
            #expect(titles.description == "\"XPath 4.0\"\n\"XQuery\"")
            let count = try processor.evaluate("count(//title)", context: book)
            #expect(count.description == "2")
        }
    }

    @Test("binds variables from values and JSON-like data")
    func bindsVariables() throws {
        try withProcessor { processor throws in
            let document = try processor.parse("<a><b/><b/></a>")
            let result = try processor.evaluate(
                "count($document//b) + $number, $text, $list?2, $record?name, empty($nothing)",
                variables: [
                    "document": document,
                    "number": 40,
                    "text": "it's \"quoted\"",
                    "list": [1, "two"],
                    "record": ["name": "sxml"],
                    "nothing": nil,
                ])
            #expect(result.description == "4.2e1\n\"it's \"\"quoted\"\"\"\n\"two\"\n\"sxml\"\ntrue()")
        }
    }

    @Test("takes variables in Q{uri}local form and namespaces")
    func qualifiedNames() throws {
        try withProcessor { processor throws in
            let document = try processor.parse("<p:root xmlns:p=\"urn:p\"><p:item>1</p:item></p:root>")
            let result = try processor.evaluate(
                "$Q{urn:v}offset + number(/q:root/q:item)", context: document,
                variables: ["Q{urn:v}offset": 1], namespaces: ["q": "urn:p"])
            #expect(result.description == "2.0e0")
        }
    }

    @Test("compiles once and evaluates many times")
    func compilesOnce() throws {
        try withProcessor { processor throws in
            let expression = try processor.compile("$n * 2", variables: ["n"])
            #expect(expression.variables == ["n"])
            for number in [1, 2, 3] {
                let result = try expression.evaluate(variables: ["n": number])
                #expect(result.description == "\(number * 2).0e0")
            }
        }
    }

    @Test("a variable without a value raises XPDY0002")
    func unboundVariable() throws {
        try withProcessor { processor throws in
            let expression = try processor.compile("$n", variables: ["n"])
            let error = #expect(throws: SxmlError.self) { try expression.evaluate() }
            #expect(error?.code == "err:XPDY0002")
        }
    }

    @Test("reports static errors with code and location")
    func staticErrors() throws {
        try withProcessor { processor throws in
            let error = #expect(throws: SxmlError.self) { try processor.evaluate("1 +") }
            #expect(error?.status == .staticError)
            #expect(error?.code == "err:XPST0003")
            #expect(error?.codeNamespaceURI == "http://www.w3.org/2005/xqt-errors")
            #expect(error?.lineNumber == 1)
        }
    }

    @Test("reports dynamic errors")
    func dynamicErrors() throws {
        try withProcessor { processor throws in
            let division = #expect(throws: SxmlError.self) { try processor.evaluate("1 idiv 0") }
            #expect(division?.code == "err:FOAR0001")
            let custom = #expect(throws: SxmlError.self) {
                try processor.evaluate("error(xs:QName(\"err:XYZ\"), \"custom\")")
            }
            #expect(custom?.code == "err:XYZ")
            #expect(custom?.description == "custom")
        }
    }
}

@Suite("XML parsing")
struct ParsingTests {
    @Test("reports a document that is not well-formed")
    func malformed() throws {
        try withProcessor { processor throws in
            let error = #expect(throws: SxmlError.self) { try processor.parse("<a>\n<b></a>", uri: "bad.xml") }
            #expect(error?.status == .malformedInput)
            #expect(error?.lineNumber == 2)
        }
    }

    @Test("takes bytes in any encoding the document declares")
    func encodings() throws {
        try withProcessor { processor throws in
            let bytes = Array("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a>".utf8) + [0xE9]
                + Array("</a>".utf8)
            let document = try processor.parse(bytes, uri: "http://example.com/a.xml")
            let result = try processor.evaluate("string(.), document-uri(.)", context: document)
            #expect(result.description == "\"é\"\n\"http://example.com/a.xml\"")
        }
    }
}

@Suite("serialization")
struct SerializationTests {
    @Test("writes nodes as XML by the adaptive method")
    func adaptive() throws {
        try withProcessor { processor throws in
            let document = try processor.parse("<a x=\"1\"><b>text</b></a>")
            #expect(document.description == "<a x=\"1\"><b>text</b></a>")
        }
    }

    @Test("takes the parameters of fn:serialize")
    func parameters() throws {
        try withProcessor { processor throws in
            let result = try processor.evaluate("map { \"a\": [1, true()] }")
            #expect(try result.serialize(["method": "json"]) == "{\"a\":[1,true]}")
            let document = try processor.parse("<a><b/></a>")
            #expect(
                try document.serialize(["method": "xml", "omitXmlDeclaration": false])
                    == "<?xml version=\"1.0\" encoding=\"UTF-8\"?><a><b/></a>")
            #expect(try document.serialize(["method": "text"]) == "")
        }
    }

    @Test("reports serialization errors")
    func errors() throws {
        try withProcessor { processor throws in
            let result = try processor.evaluate("1, 2")
            let error = #expect(throws: SxmlError.self) { try result.serialize(["method": "json"]) }
            #expect(error?.code == "err:SERE0023")
            #expect(throws: SxmlError.self) { try result.serialize(["method": "no-such-method"]) }
        }
    }
}

/// What a resolver saw, kept outside the resolver.
final class RequestLog {
    var entries: [String] = []
}

struct NetworkDown: Error, Equatable {}

@Suite("the resolver")
struct ResolverTests {
    @Test("serves fn:doc and fn:unparsed-text")
    func servesResources() throws {
        let log = RequestLog()
        try withProcessor(resolver: { request in
            log.entries.append("\(request.kind.rawValue) \(request.absoluteURI)")
            if request.absoluteURI == "http://example.com/data.xml" {
                return .text("<data><value>42</value></data>")
            }
            if request.absoluteURI == "http://example.com/note.txt" {
                return .bytes(Array("a note".utf8))
            }
            return nil
        }) { processor throws in
            let result = try processor.evaluate(
                "doc(\"data.xml\")//value/string(), unparsed-text(\"note.txt\"), doc-available(\"missing.xml\")",
                baseURI: "http://example.com/")
            #expect(result.description == "\"42\"\n\"a note\"\nfalse()")
            #expect(
                log.entries == [
                    "xml-document http://example.com/data.xml",
                    "text http://example.com/note.txt",
                    "xml-document http://example.com/missing.xml",
                ])
            let error = #expect(throws: SxmlError.self) {
                try processor.evaluate("doc(\"http://example.com/missing.xml\")")
            }
            #expect(error?.code == "err:FODC0002")
        }
    }

    @Test("an exception in the resolver is rethrown")
    func rethrowsResolverError() throws {
        try withProcessor(resolver: { _ in throw NetworkDown() }) { processor throws in
            #expect(throws: NetworkDown()) { try processor.evaluate("doc(\"http://example.com/a.xml\")") }
            #expect(try processor.evaluate("1").description == "1")
        }
    }

    @Test("the resolver may not use its processor")
    func mayNotUseProcessor() throws {
        // Weak: a resolver that held its processor strongly would be a cycle.
        weak var weakProcessor: Processor?
        try withProcessor(resolver: { _ in
            _ = try weakProcessor?.parse("<a/>")
            return .text("<b/>")
        }) { processor throws in
            weakProcessor = processor
            #expect(throws: SxmlUsageError.resolverUsesProcessor) {
                try processor.evaluate("doc(\"http://example.com/a.xml\")")
            }
        }
    }
}

@Suite("memory")
struct MemoryTests {
    // ARC (D238): there is no dispose(); objects retain their processor,
    // which is destroyed after the last of them.
    @Test("a value keeps its processor alive")
    func valueKeepsProcessor() throws {
        let report = LeakReport()
        var document: Value? = try {
            let processor = try Processor()
            report.observe(processor)
            return try processor.parse("<a/>")
        }()
        #expect(report.leakedAllocationCount == nil)
        #expect(document?.description == "<a/>")
        document = nil
        #expect(report.leakedAllocationCount == 0)
    }

    @Test("an expression keeps its processor alive")
    func expressionKeepsProcessor() throws {
        let report = LeakReport()
        var expression: XPathExpression? = try {
            let processor = try Processor()
            report.observe(processor)
            return try processor.compile("$n + 1", variables: ["n"])
        }()
        #expect(report.leakedAllocationCount == nil)
        #expect(try expression?.evaluate(variables: ["n": 1]).description == "2.0e0")
        expression = nil
        #expect(report.leakedAllocationCount == 0)
    }

    @Test("values of one processor are refused by another")
    func otherProcessor() throws {
        try withProcessor { first throws in
            try withProcessor { second throws in
                let document = try first.parse("<a/>")
                #expect(throws: SxmlUsageError.valueOfAnotherProcessor) {
                    try second.evaluate(".", context: document)
                }
            }
        }
    }

    @Test("every allocation failure is reported and leaks nothing")
    func allocationFailures() throws {
        // As the C tests do with sxml_counting_allocator (D23): fail the
        // first, second, ... allocation until the scenario succeeds.
        var failing = 1
        while try !succeeds(failingAllocationNumber: failing) {
            failing += 1
            guard failing < 100_000 else {
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
                let document = try processor.parse("<a><b>1</b></a>")
                let result = try processor.evaluate("sum(//b) + $n", context: document, variables: ["n": [2]])
                #expect(try result.serialize(["method": "text"]) == "3")
                succeeded = true
            } catch let error as SxmlError {
                #expect(
                    [.outOfMemory, .limitExceeded].contains(error.status) || !error.code.isEmpty,
                    "\(error.message)")
            }
        }
        #expect(report.leakedAllocationCount == 0, "leaked at allocation \(failingAllocationNumber)")
        return succeeded
    }

    @Test("values that are released return their memory")
    func releasedValues() throws {
        try withProcessor { processor throws in
            _ = try processor.parse("<a/>")
            let baseline = processor.liveAllocationCount()
            for _ in 0..<100 {
                _ = try processor.parse("<a/>")
            }
            #expect(processor.liveAllocationCount() == baseline)
        }
    }
}
