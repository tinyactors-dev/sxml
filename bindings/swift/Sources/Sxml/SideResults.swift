// Side results (docs/GLOSSARY.md, "side_result"; D216, D238): what a call
// on a query, stylesheet or schema delivers besides its result, given to
// the host's callbacks after the call. Also the shim's names and binding
// lists, which those calls share. Golden: bindings/javascript/src/index.ts
// (SideResult, sideResults, deliverMessages, bindingWords, nameWords).
internal import CSxmlHost

/// Trace output (XSLT 4.0, 23.1 and 23.2; F&O 4.0, fn:trace and
/// fn:message): an xsl:message or failed xsl:assert as the text method
/// writes it, fn:trace and fn:message by the adaptive method.
public struct Message: Sendable, Equatable {
    public let text: String
    /// xsl:message: its error code's local name (XTMM9000 by default);
    /// fn:trace and fn:message: the label.
    public let code: String
    /// xsl:message terminate="yes" and xsl:assert: the call then throws.
    public let isTerminating: Bool
    public let sourceName: String
    public let lineNumber: Int
    public let columnNumber: Int
}

/// A side result as the shim wrote it, its values adopted.
struct SideResult {
    let kind: UInt32
    let uri: String
    let text: String
    let code: String
    let first: Value?
    let second: Value?
    let lineNumber: Int
    let columnNumber: Int
    let isTerminating: Bool

    static let updatedTree = SXML_HOST_SIDE_RESULT_KIND_UPDATED_TREE.rawValue
    static let putDocument = SXML_HOST_SIDE_RESULT_KIND_PUT_DOCUMENT.rawValue
    static let resultDocument = SXML_HOST_SIDE_RESULT_KIND_RESULT_DOCUMENT.rawValue
    static let message = SXML_HOST_SIDE_RESULT_KIND_MESSAGE.rawValue
    static let schemaReport = SXML_HOST_SIDE_RESULT_KIND_SCHEMA_REPORT.rawValue
}

extension Processor {
    /// The side results a call wrote into its request, every value adopted
    /// (sxml_host.h: the host takes them over, failed call or not). Read
    /// right after the call: the views are valid until the handle's next.
    func sideResults(_ list: sxml_host_side_result_list) -> [SideResult] {
        guard let results = list.side_results else {
            return []
        }
        return (0..<list.side_result_count).map { position in
            let result = results[position]
            return SideResult(
                kind: result.side_result_kind,
                uri: text(of: result.uri),
                text: text(of: result.text),
                code: text(of: result.code),
                first: result.first_value.map { Value(processor: self, handle: $0) },
                second: result.second_value.map { Value(processor: self, handle: $0) },
                lineNumber: Int(result.line_number),
                columnNumber: Int(result.column_number),
                isTerminating: result.is_terminating != 0)
        }
    }

    /// Hands the MESSAGE side results to receive.
    func deliverMessages(_ results: [SideResult], to receive: ((Message) throws -> Void)?) throws {
        guard let receive else {
            return
        }
        for result in results where result.kind == SideResult.message {
            try receive(
                Message(
                    text: result.text, code: result.code, isTerminating: result.isTerminating,
                    sourceName: result.uri, lineNumber: result.lineNumber,
                    columnNumber: result.columnNumber))
        }
    }

    /// bindings as the shim's binding list, in code point order of their
    /// names, copied into scratch; the values made are appended to made,
    /// which the caller keeps until the call is done.
    func bindingList(_ bindings: [String: (any XdmInput)?], scratch: inout Scratch,
                     made: inout [Value]) throws -> sxml_host_binding_list
    {
        var list: [sxml_host_binding] = []
        for (name, data) in bindings.sorted(by: { $0.key < $1.key }) {
            let value = try input(data, made: &made)
            made.append(value)
            list.append(sxml_host_binding(name: try scratch.name(name), value: value.handle))
        }
        return sxml_host_binding_list(bindings: try scratch.array(list), binding_count: list.count)
    }
}

extension Scratch {
    /// An NCName or Q{uri}local as the shim's name; nil or "" is none.
    mutating func name(_ name: String?) throws -> sxml_host_name {
        guard let name, !name.isEmpty else {
            let none = sxml_byte_view(bytes: nil, byte_count: 0)
            return sxml_host_name(namespace_uri: none, local_name: none)
        }
        let (namespaceURI, localName) = splitName(name)
        return sxml_host_name(namespace_uri: try view(namespaceURI), local_name: try view(localName))
    }
}

/// An empty side result list, for a request the call writes into.
var noSideResults: sxml_host_side_result_list {
    sxml_host_side_result_list(side_results: nil, side_result_count: 0)
}
